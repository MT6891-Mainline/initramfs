# chopin initramfs —— `rootfs` 分支

> 另一个分支：**`recovery`** —— 救援镜像（Alpine + 全套硬件诊断 + USB shell）
> 本分支：**`rootfs`** —— 设备实际启动用的极简 PID 1（557 行）

## 目录结构

```
init.c              PID 1 的源码（静态 C，单个文件）
Makefile            make rootfs → out/initramfs.cpio.gz
tools/              mkinitramfs.sh / build-images.sh / 诊断小工具
```

> `init.c` 以前放在 `initramfs/` 子目录里，2026-11 挪到了仓库顶层：
> 整个仓库只有这一个程序，多一层目录没有意义。

## 这是什么

Redmi Note 10 Pro（MT6891 / "chopin"）主线 Linux **真正启动设备**的那个
initramfs。里面**没有任何用户态**：只有一个静态 C 程序做 PID 1，
挂上发行版然后 `switch_root` 过去。

因为是 rootfs-only 镜像，ramdisk 里不带 shell、不带 busybox、不带诊断——
没东西可救，控制台就是发行版自己的。

```sh
make rootfs       # → out/initramfs.cpio.gz（约 290 KB）
```

## 启动流程

```
preloader → LK → GenieZone → Linux → 本 init（PID 1）→ switch_root → systemd
```

本 init 做的事：

1. 挂 `/proc /sys /dev`，把所有输出重定向到 `/dev/kmsg`
2. 打开 USB gadget 控制台 `/dev/ttyGS0`（没有串口，这是唯一早期控制台）
3. **早期加载厂商固件**（原因见下）
4. 确定根设备并挂载：
   - 内核命令行 `chopin.root=/dev/sdc86`
   - 或 initramfs 里烘焙的 `/etc/chopin/rootfs`
5. `boot_rootfs(dev)` → `switch_root` → systemd

## 三个非显然的设计

### 固件必须最先写进去

```c
/* The wlan driver requests its NVRAM blob a couple of seconds after boot and
 * the conninfra pre-calibration waits only ~30s for that request */
if (rootdev) auto_fwload();
```

WiFi 的 NVRAM 晚于 conninfra 预校准的 30 秒窗口 → **连接芯片永久卡死**，
表现为 WiFi 驱动加载成功但扫不到任何 AP。

### rootfs 模式绝不能碰显示

```c
/* In rootfs mode systemd/GDM owns the display: the bring-up child leaves the
 * DSI panel powered off after its modeset and the greeter then renders into a
 * blank panel. */
if (!rootdev && fork() == 0) display_bringup();
```

早期版本在 rootfs 模式也做显示 bring-up，结果 DSI 被关掉，
GDM 渲染进黑屏。

### 没有主机也要能启动

早期把根文件系统启动放在 `if (sh_fd >= 0)`（USB 控制台可用）里面，
结果**不插 USB 就永远停在 initramfs**。现在两者独立判断。

## 构建

```sh
make rootfs
INITRAMFS_MODE=rootfs KERNEL=... DTB=... \
STOCK_DTB=<...>/git/chopin-support/dtb/stock-base.dtb \
./tools/build-images.sh out-image
```

`STOCK_DTB` 是厂商 DTB（原机 boot 分区，现存在 `git/chopin-support/dtb/`），
**必须**随镜像携带：LK 会校验它，自有 DTB 在 `setup_arch()` 里替换。

## 相关仓库

| 仓库 | 内容 |
| --- | --- |
| `git/chopin-linux-7.3` | 内核与设备树 |
| `git/chopin-support` | 固件（含厂商 DTB）、服务、配置 |
| `git/chopin-docs` | 主题文档 |
