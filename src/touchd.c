/*
 * touchd.c - Dynamic Touch Sampling Rate Daemon
 * 动态触感采样率守护进程 (精简HTTP API版)
 * 适配: 红米 K90 Max (codename: prague / 型号: 2604FRK1EC) / Android 17
 * 编译: zig cc -target aarch64-linux-musl -static -O2 -s -o touchd touchd.c
 *
 * 设计: 低内存(固定缓冲区/零malloc/单线程) 低耗电(熄屏降频/条件写入)
 * API: 127.0.0.1:8080 (仅本地回环) - /api/status /api/apps /api/config /api/profiles /api/log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>

/* strcasestr 兼容实现 (musl libc) */
static char *my_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}
#define strcasestr my_strcasestr

#define MAX_PATH     256
#define MAX_LINE     512
#define MAX_PKG      128
#define MAX_VAL      32
#define MAX_NODES    8
#define MAX_PROFILES 128
#define MAX_CAND     32
#define MAX_APPS     512
#define MAX_RATE     480
#define LOG_MAX      (256 * 1024)
#define HTTP_PORT    8080
#define HTTP_BUF     8192

#define RUNTIME_DIR  "/data/adb/dynamic_touch_sampling"
#define LOG_FILE     RUNTIME_DIR "/log/touchd.log"
#define CONFIG_FILE  RUNTIME_DIR "/config.conf"
#define PROFILES_FILE RUNTIME_DIR "/app_profiles.conf"
#define PID_FILE     RUNTIME_DIR "/touchd.pid"
#define MODE_FILE    RUNTIME_DIR "/mode"
#define STATUS_FILE  RUNTIME_DIR "/status"

typedef enum { MODE_LOW = 0, MODE_MEDIUM = 1, MODE_HIGH = 2, MODE_AUTO = 3 } TouchMode;

typedef struct {
    char path[MAX_PATH];
    char val_low[MAX_VAL];
    char val_med[MAX_VAL];
    char val_high[MAX_VAL];
} TouchNode;

typedef struct {
    char pkg[MAX_PKG];
    TouchMode mode;
} AppProfile;

typedef struct {
    char pkg[MAX_PKG];
    char name[MAX_PKG];
    int  type; /* 0=第三方用户应用, 1=系统应用 */
} AppInfo;

typedef struct {
    TouchNode   nodes[MAX_NODES];
    int         node_count;
    AppProfile  profiles[MAX_PROFILES];
    int         profile_count;
    AppInfo     apps[MAX_APPS];
    int         app_count;
    TouchMode   current;
    TouchMode   forced;
    int         poll_interval;
    int         poll_idle;
    int         battery_low;
    int         debug;
    char        cur_app[MAX_PKG];
    int         screen_on;
    int         battery;
    int         rate_low;
    int         rate_med;
    int         rate_high;
    int         http_fd;
    int         switch_count;
    time_t      start_time;
} Daemon;

static Daemon g_ctx;
static volatile sig_atomic_t g_running = 1;

static void trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (*s == '\0') return;
    e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) *e-- = '\0';
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static int read_file(const char *path, char *buf, int sz) {
    int fd = open(path, O_RDONLY), n;
    if (fd < 0) return -1;
    n = read(fd, buf, sz - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

static int write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_TRUNC), len = strlen(val), n;
    if (fd < 0) return -1;
    n = write(fd, val, len);
    close(fd);
    return (n == len) ? 0 : -1;
}

static int clamp_rate(int v) {
    if (v < 60) v = 60;
    if (v > MAX_RATE) v = MAX_RATE;
    return v;
}

static void log_rotate(void) {
    struct stat st;
    char old[MAX_PATH];
    if (stat(LOG_FILE, &st) != 0 || st.st_size < LOG_MAX) return;
    snprintf(old, sizeof(old), "%s.1", LOG_FILE);
    rename(LOG_FILE, old);
}

static void log_msg(const char *fmt, ...) {
    va_list ap;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    FILE *fp;
    log_rotate();
    strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", tm);
    fp = fopen(LOG_FILE, "a");
    if (fp) {
        fprintf(fp, "[%s] ", ts);
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fputc('\n', fp);
        fclose(fp);
    }
    if (g_ctx.debug) {
        fprintf(stderr, "[%s] ", ts);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

static TouchMode str_to_mode(const char *s) {
    if (!strcasecmp(s, "low") || !strcmp(s, "0")) return MODE_LOW;
    if (!strcasecmp(s, "medium") || !strcasecmp(s, "med") || !strcmp(s, "1")) return MODE_MEDIUM;
    if (!strcasecmp(s, "high") || !strcmp(s, "2")) return MODE_HIGH;
    return MODE_AUTO;
}

static const char *mode_to_str(TouchMode m) {
    switch (m) {
        case MODE_LOW: return "low";
        case MODE_MEDIUM: return "medium";
        case MODE_HIGH: return "high";
        default: return "auto";
    }
}

static int mode_to_rate(TouchMode m) {
    switch (m) {
        case MODE_LOW: return g_ctx.rate_low;
        case MODE_MEDIUM: return g_ctx.rate_med;
        case MODE_HIGH: return g_ctx.rate_high;
        default: return g_ctx.rate_med;
    }
}

static void save_config(void) {
    FILE *fp = fopen(CONFIG_FILE, "w");
    int i;
    if (!fp) return;
    fprintf(fp, "# Dynamic Touch Sampling - 全局配置 (Android 17)\n");
    fprintf(fp, "poll_interval=%d\n", g_ctx.poll_interval);
    fprintf(fp, "poll_idle=%d\n", g_ctx.poll_idle);
    fprintf(fp, "battery_low=%d\n", g_ctx.battery_low);
    fprintf(fp, "debug=%d\n", g_ctx.debug);
    fprintf(fp, "rate_low=%d\n", g_ctx.rate_low);
    fprintf(fp, "rate_med=%d\n", g_ctx.rate_med);
    fprintf(fp, "rate_high=%d\n", g_ctx.rate_high);
    fprintf(fp, "\n# 触摸节点配置\n");
    for (i = 0; i < g_ctx.node_count; i++) {
        fprintf(fp, "node_path=%s\n", g_ctx.nodes[i].path);
        fprintf(fp, "node_low=%s\n", g_ctx.nodes[i].val_low);
        fprintf(fp, "node_medium=%s\n", g_ctx.nodes[i].val_med);
        fprintf(fp, "node_high=%s\n", g_ctx.nodes[i].val_high);
    }
    fclose(fp);
    log_msg("config saved");
}

static void save_profiles(void) {
    FILE *fp = fopen(PROFILES_FILE, "w");
    int i;
    if (!fp) return;
    fprintf(fp, "# 应用采样率规则: 包名=模式(low/medium/high)\n");
    for (i = 0; i < g_ctx.profile_count; i++) {
        fprintf(fp, "%s=%s\n", g_ctx.profiles[i].pkg, mode_to_str(g_ctx.profiles[i].mode));
    }
    fclose(fp);
}

static void parse_config_line(const char *line, TouchNode *cur) {
    char key[64], val[MAX_VAL];
    const char *eq = strchr(line, '=');
    int klen;
    if (!eq) return;
    klen = eq - line;
    if (klen >= (int)sizeof(key)) klen = sizeof(key) - 1;
    memcpy(key, line, klen);
    key[klen] = '\0';
    strncpy(val, eq + 1, sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    trim(key); trim(val);

    if (!strcmp(key, "poll_interval")) g_ctx.poll_interval = atoi(val);
    else if (!strcmp(key, "poll_idle")) g_ctx.poll_idle = atoi(val);
    else if (!strcmp(key, "battery_low")) g_ctx.battery_low = atoi(val);
    else if (!strcmp(key, "debug")) g_ctx.debug = atoi(val);
    else if (!strcmp(key, "rate_low")) g_ctx.rate_low = clamp_rate(atoi(val));
    else if (!strcmp(key, "rate_med")) g_ctx.rate_med = clamp_rate(atoi(val));
    else if (!strcmp(key, "rate_high")) g_ctx.rate_high = clamp_rate(atoi(val));
    else if (!strcmp(key, "node_path")) {
        if (cur->path[0] && g_ctx.node_count < MAX_NODES) {
            memcpy(&g_ctx.nodes[g_ctx.node_count], cur, sizeof(TouchNode));
            g_ctx.node_count++;
        }
        memset(cur, 0, sizeof(TouchNode));
        strncpy(cur->path, val, MAX_PATH - 1);
    }
    else if (!strcmp(key, "node_low") && cur) strncpy(cur->val_low, val, MAX_VAL - 1);
    else if (!strcmp(key, "node_medium") && cur) strncpy(cur->val_med, val, MAX_VAL - 1);
    else if (!strcmp(key, "node_high") && cur) strncpy(cur->val_high, val, MAX_VAL - 1);
}

static int load_config(void) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    char line[MAX_LINE];
    TouchNode cur;
    if (!fp) return -1;
    memset(&cur, 0, sizeof(cur));
    g_ctx.node_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        parse_config_line(line, &cur);
    }
    if (cur.path[0] && g_ctx.node_count < MAX_NODES) {
        memcpy(&g_ctx.nodes[g_ctx.node_count], &cur, sizeof(TouchNode));
        g_ctx.node_count++;
    }
    fclose(fp);
    if (g_ctx.poll_interval <= 0) g_ctx.poll_interval = 3;
    if (g_ctx.poll_idle <= 0) g_ctx.poll_idle = 15;
    if (g_ctx.battery_low <= 0) g_ctx.battery_low = 20;
    if (g_ctx.rate_low <= 0) g_ctx.rate_low = 120;
    if (g_ctx.rate_med <= 0) g_ctx.rate_med = 240;
    if (g_ctx.rate_high <= 0) g_ctx.rate_high = MAX_RATE;
    return 0;
}

static int load_profiles(void) {
    FILE *fp = fopen(PROFILES_FILE, "r");
    char line[MAX_LINE], *eq;
    if (!fp) return -1;
    g_ctx.profile_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        if (g_ctx.profile_count >= MAX_PROFILES) break;
        *eq = '\0';
        trim(line); trim(eq + 1);
        strncpy(g_ctx.profiles[g_ctx.profile_count].pkg, line, MAX_PKG - 1);
        g_ctx.profiles[g_ctx.profile_count].pkg[MAX_PKG - 1] = '\0';
        g_ctx.profiles[g_ctx.profile_count].mode = str_to_mode(eq + 1);
        g_ctx.profile_count++;
    }
    fclose(fp);
    return 0;
}

/* Android 17 扩展触摸节点候选列表 */
static const char *cand_nodes[MAX_CAND] = {
    "/sys/devices/virtual/touch/tp_dev/boost_120hz",
    "/sys/devices/virtual/touch/tp_dev/boost_240hz",
    "/sys/devices/virtual/touch/tp_dev/report_rate",
    "/sys/class/touch/touch_dev/report_rate",
    "/sys/class/touch/touch_dev/touch_report_rate",
    "/sys/class/touch/tp_dev/report_rate",
    "/sys/class/touch/tp_dev/boost_120hz",
    "/sys/class/touch/tp_dev/touch_report_rate",
    "/proc/touchpanel/report_rate",
    "/proc/touchpanel/boost",
    "/proc/touchpanel/game_switch_enable",
    "/proc/touchpanel/oplus_touchpanel_game_switch_enable",
    "/sys/bus/i2c/devices/3-0038/report_rate",
    "/sys/bus/i2c/devices/3-0038/touch_report_rate",
    "/sys/bus/i2c/devices/3-0038/boost_120hz",
    "/sys/bus/i2c/devices/2-0020/report_rate",
    "/sys/bus/i2c/devices/2-0020/touch_report_rate",
    "/sys/bus/i2c/devices/2-0020/boost_120hz",
    "/sys/class/input/event0/device/report_rate",
    "/sys/class/input/event1/device/report_rate",
    "/sys/class/input/event2/device/report_rate",
    "/sys/devices/platform/soc/980000.i2c/i2c-3/3-0038/boost_120hz",
    "/sys/devices/platform/soc/980000.i2c/i2c-3/3-0038/report_rate",
    "/sys/devices/platform/soc/990000.i2c/i2c-2/2-0020/report_rate",
    "/sys/touchpanel/report_rate",
    "/sys/touchpanel/boost",
    "/sys/devices/virtual/touch/tp_dev/touch_report_rate",
    "/sys/devices/platform/soc/*.i2c/i2c-*/report_rate",
    "/sys/kernel/touchpanel/report_rate",
    "/sys/kernel/touchpanel/boost",
    "/sys/devices/platform/soc/a90000.i2c/i2c-3/3-0038/report_rate",
    "/sys/devices/platform/soc/a90000.i2c/i2c-3/3-0038/boost_120hz",
};

/* 动态扫描 /sys/class/touch 和 /sys/class/input 下所有设备 */
static int scan_touch_nodes(void) {
    DIR *dir;
    struct dirent *de;
    int detected = g_ctx.node_count;
    const char *sys_dirs[] = {"/sys/class/touch", "/sys/class/input", NULL};
    const char *node_files[] = {"report_rate", "touch_report_rate", "boost_120hz",
                                  "boost_240hz", "boost", "game_switch_enable",
                                  "tp_report_rate", "touch_rate", NULL};
    int d, f;
    for (d = 0; sys_dirs[d] && detected < MAX_NODES; d++) {
        dir = opendir(sys_dirs[d]);
        if (!dir) continue;
        while ((de = readdir(dir)) != NULL && detected < MAX_NODES) {
            char dev_path[MAX_PATH], name_buf[64];
            if (de->d_name[0] == '.') continue;
            /* 尝试 device 子目录 */
            snprintf(dev_path, sizeof(dev_path), "%s/%s/device", sys_dirs[d], de->d_name);
            if (!file_exists(dev_path))
                snprintf(dev_path, sizeof(dev_path), "%s/%s", sys_dirs[d], de->d_name);
            /* 读取设备名, 只处理触摸相关 */
            snprintf(name_buf, sizeof(name_buf), "%s/name", dev_path);
            if (read_file(name_buf, name_buf, sizeof(name_buf)) > 0) {
                trim(name_buf);
                if (strlen(name_buf) > 0 &&
                    !strcasestr(name_buf, "touch") &&
                    !strcasestr(name_buf, "tp") &&
                    !strcasestr(name_buf, "gtp") &&
                    !strcasestr(name_buf, "fts") &&
                    !strcasestr(name_buf, "goodix") &&
                    !strcasestr(name_buf, "focal") &&
                    !strcasestr(name_buf, "synaptics") &&
                    !strcasestr(name_buf, "atmel") &&
                    !strcasestr(name_buf, "cyttsp")) {
                    /* 非触摸设备但仍尝试扫描节点 (input 设备可能有) */
                }
            }
            for (f = 0; node_files[f] && detected < MAX_NODES; f++) {
                char node_path[MAX_PATH], buf[16];
                int i, dup = 0;
                snprintf(node_path, sizeof(node_path), "%s/%s", dev_path, node_files[f]);
                if (!file_exists(node_path)) continue;
                if (read_file(node_path, buf, sizeof(buf)) < 0) continue;
                trim(buf);
                for (i = 0; i < detected; i++)
                    if (!strcmp(g_ctx.nodes[i].path, node_path)) { dup = 1; break; }
                if (dup) continue;
                TouchNode *n = &g_ctx.nodes[detected];
                memset(n, 0, sizeof(TouchNode));
                strncpy(n->path, node_path, MAX_PATH - 1);
                if (strstr(node_path, "boost") || strstr(node_path, "game_switch")) {
                    strcpy(n->val_low, "0"); strcpy(n->val_med, "1"); strcpy(n->val_high, "1");
                } else {
                    strcpy(n->val_low, "120"); strcpy(n->val_med, "240");
                    snprintf(n->val_high, sizeof(n->val_high), "%d", MAX_RATE);
                }
                detected++;
                log_msg("scanned node: %s (cur=%s)", node_path, buf);
            }
        }
        closedir(dir);
    }
    g_ctx.node_count = detected;
    return detected;
}

static int auto_detect_nodes(void) {
    int i, detected = 0;
    char buf[16];
    /* 1. 扫描预定义候选列表 */
    for (i = 0; i < MAX_CAND && detected < MAX_NODES; i++) {
        const char *p = cand_nodes[i];
        if (strchr(p, '*')) continue;
        if (!file_exists(p)) continue;
        if (read_file(p, buf, sizeof(buf)) < 0) continue;
        trim(buf);
        TouchNode *n = &g_ctx.nodes[detected];
        memset(n, 0, sizeof(TouchNode));
        strncpy(n->path, p, MAX_PATH - 1);
        if (strstr(p, "boost") || strstr(p, "game_switch")) {
            strcpy(n->val_low, "0"); strcpy(n->val_med, "1"); strcpy(n->val_high, "1");
        } else {
            strcpy(n->val_low, "120"); strcpy(n->val_med, "240");
            snprintf(n->val_high, sizeof(n->val_high), "%d", MAX_RATE);
        }
        detected++;
        log_msg("detected node: %s (cur=%s)", p, buf);
    }
    g_ctx.node_count = detected;
    /* 2. 动态扫描 /sys/class/touch 和 /sys/class/input */
    scan_touch_nodes();
    return g_ctx.node_count;
}

/* 扫描已安装应用列表 (Android 17 兼容, 区分系统/第三方) */
static int scan_apps(void) {
    DIR *dir;
    struct dirent *de;
    int count = 0;
    char path[MAX_PATH];

    g_ctx.app_count = 0;

    /* 方式1: 扫描 /data/app/ (第三方用户应用, type=0) */
    dir = opendir("/data/app");
    if (dir) {
        while ((de = readdir(dir)) != NULL && count < MAX_APPS) {
            char *dash, pkg[MAX_PKG];
            if (de->d_name[0] == '.') continue;
            strncpy(pkg, de->d_name, MAX_PKG - 1);
            pkg[MAX_PKG - 1] = '\0';
            dash = strchr(pkg, '-');
            if (dash) *dash = '\0';
            dash = strchr(pkg, '=');
            if (dash) *dash = '\0';
            if (strlen(pkg) < 4 || !strchr(pkg, '.')) continue;
            {
                int j, dup = 0;
                for (j = 0; j < count; j++) {
                    if (!strcmp(g_ctx.apps[j].pkg, pkg)) { dup = 1; break; }
                }
                if (dup) continue;
            }
            strncpy(g_ctx.apps[count].pkg, pkg, MAX_PKG - 1);
            g_ctx.apps[count].pkg[MAX_PKG - 1] = '\0';
            strncpy(g_ctx.apps[count].name, pkg, MAX_PKG - 1);
            g_ctx.apps[count].name[MAX_PKG - 1] = '\0';
            g_ctx.apps[count].type = 0; /* 第三方用户应用 */
            count++;
        }
        closedir(dir);
    }

    /* 方式2: 扫描系统目录 (系统应用, type=1) */
    {
        const char *sys_dirs[] = {"/system/app", "/system/priv-app", "/product/app",
                                    "/product/priv-app", "/vendor/app", "/vendor/priv-app", NULL};
        int d;
        for (d = 0; sys_dirs[d] && count < MAX_APPS; d++) {
            dir = opendir(sys_dirs[d]);
            if (!dir) continue;
            while ((de = readdir(dir)) != NULL && count < MAX_APPS) {
                char apk_path[MAX_PATH], pkg[MAX_PKG];
                DIR *sub;
                if (de->d_name[0] == '.') continue;
                snprintf(apk_path, sizeof(apk_path), "%s/%s", sys_dirs[d], de->d_name);
                sub = opendir(apk_path);
                if (sub) {
                    struct dirent *sde;
                    while ((sde = readdir(sub)) != NULL) {
                        if (strstr(sde->d_name, ".apk")) {
                            strncpy(pkg, de->d_name, MAX_PKG - 1);
                            pkg[MAX_PKG - 1] = '\0';
                            {
                                int j, dup = 0;
                                for (j = 0; j < count; j++) {
                                    if (!strcmp(g_ctx.apps[j].pkg, pkg)) { dup = 1; break; }
                                }
                                if (!dup && strlen(pkg) > 2) {
                                    strncpy(g_ctx.apps[count].pkg, pkg, MAX_PKG - 1);
                                    g_ctx.apps[count].pkg[MAX_PKG - 1] = '\0';
                                    strncpy(g_ctx.apps[count].name, pkg, MAX_PKG - 1);
                                    g_ctx.apps[count].name[MAX_PKG - 1] = '\0';
                                    g_ctx.apps[count].type = 1; /* 系统应用 */
                                    count++;
                                }
                            }
                            break;
                        }
                    }
                    closedir(sub);
                }
            }
            closedir(dir);
        }
    }

    g_ctx.app_count = count;
    {
        int user = 0, sys = 0, i;
        for (i = 0; i < count; i++) {
            if (g_ctx.apps[i].type == 0) user++; else sys++;
        }
        log_msg("scanned %d apps (user=%d, system=%d)", count, user, sys);
    }
    return count;
}

static char *get_foreground_app(void) {
    DIR *dir;
    struct dirent *de;
    static char best_pkg[MAX_PKG];
    char path[MAX_PATH], buf[MAX_PKG];
    int best_oom = 9999, oom, n;
    best_pkg[0] = '\0';
    dir = opendir("/proc");
    if (!dir) return NULL;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/oom_score_adj", de->d_name);
        if (read_file(path, buf, sizeof(buf)) <= 0) continue;
        oom = atoi(buf);
        if (oom <= -900 || oom >= best_oom) continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
        n = read_file(path, buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = '\0';
        if (strlen(buf) < 4 || !strchr(buf, '.')) continue;
        best_oom = oom;
        strncpy(best_pkg, buf, MAX_PKG - 1);
        best_pkg[MAX_PKG - 1] = '\0';
    }
    closedir(dir);
    return best_pkg[0] ? best_pkg : NULL;
}

static int is_screen_on(void) {
    char buf[16];
    if (read_file("/sys/class/leds/lcd-backlight/brightness", buf, sizeof(buf)) > 0)
        return atoi(buf) > 0;
    if (read_file("/sys/devices/virtual/backlight/backlight/brightness", buf, sizeof(buf)) > 0)
        return atoi(buf) > 0;
    return 1;
}

static int get_battery_level(void) {
    char buf[16];
    if (read_file("/sys/class/power_supply/battery/capacity", buf, sizeof(buf)) > 0)
        return atoi(buf);
    return 100;
}

static void apply_mode(TouchMode mode) {
    int i;
    const char *val;
    char cur[16];
    if (mode == g_ctx.current) return;
    for (i = 0; i < g_ctx.node_count; i++) {
        TouchNode *n = &g_ctx.nodes[i];
        switch (mode) {
            case MODE_LOW: val = n->val_low; break;
            case MODE_MEDIUM: val = n->val_med; break;
            case MODE_HIGH: val = n->val_high; break;
            default: val = n->val_med; break;
        }
        if (read_file(n->path, cur, sizeof(cur)) > 0) {
            trim(cur);
            if (!strcmp(cur, val)) continue;
        }
        if (write_file(n->path, val) == 0)
            log_msg("apply %s(%dHz) -> %s = %s", mode_to_str(mode), mode_to_rate(mode), n->path, val);
        else
            log_msg("FAIL apply %s -> %s = %s (errno=%d)", mode_to_str(mode), n->path, val, errno);
    }
    g_ctx.current = mode;
    g_ctx.switch_count++;
    write_file(STATUS_FILE, mode_to_str(mode));
}

static TouchMode decide_mode(void) {
    int i;
    if (g_ctx.forced != MODE_AUTO) return g_ctx.forced;
    if (!g_ctx.screen_on) return MODE_LOW;
    if (g_ctx.battery <= g_ctx.battery_low) return MODE_LOW;
    for (i = 0; i < g_ctx.profile_count; i++)
        if (!strcmp(g_ctx.profiles[i].pkg, g_ctx.cur_app))
            return g_ctx.profiles[i].mode;
    return MODE_MEDIUM;
}

static void write_pid(void) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", getpid());
    write_file(PID_FILE, buf);
}

static void signal_handler(int sig) { (void)sig; g_running = 0; }

static void daemonize(void) {
    pid_t pid;
    int fd;
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);
    setsid();
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);
    umask(0);
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
}

/* ============ 精简版 HTTP API ============ */

static int http_init(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    struct sockaddr_in addr;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(HTTP_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 4) < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

static void http_send(int fd, const char *content_type, const char *body) {
    char header[512];
    int len = strlen(body);
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        content_type, len);
    write(fd, header, strlen(header));
    write(fd, body, len);
}

static void http_json_status(int fd) {
    char buf[2048];
    int uptime = (int)(time(NULL) - g_ctx.start_time);
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"current\":\"%s\",\"current_rate\":%d,\"forced\":\"%s\","
        "\"foreground_app\":\"%s\",\"screen_on\":%d,\"battery\":%d,\"battery_low\":%d,"
        "\"poll_interval\":%d,\"poll_idle\":%d,\"rate_low\":%d,\"rate_med\":%d,\"rate_high\":%d,"
        "\"node_count\":%d,\"profile_count\":%d,\"app_count\":%d,\"switch_count\":%d,"
        "\"uptime\":%d,\"max_rate\":%d,\"debug\":%d}",
        mode_to_str(g_ctx.current), mode_to_rate(g_ctx.current), mode_to_str(g_ctx.forced),
        g_ctx.cur_app, g_ctx.screen_on, g_ctx.battery, g_ctx.battery_low,
        g_ctx.poll_interval, g_ctx.poll_idle, g_ctx.rate_low, g_ctx.rate_med, g_ctx.rate_high,
        g_ctx.node_count, g_ctx.profile_count, g_ctx.app_count, g_ctx.switch_count,
        uptime, MAX_RATE, g_ctx.debug);
    http_send(fd, "application/json", buf);
}

static void http_json_apps(int fd) {
    char *buf = malloc(65536);
    int pos = 0, i;
    if (!buf) { http_send(fd, "application/json", "{\"error\":\"oom\"}"); return; }
    pos += snprintf(buf + pos, 65536 - pos, "{\"apps\":[");
    for (i = 0; i < g_ctx.app_count; i++) {
        if (i > 0) pos += snprintf(buf + pos, 65536 - pos, ",");
        pos += snprintf(buf + pos, 65536 - pos, "{\"pkg\":\"%s\",\"name\":\"%s\",\"type\":%d}",
                g_ctx.apps[i].pkg, g_ctx.apps[i].name, g_ctx.apps[i].type);
        if (pos > 64000) break;
    }
    pos += snprintf(buf + pos, 65536 - pos, "],\"count\":%d}", g_ctx.app_count);
    http_send(fd, "application/json", buf);
    free(buf);
}

static void http_json_profiles(int fd) {
    char buf[8192];
    int pos = 0, i;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"profiles\":[");
    for (i = 0; i < g_ctx.profile_count; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"pkg\":\"%s\",\"mode\":\"%s\"}", g_ctx.profiles[i].pkg, mode_to_str(g_ctx.profiles[i].mode));
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", g_ctx.profile_count);
    http_send(fd, "application/json", buf);
}

static void http_json_config(int fd) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"poll_interval\":%d,\"poll_idle\":%d,\"battery_low\":%d,\"debug\":%d,"
        "\"rate_low\":%d,\"rate_med\":%d,\"rate_high\":%d,\"max_rate\":%d}",
        g_ctx.poll_interval, g_ctx.poll_idle, g_ctx.battery_low, g_ctx.debug,
        g_ctx.rate_low, g_ctx.rate_med, g_ctx.rate_high, MAX_RATE);
    http_send(fd, "application/json", buf);
}

static char *post_val(const char *body, const char *key) {
    static char val[256];
    char search[128], *p, *end;
    snprintf(search, sizeof(search), "%s=", key);
    p = strstr(body, search);
    if (!p) return NULL;
    p += strlen(search);
    end = p;
    while (*end && *end != '&' && *end != '\r' && *end != '\n') end++;
    if (end - p >= (int)sizeof(val)) return NULL;
    memcpy(val, p, end - p);
    val[end - p] = '\0';
    return val;
}

/* 从 URL query string 中提取参数 (用于 JSONP GET 请求) */
static char *url_param(const char *query, const char *key) {
    return post_val(query, key); /* 复用 post_val, 格式相同 key=val&key=val */
}

/* URL 解码 */
static void url_decode(char *s) {
    char *p = s, *o = s;
    while (*p) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = (p[1] >= 'A') ? (p[1] | 32) - 'a' + 10 : p[1] - '0';
            int lo = (p[2] >= 'A') ? (p[2] | 32) - 'a' + 10 : p[2] - '0';
            *o++ = (char)((hi << 4) | lo);
            p += 3;
        } else if (*p == '+') { *o++ = ' '; p++; }
        else *o++ = *p++;
    }
    *o = '\0';
}

/* 发送 JSON 或 JSONP 响应 */
static void http_send_json(int fd, const char *json, const char *callback) {
    if (callback && *callback) {
        char buf[65536];
        int n = snprintf(buf, sizeof(buf), "%s(%s);", callback, json);
        http_send(fd, "application/javascript; charset=utf-8", buf);
        (void)n;
    } else {
        http_send(fd, "application/json", json);
    }
}

static void http_handle_post(int fd, const char *path, const char *body) {
    if (!strcmp(path, "/api/config")) {
        char *v;
        if ((v = post_val(body, "poll_interval"))) g_ctx.poll_interval = atoi(v);
        if ((v = post_val(body, "poll_idle"))) g_ctx.poll_idle = atoi(v);
        if ((v = post_val(body, "battery_low"))) g_ctx.battery_low = atoi(v);
        if ((v = post_val(body, "debug"))) g_ctx.debug = atoi(v);
        if ((v = post_val(body, "rate_low"))) g_ctx.rate_low = clamp_rate(atoi(v));
        if ((v = post_val(body, "rate_med"))) g_ctx.rate_med = clamp_rate(atoi(v));
        if ((v = post_val(body, "rate_high"))) g_ctx.rate_high = clamp_rate(atoi(v));
        save_config();
        http_send(fd, "application/json", "{\"status\":\"ok\"}");
    } else if (!strcmp(path, "/api/profiles")) {
        char *action = post_val(body, "action");
        char *pkg = post_val(body, "pkg");
        char *mode = post_val(body, "mode");
        if (action && pkg) {
            if (!strcmp(action, "add") && mode) {
                if (g_ctx.profile_count < MAX_PROFILES) {
                    strncpy(g_ctx.profiles[g_ctx.profile_count].pkg, pkg, MAX_PKG - 1);
                    g_ctx.profiles[g_ctx.profile_count].pkg[MAX_PKG - 1] = '\0';
                    g_ctx.profiles[g_ctx.profile_count].mode = str_to_mode(mode);
                    g_ctx.profile_count++;
                    save_profiles();
                    http_send(fd, "application/json", "{\"status\":\"ok\",\"action\":\"add\"}");
                } else http_send(fd, "application/json", "{\"error\":\"full\"}");
            } else if (!strcmp(action, "remove")) {
                int i, j;
                for (i = 0; i < g_ctx.profile_count; i++) {
                    if (!strcmp(g_ctx.profiles[i].pkg, pkg)) {
                        for (j = i; j < g_ctx.profile_count - 1; j++)
                            memcpy(&g_ctx.profiles[j], &g_ctx.profiles[j+1], sizeof(AppProfile));
                        g_ctx.profile_count--;
                        save_profiles();
                        break;
                    }
                }
                http_send(fd, "application/json", "{\"status\":\"ok\",\"action\":\"remove\"}");
            } else if (!strcmp(action, "update") && mode) {
                int i;
                for (i = 0; i < g_ctx.profile_count; i++) {
                    if (!strcmp(g_ctx.profiles[i].pkg, pkg)) {
                        g_ctx.profiles[i].mode = str_to_mode(mode);
                        save_profiles();
                        break;
                    }
                }
                http_send(fd, "application/json", "{\"status\":\"ok\",\"action\":\"update\"}");
            } else http_send(fd, "application/json", "{\"error\":\"invalid action\"}");
        } else http_send(fd, "application/json", "{\"error\":\"missing params\"}");
    } else if (!strcmp(path, "/api/mode")) {
        char *mode = post_val(body, "mode");
        if (mode) {
            if (!strcmp(mode, "auto")) {
                remove(MODE_FILE);
                g_ctx.forced = MODE_AUTO;
            } else {
                write_file(MODE_FILE, mode);
                g_ctx.forced = str_to_mode(mode);
            }
            http_send(fd, "application/json", "{\"status\":\"ok\"}");
        } else http_send(fd, "application/json", "{\"error\":\"missing mode\"}");
    } else if (!strcmp(path, "/api/rescan")) {
        scan_apps();
        http_send(fd, "application/json", "{\"status\":\"ok\",\"count\":}");
    } else {
        http_send(fd, "application/json", "{\"error\":\"not found\"}");
    }
}

static void http_handle(int fd) {
    char buf[HTTP_BUF];
    int n = read(fd, buf, sizeof(buf) - 1);
    char method[16], raw_path[512], *body, *query, *path;
    char callback[128];
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    memset(method, 0, sizeof(method));
    memset(raw_path, 0, sizeof(raw_path));
    sscanf(buf, "%15s %511s", method, raw_path);
    body = strstr(buf, "\r\n\r\n");
    if (body) body += 4; else body = "";

    /* 分离 path 和 query string */
    path = raw_path;
    query = strchr(raw_path, '?');
    if (query) { *query = '\0'; query++; } else query = "";

    /* 提取 JSONP callback */
    callback[0] = '\0';
    {
        char *cb = url_param(query, "callback");
        if (cb && strlen(cb) < sizeof(callback) - 1) {
            strncpy(callback, cb, sizeof(callback) - 1);
            callback[sizeof(callback) - 1] = '\0';
            url_decode(callback);
        }
    }

    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
            static char *webui_paths[] = {
                "/data/adb/modules/dynamic_touch_sampling/webroot/index.html",
                "/data/adb/dynamic_touch_sampling/webroot/index.html",
                NULL
            };
            char *html = malloc(65536);
            int hn = -1, wi;
            if (html) {
                for (wi = 0; webui_paths[wi] && hn < 0; wi++)
                    hn = read_file(webui_paths[wi], html, 65535);
                if (hn > 0) { html[hn] = '\0'; http_send(fd, "text/html; charset=utf-8", html); }
                else http_send(fd, "text/html; charset=utf-8",
                    "<html><body style='background:#0a0c10;color:#fff;font-family:sans-serif;padding:40px;text-align:center'>"
                    "<h2>Touch Control</h2><p>WebUI not found</p></body></html>");
                free(html);
            } else http_send(fd, "text/html", "OOM");
        }
        else if (!strcmp(path, "/api/status")) {
            char jbuf[2048];
            int uptime = (int)(time(NULL) - g_ctx.start_time);
            snprintf(jbuf, sizeof(jbuf),
                "{\"status\":\"ok\",\"current\":\"%s\",\"current_rate\":%d,\"forced\":\"%s\","
                "\"foreground_app\":\"%s\",\"screen_on\":%d,\"battery\":%d,\"battery_low\":%d,"
                "\"poll_interval\":%d,\"poll_idle\":%d,\"rate_low\":%d,\"rate_med\":%d,\"rate_high\":%d,"
                "\"node_count\":%d,\"profile_count\":%d,\"app_count\":%d,\"switch_count\":%d,"
                "\"uptime\":%d,\"max_rate\":%d,\"debug\":%d}",
                mode_to_str(g_ctx.current), mode_to_rate(g_ctx.current), mode_to_str(g_ctx.forced),
                g_ctx.cur_app, g_ctx.screen_on, g_ctx.battery, g_ctx.battery_low,
                g_ctx.poll_interval, g_ctx.poll_idle, g_ctx.rate_low, g_ctx.rate_med, g_ctx.rate_high,
                g_ctx.node_count, g_ctx.profile_count, g_ctx.app_count, g_ctx.switch_count,
                uptime, MAX_RATE, g_ctx.debug);
            http_send_json(fd, jbuf, callback);
        }
        else if (!strcmp(path, "/api/apps")) {
            char *abuf = malloc(65536);
            if (abuf) {
                int pos = 0, i;
                pos += snprintf(abuf + pos, 65536 - pos, "{\"apps\":[");
                for (i = 0; i < g_ctx.app_count; i++) {
                    if (i > 0) pos += snprintf(abuf + pos, 65536 - pos, ",");
                    pos += snprintf(abuf + pos, 65536 - pos, "{\"pkg\":\"%s\",\"name\":\"%s\",\"type\":%d}",
                            g_ctx.apps[i].pkg, g_ctx.apps[i].name, g_ctx.apps[i].type);
                    if (pos > 64000) break;
                }
                pos += snprintf(abuf + pos, 65536 - pos, "],\"count\":%d}", g_ctx.app_count);
                http_send_json(fd, abuf, callback);
                free(abuf);
            } else http_send_json(fd, "{\"error\":\"oom\"}", callback);
        }
        else if (!strcmp(path, "/api/profiles")) {
            /* GET 支持操作: ?action=add/remove/update&pkg=xxx&mode=xxx */
            char *action = url_param(query, "action");
            char *pkg = url_param(query, "pkg");
            char *mode = url_param(query, "mode");
            char pbuf[8192];
            int pos = 0, i;
            if (action && pkg) {
                url_decode(pkg);
                if (mode) url_decode(mode);
                if (!strcmp(action, "add") && mode && g_ctx.profile_count < MAX_PROFILES) {
                    strncpy(g_ctx.profiles[g_ctx.profile_count].pkg, pkg, MAX_PKG - 1);
                    g_ctx.profiles[g_ctx.profile_count].pkg[MAX_PKG - 1] = '\0';
                    g_ctx.profiles[g_ctx.profile_count].mode = str_to_mode(mode);
                    g_ctx.profile_count++;
                    save_profiles();
                } else if (!strcmp(action, "remove")) {
                    for (i = 0; i < g_ctx.profile_count; i++) {
                        if (!strcmp(g_ctx.profiles[i].pkg, pkg)) {
                            int j;
                            for (j = i; j < g_ctx.profile_count - 1; j++)
                                memcpy(&g_ctx.profiles[j], &g_ctx.profiles[j+1], sizeof(AppProfile));
                            g_ctx.profile_count--;
                            save_profiles();
                            break;
                        }
                    }
                } else if (!strcmp(action, "update") && mode) {
                    for (i = 0; i < g_ctx.profile_count; i++) {
                        if (!strcmp(g_ctx.profiles[i].pkg, pkg)) {
                            g_ctx.profiles[i].mode = str_to_mode(mode);
                            save_profiles();
                            break;
                        }
                    }
                }
            }
            pos += snprintf(pbuf + pos, sizeof(pbuf) - pos, "{\"profiles\":[");
            for (i = 0; i < g_ctx.profile_count; i++) {
                if (i > 0) pos += snprintf(pbuf + pos, sizeof(pbuf) - pos, ",");
                pos += snprintf(pbuf + pos, sizeof(pbuf) - pos,
                    "{\"pkg\":\"%s\",\"mode\":\"%s\"}", g_ctx.profiles[i].pkg, mode_to_str(g_ctx.profiles[i].mode));
            }
            pos += snprintf(pbuf + pos, sizeof(pbuf) - pos, "],\"count\":%d}", g_ctx.profile_count);
            http_send_json(fd, pbuf, callback);
        }
        else if (!strcmp(path, "/api/config")) {
            /* GET 支持保存: ?rate_low=xxx&rate_med=xxx... */
            char *v;
            if ((v = url_param(query, "poll_interval"))) g_ctx.poll_interval = atoi(v);
            if ((v = url_param(query, "poll_idle"))) g_ctx.poll_idle = atoi(v);
            if ((v = url_param(query, "battery_low"))) g_ctx.battery_low = atoi(v);
            if ((v = url_param(query, "debug"))) g_ctx.debug = atoi(v);
            if ((v = url_param(query, "rate_low"))) g_ctx.rate_low = clamp_rate(atoi(v));
            if ((v = url_param(query, "rate_med"))) g_ctx.rate_med = clamp_rate(atoi(v));
            if ((v = url_param(query, "rate_high"))) g_ctx.rate_high = clamp_rate(atoi(v));
            if (strlen(query) > 0) save_config();
            {
                char cbuf[1024];
                snprintf(cbuf, sizeof(cbuf),
                    "{\"poll_interval\":%d,\"poll_idle\":%d,\"battery_low\":%d,\"debug\":%d,"
                    "\"rate_low\":%d,\"rate_med\":%d,\"rate_high\":%d,\"max_rate\":%d,\"status\":\"ok\"}",
                    g_ctx.poll_interval, g_ctx.poll_idle, g_ctx.battery_low, g_ctx.debug,
                    g_ctx.rate_low, g_ctx.rate_med, g_ctx.rate_high, MAX_RATE);
                http_send_json(fd, cbuf, callback);
            }
        }
        else if (!strcmp(path, "/api/mode")) {
            char *mode = url_param(query, "mode");
            if (mode) {
                url_decode(mode);
                if (!strcmp(mode, "auto")) { remove(MODE_FILE); g_ctx.forced = MODE_AUTO; }
                else { write_file(MODE_FILE, mode); g_ctx.forced = str_to_mode(mode); }
            }
            http_send_json(fd, "{\"status\":\"ok\"}", callback);
        }
        else if (!strcmp(path, "/api/rescan")) {
            scan_apps();
            {
                char rbuf[64];
                snprintf(rbuf, sizeof(rbuf), "{\"status\":\"ok\",\"count\":%d}", g_ctx.app_count);
                http_send_json(fd, rbuf, callback);
            }
        }
        else if (!strcmp(path, "/api/log")) {
            char logbuf[16384];
            int ln = read_file(LOG_FILE, logbuf, sizeof(logbuf));
            if (ln > 0) {
                logbuf[ln] = '\0';
                if (callback[0]) {
                    /* JSONP 不能直接返回纯文本, 包装成 JSON */
                    char *ljson = malloc(32768);
                    if (ljson) {
                        snprintf(ljson, 32768, "{\"log\":\"%s\"}", logbuf);
                        http_send_json(fd, ljson, callback);
                        free(ljson);
                    } else http_send(fd, "text/plain", logbuf);
                } else http_send(fd, "text/plain", logbuf);
            } else http_send_json(fd, "{\"log\":\"no log\"}", callback);
        }
        else http_send_json(fd, "{\"error\":\"not found\"}", callback);
    } else if (!strcmp(method, "POST")) {
        http_handle_post(fd, path, body);
    } else {
        http_send_json(fd, "{\"error\":\"method not allowed\"}", callback);
    }
    close(fd);
}

static void http_poll(void) {
    int client;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (g_ctx.http_fd < 0) return;
    while ((client = accept(g_ctx.http_fd, (struct sockaddr *)&addr, &len)) >= 0) {
        http_handle(client);
    }
}

/* ============ 主循环 ============ */

static void run_daemon(void) {
    TouchMode target;
    int interval;
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGHUP, SIG_IGN);

    g_ctx.start_time = time(NULL);
    g_ctx.switch_count = 0;

    if (load_config() < 0) log_msg("no config, will auto-detect");
    if (g_ctx.node_count == 0) {
        if (auto_detect_nodes() == 0)
            log_msg("WARN: no touch nodes detected, configure manually");
        else
            save_config();
    }
    load_profiles();
    scan_apps();

    g_ctx.http_fd = http_init();
    if (g_ctx.http_fd >= 0)
        log_msg("HTTP API listening on 127.0.0.1:%d", HTTP_PORT);
    else
        log_msg("WARN: HTTP init failed (port %d)", HTTP_PORT);

    g_ctx.current = MODE_AUTO;
    g_ctx.forced = MODE_AUTO;

    write_pid();
    log_msg("touchd started: nodes=%d profiles=%d apps=%d poll=%ds idle=%ds (Android 17)",
            g_ctx.node_count, g_ctx.profile_count, g_ctx.app_count,
            g_ctx.poll_interval, g_ctx.poll_idle);

    while (g_running) {
        http_poll();

        {
            char mbuf[16];
            if (read_file(MODE_FILE, mbuf, sizeof(mbuf)) > 0) {
                trim(mbuf);
                g_ctx.forced = str_to_mode(mbuf);
            } else g_ctx.forced = MODE_AUTO;
        }

        g_ctx.screen_on = is_screen_on();
        g_ctx.battery = get_battery_level();

        if (g_ctx.screen_on && g_ctx.battery > g_ctx.battery_low) {
            char *app = get_foreground_app();
            if (app && strcmp(app, g_ctx.cur_app)) {
                strncpy(g_ctx.cur_app, app, MAX_PKG - 1);
                g_ctx.cur_app[MAX_PKG - 1] = '\0';
                log_msg("foreground: %s", g_ctx.cur_app);
            }
        }

        target = decide_mode();
        if (target != g_ctx.current) apply_mode(target);

        interval = g_ctx.screen_on ? g_ctx.poll_interval : g_ctx.poll_idle;
        {
            int slept = 0;
            while (slept < interval && g_running) {
                sleep(1);
                slept++;
                if (slept % 2 == 0) http_poll();
            }
        }
    }

    if (g_ctx.http_fd >= 0) close(g_ctx.http_fd);
    apply_mode(MODE_MEDIUM);
    unlink(PID_FILE);
    unlink(MODE_FILE);
    log_msg("touchd stopped");
}

static void print_status(void) {
    printf("=== Dynamic Touch Sampling Daemon (Android 17) ===\n");
    printf("Current: %s (%dHz)\n", mode_to_str(g_ctx.current), mode_to_rate(g_ctx.current));
    printf("Forced:  %s\n", mode_to_str(g_ctx.forced));
    printf("App:     %s\n", g_ctx.cur_app);
    printf("Screen:  %s\n", g_ctx.screen_on ? "on" : "off");
    printf("Battery: %d%% (low threshold: %d%%)\n", g_ctx.battery, g_ctx.battery_low);
    printf("Poll:    %ds (idle: %ds)\n", g_ctx.poll_interval, g_ctx.poll_idle);
    printf("Rates:   low=%d med=%d high=%d (max %d)\n",
           g_ctx.rate_low, g_ctx.rate_med, g_ctx.rate_high, MAX_RATE);
    printf("Nodes:   %d, Profiles: %d, Apps: %d\n", g_ctx.node_count, g_ctx.profile_count, g_ctx.app_count);
    printf("API:     http://127.0.0.1:%d\n", HTTP_PORT);
    if (g_ctx.node_count) {
        int i;
        printf("\nTouch nodes:\n");
        for (i = 0; i < g_ctx.node_count; i++)
            printf("  [%d] %s\n      low=%s med=%s high=%s\n",
                   i, g_ctx.nodes[i].path, g_ctx.nodes[i].val_low,
                   g_ctx.nodes[i].val_med, g_ctx.nodes[i].val_high);
    }
    printf("\nConfig:  %s\n", CONFIG_FILE);
    printf("Profiles: %s\n", PROFILES_FILE);
    printf("Log:     %s\n", LOG_FILE);
}

static void print_usage(const char *p) {
    printf("Usage: %s [options]\n", p);
    printf("  -d, --daemon   Run as background daemon\n");
    printf("  -s, --status   Show current status\n");
    printf("  -m, --mode <m> Set mode (low/medium/high/auto)\n");
    printf("  -l, --log      Show log (tail -f)\n");
    printf("  -k, --kill     Stop daemon\n");
    printf("  -h, --help     Show this help\n");
}

static int is_running(void) {
    char buf[16];
    pid_t pid;
    if (read_file(PID_FILE, buf, sizeof(buf)) <= 0) return 0;
    pid = atoi(buf);
    return pid > 0 && kill(pid, 0) == 0;
}

int main(int argc, char *argv[]) {
    mkdir(RUNTIME_DIR, 0755);
    mkdir(RUNTIME_DIR "/log", 0755);

    if (argc < 2) { print_usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "-d") || !strcmp(argv[1], "--daemon")) {
        if (is_running()) { fprintf(stderr, "already running\n"); return 1; }
        daemonize();
        run_daemon();
        return 0;
    }
    if (!strcmp(argv[1], "-s") || !strcmp(argv[1], "--status")) {
        load_config(); load_profiles();
        g_ctx.screen_on = is_screen_on();
        g_ctx.battery = get_battery_level();
        print_status();
        return 0;
    }
    if (!strcmp(argv[1], "-m") || !strcmp(argv[1], "--mode")) {
        const char *m = (argc > 2) ? argv[2] : "auto";
        if (!strcmp(m, "auto")) remove(MODE_FILE);
        else write_file(MODE_FILE, m);
        printf("mode set: %s\n", m);
        return 0;
    }
    if (!strcmp(argv[1], "-k") || !strcmp(argv[1], "--kill")) {
        char buf[16];
        if (read_file(PID_FILE, buf, sizeof(buf)) > 0) {
            pid_t pid = atoi(buf);
            if (pid > 0) { kill(pid, SIGTERM); unlink(MODE_FILE); printf("stopped PID %d\n", pid); return 0; }
        }
        fprintf(stderr, "not running\n"); return 1;
    }
    if (!strcmp(argv[1], "-l") || !strcmp(argv[1], "--log")) {
        execlp("tail", "tail", "-n", "100", "-f", LOG_FILE, NULL);
        return 0;
    }
    print_usage(argv[0]);
    return 1;
}
