/**
 * @file    test_file_ops.c
 * @brief   单元测试：文件 IO 持久化模块
 *
 * 测试内容：
 *  - save_config 写入配置（含告警阈值）
 *  - load_config 读回配置（含告警阈值）
 *  - 配置文件不存在时自动创建默认配置
 *  - write_log 追加写日志并带时间戳
 *  - clear_log_file 清空日志文件
 */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "file_ops.h"

static int check_file_contains(const char *path, const char *needle)
{
    FILE *fp;
    char buf[512];
    int found = 0;

    fp = fopen(path, "r");
    if (fp == NULL) return 0;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strstr(buf, needle) != NULL) {
            found = 1;
            break;
        }
    }

    fclose(fp);
    return found;
}

int main(void)
{
    app_config_t cfg;
    app_config_t loaded;

    printf("===== file_ops test start =====\n");

    /* 1. 保存配置 */
    set_default_config(&cfg);
    cfg.http_port             = 9090;
    cfg.udp_port              = 7000;
    cfg.discovery_interval_ms = 5000;
    cfg.device_timeout_ms     = 15000;
    cfg.alarm_threshold       = 95;

    if (save_config(&cfg) != 0) {
        printf("[FAIL] save_config failed\n");
        return -1;
    }
    if (!check_file_contains(cfg.config_file, "http_port=9090")) {
        printf("[FAIL] config file content wrong\n");
        return -1;
    }
    if (!check_file_contains(cfg.config_file, "alarm_threshold=95")) {
        printf("[FAIL] config file missing alarm_threshold\n");
        return -1;
    }
    printf("[PASS] save_config writes config file\n");

    /* 2. 加载配置 */
    memset(&loaded, 0, sizeof(loaded));
    if (load_config(&loaded) != 0) {
        printf("[FAIL] load_config failed\n");
        return -1;
    }
    if (loaded.http_port != 9090 || loaded.udp_port != 7000 ||
        loaded.discovery_interval_ms != 5000 || loaded.device_timeout_ms != 15000 ||
        loaded.alarm_threshold != 95) {
        printf("[FAIL] load_config read back wrong values\n");
        return -1;
    }
    printf("[PASS] load_config reads config file\n");

    /* 3. 配置文件不存在时自动创建默认配置 */
    {
        const char *cfg_path = DEFAULT_CONFIG_PATH;
        const char *bak_path = "config/config.conf.bak";
        FILE *fp = fopen(cfg_path, "r");
        int has_backup = 0;
        int ret = 0;

        if (fp != NULL) {
            fclose(fp);
            rename(cfg_path, bak_path);
            has_backup = 1;
        }

        set_default_config(&cfg);
        if (load_config(&cfg) != 0) {
            printf("[FAIL] load_config should create default config\n");
            ret = -1;
        } else if (!check_file_contains(cfg_path, "http_port=8080")) {
            printf("[FAIL] default config file not created correctly\n");
            ret = -1;
        } else {
            printf("[PASS] load_config creates default config when missing\n");
        }

        remove(cfg_path);
        if (has_backup) {
            rename(bak_path, cfg_path);
        }

        if (ret != 0) {
            return -1;
        }
    }

    /* 4. 写日志 */
    if (write_log("INFO", "test log %d", 123) != 0) {
        printf("[FAIL] write_log failed\n");
        return -1;
    }
    if (!check_file_contains("log/history.log", "test log 123")) {
        printf("[FAIL] history.log content wrong\n");
        return -1;
    }
    printf("[PASS] write_log appends timestamped log\n");

    /* 5. 清空日志 */
    if (clear_log_file() != 0) {
        printf("[FAIL] clear_log_file failed\n");
        return -1;
    }
    {
        FILE *fp = fopen("log/history.log", "r");
        int ch;

        if (fp == NULL) {
            printf("[FAIL] history.log should exist after clear\n");
            return -1;
        }
        ch = fgetc(fp);
        fclose(fp);
        if (ch != EOF) {
            printf("[FAIL] history.log not empty after clear\n");
            return -1;
        }
    }
    printf("[PASS] clear_log_file empties history.log\n");

    printf("===== file_ops test end =====\n");
    return 0;
}
