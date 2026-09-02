/**
 * @file    device_list.c
 * @brief   设备双向链表实现
 *
 * 说明：
 *  - 所有链表操作内部加锁，保证多线程安全
 *  - 对外只返回数据副本，避免外部直接操作链表节点
 */
#include "device_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

device_list_t g_device_list;

/* 初始化设备链表和互斥锁 */
void device_list_init(void)
{
    memset(&g_device_list, 0, sizeof(g_device_list));
    pthread_mutex_init(&g_device_list.lock, NULL);
}

/* 销毁设备链表，释放所有节点并销毁互斥锁 */
void device_list_destroy(void)
{
    device_t *cur;

    pthread_mutex_lock(&g_device_list.lock);

    cur = g_device_list.head;
    while (cur != NULL) {
        device_t *tmp = cur->next;
        free(cur);
        cur = tmp;
    }

    g_device_list.head = NULL;
    g_device_list.tail = NULL;

    pthread_mutex_unlock(&g_device_list.lock);
    pthread_mutex_destroy(&g_device_list.lock);
}

/* 在已加锁状态下按 ID 查找设备节点 */
static device_t *find_locked(const char *id)
{
    device_t *cur;

    if (id == NULL) return NULL;

    for (cur = g_device_list.head; cur != NULL; cur = cur->next) {
        if (strcmp(cur->id, id) == 0) {
            return cur;
        }
    }
    return NULL;
}

/* 添加设备；如果 ID 已存在则更新该设备信息 */
int device_add(const device_t *dev)
{
    device_t *node;

    if (dev == NULL || dev->id[0] == '\0') return -1;

    pthread_mutex_lock(&g_device_list.lock);

    /* 已存在：更新业务字段，不产生重复节点 */
    node = find_locked(dev->id);
    if (node != NULL) {
        device_t *prev = node->prev;
        device_t *next = node->next;

        memcpy(node, dev, sizeof(*node));
        node->prev = prev;
        node->next = next;

        pthread_mutex_unlock(&g_device_list.lock);
        return 0;
    }

    node = (device_t *)calloc(1, sizeof(*node));
    if (node == NULL) {
        pthread_mutex_unlock(&g_device_list.lock);
        return -1;
    }

    memcpy(node, dev, sizeof(*node));
    node->prev = g_device_list.tail;
    node->next = NULL;

    if (g_device_list.tail != NULL) {
        g_device_list.tail->next = node;
    } else {
        g_device_list.head = node;
    }
    g_device_list.tail = node;

    pthread_mutex_unlock(&g_device_list.lock);
    return 0;
}

/* 根据设备 ID 删除设备节点 */
int device_del_by_id(const char *id)
{
    device_t *node;
    int found = 0;

    if (id == NULL || id[0] == '\0') return -1;

    pthread_mutex_lock(&g_device_list.lock);

    node = find_locked(id);
    if (node != NULL) {
        if (node->prev != NULL) {
            node->prev->next = node->next;
        } else {
            g_device_list.head = node->next;
        }

        if (node->next != NULL) {
            node->next->prev = node->prev;
        } else {
            g_device_list.tail = node->prev;
        }

        free(node);
        found = 1;
    }

    pthread_mutex_unlock(&g_device_list.lock);
    return found;
}

/* 获取全部设备数据副本，调用方提供缓冲区 */
int device_get_all(device_t *out, size_t cap, size_t *count)
{
    device_t *cur;
    size_t n = 0;
    int ret = 0;

    if (out == NULL || cap == 0) return -1;

    pthread_mutex_lock(&g_device_list.lock);

    for (cur = g_device_list.head; cur != NULL; cur = cur->next) {
        if (n >= cap) {
            ret = -1; /* 缓冲区不够，截断 */
            break;
        }
        memcpy(&out[n], cur, sizeof(out[n]));
        out[n].prev = NULL;
        out[n].next = NULL;
        n++;
    }

    if (count != NULL) {
        *count = n;
    }

    pthread_mutex_unlock(&g_device_list.lock);
    return ret;
}

/* 根据设备 ID 查找设备，并返回数据副本 */
int device_find_by_id(const char *id, device_t *out)
{
    device_t *found;
    int ret = 0;

    if (id == NULL || id[0] == '\0' || out == NULL) return -1;

    pthread_mutex_lock(&g_device_list.lock);

    found = find_locked(id);
    if (found != NULL) {
        memcpy(out, found, sizeof(*out));
        out->prev = NULL;
        out->next = NULL;
        ret = 1;
    }

    pthread_mutex_unlock(&g_device_list.lock);
    return ret;
}

/* 设置设备开关状态，state 非 0 表示开 */
int device_set_state(const char *id, int state)
{
    device_t *node;
    int ret = 0;

    if (id == NULL || id[0] == '\0') return -1;

    pthread_mutex_lock(&g_device_list.lock);

    node = find_locked(id);
    if (node != NULL) {
        node->state = state ? 1 : 0;
        ret = 1;
    }

    pthread_mutex_unlock(&g_device_list.lock);
    return ret;
}

/* 修改设备负载和名称，new_name 为空表示不修改名称 */
int device_modify_param(const char *id, int new_load, const char *new_name)
{
    device_t *node;
    int ret = 0;

    if (id == NULL || id[0] == '\0') return -1;

    pthread_mutex_lock(&g_device_list.lock);

    node = find_locked(id);
    if (node != NULL) {
        node->load = new_load;
        if (new_name != NULL && new_name[0] != '\0') {
            snprintf(node->dev_name, sizeof(node->dev_name), "%s", new_name);
        }
        ret = 1;
    }

    pthread_mutex_unlock(&g_device_list.lock);
    return ret;
}
