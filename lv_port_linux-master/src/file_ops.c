/**
 * @file    file_ops.c
 * @brief   配置文件读写与日志实现
 *
 * 功能：
 *  - 读取和保存 config.conf
 *  - 支持告警阈值、模拟设备默认参数等配置
 *  - 提供日志写入和清空功能
 *  - 配置读写和日志操作均加锁，保证多线程安全
 */
#include "file_ops.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char g_log_path[PATH_LEN] = DEFAULT_LOG_PATH;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_config_lock = PTHREAD_MUTEX_INITIALIZER;

/* 设置默认运行配置 */
void set_default_config(app_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));

    cfg->http_port             = 8080;
    cfg->udp_port              = 6000;
    cfg->discovery_interval_ms = 3000;
    cfg->device_timeout_ms     = 10000;
    cfg->alarm_threshold       = 80;
    cfg->default_load          = 30;
    snprintf(cfg->default_dev_name, sizeof(cfg->default_dev_name), "VirtualDevice");

    snprintf(cfg->log_file,    sizeof(cfg->log_file),    DEFAULT_LOG_PATH);
    snprintf(cfg->config_file, sizeof(cfg->config_file), DEFAULT_CONFIG_PATH);
    snprintf(g_log_path, sizeof(g_log_path), "%s", cfg->log_file);
}

/* 去除字符串首尾空白字符 */
static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* 从配置文件加载配置，文件不存在时自动创建默认配置 */
int load_config(app_config_t *cfg)
{
    FILE *fp;
    char line[256];

    if (cfg == NULL) return -1;

    pthread_mutex_lock(&g_config_lock);

    set_default_config(cfg);

    fp = fopen(cfg->config_file, "r");
    if (fp == NULL) {
        if (errno == ENOENT) {
            pthread_mutex_unlock(&g_config_lock);
            return save_config(cfg);
        }
        pthread_mutex_unlock(&g_config_lock);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key, *value, *eq;

        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';') continue;

        eq = strchr(key, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);

        if (strcmp(key, "http_port") == 0) {
            cfg->http_port = atoi(value);
        } else if (strcmp(key, "udp_port") == 0) {
            cfg->udp_port = atoi(value);
        } else if (strcmp(key, "discovery_interval_ms") == 0) {
            cfg->discovery_interval_ms = atoi(value);
        } else if (strcmp(key, "device_timeout_ms") == 0) {
            cfg->device_timeout_ms = atoi(value);
        } else if (strcmp(key, "alarm_threshold") == 0) {
            cfg->alarm_threshold = atoi(value);
        } else if (strcmp(key, "default_load") == 0) {
            cfg->default_load = atoi(value);
        } else if (strcmp(key, "default_dev_name") == 0) {
            snprintf(cfg->default_dev_name, sizeof(cfg->default_dev_name), "%s", value);
        } else if (strcmp(key, "log_file") == 0) {
            snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", value);
        } else if (strcmp(key, "config_file") == 0) {
            snprintf(cfg->config_file, sizeof(cfg->config_file), "%s", value);
        }
    }

    if (fclose(fp) != 0) {
        pthread_mutex_unlock(&g_config_lock);
        return -1;
    }

    snprintf(g_log_path, sizeof(g_log_path), "%s", cfg->log_file);
    pthread_mutex_unlock(&g_config_lock);
    return 0;
}

/* 保存当前配置到配置文件 */
int save_config(const app_config_t *cfg)
{
    FILE *fp;
    const char *path;
    int ret = 0;

    if (cfg == NULL) return -1;

    pthread_mutex_lock(&g_config_lock);

    path = (cfg->config_file[0] != '\0') ? cfg->config_file : DEFAULT_CONFIG_PATH;

    fp = fopen(path, "w");
    if (fp == NULL) {
        pthread_mutex_unlock(&g_config_lock);
        return -1;
    }

    if (fprintf(fp, "# Gateway config\n") < 0) ret = -1;
    if (fprintf(fp, "http_port=%d\n",             cfg->http_port) < 0) ret = -1;
    if (fprintf(fp, "udp_port=%d\n",              cfg->udp_port) < 0) ret = -1;
    if (fprintf(fp, "discovery_interval_ms=%d\n", cfg->discovery_interval_ms) < 0) ret = -1;
    if (fprintf(fp, "device_timeout_ms=%d\n",     cfg->device_timeout_ms) < 0) ret = -1;
    if (fprintf(fp, "alarm_threshold=%d\n",       cfg->alarm_threshold) < 0) ret = -1;
    if (fprintf(fp, "default_load=%d\n",          cfg->default_load) < 0) ret = -1;
    if (fprintf(fp, "default_dev_name=%s\n",      cfg->default_dev_name) < 0) ret = -1;
    if (fprintf(fp, "log_file=%s\n",              cfg->log_file) < 0) ret = -1;
    if (fprintf(fp, "config_file=%s\n",           cfg->config_file) < 0) ret = -1;

    if (fclose(fp) != 0) ret = -1;

    if (ret == 0) {
        snprintf(g_log_path, sizeof(g_log_path), "%s", cfg->log_file);
    }

    pthread_mutex_unlock(&g_config_lock);
    return ret;
}

/* 向日志文件追加一条带时间戳的日志 */
int write_log(const char *level, const char *fmt, ...)
{
    FILE *fp;
    char msg[512];
    char ts[64];
    time_t now;
    struct tm tm_buf;
    va_list ap;
    int ret;

    if (level == NULL || fmt == NULL) return -1;

    pthread_mutex_lock(&g_log_lock);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    now = time(NULL);
#if defined(_WIN32)
    if (localtime_s(&tm_buf, &now) == 0) {
#else
    if (localtime_r(&now, &tm_buf) != NULL) {
#endif
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    } else {
        snprintf(ts, sizeof(ts), "1970-01-01 00:00:00");
    }

    fp = fopen(g_log_path, "a");
    if (fp == NULL) {
        pthread_mutex_unlock(&g_log_lock);
        return -1;
    }

    ret = fprintf(fp, "%s [%s] %s\n", ts, level, msg);
    if (fclose(fp) != 0) {
        ret = -1;
    }

    pthread_mutex_unlock(&g_log_lock);
    return (ret < 0) ? -1 : 0;
}

/* 清空日志文件内容 */
int clear_log_file(void)
{
    FILE *fp;

    pthread_mutex_lock(&g_log_lock);

    fp = fopen(g_log_path, "w");
    if (fp == NULL) {
        pthread_mutex_unlock(&g_log_lock);
        return -1;
    }

    if (fclose(fp) != 0) {
        pthread_mutex_unlock(&g_log_lock);
        return -1;
    }

    pthread_mutex_unlock(&g_log_lock);
    return 0;
}