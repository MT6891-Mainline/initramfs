// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal chopin init: mount the distro rootfs from userdata and switch to
 * its init.  No USB console, no diagnostics, no recovery -- those live in
 * the bring-up init on the dev branch.  Modelled on the MT6895 initramfs,
 * which also only knows about BOOT_PARTITION.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/loop.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT_DEV "/dev/sdc75"
#define NVDATA_DEV "/dev/sdc6"

static void msg(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n, fd;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		write(fd, "chopin-init: ", 13);
		write(fd, buf, n);
		write(fd, "\n", 1);
		close(fd);
	}
	write(1, buf, n);
	write(1, "\n", 1);
}

static void mkdir_p(const char *p)
{
	char tmp[256];
	char *s;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", p);
	len = strlen(tmp);
	if (len && tmp[len - 1] == '/')
		tmp[len - 1] = 0;
	for (s = tmp + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = 0;
		mkdir(tmp, 0755);
		*s = '/';
	}
	mkdir(tmp, 0755);
}

static void mount_pseudo(void)
{
	mkdir_p("/proc");
	mkdir_p("/sys");
	mkdir_p("/dev");
	mkdir_p("/dev/pts");
	mkdir_p("/tmp");
	mkdir_p("/run");
	mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC, NULL);
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, NULL);
	mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NOEXEC, NULL);
	mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NOEXEC, NULL);
}

/*
 * The partition number of userdata depends on the firmware revision: the stock
 * MIUI 12.5 scatter puts it at sdc77, the newer layouts this port was built
 * against put it at sdc75.  Probing beats trusting one number - a wrong guess
 * mounts someone else's filesystem (or nothing) and the board stops at this
 * init with no distro to switch to.
 */
static int has_rootfs(const char *dev)
{
	int ok;

	mkdir_p("/mnt/probe");
	if (mount(dev, "/mnt/probe", "ext4", MS_RDONLY, NULL) &&
	    mount(dev, "/mnt/probe", "f2fs", MS_RDONLY, NULL))
		return 0;

	ok = (access("/mnt/probe/sbin/init", X_OK) == 0 ||
	      access("/mnt/probe/init", X_OK) == 0);
	umount("/mnt/probe");
	return ok;
}

static const char *root_device(void)
{
	static char buf[256];
	FILE *f = fopen("/proc/cmdline", "r");

	if (f) {
		while (fgets(buf, sizeof(buf), f)) {
			char *p = strstr(buf, "chopin.root=");

			if (p) {
				p += 12;
				if (strncmp(p, "/dev/", 5) == 0) {
					char *e = p;

					while (*e && *e != ' ' && *e != '\n')
						e++;
					*e = 0;
					fclose(f);
					return p;
				}
			}
		}
		fclose(f);
	}

	/* the partition this port has always used, if it still exists */
	if (has_rootfs(ROOT_DEV))
		return ROOT_DEV;

	/* otherwise look for a distro on any sdc partition */
	for (int i = 1; i <= 200; i++) {
		static char found[64];

		snprintf(buf, sizeof(buf), "/dev/sdc%d", i);
		if (access(buf, F_OK))
			continue;
		if (has_rootfs(buf)) {
			snprintf(found, sizeof(found), "%s", buf);
			return found;
		}
	}

	return ROOT_DEV;
}

static void copy_tree(const char *src, const char *dst)
{
	DIR *d;
	struct dirent *e;
	struct stat st;

	d = opendir(src);
	if (!d)
		return;
	mkdir_p(dst);
	while ((e = readdir(d))) {
		char s[512], t[512];
		int in, out;
		char b[4096];
		ssize_t n;

		if (e->d_name[0] == '.')
			continue;
		snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
		snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
		if (stat(s, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			copy_tree(s, t);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;
		in = open(s, O_RDONLY);
		if (in < 0)
			continue;
		out = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			close(in);
			continue;
		}
		while ((n = read(in, b, sizeof(b))) > 0)
			write(out, b, n);
		close(in);
		close(out);
	}
	closedir(d);
}

static void install_firmware(void)
{
	char b[8192];
	int in, out;
	ssize_t n;

	in = open("/lib/firmware/mali_csffw.bin", O_RDONLY);
	if (in < 0)
		return;
	mkdir_p("/newroot/lib/firmware");
	out = open("/newroot/lib/firmware/mali_csffw.bin",
		   O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return;
	}
	while ((n = read(in, b, sizeof(b))) > 0)
		write(out, b, n);
	close(in);
	close(out);
	msg("installed mali_csffw.bin");
}

/*
 * Ship the G77-capable Mesa driver built on the host into the distro.  It is
 * left in /root (not installed over the distro's copy) so it can be tried
 * with LD_LIBRARY_PATH first and removed again if needed.  Debian's EGL links
 * libgallium directly, so that is the file that matters (the
 * panfrost_dri.so name is only used by Xorg and is not enough).
 */
static void install_gpu_driver(void)
{
	DIR *d;
	struct dirent *e;

	d = opendir("/lib/firmware");
	if (!d)
		return;
	mkdir_p("/newroot/root");
	while ((e = readdir(d))) {
		char src[256], dst[256], b[8192];
		size_t len = strlen(e->d_name);
		int in, out;
		ssize_t n;

		if (len < 4 || strcmp(e->d_name + len - 3, ".so"))
			continue;
		snprintf(src, sizeof(src), "/lib/firmware/%s", e->d_name);
		snprintf(dst, sizeof(dst), "/newroot/root/%s", e->d_name);
		in = open(src, O_RDONLY);
		if (in < 0)
			continue;
		out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			close(in);
			continue;
		}
		while ((n = read(in, b, sizeof(b))) > 0)
			write(out, b, n);
		close(in);
		close(out);
		msg("installed %s", e->d_name);
	}
	closedir(d);
}


/* ------------------------------------------------------------------ */
/* Firmware                                                            */
/*                                                                     */
/* The MediaTek WiFi/BT drivers ask for blobs that are in neither      */
/* Debian's firmware-mediatek nor the distro rootfs: they live in the  */
/* Android partitions.                                                 */
/*                                                                     */
/*   WiFi RAM code / ROM patch, wifi.cfg, bt.cfg -> "vendor", a        */
/*       logical partition inside "super".  lpdump: vendor_a starts at */
/*       sector 13246464 and is 4428400 sectors long, so a loop device */
/*       at that byte offset makes it mountable.                       */
/*   WIFI NVRAM + BT_Addr -> "nvdata".                                 */
/*                                                                     */
/* Without this the drivers give up and stay dead:                     */
/*   wlan: Direct firmware load for soc3_0_ram_wmmcu_1a_1_hdr.bin      */
/*         failed with error -2                                        */
/*   Direct firmware load for mediatek/mt6893/BT_Addr failed (-2)      */
/*         (retried 481 times)                                         */
/* ------------------------------------------------------------------ */
#define VENDOR_SUPER_DEV "/dev/sdc61"
#define VENDOR_OFF       (13246464ULL * 512)

static int find_part(const char *partname, char *dev, size_t devsz)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char p[256], buf[1024], want[128];
		ssize_t r;
		int fd;

		if (strncmp(e->d_name, "sd", 2))
			continue;
		snprintf(p, sizeof(p), "/sys/class/block/%s/uevent",
			 e->d_name);
		fd = open(p, O_RDONLY);
		if (fd < 0)
			continue;
		r = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (r <= 0)
			continue;
		buf[r] = 0;
		snprintf(want, sizeof(want), "PARTNAME=%s", partname);
		if (strstr(buf, want)) {
			snprintf(dev, devsz, "/dev/%s", e->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static int copy_one(const char *src, const char *dst)
{
	char b[4096];
	int in, out;
	ssize_t n;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, b, sizeof(b))) > 0)
		if (write(out, b, n) != n)
			break;
	close(in);
	close(out);
	return 0;
}

/* Find a file by name anywhere under root; returns 0 on success. */
static int find_named(const char *root, const char *name, int depth,
		      char *out, size_t outsz)
{
	DIR *d = opendir(root);
	struct dirent *e;

	if (!d || depth > 8)
		return -1;
	while ((e = readdir(d))) {
		char p[512];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", root, e->d_name);
		if (stat(p, &st))
			continue;
		if (S_ISREG(st.st_mode) && !strcmp(e->d_name, name)) {
			snprintf(out, outsz, "%s", p);
			closedir(d);
			return 0;
		}
		if (S_ISDIR(st.st_mode) &&
		    !find_named(p, name, depth + 1, out, outsz)) {
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

/*
 * The stock BT_Addr NV item stores the address in display order
 * (04:e5:98:..) while the driver consumes little-endian bdaddr bytes, so the
 * file it loads has to be the reverse.
 */
static void install_bt_addr(const char *src, const char *dst)
{
	unsigned char b[6], r[6];
	int f, i, n;

	f = open(src, O_RDONLY);
	if (f < 0)
		return;
	n = read(f, b, sizeof(b));
	close(f);
	if (n != 6) {
		msg("BT_Addr %s is %d bytes, skipped", src, n);
		return;
	}
	for (i = 0; i < 6; i++)
		r[i] = b[5 - i];
	f = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (f < 0)
		return;
	if (write(f, r, 6) == 6)
		msg("BT_Addr %02x:%02x:%02x:%02x:%02x:%02x",
		    r[5], r[4], r[3], r[2], r[1], r[0]);
	close(f);
}

/* Copy the vendor firmware tree (WiFi RAM code, ROM patch, cfg files). */
static void fwload_vendor(void)
{
	char dev[64], loopname[32];
	struct loop_info64 info;
	int ffd, loopfd, ctl, i;

	if (find_part("super", dev, sizeof(dev)))
		snprintf(dev, sizeof(dev), "%s", VENDOR_SUPER_DEV);

	ffd = open(dev, O_RDONLY);
	if (ffd < 0) {
		msg("vendor: open %s: %s", dev, strerror(errno));
		return;
	}
	ctl = open("/dev/loop-control", O_RDWR);
	i = ctl >= 0 ? ioctl(ctl, LOOP_CTL_GET_FREE) : -1;
	if (ctl >= 0)
		close(ctl);
	if (i < 0)
		i = 0;
	snprintf(loopname, sizeof(loopname), "/dev/loop%d", i);
	loopfd = open(loopname, O_RDWR);
	if (loopfd < 0) {
		msg("vendor: open %s: %s", loopname, strerror(errno));
		close(ffd);
		return;
	}
	if (ioctl(loopfd, LOOP_SET_FD, ffd)) {
		msg("vendor: LOOP_SET_FD: %s", strerror(errno));
		close(loopfd);
		close(ffd);
		return;
	}
	memset(&info, 0, sizeof(info));
	info.lo_offset = VENDOR_OFF;
	snprintf((char *)info.lo_file_name, LO_NAME_SIZE, "%s", dev);
	if (ioctl(loopfd, LOOP_SET_STATUS64, &info)) {
		msg("vendor: LOOP_SET_STATUS64: %s", strerror(errno));
		ioctl(loopfd, LOOP_CLR_FD, 0);
		close(loopfd);
		close(ffd);
		return;
	}
	mkdir_p("/mnt/vendor");
	if (mount(loopname, "/mnt/vendor", "ext4", MS_RDONLY, NULL)) {
		msg("vendor: mount %s: %s", loopname, strerror(errno));
		ioctl(loopfd, LOOP_CLR_FD, 0);
		close(loopfd);
		close(ffd);
		return;
	}
	mkdir_p("/newroot/lib/firmware");
	copy_tree("/mnt/vendor/firmware", "/newroot/lib/firmware");
	msg("vendor firmware copied from %s", dev);
	umount("/mnt/vendor");
	ioctl(loopfd, LOOP_CLR_FD, 0);
	close(loopfd);
	close(ffd);
}

/* NVRAM calibration and the BT address come from nvdata, not vendor. */
static void fwload_nvdata(const char *root)
{
	char dev[64], src[512], dst[512];
	struct stat st;
	int i;
	static const char *const cand[] = {
		"/mnt/nvdata/APCFG/APRDEB/WIFI",
		"/mnt/nvdata/mediatek/mt6893/WIFI",
		"/mnt/nvdata/mt6893/WIFI",
		"/mnt/nvdata/media/mediatek/mt6893/WIFI",
		"/mnt/nvdata/WIFI",
		"/mnt/nvdata/media/WIFI",
	};

	if (find_part("nvdata", dev, sizeof(dev)))
		snprintf(dev, sizeof(dev), "%s", NVDATA_DEV);
	mkdir_p("/mnt/nvdata");
	if (mount(dev, "/mnt/nvdata", "ext4", MS_RDONLY, NULL)) {
		msg("nvdata: mount %s: %s", dev, strerror(errno));
		return;
	}
	snprintf(dst, sizeof(dst), "%s/lib/firmware/mediatek/mt6893", root);
	mkdir_p(dst);

	for (i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
		if (!stat(cand[i], &st) && S_ISREG(st.st_mode)) {
			snprintf(dst, sizeof(dst),
				 "%s/lib/firmware/mediatek/mt6893/WIFI", root);
			copy_one(cand[i], dst);
			msg("WIFI nvram from %s", cand[i]);
			break;
		}
	}
	if (!find_named("/mnt/nvdata", "BT_Addr", 0, src, sizeof(src))) {
		snprintf(dst, sizeof(dst),
			 "%s/lib/firmware/mediatek/mt6893/BT_Addr", root);
		install_bt_addr(src, dst);
	} else {
		msg("no BT_Addr under nvdata; the BT driver will keep retrying");
	}
	umount("/mnt/nvdata");
}

static int switch_root(const char *dev)
{
	const char *init = "/sbin/init";

	mkdir_p("/newroot");
	if (mount(dev, "/newroot", "ext4", MS_RDONLY, NULL) &&
	    mount(dev, "/newroot", "f2fs", MS_RDONLY, NULL)) {
		msg("mount %s: %s", dev, strerror(errno));
		return -1;
	}
	if (access("/newroot/sbin/init", X_OK) &&
	    access("/newroot/init", X_OK)) {
		msg("%s has no /sbin/init or /init", dev);
		umount("/newroot");
		return -1;
	}
	if (access("/newroot/sbin/init", X_OK) != 0)
		init = "/init";
	if (mount(dev, "/newroot", NULL, MS_REMOUNT, NULL)) {
		msg("remount %s: %s", dev, strerror(errno));
		umount("/newroot");
		return -1;
	}

	fwload_nvdata("/newroot");
	fwload_vendor();
	install_firmware();
	install_gpu_driver();

	/* Copy the initramfs module tree into the distro; bind-mounting it
	 * across switch_root made this board reboot, a plain copy does not. */
	copy_tree("/lib/modules", "/newroot/lib/modules");

	mkdir_p("/newroot/proc");
	mkdir_p("/newroot/sys");
	mkdir_p("/newroot/dev");
	mkdir_p("/newroot/dev/pts");
	mkdir_p("/newroot/run");
	mkdir_p("/newroot/tmp");
	mount("proc", "/newroot/proc", "proc", MS_NOSUID | MS_NOEXEC, NULL);
	mount("sysfs", "/newroot/sys", "sysfs", MS_NOSUID | MS_NOEXEC, NULL);
	mount("devtmpfs", "/newroot/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount("devpts", "/newroot/dev/pts", "devpts",
	      MS_NOSUID | MS_NOEXEC, NULL);
	mount("tmpfs", "/newroot/run", "tmpfs", MS_NOSUID | MS_NOEXEC, NULL);
	mount("tmpfs", "/newroot/tmp", "tmpfs", MS_NOSUID | MS_NOEXEC, NULL);

	chdir("/newroot");
	if (mount(".", "/", NULL, MS_MOVE, NULL)) {
		msg("move /newroot: %s", strerror(errno));
		return -1;
	}
	chroot(".");
	chdir("/");

	msg("exec %s", init);
	execl(init, init, (char *)NULL);
	msg("exec %s: %s", init, strerror(errno));
	return -1;
}

int main(void)
{
	const char *dev;
	int i;

	mount_pseudo();
	msg("chopin init: boot partition %s", ROOT_DEV);

	dev = root_device();
	for (i = 0; i < 100; i++) {
		if (access(dev, F_OK) == 0)
			break;
		usleep(100000);
	}
	msg("booting rootfs %s", dev);
	if (switch_root(dev) == 0)
		return 0;

	msg("rootfs boot failed: %s", strerror(errno));
	/* Keep the board alive but let the user reboot; no USB console here. */
	for (;;)
		sleep(3600);
}
