/**
 * @file    http_server.c
 * @brief   简易 HTTP 服务（纯 socket 实现，无第三方库）
 *
 * 功能：
 *  - 提供网页和设备列表接口
 *  - 支持设备开关控制
 *  - 支持手动添加、删除、修改设备
 *  - 支持日志下载与清空
 *  - 支持查看系统信息
 *  - 支持设置告警阈值
 */
#include "http_server.h"

#include "device_list.h"
#include "msg_bus.h"
#include "sys_info.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_BUF_SIZE   4096
#define MAX_HTTP_DEVICES 64

static volatile int g_stop = 0;
static msg_bus_t g_mq_biz = NULL;
static app_config_t g_cfg;

static const char INDEX_HTML[] =
    "<!DOCTYPE html>"
    "<html><head><meta charset=\"utf-8\">"
    "<title>Gateway Device Monitor</title>"
    "<style>"
    "body{font-family:sans-serif;background:#101418;color:#eee;margin:20px;}"
    "table{border-collapse:collapse;width:100%;margin-top:12px;}"
    "th,td{border:1px solid #333;padding:8px 12px;text-align:left;}"
    "th{background:#1c2228;}"
    "button{padding:4px 14px;cursor:pointer;}"
    ".on{color:#7CFC00;}.off{color:#FF6B6B;}"
    "</style></head><body>"
    "<h1>Gateway Device Monitor</h1>"
    "<p id=\"status\">Loading...</p>"
    "<table id=\"devices\"><thead><tr>"
    "<th>ID</th><th>Name</th><th>IP</th><th>State</th><th>Load</th><th>Control</th>"
    "</tr></thead><tbody></tbody></table>"
    "<script>"
    "const tbody=document.querySelector('#devices tbody');"
    "const statusEl=document.getElementById('status');"
    "async function refresh(){"
    "try{const r=await fetch('/api/devices');const list=await r.json();"
    "statusEl.textContent='devices: '+list.length;tbody.innerHTML='';"
    "list.forEach(d=>{const tr=document.createElement('tr');"
    "const st=d.state?'on':'off';const stt=d.state?'ON':'OFF';"
    "tr.innerHTML=`<td>${d.id}</td><td>${d.dev_name}</td><td>${d.ip}</td>"
    "<td class=\"${st}\">${stt}</td><td>${d.load}%</td>"
    "<td><button onclick=\"ctrl('${d.id}','${d.state?'off':'on'}')\">${d.state?'OFF':'ON'}</button></td>`;"
    "tbody.appendChild(tr);});"
    "}catch(e){statusEl.textContent='fetch error';}}"
    "async function ctrl(id,cmd){"
    "const body='id='+encodeURIComponent(id)+'&cmd='+encodeURIComponent(cmd);"
    "await fetch('/api/control',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});"
    "refresh();}"
    "refresh();setInterval(refresh,2000);"
    "</script></body></html>";

typedef struct {
    int fd;
} client_arg_t;

/* 向 socket 完整发送一段数据 */
static int send_all(int fd, const char *data)
{
    size_t len = strlen(data);
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* 发送 HTTP 响应 */
static void send_response(int fd, int code, const char *ctype, const char *body)
{
    char header[512];
    const char *reason = "OK";

    if (code == 404) reason = "Not Found";
    else if (code == 400) reason = "Bad Request";
    else if (code == 500) reason = "Internal Server Error";

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             code, reason, ctype, strlen(body));

    send_all(fd, header);
    send_all(fd, body);
}

/* 发送纯文本 HTTP 响应 */
static void send_simple(int fd, int code, const char *body)
{
    send_response(fd, code, "text/plain; charset=utf-8", body);
}

/* 构建设备列表 JSON 字符串 */
static void build_devices_json(char *buf, size_t cap)
{
    device_t devs[MAX_HTTP_DEVICES];
    size_t count = 0;
    size_t i;
    int len = 0;

    if (buf == NULL || cap < 3) return;

    device_get_all(devs, MAX_HTTP_DEVICES, &count);

    buf[0] = '[';
    len = 1;

    for (i = 0; i < count; i++) {
        int n = snprintf(buf + len, cap - len,
                         "%s{\"id\":\"%s\",\"dev_name\":\"%s\",\"ip\":\"%s\","
                         "\"state\":%d,\"load\":%d,\"online_sec\":%u}",
                         (i == 0) ? "" : ",",
                         devs[i].id, devs[i].dev_name, devs[i].ip,
                         devs[i].state, devs[i].load, devs[i].online_time);

        if (n < 0 || (size_t)n >= cap - len) {
            len = (int)cap - 1;
            break;
        }
        len += n;
    }

    if (len + 1 < (int)cap) {
        buf[len++] = ']';
        buf[len] = '\0';
    } else {
        buf[cap - 1] = '\0';
    }
}

/* 将十六进制字符转换为数值 */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL 解码，支持 %XX 和 + 号 */
static void url_decode(char *s)
{
    char *d = s;

    while (*s != '\0') {
        if (*s == '+') {
            *d++ = ' ';
            s++;
        } else if (*s == '%' && s[1] != '\0' && s[2] != '\0') {
            int hi = hex_value(s[1]);
            int lo = hex_value(s[2]);

            if (hi >= 0 && lo >= 0) {
                *d++ = (char)((hi << 4) | lo);
                s += 3;
            } else {
                *d++ = *s++;
            }
        } else {
            *d++ = *s++;
        }
    }

    *d = '\0';
}

/* 从 POST 表单数据中查找指定参数 */
static int find_param(const char *body, const char *key,
                      char *out, size_t out_cap)
{
    const char *p;
    size_t key_len = strlen(key);

    if (body == NULL || key == NULL) return -1;

    p = body;
    while (*p) {
        const char *amp;

        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            size_t n;

            amp = strchr(val, '&');
            if (amp == NULL) {
                n = strlen(val);
            } else {
                n = (size_t)(amp - val);
            }

            if (n >= out_cap) n = out_cap - 1;
            memcpy(out, val, n);
            out[n] = '\0';
            url_decode(out);
            return 0;
        }

        amp = strchr(p, '&');
        if (amp == NULL) break;
        p = amp + 1;
    }

    return -1;
}

/* 从两个候选参数名中查找一个参数 */
static int find_param_either(const char *body, const char *key1, const char *key2,
                             char *out, size_t out_cap)
{
    if (find_param(body, key1, out, out_cap) == 0) {
        return 0;
    }
    return find_param(body, key2, out, out_cap);
}


/* 将日志文件内容作为 HTTP 响应发送 */
static void send_log_file(int fd)
{
    FILE *fp;
    char path[PATH_LEN];
    long sz;
    char *buf;
    size_t rd;
    const char *log_path = (g_cfg.log_file[0] != '\0') ? g_cfg.log_file : "log/history.log";

    snprintf(path, sizeof(path), "%s", log_path);

    fp = fopen(path, "rb");
    if (fp == NULL) {
        send_simple(fd, 200, "");
        return;
    }

    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0 || sz > 1024 * 1024) {
        fclose(fp);
        send_simple(fd, 500, "500 log too large");
        return;
    }

    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(fp);
        send_simple(fd, 500, "500 no memory");
        return;
    }

    rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';

    {
        char header[512];

        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Disposition: attachment; filename=\"history.log\"\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 rd);
        send_all(fd, header);
        send_all(fd, buf);
    }
    free(buf);
}

/* 处理单个 HTTP 客户端请求 */
static void handle_client(int fd)
{
    char req[HTTP_BUF_SIZE];
    char method[8] = {0};
    char path[256] = {0};
    char *body = NULL;
    ssize_t n;

    n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = '\0';

    sscanf(req, "%7s %255s", method, path);

    body = strstr(req, "\r\n\r\n");
    if (body != NULL) body += 4;

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            send_response(fd, 200, "text/html; charset=utf-8", INDEX_HTML);
        } else if (strcmp(path, "/api/devices") == 0) {
            char json[HTTP_BUF_SIZE * 4];
            build_devices_json(json, sizeof(json));
            send_response(fd, 200, "application/json; charset=utf-8", json);
        } else if (strcmp(path, "/api/log/download") == 0) {
            send_log_file(fd);
        } else if (strcmp(path, "/api/sysinfo") == 0) {
            sys_info_t info;
            char json[128];

            sys_info_get(&info);
            snprintf(json, sizeof(json), "{\"cpu\":%d,\"mem\":%d}",
                     info.cpu_usage, info.mem_usage);
            send_response(fd, 200, "application/json; charset=utf-8", json);
        } else {
            send_simple(fd, 404, "404 Not Found");
        }
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/control") == 0) {
            char id[DEVICE_ID_LEN] = {0};
            char cmd[CMD_LEN] = {0};
            msg_t msg;

            if (find_param(body, "id", id, sizeof(id)) != 0 ||
                find_param(body, "cmd", cmd, sizeof(cmd)) != 0) {
                send_simple(fd, 400, "{\"ok\":false,\"error\":\"missing id/cmd\"}");
                close(fd);
                return;
            }

            msg_bus_msg_init(&msg);
            msg.type = MSG_DEV_CTRL;
            snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);
            snprintf(msg.cmd, sizeof(msg.cmd), "%s", cmd);
            snprintf(msg.from, sizeof(msg.from), "http");

            if (msg_bus_send(g_mq_biz, &msg) != 0) {
                send_simple(fd, 500, "{\"ok\":false,\"error\":\"mq send failed\"}");
            } else {
                send_simple(fd, 200, "{\"ok\":true}");
            }
        } else if (strcmp(path, "/api/dev/add") == 0) {
            char id[DEVICE_ID_LEN] = {0};
            char ip[DEVICE_IP_LEN] = {0};
            char name[DEVICE_NAME_LEN] = {0};
            char load[16] = {0};
            msg_t msg;

            if (find_param(body, "id", id, sizeof(id)) != 0 ||
                find_param(body, "ip", ip, sizeof(ip)) != 0 ||
                find_param_either(body, "dev_name", "name", name, sizeof(name)) != 0 ||
                find_param(body, "load", load, sizeof(load)) != 0) {
                send_simple(fd, 400, "{\"ok\":false,\"error\":\"missing params\"}");
                close(fd);
                return;
            }

            msg_bus_msg_init(&msg);
            msg.type = MSG_DEV_ADD_MANUAL;
            snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);
            snprintf(msg.dev.ip, sizeof(msg.dev.ip), "%s", ip);
            snprintf(msg.dev.dev_name, sizeof(msg.dev.dev_name), "%s", name);
            msg.dev.load = atoi(load);
            msg.dev.state = 1;
            snprintf(msg.from, sizeof(msg.from), "http");

            if (msg_bus_send(g_mq_biz, &msg) != 0) {
                send_simple(fd, 500, "{\"ok\":false,\"error\":\"mq send failed\"}");
            } else {
                send_simple(fd, 200, "{\"ok\":true}");
            }
        } else if (strcmp(path, "/api/dev/del") == 0) {
            char id[DEVICE_ID_LEN] = {0};
            msg_t msg;

            if (find_param(body, "id", id, sizeof(id)) != 0) {
                send_simple(fd, 400, "{\"ok\":false,\"error\":\"missing id\"}");
                close(fd);
                return;
            }

            msg_bus_msg_init(&msg);
            msg.type = MSG_DEV_DEL_MANUAL;
            snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);
            snprintf(msg.from, sizeof(msg.from), "http");

            if (msg_bus_send(g_mq_biz, &msg) != 0) {
                send_simple(fd, 500, "{\"ok\":false,\"error\":\"mq send failed\"}");
            } else {
                send_simple(fd, 200, "{\"ok\":true}");
            }
        } else if (strcmp(path, "/api/dev/modify") == 0) {
            char id[DEVICE_ID_LEN] = {0};
            char load[16] = {0};
            char name[DEVICE_NAME_LEN] = {0};
            msg_t msg;

            if (find_param(body, "id", id, sizeof(id)) != 0 ||
                find_param(body, "load", load, sizeof(load)) != 0 ||
                find_param_either(body, "dev_name", "name", name, sizeof(name)) != 0) {
                send_simple(fd, 400, "{\"ok\":false,\"error\":\"missing params\"}");
                close(fd);
                return;
            }

            msg_bus_msg_init(&msg);
            msg.type = MSG_DEV_MODIFY_PARAM;
            snprintf(msg.dev.id, sizeof(msg.dev.id), "%s", id);
            msg.new_load = atoi(load);
            snprintf(msg.new_name, sizeof(msg.new_name), "%s", name);
            snprintf(msg.from, sizeof(msg.from), "http");

            if (msg_bus_send(g_mq_biz, &msg) != 0) {
                send_simple(fd, 500, "{\"ok\":false,\"error\":\"mq send failed\"}");
            } else {
                send_simple(fd, 200, "{\"ok\":true}");
            }
        } else if (strcmp(path, "/api/log/clear") == 0) {
            msg_t msg;

            msg_bus_msg_init(&msg);
            msg.type = MSG_LOG_CLEAR;
            snprintf(msg.from, sizeof(msg.from), "http");

            if (msg_bus_send(g_mq_biz, &msg) != 0) {
                send_simple(fd, 500, "{\"ok\":false,\"error\":\"mq send failed\"}");
            } else {
                send_simple(fd, 200, "{\"ok\":true}");
            }
        } else if (strcmp(path, "/api/alarm/threshold") == 0) {
            char threshold[16] = {0};
            msg_t msg;

            if (find_param(body, "threshold", threshold, sizeof(threshold)) != 0) {
                send_simple(fd, 400, "{\"ok\":false,\"error\":\"missing threshold\"}");
                close(fd);
                return;
            }

            msg_bus_msg_init(&msg);
            msg.type = MSG_SET_ALARM_THRESH;
            msg.alarm_threshold = atoi(threshold);
            snprintf(msg.from, sizeof(msg.from), "http");

            if (msg_bus_send(g_mq_biz, &msg) != 0) {
                send_simple(fd, 500, "{\"ok\":false,\"error\":\"mq send failed\"}");
            } else {
                send_simple(fd, 200, "{\"ok\":true}");
            }
        } else {
            send_simple(fd, 404, "404 Not Found");
        }
    } else {
        send_simple(fd, 400, "400 Bad Request");
    }

    close(fd);
}

/* HTTP 客户端处理线程 */
static void *client_thread(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;

    free(ca);
    handle_client(fd);
    return NULL;
}

/* 启动 HTTP 服务线程 */
int http_server_start(pthread_t *tid, const app_config_t *cfg)
{
    if (tid == NULL || cfg == NULL) return -1;

    g_cfg = *cfg;
    g_stop = 0;
    return pthread_create(tid, NULL, http_server_thread, NULL);
}

/* 请求停止 HTTP 服务 */
int http_server_stop(void)
{
    g_stop = 1;
    return 0;
}

/* HTTP 服务线程主循环 */
void *http_server_thread(void *arg)
{
    int listen_fd;
    struct sockaddr_in addr;
    struct timeval tv;
    int opt = 1;

    (void)arg;

    if (msg_bus_open(MSG_QUEUE_BIZ_NAME, &g_mq_biz) != 0) {
        fprintf(stderr, "[http] open biz mq failed\n");
        return NULL;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("http socket");
        msg_bus_close_handle(&g_mq_biz);
        return NULL;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)g_cfg.http_port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("http bind");
        close(listen_fd);
        msg_bus_close_handle(&g_mq_biz);
        return NULL;
    }

    if (listen(listen_fd, 8) < 0) {
        perror("http listen");
        close(listen_fd);
        msg_bus_close_handle(&g_mq_biz);
        return NULL;
    }

    printf("[http] listening on port %d\n", g_cfg.http_port);

    while (!g_stop) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cli_fd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);

        if (cli_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            continue;
        }

        client_arg_t *ca = (client_arg_t *)malloc(sizeof(*ca));
        if (ca != NULL) {
            pthread_t th;
            ca->fd = cli_fd;
            if (pthread_create(&th, NULL, client_thread, ca) == 0) {
                pthread_detach(th);
            } else {
                free(ca);
                close(cli_fd);
            }
        } else {
            close(cli_fd);
        }
    }

    close(listen_fd);
    msg_bus_close_handle(&g_mq_biz);
    return NULL;
}