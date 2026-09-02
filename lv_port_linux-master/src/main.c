/**
 * @file    main.c
 * @brief   程序入口：创建所有线程
 *
 * 启动顺序：
 *  - 先启动业务线程，并创建业务消息队列
 *  - 再启动 UDP 发现线程
 *  - 启动 HTTP 服务线程和系统信息采集线程
 *  - 最后启动 LVGL 界面线程
 */
#include <pthread.h>
#include <stdio.h>

#include "business.h"
#include "common.h"
#include "device_list.h"
#include "file_ops.h"
#include "http_server.h"
#include "lv_ui.h"
#include "sys_info.h"
#include "udp_disc.h"

/* 程序入口：初始化系统并启动所有线程 */
int main(void)
{
    app_config_t cfg;
    pthread_t biz_tid = 0, udp_tid = 0, http_tid = 0, sys_tid = 0, ui_tid = 0;
    int biz_ok = 0, udp_ok = 0, http_ok = 0, sys_ok = 0, ui_ok = 0;

    /* 串口/终端输出实时可见 */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1. 加载配置 */
    if (load_config(&cfg) != 0) {
        fprintf(stderr, "failed to load config\n");
        return -1;
    }

    /* 2. 初始化设备链表和系统信息缓存 */
    device_list_init();
    sys_info_init();

    /* 3. 启动业务线程（先创建业务消息队列） */
    if (business_start(&biz_tid, &cfg) == 0) {
        biz_ok = 1;
    } else {
        fprintf(stderr, "failed to start business thread\n");
        goto fail;
    }

    /* 4. 启动 UDP 发现线程 */
    if (udp_disc_start(&udp_tid, &cfg) == 0) {
        udp_ok = 1;
    } else {
        fprintf(stderr, "failed to start udp_disc thread\n");
        goto fail;
    }

    /* 5. 启动 HTTP 线程 */
    if (http_server_start(&http_tid, &cfg) == 0) {
        http_ok = 1;
    } else {
        fprintf(stderr, "failed to start http thread\n");
        goto fail;
    }

    /* 6. 启动系统信息采集线程 */
    if (sys_info_start(&sys_tid, &cfg) == 0) {
        sys_ok = 1;
    } else {
        fprintf(stderr, "failed to start sys_info thread\n");
        goto fail;
    }

    /* 7. 启动 LVGL UI 线程（骨架） */
    if (lv_ui_start(&ui_tid) == 0) {
        ui_ok = 1;
    } else {
        fprintf(stderr, "failed to start lv_ui thread\n");
        goto fail;
    }

    printf("gateway started, http_port=%d, udp_port=%d\n",
           cfg.http_port, cfg.udp_port);

    /* 7. UI 线程退出后清理 */
    pthread_join(ui_tid, NULL);
    ui_ok = 0;

    business_stop();
    udp_disc_stop();
    http_server_stop();
    sys_info_stop();

    if (biz_ok)  pthread_join(biz_tid, NULL);
    if (udp_ok)  pthread_join(udp_tid, NULL);
    if (http_ok) pthread_join(http_tid, NULL);
    if (sys_ok)  pthread_join(sys_tid, NULL);

    device_list_destroy();
    return 0;

fail:
    business_stop();
    udp_disc_stop();
    http_server_stop();
    sys_info_stop();
    lv_ui_stop();

    if (ui_ok)   pthread_join(ui_tid, NULL);
    if (sys_ok)  pthread_join(sys_tid, NULL);
    if (http_ok) pthread_join(http_tid, NULL);
    if (udp_ok)  pthread_join(udp_tid, NULL);
    if (biz_ok)  pthread_join(biz_tid, NULL);

    device_list_destroy();
    return -1;
}