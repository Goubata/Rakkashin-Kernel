#!/system/bin/sh
# Rakkashin Kernel: keep more apps in RAM on a 16GB phone.
#   lmk.sh apply    relax lmkd and raise ActivityManager's cached-process cap
#   lmk.sh revert   restore the values saved by the first apply
#
# Stock thresholds are tuned for phones with far less RAM. With 16GB and the
# two-stage zram, apps can stay cached much longer before anything needs to
# be killed, so switching back to them doesn't cold-start.

STATE=/data/adb/rakkashin
SAVED=$STATE/lmk_saved
RP=$(command -v resetprop || echo /data/adb/ksu/bin/resetprop)

# prop  value-when-applied
PROPS="
ro.lmk.swap_free_low_percentage 5
ro.lmk.thrashing_limit 200
ro.lmk.psi_partial_stall_ms 150
ro.lmk.psi_complete_stall_ms 1000
"
CACHED=64

# log to dmesg and to a file that survives the kernel ring buffer wrapping
log() {
	echo "rakkashin-lmk: $*" > /dev/kmsg
	mkdir -p /data/adb/rakkashin
	f=/data/adb/rakkashin/rakkashin.log
	[ -f "$f" ] && [ "$(wc -c < "$f")" -gt 65536 ] && mv -f "$f" "$f.old"
	echo "$(date '+%m-%d %H:%M:%S') lmk: $*" >> "$f"
}

apply() {
	if [ ! -x "$RP" ] && ! command -v resetprop >/dev/null; then
		log "resetprop not found, lmkd left unchanged"
		return 1
	fi
	mkdir -p "$STATE"
	if [ ! -s "$SAVED" ]; then
		echo "$PROPS" | while read -r p _; do
			[ -n "$p" ] && echo "$p $(getprop "$p")"
		done > "$SAVED"
		echo "@cached $(device_config get activity_manager max_cached_processes)" >> "$SAVED"
	fi
	echo "$PROPS" | while read -r p v; do
		[ -n "$p" ] && "$RP" -n "$p" "$v"
	done
	device_config put activity_manager max_cached_processes "$CACHED"
	setprop lmkd.reinit 1
	log "applied (cached apps $CACHED)"
}

revert() {
	[ -s "$SAVED" ] || return 0
	while read -r p v; do
		case "$p" in
		@cached)
			if [ -z "$v" ] || [ "$v" = null ]; then
				device_config delete activity_manager max_cached_processes
			else
				device_config put activity_manager max_cached_processes "$v"
			fi ;;
		"") ;;
		*)
			if [ -z "$v" ]; then "$RP" -n --delete "$p"; else "$RP" -n "$p" "$v"; fi ;;
		esac
	done < "$SAVED"
	setprop lmkd.reinit 1
	log "reverted"
}

case "$1" in
apply) apply ;;
revert) revert ;;
esac
