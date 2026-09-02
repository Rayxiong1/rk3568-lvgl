/**
 * @file    test_device_list.c
 * @brief   单元测试：设备双向链表模块测试桩
 *
 * 自测点：
 *  1. 重复添加同一设备不会产生重复节点
 *  2. 删除不存在设备不崩溃
 *  3. 遍历/获取全部设备副本正常
 *  4. 修改设备负载和名称
 */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "device_list.h"

static void print_device(const device_t *dev)
{
    if (dev == NULL) return;
    printf("  id=%s ip=%s name=%s state=%d load=%d online=%u\n",
           dev->id, dev->ip, dev->dev_name, dev->state, dev->load, dev->online_time);
}

static void print_all(const char *title)
{
    device_t buf[16];
    size_t count = 0;
    size_t i;

    printf("--- %s ---\n", title);

    if (device_get_all(buf, 16, &count) != 0) {
        printf("  [warning] device_get_all truncated\n");
    }

    printf("  total=%u\n", (unsigned)count);
    for (i = 0; i < count; i++) {
        print_device(&buf[i]);
    }
}

int main(void)
{
    device_t dev1;
    device_t dev2;
    device_t out;
    size_t count = 0;
    device_t all[16];

    printf("===== device_list test start =====\n");

    /* 1. 初始化 */
    device_list_init();

    /* 2. 模拟设备上线 */
    memset(&dev1, 0, sizeof(dev1));
    snprintf(dev1.id, sizeof(dev1.id), "dev-001");
    snprintf(dev1.ip, sizeof(dev1.ip), "192.168.1.101");
    snprintf(dev1.dev_name, sizeof(dev1.dev_name), "Device1");
    dev1.state = 1;
    dev1.load = 35;
    dev1.online_time = 10;

    memset(&dev2, 0, sizeof(dev2));
    snprintf(dev2.id, sizeof(dev2.id), "dev-002");
    snprintf(dev2.ip, sizeof(dev2.ip), "192.168.1.102");
    snprintf(dev2.dev_name, sizeof(dev2.dev_name), "Device2");
    dev2.state = 0;
    dev2.load = 10;
    dev2.online_time = 5;

    printf("add dev-001 => %d\n", device_add(&dev1));
    printf("add dev-002 => %d\n", device_add(&dev2));
    print_all("after add two devices");

    /* 3. 重复添加同一设备：不应产生重复节点 */
    dev1.load = 66;
    dev1.online_time = 20;
    if (device_add(&dev1) != 0) {
        printf("[FAIL] add duplicate dev-001 failed\n");
        return -1;
    }
    print_all("after add duplicate dev-001");

    if (device_get_all(all, 16, &count) != 0) {
        printf("device_get_all failed\n");
        return -1;
    }
    if (count != 2) {
        printf("[FAIL] expected 2 devices, got %u\n", (unsigned)count);
        return -1;
    }
    printf("[PASS] duplicate add does not create duplicate node\n");

    /* 4. 查找设备 */
    if (device_find_by_id("dev-001", &out) != 1) {
        printf("[FAIL] find dev-001 failed\n");
        return -1;
    }
    printf("find dev-001 => load=%d online=%u\n", out.load, out.online_time);
    printf("[PASS] find_by_id returns copy\n");

    /* 5. 修改设备参数：负载和名称 */
    if (device_modify_param("dev-001", 88, "Device1-Renamed") != 1) {
        printf("[FAIL] modify dev-001 should return 1\n");
        return -1;
    }
    if (device_find_by_id("dev-001", &out) != 1) {
        printf("[FAIL] find dev-001 after modify failed\n");
        return -1;
    }
    if (out.load != 88 || strcmp(out.dev_name, "Device1-Renamed") != 0) {
        printf("[FAIL] modify dev-001 load/name not applied: load=%d name=%s\n",
               out.load, out.dev_name);
        return -1;
    }
    printf("[PASS] device_modify_param updates load and name\n");

    /* 6. 修改设备参数：new_name 为 NULL 时只改负载 */
    if (device_modify_param("dev-001", 77, NULL) != 1) {
        printf("[FAIL] modify dev-001 load-only should return 1\n");
        return -1;
    }
    if (device_find_by_id("dev-001", &out) != 1) {
        printf("[FAIL] find dev-001 after load-only modify failed\n");
        return -1;
    }
    if (out.load != 77 || strcmp(out.dev_name, "Device1-Renamed") != 0) {
        printf("[FAIL] load-only modify wrong: load=%d name=%s\n",
               out.load, out.dev_name);
        return -1;
    }
    printf("[PASS] device_modify_param with NULL name only updates load\n");

    /* 7. 修改不存在设备：不崩溃，返回 0 */
    if (device_modify_param("dev-999", 50, "Ghost") != 0) {
        printf("[FAIL] modify non-exist device should return 0\n");
        return -1;
    }
    printf("[PASS] modify non-exist device no crash\n");

    /* 8. 删除不存在设备：不崩溃，返回 0 */
    if (device_del_by_id("dev-999") != 0) {
        printf("[FAIL] delete non-exist device should return 0\n");
        return -1;
    }
    printf("[PASS] delete non-exist device no crash\n");

    /* 9. 删除设备 */
    if (device_del_by_id("dev-001") != 1) {
        printf("[FAIL] delete dev-001 should return 1\n");
        return -1;
    }
    print_all("after delete dev-001");

    if (device_get_all(all, 16, &count) != 0) {
        printf("device_get_all failed\n");
        return -1;
    }
    if (count != 1) {
        printf("[FAIL] expected 1 device, got %u\n", (unsigned)count);
        return -1;
    }
    printf("[PASS] delete device normal\n");

    /* 10. 销毁链表 */
    device_list_destroy();
    printf("device_list_destroy ok\n");

    printf("===== device_list test end =====\n");
    return 0;
}
