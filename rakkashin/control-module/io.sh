#!/system/bin/sh
# Rakkashin Kernel: ADIOS I/O scheduler on the UFS LUNs.
#   io.sh apply    switch sd* queues to adios, remembering the previous one
#   io.sh revert   put the remembered schedulers back

STATE=/data/adb/rakkashin
SAVED=$STATE/io_saved

apply() {
	mkdir -p "$STATE"
	: > "$SAVED.new"
	for q in /sys/block/sd*/queue/scheduler; do
		[ -w "$q" ] || continue
		grep -q adios "$q" || continue
		cur=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$q")
		echo "$q $cur" >> "$SAVED.new"
		echo adios > "$q"
	done
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
