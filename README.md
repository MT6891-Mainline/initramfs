# chopin boot initramfs

Redmi Note 10 Pro（MT6891 / "chopin"）主线 Linux 的**启动 init**：
一个静态链接的 C 程序，作为 initramfs 里的 PID 1。

不是 busybox + 脚本，而是 4400 行 C，原因是这台机器**没有串口可用**
（只能用 USB gadget 的 `ttyGS0`），而且很多问题（固件加载时序、面板点亮、
SCP 释放）必须在 userspace 极早期处理。

## 启动链

```
preloader → LK → GenieZone → Linux → 本 init（PID 1）
```

没有 U-Boot / UEFI：boot 镜像由 `fastboot flash boot_a` 写入，
内含厂商 DTB + 内核 + 本 initramfs。

## 两种模式

### `rootfs`（设备上实际使用的）

initramfs 里**没有任何用户态**，只做这几件事然后 switch_root 到发行版：

1. 挂 `/proc /sys /dev`，把 stdout 重定向到 `/dev/kmsg`
2. 打开 USB gadget 控制台 `/dev/ttyGS0`
3. 早期加载厂商固件（见下）
4. **确定根设备**：内核命令行 `chopin.root=/dev/sdc86`，
   或 initramfs 里烘焙的 `/etc/chopin/rootfs`
5. `boot_rootfs(dev)` → 挂载、`switch_root` → systemd 接管

### `bringup`（救援/诊断）

带 Alpine 用户态、诊断输出和 USB shell，用来在刷机前验证硬件。

## 几个非显然的设计

### 固件必须最先加载

```c
/* The wlan driver requests its NVRAM blob a couple of seconds after boot and
 * the conninfra pre-calibration waits only ~30s for that request to finish. */
if (rootdev) auto_fwload();
```

WiFi 的 NVRAM 如果晚于 conninfra 的 30 秒预校准窗口到达，
**连接芯片会永久卡死**。所以固件在诊断输出之前就要写好。

### 诊断不能晚于 shell

`print_diagnostics()` 会 dump 整个内核日志，可能耗时几十秒。
顺序错了会让 USB 主机等不到控制台。

### 音量键要在开机瞬间锁定

```c
/* the display (and therefore the recovery UI) only appears ~12s in,
 * long after a user has let go of the key */
volup_at_boot = keypad_hw_down(0);
```

12 秒后面板才亮，那时按键早松开了，所以开机时就要把音量上键的状态锁存下来。

### rootfs 模式下不碰显示

```c
/* In rootfs mode systemd/GDM owns the display: the bring-up child leaves the
 * DSI panel powered off after its modeset and the greeter then renders into a
 * blank panel. */
if (!rootdev && fork() == 0) display_bringup();
```

### recovery 不依赖主机在线

早期版本把 recovery 和根文件系统启动都放在 `if (sh_fd >= 0)` 里，
结果**没插 USB 就会永远停在 initramfs**。现在两者独立判断。

## 内置的诊断命令

init 在 bringup 模式提供 shell，带这些命令（都直接操作 sysfs/devfs）：

| 命令 | 作用 |
| --- | --- |
| `i2creg <bus> <addr> <reg> [n]` | 直接读写 I2C 寄存器（定位过电量计问题） |
| `power` / `battery` | 电源与电量计状态 |
| `usbhost` | 切换 USB 角色、重新绑定 gadget |
| 设备树 / framebuffer / 内核日志 dump | 无串口时的"黑匣子" |

## 构建

```sh
# 设备上用的形态
make rootfs      # → out/initramfs.cpio.gz

# 救援形态（需要 Alpine rootfs，用 ALPINE_ROOTFS 指定）
make bringup
```

然后打包 boot 镜像：

```sh
INITRAMFS_MODE=rootfs \
KERNEL=<build>/arch/arm64/boot/Image \
DTB=<build>/arch/arm64/boot/dts/mediatek/mt6893-xiaomi-chopin.dtb \
STOCK_DTB=<vendor dtb> \
./tools/build-images.sh out-image
```

`STOCK_DTB` 是厂商 DTB（from 原机 boot 分区），**必须**随镜像携带：
LK 会校验并在 `setup_arch()` 里被自有 DTB 替换。

## 相关仓库

| 仓库 | 内容 |
| --- | --- |
| `git/chopin-linux-7.3` | 内核与设备树 |
| `git/chopin-support` | 固件、服务、配置 |
| `git/chopin-docs` | 主题文档 |
