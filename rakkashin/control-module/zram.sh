#!/system/bin/sh
# Rakkashin Kernel: two-stage zram.
#   zram.sh setup   rebuild zram0 with lz4 (fast; lzo-rle on kernels without
#                   it) + zstd as recompression
#                   algorithm; run right after boot, while swap is still small
#   zram.sh loop    every PERIOD, recompress pages that stayed idle with zstd
#
# Hot pages stay in a fast algorithm so app switching is quick; pages nobody
# touched for a whole period get squeezed with zstd, so more apps fit in RAM.

Z=/sys/block/zram0
DEV=/dev/block/zram0
PERIOD=1800

log() { echo "rakkashin-zram: $*" > /dev/kmsg; }

setup() {
	[ -e "$Z/recomp_algorithm" ] || { log "no multi-comp support"; return 1; }
	size=$(cat "$Z/disksize")
	[ "${size:-0}" -gt 0 ] || { log "zram0 not initialised"; return 1; }
	grep -q "\[zstd\]" "$Z/recomp_algorithm" && return 0	# already set up
	prio=$(awk '$1 ~ /zram0/ {print $5}' /proc/swaps)
	bd=$(cat "$Z/backing_dev" 2>/dev/null)

	swapoff "$DEV" || { log "swapoff failed"; return 1; }
	echo 1 > "$Z/reset"
	if grep -qw lz4 "$Z/comp_algorithm"; then fast=lz4; else fast=lzo-rle; fi
	echo "$fast" > "$Z/comp_algorithm"
	echo "algo=zstd priority=1" > "$Z/recomp_algorithm" || log "recomp_algorithm failed"
	[ -n "$bd" ] && [ "$bd" != none ] && echo "$bd" > "$Z/backing_dev"
	echo "$size" > "$Z/disksize" || { log "disksize failed"; return 1; }
	mkswap "$DEV" >/dev/null || { log "mkswap failed"; return 1; }
	if [ -n "$prio" ]; then swapon -p "$prio" "$DEV"; else swapon "$DEV"; fi \
		|| { log "swapon failed"; return 1; }
	log "$fast + zstd recompression, $((size / 1048576)) MB"
}

loop() {
	while :; do
		echo all > "$Z/idle" 2>/dev/null
		sleep "$PERIOD"
		echo "type=idle" > "$Z/recompress" 2>/dev/null
	done
}

case "$1" in
setup) setup ;;
loop) loop ;;
esac
