/**
 * @file    udp_disc.c
 * @brief   UDP 局域网设备发现线程
 *
 * 功能：
 *  - 定时发送 UDP 广播探测包
 *  - 监听并解析设备应答
 *  - 内置 3 台虚拟设备，用于自动演示上下线
 *  - 设备上线/下线消息通过消息总线发送给业务线程
 */
#include "udp_disc.h"

#include "msg_bus.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DISCOVERY_REQ        "GATEWAY_DISCOVERY_REQ"
#define DEVICE_REPLY_PREFIX  "GATEWAY_DEVICE|"
#define DEVICE_OFFLINE_PREFIX "GATEWAY_OFFLINE|"
#define MAX_SIM_DEVICES      3

typedef struct {
    device_info_t info;
    int online;
    time_t next_change;
} sim_device_t;

static volatile int g_stop = 0;
static msg_bus_t g_mq_biz = NULL;

static sim_device_t g_sim_devices[MAX_SIM_DEVICES];

/* 发送设备上线消息给业务线程 */
static void send_found_msg(const device_info_t *info)
{
    msg_t msg;

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_ONLINE;
    msg.dev = *info;
    snprintf(msg.from, sizeof(msg.from), "udp");

    if (msg_bus_send(g_mq_biz, &msg) != 0) {
        perror("msg_bus_send(FOUND)");
    }
}

/* 发送设备离线消息给业务线程 */
static void send_lost_msg(const char *id)
{
    msg_t msg;

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_OFFLINE;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);
    snprintf(msg.from, sizeof(msg.from), "udp");

    if (msg_bus_send(g_mq_biz, &msg) != 0) {
        perror("msg_bus_send(LOST)");
    }
}

/* 解析 UDP 设备应答报文 */
static int parse_reply(const char *buf, device_info_t *info)
{
    if (buf == NULL || info == NULL) return -1;

    if (strncmp(buf, DEVICE_REPLY_PREFIX, strlen(DEVICE_REPLY_PREFIX)) != 0) {
        return -1;
    }

    if (sscanf(buf, DEVICE_REPLY_PREFIX "%31[^|]|%63[^|]|%31[^|]|%d|%d",
               info->id, info->ip, info->dev_name,
               &info->state, &info->load) >= 5) {
        info->online_time = 0;
        return 0;
    }

    return -1;
}

/* 初始化内置虚拟设备 */
static void sim_devices_init(void)
{
    time_t now = time(NULL);

    memset(g_sim_devices, 0, sizeof(g_sim_devices));

    snprintf(g_sim_devices[0].info.id,   sizeof(g_sim_devices[0].info.id),   "virt-001");
    snprintf(g_sim_devices[0].info.ip,   sizeof(g_sim_devices[0].info.ip),   "192.168.10.101");
    snprintf(g_sim_devices[0].info.dev_name, sizeof(g_sim_devices[0].info.dev_name), "VirtualDevice1");
    g_sim_devices[0].info.state = 1;
    g_sim_devices[0].info.load  = 35;
    g_sim_devices[0].next_change = now + 1;

    snprintf(g_sim_devices[1].info.id,   sizeof(g_sim_devices[1].info.id),   "virt-002");
    snprintf(g_sim_devices[1].info.ip,   sizeof(g_sim_devices[1].info.ip),   "192.168.10.102");
    snprintf(g_sim_devices[1].info.dev_name, sizeof(g_sim_devices[1].info.dev_name), "VirtualDevice2");
    g_sim_devices[1].info.state = 1;
    g_sim_devices[1].info.load  = 60;
    g_sim_devices[1].next_change = now + 3;

    snprintf(g_sim_devices[2].info.id,   sizeof(g_sim_devices[2].info.id),   "virt-003");
    snprintf(g_sim_devices[2].info.ip,   sizeof(g_sim_devices[2].info.ip),   "192.168.10.103");
    snprintf(g_sim_devices[2].info.dev_name, sizeof(g_sim_devices[2].info.dev_name), "VirtualDevice3");
    g_sim_devices[2].info.state = 0;
    g_sim_devices[2].info.load  = 10;
    g_sim_devices[2].next_change = now + 5;
}

/* 模拟虚拟设备定时上线/下线，方便演示 */
static void simulate_virtual_devices(int sock, const struct sockaddr_in *to)
{
    time_t now = time(NULL);
    char pkt[256];
    int i;

    for (i = 0; i < MAX_SIM_DEVICES; i++) {
        sim_device_t *sd = &g_sim_devices[i];

        if (!sd->online && now >= sd->next_change) {
            sd->online = 1;
            sd->info.online_time = 0;
            sd->next_change = now + 8;

            snprintf(pkt, sizeof(pkt),
                     "GATEWAY_DEVICE|%.31s|%.63s|%.31s|%d|%d",
                     sd->info.id, sd->info.ip, sd->info.dev_name,
                     sd->info.state, sd->info.load);
            sendto(sock, pkt, strlen(pkt), 0,
                   (const struct sockaddr *)to, sizeof(*to));
            printf("[udp] virtual online: %s (%s)\n", sd->info.id, sd->info.ip);
        } else if (sd->online && now >= sd->next_change) {
            sd->online = 0;
            sd->next_change = now + 5;

            snprintf(pkt, sizeof(pkt), "GATEWAY_OFFLINE|%.31s", sd->info.id);
            sendto(sock, pkt, strlen(pkt), 0,
                   (const struct sockaddr *)to, sizeof(*to));
            printf("[udp] virtual offline: %s\n", sd->info.id);
        }
    }
}

/* 启动 UDP 设备发现线程 */
int udp_disc_start(pthread_t *tid, const app_config_t *cfg)
{
    if (tid == NULL || cfg == NULL) return -1;
    g_stop = 0;
    return pthread_create(tid, NULL, udp_discovery_thread, (void *)cfg);
}

/* 请求停止 UDP 发现线程 */
int udp_disc_stop(void)
{
    g_stop = 1;
    return 0;
}

/* UDP 设备发现线程主循环 */
void *udp_discovery_thread(void *arg)
{
    const app_config_t *cfg = (const app_config_t *)arg;
    int sock;
    int broadcast = 1;
    int reuse = 1;
    struct sockaddr_in bind_addr;
    struct sockaddr_in dst_addr;
    struct timeval tv;
    char buf[256];

    if (cfg == NULL) return NULL;

    if (msg_bus_open(MSG_QUEUE_BIZ_NAME, &g_mq_biz) != 0) {
        fprintf(stderr, "[udp] open biz mq failed\n");
        return NULL;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        msg_bus_close_handle(&g_mq_biz);
        return NULL;
    }

    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((uint16_t)cfg->udp_port);

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(sock);
        msg_bus_close_handle(&g_mq_biz);
        return NULL;
    }

    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    dst_addr.sin_port = htons((uint16_t)cfg->udp_port);

    sim_devices_init();

    {
        struct sockaddr_in sim_addr;
        memset(&sim_addr, 0, sizeof(sim_addr));
        sim_addr.sin_family = AF_INET;
        sim_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sim_addr.sin_port = htons((uint16_t)cfg->udp_port);

        while (!g_stop) {
            sendto(sock, DISCOVERY_REQ, strlen(DISCOVERY_REQ), 0,
                   (struct sockaddr *)&dst_addr, sizeof(dst_addr));

            while (!g_stop) {
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                     (struct sockaddr *)&from, &from_len);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }

                buf[n] = '\0';

                if (strncmp(buf, DEVICE_OFFLINE_PREFIX,
                            strlen(DEVICE_OFFLINE_PREFIX)) == 0) {
                    char id[DEVICE_ID_LEN] = {0};
                    if (sscanf(buf, DEVICE_OFFLINE_PREFIX "%31s", id) >= 1) {
                        send_lost_msg(id);
                        printf("[udp] recv offline: %s\n", id);
                    }
                    continue;
                }

                device_info_t info;
                if (parse_reply(buf, &info) == 0) {
                    send_found_msg(&info);
                    printf("[udp] recv device: %s (%s)\n", info.id, info.ip);
                }
            }

            simulate_virtual_devices(sock, &sim_addr);
            usleep((useconds_t)(cfg->discovery_interval_ms * 1000));
        }
    }

    close(sock);
    msg_bus_close_handle(&g_mq_biz);
    return NULL;
}