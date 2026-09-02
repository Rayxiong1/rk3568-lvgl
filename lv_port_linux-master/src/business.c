/**
 * @file    business.c
 * @brief   业务处理线程
 *
 * 从消息总线接收消息并执行设备操作：
 *  - 上线/下线/开关设备
 *  - 手动新增/删除设备
 *  - 修改设备负载和名称
 *  - 清空日志
 *  - 设置告警阈值
 */
#include "business.h"

#include "device_list.h"
#include "file_ops.h"
#include "msg_bus.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static volatile int g_stop = 0;
static msg_bus_t g_mq_biz = NULL;
static app_config_t g_cfg;

/* 启动业务线程，并创建业务消息队列 */
int business_start(pthread_t *tid, const app_config_t *cfg)
{
    if (tid == NULL || cfg == NULL) return -1;
    g_cfg = *cfg;

    g_stop = 0;

    /* 在启动线程前创建业务消息队列，确保其他线程可以 open */
    if (msg_bus_create(MSG_QUEUE_BIZ_NAME, &g_mq_biz) != 0) {
        fprintf(stderr, "[business] create biz mq failed\n");
        return -1;
    }

    if (pthread_create(tid, NULL, business_thread, NULL) != 0) {
        msg_bus_close(MSG_QUEUE_BIZ_NAME, &g_mq_biz);
        return -1;
    }

    return 0;
}

/* 请求停止业务线程 */
int business_stop(void)
{
    g_stop = 1;
    return 0;
}

/* 业务线程主循环：从业务队列接收消息并执行对应操作 */
void *business_thread(void *arg)
{
    (void)arg;

    while (!g_stop) {
        msg_t msg;

        if (msg_bus_recv_timeout(g_mq_biz, &msg, 200) != 0) {
            continue;
        }

        switch (msg.type) {
        case MSG_DEV_ONLINE: {
            device_t dev;
            device_from_info(&dev, &msg.dev);

            if (device_add(&dev) == 0) {
                write_log("INFO", "device online: %s (%s)", dev.id, dev.ip);
                printf("[business] online: %s (%s)\n", dev.id, dev.ip);
            }
            break;
        }
        case MSG_DEV_OFFLINE:
            if (device_del_by_id(msg.dev.id) != 0) {
                write_log("WARN", "device offline: %s", msg.dev.id);
                printf("[business] offline: %s\n", msg.dev.id);
            }
            break;
        case MSG_DEV_CTRL: {
            int state = 0;

            if (strcmp(msg.cmd, "on") == 0) {
                state = 1;
            } else if (strcmp(msg.cmd, "off") == 0) {
                state = 0;
            } else {
                break;
            }

            if (device_set_state(msg.dev.id, state) == 1) {
                write_log("INFO", "control %s -> %s", msg.dev.id, msg.cmd);
                printf("[business] control %s -> %s\n", msg.dev.id, msg.cmd);
            }
            break;
        }
        case MSG_DEV_ADD_MANUAL: {
            device_t dev;
            device_from_info(&dev, &msg.dev);

            if (device_add(&dev) == 0) {
                write_log("INFO", "manual add: %s (%s)", dev.id, dev.dev_name);
                printf("[business] manual add: %s (%s)\n", dev.id, dev.dev_name);
                save_config(&g_cfg);
            }
            break;
        }
        case MSG_DEV_DEL_MANUAL:
            if (device_del_by_id(msg.dev.id) != 0) {
                write_log("WARN", "manual delete: %s", msg.dev.id);
                printf("[business] manual delete: %s\n", msg.dev.id);
                save_config(&g_cfg);
            }
            break;
        case MSG_DEV_MODIFY_PARAM:
            if (device_modify_param(msg.dev.id, msg.new_load, msg.new_name) == 1) {
                write_log("INFO", "modify %s load=%d name=%s",
                          msg.dev.id, msg.new_load, msg.new_name);
                printf("[business] modify %s load=%d name=%s\n",
                       msg.dev.id, msg.new_load, msg.new_name);
                save_config(&g_cfg);
            }
            break;
        case MSG_LOG_CLEAR:
            if (clear_log_file() == 0) {
                write_log("INFO", "日志已清空");
                printf("[business] log cleared\n");
            }
            break;
        case MSG_SET_ALARM_THRESH:
            g_cfg.alarm_threshold = msg.alarm_threshold;
            if (save_config(&g_cfg) == 0) {
                write_log("INFO", "alarm threshold set to %d", msg.alarm_threshold);
                printf("[business] alarm threshold set to %d\n", msg.alarm_threshold);
            }
            break;
        case MSG_QUIT:
            g_stop = 1;
            break;
        default:
            break;
        }
    }

    msg_bus_close(MSG_QUEUE_BIZ_NAME, &g_mq_biz);
    return NULL;
}
