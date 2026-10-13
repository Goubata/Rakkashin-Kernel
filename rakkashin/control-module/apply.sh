#!/system/bin/sh
# Rakkashin Kernel: apply a profile.
#   apply.sh <mode>       set the global profile (kernel + cgroup clamps)
#                         modes: battery balanced performance game extreme
#   apply.sh --cg <mode>  only re-apply cgroup clamps (kernel already switched,
#                         e.g. a per-app profile kicked in)
#
# The kernel side (/sys/kernel/avr_profile) moves cpufreq floors/caps, PELT,
# launch boost and writeback. Here we add per-cgroup uclamp limits, which
# only userspace can set: they keep background apps from spiking the CPU
# (battery) or reserve capacity for the foreground app (game).

STATE=/data/adb/rakkashin
NODE=/sys/kernel/avr_profile/mode
CG=/dev/cpuctl

CG_ONLY=0
[ "$1" = "--cg" ] && { CG_ONLY=1; shift; }
MODE=$1

case "$MODE" in battery|balanced|performance|game|extreme) ;; *) echo "bad mode: $MODE" >&2; exit 1 ;; esac
if [ $CG_ONLY = 0 ]; then
	echo "$MODE" > "$NODE" || exit 1
	mkdir -p "$STATE" && echo "$MODE" > "$STATE/mode"
fi

setu() { [ -w "$CG/$1/cpu.uclamp.$2" ] && echo "$3" > "$CG/$1/cpu.uclamp.$2"; }

# Hardware knobs only extreme touches: memory/cache bus floors (bus_dcvs) and
# the GPU's lowest allowed power level (kgsl, 0 = fastest). Their stock
# values are saved on entering extreme and put back on any other mode, so
# the other profiles never see them changed.
HWSAVED=$STATE/extreme_saved
GPU=/sys/class/kgsl/kgsl-3d0/min_pwrlevel

hw_restore() {
	[ -s "$HWSAVED" ] || return 0
	while read -r f v; do [ -w "$f" ] && echo "$v" > "$f"; done < "$HWSAVED"
	rm -f "$HWSAVED"
}

hw_extreme() {
	if [ ! -s "$HWSAVED" ]; then
		for f in /sys/devices/system/cpu/bus_dcvs/*/boost_freq "$GPU"; do
			[ -w "$f" ] && echo "$f $(cat "$f")"
		done > "$HWSAVED"
	fi
	for f in /sys/devices/system/cpu/bus_dcvs/*/boost_freq; do
		d=${f%/boost_freq}
		[ -w "$f" ] && [ -r "$d/hw_max_freq" ] && cat "$d/hw_max_freq" > "$f"
	done
	[ -w "$GPU" ] && echo 0 > "$GPU"
}

# Remember Android's own cgroup clamps once, so balanced restores exactly those.
mkdir -p "$STATE"
if [ ! -s "$STATE/cg_defaults" ]; then
	for g in top-app foreground background system-background; do
		for k in min max; do
			f="$CG/$g/cpu.uclamp.$k"
			[ -r "$f" ] && echo "$g $k $(cat "$f")"
		done
	done > "$STATE/cg_defaults"
fi
while read -r g k v; do setu "$g" "$k" "$v"; done < "$STATE/cg_defaults"

if [ "$MODE" = extreme ]; then hw_extreme; else hw_restore; fi

case "$MODE" in
balanced)
	# background work runs on lower, more efficient frequencies; the
	# foreground app is untouched
	setu background max 50
	setu system-background max 60
	;;
battery)
	setu background max 20
	setu system-background max 30
	setu foreground max 70
	;;
performance)
	setu top-app min 10
	;;
game)
	setu top-app min 30
	setu background max 10
	setu system-background max 20
	;;
extreme)
	setu top-app min 60
	setu background max 5
	setu system-background max 10
	;;
esac
exit 0
