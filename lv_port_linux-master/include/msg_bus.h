/**
 * @file    msg_bus.h
 * @brief   线程间消息总线封装
 *
 * 说明：
 *  - 使用进程内 pthread 环形队列实现，不依赖内核 POSIX mqueue
 *  - 所有线程间通信通过消息队列传递完整数据副本
 *  - 禁止在消息中传递指针
 */
#ifndef MSG_BUS_H
#define MSG_BUS_H

#include "common.h"

/* 进程内消息队列句柄 */
typedef struct msg_queue *msg_bus_t;

/* 初始化消息结构体，填充魔数并清零 */
void msg_bus_msg_init(msg_t *msg);

/*
 * 创建消息队列。
 * name 例如 "/gateway_biz"。
 * 成功返回 0，失败返回 -1。
 */
int msg_bus_create(const char *name, msg_bus_t *mq);

/*
 * 打开已经存在的消息队列（不创建）。
 * 供非创建方线程使用。
 */
int msg_bus_open(const char *name, msg_bus_t *mq);

/*
 * 发送消息。
 * 队列满时返回 -1，不会一直阻塞。
 */
int msg_bus_send(msg_bus_t mq, const msg_t *msg);

/*
 * 阻塞接收消息，成功返回 0。
 */
int msg_bus_recv(msg_bus_t mq, msg_t *msg);

/*
 * 超时接收消息，timeout_ms < 0 表示阻塞。
 * 成功返回 0，超时返回 -1。
 */
int msg_bus_recv_timeout(msg_bus_t mq, msg_t *msg, int timeout_ms);

/*
 * 关闭并销毁消息队列。
 * 会关闭当前句柄并标记队列销毁。
 */
int msg_bus_close(const char *name, msg_bus_t *mq);

/*
 * 只关闭当前句柄，不销毁队列。
 * 供非创建方线程退出时使用。
 */
int msg_bus_close_handle(msg_bus_t *mq);

#endif /* MSG_BUS_H */