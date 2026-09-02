/**
 * @file    test_business.c
 * @brief   业务模块自测：直接向业务线程消息队列发送消息，验证设备操作业务逻辑
 *
 * 验证点：
 *  - 手动新增设备 -> 链表增加节点
 *  - 修改设备负载/名称 -> 链表节点更新
 *  - 手动删除设备 -> 链表节点移除
 *  - 清空日志 -> history 日志被清空并写入“日志已清空”
 *  - 设置告警阈值 -> config.conf 更新
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "business.h"
#include "device_list.h"
#include "file_ops.h"
#include "msg_bus.h"

#define TEST_CFG "config/test_business.conf"
#define TEST_LOG "log/test_business.log"
#define TEST_DEV_ID "test-dev-001"

static int file_contains(const char *path, const char *needle)
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

static int wait_device(const char *id, device_t *out, int want_found)
{
    int i;

    for (i = 0; i < 100; i++) {
        int ret = device_find_by_id(id, out);

        if (want_found ? (ret == 1) : (ret == 0)) {
            return 0;
        }
        usleep(20000);
    }

    return -1;
}

static int wait_device_param(const char *id, int load, const char *name, device_t *out)
{
    int i;

    for (i = 0; i < 100; i++) {
        if (device_find_by_id(id, out) == 1 &&
            out->load == load &&
            strcmp(out->dev_name, name) == 0) {
            return 0;
        }
        usleep(20000);
    }

    return -1;
}

static int wait_file_contains(const char *path, const char *needle, int want_found)
{
    int i;

    for (i = 0; i < 100; i++) {
        int found = file_contains(path, needle);

        if (want_found ? found : !found) {
            return 0;
        }
        usleep(20000);
    }

    return -1;
}

int main(void)
{
    app_config_t cfg;
    pthread_t tid = 0;
    msg_bus_t mq = NULL;
    msg_t msg;
    device_t out;
    int ret = 0;

    printf("===== business test start =====\n");

    /* 使用临时配置/日志文件，避免污染正式 config.conf 和 history.log */
    set_default_config(&cfg);
    snprintf(cfg.config_file, sizeof(cfg.config_file), "%s", TEST_CFG);
    snprintf(cfg.log_file,    sizeof(cfg.log_file),    "%s", TEST_LOG);

    if (save_config(&cfg) != 0) {
        printf("[FAIL] prepare temp config failed\n");
        return -1;
    }

    device_list_init();

    if (business_start(&tid, &cfg) != 0) {
        printf("[FAIL] business_start failed\n");
        device_list_destroy();
        remove(TEST_CFG);
        remove(TEST_LOG);
        return -1;
    }

    if (msg_bus_open(MSG_QUEUE_BIZ_NAME, &mq) != 0) {
        printf("[FAIL] open biz mq failed\n");
        business_stop();
        pthread_join(tid, NULL);
        device_list_destroy();
        remove(TEST_CFG);
        remove(TEST_LOG);
        return -1;
    }

    /* 1. 手动新增设备 */
    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_ADD_MANUAL;
    snprintf(msg.dev.id,       sizeof(msg.dev.id),       "%s", TEST_DEV_ID);
    snprintf(msg.dev.ip,       sizeof(msg.dev.ip),       "192.168.99.10");
    snprintf(msg.dev.dev_name, sizeof(msg.dev.dev_name), "TestDev");
    msg.dev.state = 1;
    msg.dev.load  = 42;

    if (msg_bus_send(mq, &msg) != 0) {
        printf("[FAIL] send MSG_DEV_ADD_MANUAL failed\n");
        ret = -1;
        goto cleanup;
    }
    if (wait_device(TEST_DEV_ID, &out, 1) != 0) {
        printf("[FAIL] device not added after MSG_DEV_ADD_MANUAL\n");
        ret = -1;
        goto cleanup;
    }
    if (out.load != 42 || strcmp(out.dev_name, "TestDev") != 0) {
        printf("[FAIL] added device fields wrong: load=%d name=%s\n",
               out.load, out.dev_name);
        ret = -1;
        goto cleanup;
    }
    printf("[PASS] MSG_DEV_ADD_MANUAL adds device\n");

    /* 2. 修改设备参数 */
    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_MODIFY_PARAM;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", TEST_DEV_ID);
    msg.new_load = 77;
    snprintf(msg.new_name, sizeof(msg.new_name), "TestRenamed");

    if (msg_bus_send(mq, &msg) != 0) {
        printf("[FAIL] send MSG_DEV_MODIFY_PARAM failed\n");
        ret = -1;
        goto cleanup;
    }
    if (wait_device_param(TEST_DEV_ID, 77, "TestRenamed", &out) != 0) {
        printf("[FAIL] modify not applied: load=%d name=%s\n",
               out.load, out.dev_name);
        ret = -1;
        goto cleanup;
    }
    printf("[PASS] MSG_DEV_MODIFY_PARAM modifies device\n");

    /* 3. 手动删除设备 */
    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_DEL_MANUAL;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", TEST_DEV_ID);

    if (msg_bus_send(mq, &msg) != 0) {
        printf("[FAIL] send MSG_DEV_DEL_MANUAL failed\n");
        ret = -1;
        goto cleanup;
    }
    if (wait_device(TEST_DEV_ID, &out, 0) != 0) {
        printf("[FAIL] device still exists after MSG_DEV_DEL_MANUAL\n");
        ret = -1;
        goto cleanup;
    }
    printf("[PASS] MSG_DEV_DEL_MANUAL deletes device\n");

    /* 4. 清空日志，并写入“日志已清空”记录 */
    if (write_log("INFO", "before clear") != 0) {
        printf("[FAIL] write_log before clear failed\n");
        ret = -1;
        goto cleanup;
    }

    msg_bus_msg_init(&msg);
    msg.type = MSG_LOG_CLEAR;

    if (msg_bus_send(mq, &msg) != 0) {
        printf("[FAIL] send MSG_LOG_CLEAR failed\n");
        ret = -1;
        goto cleanup;
    }
    if (wait_file_contains(TEST_LOG, "日志已清空", 1) != 0) {
        printf("[FAIL] clear log marker not found\n");
        ret = -1;
        goto cleanup;
    }
    if (file_contains(TEST_LOG, "before clear")) {
        printf("[FAIL] old log content still exists after clear\n");
        ret = -1;
        goto cleanup;
    }
    printf("[PASS] MSG_LOG_CLEAR clears log and writes marker\n");

    /* 5. 设置告警阈值 */
    msg_bus_msg_init(&msg);
    msg.type = MSG_SET_ALARM_THRESH;
    msg.alarm_threshold = 66;

    if (msg_bus_send(mq, &msg) != 0) {
        printf("[FAIL] send MSG_SET_ALARM_THRESH failed\n");
        ret = -1;
        goto cleanup;
    }
    if (wait_file_contains(TEST_CFG, "alarm_threshold=66", 1) != 0) {
        printf("[FAIL] config alarm_threshold not updated\n");
        ret = -1;
        goto cleanup;
    }
    printf("[PASS] MSG_SET_ALARM_THRESH updates config\n");

cleanup:
    msg_bus_msg_init(&msg);
    msg.type = MSG_QUIT;
    msg_bus_send(mq, &msg);

    business_stop();
    pthread_join(tid, NULL);
    msg_bus_close_handle(&mq);
    device_list_destroy();

    remove(TEST_CFG);
    remove(TEST_LOG);

    if (ret != 0) {
        return -1;
    }

    printf("===== business test end =====\n");
    return 0;
}
