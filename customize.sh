#!/system/bin/sh
# customize.sh - 安装脚本
MODDIR=${0%/*}
API_LEVEL=$(getprop ro.build.version.sdk)
DEVICE_CODENAME=$(getprop ro.product.device)
DEVICE_MODEL=$(getprop ro.product.model)
DEVICE_VENDOR=$(getprop ro.product.vendor.device)
MANUFACTURER=$(getprop ro.product.manufacturer)
ANDROID_VER=$(getprop ro.build.version.release)

ui_print() { echo "$1"; }

ui_print "=============================================="
ui_print "  Touch Control - 动态触感采样率调节"
ui_print "  Version v1.0.0  |  by Jc"
ui_print "  C语言守护进程 + KernelSU WebUI 可视化"
ui_print "=============================================="
ui_print ""
ui_print "[*] 设备: $MANUFACTURER $DEVICE_MODEL"
ui_print "[*] 代号: $DEVICE_CODENAME"
ui_print "[*] Android: $ANDROID_VER (API $API_LEVEL)"
ui_print ""

IS_TARGET=0
if [ "$DEVICE_CODENAME" = "prague" ] || [ "$DEVICE_VENDOR" = "prague" ] || \
   [ "$DEVICE_MODEL" = "2604FRK1EC" ] || echo "$DEVICE_MODEL" | grep -qi "K90 Max"; then
    IS_TARGET=1
    ui_print "[+] 目标机型确认: 红米 K90 Max (prague)"
else
    ui_print "[!] 非目标机型, 将以通用模式安装"
    ui_print "[!] 触摸节点可能需要手动配置"
fi

if [ "$API_LEVEL" -ge 35 ]; then
    ui_print "[+] Android 17+ 已适配"
else
    ui_print "[*] Android $ANDROID_VER, 兼容运行"
fi
ui_print ""

ui_print "[*] 设置文件权限..."
set_perm_recursive "$MODDIR" 0 0 0755 0644
set_perm "$MODDIR/bin/touchd" 0 0 0755
set_perm "$MODDIR/service.sh" 0 0 0755
set_perm "$MODDIR/post-fs-data.sh" 0 0 0755
set_perm "$MODDIR/uninstall.sh" 0 0 0755
set_perm "$MODDIR/customize.sh" 0 0 0755

RUNTIME="/data/adb/dynamic_touch_sampling"
mkdir -p "$RUNTIME/log"
set_perm_recursive "$RUNTIME" 0 0 0755 0644
[ ! -f "$RUNTIME/config.conf" ] && cp "$MODDIR/etc/config.conf" "$RUNTIME/config.conf"
[ ! -f "$RUNTIME/app_profiles.conf" ] && cp "$MODDIR/etc/app_profiles.conf" "$RUNTIME/app_profiles.conf"

cat > "$RUNTIME/device_info" <<EOF
codename=$DEVICE_CODENAME
vendor=$DEVICE_VENDOR
model=$DEVICE_MODEL
manufacturer=$MANUFACTURER
android=$ANDROID_VER
api=$API_LEVEL
is_target=$IS_TARGET
install_time=$(date)
EOF

ui_print ""
ui_print "=============================================="
ui_print "[+] 安装完成! 请重启设备"
ui_print "=============================================="
ui_print ""
ui_print "使用方法:"
ui_print "  1. 重启设备后, 打开 KernelSU 管理器"
ui_print "  2. 进入模块 -> Touch Control -> 打开 WebUI"
ui_print "  3. 在 WebUI 中可视化配置采样率和应用规则"
ui_print ""
ui_print "核心功能:"
ui_print "  - 三档采样率: 省电/平衡/高性能 (最高 480Hz)"
ui_print "  - 应用规则: 打开不同 App 自动切换采样率"
ui_print "  - 应用列表: 自动扫描本机已安装应用, 可视化选择"
ui_print "  - 实时状态: 当前采样率/模式/前台应用/电量"
ui_print "  - 低耗电: 熄屏自动降频, 低电量自动省电"
ui_print "  - 日志记录: 完整运行日志, 方便排查"
ui_print ""
ui_print "命令行控制 (终端):"
ui_print "  touchd --status   查看状态"
ui_print "  touchd --mode high  强制高性能"
ui_print "  touchd --mode auto  恢复自动"
ui_print "  touchd --log      查看日志"
ui_print ""
ui_print "配置目录: $RUNTIME"
ui_print "  config.conf        全局配置"
ui_print "  app_profiles.conf  应用规则"
ui_print "  log/touchd.log     运行日志"
ui_print ""
ui_print "卸载模块 + 重启 = 完全恢复原始状态"
ui_print "=============================================="
