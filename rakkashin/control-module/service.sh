#!/system/bin/sh
# Rakkashin Kernel: restore saved settings after boot, then keep the cgroup
# clamps in step with kernel mode switches (per-app profiles).
MODDIR=${0%/*}
STATE=/data/adb/rakkashin
K=/sys/kernel/avr_profile

until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 2; done
[ -e "$K/mode" ] || exit 0

[ -f "$STATE/mode" ] && sh "$MODDIR/apply.sh" "$(cat "$STATE/mode")"
[ -f "$STATE/power_limit" ] && cat "$STATE/power_limit" > "$K/power_limit"
[ -f "$STATE/smart_cap" ] && cat "$STATE/smart_cap" > "$K/smart_cap"
[ -f "$STATE/app_profiles" ] && cat "$STATE/app_profiles" > "$K/app_profiles"
[ -f "$STATE/per_app" ] && cat "$STATE/per_app" > "$K/per_app"
[ -f "$STATE/fsync" ] && cat "$STATE/fsync" > /sys/module/sync/parameters/fsync_enabled
[ -f "$STATE/tboost" ] && cat "$STATE/tboost" > /sys/module/rk_tboost/parameters/enabled
[ -f "$STATE/launch_boost" ] && cat "$STATE/launch_boost" > /sys/module/launch_boost/parameters/enabled
[ -f "$STATE/wl_block" ] && cat "$STATE/wl_block" > /sys/class/misc/boeffla_wakelock_blocker/wakelock_blocker

# Keep more apps cached (default on; "0" in $STATE/lmk disables it)
[ "$(cat "$STATE/lmk" 2>/dev/null)" != 0 ] && sh "$MODDIR/lmk.sh" apply

# ADIOS I/O scheduler on UFS (default on; "0" in $STATE/adios disables it)
[ "$(cat "$STATE/adios" 2>/dev/null)" != 0 ] && sh "$MODDIR/io.sh" apply

# f2fs: let fsync of files with up to 20 dirty blocks take the fast
# roll-forward path instead of a full checkpoint (stock: 8)
for f in /sys/fs/f2fs/*/min_fsync_blocks; do [ -w "$f" ] && echo 20 > "$f"; done

# Two-stage zram (default on; "0" in $STATE/zram2 disables it next boot)
if [ "$(cat "$STATE/zram2" 2>/dev/null)" != 0 ]; then
	sh "$MODDIR/zram.sh" setup && sh "$MODDIR/zram.sh" loop &
fi

# GPU demand governor (default on; "0" in $STATE/gpu disables it). Needs a
# kernel with demand_active and the kgsl /sys/kernel/gpu interface.
if [ "$(cat "$STATE/gpu" 2>/dev/null)" != 0 ] && [ -e "$K/demand_active" ] &&
   [ -w /sys/kernel/gpu/gpu_max_clock ]; then
	chmod 755 "$MODDIR/bin/rk_gpud"
	# the saved original max only guards restarts within one boot; record
	# afresh each boot so changes to the GPU max (e.g. an underclock) count
	rm -f "$STATE/gpu_orig_max"
	"$MODDIR/bin/rk_gpud" </dev/null >/dev/null 2>&1 &
fi

# The kernel sysfs_notify()s "mode" on every switch; inotifyd blocks until
# then, so this costs nothing while idle.
active() { sed -n 's/.*\[\([a-z]*\)\].*/\1/p' "$K/mode"; }
inotifyd - "$K/mode:c" 2>/dev/null | while read -r _ev _f; do
	sh "$MODDIR/apply.sh" --cg "$(active)"
done &
exit 0
