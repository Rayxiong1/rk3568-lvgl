/**
 * @file    lv_ui.h
 * @brief   LVGL 界面线程
 */
#ifndef LV_UI_H
#define LV_UI_H

#include "common.h"
#include <pthread.h>

int  lv_ui_start(pthread_t *tid);
int  lv_ui_stop(void);
void *lv_ui_thread(void *arg);

#endif /* LV_UI_H */
