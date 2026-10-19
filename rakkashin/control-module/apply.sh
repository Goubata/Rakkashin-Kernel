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

# core_ctl (Qualcomm WALT core control) pauses cores when a cluster is
# lightly loaded. Its stock minimums are saved once; battery lets it pause
# more of the efficiency cluster (down to 2 of 6 awake), balanced keeps the
# stock behaviour, and the fast profiles keep every core awake so a burst
# never waits for a paused core. (Vendor boosts may also write min_cpus.)
CCSAVED=$STATE/core_ctl_defaults
if [ ! -s "$CCSAVED" ]; then
	for d in /sys/devices/system/cpu/cpu*/core_ctl; do
		[ -r "$d/min_cpus" ] && echo "$d $(cat "$d/min_cpus") $(cat "$d/max_cpus")"
	done > "$CCSAVED"
fi
core_ctl() {
	while read -r d min max; do
		[ -w "$d/min_cpus" ] || continue
		case "$1" in
		low)  [ "$min" -gt 2 ] && v=2 || v=$min ;;
		all)  v=$max ;;
		*)    v=$min ;;
		esac
		echo "$v" > "$d/min_cpus"
	done < "$CCSAVED"
}
case "$MODE" in
battery) core_ctl low ;;
performance|game|extreme) core_ctl all ;;
*) core_ctl stock ;;
esac

# kgsl (GPU driver) power timers, from LawRun/AGNi: idle_timer is how long
# the GPU stays up after its last work before slumbering (stock 80 ms;
# LawRun used 58, AGNi 64), wake_timeout how long a touch keeps a woken GPU
# up waiting for a frame (stock 100 ms). Shorter saves power in bursty UI;
# the fast profiles keep the stock values. Stock values are saved once.
KG=/sys/class/kgsl/kgsl-3d0
KGSAVED=$STATE/kgsl_defaults
if [ ! -s "$KGSAVED" ]; then
	for k in idle_timer wake_timeout; do
		[ -r "$KG/$k" ] && echo "$k $(cat "$KG/$k")"
	done > "$KGSAVED"
fi
kgsl_set() {	# <idle_timer ms> <wake_timeout ms>, or "stock"
	while read -r k v; do
		case "$1:$k" in
		stock:*) ;;
		*:idle_timer) v=$1 ;;
		*:wake_timeout) v=$2 ;;
		esac
		[ -w "$KG/$k" ] && echo "$v" > "$KG/$k"
	done < "$KGSAVED"
}
case "$MODE" in
battery) kgsl_set 50 50 ;;
balanced) kgsl_set 64 80 ;;
*) kgsl_set stock ;;
esac

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
