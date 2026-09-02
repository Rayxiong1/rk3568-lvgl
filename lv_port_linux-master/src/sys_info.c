/**
 * @file    sys_info.c
 * @brief   系统资源采集线程
 *
 * 读取 /proc/meminfo 和 /proc/stat，计算 CPU/内存使用率，
 * 更新本地缓存，并通过消息队列发送系统信息。
 */
#include "sys_info.h"

#include "msg_bus.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sys_info_t g_info;
static volatile int g_stop = 0;
static msg_bus_t g_mq_ui = NULL;

/* 从 /proc/meminfo 读取内存使用率 */
static int read_mem_usage(void)
{
    FILE *fp;
    char line[256];
    unsigned long total = 0;
    unsigned long avail = 0;

    fp = fopen("/proc/meminfo", "r");
    if (fp == NULL) return 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long val;
        if (sscanf(line, "MemTotal: %lu kB", &val) == 1) {
            total = val;
        } else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1) {
            avail = val;
        }
    }
    fclose(fp);

    if (total == 0) return 0;
    return (int)((total - avail) * 100UL / total);
}

/* 读取 /proc/stat 中的 CPU 总时间和空闲时间 */
static unsigned long read_cpu_total(unsigned long *idle)
{
    FILE *fp;
    char line[256];
    unsigned long user, nice, system, idle_, iowait, irq, softirq, steal;

    fp = fopen("/proc/stat", "r");
    if (fp == NULL) return 0;

    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
               &user, &nice, &system, &idle_, &iowait,
               &irq, &softirq, &steal) < 4) {
        return 0;
    }

    *idle = idle_ + iowait;
    return user + nice + system + idle_ + iowait + irq + softirq + steal;
}

/* 通过两次采样计算 CPU 使用率 */
static int read_cpu_usage(void)
{
    unsigned long idle1 = 0, total1 = 0;
    unsigned long idle2 = 0, total2 = 0;

    total1 = read_cpu_total(&idle1);
    usleep(200000);
    total2 = read_cpu_total(&idle2);

    if (total1 == 0 || total2 == 0 || total2 <= total1) return 0;

    {
        unsigned long idle_delta = idle2 - idle1;
        unsigned long total_delta = total2 - total1;
        return (int)((total_delta - idle_delta) * 100UL / total_delta);
    }
}

/* 初始化系统信息缓存 */
void sys_info_init(void)
{
    memset(&g_info, 0, sizeof(g_info));
}

/* 获取最新系统信息缓存 */
void sys_info_get(sys_info_t *info)
{
    if (info == NULL) return;

    pthread_mutex_lock(&g_lock);
    *info = g_info;
    pthread_mutex_unlock(&g_lock);
}

/* 启动系统信息采集线程 */
int sys_info_start(pthread_t *tid, const app_config_t *cfg)
{
    if (tid == NULL || cfg == NULL) return -1;
    (void)cfg;
    g_stop = 0;
    return pthread_create(tid, NULL, sys_info_thread, NULL);
}

/* 请求停止系统信息线程 */
int sys_info_stop(void)
{
    g_stop = 1;
    return 0;
}

/* 系统信息采集线程主循环 */
void *sys_info_thread(void *arg)
{
    (void)arg;

    /*
     * 系统信息消息发送给 UI 消息队列，供后续 LVGL 线程接收显示；
     * 优先打开已存在的队列，不存在则创建。
     */
    if (msg_bus_open(MSG_QUEUE_UI_NAME, &g_mq_ui) != 0) {
        if (msg_bus_create(MSG_QUEUE_UI_NAME, &g_mq_ui) != 0) {
            fprintf(stderr, "[sys_info] open/create ui mq failed\n");
            return NULL;
        }
    }

    while (!g_stop) {
        sys_info_t info;
        msg_t msg;

        info.cpu_usage = read_cpu_usage();
        info.mem_usage = read_mem_usage();

        pthread_mutex_lock(&g_lock);
        g_info = info;
        pthread_mutex_unlock(&g_lock);

        printf("[sys_info] cpu=%d%% mem=%d%%\n", info.cpu_usage, info.mem_usage);

        msg_bus_msg_init(&msg);
        msg.type = MSG_SYS_INFO_REQ;
        msg.sys_cpu = info.cpu_usage;
        msg.sys_mem = info.mem_usage;
        snprintf(msg.from, sizeof(msg.from), "sys");

        if (msg_bus_send(g_mq_ui, &msg) != 0) {
            /* 队列满时丢弃本次采集结果 */
        }

        sleep(1);
    }

    msg_bus_close_handle(&g_mq_ui);
    return NULL;
}
