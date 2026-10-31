#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# chopin: board-side SCP bring-up diagnostic.
#
# Everything goes through /sys/kernel/debug/scp-chopin (the helper driver in
# drivers/misc/mediatek/scp-chopin.c), so no /dev/mem access is needed.
#
#   sh /board-scp-audit.sh            read-only report
#   sh /board-scp-audit.sh start      start the firmware, then report again
#   sh /board-scp-audit.sh start2     same, but release core1 as well
#
# The interesting numbers, in the order they appear:
#   resv        DRAM_RESV_ADDR/SIZE - must be the SCP *share* window
#               (0x8f000000/0x500000), never 0x9fa00000 (that is the image)
#   share       the window the helper resolved, and where it got it from
#   tcm[07d5e0] firmware log mutex; 0 = free, ffffffff = held
#   pc0/pc1     firmware PCs; 0x3ef96/0x3ef9a == spinning on that mutex

D=/sys/kernel/debug/scp-chopin

if [ ! -d "$D" ]; then
	echo "no $D - is CONFIG_MTK_SCP_CHOPIN=y in the kernel?" >&2
	exit 1
fi

say() {
	echo
	echo "=== $* ==="
}

field() {	# field <file> <key>: everything after the key on that line
	awk -v key="$2" '$1 == key { for (i = 2; i <= NF; i++)
		printf "%s%s", $i, (i < NF ? " " : "\n") }' "$1"
}

summary() {
	s="$D/status"
	a="$D/audit"
	printf 'pc0        %s\n' "$(field "$s" pc0)"
	printf 'pc1        %s\n' "$(field "$s" pc1)"
	printf 'status0    %s\n' "$(field "$s" status0)"
	printf 'status1    %s\n' "$(field "$s" status1)"
	printf 'resv       %s\n' "$(field "$s" resv)"
	printf 'share      %s\n' "$(field "$s" share)"
	printf 'clk        %s\n' "$(field "$s" clk)"
	printf 'lock       %s\n' "$(awk '$1 == "tcm[07d5e0]" { print $2 }' "$a")"
	printf 'mbox0 recv %s\n' "$(awk '$1 == "tcm[07d600]" { print $2 }' "$a")"
	printf 'mbox1 recv %s\n' "$(awk '$1 == "tcm[07d700]" { print $2 }' "$a")"
}

say "helper boot messages"
dmesg | grep -i "scp-chopin" | tail -n 20

say "status"
cat "$D/status"

if [ "$1" = "start" ] || [ "$1" = "start2" ]; then
	if [ "$1" = "start2" ]; then
		echo 1 >"$D/core1"
	fi
	echo 1 >"$D/start"
	sleep 2
	say "helper messages after start"
	dmesg | grep -i "scp-chopin" | tail -n 25
fi

say "audit"
cat "$D/audit"

say "summary"
summary
