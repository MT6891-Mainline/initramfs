# chopin boot initramfs
#
# recovery branch: the rescue image - Alpine userland, hardware diagnostics and
# a USB gadget shell.  The image that actually boots the device lives on the
# 'rootfs' branch.
#
# Both need aarch64-linux-gnu-gcc; the output lands in ./out.

CROSS_COMPILE ?= aarch64-linux-gnu-
export CROSS_COMPILE

.PHONY: recovery clean

recovery:
	INITRAMFS_MODE=bringup INIT_SRC=init.c ./tools/mkinitramfs.sh out

clean:
	rm -rf out
