#!/system/bin/sh
# Rakkashin Kernel diagnostics.
# Run in Termux:  sh /sdcard/RakkashinKernel/rk_diag.sh
# Use Smart Balanced and don't touch the screen while it samples (~15 s).
# "sh rk_diag.sh play" samples for 30 s instead: start it, switch to a game
# within 10 s and keep playing (checks Sustained performance while touched).
# Per-app profiles are paused while sampling (Termux itself is in front),
# then restored.
# The report goes to /sdcard/RakkashinKernel/diag.txt (and is copied to an
# Andronix Ubuntu home if there is one, for the developer's tools).

# needs root: re-run through su when started as a normal user
if [ "$(id -u)" != 0 ]; then
	exec su -c "sh '$0' $1"
fi

PLAY=0
[ "$1" = play ] && PLAY=1

OUT=/sdcard/RakkashinKernel/diag.txt
COPY=/data/data/com.termux/files/home/.andronix/distros/ubuntu/rootfs/home/alex
mkdir -p /sdcard/RakkashinKernel
K=/sys/kernel/avr_profile
G=/sys/kernel/gpu

# pause per-app profiles so the global profile is what gets measured
PERAPP=$(cat $K/per_app 2>/dev/null)
[ "$PERAPP" = 1 ] && echo 0 > $K/per_app
trap '[ "$PERAPP" = 1 ] && echo 1 > $K/per_app' EXIT

if [ $PLAY = 1 ]; then
	echo "Switch to your game now and keep playing for about 40 seconds..."
	sleep 10
else
	echo "Sampling for about 15 seconds. Don't touch the screen..."
	sleep 3
fi

{
echo "== kernel"
uname -r
for f in version status demand_active sustained sustained_temp sustained_state top_pid frame_state smart_cap power_limit; do
	echo "$f: $(cat $K/$f 2>&1)"
done
cat $K/battery_caps 2>&1

echo "== module"
grep -E '^(version|versionCode)=' /data/adb/modules/rakkashin/module.prop 2>&1
for f in /data/adb/rakkashin/*; do
	case "$f" in *.log*|*/cg_defaults|*/app_profiles|*/wl_block) continue ;; esac
	echo "$(basename "$f"): $(head -c 80 "$f" | tr '\n' ' ')"
done
echo "per_app before sampling: $PERAPP (paused while sampling)"

echo "== cgroup uclamp (min max)"
for g in top-app foreground background system-background; do
	echo "$g $(cat /dev/cpuctl/$g/cpu.uclamp.min 2>&1) $(cat /dev/cpuctl/$g/cpu.uclamp.max 2>&1)"
done

echo "== zram"
cat /sys/block/zram0/comp_algorithm /sys/block/zram0/recomp_algorithm /proc/swaps 2>&1

echo "== io scheduler"
cat /sys/block/sd*/queue/scheduler 2>&1 | sort | uniq -c

echo "== lmk"
getprop | grep -E 'ro.lmk|lmkd'
echo "max_cached_processes: $(device_config get activity_manager max_cached_processes 2>&1)"

echo "== gpu"
for f in gpu_freq_table gpu_min_clock gpu_max_clock gpu_clock gpu_busy; do
	echo "$f: $(cat $G/$f 2>&1)"
done
echo "rk_gpud: $(pgrep -f rk_gpud || echo 'not running')"
echo "encore_fas: $([ -e /dev/encore_fas ] && echo present || echo missing)"
TP=$(cat $K/top_pid 2>/dev/null)
echo "top app: $(tr '\0' ' ' < "/proc/$TP/cmdline" 2>/dev/null | cut -c1-80) (pid $TP)"
echo "libgui probed: inode $(stat -c '%i' /system/lib64/libgui.so 2>&1)"
echo "libgui in top app: $(grep -m3 'libgui' "/proc/$TP/maps" 2>/dev/null | awk '{print "inode", $5, $6}' | sort -u | tr '\n' ' ')"

echo "== core_ctl (min_cpus active_cpus need_cpus)"
for d in /sys/devices/system/cpu/cpu*/core_ctl; do
	echo "$d $(cat "$d/min_cpus" "$d/active_cpus" "$d/need_cpus" 2>/dev/null | tr '\n' ' ')"
done

echo "== cpufreq"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
	echo "$p cur=$(cat "$p/scaling_cur_freq") min=$(cat "$p/scaling_min_freq") max=$(cat "$p/scaling_max_freq") hw=$(cat "$p/cpuinfo_max_freq")"
done

if [ $PLAY = 1 ]; then N=30; WHAT="while playing"; else N=10; WHAT="untouched"; fi
echo "== $N s sampling $WHAT: cpu max per policy | gpu max clock busy | demand_active power | sustained | frames"
i=0
while [ $i -lt $N ]; do
	echo "$(cat /sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq | tr '\n' ' ')| $(cat $G/gpu_max_clock) $(cat $G/gpu_clock) $(cat $G/gpu_busy) | $(cat $K/demand_active) $(cat $K/power_limit) | $(cat $K/sustained_state 2>/dev/null) | $(cat $K/frame_state 2>/dev/null) $(cat /data/adb/rakkashin/fas_state 2>/dev/null)"
	i=$((i + 1))
	sleep 1
done

echo "== module log"
tail -40 /data/adb/rakkashin/rakkashin.log 2>&1

echo "== dmesg"
dmesg | grep -iE 'rakkashin|rk_gpud|boeffla|avr_profile|encore_fas' | tail -40
} > "$OUT" 2>&1

if [ -d "$COPY" ]; then
	cp "$OUT" "$COPY/diag.txt" && chmod 644 "$COPY/diag.txt"
fi
echo "done: $OUT"
