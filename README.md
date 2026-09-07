# Dynamic Touch Sampling Rate

动态触感采样率调节模块 — C语言守护进程 + Web管理端，适配 **红米 K90 Max (codename: prague / 型号: 2604FRK1EC)**。

## 功能特性

- **动态采样率调节**：根据前台 App 自动切换 120Hz / 240Hz / 480Hz
- **应用级规则**：每个 App 可独立配置采样率，打开即匹配
- **Web 可视化管理**：`http://127.0.0.1:8080`，状态监控、规则管理、日志查看
- **触摸固件优化**：bind mount 替换 `prague_gtp_thp_config.ini`，支持 135/240/480Hz 三档
- **完整日志记录**：带自动轮转（256KB），方便排查异常
- **低耗电设计**：熄屏自动降频（15s）、亮屏才检测前台应用、条件写入 sysfs
- **低内存**：C语言单线程、固定缓冲区、零 malloc、静态编译约 93KB
- **不挂载 system 分区**：仅 bind `/odm/firmware/` 单个文件，安全可逆
- **最高采样率 480Hz**

## 适配机型

| 机型 | codename | 型号 | 状态 |
|------|----------|------|------|
| 红米 K90 Max | prague | 2604FRK1EC | 主适配 |
| 其他安卓机型 | - | - | 通用模式 |

## 安装

1. 下载 `DynamicTouchSampling-v1.2.0.zip`
2. KernelSU / Magisk 中刷入
3. 重启设备
4. 浏览器访问 `http://127.0.0.1:8080`

> 模块已内含预编译 aarch64 静态二进制（bin/touchd），可直接刷入使用。

## 使用

### Web 管理端

手机本地浏览器访问 **http://127.0.0.1:8080**

功能：
- 实时状态：当前模式、采样率、前台 App、电量、屏幕状态
- 模式切换：自动 / 省电 / 平衡 / 高性能
- 全局配置：轮询间隔、低电量阈值、三档采样率（最高 480Hz）
- 应用规则：添加 / 删除 App 对应的采样率
- 触摸节点：查看已探测节点，支持重新探测
- 日志查看：实时刷新守护进程日志

### 命令行

```bash
touchd --status          # 查看状态
touchd --mode high       # 设置模式 (low/medium/high/auto)
touchd --mode auto       # 恢复自动
touchd --log             # 查看日志
touchd --kill            # 停止守护进程
touchd --web             # 显示 Web 端地址
```

### 配置文件

- 全局配置：`/data/adb/dynamic_touch_sampling/config.conf`
- 应用规则：`/data/adb/dynamic_touch_sampling/app_profiles.conf`
- 日志：`/data/adb/dynamic_touch_sampling/log/touchd.log`

## 采样率档位

| 模式 | 采样率 | 适用场景 |
|------|--------|----------|
| 省电 (low) | 120Hz | 日常阅读、熄屏、低电量 |
| 平衡 (medium) | 240Hz | 社交、视频、浏览器（默认） |
| 高性能 (high) | 480Hz | 游戏、高响应需求 |

> 最高 480Hz，需硬件和触摸固件支持。固件配置中 `rate_game_super=480`。

## 技术架构

```
KernelSU / Magisk 模块
├── post-fs-data.sh
│   ├── 智能检测 /odm/firmware/*.ini
│   └── bind mount 触摸固件配置 (不挂载 system)
├── service.sh
│   └── 启动 touchd --daemon
└── bin/touchd (C语言, 静态 ~93KB)
    ├── 主循环: 状态检测 + 模式决策
    ├── HTTP 服务器: 127.0.0.1:8080 (Web UI)
    ├── 日志系统 (自动轮转 256KB)
    └── sysfs 写入 (条件写入, 避免重复)
```

### 耗电优化

- 熄屏后轮询间隔从 3s 降至 15s
- 仅亮屏且非低电量时检测前台 App
- 采样率未变化时不写入 sysfs
- HTTP 非阻塞 accept，无连接时零 CPU 占用
- 前台 App 检测直接读 `/proc`，不 fork dumpsys

### 日志排查

日志文件：`/data/adb/dynamic_touch_sampling/log/touchd.log`

关键日志事件：
- `detected node:` — 触摸节点自动探测结果
- `foreground:` — 前台 App 变化
- `apply high(480Hz) ->` — 采样率切换
- `FAIL apply` — 写入失败
- `bind:` — 固件配置 bind mount 结果
- `web UI started:` — Web 管理端启动

## 编译

模块已内含预编译二进制。如需重新编译：

```bash
# zig (推荐, 单文件免安装)
zig cc -target aarch64-linux-musl -static -O2 -s \
  -fdata-sections -ffunction-sections -Wl,--gc-sections \
  -o bin/touchd src/touchd.c

# NDK
export ANDROID_NDK_HOME=/path/to/ndk
./build.sh

# 通用交叉编译
sudo apt install gcc-aarch64-linux-gnu
make generic
```

## 目录结构

```
DynamicTouchSampling/
├── module.prop
├── customize.sh
├── service.sh
├── post-fs-data.sh
├── uninstall.sh
├── build.sh / Makefile
├── bin/touchd              # 预编译 aarch64 二进制
├── src/touchd.c            # C 源码
├── etc/
│   ├── config.conf
│   └── app_profiles.conf
├── Link/odm/firmware/
│   └── prague_gtp_thp_config.ini  # 触摸固件配置
└── META-INF/com/google/android/
    ├── update-binary
    └── updater-script
```

## 免责声明

- 本模块仅供学习研究使用
- 刷入前请备份数据，作者不对任何设备损坏负责
- 触摸固件配置基于 K90 系列调校，K90 Max 可能需要根据实际硬件微调
- 480Hz 采样率需屏幕和触摸 IC 硬件支持

## 致谢

- 触摸固件配置参考：优化Redmi k90u 触控 (作者: 澹清拾石子)
- KernelSU / Magisk 模块框架
