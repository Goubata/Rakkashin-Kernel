#!/system/bin/sh
# Rakkashin Kernel: (re)start or stop rk_gpud, the GPU demand governor
# ($STATE/gpu) and frame timing for the foreground app ($STATE/fas); "0"
# in either file turns that part off, rk_gpud exits when both are.
# usage: gpud.sh [restart|stop]
MODDIR=${0%/*}
STATE=/data/adb/rakkashin
K=/sys/kernel/avr_profile

# SIGTERM makes rk_gpud restore the GPU max clock and drop its listener.
# (This script's own command line doesn't contain the binary path, so
# pkill can't hit the shell running it.)
pkill -f "$MODDIR/bin/rk_gpud" && sleep 0.3
# the saved original max only guards restarts while capped; record afresh
# so changes to the GPU max (e.g. an underclock) count
rm -f "$STATE/gpu_orig_max" "$STATE/fas_state"
[ "$1" = stop ] && exit 0

[ -e "$K/demand_active" ] || exit 0
[ "$(cat "$STATE/gpu" 2>/dev/null)" = 0 ] && [ "$(cat "$STATE/fas" 2>/dev/null)" = 0 ] && exit 0
chmod 755 "$MODDIR/bin/rk_gpud"
setsid "$MODDIR/bin/rk_gpud" </dev/null >/dev/null 2>&1 &
