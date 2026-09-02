/**
 * @file    test_msg_bus.c
 * @brief   单元测试：消息总线封装测试
 *
 * 测试内容：
 *  - 创建消息队列
 *  - 一个线程发送完整消息结构体
 *  - 一个线程阻塞接收并校验字段完整性
 *  - 关闭销毁消息队列
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "msg_bus.h"

#define TEST_MSG_COUNT 5
#define TEST_QUEUE_NAME "/gateway_test_bus"

typedef struct {
    msg_bus_t mq;
    int   count;
} sender_arg_t;

typedef struct {
    msg_bus_t mq;
    int   count;
    int   ok;
} receiver_arg_t;

static void *sender_thread(void *arg)
{
    sender_arg_t *sa = (sender_arg_t *)arg;
    int i;

    for (i = 0; i < sa->count; i++) {
        msg_t msg;

        msg_bus_msg_init(&msg);
        msg.type = MSG_DEV_ONLINE;

        snprintf(msg.dev.id,   sizeof(msg.dev.id),   "dev-%03d", i);
        snprintf(msg.dev.ip,   sizeof(msg.dev.ip),   "192.168.1.%d", 100 + i);
        snprintf(msg.dev.dev_name, sizeof(msg.dev.dev_name), "Device%d", i);
        msg.dev.state = i % 2;
        msg.dev.load = i * 10;
        msg.dev.online_time = (uint32_t)(i * 10);

        snprintf(msg.cmd,     sizeof(msg.cmd),     "on");
        snprintf(msg.from,    sizeof(msg.from),    "test");
        snprintf(msg.payload, sizeof(msg.payload), "payload-%d", i);

        if (msg_bus_send(sa->mq, &msg) != 0) {
            perror("msg_bus_send");
            return NULL;
        }

        printf("[send] %s state=%d load=%d online=%u\n",
               msg.dev.id, msg.dev.state, msg.dev.load, msg.dev.online_time);
    }

    return NULL;
}

static void *receiver_thread(void *arg)
{
    receiver_arg_t *ra = (receiver_arg_t *)arg;
    int i;

    ra->ok = 1;

    for (i = 0; i < ra->count; i++) {
        msg_t msg;
        char expect_id[DEVICE_ID_LEN];
        char expect_ip[DEVICE_IP_LEN];
        char expect_name[DEVICE_NAME_LEN];
        char expect_payload[MSG_PAYLOAD_LEN];

        if (msg_bus_recv(ra->mq, &msg) != 0) {
            perror("msg_bus_recv");
            ra->ok = 0;
            return NULL;
        }

        snprintf(expect_id,      sizeof(expect_id),      "dev-%03d", i);
        snprintf(expect_ip,      sizeof(expect_ip),      "192.168.1.%d", 100 + i);
        snprintf(expect_name,    sizeof(expect_name),    "Device%d", i);
        snprintf(expect_payload, sizeof(expect_payload), "payload-%d", i);

        if (msg.magic != MSG_MAGIC) {
            printf("[FAIL] magic mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (msg.type != MSG_DEV_ONLINE) {
            printf("[FAIL] type mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (strcmp(msg.dev.id, expect_id) != 0) {
            printf("[FAIL] id mismatch at %d: %s != %s\n", i, msg.dev.id, expect_id);
            ra->ok = 0;
            return NULL;
        }
        if (strcmp(msg.dev.ip, expect_ip) != 0) {
            printf("[FAIL] ip mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (strcmp(msg.dev.dev_name, expect_name) != 0) {
            printf("[FAIL] name mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (msg.dev.state != (i % 2)) {
            printf("[FAIL] state mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (msg.dev.load != i * 10) {
            printf("[FAIL] load mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (msg.dev.online_time != (uint32_t)(i * 10)) {
            printf("[FAIL] online_time mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (strcmp(msg.cmd, "on") != 0) {
            printf("[FAIL] cmd mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (strcmp(msg.from, "test") != 0) {
            printf("[FAIL] from mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }
        if (strcmp(msg.payload, expect_payload) != 0) {
            printf("[FAIL] payload mismatch at %d\n", i);
            ra->ok = 0;
            return NULL;
        }

        printf("[recv] %s state=%d load=%d online=%u\n",
               msg.dev.id, msg.dev.state, msg.dev.load, msg.dev.online_time);
    }

    return NULL;
}

static int test_queue_full(void)
{
    msg_bus_t mq = NULL;
    msg_t msg;
    int i;
    const char *name = "/gateway_test_bus_full";

    if (msg_bus_create(name, &mq) != 0) {
        printf("[FAIL] create full-test queue failed\n");
        return -1;
    }

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_UPDATE;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "full-test");

    for (i = 0; i < MSG_QUEUE_MAX_MSG; i++) {
        if (msg_bus_send(mq, &msg) != 0) {
            printf("[FAIL] send %d should succeed\n", i);
            msg_bus_close(name, &mq);
            return -1;
        }
    }

    if (msg_bus_send(mq, &msg) == 0) {
        printf("[FAIL] send when queue full should return error\n");
        msg_bus_close(name, &mq);
        return -1;
    }

    printf("[PASS] msg_bus_send returns error when queue full\n");
    msg_bus_close(name, &mq);
    return 0;
}


int main(void)
{
    msg_bus_t mq = NULL;
    pthread_t sender_tid, receiver_tid;
    sender_arg_t sender_arg;
    receiver_arg_t receiver_arg;

    printf("===== msg_bus test start =====\n");

    if (msg_bus_create(TEST_QUEUE_NAME, &mq) != 0) {
        printf("[FAIL] msg_bus_create failed\n");
        return -1;
    }

    sender_arg.mq = mq;
    sender_arg.count = TEST_MSG_COUNT;

    receiver_arg.mq = mq;
    receiver_arg.count = TEST_MSG_COUNT;
    receiver_arg.ok = 0;

    if (pthread_create(&sender_tid, NULL, sender_thread, &sender_arg) != 0) {
        printf("[FAIL] create sender thread failed\n");
        msg_bus_close(TEST_QUEUE_NAME, &mq);
        return -1;
    }
    if (pthread_create(&receiver_tid, NULL, receiver_thread, &receiver_arg) != 0) {
        printf("[FAIL] create receiver thread failed\n");
        pthread_join(sender_tid, NULL);
        msg_bus_close(TEST_QUEUE_NAME, &mq);
        return -1;
    }

    pthread_join(sender_tid, NULL);
    pthread_join(receiver_tid, NULL);

    msg_bus_close(TEST_QUEUE_NAME, &mq);

    if (!receiver_arg.ok) {
        printf("[FAIL] msg_bus struct transfer error\n");
        return -1;
    }
    printf("[PASS] msg_bus struct transfer ok\n");

    if (test_queue_full() != 0) {
        printf("[FAIL] queue full test failed\n");
        return -1;
    }

    printf("===== msg_bus test end =====\n");
    return 0;
}
