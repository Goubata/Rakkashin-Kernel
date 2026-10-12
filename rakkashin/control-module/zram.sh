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

# swapon, logging the error; retry without a priority if that is rejected
swap_on() {
	if [ -n "$prio" ]; then
		err=$(swapon -p "$prio" "$DEV" 2>&1) && return 0
		log "swapon -p $prio failed: $err"
	fi
	err=$(swapon "$DEV" 2>&1) && return 0
	log "swapon failed: $err"
	return 1
}

# (re)initialise zram0 with algorithm $1 and optional recompression $2
init_zram() {
	echo 1 > "$Z/reset" || { log "reset failed"; return 1; }
	echo "$1" > "$Z/comp_algorithm" || log "comp_algorithm $1 failed"
	[ -n "$2" ] && { echo "algo=$2 priority=1" > "$Z/recomp_algorithm" || log "recomp_algorithm $2 failed"; }
	[ -n "$bd" ] && [ "$bd" != none ] && echo "$bd" > "$Z/backing_dev"
	echo "$size" > "$Z/disksize" || { log "disksize failed"; return 1; }
	err=$(mkswap "$DEV" 2>&1) || { log "mkswap failed: $err"; return 1; }
	swap_on
}

setup() {
	[ -e "$Z/recomp_algorithm" ] || { log "no multi-comp support"; return 1; }
	size=$(cat "$Z/disksize")
	[ "${size:-0}" -gt 0 ] || { log "zram0 not initialised"; return 1; }
	if grep -q "\[zstd\]" "$Z/recomp_algorithm"; then
		# Rakkashin Kernel >= 2.2 creates zram with zstd recompression, so a
		# live device never needs a reset (nor races other zram tools)
		log "$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$Z/comp_algorithm") + zstd recompression, $((size / 1048576)) MB"
		return 0
	fi
	# Older kernel: rebuild. Wait until nobody else is still reconfiguring
	# zram (other modules do it right after boot) before touching it.
	stable=0
	while [ $stable -lt 3 ]; do
		s1=$(cat /proc/swaps); sleep 5
		if [ "$s1" = "$(cat /proc/swaps)" ]; then stable=$((stable + 1)); else stable=0; fi
	done
	size=$(cat "$Z/disksize")
	prio=$(awk '$1 ~ /zram0/ {print $5}' /proc/swaps)
	bd=$(cat "$Z/backing_dev" 2>/dev/null)
	orig=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$Z/comp_algorithm")

	err=$(swapoff "$DEV" 2>&1) || { log "swapoff failed: $err"; return 1; }
	if grep -qw lz4 "$Z/comp_algorithm"; then fast=lz4; else fast=lzo-rle; fi
	if init_zram "$fast" zstd; then
		log "$fast + zstd recompression, $((size / 1048576)) MB"
		return 0
	fi
	# never leave the phone without swap: rebuild it the way Android had it
	log "falling back to ${orig:-lzo-rle} without recompression"
	init_zram "${orig:-lzo-rle}" "" && log "restored ${orig:-lzo-rle} swap"
	return 1
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
