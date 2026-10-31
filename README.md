# chopin initramfs —— `recovery` 分支

> 另一个分支：**`rootfs`** —— 设备实际启动用的极简 PID 1（557 行）
> 本分支：**`recovery`** —— 救援镜像（4416 行，含全套硬件诊断）

## 这是什么

Redmi Note 10 Pro（MT6891 / "chopin"）主线 Linux 的**救援 initramfs**：
Alpine 用户态 + 一个静态链接的 C 程序作为 PID 1，用来在无串口条件下
摸清硬件、验证驱动、抢救数据。

```sh
make recovery     # → out/initramfs.cpio.gz（约 1 MB）
```

## 为什么需要它

这台机器**没有可用的串口**，唯一控制台是 USB gadget 的 `ttyGS0`。
内核早期、驱动 bring-up、面板点亮这些阶段出问题时，需要一个能在
userspace 里直接读寄存器、切 USB 角色、dump 设备树的救援环境。

## 内置诊断命令

init 提供 shell，带这些直接操作 sysfs/devfs 的命令：

| 命令 | 作用 |
| --- | --- |
| `i2creg <bus> <addr> <reg> [n]` | 直接读写 I2C 寄存器（定位过电量计读全 0xFF 的问题） |
| `power` / `battery` | 电源与电量计状态 |
| `usbhost` | 切换 USB 角色、重新绑定 gadget |
| 设备树 / framebuffer / 内核日志 dump | 无串口时的"黑匣子" |

## 几个非显然的设计（代码注释里都有）

### 固件必须最先加载

```c
/* The wlan driver requests its NVRAM blob a couple of seconds after boot and
 * the conninfra pre-calibration waits only ~30s for that request */
auto_fwload();
```

WiFi 的 NVRAM 晚于 conninfra 的 30 秒预校准窗口 → **连接芯片永久卡死**。

### 音量键要在开机瞬间锁存

面板约 12 秒后才亮，那时按键早松开了，所以开机时就要锁存音量上键状态。

### 诊断不能晚于 shell

`print_diagnostics()` 会 dump 整个内核日志，可能几十秒；顺序错了
USB 主机等不到控制台。

### recovery 不依赖主机在线

早期版本把 recovery 和启动都放在 `if (sh_fd >= 0)` 里，
结果**没插 USB 就永远停在 initramfs**。

## 构建

```sh
make recovery
INITRAMFS_MODE=bringup KERNEL=... DTB=... \
STOCK_DTB=<...>/git/chopin-support/dtb/stock-base.dtb \
./tools/build-images.sh out-image
```

`STOCK_DTB` 是厂商 DTB（原机 boot 分区），**必须**随镜像携带：
LK 会校验，自有 DTB 在 `setup_arch()` 里替换。

## 相关仓库

| 仓库 | 内容 |
| --- | --- |
| `git/chopin-linux-7.3` | 内核与设备树 |
| `git/chopin-support` | 固件（含厂商 DTB）、服务、配置 |
| `git/chopin-docs` | 主题文档 |
