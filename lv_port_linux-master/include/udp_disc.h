/**
 * @file    udp_disc.h
 * @brief   UDP 局域网设备发现线程
 */
#ifndef UDP_DISC_H
#define UDP_DISC_H

#include "common.h"
#include <pthread.h>

int  udp_disc_start(pthread_t *tid, const app_config_t *cfg);
int  udp_disc_stop(void);
void *udp_discovery_thread(void *arg);

#endif /* UDP_DISC_H */