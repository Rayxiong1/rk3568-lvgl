/**
 * @file    business.h
 * @brief   业务处理线程：从消息总线接收消息，维护设备链表、日志和配置
 */
#ifndef BUSINESS_H
#define BUSINESS_H

#include "common.h"
#include <pthread.h>

int  business_start(pthread_t *tid, const app_config_t *cfg);
int  business_stop(void);
void *business_thread(void *arg);

#endif /* BUSINESS_H */
