/**
 * @file    sys_info.h
 * @brief   系统资源采集线程（CPU、内存）
 */
#ifndef SYS_INFO_H
#define SYS_INFO_H

#include "common.h"
#include <pthread.h>

typedef struct {
    int cpu_usage;  /* CPU 使用率 % */
    int mem_usage;  /* 内存使用率 % */
} sys_info_t;

void sys_info_init(void);
void sys_info_get(sys_info_t *info);

int  sys_info_start(pthread_t *tid, const app_config_t *cfg);
int  sys_info_stop(void);
void *sys_info_thread(void *arg);

#endif /* SYS_INFO_H */
