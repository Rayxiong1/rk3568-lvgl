/**
 * @file    file_ops.h
 * @brief   文件 IO 持久化模块：配置读写、日志写入
 *
 * 功能：
 *  - config.conf：保存运行参数、告警阈值
 *  - history.log：记录设备上下线等事件日志
 *  - 多线程写日志内部加锁，防止日志错乱
 *  - clear_log_file()：清空日志文件
 */
#ifndef FILE_OPS_H
#define FILE_OPS_H

#include "common.h"

#define DEFAULT_CONFIG_PATH "config/config.conf"
#define DEFAULT_LOG_PATH    "log/history.log"

/* 设置默认配置 */
void set_default_config(app_config_t *cfg);

/*
 * 启动时加载 config.conf。
 * 如果文件不存在，会创建默认配置文件。
 * 成功返回 0，失败返回 -1。
 */
int load_config(app_config_t *cfg);

/*
 * 把当前配置写入 config.conf。
 * 成功返回 0，失败返回 -1。
 */
int save_config(const app_config_t *cfg);

/*
 * 追加写日志到 history.log，带时间戳。
 * 内部加互斥锁，多线程调用安全。
 * 成功返回 0，失败返回 -1。
 */
int write_log(const char *level, const char *fmt, ...);

/*
 * 清空 history.log。
 * 内部加互斥锁，避免与 write_log 冲突。
 * 成功返回 0，失败返回 -1。
 */
int clear_log_file(void);

#endif /* FILE_OPS_H */