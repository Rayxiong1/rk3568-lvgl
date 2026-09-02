/**
 * @file    common.h
 * @brief   Gateway 公共数据结构与宏定义
 *
 * 包含设备结构、配置结构、消息类型、消息结构等。
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define DEVICE_ID_LEN     32
#define DEVICE_IP_LEN     64
#define DEVICE_NAME_LEN   32
#define CMD_LEN           16
#define MSG_PAYLOAD_LEN   256
#define PATH_LEN          128

/* ---------------- 设备结构体 ---------------- */
typedef struct device {
    char     id[DEVICE_ID_LEN];       /* 设备 ID */
    char     ip[DEVICE_IP_LEN];       /* IP 地址 */
    char     dev_name[DEVICE_NAME_LEN]; /* 设备名称 */
    int      state;                   /* 运行状态：0=关，1=开 */
    int      load;                    /* 模拟负载：0~100 */
    uint32_t online_time;             /* 在线时间：秒 */
    struct device *prev;              /* 双向链表前驱 */
    struct device *next;              /* 双向链表后继 */
} device_t;

/*
 * 设备信息纯数据副本，用于消息队列传输。
 * 注意：不能包含指针，POSIX 消息队列传递的是完整数据副本。
 */
typedef struct {
    char     id[DEVICE_ID_LEN];
    char     ip[DEVICE_IP_LEN];
    char     dev_name[DEVICE_NAME_LEN];
    int      state;
    int      load;
    uint32_t online_time;
} device_info_t;

/* 设备链表节点 -> 消息纯数据副本 */
static inline void device_info_from_device(device_info_t *dst, const device_t *src)
{
    if (dst == NULL || src == NULL) return;
    memcpy(dst, src, sizeof(*dst));
}

/* 消息纯数据副本 -> 设备链表节点（prev/next 置空） */
static inline void device_from_info(device_t *dst, const device_info_t *src)
{
    if (dst == NULL || src == NULL) return;
    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, sizeof(*src));
}

/* ---------------- 运行配置 ---------------- */
typedef struct {
    int      http_port;              /* HTTP 服务端口 */
    int      udp_port;               /* UDP 发现端口 */
    int      discovery_interval_ms;  /* 广播发现间隔 */
    int      device_timeout_ms;      /* 设备离线超时 */
    int      alarm_threshold;        /* 告警阈值 */
    int      default_load;           /* 模拟设备默认负载 */
    char     default_dev_name[DEVICE_NAME_LEN]; /* 模拟设备默认名称 */
    char     log_file[PATH_LEN];
    char     config_file[PATH_LEN];
} app_config_t;

/* ---------------- 消息类型 ---------------- */
typedef enum {
    MSG_DEV_ONLINE = 1,        /* 设备上线 */
    MSG_DEV_OFFLINE,           /* 设备下线 */
    MSG_DEV_UPDATE,            /* 设备状态更新 */
    MSG_DEV_CTRL,              /* 开关设备 */
    MSG_DEV_ADD_MANUAL,        /* 手动新增虚拟设备 */
    MSG_DEV_DEL_MANUAL,        /* 手动删除设备 */
    MSG_DEV_MODIFY_PARAM,      /* 修改设备参数：负载、名称 */
    MSG_LOG_CLEAR,             /* 清空日志 */
    MSG_SYS_INFO_REQ,          /* 系统信息 */
    MSG_SET_ALARM_THRESH,      /* 设置告警阈值 */
    MSG_QUIT                   /* 退出 */
} msg_type_t;

/* ---------------- 消息队列消息结构体 ---------------- */
typedef struct {
    uint32_t  magic;                 /* 魔数，简单校验 */
    msg_type_t type;                 /* 消息类型 */
    device_info_t dev;               /* 设备信息（纯数据，无指针） */
    char      cmd[CMD_LEN];          /* 控制指令：on/off */
    char      from[16];              /* 来源：http/ui/discovery/sys */
    char      payload[MSG_PAYLOAD_LEN];

    /* 扩展字段 */
    int       new_load;              /* 新负载 */
    char      new_name[DEVICE_NAME_LEN]; /* 新名称 */
    int       alarm_threshold;       /* 告警阈值 */
    int       sys_cpu;               /* CPU 使用率 % */
    int       sys_mem;               /* 内存使用率 % */
} msg_t;

/* 消息队列名称 */
#define MSG_QUEUE_UI_NAME   "/gateway_ui"
#define MSG_QUEUE_BIZ_NAME  "/gateway_biz"
#define MSG_QUEUE_MAX_MSG   8

/* 消息魔数 */
#define MSG_MAGIC           0x47415445u  /* "GATE" */

#endif /* COMMON_H */