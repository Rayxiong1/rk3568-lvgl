/**
 * @file    lv_ui.c
 * @brief   LVGL 9.4 开发板界面线程
 *
 * 功能：
 *  - 设备列表显示：ID、名称、IP、状态、负载
 *  - 设备开关控制：通过业务消息队列发送 MSG_DEV_CTRL
 *  - 系统信息显示：CPU、内存使用率
 *  - 告警阈值设置、清空日志
 */
#include "lv_ui.h"

#include "device_list.h"
#include "file_ops.h"
#include "msg_bus.h"
#include "sys_info.h"

#include "lvgl/lvgl.h"
#include "src/lib/driver_backends.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_UI_DEVICES   64
#define UI_REFRESH_MS    500

static volatile int g_stop = 0;
static msg_bus_t g_ui_mq = NULL;
static msg_bus_t g_biz_mq = NULL;

static lv_style_t g_ui_style;
static lv_font_t *g_ui_font = NULL;

static lv_obj_t *g_dev_list = NULL;
static lv_obj_t *g_sys_cpu_bar = NULL;
static lv_obj_t *g_sys_mem_bar = NULL;
static lv_obj_t *g_sys_cpu_label = NULL;
static lv_obj_t *g_sys_mem_label = NULL;
static lv_obj_t *g_thresh_label = NULL;
static int g_alarm_threshold = 80;

/* 管理页/弹窗用控件 */
static lv_obj_t *g_kb = NULL;
static lv_obj_t *g_add_popup = NULL;
static lv_obj_t *g_add_id_ta = NULL;
static lv_obj_t *g_add_name_ta = NULL;
static lv_obj_t *g_add_ip_ta = NULL;
static lv_obj_t *g_add_load_ta = NULL;
static lv_obj_t *g_search_ta = NULL;
static lv_obj_t *g_del_id_ta = NULL;
static lv_obj_t *g_mod_id_ta = NULL;
static lv_obj_t *g_mod_load_ta = NULL;
static lv_obj_t *g_mod_name_ta = NULL;

typedef struct {
    char id[DEVICE_ID_LEN];
    int  state;
} ui_btn_data_t;

static ui_btn_data_t g_btn_data[MAX_UI_DEVICES];

static void ui_refresh_devices(void);

/* 忽略大小写子串匹配 */
static int ui_contains_ignore_case(const char *haystack, const char *needle)
{
    size_t hlen;
    size_t nlen;
    size_t i;
    size_t j;

    if (needle == NULL || needle[0] == '\0') return 1;
    if (haystack == NULL) return 0;

    hlen = strlen(haystack);
    nlen = strlen(needle);
    if (nlen > hlen) return 0;

    for (i = 0; i <= hlen - nlen; i++) {
        for (j = 0; j < nlen; j++) {
            char c1 = haystack[i + j];
            char c2 = needle[j];

            if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 - 'A' + 'a');
            if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 - 'A' + 'a');
            if (c1 != c2) break;
        }
        if (j == nlen) return 1;
    }

    return 0;
}

/* 清除搜索框 */
static void ui_clear_search_cb(lv_event_t *e)
{
    (void)e;
    if (g_search_ta != NULL) {
        lv_textarea_set_text(g_search_ta, "");
    }
    if (g_dev_list != NULL) {
        lv_obj_scroll_to_y(g_dev_list, 0, LV_ANIM_OFF);
    }
    ui_refresh_devices();
}

/* 发送设备开关控制命令 */
static void ui_send_ctrl_cb(lv_event_t *e)
{
    ui_btn_data_t *data = (ui_btn_data_t *)lv_event_get_user_data(e);
    msg_t msg;

    if (data == NULL || g_biz_mq == NULL) return;

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_CTRL;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", data->id);
    snprintf(msg.cmd, sizeof(msg.cmd), "%s", data->state ? "off" : "on");
    snprintf(msg.from, sizeof(msg.from), "ui");

    msg_bus_send(g_biz_mq, &msg);
}

/* 删除设备 */
static void ui_del_device_cb(lv_event_t *e)
{
    ui_btn_data_t *data = (ui_btn_data_t *)lv_event_get_user_data(e);
    msg_t msg;

    if (data == NULL || g_biz_mq == NULL) return;

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_DEL_MANUAL;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", data->id);
    snprintf(msg.from, sizeof(msg.from), "ui");

    msg_bus_send(g_biz_mq, &msg);
}

/* 修改告警阈值 */
static void ui_threshold_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    msg_t msg;

    g_alarm_threshold += delta;
    if (g_alarm_threshold < 0) g_alarm_threshold = 0;
    if (g_alarm_threshold > 100) g_alarm_threshold = 100;

    if (g_thresh_label != NULL) {
        lv_label_set_text_fmt(g_thresh_label, "告警阈值: %d%%", g_alarm_threshold);
    }

    if (g_biz_mq == NULL) return;

    msg_bus_msg_init(&msg);
    msg.type = MSG_SET_ALARM_THRESH;
    msg.alarm_threshold = g_alarm_threshold;
    snprintf(msg.from, sizeof(msg.from), "ui");
    msg_bus_send(g_biz_mq, &msg);
}

/* 清空日志 */
static void ui_clear_log_cb(lv_event_t *e)
{
    msg_t msg;

    (void)e;
    if (g_biz_mq == NULL) return;

    msg_bus_msg_init(&msg);
    msg.type = MSG_LOG_CLEAR;
    snprintf(msg.from, sizeof(msg.from), "ui");
    msg_bus_send(g_biz_mq, &msg);
}

/* 文本框聚焦时弹出键盘 */
static void ui_ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);

    if (kb == NULL) return;

    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(kb, -1);
    }
    else if (code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 关闭添加设备弹窗 */
static void ui_close_add_popup(lv_event_t *e)
{
    (void)e;

    if (g_kb != NULL) {
        lv_keyboard_set_textarea(g_kb, NULL);
        lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    }

    if (g_add_popup != NULL) {
        lv_obj_delete_async(g_add_popup);
        g_add_popup = NULL;
        g_add_id_ta = NULL;
        g_add_name_ta = NULL;
        g_add_ip_ta = NULL;
        g_add_load_ta = NULL;
    }
}

/* 添加设备弹窗确认 */
static void ui_add_confirm_cb(lv_event_t *e)
{
    const char *id;
    const char *name;
    const char *ip;
    const char *load_str;
    msg_t msg;

    (void)e;

    if (g_add_id_ta == NULL || g_biz_mq == NULL) return;

    id = lv_textarea_get_text(g_add_id_ta);
    name = lv_textarea_get_text(g_add_name_ta);
    ip = lv_textarea_get_text(g_add_ip_ta);
    load_str = lv_textarea_get_text(g_add_load_ta);

    if (id[0] == '\0') return;

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_ADD_MANUAL;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);
    snprintf(msg.dev.dev_name, sizeof(msg.dev.dev_name), "%s",
             name[0] != '\0' ? name : "UI-Device");
    snprintf(msg.dev.ip, sizeof(msg.dev.ip), "%s",
             ip[0] != '\0' ? ip : "192.168.1.100");
    if (load_str[0] != '\0') {
        msg.dev.load = atoi(load_str);
        if (msg.dev.load < 0) msg.dev.load = 0;
        if (msg.dev.load > 100) msg.dev.load = 100;
    } else {
        msg.dev.load = 30;
    }
    msg.dev.state = 1;
    msg.dev.online_time = 0;
    snprintf(msg.from, sizeof(msg.from), "ui");

    msg_bus_send(g_biz_mq, &msg);
    ui_close_add_popup(NULL);
}

/* 打开添加设备弹窗 */
static void ui_open_add_popup(lv_event_t *e)
{
    lv_obj_t *title;
    lv_obj_t *row;
    lv_obj_t *lab;
    lv_obj_t *btn;
    lv_obj_t *btn_label;

    (void)e;

    if (g_add_popup != NULL) return;

    /* 打开弹窗前先隐藏键盘，避免遮挡 */
    if (g_kb != NULL) {
        lv_keyboard_set_textarea(g_kb, NULL);
        lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);
    }

    g_add_popup = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_add_popup, lv_pct(90), 260);
    lv_obj_align(g_add_popup, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_flex_flow(g_add_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_add_popup, 12, LV_PART_MAIN);
    lv_obj_add_flag(g_add_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_add_popup, LV_DIR_VER);
    lv_obj_move_to_index(g_add_popup, -1);

    title = lv_label_create(g_add_popup);
    lv_label_set_text(title, "添加设备");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00BCD4), LV_PART_MAIN);

    /* ID */
    row = lv_obj_create(g_add_popup);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lab = lv_label_create(row);
    lv_label_set_text(lab, "ID:");
    lv_obj_set_width(lab, 60);
    g_add_id_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_add_id_ta, true);
    lv_textarea_set_placeholder_text(g_add_id_ta, "设备ID");
    lv_textarea_set_max_length(g_add_id_ta, DEVICE_ID_LEN - 1);
    lv_obj_set_width(g_add_id_ta, 220);
    lv_obj_add_event_cb(g_add_id_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    /* 名称 */
    row = lv_obj_create(g_add_popup);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lab = lv_label_create(row);
    lv_label_set_text(lab, "名称:");
    lv_obj_set_width(lab, 60);
    g_add_name_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_add_name_ta, true);
    lv_textarea_set_placeholder_text(g_add_name_ta, "设备名称");
    lv_textarea_set_max_length(g_add_name_ta, DEVICE_NAME_LEN - 1);
    lv_obj_set_width(g_add_name_ta, 220);
    lv_obj_add_event_cb(g_add_name_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    /* IP */
    row = lv_obj_create(g_add_popup);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lab = lv_label_create(row);
    lv_label_set_text(lab, "IP:");
    lv_obj_set_width(lab, 60);
    g_add_ip_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_add_ip_ta, true);
    lv_textarea_set_placeholder_text(g_add_ip_ta, "IP地址(可选)");
    lv_textarea_set_max_length(g_add_ip_ta, DEVICE_IP_LEN - 1);
    lv_obj_set_width(g_add_ip_ta, 220);
    lv_obj_add_event_cb(g_add_ip_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    /* 负载 */
    row = lv_obj_create(g_add_popup);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lab = lv_label_create(row);
    lv_label_set_text(lab, "负载:");
    lv_obj_set_width(lab, 60);
    g_add_load_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_add_load_ta, true);
    lv_textarea_set_placeholder_text(g_add_load_ta, "0-100");
    lv_textarea_set_max_length(g_add_load_ta, 3);
    lv_obj_set_width(g_add_load_ta, 220);
    lv_obj_add_event_cb(g_add_load_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    /* 按钮行 */
    row = lv_obj_create(g_add_popup);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    btn = lv_button_create(row);
    lv_obj_set_size(btn, 100, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "确定");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_add_confirm_cb, LV_EVENT_CLICKED, NULL);

    btn = lv_button_create(row);
    lv_obj_set_size(btn, 100, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "取消");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_close_add_popup, LV_EVENT_CLICKED, NULL);
}

/* 删除设备：根据输入 ID 删除 */
static void ui_del_by_id_cb(lv_event_t *e)
{
    const char *id;
    msg_t msg;

    (void)e;
    if (g_del_id_ta == NULL || g_biz_mq == NULL) return;

    id = lv_textarea_get_text(g_del_id_ta);
    if (id[0] == '\0') return;

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_DEL_MANUAL;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);
    snprintf(msg.from, sizeof(msg.from), "ui");

    msg_bus_send(g_biz_mq, &msg);
    lv_textarea_set_text(g_del_id_ta, "");
}

/* 修改设备参数：ID + 负载 + 名称 */
static void ui_modify_cb(lv_event_t *e)
{
    const char *id;
    const char *load_str;
    const char *name;
    msg_t msg;

    (void)e;
    if (g_mod_id_ta == NULL || g_biz_mq == NULL) return;

    id = lv_textarea_get_text(g_mod_id_ta);
    load_str = lv_textarea_get_text(g_mod_load_ta);
    name = lv_textarea_get_text(g_mod_name_ta);

    if (id[0] == '\0') return;

    msg_bus_msg_init(&msg);
    msg.type = MSG_DEV_MODIFY_PARAM;
    snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);

    /* 负载为空时，保留设备当前负载，方便只修改名称 */
    if (load_str[0] != '\0') {
        msg.new_load = atoi(load_str);
        if (msg.new_load < 0) msg.new_load = 0;
        if (msg.new_load > 100) msg.new_load = 100;
    } else {
        device_t dev;
        if (device_find_by_id(id, &dev) == 1) {
            msg.new_load = dev.load;
        } else {
            msg.new_load = 0;
        }
    }

    snprintf(msg.new_name, sizeof(msg.new_name), "%s", name);
    snprintf(msg.from, sizeof(msg.from), "ui");

    msg_bus_send(g_biz_mq, &msg);
    lv_textarea_set_text(g_mod_id_ta, "");
    lv_textarea_set_text(g_mod_load_ta, "");
    lv_textarea_set_text(g_mod_name_ta, "");
}

/* 刷新设备列表（支持按 ID/IP 过滤 + 告警红色显示） */
static void ui_refresh_devices(void)
{
    device_t devs[MAX_UI_DEVICES];
    size_t count = 0;
    size_t i;
    size_t disp = 0;
    int32_t scroll_y = 0;
    const char *kw = (g_search_ta != NULL) ? lv_textarea_get_text(g_search_ta) : "";

    if (g_dev_list == NULL) return;

    /* 记住当前滚动位置，刷新后恢复，避免列表跳回顶部 */
    scroll_y = lv_obj_get_scroll_y(g_dev_list);
    lv_obj_clean(g_dev_list);

    device_get_all(devs, MAX_UI_DEVICES, &count);
    if (count > MAX_UI_DEVICES) count = MAX_UI_DEVICES;

    for (i = 0; i < count; i++) {
        lv_obj_t *row;
        lv_obj_t *info_label;
        lv_obj_t *state_label;
        lv_obj_t *btn;
        lv_obj_t *btn_label;
        lv_obj_t *del_btn;
        lv_obj_t *del_btn_label;
        int alarm;

        /* 按 ID/IP 过滤 */
        if (!ui_contains_ignore_case(devs[i].id, kw) &&
            !ui_contains_ignore_case(devs[i].ip, kw)) {
            continue;
        }

        alarm = (devs[i].load >= g_alarm_threshold);

        row = lv_obj_create(g_dev_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        info_label = lv_label_create(row);
        lv_label_set_text_fmt(info_label, "%s  %s",
                              devs[i].dev_name, devs[i].id);
        lv_label_set_long_mode(info_label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(info_label, 260);

        state_label = lv_label_create(row);
        lv_label_set_text_fmt(state_label, "%s  %d%%  %s",
                              devs[i].state ? "ON" : "OFF",
                              devs[i].load,
                              devs[i].ip);

        /* 告警行文字变红 */
        if (alarm) {
            lv_obj_set_style_text_color(info_label, lv_color_hex(0xFF0000),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_color(state_label, lv_color_hex(0xFF0000),
                                        LV_PART_MAIN);
        }

        /* 保存按钮对应的设备信息，供回调使用 */
        snprintf(g_btn_data[disp].id, sizeof(g_btn_data[disp].id), "%s",
                 devs[i].id);
        g_btn_data[disp].state = devs[i].state;

        btn = lv_button_create(row);
        lv_obj_set_size(btn, 72, 40);
        btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, devs[i].state ? "关闭" : "开启");
        lv_obj_center(btn_label);
        lv_obj_add_event_cb(btn, ui_send_ctrl_cb, LV_EVENT_CLICKED,
                            &g_btn_data[disp]);

        del_btn = lv_button_create(row);
        lv_obj_set_size(del_btn, 64, 40);
        del_btn_label = lv_label_create(del_btn);
        lv_label_set_text(del_btn_label, "删除");
        lv_obj_center(del_btn_label);
        lv_obj_add_event_cb(del_btn, ui_del_device_cb, LV_EVENT_CLICKED,
                            &g_btn_data[disp]);

        disp++;
    }

    if (disp == 0) {
        lv_obj_t *label = lv_label_create(g_dev_list);
        lv_label_set_text(label, kw[0] != '\0' ? "未找到匹配设备" : "暂无设备");
    }

    /* 恢复之前的滚动位置 */
    lv_obj_update_layout(g_dev_list);
    lv_obj_scroll_to_y(g_dev_list, scroll_y, LV_ANIM_OFF);
}

/* 搜索框内容变化时立即刷新，并回到列表顶部查看匹配结果 */
static void ui_search_value_changed_cb(lv_event_t *e)
{
    (void)e;
    if (g_dev_list != NULL) {
        lv_obj_scroll_to_y(g_dev_list, 0, LV_ANIM_OFF);
    }
    ui_refresh_devices();
}

/* 刷新系统信息 */
static void ui_refresh_sysinfo(void)
{
    sys_info_t info;

    sys_info_get(&info);

    if (g_sys_cpu_bar != NULL) {
        lv_bar_set_value(g_sys_cpu_bar, info.cpu_usage, LV_ANIM_OFF);
    }
    if (g_sys_mem_bar != NULL) {
        lv_bar_set_value(g_sys_mem_bar, info.mem_usage, LV_ANIM_OFF);
    }
    if (g_sys_cpu_label != NULL) {
        lv_label_set_text_fmt(g_sys_cpu_label, "CPU: %d%%", info.cpu_usage);
    }
    if (g_sys_mem_label != NULL) {
        lv_label_set_text_fmt(g_sys_mem_label, "内存: %d%%", info.mem_usage);
    }
}

/* 加载中文字体（如果 Freetype 可用） */
static void ui_init_font(void)
{
    g_ui_font = lv_freetype_font_create(
        "lvgl/examples/libs/freetype/simkai.ttf",
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
        20,
        LV_FREETYPE_FONT_STYLE_NORMAL);

    if (g_ui_font != NULL) {
        lv_style_init(&g_ui_style);
        lv_style_set_text_font(&g_ui_style, g_ui_font);
        lv_obj_add_style(lv_screen_active(), &g_ui_style, 0);
    } else {
        LV_LOG_WARN("freetype font create failed, use default font");
    }
}

/* LVGL 定时器回调：周期刷新界面和接收消息 */
static void ui_timer_cb(lv_timer_t *timer)
{
    msg_t msg;

    (void)timer;

    /* 确保业务队列已打开 */
    if (g_biz_mq == NULL) {
        msg_bus_open(MSG_QUEUE_BIZ_NAME, &g_biz_mq);
    }

    /* 排空 UI 消息队列，避免 sys_info 发送时队列满 */
    if (g_ui_mq != NULL) {
        while (msg_bus_recv_timeout(g_ui_mq, &msg, 0) == 0) {
            /* 当前直接读取 sys_info 缓存，消息仅用于排空 */
        }
    }

    /* 如果 HTTP 或其他入口修改了告警阈值，界面同步更新 */
    {
        app_config_t cfg;
        if (load_config(&cfg) == 0 && cfg.alarm_threshold != g_alarm_threshold) {
            g_alarm_threshold = cfg.alarm_threshold;
            if (g_thresh_label != NULL) {
                lv_label_set_text_fmt(g_thresh_label, "告警阈值: %d%%",
                                      g_alarm_threshold);
            }
        }
    }

    ui_refresh_devices();
    ui_refresh_sysinfo();
}

/* 创建设备列表页 */
static void ui_create_device_tab(lv_obj_t *tab)
{
    lv_obj_t *title = lv_label_create(tab);
    lv_obj_t *top_row;
    lv_obj_t *lab;
    lv_obj_t *btn;
    lv_obj_t *btn_label;

    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);

    lv_label_set_text(title, "设备列表");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00BCD4), LV_PART_MAIN);

    top_row = lv_obj_create(tab);
    lv_obj_set_width(top_row, lv_pct(100));
    lv_obj_set_height(top_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(top_row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);

    /* 查找设备：按 ID/IP 过滤 */
    lab = lv_label_create(top_row);
    lv_label_set_text(lab, "查找:");
    lv_obj_set_width(lab, 60);

    g_search_ta = lv_textarea_create(top_row);
    lv_textarea_set_one_line(g_search_ta, true);
    lv_textarea_set_placeholder_text(g_search_ta, "ID/IP");
    lv_textarea_set_max_length(g_search_ta, 64);
    lv_obj_set_width(g_search_ta, 200);
    lv_obj_add_event_cb(g_search_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);
    lv_obj_add_event_cb(g_search_ta, ui_search_value_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    btn = lv_button_create(top_row);
    lv_obj_set_size(btn, 80, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "清除");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_clear_search_cb, LV_EVENT_CLICKED, NULL);

    g_dev_list = lv_obj_create(tab);
    lv_obj_set_width(g_dev_list, lv_pct(100));
    lv_obj_set_flex_grow(g_dev_list, 1);
    lv_obj_set_height(g_dev_list, 0);
    lv_obj_set_flex_flow(g_dev_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_dev_list, 8, LV_PART_MAIN);
    lv_obj_add_flag(g_dev_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_dev_list, LV_DIR_VER);
}

/* 创建系统信息页 */
static void ui_create_sys_tab(lv_obj_t *tab)
{
    lv_obj_t *row;
    lv_obj_t *label;
    lv_obj_t *btn;
    lv_obj_t *btn_label;

    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 16, LV_PART_MAIN);

    g_sys_cpu_label = lv_label_create(tab);
    lv_label_set_text(g_sys_cpu_label, "CPU: --%");

    g_sys_cpu_bar = lv_bar_create(tab);
    lv_obj_set_width(g_sys_cpu_bar, lv_pct(100));
    lv_obj_set_height(g_sys_cpu_bar, 24);
    lv_bar_set_range(g_sys_cpu_bar, 0, 100);

    g_sys_mem_label = lv_label_create(tab);
    lv_label_set_text(g_sys_mem_label, "内存: --%");

    g_sys_mem_bar = lv_bar_create(tab);
    lv_obj_set_width(g_sys_mem_bar, lv_pct(100));
    lv_obj_set_height(g_sys_mem_bar, 24);
    lv_bar_set_range(g_sys_mem_bar, 0, 100);

    label = lv_label_create(tab);
    lv_label_set_text(label, "运行控制");

    /* 告警阈值调节 */
    g_thresh_label = lv_label_create(tab);
    lv_label_set_text_fmt(g_thresh_label, "告警阈值: %d%%", g_alarm_threshold);

    row = lv_obj_create(tab);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    btn = lv_button_create(row);
    lv_obj_set_size(btn, 72, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "-5");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_threshold_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(-5));

    btn = lv_button_create(row);
    lv_obj_set_size(btn, 72, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "+5");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_threshold_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(5));

    /* 清空日志 */
    btn = lv_button_create(row);
    lv_obj_set_size(btn, 96, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "清空日志");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_clear_log_cb, LV_EVENT_CLICKED, NULL);
}

/* 创建管理页：删除设备、修改参数 */
static void ui_create_manage_tab(lv_obj_t *tab)
{
    lv_obj_t *title;
    lv_obj_t *section;
    lv_obj_t *row;
    lv_obj_t *lab;
    lv_obj_t *btn;
    lv_obj_t *btn_label;

    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 12, LV_PART_MAIN);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);

    title = lv_label_create(tab);
    lv_label_set_text(title, "设备管理");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00BCD4), LV_PART_MAIN);

    /* 添加设备入口 */
    btn = lv_button_create(tab);
    lv_obj_set_size(btn, 140, 44);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "添加设备");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_open_add_popup, LV_EVENT_CLICKED, NULL);

    /* 删除设备区域 */
    section = lv_label_create(tab);
    lv_label_set_text(section, "删除设备");

    row = lv_obj_create(tab);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lab = lv_label_create(row);
    lv_label_set_text(lab, "ID:");
    lv_obj_set_width(lab, 40);

    g_del_id_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_del_id_ta, true);
    lv_textarea_set_placeholder_text(g_del_id_ta, "输入设备ID");
    lv_textarea_set_max_length(g_del_id_ta, DEVICE_ID_LEN - 1);
    lv_obj_set_width(g_del_id_ta, 200);
    lv_obj_add_event_cb(g_del_id_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    btn = lv_button_create(row);
    lv_obj_set_size(btn, 110, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "删除设备");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_del_by_id_cb, LV_EVENT_CLICKED, NULL);

    /* 修改参数区域 */
    section = lv_label_create(tab);
    lv_label_set_text(section, "修改参数");

    row = lv_obj_create(tab);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lab = lv_label_create(row);
    lv_label_set_text(lab, "ID:");
    lv_obj_set_width(lab, 40);
    g_mod_id_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_mod_id_ta, true);
    lv_textarea_set_placeholder_text(g_mod_id_ta, "设备ID");
    lv_textarea_set_max_length(g_mod_id_ta, DEVICE_ID_LEN - 1);
    lv_obj_set_width(g_mod_id_ta, 150);
    lv_obj_add_event_cb(g_mod_id_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    lab = lv_label_create(row);
    lv_label_set_text(lab, "负载:");
    lv_obj_set_width(lab, 60);
    g_mod_load_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_mod_load_ta, true);
    lv_textarea_set_placeholder_text(g_mod_load_ta, "0-100");
    lv_textarea_set_max_length(g_mod_load_ta, 3);
    lv_obj_set_width(g_mod_load_ta, 80);
    lv_obj_add_event_cb(g_mod_load_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    row = lv_obj_create(tab);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lab = lv_label_create(row);
    lv_label_set_text(lab, "名称:");
    lv_obj_set_width(lab, 60);
    g_mod_name_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(g_mod_name_ta, true);
    lv_textarea_set_placeholder_text(g_mod_name_ta, "新名称");
    lv_textarea_set_max_length(g_mod_name_ta, DEVICE_NAME_LEN - 1);
    lv_obj_set_width(g_mod_name_ta, 200);
    lv_obj_add_event_cb(g_mod_name_ta, ui_ta_event_cb, LV_EVENT_ALL, g_kb);

    btn = lv_button_create(row);
    lv_obj_set_size(btn, 110, 40);
    btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "修改参数");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, ui_modify_cb, LV_EVENT_CLICKED, NULL);
}

/* 创建主界面 */
static void ui_create(void)
{
    lv_obj_t *tabview;
    lv_obj_t *tab1;
    lv_obj_t *tab2;
    lv_obj_t *tab3;

    tabview = lv_tabview_create(lv_screen_active());
    if (g_ui_font != NULL) {
        lv_obj_add_style(tabview, &g_ui_style, 0);
    }

    /* 全局虚拟键盘，默认隐藏，文本框聚焦时弹出 */
    g_kb = lv_keyboard_create(lv_screen_active());
    lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(g_kb, LV_OBJ_FLAG_HIDDEN);

    tab1 = lv_tabview_add_tab(tabview, "设备");
    tab2 = lv_tabview_add_tab(tabview, "系统");
    tab3 = lv_tabview_add_tab(tabview, "管理");

    ui_create_device_tab(tab1);
    ui_create_sys_tab(tab2);
    ui_create_manage_tab(tab3);

    /* 创建周期刷新定时器 */
    lv_timer_create(ui_timer_cb, UI_REFRESH_MS, NULL);
}

/* 启动 LVGL 界面线程 */
int lv_ui_start(pthread_t *tid)
{
    if (tid == NULL) return -1;
    g_stop = 0;
    return pthread_create(tid, NULL, lv_ui_thread, NULL);
}

/* 请求停止 LVGL 界面线程 */
int lv_ui_stop(void)
{
    g_stop = 1;
    return 0;
}

/* LVGL 界面线程主循环 */
void *lv_ui_thread(void *arg)
{
    app_config_t cfg;
    uint32_t idle_time;

    (void)arg;

    /* 读取告警阈值初始值 */
    if (load_config(&cfg) == 0) {
        g_alarm_threshold = cfg.alarm_threshold;
    }

    /* 打开/创建 UI 消息队列 */
    if (msg_bus_open(MSG_QUEUE_UI_NAME, &g_ui_mq) != 0) {
        msg_bus_create(MSG_QUEUE_UI_NAME, &g_ui_mq);
    }

    /* 初始化 LVGL */
    lv_init();

    /* 注册并初始化显示/输入后端 */
    driver_backends_register();
    if (driver_backends_init_backend(NULL) != 0) {
        fprintf(stderr, "[lv_ui] init display backend failed\n");
        return NULL;
    }

#if LV_USE_EVDEV
    if (driver_backends_init_backend("EVDEV") != 0) {
        fprintf(stderr, "[lv_ui] init evdev failed\n");
    }
#endif

    /* 显示后端初始化完成后才能获取 active screen，再加载中文字体 */
    ui_init_font();

    /* 创建界面 */
    ui_create();

    /* 打开业务消息队列，供控制按钮使用 */
    msg_bus_open(MSG_QUEUE_BIZ_NAME, &g_biz_mq);

    /* 进入 LVGL 事件循环 */
    while (!g_stop) {
        idle_time = lv_timer_handler();
        if (idle_time > 20) idle_time = 20;
        if (idle_time < 5) idle_time = 5;
        usleep(idle_time * 1000);
    }

    return NULL;
}
