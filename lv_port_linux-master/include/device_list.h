/**
 * @file    device_list.h
 * @brief   设备双向链表管理（线程安全）
 *
 * 说明：
 *  - 所有接口操作全局设备链表 g_device_list
 *  - 对外只返回数据副本，不返回原始链表指针
 *  - 支持修改设备状态、负载和名称
 */
#ifndef DEVICE_LIST_H
#define DEVICE_LIST_H

#include "common.h"
#include <pthread.h>
#include <stddef.h>

/* 链表容器：头尾指针 + 互斥锁 */
typedef struct {
    device_t *head;
    device_t *tail;
    pthread_mutex_t lock;
} device_list_t;

/* 全局设备链表，所有线程通过 device_list 接口访问 */
extern device_list_t g_device_list;

/* 初始化链表和互斥锁 */
void device_list_init(void);

/* 销毁链表：释放全部节点内存并销毁互斥锁 */
void device_list_destroy(void);

/*
 * 新增设备节点。
 * 如果设备已存在，则更新设备信息，不产生重复节点。
 * 成功返回 0，参数错误或内存分配失败返回 -1。
 */
int device_add(const device_t *dev);

/*
 * 根据 ID 删除设备。
 * 找到并删除返回 1，未找到返回 0，参数错误返回 -1。
 */
int device_del_by_id(const char *id);

/*
 * 遍历拷贝全部设备数据副本。
 * out 为调用方缓冲区，cap 为缓冲区可容纳节点数。
 * count 返回实际拷贝数量（可为 NULL）。
 * 如果链表节点数超过 cap，则只拷贝 cap 个并返回 -1；否则返回 0。
 */
int device_get_all(device_t *out, size_t cap, size_t *count);

/*
 * 根据 ID 查找设备，拷贝数据副本到 out。
 * 找到返回 1，未找到返回 0，参数错误返回 -1。
 */
int device_find_by_id(const char *id, device_t *out);

/*
 * 根据 ID 修改设备开关状态。
 * 找到并修改返回 1，未找到返回 0，参数错误返回 -1。
 */
int device_set_state(const char *id, int state);

/*
 * 根据 ID 修改设备负载和名称。
 * new_name 可为 NULL 表示不修改名称。
 * 找到并修改返回 1，未找到返回 0，参数错误返回 -1。
 */
int device_modify_param(const char *id, int new_load, const char *new_name);

#endif /* DEVICE_LIST_H */
