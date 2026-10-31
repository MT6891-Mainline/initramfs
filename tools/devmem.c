// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal devmem(1) for the chopin initramfs.
 *
 * The Alpine busybox in the ramdisk has no devmem applet, and the mainline
 * port needs to peek at MMIO blocks (SCP registers, audio top registers,
 * ...) long before any driver exposes them.  /dev/mem is usable here
 * because CONFIG_IO_STRICT_DEVMEM is off and those windows are not RAM.
 *
 *   devmem <addr> [width] [value]
 *
 * With no value it reads; with a value it writes.  Width is 8, 16, 32 or
 * 64 bits, default 32.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long long addr, value = 0;
	unsigned int width = 32;
	int fd, write_val;
	off_t page;
	size_t off;
	void *map;
	volatile void *p;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <addr> [width] [value]\n", argv[0]);
		return 1;
	}

	addr = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		width = (unsigned int)strtoul(argv[2], NULL, 0);
	write_val = argc > 3;
	if (write_val)
		value = strtoull(argv[3], NULL, 0);

	if (width != 8 && width != 16 && width != 32 && width != 64) {
		fprintf(stderr, "devmem: width must be 8, 16, 32 or 64\n");
		return 1;
	}

	fd = open("/dev/mem", (write_val ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "devmem: open /dev/mem: %s\n", strerror(errno));
		return 1;
	}

	page = (off_t)(addr & ~(unsigned long long)0xfff);
	off = (size_t)(addr & 0xfff);
	map = mmap(NULL, 0x1000, PROT_READ | (write_val ? PROT_WRITE : 0),
		   MAP_SHARED, fd, page);
	if (map == MAP_FAILED) {
		fprintf(stderr, "devmem: mmap: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	p = (volatile char *)map + off;

	switch (width) {
	case 8:
		if (write_val)
			*(volatile uint8_t *)p = (uint8_t)value;
		else
			printf("0x%02x\n", *(volatile uint8_t *)p);
		break;
	case 16:
		if (write_val)
			*(volatile uint16_t *)p = (uint16_t)value;
		else
			printf("0x%04x\n", *(volatile uint16_t *)p);
		break;
	case 32:
		if (write_val)
			*(volatile uint32_t *)p = (uint32_t)value;
		else
			printf("0x%08x\n", *(volatile uint32_t *)p);
		break;
	case 64:
		if (write_val)
			*(volatile uint64_t *)p = value;
		else
			printf("0x%016llx\n", (unsigned long long)*(volatile uint64_t *)p);
		break;
	}

	munmap(map, 0x1000);
	close(fd);
	return 0;
}
