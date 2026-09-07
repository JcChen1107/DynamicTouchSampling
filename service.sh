#!/system/bin/sh
# service.sh - 开机启动守护进程
MODDIR=${0%/*}
RUNTIME="/data/adb/dynamic_touch_sampling"
LOG="$RUNTIME/log/touchd.log"

mkdir -p "$RUNTIME/log"

for i in $(seq 1 90); do
    [ "$(getprop sys.boot_completed)" = "1" ] && break
    sleep 1
done
sleep 2

[ ! -f "$RUNTIME/config.conf" ] && cp "$MODDIR/etc/config.conf" "$RUNTIME/config.conf"
[ ! -f "$RUNTIME/app_profiles.conf" ] && cp "$MODDIR/etc/app_profiles.conf" "$RUNTIME/app_profiles.conf"

# 确保二进制有执行权限 (打包权限丢失时的保险)
chmod 755 "$MODDIR/bin/touchd" 2>/dev/null

if [ -f "$MODDIR/bin/touchd" ]; then
    if [ -x "$MODDIR/bin/touchd" ]; then
        "$MODDIR/bin/touchd" --daemon >> "$LOG" 2>&1 &
        echo "[$(date '+%m-%d %H:%M:%S')] touchd started (pid=$!)" >> "$LOG"
    else
        echo "[$(date '+%m-%d %H:%M:%S')] ERROR: touchd exists but not executable" >> "$LOG"
    fi
else
    echo "[$(date '+%m-%d %H:%M:%S')] ERROR: touchd binary not found at $MODDIR/bin/touchd" >> "$LOG"
    echo "[$(date '+%m-%d %H:%M:%S')] MODDIR=$MODDIR" >> "$LOG"
    ls -la "$MODDIR/bin/" >> "$LOG" 2>&1
fi
