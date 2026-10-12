#!/system/bin/sh
# Rakkashin Kernel: apply a profile.
#   apply.sh <mode>       set the global profile (kernel + cgroup clamps)
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

case "$MODE" in battery|balanced|performance|game) ;; *) echo "bad mode: $MODE" >&2; exit 1 ;; esac
if [ $CG_ONLY = 0 ]; then
	echo "$MODE" > "$NODE" || exit 1
	mkdir -p "$STATE" && echo "$MODE" > "$STATE/mode"
fi

setu() { [ -w "$CG/$1/cpu.uclamp.$2" ] && echo "$3" > "$CG/$1/cpu.uclamp.$2"; }

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

case "$MODE" in
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
esac
exit 0
