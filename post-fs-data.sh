#!/system/bin/sh
# post-fs-data.sh - 早期启动: bind mount 固件配置 + 环境初始化
MODDIR=${0%/*}
RUNTIME="/data/adb/dynamic_touch_sampling"
LOG="$RUNTIME/log/touchd.log"

mkdir -p "$RUNTIME/log"
chmod 755 "$RUNTIME" "$RUNTIME/log"
echo "[$(date '+%m-%d %H:%M:%S')] post-fs-data: start" >> "$LOG"

# 智能 bind mount 触摸固件配置 (不挂载 system 分区, 仅 bind 单个文件)
bind_firmware() {
    local src_dir="$MODDIR/Link/odm/firmware"
    local dst_dir="/odm/firmware"
    local found=0
    [ -d "$src_dir" ] || return 0
    [ -d "$dst_dir" ] || return 0
    for src in "$src_dir"/*.ini; do
        [ -f "$src" ] || continue
        local fname=$(basename "$src")
        if [ -f "$dst_dir/$fname" ]; then
            mount --bind "$src" "$dst_dir/$fname" 2>> "$LOG"
            restorecon "$dst_dir/$fname" 2>/dev/null
            echo "[$(date '+%m-%d %H:%M:%S')] bind: $dst_dir/$fname" >> "$LOG"
            found=1
            continue
        fi
        if [ "$found" -eq 0 ]; then
            for dst in "$dst_dir"/*_gtp_thp_config.ini; do
                [ -f "$dst" ] || continue
                mount --bind "$src" "$dst" 2>> "$LOG"
                restorecon "$dst" 2>/dev/null
                echo "[$(date '+%m-%d %H:%M:%S')] bind(fuzzy): $dst" >> "$LOG"
                found=1
                break
            done
        fi
    done
    [ "$found" -eq 0 ] && echo "[$(date '+%m-%d %H:%M:%S')] WARN: no firmware config found" >> "$LOG"
}

bind_firmware

set_perm "$MODDIR/bin/touchd" 0 0 0755 2>/dev/null
rm -f "$RUNTIME/touchd.pid" "$RUNTIME/mode"
echo "[$(date '+%m-%d %H:%M:%S')] post-fs-data: done" >> "$LOG"
