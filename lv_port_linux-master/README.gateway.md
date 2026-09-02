# Gateway 集成说明（LVGL 9.4 / RK3568）

Gateway 业务代码已经合并到本工程的 `src/`、`include/`、`config/`、`log/`、`test/` 中。
原来的 LVGL 示例入口已备份为 `src/lvgl_main.c.bak`，当前 `src/main.c` 是 Gateway 入口。

## 编译（推荐使用 Makefile，交叉编译器为 aarch64-linux-gcc/g++）

```bash
cd rk3568/lv_port_linux-master
make clean
make -j
```

如果交叉编译器前缀不是 `aarch64-linux-`，请修改 `Makefile` 开头：

```make
CC  = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++
```

编译成功后生成：

```
build/bin/main
```

## 运行

开发板上需要把整个工程目录（至少包含 `build/bin/main`、`config/`、`log/`）放到板子上，
然后在工程根目录运行：

```bash
cd /path/to/lv_port_linux-master
./build/bin/main
```

或者只拷贝可执行文件时，也要把 `config/config.conf` 和 `log/` 放到与运行时当前目录对应的位置，
因为程序默认使用相对路径 `config/config.conf`、`log/history.log`。

启动后终端会打印 gateway 启动信息，浏览器访问：

```
http://<开发板IP>:8080
```

## 中文字体

界面会尝试加载：

```text
lvgl/examples/libs/freetype/simkai.ttf
```

如果只拷贝可执行文件到板子，请把该字体文件也按相同相对路径放到板子上，否则中文会显示为方框/默认字体。

## 当前状态

- 已实现 LVGL 9.4 开发板界面：
  - 设备列表显示与开关控制
  - 设备列表支持按 ID/IP 搜索过滤
  - 添加设备弹窗输入（ID、名称、IP、负载）
  - 删除设备区域（输入 ID 删除）
  - 修改参数区域（ID、负载、名称）
  - 系统 CPU/内存信息显示
  - 告警阈值调节
  - 任意设备负载达到阈值时，该行文字变红，并点亮板载 LED
- 消息总线已改为进程内 pthread 环形队列，不再依赖内核 `CONFIG_POSIX_MQUEUE`。
