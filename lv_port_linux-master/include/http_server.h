/**
 * @file    http_server.h
 * @brief   简易 HTTP 服务线程
 */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "common.h"
#include <pthread.h>

int  http_server_start(pthread_t *tid, const app_config_t *cfg);
int  http_server_stop(void);
void *http_server_thread(void *arg);

#endif /* HTTP_SERVER_H */
