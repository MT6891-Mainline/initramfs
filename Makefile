# chopin boot initramfs
#
# make rootfs    -> the image used on the device: no userland, mounts the distro
#                   from chopin.root= (or /etc/chopin/rootfs) and switch_roots
# make bringup   -> rescue image: Alpine userland, diagnostics, USB shell
#
# Both need aarch64-linux-gnu-gcc; the output lands in ./out.

CROSS_COMPILE ?= aarch64-linux-gnu-
export CROSS_COMPILE

.PHONY: rootfs bringup clean

rootfs:
	INITRAMFS_MODE=rootfs ./tools/mkinitramfs.sh out

bringup:
	INITRAMFS_MODE=bringup ./tools/mkinitramfs.sh out

clean:
	rm -rf out
