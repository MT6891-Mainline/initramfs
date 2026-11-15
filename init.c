// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal PID 1 for the Redmi Note 10 Pro (chopin) mainline bring-up
 * initramfs.
 *
 * It mounts the usual pseudo filesystems, prints a marker and the kernel
 * version so that a serial console is enough to prove that the kernel reached
 * userspace, then drops into /bin/sh when one is present.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <signal.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/fb.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/loop.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Everything goes to /dev/kmsg, which has two limits that silently ate PID 1's
 * output before:
 *   - a record may not exceed 1024 bytes (PRINTKRB_RECORD_MAX); /proc/cmdline is
 *     ~1.6 KiB, so write() returned -EINVAL and the text simply vanished;
 *   - userspace writes are RATELIMITED unless the kernel is booted with
 *     printk.devkmsg=on, which drops everything after the first few lines.
 * So: chunk every write, and always report a short write.
 */
static void print_file(const char *path);
static void print_head(const char *path, int max);
static void cat_file(const char *path);

/* ------------------------------------------------------------------ */
/* USB bring-up diagnostics                                            */
/*                                                                     */
/* The gadget reports "pullup D+" but the host saw nothing, so the     */
/* question is whether the controller ever sees a session at all:      */
/*   - the udc "state" file says "not attached" vs "powered"/          */
/*     "default"/"configured".  The first means our side never got the */
/*     link up (PHY, VBUS/session detection, clock); the second means  */
/*     the link is up and the problem is past the PHY.                 */
/*   - the tphy debugfs files are the values the driver parsed from    */
/*     DT, so they prove the PHY instance was created and tuned.       */
/*   - the tphy "vrt"/"term"/"discth" values are the tuning the driver */
/*     parsed out of DT, so they prove the instance exists and is      */
/*     configured.  Raw PHY registers cannot be read from userspace.   */
/* ------------------------------------------------------------------ */

/*
 * There is deliberately no /dev/mem register dumper here.  On arm64
 * devmem_is_allowed() falls back to the generic "always 0" implementation, so
 * read_mem() fails with -EFAULT for every address - MMIO included.  Register
 * dumps have to be exported by the driver that owns them (see the logstore
 * ring debugfs files) rather than poked from userspace.
 */

static void print_matching(const char *path, const char *needle)
{
	static char buf[65536];
	int fd = open(path, O_RDONLY);
	ssize_t n;
	char *p, *save;
	int hits = 0;

	if (fd < 0) {
		printf("  %s: (missing)\n", path);
		return;
	}
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		save = NULL;
		for (p = strtok_r(buf, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
			if (strstr(p, needle)) {
				printf("  %s\n", p);
				hits++;
			}
		}
	}
	close(fd);
	if (!hits)
		printf("  (no line matching \"%s\")\n", needle);
}

static void dump_dir_tree(const char *path, int depth)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (depth > 3)
		return;
	if (!d) {
		printf("  %s: (missing)\n", path);
		return;
	}
	while ((e = readdir(d))) {
		char p[256];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		struct stat st;

		snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
		/*
		 * stat(), not d_type: sysfs is built out of symlinks (every
		 * /sys/bus/... entry is one), so DT_LNK is the common case there
		 * and treating it as a file makes whole device directories show up
		 * as "(empty)".
		 */
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			printf("  [dir] %s\n", p);
			dump_dir_tree(p, depth + 1);
		} else {
			printf("  %s = ", p);
			print_file(p);
		}
	}
	closedir(d);
}

static void dump_udc(void)
{
	static const char *const files[] = {
		"state", "current_speed", "maximum_speed", "function",
		"is_a_peripheral", "is_otg", "role", NULL,
	};
	DIR *d = opendir("/sys/class/udc");
	struct dirent *e;

	if (!d) {
		printf("  /sys/class/udc: (missing)\n");
		return;
	}
	while ((e = readdir(d))) {
		char path[256];
		int i;

		if (e->d_name[0] == '.')
			continue;
		printf("  udc %s:\n", e->d_name);
		for (i = 0; files[i]; i++) {
			snprintf(path, sizeof(path), "/sys/class/udc/%s/%s",
				 e->d_name, files[i]);
			if (access(path, R_OK))
				continue;
			printf("    %-16s: ", files[i]);
			print_file(path);
		}
	}
	closedir(d);
}

static void dump_usb_diag(void)
{
	printf("\ninit: --- USB: udc state (is the controller attached?) ---\n");
	dump_udc();

	printf("\ninit: --- USB: tphy debugfs (what the driver parsed) ---\n");
	dump_dir_tree("/sys/kernel/debug/phy", 0);

	printf("\ninit: --- USB: ssusb clocks ---\n");
	print_matching("/sys/kernel/debug/clk/clk_summary", "ssusb");
	printf("\ninit: --- USB: interrupts (mtu3 IRQ activity) ---\n");
	print_head("/proc/interrupts", 60);
}

/*
 * USB serial console.
 *
 * This kernel's legacy g_serial has no struct console, so the kernel cannot
 * print to ttyGS0 by itself - userspace has to push the log out.  Everything
 * that goes to /dev/kmsg is mirrored to /dev/ttyGS0 when it exists, and the
 * kernel-log dump is repeated later so a terminal attached after boot still
 * gets the whole thing.  That is what replaces photographing the panel.
 *
 * Best effort everywhere: with no host on the other end the writes simply
 * fail or land in a tty buffer nobody reads, and that must never disturb the
 * /dev/kmsg path, which is the one the expdb relay depends on.
 */
static int usb_fd = -1;
static int usb_only;	/* re-dumps: USB only, do not spam the ring again */

static void mount_pseudo_fs(void);	/* defined below */

static void usb_open_console(void)
{
	int i;

	mount_pseudo_fs();	/* devtmpfs, so /dev/ttyGS0 can appear at all */
	for (i = 0; i < 8 && usb_fd < 0; i++) {
		usb_fd = open("/dev/ttyGS0", O_WRONLY | O_NONBLOCK);
		if (usb_fd < 0)
			usleep(250000);
	}
	if (usb_fd >= 0)
		fcntl(usb_fd, F_SETFL, O_WRONLY | O_NONBLOCK);
}

static size_t kmsg_tx(const char *s, size_t n)
{
	size_t off = 0;
	int mirrored = 0;

	if (usb_fd >= 0) {
		size_t u = 0;

		while (u < n) {
			ssize_t w = write(usb_fd, s + u, n - u);

			if (w > 0) {
				u += (size_t)w;
				mirrored = 1;
				continue;
			}
			/*
			 * The gadget goes away and comes back: a suspend resets the
			 * USB link, and this descriptor is then useless.  It used
			 * to be opened once at startup and never again, so after a
			 * resume every message went into the void and the console
			 * stayed silent for the rest of the session.  That cost one
			 * suspend test its readout - the log was intact in the ring
			 * and there was no way to print it.
			 */
			close(usb_fd);
			usb_fd = open("/dev/ttyGS0", O_WRONLY | O_NONBLOCK);
			if (usb_fd >= 0)
				fcntl(usb_fd, F_SETFL, O_WRONLY | O_NONBLOCK);
			else
				break;
		}
	}
	/*
	 * usb_only keeps ordinary command output out of the kernel ring, which is
	 * the right default and stays.  But when the gadget is gone the output has
	 * to go somewhere, and /dev/console - which is tty0, the screen - is the
	 * only channel left.  Without this, a resume that takes the USB link down
	 * also takes away any way to read what happened.
	 */
	if (usb_only && mirrored)
		return n;

	while (off < n) {
		size_t chunk = (n - off) > 256 ? 256 : (n - off);
		ssize_t w = write(1, s + off, chunk);

		if (w <= 0)
			break;
		off += (size_t)w;
	}
	return off;
}

static void kmsg_str(const char *s)
{
	size_t n = strlen(s);

	if (kmsg_tx(s, n) != n)
		printf("init: !! short write (%zu bytes lost)\n", n);
}

static void __attribute__((format(printf, 1, 2)))
kmsg(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		kmsg_tx(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static void try_mount(const char *source, const char *target,
		      const char *fstype)
{
	if (mount(source, target, fstype, 0, NULL) && errno != EBUSY) {
		printf("init: mount %s on %s failed: %s\n", source, target,
		       strerror(errno));
		fflush(stdout);
	}
}

static void mount_pseudo_fs(void)
{
	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	mkdir("/dev", 0755);
	mkdir("/dev/pts", 0755);
	mkdir("/tmp", 01777);

	try_mount("proc", "/proc", "proc");
	try_mount("sysfs", "/sys", "sysfs");
	try_mount("devtmpfs", "/dev", "devtmpfs");
	try_mount("devpts", "/dev/pts", "devpts");
	try_mount("tmpfs", "/tmp", "tmpfs");
}

/*
 * cat - print a file to the console.
 *
 * print_file() sends what it reads to /dev/kmsg, which is right for the boot
 * diagnostics but means that "cat <debugfs file>" shows nothing at all on the
 * terminal: the value goes into the ring buffer instead.  That cost several
 * rounds of confusion - the number was read correctly every time and simply
 * never appeared where it was being looked for.  Interactive reads go to
 * stdout.
 */
/*
 * w <path> <value> - write a value to a sysfs/debugfs file and read it back.
 *
 * This shell has no redirection: "echo 300 > /sys/..." cannot be typed at it.
 * Without something like this there is no way to drive a control from the
 * console at all, and brightness is the first control that needs one.
 */
static void write_file(const char *path, const char *value)
{
	ssize_t n;
	int fd = open(path, O_WRONLY);

	if (fd < 0) {
		printf("(cannot open %s for writing: %s)\n", path,
		       strerror(errno));
		return;
	}
	n = write(fd, value, strlen(value));
	close(fd);
	if (n < 0) {
		printf("(write to %s failed: %s)\n", path, strerror(errno));
		return;
	}
	printf("%s <- %s  =>  ", path, value);
	cat_file(path);
}

static void cat_file(const char *path)
{
	char buf[512];
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		printf("(cannot open %s: %s)\n", path, strerror(errno));
		return;
	}
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		printf("%s", buf);
	}
	close(fd);
}

static void print_file(const char *path)
{
	char buf[512];
	int fd = open(path, O_RDONLY);
	ssize_t n;
	int any = 0, nl = 0;

	if (fd < 0) {
		printf("(missing)\n");
		return;
	}
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		any = 1;
		nl = (buf[n - 1] == '\n');
		kmsg_tx(buf, (size_t)n);
	}
	close(fd);
	if (!any)
		printf("(empty)");
	if (!nl)
		printf("\n");
}

/*
 * Print at most max_lines lines of a file; used to snapshot debugfs files
 * without flooding the serial console.
 */
static void print_head(const char *path, int max_lines)
{
	char buf[512];
	int fd = open(path, O_RDONLY);
	ssize_t n;
	int lines = 0;

	if (fd < 0) {
		printf("  (missing: %s)\n", path);
		return;
	}
	while (lines < max_lines && (n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		char *p = buf;
		int i;

		buf[n] = '\0';
		for (i = 0; i < n && lines < max_lines; i++) {
			if (buf[i] == '\n') {
				buf[i] = '\0';
				printf("  %s\n", p);
				lines++;
				p = buf + i + 1;
			}
		}
		if (lines < max_lines && p < buf + n)
			printf("  %s", p);
	}
	close(fd);
	if (lines >= max_lines)
		printf("  ... (truncated)\n");
	fflush(stdout);
}


/*
 * Recover the parts of the kernel log our ring windows lost.
 *
 * The kernel side mirrors printk into two fixed windows of the bootloader ring
 * (62 KiB + 124 KiB) that restart when they fill, so with initcall_debug on, the
 * early and middle of the boot - exactly where "did this driver probe?" is
 * answered - is overwritten long before LK relays it to expdb.
 *
 * SYSLOG_ACTION_READ_ALL hands userspace the WHOLE printk ring buffer (512 KiB)
 * in one go, so PID 1 can recover it and re-emit the interesting lines through
 * /dev/kmsg (which the kernel then mirrors into the ring again, in order).
 *
 * Output is deliberately capped: more than a few tens of KiB here would just
 * push the earlier lines out of the windows again.
 */
/* Fallback read size if the ring size cannot be queried, plus the sanity cap
 * we refuse to allocate above.  The ring itself is sized by log_buf_len=4M. */
#define KLOG_MAX	(512 * 1024)
#define KLOG_MAX_HARD	(16 * 1024 * 1024)
#define KLOG_HEAD_LINES	110
#define KLOG_MATCH_MAX	300

static const char *klog_keep[] = {
	/* display */
	"simple-framebuffer", "simplefb", "fb0", "fbcon", "framebuffer",
	"Console: switching", "mediatek-drm", "mtk-drm", "dsi", "panel", "mipi",
	"drm",
	/*
	 * The panel's supplies and backlight.  None of the lines these drivers
	 * emit contains any of the words above - "lm36273_probe", "bias_enable",
	 * "re-applied level" - so a bare dmesg showed the display binding and
	 * nothing about whether it was actually powered.  That is the blind spot
	 * that made several rounds of debugging harder than they needed to be.
	 * "regulator" happens to catch wl2866d because it prefixes its own
	 * messages with it.
	 */
	"lm36273", "backlight", "re-applied level",
	/* pmic */
	"pwrap", "pmic", "mt6359", "mt6360", "regulator", "spmi", "rtc-mt",
	/* storage */
	"ufshcd", "ufsphy", "ufs", "msdc", "mmc0", "mmc1", "sdhci",
	/* usb */
	"mtu3", "xhci", "dwc3", "usb-phy", "xsp", "usb ",
	/* general trouble */
	"probe of", "deferred", "EPROBE", "not found", "No such",
	"Failed", "failed", "error", "Error", "cannot", "Cannot",
	NULL
};

/*
 * filter == NULL: the boot-time report (first N lines, then the keyword-matched
 * driver lines).  filter != NULL: every line containing it, which is what the
 * shell's "dmesg <substr>" wants.
 */
/*
 * tail: the newest N lines of a file.  The logstore windows are written with
 * "first fit, else rewind", so their newest text sits at the end of the window -
 * which is where a hang's last words are.
 */

/* ------------- I2C: interrogate the sub-PMICs with no driver at all ----------
 *
 * i2c6 carries both sub-PMICs: the MT6360 (charger + Type-C + ADC, PMU at 0x34)
 * and the MT6691 that feeds the UFS rail (0x51).  Neither has a working driver
 * yet - the MT6360's MFD insists on an interrupt, which needs a pinctrl driver
 * that does not bind to this tree yet - and none of that stops us from reading
 * the chips directly through /dev/i2c-N.
 *
 * MT6360 landmarks, taken from the mainline driver:
 *   0x000 chip id, bits [7:4] must read 0x5
 *   0x300 DEV_INFO, same vendor nibble
 *   0x34A CHG_STAT, bits [7:6]: 0 = not charging, 1 = charging, 2 = full
 */
#define N_ELEM(a) (sizeof(a) / sizeof((a)[0]))

static int i2c_dev_open(int bus)
{
	char path[24];

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	return open(path, O_RDWR);
}

static int i2c_reg_read(int fd, int addr, int reg, unsigned int *out)
{
	union i2c_smbus_data data;
	struct i2c_smbus_ioctl_data args;

	args.read_write = I2C_SMBUS_READ;
	args.command = (unsigned char)reg;
	args.size = I2C_SMBUS_BYTE_DATA;
	args.data = &data;

	if (ioctl(fd, I2C_SLAVE, addr) < 0)
		return -1;
	if (ioctl(fd, I2C_SMBUS, &args) < 0)
		return -1;
	*out = data.byte;
	return 0;
}

/* A bare receive-byte, used for scanning: it must not poke any register. */
static int i2c_probe(int fd, int addr)
{
	union i2c_smbus_data data;
	struct i2c_smbus_ioctl_data args;

	args.read_write = I2C_SMBUS_READ;
	args.command = 0;
	args.size = I2C_SMBUS_BYTE;
	args.data = &data;

	if (ioctl(fd, I2C_SLAVE, addr) < 0)
		return -1;
	return ioctl(fd, I2C_SMBUS, &args);
}


/*
 * A bus's identity, in the only form that is stable.
 *
 * The kernel numbers /dev/i2c-N by probe order, not by the DT alias: on this
 * board the controller at 0x11F01000 (the vendor's "i2c6") came up as i2c-1, so
 * "/dev/i2c-6" does not exist at all.  The adapter name, on the other hand, is
 * the controller's own device name - 11f01000.i2c - which is exactly what a
 * device tree node has to match.
 */
static void i2c_adapter_name(int bus, char *out, size_t len)
{
	char path[80], *slash;
	ssize_t n;
	int fd;

	out[0] = '\0';

	/*
	 * Prefer the device tree path: the adapter's own name is the generic
	 * "i2c-mt65xx" on this SoC, which identifies nothing, while the of_node
	 * symlink resolves to e.g. /i2c5@11F00000 - the controller address, which
	 * is what a device tree node has to match.
	 */
	snprintf(path, sizeof(path), "/sys/bus/i2c/devices/i2c-%d/of_node", bus);
	n = readlink(path, out, len - 1);
	if (n > 0) {
		out[n] = '\0';
		slash = strrchr(out, '/');
		if (slash && slash[1])
			memmove(out, slash + 1, strlen(slash + 1) + 1);
		return;
	}

	snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, out, len - 1);
	if (n > 0) {
		if (out[n - 1] == '\n')
			n--;
		out[n] = '\0';
	}
	close(fd);
}

static void cmd_i2cscan(int argc, char **argv)
{
	int first = 0, last = 13, bus, addr, found = 0;

	if (argc > 1) {
		first = last = atoi(argv[1]);
		if (first < 0 || first > 20) {
			printf("  bad bus number\n");
			return;
		}
	}
	for (bus = first; bus <= last; bus++) {
		char name[64];
		int fd = i2c_dev_open(bus);
		int any = 0;

		if (fd < 0)
			continue;
		i2c_adapter_name(bus, name, sizeof(name));
		for (addr = 0x08; addr <= 0x77; addr++) {
			if (i2c_probe(fd, addr) == 0) {
				if (!any)
					printf("  i2c-%d (%s):", bus, name[0] ? name : "?");
				printf(" %02x", addr);
				any = 1;
				found++;
			}
		}
		if (any)
			printf("\n");
		close(fd);
	}
	printf("  --- %d device(s) found ---\n", found);
	if (!found)
		printf("  (no bus answered: does the controller probe? try: dmesg i2c)\n");
}

static void cmd_i2creg(int argc, char **argv)
{
	int bus, addr, reg, count = 1, i, fd;

	if (argc < 4) {
		printf("  usage: i2creg <bus> <addr> <reg> [count]\n");
		return;
	}
	bus = atoi(argv[1]);
	addr = strtol(argv[2], NULL, 0);
	reg = strtol(argv[3], NULL, 0);
	if (argc > 4)
		count = atoi(argv[4]);

	fd = i2c_dev_open(bus);
	if (fd < 0) {
		printf("  no /dev/i2c-%d\n", bus);
		return;
	}
	for (i = 0; i < count; i++) {
		unsigned int v;

		if (i2c_reg_read(fd, addr, reg + i, &v))
			printf("  [0x%03x] NAK\n", reg + i);
		else
			printf("  [0x%03x] 0x%02x\n", reg + i, v);
	}
	close(fd);
}

/*
 * Look at the charger.  With no argument, find it: the MT6360's PMU answers at
 * 0x34 with its vendor nibble (0x5) in register 0x00 - the same check the vendor
 * driver makes - so a search over every adapter is unambiguous, and it is what
 * settled the fact that this board's "i2c6" in the vendor tree is not where the
 * part actually lives.
 */
static void mt6360_dump(int bus)
{
	static const struct {
		int reg;
		const char *what;
	} regs[] = {
		{ 0x000, "chip id (bits 7:4 must be 5)" },
		{ 0x300, "DEV_INFO" },
		{ 0x311, "CHG_CTRL1" },
		{ 0x317, "CHG_CTRL7" },
		{ 0x34A, "CHG_STAT" },
	};
	char name[64];
	unsigned int v, i;
	int fd = i2c_dev_open(bus);

	if (fd < 0)
		return;
	i2c_adapter_name(bus, name, sizeof(name));
	printf("  found on i2c-%d (%s):\n", bus, name[0] ? name : "?");
	for (i = 0; i < N_ELEM(regs); i++) {
		if (i2c_reg_read(fd, 0x34, regs[i].reg, &v))
			printf("    [0x%03x] NAK          %s\n", regs[i].reg, regs[i].what);
		else
			printf("    [0x%03x] 0x%02x         %s\n", regs[i].reg, v, regs[i].what);
	}
	if (!i2c_reg_read(fd, 0x34, 0x34A, &v))
		printf("  => MT6360 says: %s\n",
		       ((v >> 6) & 3) == 1 ? "CHARGING" :
		       ((v >> 6) & 3) == 2 ? "charge FULL" :
		       ((v >> 6) & 3) == 0 ? "not charging" : "unknown state");
	close(fd);
}

static void cmd_mt6360(int argc, char **argv)
{
	int bus;

	if (argc > 1) {
		mt6360_dump(atoi(argv[1]));
		return;
	}
	for (bus = 0; bus <= 20; bus++) {
		unsigned int v;
		int fd = i2c_dev_open(bus);

		if (fd < 0)
			continue;
		if (i2c_reg_read(fd, 0x34, 0x00, &v) == 0 && (v & 0xf0) == 0x50) {
			close(fd);
			mt6360_dump(bus);
			return;
		}
		close(fd);
	}
	printf("  no MT6360 found on /dev/i2c-*\n");
	printf("  NOTE: /dev/i2c refuses addresses that a kernel driver owns, so once\n");
	printf("  the MT6360 has bound this is expected.  Check the driver side:\n");
	printf("    dmesg mt6360 / dmesg chg / dmesg 11f00000 / power\n");
}


/*
 * Every power supply, with everything it knows.
 *
 * The MT6360's cells are platform devices created with PLATFORM_DEVID_AUTO, so
 * their names carry a ".N.auto" suffix (mt6360-chg.2.auto) that is not worth
 * guessing at a shell prompt - and reading a name that does not exist looks
 * exactly like a driver that failed to bind.  This walks /sys/class/power_supply
 * and prints each supply's uevent, which lists every property the driver offers
 * with its current value.
 */
static void dump_one_supply(const char *name)
{
	static const char *const interesting[] = {
		"status", "online", "present", "capacity", "voltage_now",
		"current_now", "input_current_limit", "constant_charge_current",
	};
	char path[192];
	unsigned int i;

	printf("  [%s]\n", name);
	snprintf(path, sizeof(path), "/sys/class/power_supply/%s/uevent", name);
	print_head(path, 64);
	for (i = 0; i < N_ELEM(interesting); i++) {
		int fd;

		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s",
			 name, interesting[i]);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		close(fd);
		printf("    %s = ", interesting[i]);
		print_head(path, 1);
	}
}

static void cmd_power(int argc, char **argv)
{
	DIR *d;

	(void)argc;
	(void)argv;
	d = opendir("/sys/class/power_supply");
	struct dirent *e;
	int n = 0;

	if (!d) {
		printf("  no /sys/class/power_supply\n");
		return;
	}
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		dump_one_supply(e->d_name);
		n++;
	}
	closedir(d);
	printf("  --- %d power supply(ies) ---\n", n);
	if (!n)
		printf("  (none: check dmesg mt6360 / dmesg chg)\n");
}


/* ---- battery: the real state, from the chip that actually knows -----------
 *
 * Two candidates were tried and only one is honest:
 *
 *  - the MT6359's auxadc: its VBAT/IBAT channels go through the impedance
 *    measurement path, which returns an already-converted value that the driver
 *    then scales again (observed: 1719 V), and it is not even stable (two reads
 *    of the current differed by 2x).  Left alone.
 *  - the MT6360's own PMU ADC: a stock mainline driver with sane per-channel
 *    scales, already bound.  Channel 4 is VBAT and 6 is IBAT, each with a _scale
 *    that is in microvolts (1250) / microamps (2500) per LSB, so the real value
 *    is raw x scale - printed here along with the raw pair so the arithmetic can
 *    be checked at a glance.
 *
 * The charger's own view (status/online/limits) comes from whichever
 * power_supply node carries OF_COMPATIBLE_0=mediatek,mt6360-chg, because the
 * MFD's PLATFORM_DEVID_AUTO makes the name unstable.
 */
static int read_int_file(const char *path, long *out)
{
	char buf[64];
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	*out = strtol(buf, NULL, 10);
	return 0;
}

/*
 * IIO device names carry the platform device's name, which for anything the MFD
 * created with PLATFORM_DEVID_AUTO means a ".1.auto" suffix - the same trap that
 * made a bound charger look missing through /sys/class/power_supply.  So match by
 * substring, and list everything found so a miss is self-explanatory.
 */
static int iio_find(const char *want, char *out, size_t len)
{
	int n, hit = -1;

	for (n = 0; n < 16; n++) {
		char path[96], name[80];
		int fd;
		ssize_t r;

		snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device%d/name", n);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		r = read(fd, name, sizeof(name) - 1);
		close(fd);
		if (r <= 0)
			continue;
		if (name[r - 1] == '\n')
			r--;
		name[r] = '\0';
		printf("    iio:device%-2d = %s\n", n, name);
		if (hit < 0 && strstr(name, want)) {
			snprintf(out, len, "/sys/bus/iio/devices/iio:device%d", n);
			hit = n;
		}
	}
	return hit < 0 ? -1 : 0;
}

static void print_iio_chan(const char *dev, const char *kind, int chan,
			   const char *what, const char *unit)
{
	char path[192];
	long raw, scale = 0;
	int have_scale;

	snprintf(path, sizeof(path), "%s/in_%s%d_raw", dev, kind, chan);
	if (read_int_file(path, &raw)) {
		printf("    %-7s (no channel %d)\n", what, chan);
		return;
	}
	snprintf(path, sizeof(path), "%s/in_%s%d_scale", dev, kind, chan);
	have_scale = read_int_file(path, &scale) == 0;
	if (have_scale && scale) {
		long v = raw * scale;

		printf("    %-7s raw=%-7ld x scale=%-6ld = %ld %s",
		       what, raw, scale, v, unit);
		if (!strcmp(unit, "uV"))
			printf("  (= %ld.%03ld V)", v / 1000000, (v / 1000) % 1000);
		if (!strcmp(unit, "uA"))
			printf("  (= %ld.%03ld A)", v / 1000000, (v / 1000) % 1000);
		printf("\n");
	} else {
		printf("    %-7s raw=%ld (no scale)\n", what, raw);
	}
}

/*
 * The MT6360 comes up with the chip's hardware default input limit (500 mA),
 * while this board's vendor configuration asks for 2 A.  The property is
 * writable - verified by hand on ringfix47 - so set it at boot and say what
 * happened.  It is only a limit: a weaker source still charges at its own pace.
 */
static void set_charge_current(void)
{
	static const char *const props[] = {
		"input_current_limit", "constant_charge_current",
	};
	char dir[128];
	DIR *d = opendir("/sys/class/power_supply");

	if (!d)
		return;
	{
		struct dirent *e;

		while ((e = readdir(d))) {
			char ue[192], buf[256];
			int fd;
			ssize_t r;
			unsigned int i;

			if (e->d_name[0] == '.')
				continue;
			snprintf(ue, sizeof(ue), "/sys/class/power_supply/%s/uevent",
				 e->d_name);
			fd = open(ue, O_RDONLY);
			if (fd < 0)
				continue;
			r = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (r <= 0)
				continue;
			buf[r] = '\0';
			if (!strstr(buf, "mediatek,mt6360-chg"))
				continue;

			for (i = 0; i < N_ELEM(props); i++) {
				char p[224];
				long v = 0;
				int n;

				n = snprintf(p, sizeof(p), "/sys/class/power_supply/%s/%s",
					     e->d_name, props[i]);
				/* a truncated path could name a different attribute:
				 * never write through one */
				if (n < 0 || (size_t)n >= sizeof(p))
					continue;
				if (read_int_file(p, &v))
					continue;
				fd = open(p, O_WRONLY);
				if (fd < 0)
					continue;
				r = write(fd, "2000000", 7);
				close(fd);
				if (read_int_file(p, &v) == 0)
					printf("  %s: %s -> %ld\n", e->d_name,
					       props[i], v);
			}
			(void)dir;
			break;
		}
	}
	closedir(d);
}

static void cmd_battery(int argc, char **argv)
{
	char dev[96], path[192];
	int found = 0;

	(void)argc;
	(void)argv;

	if (!iio_find("mt6360-adc", dev, sizeof(dev))) {
		printf("  MT6360 PMU ADC (%s):\n", dev);
		print_iio_chan(dev, "voltage", 4, "VBAT", "uV");
		print_iio_chan(dev, "current", 6, "IBAT", "uA");
		print_iio_chan(dev, "voltage", 3, "VSYS", "uV");
		print_iio_chan(dev, "current", 5, "IBUS", "uA");
		found = 1;
	}
	if (!iio_find("MT6359", dev, sizeof(dev))) {
		printf("  MT6359 auxadc (%s) - known mis-scaled, for reference only:\n", dev);
		print_iio_chan(dev, "voltage", 12, "VBAT", "uV");
		print_iio_chan(dev, "current", 13, "IBAT", "uA");
		found = 1;
	}
	if (!found)
		printf("  no battery ADC found (ls /sys/bus/iio/devices)\n");

	/* The charger, whichever name PLATFORM_DEVID_AUTO gave it. */
	{
		static const char dir[] = "/sys/class/power_supply";
		DIR *d = opendir(dir);

		if (d) {
			struct dirent *e;

			while ((e = readdir(d))) {
				char ue[192];
				int fd;
				ssize_t r;
				char buf[256];

				if (e->d_name[0] == '.')
					continue;
				snprintf(ue, sizeof(ue), "%s/%s/uevent", dir, e->d_name);
				fd = open(ue, O_RDONLY);
				if (fd < 0)
					continue;
				r = read(fd, buf, sizeof(buf) - 1);
				close(fd);
				if (r <= 0)
					continue;
				buf[r] = '\0';
				if (!strstr(buf, "mediatek,mt6360-chg"))
					continue;
				printf("  charger [%s]:\n", e->d_name);
				snprintf(path, sizeof(path), "%s/%s/status", dir, e->d_name);
				printf("    status      = ");
				print_head(path, 1);
				snprintf(path, sizeof(path), "%s/%s/online", dir, e->d_name);
				printf("    online      = ");
				print_head(path, 1);
				snprintf(path, sizeof(path), "%s/%s/input_current_limit", dir, e->d_name);
				printf("    input_limit = ");
				print_head(path, 1);
			}
			closedir(d);
		}
	}
}


/* ---- usbhost: a timed host window, with the console coming back ------------
 *
 * The mtu3's role switch allows userspace control, so the role lives in
 * /sys/class/usb_role/<name>/role: "host" brings up xhci and enables the MT6360's
 * OTG boost (ssusb_set_vbus), "device" tears both down again.
 *
 * The catch is that this console *is* the gadget, so switching to host means the
 * shell can no longer be typed at.  Hence the timed window: usbhost N switches to
 * host, waits N seconds, switches back, and then re-opens /dev/ttyGS0 onto its
 * own stdio so the prompt actually returns.  Plug the device in first, and read
 * what happened with "dmesg usb" afterwards.
 */
static int usb_role_path(char *out, size_t len)
{
	DIR *d = opendir("/sys/class/usb_role");

	if (d) {
		struct dirent *e;

		while ((e = readdir(d))) {
			char p[192];

			if (e->d_name[0] == '.')
				continue;
			snprintf(p, sizeof(p), "/sys/class/usb_role/%s/role", e->d_name);
			closedir(d);
			snprintf(out, len, "%s", p);
			return 0;
		}
		closedir(d);
	}
	return -1;
}

static void usb_role_write(const char *path, const char *want)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0) {
		printf("  cannot write %s\n", path);
		return;
	}
	if (write(fd, want, strlen(want)) < 0)
		printf("  write %s failed\n", want);
	close(fd);
}

static int usb_rebind(void);
static void usb_wait_tty_and_reexec(void);
static int udc_wait_configured(int ms);
static int udc_is_configured(void);
static void usb_settle_after_device(void);

static void cmd_usbhost(int argc, char **argv)
{
	char path[192], cur[32];
	int seconds = argc > 1 ? atoi(argv[1]) : 0;
	int fd, n;

	if (usb_role_path(path, sizeof(path))) {
		printf("  no /sys/class/usb_role (is the role switch registered?)\n");
		return;
	}
	printf("  role switch: %s\n", path);
	usb_role_write(path, "host");
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, cur, sizeof(cur) - 1);
		close(fd);
		if (n > 0) {
			cur[n] = '\0';
			printf("  now: %s", cur);
		}
	}
	printf("  >>> the USB console is gone while this lasts.  Plug the device in now.\n");
	printf("  NOTE: if the phone is attached to a hub's upstream port, the hub will\n");
	printf("  enumerate for us here, but it cannot enumerate us when we go back to\n");
	printf("  device mode - plug the phone into the PC directly to get the console back.\n");
	if (seconds <= 0) {
		printf("  (no timeout given: reboot or re-flash to get the console back)\n");
		return;
	}
	printf("  switching back in %d s ...\n", seconds);
	sleep(seconds);
	usb_role_write(path, "device");
	/*
	 * Give the host a few seconds to enumerate us; if it does not, rebind the
	 * controller (see usb_settle_after_device).  Then take /dev/ttyGS0 over.
	 */
	usb_settle_after_device();
	/* The gadget re-enumerates, so /dev/ttyGS0 is a new tty: take it over. */
	for (n = 0; n < 100; n++) {
		int tty = open("/dev/ttyGS0", O_RDWR);

		if (tty >= 0) {
			dup2(tty, 0);
			dup2(tty, 1);
			dup2(tty, 2);
			printf("\n  console back (role=device); check: dmesg usb\n");
			return;
		}
		usleep(100000);
	}
}


/* ---- USB replug: the gadget only pulls up once -----------------------------
 *
 * This board does not wire the PHY's VBUS pad to the Type-C connector (the
 * charger IC senses VBUS instead), so the PHY was told to force the OTG session
 * signals - without that the gadget never enumerates at all.  The cost is that
 * nothing tells the controller when the cable is unplugged and plugged back in:
 * the pullup happens once at probe and is never revisited, so the console stays
 * dead until something rebinds the controller.
 *
 * The one piece of hardware that does see VBUS is the MT6360 charger, and its
 * interrupts work.  So watch its "online" property and rebind the USB controller
 * on an offline -> online edge; "reconnect" does the same thing by hand.
 */
#define USB_DEV_NAME	"11201000.usb"

static int usb_driver_dir(char *out, size_t len)
{
	DIR *d = opendir("/sys/bus/platform/drivers");

	if (!d)
		return -1;
	{
		struct dirent *e;

		while ((e = readdir(d))) {
			char probe[192];
			int fd;

			if (e->d_name[0] == '.')
				continue;
			snprintf(probe, sizeof(probe), "/sys/bus/platform/drivers/%s/%s",
				 e->d_name, USB_DEV_NAME);
			fd = open(probe, O_RDONLY);
			if (fd < 0)
				continue;
			close(fd);
			closedir(d);
			snprintf(out, len, "/sys/bus/platform/drivers/%s", e->d_name);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static void kmsg_line(const char *fmt, ...)
{
	char buf[192];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fd = open("/dev/kmsg", O_WRONLY);
	if (fd >= 0) {
		if (n > 0)
			write(fd, buf, n);
		close(fd);
	}
}

/* Rebind the controller so its gadget enumerates again. */
static int usb_rebind(void)
{
	char dir[160], path[224];
	int fd;

	if (usb_driver_dir(dir, sizeof(dir))) {
		kmsg_line("init: replug: no driver bound to " USB_DEV_NAME "\n");
		return -1;
	}
	snprintf(path, sizeof(path), "%s/unbind", dir);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	write(fd, USB_DEV_NAME, strlen(USB_DEV_NAME));
	close(fd);

	usleep(300000);

	snprintf(path, sizeof(path), "%s/bind", dir);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	write(fd, USB_DEV_NAME, strlen(USB_DEV_NAME));
	close(fd);

	kmsg_line("init: replug: rebound %s (%s)\n", USB_DEV_NAME, dir);
	return 0;
}

static void usb_wait_tty_and_reexec(void);

static void cmd_reconnect(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("  reconnect: rebinding the USB controller; this console will go away\n");
	printf("  and a fresh init will come up on the recreated /dev/ttyGS0.\n");
	printf("  Re-attach minicom when the USB device reappears.\n");
	usb_rebind();
	usb_wait_tty_and_reexec();	/* re-takes the console, then execs /init */
}

/*
 * Watch the charger's online flag in a child process and rebind on a rising edge.
 * Output goes to /dev/kmsg on purpose: while the rebind is in flight this console
 * does not exist, so the record has to be visible afterwards on the screen.
 */
/* Print the OTG boost's state, so "did VBUS get switched off" is answerable. */
static void log_otg_regulator(void)
{
	DIR *d = opendir("/sys/class/regulator");

	if (!d)
		return;
	{
		struct dirent *e;

		while ((e = readdir(d))) {
			char namep[192], statep[192], name[64] = "", state[32] = "";
			int fd;
			ssize_t r;

			if (e->d_name[0] == '.')
				continue;
			snprintf(namep, sizeof(namep), "/sys/class/regulator/%s/name", e->d_name);
			fd = open(namep, O_RDONLY);
			if (fd < 0)
				continue;
			r = read(fd, name, sizeof(name) - 1);
			close(fd);
			if (r <= 0)
				continue;
			name[r] = '\0';
			if (!strstr(name, "usb-otg-vbus"))
				continue;
			snprintf(statep, sizeof(statep), "/sys/class/regulator/%s/state", e->d_name);
			fd = open(statep, O_RDONLY);
			if (fd >= 0) {
				r = read(fd, state, sizeof(state) - 1);
				close(fd);
				if (r > 0)
					state[r] = '\0';
			}
			kmsg_line("init: otg vbus regulator: %s", state[0] ? state : "?\n");
			break;
		}
	}
	closedir(d);
}

/*
 * Wait for a host to enumerate us, and only rebind if none does.
 *
 * The UDC's "configured" can be left over from whatever was attached last (a hub
 * upstream port, in the test that exposed this), so a single read is not evidence:
 * poll for a few seconds and treat "never becomes configured" as the real signal
 * that the controller needs a fresh probe.  A rebind re-runs phy_init and starts
 * the gadget again, which also clears anything the host window left behind in the
 * PHY - including, if it happened, a boost that stayed on and back-fed the PC.
 */
static int udc_wait_configured(int ms)
{
	int i;

	for (i = 0; i < ms / 200; i++) {
		if (udc_is_configured())
			return 0;
		usleep(200000);
	}
	return -1;
}

/* After switching back to device: confirm the host sees us, else rebind. */
static void usb_settle_after_device(void)
{
	log_otg_regulator();
	if (udc_wait_configured(5000) == 0) {
		kmsg_line("init: device role: host enumerated us, console should be back\n");
		return;
	}
	kmsg_line("init: device role: no host enumerated us, rebinding USB\n");
	usb_rebind();
	usb_wait_tty_and_reexec();
}

/* Is the controller attached and configured by a host? */
static int udc_is_configured(void)
{
	int fd = open("/sys/class/udc/" USB_DEV_NAME "/state", O_RDONLY);
	char buf[64];
	ssize_t n;
	int ret = 0;

	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n > 0) {
		buf[n] = '\0';
		ret = strstr(buf, "configured") != NULL;
	}
	return ret;
}

/* Wait for the charger to report a rising edge.  Returns 0 if a rebind is needed. */
static int usb_watch_loop_once(const char *charger)
{
	char path[224];
	int prev = -1;

	snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", charger);
	for (;;) {
		long v = -1;

		if (read_int_file(path, &v) == 0) {
			if (prev == 0 && v == 1) {
				/*
				 * Give the host time to re-enumerate.  If it did, the
				 * UDC reports configured and the shell's /dev/ttyGS0
				 * is still the same tty - g_serial registers it once at
				 * bind, not per attach - so the prompt comes back on its
				 * own and rebinding would only destroy it.  Observed on
				 * this board: every replug produced a new USB device and
				 * a fresh ttyACM0 on the host.
				 */
				if (udc_wait_configured(4000) == 0) {
					kmsg_line("init: replug: host re-enumerated, console should be back\n");
					return -1;	/* keep watching, change nothing */
				}
				kmsg_line("init: replug: no host enumerated us, rebinding USB\n");
				usb_rebind();
				return 0;	/* caller re-takes the console */
			}
			prev = (int)v;
		}
		usleep(500000);
	}
}

/* Take the recreated console over and start a fresh init on it. */
static void usb_wait_tty_and_reexec(void)
{
	int i;

	for (i = 0; i < 100; i++) {
		int tty = open("/dev/ttyGS0", O_RDWR);

		if (tty >= 0) {
			dup2(tty, 0);
			dup2(tty, 1);
			dup2(tty, 2);
			kmsg_line("init: replug: console back, restarting init\n");
			execl("/init", "init", NULL);
			return;
		}
		usleep(200000);
	}
	kmsg_line("init: replug: /dev/ttyGS0 never came back\n");
}

static void usb_start_watcher(void)
{
	char dir[128];
	DIR *d = opendir("/sys/class/power_supply");
	char charger[64] = "";

	if (!d)
		return;
	{
		struct dirent *e;

		while ((e = readdir(d))) {
			char ue[192], buf[256];
			int fd;
			ssize_t r;

			if (e->d_name[0] == '.')
				continue;
			snprintf(ue, sizeof(ue), "/sys/class/power_supply/%s/uevent", e->d_name);
			fd = open(ue, O_RDONLY);
			if (fd < 0)
				continue;
			r = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (r <= 0)
				continue;
			buf[r] = '\0';
			if (strstr(buf, "mediatek,mt6360-chg")) {
				snprintf(charger, sizeof(charger), "%s", e->d_name);
				break;
			}
		}
	}
	closedir(d);
	if (!charger[0])
		return;
	(void)dir;
	if (fork() == 0) {
		/*
		 * The rebind destroys and recreates the gadget's tty, so the shell
		 * this child was forked from is left holding a dead fd.  After a
		 * successful rebind, take the new /dev/ttyGS0 over and exec a fresh
		 * init: it redoes the few seconds of diagnostics, starts a new shell
		 * on the live tty, and forks a fresh watcher.  The old init stays
		 * blocked on its dead fd, which is harmless.
		 */
		for (;;)
			if (usb_watch_loop_once(charger) == 0)
				usb_wait_tty_and_reexec();
	}
}

static void print_tail(const char *path, int max_lines)
{
	static char buf[1 << 16];
	int fd = open(path, O_RDONLY);
	ssize_t total = 0, n, i;
	int lines = 0;

	if (fd < 0) {
		printf("  (missing: %s)\n", path);
		return;
	}
	while ((n = read(fd, buf + total, sizeof(buf) - total)) > 0) {
		total += n;
		if ((size_t)total == sizeof(buf))
			break;
	}
	close(fd);
	for (i = total - 1; i >= 0 && lines <= max_lines; i--)
		if (buf[i] == '\n')
			lines++;
	i = (i < 0) ? 0 : i + 2;
	kmsg_tx(buf + i, (size_t)(total - i));
	if (total && buf[total - 1] != '\n')
		printf("\n");
}

/* find: the N lines that follow every line containing @pat. */
static void print_find(const char *path, const char *pat, int after)
{
	static char buf[1 << 16];
	int fd = open(path, O_RDONLY);
	ssize_t n;
	char *p, *save;
	int hits = 0, left = 0;

	if (fd < 0) {
		printf("  (missing: %s)\n", path);
		return;
	}
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		save = NULL;
		for (p = strtok_r(buf, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
			if (left > 0) {
				printf("  %s\n", p);
				left--;
				continue;
			}
			if (strstr(p, pat)) {
				printf("  >>> %s\n", p);
				hits++;
				left = after;
			}
		}
	}
	close(fd);
	printf("  --- %d match(es) for \"%s\" ---\n", hits, pat);
}

static void dump_kernel_log_filtered(const char *filter);
static void dump_kernel_log(void)
{
	dump_kernel_log_filtered(NULL);
}

static void dump_kernel_log_filtered(const char *filter)
{
	char *buf;
	long size, n;
	size_t i;
	int matched = 0, emitted = 0;

	/*
	 * Size the read to the ring, not to a constant.  SYSLOG_ACTION_READ_ALL
	 * returns only the LAST len bytes of the buffer, so a 512 KiB cap meant
	 * that once the log had grown past that - which a few `drm` runs do on
	 * their own, since the tool's output goes to /dev/kmsg - nothing from
	 * early boot could be found any more.  That is why `dmesg HFP` came back
	 * empty even though the line exists.
	 *
	 * SYSLOG_ACTION_SIZE_BUFFER == 10, SYSLOG_ACTION_READ_ALL == 3.
	 */
	size = syscall(SYS_syslog, 10, NULL, 0);
	if (!filter)
		kmsg("init: kernel log buffer is %ld bytes\n", size);
	if (size <= 0 || size > KLOG_MAX_HARD)
		size = KLOG_MAX;

	buf = malloc(size);
	if (!buf) {
		kmsg("init: !! cannot allocate %ld bytes for the kernel log\n",
		     size);
		return;
	}

	n = syscall(SYS_syslog, 3, buf, size);
	if (n < 0) {
		kmsg("init: !! syslog(READ_ALL) failed: errno=%d\n", errno);
		free(buf);
		return;
	}
	if (filter)
		goto filtered;

	kmsg("init: recovered %ld bytes of kernel log\n", n);
	kmsg("\ninit: --- kernel log: first %d lines ---\n", KLOG_HEAD_LINES);
	for (i = 0; i < (size_t)n && emitted < KLOG_HEAD_LINES; i++) {
		size_t j = i;

		while (j < (size_t)n && buf[j] != '\n')
			j++;
		kmsg_tx(buf + i, j - i);
		kmsg_str("\n");
		emitted++;
		i = j;
	}

	kmsg("\ninit: --- kernel log: driver-relevant lines ---\n");
	for (i = 0; i < (size_t)n && matched < KLOG_MATCH_MAX; i++) {
		size_t j = i;
		int keep = 0, k;

		while (j < (size_t)n && buf[j] != '\n')
			j++;
		/* initcall_debug floods the buffer with thousands of
		 * "calling X"/"initcall X returned" lines that match almost
		 * every keyword below and bury the real probe messages. */
		if (memmem(buf + i, j - i, " initcall ", 10) ||
		    memmem(buf + i, j - i, "] initcall ", 11) ||
		    memmem(buf + i, j - i, "calling  ", 9)) {
			i = j;
			continue;
		}
		for (k = 0; klog_keep[k]; k++) {
			if (memmem(buf + i, j - i, klog_keep[k],
				   strlen(klog_keep[k]))) {
				keep = 1;
				break;
			}
		}
		if (keep) {
			kmsg_tx(buf + i, j - i);
			kmsg_str("\n");
			matched++;
		}
		i = j;
	}
	kmsg("init: --- end (%d matching lines) ---\n\n", matched);

	/* The crash/hang point is at the end of the recovered log, which the
	 * head/matched sections above never show.  Print the tail too. */
	{
		size_t start = (size_t)n;
		int lines = 0;

		while (start > 0 && lines < 80) {
			start--;
			if (buf[start] == '\n' && ++lines == 80) {
				start++;
				break;
			}
		}
		kmsg("\ninit: --- kernel log: last 80 lines ---\n");
		kmsg_tx(buf + start, (size_t)n - start);
		kmsg_str("\n");
	}

	free(buf);
	return;

filtered:
	for (i = 0; i < (size_t)n; i++) {
		size_t j = i;

		while (j < (size_t)n && buf[j] != '\n')
			j++;
		if (memmem(buf + i, j - i, filter, strlen(filter))) {
			kmsg_tx(buf + i, j - i);
			kmsg_str("\n");
			matched++;
		}
		i = j;
	}
	kmsg("--- %d line(s) matching \"%s\" ---\n", matched, filter);
	free(buf);
}

/*
 * Direct answers for the display question: is a framebuffer registered at all?
 */
static void dump_fb_state(void)
{
	kmsg("init: /proc/fb            : ");
	print_file("/proc/fb");
	kmsg("init: /proc/consoles      : ");
	print_file("/proc/consoles");
	kmsg("init: simplefb bound      : ");
	print_file("/sys/bus/platform/drivers/simple-framebuffer/");
	kmsg("init: platform devices    : ");
	print_head("/sys/bus/platform/devices/", 12);
}

/*
 * Which device tree did the kernel actually receive?  The stock MTK LK builds
 * the kernel DT from its own lk_main_dtb plus the dtbo overlay, so this is the
 * only reliable way to tell whether a custom DTB in boot.img took effect.
 */
static void print_device_tree(void)
{
	printf("init: DT model      : ");
	print_file("/proc/device-tree/model");
	printf("init: DT compatible : ");
	print_file("/proc/device-tree/compatible");
	printf("init: DT bootprof   : %s\n",
	       access("/proc/device-tree/bootprof", F_OK) == 0 ? "yes (LK merged)"
							       : "no");
	printf("init: DT odm node   : %s\n",
	       access("/proc/device-tree/odm", F_OK) == 0 ? "yes (vendor dtbo applied)"
							  : "no");
}

/*
 * Bring-up diagnostics: these tell us from a single boot whether the clock
 * driver registered its providers and whether the UART got its interrupt.
 */
static void print_diagnostics(void)
{
	mkdir("/sys/kernel/debug", 0755);
	try_mount("debugfs", "/sys/kernel/debug", "debugfs");

	printf("\ninit: --- clk_summary (head) ---\n");
	print_head("/sys/kernel/debug/clk/clk_summary", 30);
	printf("\ninit: --- /proc/interrupts (head) ---\n");
	print_head("/proc/interrupts", 14);
	printf("\ninit: --- tty driver info ---\n");
	print_file("/proc/tty/driver/serial");
	printf("\n");
}

/*
 * Point stdout/stderr at /dev/kmsg.
 *
 * This is the whole reason userspace was invisible until now: the kernel is
 * started with "console=tty0 console=ttyS0,...", so /dev/console is bound to the
 * dummy tty0 console and everything PID 1 writes simply disappears.
 *
 * Writes to /dev/kmsg go through printk(), which the kernel side mirrors into
 * the bootloader log_store ring (vprintk_emit hook), which LK relays into the
 * expdb partition on the next boot.  So this one call is what makes the
 * userspace half of the bring-up observable without a serial port.
 */
static void redirect_to_kmsg(void)
{
	int fd = open("/dev/kmsg", O_WRONLY);

	if (fd < 0) {
		mount_pseudo_fs();	/* devtmpfs may not be mounted yet */
		fd = open("/dev/kmsg", O_WRONLY);
	}
	if (fd < 0)
		return;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
	setvbuf(stdout, NULL, _IONBF, 0);
}


/* ------------------------------------------------------------------ */
/* Minimal interactive shell on the USB serial console                  */
/*                                                                      */
/* There is no shell in the initramfs and no busybox for aarch64 (the    */
/* host's is x86-64), so this is a purpose-built one: read any file,     */
/* walk a directory, filter the kernel log, inspect clocks / the USB     */
/* state, write a sysfs attribute, reboot.  All of it goes straight to   */
/* /dev/ttyGS0, so it works with no display and without reflashing to    */
/* get a diagnostic - which is the whole point after the serial console  */
/* started working.                                                     */
/*                                                                      */
/* Command output is sent with usb_only set, so the shell is not         */
/* mirrored into /dev/kmsg (that would spam the logstore ring and the    */
/* expdb relay); the boot diagnostics above still go to both.            */
/* ------------------------------------------------------------------ */

#define SH_LINE_MAX	512
#define SH_ARGV_MAX	8

static int sh_fd = -1;

static void sh_puts(const char *s)
{
	size_t n = strlen(s), off = 0;

	while (off < n) {
		ssize_t w = write(sh_fd, s + off, n - off);

		if (w <= 0)
			return;
		off += (size_t)w;
	}
}

static void sh_prompt(void)
{
	sh_puts("chopin# ");
}

/* Returns the line length, or -1 when the console went away. */
static void cmd_tasks(void);

static void print_dtprop(const char *path);

static void irq_sample(void);

static int sh_getline(char *buf, size_t max)
{
	size_t n = 0;

	for (;;) {
		char c;
		ssize_t r = read(sh_fd, &c, 1);

		if (r == 1) {
			if (c == '\r')
				continue;
			if (c == '\n') {
				sh_puts("\r\n");
				break;
			}
			if (c == 0x7f || c == '\b') {
				if (n) {
					n--;
					sh_puts("\b \b");
				}
				continue;
			}
			if (n + 1 < max) {
				buf[n++] = c;
				write(sh_fd, &c, 1);
			}
			continue;
		}
		/*
		 * r == 0 is a real EOF: with O_NONBLOCK a read that simply has
		 * no data yet fails with EAGAIN instead.  Both that and a real
		 * error mean the console went away (cable unplugged), so tell the
		 * caller rather than spinning here forever.
		 */
		if (r == 0)
			return -1;
		if (r < 0 && errno != EAGAIN && errno != EINTR)
			return -1;
		/*
		 * Idle: poll by hand, and use the idle time to record interrupt
		 * activity (see irq_sample) every ~2 s.
		 */
		{
			static unsigned int idle;

			if (++idle % 20 == 0)
				irq_sample();
		}
		usleep(100000);
	}
	buf[n] = '\0';
	return (int)n;
}

/*
 * Paint the framebuffer directly, taking fbcon out of the equation.
 *
 * A registered-but-never-drawn DRM framebuffer is black - indistinguishable
 * from a panel that never decoded a frame.  This writes vertical colour bars
 * straight into /dev/fb0, which the display hardware scans out whether or not a
 * console is attached.  If the bars show up, DDP + DSI + panel are all working
 * and the earlier black screen was only "nothing had drawn into the
 * framebuffer"; if they do not, the fault is in the display path itself.
 *
 * mmap(), because DRM's fbdev provides fb_mmap but not fb_write.
 */
static void cmd_fbpaint(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/fb0";
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	static const unsigned int bars[6] = {
		0x00ff0000, 0x0000ff00, 0x000000ff,
		0x00ffff00, 0x00ff00ff, 0x0000ffff,
	};
	unsigned char *fb;
	size_t len, y, x;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		printf("fbpaint: %s: %s\n", path, strerror(errno));
		return;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		printf("fbpaint: %s: not a framebuffer: %s\n", path,
		       strerror(errno));
		close(fd);
		return;
	}

	printf("fbpaint: %s %ux%u bpp=%u stride=%u smem=%u\n", path,
	       var.xres, var.yres, var.bits_per_pixel, fix.line_length,
	       fix.smem_len);

	if (var.bits_per_pixel != 32) {
		printf("fbpaint: only 32bpp is supported\n");
		close(fd);
		return;
	}

	len = (size_t)fix.line_length * var.yres;
	if (!fix.smem_len || len > fix.smem_len)
		len = fix.smem_len;

	fb = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		printf("fbpaint: mmap %zu bytes: %s\n", len, strerror(errno));
		close(fd);
		return;
	}

	for (y = 0; y < var.yres; y++) {
		unsigned int *line = (unsigned int *)(fb + y * fix.line_length);

		for (x = 0; x < var.xres; x++)
			line[x] = bars[(x * 6) / var.xres];
	}

	printf("fbpaint: wrote %zu bytes of colour bars\n", len);
	munmap(fb, len);
	close(fd);
}

/*
 * Display bring-up state, collected in one place.
 *
 * A single `drm` over the USB console has to answer, without a second boot:
 *   - did the mediatek-drm / mtk-dsi / panel drivers bind at all?
 *   - is there a connector, is it enabled, and which mode got picked?
 *   - what does the kernel log say about the display (probe errors, DSI/PHY
 *     PLL failures, underruns)?
 *
 * Raw display registers cannot be dumped from userspace: arm64 has no usable
 * /dev/mem (devmem_is_allowed() is always false), so register level evidence
 * has to come from the drivers' own debug output.
 */
/*
 * Display bring-up state, collected in one place - and deliberately compact.
 *
 * The USB gadget console is slow and drops characters when a command prints
 * hundreds of lines (a recursive `ls` of /sys flooded it once already), so this
 * prints only specific files: the connector, the DRM atomic state, the register
 * dump, the display clocks, the interrupt counters and the display lines of the
 * kernel log.  Anything longer is meant to be grepped:
 *
 *     grep mmsys /sys/kernel/debug/mediatek-drm/disp-regs
 *     grep dsi0  /sys/kernel/debug/mediatek-drm/disp-regs
 */
static void drm_show(const char *label, const char *path)
{
	printf("%-22s: ", label);
	print_file(path);
}

/*
 * The regulator framework's own counters answer the panel supply question, and
 * they answer it in numbers rather than in a message a driver chose to print:
 * use_count is the enable refcount, open_count the number of consumers that
 * have taken the regulator, and each consumer gets a directory of its own
 * underneath it.
 *
 * The directory name is not stable - it has shown up as both wl2866d-dvdd2 and
 * 7-0028-wl2866d-dvdd2 - so scan for the name instead of spelling out a path.
 * Reading these by hand is what went wrong repeatedly; it should not be a
 * manual step.
 */
static void dump_regulator_state(void)
{
	DIR *dir;
	struct dirent *e;

	printf("\n=== wl2866d regulators (use = enable refcount, open = consumers) ===\n");

	dir = opendir("/sys/kernel/debug/regulator");
	if (!dir) {
		printf("  (no /sys/kernel/debug/regulator)\n");
		return;
	}
	while ((e = readdir(dir))) {
		char path[300], f[340];
		long use, opened, bypass;
		DIR *cd;
		struct dirent *ce;
		int consumers = 0;

		if (!strstr(e->d_name, "wl2866d"))
			continue;

		snprintf(path, sizeof(path), "/sys/kernel/debug/regulator/%s",
			 e->d_name);
		snprintf(f, sizeof(f), "%s/use_count", path);
		if (read_int_file(f, &use))
			use = -1;
		snprintf(f, sizeof(f), "%s/open_count", path);
		if (read_int_file(f, &opened))
			opened = -1;
		snprintf(f, sizeof(f), "%s/bypass_count", path);
		if (read_int_file(f, &bypass))
			bypass = -1;

		printf("  %-22s use=%ld open=%ld bypass=%ld\n",
		       e->d_name, use, opened, bypass);

		cd = opendir(path);
		if (!cd)
			continue;
		while ((ce = readdir(cd))) {
			if (ce->d_name[0] == '.')
				continue;
			if (ce->d_type != DT_DIR)
				continue;
			/*
			 * Only the regulator carries use_count and open_count;
			 * a consumer's directory has constraint_flags, min_uV,
			 * max_uV and uA_load and nothing else.  Asking for
			 * use_count here reported -1, which reads like a
			 * failure when it is only the wrong question.
			 */
			printf("      consumer %s\n", ce->d_name);
			consumers++;
		}
		closedir(cd);
		if (!consumers)
			printf("      (nothing has taken this rail)\n");
	}
	closedir(dir);
}

static void cmd_drm(void)
{
	/*
	 * Order matters.  Everything that takes a DRM/fbdev lock comes last:
	 * reading /proc/fb blocks on fb_procfs.c's registration_lock whenever
	 * register_framebuffer() is stuck (a wedged modeset does exactly that),
	 * and it used to sit fifth, so the whole command stalled before it had
	 * printed anything useful.  The trace ring is lock-free by design and
	 * goes first.
	 */
	printf("\n=== connector ===\n");
	drm_show("status",  "/sys/class/drm/card0-DSI-1/status");
	drm_show("enabled", "/sys/class/drm/card0-DSI-1/enabled");
	drm_show("dpms",    "/sys/class/drm/card0-DSI-1/dpms");
	drm_show("modes",   "/sys/class/drm/card0-DSI-1/modes");

	printf("\n=== display trace ring (lock-free, survives a stuck printk) ===\n");
	print_file("/sys/kernel/debug/mtk-disp-trace");

	printf("\n=== display registers (grep by tag: mmsys/mutex/ovl0/rdma0/dsi0) ===\n");
	print_file("/sys/kernel/debug/mediatek-drm-regs");
	print_file("/sys/kernel/debug/mediatek-drm/disp-regs");

	printf("\n=== display clocks ===\n");
	print_matching("/sys/kernel/debug/clk/clk_summary", "mm_disp");
	print_matching("/sys/kernel/debug/clk/clk_summary", "mm_dsi");
	print_matching("/sys/kernel/debug/clk/clk_summary", "mipi_tx");

	printf("\n=== display interrupts (run drm twice, compare) ===\n");
	print_matching("/proc/interrupts", "disp");
	print_matching("/proc/interrupts", "dsi");
	print_matching("/proc/interrupts", "mutex");
	print_matching("/proc/interrupts", "mipi");

	printf("\n=== kernel log: panel readback ===\n");
	dump_kernel_log_filtered("readback");
	printf("\n=== kernel log: mediatek-drm ===\n");
	dump_kernel_log_filtered("mediatek-drm");
	printf("\n=== kernel log: route/engine read-back ===\n");
	dump_kernel_log_filtered("mmsys routes");
	dump_kernel_log_filtered("ovl started");
	dump_kernel_log_filtered("rdma started");
	dump_kernel_log_filtered("mutex ");
	dump_kernel_log_filtered("enabled: START");

	/*
	 * Deliberately last: these two take DRM/fbdev locks and will hang the
	 * whole command if the modeset is wedged.
	 */
	dump_regulator_state();

	printf("\n=== atomic state (needs DRM locks, may stall) ===\n");
	print_file("/sys/kernel/debug/dri/0/state");
	printf("\n=== /proc/fb (needs the fbdev registration lock, may stall) ===\n");
	drm_show("/proc/fb", "/proc/fb");

	printf("\n");
}

static const char *sh_usage =
	"commands:\n"
	"  help                    this text\n"
	"  dmesg [substr]          kernel log (whole, or matching lines)\n"
	"  cat <file>              print a file\n"
	"  head <n> <file>         first n lines\n"
	"  tail <n> <file>         last n lines (a window's newest text is at its end)\n"
	"  find <pat> <n> <file>   n lines after each match\n"	"  i2cscan [bus]           probe every address on one/all I2C buses\n"
	"  i2creg <bus> <a> <r> [n] read n registers from an I2C device\n"
	"  mt6360 [bus]            read the charger's id/status (default i2c6/0x34)\n"
	"  power                   dump every power supply and its properties\n"	"  battery                 real battery voltage/current + charger state\n"	"  usbhost [seconds]       timed host window (VBUS on); returns to device\n"	"  reconnect               rebind the USB controller (gadget re-enumerates)\n"
	"                          (boot also sets the charge input limit to 2 A)\n"
	"  drm                     display/DRM state + display lines of the log\n"
	"  setmode [n|fps <f>]      perform a real DRM mode change (lists modes with no arg)\n"
	"  parts                   list GPT partition names (from sysfs uevent)\n"
	"  wifiscan [ifname]       bring wlan up, trigger a scan, print BSSes\n"
	"  fwload [dev [off [size]]] mount /vendor from super and copy the Wi-Fi\n"
	"                          blobs to /lib/firmware (offset in 512-byte sectors)\n"
	"  fbpaint [dev]           fill the framebuffer with colour bars (default /dev/fb0)\n"
	"  ls <dir>                list a directory\n"
	"  lsr <dir>               list recursively (files with contents)\n"
	"  grep <substr> <file>    matching lines\n"
	"  write <file> <value>    write to a (sysfs) attribute\n"
	"  clk [substr]            clk_summary\n"
	"  irq                     /proc/interrupts\n"
	"  udc                     USB device controller state\n"
	"  trace                   lock-free display bring-up trace ring\n"
	"  tasks                   task states and wait channels\n"
	"  dtprop <path>           hexdump a device tree property\n"
	"  phy                     tphy debugfs registers\n"
	"  usb                     full USB diagnostic block\n"
	"  reboot | poweroff       reset the board\n";


/*
 * setmode - perform a real DRM mode change.
 *
 * This is the piece the port was missing.  On GNU/Linux a refresh change
 * arrives as an ordinary CRTC mode, and DRM then takes the encoder through
 * disable + enable.  That is what makes mtk_output_dsi_enable() run its full
 * path - mtk_preconfig_dsi_enable() -> mtk_dsi_calc_vdo_timing() ->
 * mtk_dsi_config_vdo_timing() - and reprogram the DSI byte timings for the new
 * mode.  Android reaches the same place through the MTK
 * CRTC_PROP_DISP_MODE_IDX property instead, which is why the vendor driver
 * only ever does it on a mode change.
 *
 * Booting sets the mode exactly once, down the path that adopts what the
 * bootloader left, so nothing ever moves the DSI off LK's timings and the
 * panel keeps running at LK's refresh whatever DRM reports.  Asking for a
 * different mode here lets DRM do the rest.
 *
 * Note that DRM considers the preferred mode (120 Hz) to be the current one,
 * so setting that alone is a no-op: switch away first and then back.
 *
 *   setmode              list the connector's modes, current one marked
 *   setmode <n>          set mode index n
 *   setmode fps <f>      set the first mode whose refresh rate is f Hz
 */
#define SETMODE_MAX_MODES 32
#define SETMODE_MAX_CONN  8
#define SETMODE_MAX_CRTC  8

static unsigned int setmode_vrefresh(const struct drm_mode_modeinfo *m)
{
	unsigned int total = m->htotal * m->vtotal;

	if (!total)
		return 0;
	return (m->clock * 1000 + total / 2) / total;
}

static void cmd_setmode(int argc, char **argv)
{
	int fd, i, ret;
	struct drm_mode_card_res res;
	struct drm_mode_get_connector conn;
	struct drm_mode_crtc crtc;
	__u32 crtcs[SETMODE_MAX_CRTC] = {0};
	__u32 connectors[SETMODE_MAX_CONN] = {0};
	static struct drm_mode_modeinfo modes[SETMODE_MAX_MODES];
	int nconn, ncrtc, use_conn = -1, cur_crtc = -1, target = -1;
	unsigned int want_fps = 0;
	__u32 cur_fb = 0;
	struct drm_mode_modeinfo cur_mode;

	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		printf("setmode: cannot open /dev/dri/card0: %s\n", strerror(errno));
		return;
	}

	memset(&res, 0, sizeof(res));
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
	res.count_crtcs = SETMODE_MAX_CRTC;
	res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
	res.count_connectors = SETMODE_MAX_CONN;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		printf("setmode: GETRESOURCES failed: %s\n", strerror(errno));
		goto out;
	}
	ncrtc = res.count_crtcs;
	nconn = res.count_connectors;
	if (ncrtc > SETMODE_MAX_CRTC || nconn > SETMODE_MAX_CONN) {
		printf("setmode: too many crtcs/connectors (%d/%d)\n", ncrtc, nconn);
		goto out;
	}

	/* the connected connector that has modes */
	for (i = 0; i < nconn; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = connectors[i];
		conn.modes_ptr = (uint64_t)(uintptr_t)modes;
		conn.count_modes = SETMODE_MAX_MODES;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		if (conn.connection == 1 && conn.count_modes > 0) {
			use_conn = i;
			break;
		}
	}
	if (use_conn < 0) {
		printf("setmode: no connected connector with modes\n");
		goto out;
	}
	if (conn.count_modes > SETMODE_MAX_MODES)
		conn.count_modes = SETMODE_MAX_MODES;

	/* the crtc that is actually scanning out */
	for (i = 0; i < ncrtc; i++) {
		memset(&crtc, 0, sizeof(crtc));
		crtc.crtc_id = crtcs[i];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0)
			continue;
		if (crtc.mode_valid && crtc.fb_id) {
			cur_crtc = i;
			cur_fb = crtc.fb_id;
			cur_mode = crtc.mode;
			break;
		}
	}
	if (cur_crtc < 0) {
		printf("setmode: no crtc is scanning out\n");
		goto out;
	}

	/* what was asked for */
	if (argc >= 3 && !strcmp(argv[1], "fps")) {
		want_fps = (unsigned int)atoi(argv[2]);
		for (i = 0; i < (int)conn.count_modes; i++) {
			if (setmode_vrefresh(&modes[i]) == want_fps) {
				target = i;
				break;
			}
		}
		if (target < 0) {
			printf("setmode: no mode at %u Hz\n", want_fps);
			goto out;
		}
	} else if (argc >= 2) {
		target = atoi(argv[1]);
		if (target < 0 || target >= (int)conn.count_modes) {
			printf("setmode: mode index %d out of range (0..%u)\n",
			       target, conn.count_modes - 1);
			goto out;
		}
	}

	printf("setmode: connector %u (%u modes), crtc %u fb %u\n",
	       conn.connector_id, conn.count_modes, crtcs[cur_crtc], cur_fb);
	for (i = 0; i < (int)conn.count_modes; i++) {
		int is_cur = (modes[i].clock == cur_mode.clock &&
			      modes[i].htotal == cur_mode.htotal &&
			      modes[i].vtotal == cur_mode.vtotal);
		printf("  [%2d] %s %ux%u %u Hz clock=%u htotal=%u vtotal=%u%s%s\n",
		       i, modes[i].name, modes[i].hdisplay, modes[i].vdisplay,
		       setmode_vrefresh(&modes[i]), modes[i].clock,
		       modes[i].htotal, modes[i].vtotal,
		       is_cur ? "  <- current" : "",
		       (modes[i].type & DRM_MODE_TYPE_PREFERRED) ? " (preferred)" : "");
	}

	if (target < 0) {
		printf("setmode: nothing to do; give an index or 'fps <f>'\n");
		goto out;
	}

	if (modes[target].clock == cur_mode.clock &&
	    modes[target].htotal == cur_mode.htotal &&
	    modes[target].vtotal == cur_mode.vtotal) {
		printf("setmode: mode %d is already current - DRM would treat this as a no-op.\n"
		       "         Switch to another mode first, then back.\n", target);
		goto out;
	}

	printf("setmode: setting mode %d (%u Hz) on crtc %u ...\n",
	       target, setmode_vrefresh(&modes[target]), crtcs[cur_crtc]);
	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = crtcs[cur_crtc];
	crtc.fb_id = cur_fb;
	crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&connectors[use_conn];
	crtc.count_connectors = 1;
	crtc.mode_valid = 1;
	crtc.mode = modes[target];

	ret = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	if (ret < 0)
		printf("setmode: SETCRTC failed: %s\n", strerror(errno));
	else
		printf("setmode: done, now at %u Hz\n",
		       setmode_vrefresh(&modes[target]));

out:
	close(fd);
}


/* ------------------------------------------------------------------ */
/* Vendor firmware extraction                                          */
/*                                                                     */
/* The Wi-Fi RAM code / ROM patch / NVRAM blobs live in /vendor, which  */
/* on this device is a logical partition inside Android's "super"      */
/* partition.  Nothing on the kernel side can mount it by name: there   */
/* is no ueventd to create by-name links, and "super" is a container of */
/* ext4 images, not a filesystem.                                       */
/*                                                                     */
/* lpdump on the running stock build says:                              */
/*                                                                     */
/*   Name: vendor_a                                                     */
/*   Extents:                                                           */
/*     0 .. 4428399 linear super 13246464                               */
/*                                                                     */
/* so the vendor ext4 starts at sector 13246464 of /dev/sdc61 and is    */
/* 4428400 sectors long.  A loop device with an offset turns that back  */
/* into a mountable filesystem.  The numbers are per-device metadata;   */
/* "fwload <dev> <offset-in-sectors>" overrides them.                   */
/*                                                                     */
/* The kernel drivers probe long before this runs, which is why the     */
/* Wi-Fi glue defers its NVRAM load + power-on: it retries for ~120 s   */
/* waiting for /lib/firmware to appear.                                 */
/* ------------------------------------------------------------------ */
#define VENDOR_SUPER_DEV		"/dev/sdc61"
#define VENDOR_OFFSET_SECTORS		13246464ULL
#define VENDOR_SIZE_SECTORS		4428400ULL

static int mkdir_p(const char *path)
{
	char tmp[256];
	size_t len;
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (len && tmp[len - 1] == '/')
		tmp[len - 1] = 0;
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) && errno != EEXIST)
		return -1;
	return 0;
}

static int copy_one(const char *src, const char *dst)
{
	char buf[65536];
	int in, out;
	ssize_t n;
	struct stat st;

	if (stat(src, &st)) {
		printf("  %-58s missing\n", src);
		return -1;
	}
	in = open(src, O_RDONLY);
	if (in < 0) {
		printf("  %-58s open: %s\n", src, strerror(errno));
		return -1;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		printf("  %-58s create %s: %s\n", src, dst, strerror(errno));
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;

		while (off < n) {
			ssize_t w = write(out, buf + off, n - off);

			if (w <= 0) {
				printf("  %-58s short write\n", src);
				close(in);
				close(out);
				return -1;
			}
			off += w;
		}
	}
	close(in);
	close(out);
	printf("  %-58s %lld bytes\n", src, (long long)st.st_size);
	return 0;
}

/* copy /vendor/firmware/mediatek/mt<id>/ (NVRAM calibration, small) */
static void copy_mediatek_dir(const char *from, const char *to)
{
	DIR *d = opendir(from);
	struct dirent *e;

	if (!d)
		return;
	mkdir_p(to);
	while ((e = readdir(d))) {
		char s[512], t[512];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(s, sizeof(s), "%s/%s", from, e->d_name);
		snprintf(t, sizeof(t), "%s/%s", to, e->d_name);
		if (stat(s, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			copy_mediatek_dir(s, t);
		else if (S_ISREG(st.st_mode))
			copy_one(s, t);
	}
	closedir(d);
}


/*
 * Find a /dev node by GPT partition name.  The kernel exports the name in the
 * partition's uevent (PARTNAME=...), which is the only by-name channel we have
 * without udev: there is no /dev/disk/by-partlabel in this initramfs.
 */
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

static void cmd_parts(void)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;

	if (!d) {
		printf("parts: /sys/class/block missing\n");
		return;
	}
	while ((e = readdir(d))) {
		char p[256], buf[1024], *name, *dev;
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
		name = strstr(buf, "PARTNAME=");
		dev = strstr(buf, "DEVNAME=");
		if (!name || !dev)
			continue;
		name += strlen("PARTNAME=");
		dev += strlen("DEVNAME=");
		*(strchr(name, '\n') ?: name + strlen(name)) = 0;
		*(strchr(dev, '\n') ?: dev + strlen(dev)) = 0;
		printf("  %-12s %s\n", dev, name);
	}
	closedir(d);
}

static int mount_ro(const char *dev, const char *mnt, const char *fstype)
{
	mkdir_p(mnt);
	if (mount(dev, mnt, fstype, MS_RDONLY, NULL)) {
		if (errno == EBUSY) {
			printf("fwload: %s already mounted on %s\n", dev, mnt);
			return 0;
		}
		printf("fwload: mount %s on %s: %s\n", dev, mnt,
		       strerror(errno));
		return -1;
	}
	printf("fwload: %s mounted on %s (ro, %s)\n", dev, mnt, fstype);
	return 0;
}

static int find_named(const char *dir, const char *name, int depth,
		      char *out, size_t outsz)
{
	DIR *d;
	struct dirent *e;
	int ret = -1;

	if (depth > 4)
		return -1;
	d = opendir(dir);
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char p[512];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		if (stat(p, &st))
			continue;
		if (S_ISREG(st.st_mode)) {
			if (!strcmp(e->d_name, name)) {
				snprintf(out, outsz, "%s", p);
				ret = 0;
				break;
			}
		} else if (S_ISDIR(st.st_mode)) {
			if (find_named(p, name, depth + 1, out, outsz) == 0) {
				ret = 0;
				break;
			}
		}
	}
	closedir(d);
	return ret;
}

/* NVRAM has to come from the nvdata partition; it is not in /vendor. */
/*
 * The stock BT_Addr NV item keeps the address in display order
 * (04:e5:98:..), while the BT driver consumes it as little-endian
 * bdaddr bytes, so the file it loads has to be the reverse.
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
		printf("init: BT_Addr %s is %d bytes, skipped\n", src, n);
		return;
	}
	for (i = 0; i < 6; i++)
		r[i] = b[5 - i];

	f = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (f < 0)
		return;
	if (write(f, r, 6) != 6)
		printf("init: BT_Addr short write\n");
	close(f);
	printf("init: BT_Addr %02x:%02x:%02x:%02x:%02x:%02x\n",
	       r[5], r[4], r[3], r[2], r[1], r[0]);
}

static void fwload_nvdata(void)
{
	static const char *const cand[] = {
		/* what this device actually uses (found by the recursive scan) */
		"/nvdata/APCFG/APRDEB/WIFI",
		"/nvdata/mediatek/mt6893/WIFI",
		"/nvdata/mt6893/WIFI",
		"/nvdata/media/mediatek/mt6893/WIFI",
		"/nvdata/media/mt6893/WIFI",
		"/nvdata/WIFI",
		"/nvdata/media/WIFI",
		"/nvdata/media/0/WIFI",
	};
	char dev[64], src[512];
	int i;

	if (find_part("nvdata", dev, sizeof(dev))) {
		printf("fwload: no nvdata partition in /sys/class/block\n");
		return;
	}
	if (mount_ro(dev, "/nvdata", "ext4"))
		return;

	mkdir_p("/lib/firmware/mediatek/mt6893");

	/* Bluetooth address: the BT driver loads it as firmware.  The stock
	 * stack keeps it next to the WIFI NVRAM in nvdata.
	 */
	if (!find_named("/nvdata", "BT_Addr", 0, src, sizeof(src))) {
		printf("fwload: BT_Addr from %s\n", src);
		install_bt_addr(src, "/lib/firmware/mediatek/mt6893/BT_Addr");
	}

	for (i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
		struct stat st;

		if (!stat(cand[i], &st) && S_ISREG(st.st_mode)) {
			copy_one(cand[i], "/lib/firmware/mediatek/mt6893/WIFI");
			return;
		}
	}
	if (!find_named("/nvdata", "WIFI", 0, src, sizeof(src))) {
		printf("fwload: found NVRAM at %s\n", src);
		copy_one(src, "/lib/firmware/mediatek/mt6893/WIFI");
		return;
	}
	printf("fwload: no WIFI file under /nvdata; listing:\n");
	dump_dir_tree("/nvdata", 0);
}

static void cmd_fwload(int argc, char **argv)
{
	char devbuf[64];
	const char *dev = argc > 1 ? argv[1] : NULL;
	unsigned long long off = (argc > 2 ? strtoull(argv[2], NULL, 0)
					   : VENDOR_OFFSET_SECTORS) * 512ULL;
	unsigned long long size = (argc > 3 ? strtoull(argv[3], NULL, 0)
					    : VENDOR_SIZE_SECTORS) * 512ULL;
	static const char *const files[] = {
		"WIFI_RAM_CODE_soc3_0_1a_1.bin",
		"soc3_0_patch_wmmcu_1a_1_hdr.bin",
		"soc3_0_ram_wifi_1a_1_hdr.bin",
		"soc3_0_ram_wmmcu_1a_1_hdr.bin",
		"soc3_0_ram_mcu_1a_1_hdr.bin",
		"wifi.cfg",
		"wifi_sigma.cfg",
		"txpowerctrl.cfg",
		"conninfra.cfg",
		/* Bluetooth: connac ROM patch + BT fw config. */
		"soc3_0_ram_bt_1a_1_hdr.bin",
		"soc3_0_ram_bt_1_1_hdr.bin",
		"BT_FW.cfg",
	};
	struct loop_info64 info;
	int ctl, loopfd, ffd, i;
	char loopname[32];

	if (!dev) {
		if (find_part("super", devbuf, sizeof(devbuf)))
			snprintf(devbuf, sizeof(devbuf), "%s", VENDOR_SUPER_DEV);
		dev = devbuf;
	}
	printf("fwload: %s offset=%llu size=%llu\n", dev, off, size);

	ffd = open(dev, O_RDONLY);
	if (ffd < 0) {
		printf("fwload: open %s: %s\n", dev, strerror(errno));
		return;
	}

	/* Prefer a free loop device so repeated calls work; fall back to 0. */
	ctl = open("/dev/loop-control", O_RDWR);
	if (ctl >= 0) {
		i = ioctl(ctl, LOOP_CTL_GET_FREE);
		close(ctl);
	} else {
		i = -1;
	}
	if (i < 0)
		i = 0;
	snprintf(loopname, sizeof(loopname), "/dev/loop%d", i);
	loopfd = open(loopname, O_RDWR);
	if (loopfd < 0) {
		printf("fwload: open %s: %s\n", loopname, strerror(errno));
		close(ffd);
		return;
	}
	if (ioctl(loopfd, LOOP_SET_FD, ffd)) {
		printf("fwload: LOOP_SET_FD: %s\n", strerror(errno));
		close(loopfd);
		close(ffd);
		return;
	}
	memset(&info, 0, sizeof(info));
	info.lo_offset = off;
	info.lo_sizelimit = size;
	snprintf((char *)info.lo_file_name, LO_NAME_SIZE, "%s", dev);
	if (ioctl(loopfd, LOOP_SET_STATUS64, &info)) {
		printf("fwload: LOOP_SET_STATUS64: %s\n", strerror(errno));
		ioctl(loopfd, LOOP_CLR_FD, 0);
		close(loopfd);
		close(ffd);
		return;
	}

	mkdir_p("/vendor");
	if (mount(loopname, "/vendor", "ext4", MS_RDONLY, NULL)) {
		printf("fwload: mount %s: %s\n", loopname, strerror(errno));
		ioctl(loopfd, LOOP_CLR_FD, 0);
		close(loopfd);
		close(ffd);
		return;
	}
	printf("fwload: %s mounted on /vendor (ro)\n", loopname);

	mkdir_p("/lib/firmware");
	for (i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
		char src[256], dst[256];

		snprintf(src, sizeof(src), "/vendor/firmware/%s", files[i]);
		snprintf(dst, sizeof(dst), "/lib/firmware/%s", files[i]);
		copy_one(src, dst);
	}
	/* The BT driver looks for its config as "bt.cfg". */
	copy_one("/lib/firmware/BT_FW.cfg", "/lib/firmware/bt.cfg");

	/* NVRAM calibration for the connectivity combo. */
	copy_mediatek_dir("/vendor/firmware/mediatek/mt6893",
			  "/lib/firmware/mediatek/mt6893");
	fwload_nvdata();

	/* Leave /vendor mounted: "ls /vendor/firmware" then shows what is there. */
}


/* ------------------------------------------------------------------ */
/* wifiscan - ask cfg80211 for a scan and print what came back         */
/*                                                                     */
/* The initramfs has no iw/wpa_supplicant, and the useful question at   */
/* this point is not "does a netdev exist" (it does) but "does the RF   */
/* side work": a scan needs the firmware, the WFDMA rings and the       */
/* calibration path to be alive.  Talk generic netlink directly.        */
/* ------------------------------------------------------------------ */
static int nla_put(struct nlmsghdr *nlh, int maxlen, int type,
		   const void *data, int len)
{
	struct nlattr *a;
	int alen = NLA_HDRLEN + len;
	int newlen = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(alen);

	if (newlen > maxlen)
		return -1;
	a = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
	a->nla_type = type;
	a->nla_len = alen;
	if (len)
		memcpy((char *)a + NLA_HDRLEN, data, len);
	memset((char *)a + alen, 0, NLA_ALIGN(alen) - alen);
	nlh->nlmsg_len = newlen;
	return 0;
}

static struct nlattr *nla_find(struct nlattr *head, int len, int type)
{
	struct nlattr *a;
	int rem = len;

	for (a = head; rem >= (int)sizeof(*a) && a->nla_len >= sizeof(*a) &&
	     a->nla_len <= rem;
	     a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len)),
	     rem -= NLA_ALIGN(a->nla_len)) {
		if ((a->nla_type & NLA_TYPE_MASK) == type)
			return a;
	}
	return NULL;
}

/* Word-scan variant: does not trust nla_len of preceding attributes.
 * The kernel's 64-bit attributes carry a separate pad attribute, and we
 * were stopping one attribute short of NL80211_ATTR_BSS, so scan for the
 * type word instead of walking the chain.
 */
/* Tolerant attribute walker: the kernel emits 64-bit attributes followed
 * by a zero-length pad word, which makes a plain nla_len chain hit a
 * zero length and stop.  Walk lengths, but skip a zero word instead of
 * giving up, and stop as soon as the requested type shows up.
 */
static struct nlattr *attr_scan(void *base, int len, int type)
{
	int off = 0;

	while (off + 4 <= len) {
		uint32_t v = *(uint32_t *)((char *)base + off);
		int alen = v & 0xffff;
		int atype = (v >> 16) & 0x3fff;

		if (alen < 4) {
			off += 4;
			continue;
		}
		if (atype == type)
			return (struct nlattr *)((char *)base + off);
		if (alen > len - off)
			break;
		off += (alen + 3) & ~3;
	}
	return NULL;
}

static struct nlattr *nla_nest_start(struct nlmsghdr *nlh, int maxlen,
				      int type)
{
	struct nlattr *a;

	if (NLMSG_ALIGN(nlh->nlmsg_len) + NLA_HDRLEN > maxlen)
		return NULL;
	a = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
	a->nla_type = type | NLA_F_NESTED;
	a->nla_len = NLA_HDRLEN;
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_HDRLEN;
	return a;
}

static void nla_nest_end(struct nlmsghdr *nlh, struct nlattr *a)
{
	a->nla_len = (char *)nlh + nlh->nlmsg_len - (char *)a;
}

static int genl_family_id(int fd, const char *name)
{
	char buf[512];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct iovec iov = { buf, 0 };
	struct msghdr msg = { &sa, sizeof(sa), &iov, 1, NULL, 0, 0 };
	int n, family = -1, rem;
	struct nlattr *a;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = 1;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = CTRL_CMD_GETFAMILY;
	gh->version = 1;
	nla_put(nlh, sizeof(buf), CTRL_ATTR_FAMILY_NAME, name,
		strlen(name) + 1);

	if (sendto(fd, nlh, nlh->nlmsg_len, 0, (void *)&sa, sizeof(sa)) < 0)
		return -1;
	iov.iov_len = sizeof(buf);
	n = recvmsg(fd, &msg, 0);
	if (n < 0)
		return -1;
	if (nlh->nlmsg_type == NLMSG_ERROR)
		return -1;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	a = nla_find((struct nlattr *)((char *)gh + GENL_HDRLEN),
		     nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN),
		     CTRL_ATTR_FAMILY_ID);
	if (!a)
		return -1;
	rem = a->nla_len - NLA_HDRLEN;
	if (rem >= 2)
		family = *(uint16_t *)((char *)a + NLA_HDRLEN);
	return family;
}

static int if_up(const char *name)
{
	struct ifreq ifr;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
		close(s);
		return -1;
	}
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
		close(s);
		return -1;
	}
	close(s);
	return 0;
}

static int nl_scan_dump(int fd, int family, int ifindex)
{
	char buf[16384];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct timeval tv = { .tv_sec = 2 };
	uint32_t u32;
	int found = 0, msgs = 0, bssattr = 0;

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = 3;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = NL80211_CMD_GET_SCAN;
	gh->version = 1;
	u32 = ifindex;
	nla_put(nlh, sizeof(buf), NL80211_ATTR_IFINDEX, &u32, sizeof(u32));
	{
		/* one zero-length SSID = wildcard -> active scan, not passive */
		struct nlattr *ssids = nla_nest_start(nlh, sizeof(buf),
						      NL80211_ATTR_SCAN_SSIDS);

		nla_put(nlh, sizeof(buf), 0, NULL, 0);
		if (ssids)
			nla_nest_end(nlh, ssids);
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (sendto(fd, nlh, nlh->nlmsg_len, 0, (void *)&sa, sizeof(sa)) < 0)
		return -1;

	for (;;) {
		int n = recv(fd, buf, sizeof(buf), 0);

		if (n <= 0)
			break;
		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, n);
		     nlh = NLMSG_NEXT(nlh, n)) {
			struct nlattr *a, *bss;
			const uint8_t *bssid = NULL, *ie = NULL;
			uint32_t freq = 0;
			int32_t sig = 0;
			char ssid[33] = "";
			int alen, ielen = 0;

			if (nlh->nlmsg_type == NLMSG_DONE) {
				n = 0;
				break;
			}
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e =
					(struct nlmsgerr *)NLMSG_DATA(nlh);

				printf("wifiscan: dump error %d (%s)\n",
				       e->error, strerror(-e->error));
				continue;
			}
			msgs++;
			gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
			if (gh->cmd != NL80211_CMD_NEW_SCAN_RESULTS) {
				printf("wifiscan: msg cmd=%u (not NEW_SCAN_RESULTS)\n",
				       gh->cmd);
				continue;
			}
			alen = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
			bss = attr_scan((char *)gh + GENL_HDRLEN, alen,
					NL80211_ATTR_BSS);
			if (!bss) {
				if (msgs == 1) {
					struct nlattr *a;
					int rem = alen;

					uint32_t *w = (uint32_t *)((char *)gh + GENL_HDRLEN);

					printf("wifiscan: msg1 len=%u alen=%d BSSattr=%d\n",
					       nlh->nlmsg_len, alen,
					       NL80211_ATTR_BSS);
					printf("wifiscan: raw %08x %08x %08x %08x %08x %08x %08x %08x\n",
					       w[0], w[1], w[2], w[3], w[4], w[5],
					       w[6], w[7]);
					for (a = (struct nlattr *)((char *)gh + GENL_HDRLEN);
					     rem >= (int)sizeof(*a) &&
					     a->nla_len >= sizeof(*a) &&
					     a->nla_len <= rem;
					     a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len)),
					     rem -= NLA_ALIGN(a->nla_len))
						printf("wifiscan:   attr type=%u len=%u\n",
						       a->nla_type & NLA_TYPE_MASK,
						       a->nla_len);
				}
				printf("wifiscan: NEW_SCAN_RESULTS without NL80211_ATTR_BSS\n");
				continue;
			}
			bssattr++;
			{
				int bssoff = (char *)bss -
					((char *)gh + GENL_HDRLEN);
				char *bdata = (char *)bss + NLA_HDRLEN;
				int blen = alen - bssoff - NLA_HDRLEN;

				if (bss->nla_len >= NLA_HDRLEN &&
				    (int)bss->nla_len - NLA_HDRLEN < blen)
					blen = bss->nla_len - NLA_HDRLEN;

				a = attr_scan(bdata, blen,
					      NL80211_BSS_BSSID);
				if (a)
					bssid = (const uint8_t *)a + NLA_HDRLEN;
				a = attr_scan(bdata, blen,
					      NL80211_BSS_FREQUENCY);
				if (a)
					freq = *(uint32_t *)((char *)a + NLA_HDRLEN);
				a = attr_scan(bdata, blen,
					      NL80211_BSS_SIGNAL_MBM);
				if (a)
					sig = *(int32_t *)((char *)a + NLA_HDRLEN) / 100;
				a = attr_scan(bdata, blen,
					      NL80211_BSS_INFORMATION_ELEMENTS);
				if (a) {
					ie = (const uint8_t *)a + NLA_HDRLEN;
					ielen = a->nla_len - NLA_HDRLEN;
				}
			}
			if (ie) {
				const uint8_t *p = ie;
				int rem = ielen;
				int k;

				while (rem >= 2 && 2 + p[1] <= rem) {
					if (p[0] == 0) { /* SSID element */
						int sl = p[1];

						if (sl > 32)
							sl = 32;
						for (k = 0; k < sl; k++)
							ssid[k] = (p[2 + k] >= 32 &&
								   p[2 + k] < 127)
								  ? p[2 + k] : '.';
						ssid[sl] = 0;
						break;
					}
					rem -= 2 + p[1];
					p += 2 + p[1];
				}
			}
			if (!ssid[0])
				snprintf(ssid, sizeof(ssid), "<hidden>");
			printf("  %-24s %02x:%02x:%02x:%02x:%02x:%02x  %5u MHz  %4d dBm\n",
			       ssid,
			       bssid ? bssid[0] : 0, bssid ? bssid[1] : 0,
			       bssid ? bssid[2] : 0, bssid ? bssid[3] : 0,
			       bssid ? bssid[4] : 0, bssid ? bssid[5] : 0,
			       freq, sig);
			found++;
		}
		if (n == 0)
			break;
	}
	printf("wifiscan: dump msgs=%d bss_attr=%d found=%d\n",
	       msgs, bssattr, found);
	return found;
}

static void cmd_wifiscan(int argc, char **argv)
{
	const char *ifname = argc > 1 ? argv[1] : "wlan0";
	char buf[8192];
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct genlmsghdr *gh;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct timeval tv = { .tv_sec = 2 };
	int fd, family, ifindex, ret, attempt, total = 0;
	uint32_t u32;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		printf("wifiscan: netlink socket: %s\n", strerror(errno));
		return;
	}
	if (bind(fd, (void *)&sa, sizeof(sa)) < 0) {
		printf("wifiscan: bind: %s\n", strerror(errno));
		close(fd);
		return;
	}
	family = genl_family_id(fd, "nl80211");
	if (family < 0) {
		printf("wifiscan: nl80211 not registered (is cfg80211 up?)\n");
		close(fd);
		return;
	}
	ifindex = if_nametoindex(ifname);
	if (!ifindex) {
		printf("wifiscan: no interface %s (have wlan0/wlan1/p2p0/ap0)\n",
		       ifname);
		close(fd);
		return;
	}
	printf("wifiscan: %s ifindex=%d\n", ifname, ifindex);
	if (if_up(ifname))
		printf("wifiscan: SIOCSIFFLAGS: %s\n", strerror(errno));
	usleep(800000);

	memset(buf, 0, sizeof(buf));
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = 2;
	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = NL80211_CMD_TRIGGER_SCAN;
	gh->version = 1;
	u32 = ifindex;
	nla_put(nlh, sizeof(buf), NL80211_ATTR_IFINDEX, &u32, sizeof(u32));
	{
		/* one zero-length SSID = wildcard -> active scan, not passive */
		struct nlattr *ssids = nla_nest_start(nlh, sizeof(buf),
						      NL80211_ATTR_SCAN_SSIDS);

		nla_put(nlh, sizeof(buf), 0, NULL, 0);
		if (ssids)
			nla_nest_end(nlh, ssids);
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (sendto(fd, nlh, nlh->nlmsg_len, 0, (void *)&sa, sizeof(sa)) < 0) {
		printf("wifiscan: trigger send: %s\n", strerror(errno));
		close(fd);
		return;
	}
	ret = recv(fd, buf, sizeof(buf), 0);
	if (ret <= 0) {
		printf("wifiscan: no trigger ack: %s\n",
		       ret < 0 ? strerror(errno) : "eof");
	} else if (nlh->nlmsg_type == NLMSG_ERROR &&
		   ((struct nlmsgerr *)NLMSG_DATA(nlh))->error) {
		printf("wifiscan: trigger rejected: %s\n",
		       strerror(-((struct nlmsgerr *)NLMSG_DATA(nlh))->error));
	} else {
		printf("wifiscan: scan accepted, waiting for results\n");
	}

	/* a full scan can take several seconds; poll the result table */
	for (attempt = 0; attempt < 8; attempt++) {
		usleep(2000000);
		total = nl_scan_dump(fd, family, ifindex);
		printf("wifiscan: attempt %d: %d BSS%s\n", attempt + 1, total,
		       total == 1 ? "" : "es");
		if (total > 0)
			break;
	}
	if (total <= 0)
		printf("wifiscan: no BSS. RF/FEM/calibration or channel list, not netdev setup.\n");
	close(fd);
}

static void sh_run(char *line)
{
	char *argv[SH_ARGV_MAX];
	int argc = 0;
	char *p, *save = NULL;

	for (p = strtok_r(line, " \t", &save); p && argc < SH_ARGV_MAX;
	     p = strtok_r(NULL, " \t", &save))
		argv[argc++] = p;
	if (!argc)
		return;

	usb_only = 1;		/* shell output must not enter the kernel log */

	if (!strcmp(argv[0], "help")) {
		sh_puts(sh_usage);
	} else if (!strcmp(argv[0], "dmesg")) {
		if (argc > 1)
			dump_kernel_log_filtered(argv[1]);
		else
			dump_kernel_log();
	} else if (!strcmp(argv[0], "w") && argc > 2) {
		write_file(argv[1], argv[2]);
	} else if (!strcmp(argv[0], "cat") && argc > 1) {
		cat_file(argv[1]);
	} else if (!strcmp(argv[0], "head") && argc > 2) {
		print_head(argv[2], atoi(argv[1]));
	} else if (!strcmp(argv[0], "tail") && argc > 2) {
		print_tail(argv[2], atoi(argv[1]));
	} else if (!strcmp(argv[0], "find") && argc > 3) {
		print_find(argv[3], argv[1], atoi(argv[2]));
	} else if (!strcmp(argv[0], "i2cscan")) {
		cmd_i2cscan(argc, argv);
	} else if (!strcmp(argv[0], "i2creg") && argc > 3) {
		cmd_i2creg(argc, argv);
	} else if (!strcmp(argv[0], "mt6360")) {
		cmd_mt6360(argc, argv);
	} else if (!strcmp(argv[0], "power")) {
		cmd_power(argc, argv);
	} else if (!strcmp(argv[0], "battery")) {
		cmd_battery(argc, argv);
	} else if (!strcmp(argv[0], "usbhost")) {
		cmd_usbhost(argc, argv);
	} else if (!strcmp(argv[0], "reconnect")) {
		cmd_reconnect(argc, argv);
	} else if (!strcmp(argv[0], "fwload")) {
		cmd_fwload(argc, argv);
	} else if (!strcmp(argv[0], "parts")) {
		cmd_parts();
	} else if (!strcmp(argv[0], "wifiscan")) {
		cmd_wifiscan(argc, argv);
	} else if (!strcmp(argv[0], "setmode")) {
		cmd_setmode(argc, argv);
	} else if (!strcmp(argv[0], "drm")) {
		cmd_drm();
	} else if (!strcmp(argv[0], "fbpaint")) {
		cmd_fbpaint(argc, argv);
	} else if (!strcmp(argv[0], "ls") && argc > 1) {
		dump_dir_tree(argv[1], 0);
	} else if (!strcmp(argv[0], "lsr") && argc > 1) {
		dump_dir_tree(argv[1], 0);
	} else if (!strcmp(argv[0], "trace")) {
		print_file("/sys/kernel/debug/mtk-disp-trace");
	} else if (!strcmp(argv[0], "tasks")) {
		cmd_tasks();
	} else if (!strcmp(argv[0], "dtprop") && argc > 1) {
		print_dtprop(argv[1]);
	} else if (!strcmp(argv[0], "grep") && argc > 2) {
		print_matching(argv[2], argv[1]);
	} else if (!strcmp(argv[0], "write") && argc > 2) {
		int fd = open(argv[1], O_WRONLY);

		if (fd < 0) {
			printf("write: %s: %s\n", argv[1], strerror(errno));
		} else {
			if (write(fd, argv[2], strlen(argv[2])) < 0)
				printf("write: %s\n", strerror(errno));
			close(fd);
		}
	} else if (!strcmp(argv[0], "clk")) {
		print_matching("/sys/kernel/debug/clk/clk_summary",
			       argc > 1 ? argv[1] : "");
	} else if (!strcmp(argv[0], "irq")) {
		print_file("/proc/interrupts");
	} else if (!strcmp(argv[0], "udc")) {
		dump_udc();
	} else if (!strcmp(argv[0], "phy")) {
		dump_dir_tree("/sys/kernel/debug/phy", 0);
	} else if (!strcmp(argv[0], "usb")) {
		dump_usb_diag();
	} else if (!strcmp(argv[0], "reboot")) {
		sh_puts("rebooting\n");
		sync();
		reboot(RB_AUTOBOOT);
	} else if (!strcmp(argv[0], "poweroff")) {
		sh_puts("powering off\n");
		sync();
		reboot(RB_POWER_OFF);
	} else {
		printf("%s: unknown command (try 'help')\n", argv[0]);
	}

	usb_only = 0;
}

/*
 * Wait for the console to come back, then take it over again.
 *
 * Unplugging the cable makes reads on the gadget's tty fail (EIO) or return EOF,
 * and treating that as end of input used to end this process - after which
 * nothing read /dev/ttyGS0 any more, so minicom could connect and never see a
 * prompt.  That looks exactly like a dead board even though the kernel is fine
 * (the log timestamps keep advancing), which is what the screen showed.
 *
 * g_serial registers the tty once at bind and keeps it across attach cycles, but
 * re-opening is the reliable way to pick up a fresh connection, so wait here
 * instead of leaving.
 */
static void sh_wait_for_console(void)
{
	kmsg_line("init: console lost (cable out?); waiting for it to come back\n");
	for (;;) {
		int tty = open("/dev/ttyGS0", O_RDWR | O_NONBLOCK);

		if (tty >= 0) {
			sh_fd = tty;
			dup2(tty, 0);
			dup2(tty, 1);
			dup2(tty, 2);
			sh_puts("\r\nchopin initramfs shell - console reconnected\r\n");
			kmsg_line("init: console back\n");
			return;
		}
		usleep(300000);
	}
}


/*
 * Interrupt storm recorder.
 *
 * The board freezes shortly after the display takes over and the shell goes
 * down with it, so a shell command cannot report what happened.  This samples
 * /proc/interrupts while the shell waits for input and writes to /dev/kmsg:
 *
 *  - a line for the display blocks (disp/dsi/mutex/mipi) whenever one of them
 *    is moving, and a heartbeat with their current counts every ~30 s, so the
 *    last heartbeat before a freeze is visible;
 *  - a line for any interrupt moving faster than 2000 per sample, whatever its
 *    name, which is what an interrupt storm looks like.
 *
 * It must write to /dev/kmsg explicitly: after main() points fd 1 at the USB
 * tty, kmsg_tx() no longer reaches printk, and printk is what the logstore
 * driver keeps in DRAM for the next boot to recover (the earlier version wrote
 * to the console twice and left nothing behind for a post-freeze reading).
 */
static int kmsg_watch_fd = -1;

static void kmsg_watch(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	if (kmsg_watch_fd < 0) {
		kmsg_watch_fd = open("/dev/kmsg", O_WRONLY);
		if (kmsg_watch_fd < 0)
			return;
	}
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		write(kmsg_watch_fd, buf, (size_t)n < sizeof(buf) ? (size_t)n
								   : sizeof(buf) - 1);
}

/* does this line belong to the display pipeline? */
static int irq_is_display(const char *label)
{
	return strstr(label, "disp") || strstr(label, "dsi") ||
	       strstr(label, "mutex") || strstr(label, "mipi") ||
	       strstr(label, "drm");
}

/*
 * Print a device tree property as hex.  The effective board tree is what the
 * kernel actually parsed, which is the only way to check things like the DSI
 * node's "interrupts" cells on the device (a wrong GIC SPI there would explain
 * an interrupt that is registered but never fires).
 */
static void print_dtprop(const char *path)
{
	unsigned char buf[64];
	int fd = open(path, O_RDONLY);
	ssize_t n;
	int i;

	if (fd < 0) {
		printf("dtprop: %s: %s\n", path, strerror(errno));
		return;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 0) {
		printf("dtprop: %s: %s\n", path, strerror(errno));
		return;
	}
	printf("dtprop: %s (%zd bytes):", path, n);
	for (i = 0; i < n; i++)
		printf(" %02x", buf[i]);
	printf("\n");
}

/*
 * Task states and wait channels.
 *
 * When a thread hangs, "which thread and where" beats any amount of
 * reasoning: /proc/<pid>/wchan names the kernel function the task is sleeping
 * in ("printk", "schedule_timeout", "wait_for_completion", ...), and the state
 * letter separates "sleeping normally" (S) from "stuck in the kernel" (D) or
 * "running/spinning" (R).  CONFIG_STACKTRACE is not enabled, so wchan is the
 * best available answer.
 */
static void cmd_tasks(void)
{
	DIR *dir = opendir("/proc");
	struct dirent *de;
	int shown = 0;

	if (!dir) {
		printf("tasks: %s\n", strerror(errno));
		return;
	}

	printf("%-7s %-4s %-18s %s\n", "PID", "ST", "COMM", "WCHAN");
	while ((de = readdir(dir)) != NULL) {
		char path[64], comm[64] = "", stat[512], wchan[128] = "";
		char *close_paren, *p;
		int fd, n;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;

		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, comm, sizeof(comm) - 1);
			close(fd);
			if (n > 0) {
				comm[n] = '\0';
				p = strchr(comm, '\n');
				if (p)
					*p = '\0';
			}
		}

		snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, stat, sizeof(stat) - 1);
		close(fd);
		if (n <= 0)
			continue;
		stat[n] = '\0';
		close_paren = strrchr(stat, ')');
		if (!close_paren || close_paren[1] != ' ')
			continue;

		snprintf(path, sizeof(path), "/proc/%s/wchan", de->d_name);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, wchan, sizeof(wchan) - 1);
			close(fd);
			if (n > 0) {
				wchan[n] = '\0';
				p = strchr(wchan, '\n');
				if (p)
					*p = '\0';
			}
		}

		printf("%-7s %-4c %-18s %s\n", de->d_name, close_paren[2],
		       comm[0] ? comm : "?", wchan[0] ? wchan : "-");
		shown++;
	}
	closedir(dir);
	printf("tasks: %d\n", shown);
}

static void irq_sample(void)
{
	static long prev[1024];
	static long prev_delta[1024];
	static char buf[16384];
	static int round_seen;
	static unsigned int round;
	char disp[512] = "";
	char storm[128] = "";
	char *line, *save = NULL;
	int disp_move = 0, heartbeat;
	ssize_t r;
	int fd, n = 0;

	fd = open("/proc/interrupts", O_RDONLY);
	if (fd < 0)
		return;
	while ((r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0) {
		n += (int)r;
		if ((size_t)n >= sizeof(buf) - 1)
			break;
	}
	close(fd);
	buf[n] = '\0';
	round++;
	heartbeat = (round % 15) == 0;

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *p = line, *last;
		long irq, total = 0, delta = 0;

		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			continue;	/* header, IPI lines */
		irq = strtol(p, &p, 10);
		if (irq < 0 || irq >= (long)(sizeof(prev) / sizeof(prev[0])))
			continue;
		if (*p == ':')
			p++;		/* counts start after "IRQ:" */

		for (;;) {
			char *end;

			while (*p == ' ' || *p == '\t')
				p++;
			if (*p < '0' || *p > '9')
				break;
			total += strtol(p, &end, 10);
			if (end == p)
				break;
			p = end;
		}

		last = strrchr(line, ' ');
		if (!last)
			continue;
		last++;

		if (prev[irq] && total > prev[irq])
			delta = total - prev[irq];
		prev[irq] = total;

		/*
		 * First sample: record *every* line once.  Filtering by name
		 * already fooled us twice (the DSI and mutex lines never
		 * showed up even though mtk_dsi_probe() cannot succeed without
		 * requesting its interrupt), so keep the raw truth in the
		 * persistent log.
		 */
		if (!round_seen)
			kmsg_watch("irqwatch: name=%s irq=%ld count=%ld\n",
				   last, irq, total);
		if (!round_seen && irq_is_display(last))
			kmsg_watch("irqwatch: registered: %s total=%ld\n",
				   last, total);

		if (delta > 2000 && !storm[0])
			snprintf(storm, sizeof(storm), "%s +%ld", last, delta);

		if (irq_is_display(last)) {
			long was = prev_delta[irq];

			prev_delta[irq] = delta;

			/*
			 * Print only when the rate actually moves.
			 *
			 * A display interrupt fires hundreds of times per round
			 * for ever, so the old "delta > 20" test printed every
			 * single round - six lines every two seconds, which is
			 * what buried the one thing worth seeing: the step when
			 * the panel changes rate.  Ten percent sits well inside
			 * the 60 to 120 Hz jump and well outside the jitter.
			 */
			if (delta != was &&
			    (delta == 0 || was == 0 ||
			     delta * 10 < was * 9 || delta * 10 > was * 11)) {
				/*
				 * An interrupt that never fires says nothing, and
				 * neither does one whose rate is steady.  The first
				 * version of this tested "!was || out of band",
				 * which an always-zero delta satisfies every round
				 * because was never leaves zero - seven lines every
				 * two seconds, forever.  Testing delta != was first
				 * is what makes idle interrupts silent, while still
				 * reporting one that starts or stops.
				 */
				size_t used = strlen(disp);

				disp_move = 1;
				snprintf(disp + used, sizeof(disp) - used,
					 "%s=%ld ", last, total);
				kmsg_watch("irqwatch: #%u disp name=%s irq=%ld count=%ld delta=%ld\n",
					   round, last, irq, total, delta);
			}
		}
	}
	round_seen = 1;

	if (storm[0])
		kmsg_watch("irqwatch: #%u STORM %s (per ~2 s)\n", round, storm);
	if (disp_move)
		kmsg_watch("irqwatch: #%u display moving: %s\n", round, disp);
	else if (heartbeat)
		/*
		 * One short line per display interrupt with the name first.
		 * A single long line here got truncated by the console and left
		 * us arguing about whether the DSI line exists at all.
		 */
		kmsg_watch("irqwatch: #%u alive%s\n", round,
			   disp[0] ? "" : " (no display irq registered)");
}

static void shell_loop(void)
{
	static char line[SH_LINE_MAX];

	sh_puts("\nchopin initramfs shell - type 'help'\n");
	for (;;) {
		sh_prompt();
		if (sh_getline(line, sizeof(line)) < 0) {
			sh_wait_for_console();
			continue;
		}
		sh_run(line);
		fflush(stdout);
	}
}

/*
 * display_bringup - move the panel off the rate the bootloader left it at.
 *
 * The kernel adopts LK's display state, so at this point the DSI is running at
 * whatever rate LK programmed and DRM has no idea - it already reports this
 * board's preferred mode, 120 Hz, because that is what the panel driver lists
 * first.  Asking for 120 Hz is therefore a no-op and changes nothing on the
 * wire.
 *
 * A change only happens when the requested mode differs from the one DRM
 * thinks is active, so switch to 60 Hz first and then to the preferred rate.
 * Each step is a real mode change: DRM takes the encoder through disable and
 * enable, mtk_output_dsi_enable() runs its full path and programmes the DSI
 * timings from the new mode, and the panel re-runs its init with dynamic_fps
 * already set for that rate.
 *
 * Doing this at boot is what makes 120 Hz work for anything that comes later,
 * including a desktop environment: when it sets the preferred mode as its
 * initial modeset, that is a no-op too, but the hardware is already correct.
 */

/*
 * Run the bring-up hooks the Alpine image ships in /etc/local.d/.
 *
 * Those scripts are OpenRC's "local" service, and this initramfs runs this PID 1
 * instead of OpenRC, so without this nothing executes them.  That is not
 * cosmetic: the SCP declares "mediatek,no-auto-start" in the DT, so its firmware
 * is only ever released by the patchscp/fullreset/sram/start sequence in
 * 10-chsensor.start.  Skip it and every SCP-backed node is dead - the sensor
 * FIFO never leaves wp=0, the SCP logger never fills, and
 * /sys/kernel/debug/scp-chopin/* just returns "SCP not ready".
 *
 * They sleep for tens of seconds between steps, so run them in a child and let
 * the console come up meanwhile.
 */
static void run_local_d(void)
{
	if (fork() != 0)
		return;

	execl("/bin/sh", "sh", "-c",
	      "for f in /etc/local.d/*.start; do "
	      "[ -e \"$f\" ] || continue; "
	      "/bin/sh \"$f\"; "
	      "done",
	      (char *)NULL);
	_exit(127);
}

static void display_bringup(void)
{
	char *away[] = { "setmode", "fps", "60", NULL };
	char *back[] = { "setmode", "fps", "120", NULL };

	int i;

	/*
	 * DRM registers card0 about ten seconds after init starts - measured:
	 * init reaches this point at t=2.3 s, the DRM device appears at
	 * t=11.9 s, because the display components defer their probes.  So
	 * this waits for the node rather than assuming it, and the caller runs
	 * it in a child so the console is not held up for those ten seconds.
	 */
	for (i = 0; i < 300; i++) {
		if (access("/dev/dri/card0", F_OK) == 0)
			break;
		usleep(100 * 1000);
	}
	if (i == 300) {
		kmsg("init: display bring-up: /dev/dri/card0 never appeared\n");
		return;
	}
	/* and a moment for the first modeset to land */
	usleep(500 * 1000);

	kmsg("init: display bring-up - LK left the panel at its own rate\n");
	cmd_setmode(3, away);
	cmd_setmode(3, back);
	kmsg("init: display bring-up done\n");
}

#ifndef CHOPIN_ROOTFS_DEV
#define CHOPIN_ROOTFS_DEV ""
#endif

/*
 * Power control.  As PID 1 we have to do this ourselves: busybox reboot/
 * poweroff signals PID 1 and waits for it to act, but an unhandled signal
 * is discarded for PID 1, so without these handlers "reboot" just hangs.
 */
static volatile sig_atomic_t g_power_sig;

static void power_signal(int sig)
{
	g_power_sig = sig;
}

static void install_power_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = power_signal;
	sigaction(SIGTERM, &sa, NULL);	/* reboot */
	sigaction(SIGINT, &sa, NULL);	/* ctrl-alt-del */
	sigaction(SIGUSR1, &sa, NULL);	/* halt/poweroff */
	sigaction(SIGUSR2, &sa, NULL);	/* poweroff */
}

static void do_power(void)
{
	int sig = g_power_sig;

	printf("\ninit: signal %d, %s\n", sig,
	       (sig == SIGUSR1 || sig == SIGUSR2) ? "power off" : "reboot");
	fflush(stdout);
	sync();
	if (sig == SIGUSR1 || sig == SIGUSR2)
		reboot(RB_POWER_OFF);
	else
		reboot(RB_AUTOBOOT);
	printf("init: reboot(2) failed: %s\n", strerror(errno));
	fflush(stdout);
}

/* Name of the input event device whose /sys name is "want", as /dev/input/... */
static int find_input_event(const char *want, char *out, size_t outlen)
{
	int i;

	for (i = 0; i < 32; i++) {
		int pass;

		for (pass = 0; pass < 2; pass++) {
			char path[128], name[128], ev[32];
			FILE *f;
			int fd;
			ssize_t n;

			snprintf(path, sizeof(path),
				 "/sys/class/input/event%d/device/name", i);
			f = fopen(path, "r");
			if (!f)
				continue;
			if (!fgets(name, sizeof(name), f)) {
				fclose(f);
				continue;
			}
			fclose(f);
			name[strcspn(name, "\n")] = 0;
			if (strcmp(name, want))
				continue;
			snprintf(out, outlen, "/dev/input/event%d", i);
			fd = open(out, O_RDONLY | O_NONBLOCK);
			if (fd < 0)
				continue;
			close(fd);
			return 0;
		}
		if (i == 19) {
			/* the keypad may still be probing */
			usleep(50000);
		}
	}
	return -1;
}

/* Direct view of the keypad memories through the driver's sysfs file, so
 * the recovery trigger does not depend on the input core having delivered
 * (or remembered) an event for a key that was held before init ran.
 * Word 0 bit 0 is KEY_VOLUMEUP, bit 1 KEY_VOLUMEDOWN (active low).
 */
static int keypad_hw_down(int bit)
{
	FILE *f = fopen("/sys/devices/platform/10010000.kp/keys", "r");
	unsigned int w0, w1, w2, w3, w4;

	if (!f)
		return 0;
	if (fscanf(f, "%x %x %x %x %x", &w0, &w1, &w2, &w3, &w4) != 5) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return !(w0 & (1U << bit));
}

/* Is a key currently down, or pressed within the next "ms" milliseconds? */
static int key_held(int code, int ms)
{
	char path[64];
	unsigned long keys[KEY_CNT / (8 * sizeof(unsigned long))];
	struct input_event ev;
	int fd, waited = 0;

	if (keypad_hw_down(code == KEY_VOLUMEUP ? 0 : 1))
		return 1;

	if (find_input_event("mtk-kpd", path, sizeof(path)))
		return 0;

	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return 0;

	memset(keys, 0, sizeof(keys));
	if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) == 0) {
		if (keys[code / (8 * sizeof(unsigned long))] &
		    (1UL << (code % (8 * sizeof(unsigned long))))) {
			close(fd);
			return 1;
		}
	}

	while (waited < ms) {
		ssize_t n = read(fd, &ev, sizeof(ev));

		if (n == sizeof(ev) && ev.type == EV_KEY && ev.code == code &&
		    ev.value) {
			close(fd);
			return 1;
		}
		usleep(50000);
		waited += 50;
	}

	close(fd);
	return 0;
}

/* Is "needle" present on the kernel command line? */
static int cmdline_has(const char *needle)
{
	char buf[1024];
	FILE *f = fopen("/proc/cmdline", "r");
	int found = 0;

	if (!f)
		return 0;
	if (fgets(buf, sizeof(buf), f))
		found = strstr(buf, needle) != NULL;
	fclose(f);
	return found;
}

/* The framebuffer console would keep overwriting the LVGL surface. */
static void disable_fbcon(void)
{
	int i;

	for (i = 0; i < 8; i++) {
		char path[64];
		int fd;

		snprintf(path, sizeof(path),
			 "/sys/class/vtconsole/vtcon%d/bind", i);
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			if (write(fd, "0", 1) > 0)
				printf("init: unbound %s\n", path);
			close(fd);
		}
	}
}

/* Recovery mode: a dedicated boot image (chopin.recovery=1) or a marker in
 * the cpio starts the LVGL UI instead of the shell. */
static void start_recovery(void)
{
	disable_fbcon();
	mkdir_p("/mnt");
	mkdir_p("/tmp");
	printf("init: starting recovery UI\n");
	fflush(stdout);
	execl("/usr/bin/recovery", "recovery", NULL);
	printf("init: exec /usr/bin/recovery failed: %s\n", strerror(errno));
	fflush(stdout);
}

/* Wait up to ms milliseconds for a device node. */
static void wait_dev(const char *path, int ms)
{
	int i;

	for (i = 0; i < ms / 100; i++) {
		if (access(path, F_OK) == 0)
			return;
		usleep(100000);
	}
}

/*
 * Where the real root filesystem lives, if any.  Two ways to say it, in
 * order: the kernel command line ("chopin.root=/dev/sdc86") and a one-line
 * /etc/chopin/rootfs baked into the cpio.  Without either, the initramfs
 * itself is the root filesystem and we hand over to its shell.
 */
static const char *rootfs_dev(char *buf, size_t len)
{
	FILE *f;
	char *p, *q;
	size_t n;

	f = fopen("/proc/cmdline", "r");
	if (f) {
		if (fgets(buf, len, f)) {
			p = strstr(buf, "chopin.root=");
			if (p) {
				p += strlen("chopin.root=");
				q = p;
				n = 0;
				while (*q && *q != ' ' && *q != '\n' && n + 1 < len)
					buf[n++] = *q++;
				buf[n] = 0;
				fclose(f);
				if (buf[0])
					goto resolve;
			}
		}
		fclose(f);
	}

	f = fopen("/etc/chopin/rootfs", "r");
	if (f) {
		if (fgets(buf, len, f)) {
			n = strlen(buf);
			while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
				     buf[n - 1] == ' '))
				buf[--n] = 0;
			fclose(f);
			if (buf[0])
				goto resolve;
		}
		fclose(f);
	}

	return NULL;

resolve:
	/* Accept a partition name ("userdata") as well as a device path; the
	 * number differs between chopin, xaga and friends. */
	if (buf[0] && buf[0] != '/') {
		char dev[128];

		if (find_part(buf, dev, sizeof(dev)) == 0) {
			snprintf(buf, len, "%s", dev);
		} else {
			printf("init: partition '%s' not found\n", buf);
			buf[0] = 0;
		}
	}
	return buf[0] ? buf : NULL;
}

/* Firmware that the drivers request after the pivot, so it has to exist on
 * the real rootfs and not only in the initramfs tmpfs.
 */
static void mirror_firmware(const char *root)
{
	static const char *const files[] = {
		"WIFI_RAM_CODE_soc3_0_1a_1.bin",
		"soc3_0_patch_wmmcu_1a_1_hdr.bin",
		"soc3_0_ram_wifi_1a_1_hdr.bin",
		"soc3_0_ram_wmmcu_1a_1_hdr.bin",
		"soc3_0_ram_mcu_1a_1_hdr.bin",
		"wifi.cfg",
		"wifi_sigma.cfg",
		"txpowerctrl.cfg",
		"conninfra.cfg",
		"soc3_0_ram_bt_1a_1_hdr.bin",
		"soc3_0_ram_bt_1_1_hdr.bin",
		"BT_FW.cfg",
		"bt.cfg",
	};
	char dir[256], src[256], dst[256];
	int i;

	snprintf(dir, sizeof(dir), "%s/lib/firmware/mediatek/mt6893", root);
	mkdir_p(dir);
	snprintf(dir, sizeof(dir), "%s/lib/firmware", root);
	mkdir_p(dir);

	for (i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
		snprintf(src, sizeof(src), "/lib/firmware/%s", files[i]);
		snprintf(dst, sizeof(dst), "%s/lib/firmware/%s", root, files[i]);
		copy_one(src, dst);
	}

	snprintf(dst, sizeof(dst), "%s/lib/firmware/mediatek/mt6893/WIFI",
		 root);
	copy_one("/lib/firmware/mediatek/mt6893/WIFI", dst);

	snprintf(dst, sizeof(dst), "%s/lib/firmware/mediatek/mt6893/BT_Addr",
		 root);
	copy_one("/lib/firmware/mediatek/mt6893/BT_Addr", dst);
}

/*
 * Tidy up files that earlier bring-up steps left on the rootfs: the BT
 * driver moved its address file from mediatek/mt6895 to mediatek/mt6893,
 * and the MT7922/MT7961 "rom patch" copies were only a guess at the
 * firmware name (the connac path really loads soc3_0_ram_bt_*).
 */
static void cleanup_firmware(const char *root)
{
	char src[256], dst[256];
	static const char *const stale[] = {
		"BT_RAM_CODE_MT7922_1a_1_hdr.bin",
		"BT_RAM_CODE_MT7961_1a_1_hdr.bin",
	};
	int i;

	snprintf(dst, sizeof(dst), "%s/lib/firmware/mediatek/mt6893/BT_Addr",
		 root);
	if (access(dst, F_OK)) {
		snprintf(src, sizeof(src),
			 "%s/lib/firmware/mediatek/mt6895/BT_Addr", root);
		if (!access(src, F_OK)) {
			printf("init: migrate %s -> %s\n", src, dst);
			copy_one(src, dst);
		}
	}

	for (i = 0; i < (int)(sizeof(stale) / sizeof(stale[0])); i++) {
		snprintf(dst, sizeof(dst), "%s/lib/firmware/%s", root,
			 stale[i]);
		if (!access(dst, F_OK) && !unlink(dst))
			printf("init: removed stale %s\n", stale[i]);
	}

	snprintf(dst, sizeof(dst), "%s/lib/firmware/mediatek/mt6895", root);
	rmdir(dst);
}

/*
 * Mount a real root filesystem from a partition, mirror the firmware onto it
 * and pivot into it.  This is the mt6895 model: the distro lives on userdata
 * (ext4) and the initramfs only carries the board glue.
 */
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

static void copy_file(const char *src, const char *dst)
{
	char b[4096];
	int in, out;
	ssize_t n;

	in = open(src, O_RDONLY);
	if (in < 0)
		return;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (out < 0) {
		close(in);
		return;
	}
	while ((n = read(in, b, sizeof(b))) > 0)
		write(out, b, n);
	close(in);
	close(out);
}

static int boot_rootfs(const char *dev)
{
	char *argv[2];
	char *envp[4];
	const char *init = "/sbin/init";
	int i;

	wait_dev(dev, 5000);
	mkdir_p("/newroot");

	/* Probe read-only first: if this is not a distro rootfs (e.g. Android
	 * userdata), a read-write mount with journal replay could damage it. */
	if (mount(dev, "/newroot", "ext4", MS_RDONLY, NULL) &&
	    mount(dev, "/newroot", "f2fs", MS_RDONLY, NULL)) {
		printf("init: mount %s: %s\n", dev, strerror(errno));
		return -1;
	}

	if (access("/newroot/sbin/init", X_OK) &&
	    access("/newroot/init", X_OK)) {
		printf("init: %s has no /sbin/init or /init, not a rootfs\n",
		       dev);
		umount("/newroot");
		return -1;
	}
	if (access("/newroot/sbin/init", X_OK) != 0)
		init = "/init";

	if (mount(dev, "/newroot", NULL, MS_REMOUNT, NULL)) {
		printf("init: remount %s rw: %s\n", dev, strerror(errno));
		umount("/newroot");
		return -1;
	}

	printf("init: rootfs %s, mirroring firmware\n", dev);
	mirror_firmware("/newroot");
	cleanup_firmware("/newroot");

	/* Optional kernel modules carried in the initramfs (GPU testing). */
	printf("init: copying modules\n");
	fflush(stdout);
	copy_tree("/lib/modules", "/newroot/lib/modules");
	printf("init: modules copied\n");
	fflush(stdout);

	/* Keep the ramdisk bring-up helpers reachable after switch_root. */
	mkdir_p("/newroot/usr/local/bin");
	copy_file("/usr/bin/devmem", "/newroot/usr/local/bin/devmem");

	mkdir_p("/newroot/proc");
	mkdir_p("/newroot/sys");
	mkdir_p("/newroot/dev");
	mkdir_p("/newroot/dev/pts");
	mkdir_p("/newroot/dev/shm");
	mkdir_p("/newroot/run");
	mkdir_p("/newroot/tmp");
	if (mount("proc", "/newroot/proc", "proc",
		  MS_NOSUID | MS_NOEXEC, NULL))
		printf("init: mount new /proc: %s\n", strerror(errno));
	if (mount("sysfs", "/newroot/sys", "sysfs",
		  MS_NOSUID | MS_NOEXEC, NULL))
		printf("init: mount new /sys: %s\n", strerror(errno));
	if (mount("devtmpfs", "/newroot/dev", "devtmpfs", MS_NOSUID,
		  "mode=0755"))
		printf("init: mount new /dev: %s\n", strerror(errno));
	if (mount("tmpfs", "/newroot/run", "tmpfs", MS_NOSUID | MS_NOEXEC,
		  NULL))
		printf("init: mount new /run: %s\n", strerror(errno));
	if (mount("tmpfs", "/newroot/tmp", "tmpfs", MS_NOSUID | MS_NOEXEC,
		  NULL))
		printf("init: mount new /tmp: %s\n", strerror(errno));
	if (mount("devpts", "/newroot/dev/pts", "devpts",
		  MS_NOSUID | MS_NOEXEC, NULL))
		printf("init: mount new /dev/pts: %s\n", strerror(errno));
	if (mount("tmpfs", "/newroot/dev/shm", "tmpfs",
		  MS_NOSUID | MS_NOEXEC, NULL))
		printf("init: mount new /dev/shm: %s\n", strerror(errno));

	chdir("/newroot");
	if (mount(".", "/", NULL, MS_MOVE, NULL)) {
		printf("init: move /newroot to /: %s\n", strerror(errno));
		return -1;
	}
	chroot(".");
	chdir("/");

	printf("init: switch_root -> %s\n", init);
	fflush(stdout);

	argv[0] = (char *)init;
	argv[1] = NULL;
	envp[0] = "HOME=/";
	envp[1] = "TERM=linux";
	envp[2] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	envp[3] = NULL;
	execve(init, argv, envp);

	printf("init: exec %s failed: %s\n", init, strerror(errno));
	for (i = 0; i < 3; i++)
		;
	return -1;
}

/* Fetch the vendor WiFi blobs automatically: the driver's pre-cal only
 * waits 120s for NVRAM and then gives up, and with Alpine booting straight
 * to a shell there is no guarantee anyone types "fwload" in time.
 */
static void auto_fwload(void)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (access("/sys/class/block/sdc61", F_OK) == 0)
			break;
		usleep(100000);
	}
	mkdir_p("/lib/firmware/mediatek/mt6893");
	cmd_fwload(1, (char *[]){ "fwload", NULL });
}

int main(void)
{
	char rootbuf[1024];
	const char *rootdev = NULL;
	int volup_at_boot = 0;

	redirect_to_kmsg();
	usb_open_console();
	install_power_handlers();

	kmsg_str("\n=============================================\n"
		 " CHOPIN MAINLINE: userspace reached (PID 1)\n"
		 "=============================================\n");
	fflush(stdout);

	kmsg("init: kernel    : ");
	print_file("/proc/version");
	kmsg("init: cmdline   : ");
	print_file("/proc/cmdline");
	fflush(stdout);

	kmsg("init: step 1: mounting pseudo filesystems\n");
	mount_pseudo_fs();
	kmsg("init: step 2: pseudo filesystems mounted\n");

	/* Latch the volume-up state now: the display (and therefore the
	 * recovery UI) only appears ~12s in, long after a user has let go of
	 * the key, and the keypad driver has already probed by this point. */
	volup_at_boot = keypad_hw_down(0);
	if (volup_at_boot)
		printf("init: volume-up latched at boot\n");

	rootdev = rootfs_dev(rootbuf, sizeof(rootbuf));
	printf("init: rootfs device: %s\n",
	       rootdev ? rootdev : "(none, initramfs is root)");
	fflush(stdout);

	/* The wlan driver requests its NVRAM blob a couple of seconds after
	 * boot and the conninfra pre-calibration waits only ~30s for that
	 * request to finish.  The diagnostics below can take tens of
	 * seconds (they dump the whole kernel log), so the vendor firmware
	 * has to be in the initramfs first - otherwise the NVRAM load lands
	 * after the pre-cal has already given up and the connectivity chip
	 * is left wedged. */
	if (!rootdev && fork() == 0) {
		auto_fwload();
		_exit(0);
	}
	if (rootdev)
		auto_fwload();

	print_device_tree();
	kmsg("init: step 3: device tree snapshot done\n");
	fflush(stdout);

	print_diagnostics();
	kmsg("init: step 4: diagnostics done\n");

	kmsg("init: step 5: framebuffer state\n");
	dump_fb_state();
	kmsg("init: step 6: kernel log recovery\n");
	dump_kernel_log();
	dump_usb_diag();
	kmsg("init: step 7: all diagnostics done\n");

	/*
	 * Release the SCP (and run any other board bring-up hook) before the
	 * console is handed over: it takes tens of seconds, and everything the
	 * sensor work looks at needs it.
	 */
	if (!rootdev) {
		kmsg("init: running /etc/local.d/*.start\n");
		run_local_d();
	}

	/*
	 * Hand the display bring-up to a child: it has to wait for card0, and
	 * the console must not wait with it.
	 */
	/* In rootfs mode systemd/GDM owns the display: the bring-up child
	 * leaves the DSI panel powered off after its modeset and the greeter
	 * then renders into a blank panel. */
	if (!rootdev && fork() == 0) {
		display_bringup();
		_exit(0);
	}

	if (usb_fd >= 0)
		printf("init: usb console is up on /dev/ttyGS0\n");
	else
		printf("init: no /dev/ttyGS0 (usb gadget did not bind)\n");
	fflush(stdout);

	/*
	 * Interactive shell when the gadget is up: from here on the board is
	 * debuggable without reflashing.  Without it, fall back to the old
	 * "stay alive and re-dump the log periodically" behaviour so a terminal
	 * attached late still gets the boot log.
	 */
	/*
	 * The USB console is optional from here on.  Recovery and the real
	 * rootfs must not depend on a host being plugged in: the previous
	 * order put both inside "if (sh_fd >= 0)", so a headless boot sat in
	 * the initramfs forever.
	 */
	usb_start_watcher();

	if (cmdline_has("chopin.recovery=1") ||
	    access("/etc/chopin/recovery", F_OK) == 0 ||
	    volup_at_boot || key_held(KEY_VOLUMEUP, 2500)) {
		printf("init: recovery requested (cmdline/marker/volume-up)\n");
		start_recovery();
		printf("init: recovery UI unavailable, falling back\n");
	}

	if (rootdev) {
		printf("init: booting rootfs %s\n", rootdev);
		fflush(stdout);
		if (boot_rootfs(rootdev) == 0)
			return 0;
		printf("init: rootfs boot failed, staying in initramfs\n");
		fflush(stdout);
	}

	sh_fd = open("/dev/ttyGS0", O_RDWR | O_NONBLOCK);
	if (sh_fd >= 0) {
		/*
		 * The shell owns the console from here: point stdin/stdout/stderr
		 * at it.  usb_only only guards kmsg_tx(), so plain printf() from
		 * the directory walker (and anywhere else) still went to fd 1 -
		 * which redirect_to_kmsg() had made /dev/kmsg.  Walking a sysfs
		 * tree therefore wrote one kernel log line per entry, pushing the
		 * boot log out of the ring buffer (the black box!) and costing a
		 * full printk per line.  Redirecting the descriptors fixes both,
		 * and usb_only is still set around commands so kmsg_tx() does not
		 * write each line twice.
		 */
		dup2(sh_fd, 0);
		dup2(sh_fd, 1);
		dup2(sh_fd, 2);
		set_charge_current();
	{
		/* belt and braces: whatever the role switch defaulted to, start
		 * as a device so the console exists */
		char role[192];

		if (!usb_role_path(role, sizeof(role))) {
			usb_role_write(role, "device");
			printf("  usb_role forced to device\n");
		}
	}

	/*
	 * If the rootfs carries a real shell (Alpine), run it on the console
	 * just wired up above.  It must not be exec'd early: before the dup2
	 * below, fds 0..2 still point at /dev/kmsg, and a shell reading that
	 * exits immediately - as PID 1 that panics the board.  Stay PID 1 and
	 * respawn it so a shell exit is not fatal either.
	 */
	if (access("/bin/sh", X_OK) == 0) {
		for (;;) {
			pid_t p = fork();

			if (p == 0) {
				int fl = fcntl(0, F_GETFL);

				if (fl >= 0)
					fcntl(0, F_SETFL, fl & ~O_NONBLOCK);
				setenv("PATH",
				       "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
				       1);
				setsid();
				ioctl(0, TIOCSCTTY, 1);
				execl("/bin/sh", "sh", NULL);
				_exit(127);
			}
			if (p < 0) {
				printf("init: fork: %s\n", strerror(errno));
				break;
			}
			while (waitpid(p, NULL, 0) < 0) {
				if (errno == EINTR && g_power_sig)
					break;
				if (errno != EINTR)
					break;
			}
			if (g_power_sig)
				do_power();
			printf("\ninit: shell exited, restarting\n");
			sleep(1);
		}
	}

	printf("init: no usable /bin/sh, using the built-in shell\n");
	shell_loop();
		return 0;
	}

	for (;;) {
		sleep(30);
		printf("init: alive\n");
		fflush(stdout);

		/*
		 * Re-dump the kernel log to the USB console only, so a terminal
		 * attached long after boot still sees it.  usb_only keeps this
		 * out of the ring/expdb path, which must not be spammed.
		 */
		if (usb_fd >= 0) {
			usb_only = 1;
			printf("\n\n=== chopin: kernel log re-dump for the usb console ===\n");
			dump_kernel_log();
			printf("\n=== usb udc state now ===\n");
			dump_udc();
			usb_only = 0;
		}
	}

	return 0;
}
