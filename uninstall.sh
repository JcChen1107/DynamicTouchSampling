#!/system/bin/sh
# uninstall.sh - 卸载清理
RUNTIME="/data/adb/dynamic_touch_sampling"

if [ -f "$RUNTIME/touchd.pid" ]; then
    PID=$(cat "$RUNTIME/touchd.pid")
    [ -n "$PID" ] && kill "$PID" 2>/dev/null && sleep 1 && kill -9 "$PID" 2>/dev/null
fi

for f in /odm/firmware/*_gtp_thp_config.ini; do
    [ -f "$f" ] && umount "$f" 2>/dev/null
done

rm -f "$RUNTIME/mode" "$RUNTIME/touchd.pid"
if [ -d "$RUNTIME" ]; then
    cp "$RUNTIME/config.conf" /sdcard/touchd_config_backup.conf 2>/dev/null
    cp "$RUNTIME/app_profiles.conf" /sdcard/touchd_profiles_backup.conf 2>/dev/null
    rm -rf "$RUNTIME"
fi
echo "Dynamic Touch Sampling Rate uninstalled."
