#!/bin/bash
# build.sh - 编译 touchd 守护进程
set -e
SRC="src/touchd.c"
OUT="bin/touchd"
mkdir -p bin

echo "=== Dynamic Touch Sampling - Build ==="

if command -v zig >/dev/null 2>&1; then
    echo "[*] 使用 zig 交叉编译..."
    zig cc -target aarch64-linux-musl -static -O2 -s \
      -fdata-sections -ffunction-sections -Wl,--gc-sections -o "$OUT" "$SRC"
elif [ -n "$ANDROID_NDK_HOME" ]; then
    echo "[*] 使用 NDK 编译..."
    CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android-clang"
    "$CC" -static -O2 -s -o "$OUT" "$SRC"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    echo "[*] 使用 aarch64-linux-gnu-gcc 编译..."
    aarch64-linux-gnu-gcc -static -O2 -s -o "$OUT" "$SRC"
else
    echo "[!] 未找到编译器, 请安装 zig 或 Android NDK"
    exit 1
fi

echo "[+] 编译完成: $OUT"
ls -lh "$OUT"
