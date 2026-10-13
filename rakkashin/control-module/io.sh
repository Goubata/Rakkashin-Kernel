#!/system/bin/sh
# Rakkashin Kernel: ADIOS I/O scheduler on the UFS LUNs.
#   io.sh apply    switch sd* queues to adios, remembering the previous one
#   io.sh revert   put the remembered schedulers back

STATE=/data/adb/rakkashin
SAVED=$STATE/io_saved

# log to dmesg and to a file that survives the kernel ring buffer wrapping
log() {
	echo "rakkashin-io: $*" > /dev/kmsg
	mkdir -p /data/adb/rakkashin
	f=/data/adb/rakkashin/rakkashin.log
	[ -f "$f" ] && [ "$(wc -c < "$f")" -gt 65536 ] && mv -f "$f" "$f.old"
	echo "$(date '+%m-%d %H:%M:%S') io: $*" >> "$f"
}

apply() {
	n=0
	mkdir -p "$STATE"
	: > "$SAVED.new"
	for q in /sys/block/sd*/queue/scheduler; do
		[ -w "$q" ] || continue
		grep -q adios "$q" || continue
		cur=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$q")
		echo "$q $cur" >> "$SAVED.new"
		err=$( { echo adios > "$q"; } 2>&1 )
		now=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$q")
		if [ "$now" = adios ]; then
			n=$((n + 1))
		else
			log "$q: still $now after setting adios ${err:+($err)}"
		fi
	done
	log "adios on $n queue(s)"
	# keep the very first record so revert returns to stock
	[ -s "$SAVED" ] || mv "$SAVED.new" "$SAVED"
	rm -f "$SAVED.new"
}

revert() {
	[ -s "$SAVED" ] || return 0
	while read -r q s; do
		[ -w "$q" ] && echo "$s" > "$q"
	done < "$SAVED"
}

case "$1" in
apply) apply ;;
revert) revert ;;
esac
