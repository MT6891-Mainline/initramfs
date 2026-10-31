# chopin boot initramfs
#
# rootfs branch: the image the device actually boots - no userland at all, it
# mounts the distro from chopin.root= (or /etc/chopin/rootfs) and switch_roots.
# The rescue image with the hardware diagnostics lives on the 'recovery' branch.
#
# Both need aarch64-linux-gnu-gcc; the output lands in ./out.

CROSS_COMPILE ?= aarch64-linux-gnu-
export CROSS_COMPILE

.PHONY: rootfs clean

rootfs:
	INITRAMFS_MODE=rootfs INIT_SRC=initramfs/init.c ./tools/mkinitramfs.sh out

clean:
	rm -rf out
