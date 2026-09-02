/**
 * @file    msg_bus.c
 * @brief   线程间消息总线实现（进程内 pthread 环形队列）
 *
 * 注意：
 *  - 消息队列传递的是完整结构体副本，不能传指针
 *  - 队列满时 msg_bus_send 立即返回错误，不阻塞
 *  - 不依赖内核 POSIX mqueue，避免 RK3568 内核未开启 CONFIG_POSIX_MQUEUE 的问题
 */
#include "msg_bus.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MSG_QUEUE_NAME_LEN 32

struct msg_queue {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    msg_t           slots[MSG_QUEUE_MAX_MSG];
    int             head;
    int             count;
    int             refs;
    int             destroyed;
    char            name[MSG_QUEUE_NAME_LEN];
    struct msg_queue *next;
};

static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static msg_bus_t g_queues = NULL;

/* 在注册表锁保护下按名称查找未销毁的队列 */
static msg_bus_t find_queue_locked(const char *name)
{
    msg_bus_t q;

    if (name == NULL) return NULL;

    for (q = g_queues; q != NULL; q = q->next) {
        if (strcmp(q->name, name) == 0 && !q->destroyed) {
            return q;
        }
    }

    return NULL;
}

/* 初始化消息结构体，填充魔数并清零 */
void msg_bus_msg_init(msg_t *msg)
{
    if (msg == NULL) return;
    memset(msg, 0, sizeof(*msg));
    msg->magic = MSG_MAGIC;
}

/* 创建消息队列，并加入全局队列注册表 */
int msg_bus_create(const char *name, msg_bus_t *mq)
{
    msg_bus_t q;

    if (name == NULL || mq == NULL) return -1;
    if (strlen(name) >= MSG_QUEUE_NAME_LEN) return -1;

    pthread_mutex_lock(&g_reg_lock);

    /* 如果已存在同名且仍存活的队列，不允许重复创建 */
    if (find_queue_locked(name) != NULL) {
        pthread_mutex_unlock(&g_reg_lock);
        return -1;
    }

    q = (msg_bus_t)calloc(1, sizeof(*q));
    if (q == NULL) {
        pthread_mutex_unlock(&g_reg_lock);
        return -1;
    }

    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    snprintf(q->name, sizeof(q->name), "%s", name);
    q->refs = 1;

    q->next = g_queues;
    g_queues = q;

    pthread_mutex_unlock(&g_reg_lock);

    *mq = q;
    return 0;
}

/* 打开已存在的消息队列，引用计数加 1 */
int msg_bus_open(const char *name, msg_bus_t *mq)
{
    msg_bus_t q;

    if (name == NULL || mq == NULL) return -1;

    pthread_mutex_lock(&g_reg_lock);
    q = find_queue_locked(name);
    if (q == NULL) {
        pthread_mutex_unlock(&g_reg_lock);
        return -1;
    }
    q->refs++;
    pthread_mutex_unlock(&g_reg_lock);

    *mq = q;
    return 0;
}

/* 发送消息；队列满或已销毁时返回 -1，不阻塞 */
int msg_bus_send(msg_bus_t mq, const msg_t *msg)
{
    int idx;

    if (mq == NULL || msg == NULL) return -1;

    pthread_mutex_lock(&mq->lock);

    if (mq->destroyed || mq->count >= MSG_QUEUE_MAX_MSG) {
        pthread_mutex_unlock(&mq->lock);
        return -1;
    }

    idx = (mq->head + mq->count) % MSG_QUEUE_MAX_MSG;
    mq->slots[idx] = *msg;
    mq->count++;

    pthread_cond_signal(&mq->not_empty);
    pthread_mutex_unlock(&mq->lock);

    return 0;
}

/* 阻塞接收消息 */
int msg_bus_recv(msg_bus_t mq, msg_t *msg)
{
    if (mq == NULL || msg == NULL) return -1;

    pthread_mutex_lock(&mq->lock);

    while (mq->count == 0 && !mq->destroyed) {
        pthread_cond_wait(&mq->not_empty, &mq->lock);
    }

    if (mq->count == 0) {
        pthread_mutex_unlock(&mq->lock);
        return -1;
    }

    *msg = mq->slots[mq->head];
    mq->head = (mq->head + 1) % MSG_QUEUE_MAX_MSG;
    mq->count--;

    pthread_cond_signal(&mq->not_full);
    pthread_mutex_unlock(&mq->lock);

    return 0;
}

/* 超时接收消息；timeout_ms < 0 表示一直阻塞 */
int msg_bus_recv_timeout(msg_bus_t mq, msg_t *msg, int timeout_ms)
{
    struct timespec ts;
    int ret = 0;

    if (mq == NULL || msg == NULL) return -1;

    pthread_mutex_lock(&mq->lock);

    if (timeout_ms < 0) {
        while (mq->count == 0 && !mq->destroyed) {
            pthread_cond_wait(&mq->not_empty, &mq->lock);
        }
    } else {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        while (mq->count == 0 && !mq->destroyed) {
            ret = pthread_cond_timedwait(&mq->not_empty, &mq->lock, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&mq->lock);
                return -1;
            }
            if (ret != 0 && ret != EINTR) {
                pthread_mutex_unlock(&mq->lock);
                return -1;
            }
        }
    }

    if (mq->count == 0) {
        pthread_mutex_unlock(&mq->lock);
        return -1;
    }

    *msg = mq->slots[mq->head];
    mq->head = (mq->head + 1) % MSG_QUEUE_MAX_MSG;
    mq->count--;

    pthread_cond_signal(&mq->not_full);
    pthread_mutex_unlock(&mq->lock);

    return 0;
}

/* 在注册表锁保护下将队列从全局链表中移除 */
static void remove_queue_locked(msg_bus_t q)
{
    msg_bus_t *p;

    for (p = &g_queues; *p != NULL; p = &(*p)->next) {
        if (*p == q) {
            *p = q->next;
            break;
        }
    }
}

/* 关闭当前句柄并标记队列销毁 */
int msg_bus_close(const char *name, msg_bus_t *mq)
{
    (void)name;

    if (mq != NULL && *mq != NULL) {
        pthread_mutex_lock(&g_reg_lock);
        (*mq)->destroyed = 1;
        pthread_mutex_unlock(&g_reg_lock);
    }

    return msg_bus_close_handle(mq);
}

/* 只关闭当前句柄，引用计数减 1，不立即销毁队列 */
int msg_bus_close_handle(msg_bus_t *mq)
{
    msg_bus_t q;

    if (mq == NULL || *mq == NULL) return 0;

    q = *mq;
    *mq = NULL;

    pthread_mutex_lock(&g_reg_lock);
    q->refs--;
    if (q->refs == 0 && q->destroyed) {
        remove_queue_locked(q);
        pthread_mutex_unlock(&g_reg_lock);

        pthread_mutex_destroy(&q->lock);
        pthread_cond_destroy(&q->not_empty);
        pthread_cond_destroy(&q->not_full);
        free(q);
    } else {
        pthread_mutex_unlock(&g_reg_lock);
    }

    return 0;
}