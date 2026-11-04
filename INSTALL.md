# Xiaomi Mi Pad 5 Pro（xiaomi-elish）Arch Linux ARM 安装指南

> 适用 Release：**26/11/04**（2026-11-04 构建）  
> 设备：Xiaomi Mi Pad 5 Pro（`xiaomi-elish`，SM8250；CSOT 或 BOE 面板）  
> 本指南覆盖：从 fastboot 开始 → 刷 rootfs/boot → 首次开机 → 首启配置 → 验证 → 日常更新 → 故障恢复。

---

## 1. Release 资产清单

| 资产 | 用途 | 是否必须 |
|---|---|---|
| `boot.img` | **CSOT 面板**的引导镜像（内核 + dtb + initramfs；initramfs 含面板驱动、GPU 固件、f2fs 首启扩容工具） | ✅ 必须 |
| `boot-boe-alt.img` | **BOE 面板**的引导镜像（只有 dtb 不同） | 仅 BOE 设备 |
| `linux-rootfs-f2fs.img.zst` | `linux` 分区的根文件系统镜像（f2fs，16 GiB，zstd 压缩） | ✅ 必须 |
| `ArchLinuxARM-aarch64-elish-rootfs.tar.zst` | 同一 rootfs 的归档（chroot / 自行制作镜像用） | 可选 |
| `initramfs.cpio.gz` | 已打进 `boot.img` 的 initramfs（单独留档） | 可选 |
| `modules.tar.zst` | 内核模块（已内置在 rootfs 的 `/lib/modules`，只有手工更新系统时才需要） | 可选 |
| `mango-0.18.3-aarch64.tar.zst` | MangoWM 运行时（已内置在 rootfs） | 可选 |
| `SHA256SUMS.txt` / `MANIFEST` | 校验和与产物清单 | ✅ 建议 |

> 默认凭据：用户 **magma** / 密码 **magma**，root 锁定；**首次登录后必须立刻改密码**。

---

## 2. 安全边界（必读）

本项目**只允许写两个分区**：

- `boot_b`：内核 + dtb + initramfs（`boot.img` / `boot-boe-alt.img`）
- `linux`：根文件系统（`linux-rootfs-f2fs.img`）

**绝对不要**：

- `fastboot flash` / `erase` 任何其它分区（`boot_a`、`dtbo_b`、`system_b`、`vendor_boot`、`super`、`userdata`、`persist`、`modem` …）；
- `fastboot erase` 任何分区（唯一历史例外是机主本人显式确认的一次性 `erase dtbo_b`，见 `docs/decisions.md` D-009）；
- 刷错面板：**CSOT 设备刷 `boot.img`，BOE 设备刷 `boot-boe-alt.img`**，刷错会黑屏（可重刷恢复）。

> 刷 `linux` 分区 = **清空当前系统与用户数据**，务必先备份。

---

## 3. 前置条件

1. 一台 Linux 主机（Arch 推荐），安装 `android-tools`（提供 `fastboot`）和 `zstd`：

```sh
sudo pacman -S --needed android-tools zstd
```

2. 设备**已解锁 bootloader**（这台设备已解锁；未解锁的设备无法按本指南刷入）。
3. 一根能传数据的 USB 线，连接设备与主机。
4. 进入 fastboot：**长按电源键约 15 秒关机，再按住「音量下 + 电源」**直到出现 fastboot 界面。

```sh
fastboot devices          # 必须列出设备，例如 <device-serial>  fastboot
fastboot getvar current-slot   # 期望输出 a（本项目只写 _b 槽位）
fastboot getvar partition-size:boot_b   # 期望 0x6000000（192 MiB）
```

---

## 4. 校验并准备镜像

```sh
# 下载 Release 资产到同一目录后：
sha256sum -c SHA256SUMS.txt

# 解压根分区镜像（GitHub 单文件上限 2 GiB，所以上传的是 .zst）
zstd -d linux-rootfs-f2fs.img.zst
ls -l linux-rootfs-f2fs.img     # 约 16 GiB（稀疏文件，实际占用更小）
```

也可以对照 `MANIFEST` 检查每个产物的 sha256 与大小。

---

## 5. 刷入

### 5.1 手工 fastboot（推荐，最直观）

```sh
fastboot getvar current-slot

# 1) 根文件系统 -> linux 分区（会清空该分区，耗时数分钟）
fastboot flash linux linux-rootfs-f2fs.img

# 2) 引导镜像 -> boot_b 分区（CSOT 面板；BOE 用 boot-boe-alt.img）
fastboot flash boot_b boot.img

# 3) 重启
fastboot reboot
```

### 5.2 用仓库脚本（带白名单 + 哈希校验，默认 dry-run）

在仓库根目录（把 `output/20261104/...` 换成你解压后的路径）：

```sh
OUTPUT_DATE=20261104 scripts/flash-elish.sh \
  --root  output/20261104/rootfs/linux-rootfs-f2fs.img \
  --boot  output/20261104/boot/boot.img \
  --manifest output/20261104/MANIFEST \
  --dry-run          # 先演练：只打印命令，不写设备

# 确认无误后把 --dry-run 换成 --execute 真正执行：
OUTPUT_DATE=20261104 scripts/flash-elish.sh \
  --root  output/20261104/rootfs/linux-rootfs-f2fs.img \
  --boot  output/20261104/boot/boot.img \
  --manifest output/20261104/MANIFEST --execute
```

脚本会拒绝白名单外的分区、强制校验 sha256 与分区容量，并把每一步写进 `output/<date>/FLASH-LOG`。

---

## 6. 首次开机

1. 设备自动重启进入系统；`tty1` 自动登录用户 `magma` 并启动 MangoWM 会话（约 2 分钟后进入桌面，`elish-bt-autoconnect` 会做几轮蓝牙重连，属正常）。
2. 开机时 initramfs 会检测到根分区比镜像大，**自动执行 `resize.f2fs` 把根扩到整个 195 GiB**；`dmesg` 里应看到：

```sh
sudo dmesg | grep -iE "elish-init: resize|growing f2fs"
df -h /            # 期望可用约 170+ GiB
```

3. 如果根仍是 16 GiB（例如用了旧 boot.img），重新刷入本 Release 的 `boot.img` 后再重启即可自动扩容。

---

## 7. 首启后必做

### 7.1 修改默认密码（必须）

```sh
passwd             # 当前密码 magma；改成你自己的
```

### 7.2 连接 Wi-Fi

```sh
nmcli device status
nmcli device wifi connect "<wifi-ssid>" password "<wifi-password>"
ip -br addr show wlp1s0
```

> Wi-Fi/BT 的 MAC 地址会由 `wlp-mac.service` / `elish-bt-mac.service` 按设备序列号派生，无需手工设置。

### 7.3 启用 SSH（可选，默认已安装）

```sh
sudo systemctl enable --now sshd
ssh magma@<device-ip>      # 主机 ssh 到设备；首次连接确认指纹
```

### 7.4 声音/亮度/桌面自检

```sh
wpctl status | grep -A3 Sinks       # 应有 Built-in Audio Speaker playback
cat /proc/asound/cards              # 应有 sm8250 - Xiaomi Mi Pad 5 Pro
brightnessctl -l                    # 背光设备
```

---

## 8. 刷入后验证清单

| 检查 | 命令 | 期望 |
|---|---|---|
| 内核版本 | `uname -v` | `Wed Nov 4 15:00:07 CST 2026`（本 Release） |
| cmdline | `cat /proc/cmdline` | 含 `systemd.gpt_auto=no`、`root=PARTLABEL=linux` |
| 根分区 | `findmnt /` | `f2fs`，约 195 GiB |
| GPU 固件 | `sudo dmesg \| grep -i a650` | `loaded qcom/a650_sqe.fw` / `a650_gmu.bin` |
| 显示 | `ls /dev/dri/` | `card0`、`renderD128` |
| 桌面 | `pgrep -a -x mango` | `/usr/bin/mango` 在跑 |
| 状态栏 | `pgrep -a -x waybar` | `waybar` 在跑 |
| 蓝牙 | `sudo btmgmt info` | `Index list with 1 item`，MAC 非全 0 |
| 音频 | `cat /proc/asound/cards` | `sm8250` 声卡 |
| 触摸 | `cat /proc/bus/input/devices \| grep -i NVT` | `NVTCapacitiveTouchScreen` |

---

## 9. 日常更新（不重刷 rootfs）

### 9.1 只更新内核/引导（Image、dtb、initramfs 变化时）

```sh
# 主机侧
fastboot flash boot_b boot.img
fastboot reboot
```

### 9.2 只更新内核模块（纯模块补丁）

```sh
# 把 modules.tar.zst 传到设备后（设备侧执行）：
sudo tar --zstd -xf modules.tar.zst -C /
sudo depmod -a
sudo systemctl restart bluetooth    # 涉及蓝牙模块时
```

> 注意：`sudo` 只对它后面那一条命令生效，`sudo tar ... && depmod -a` 里的 `depmod` 会以普通用户运行并报 Permission denied。

### 9.3 系统包更新

```sh
sudo pacman -Syu
```

（`/etc/pacman.conf` 已预置 `DisableSandboxFilesystem` / `DisableSandboxSyscalls`，因为本内核没有 Landlock。）

---

## 10. 故障恢复

| 现象 | 处理 |
|---|---|
| 刷完黑屏、但设备能被 `fastboot devices` 看到 | 检查面板型号：CSOT 用 `boot.img`，BOE 用 `boot-boe-alt.img`；重刷对应 boot 镜像 |
| 卡在开机画面 / 无法进入系统 | 进 fastboot 重刷 `boot_b`（rootfs 数据不动）；仍不行再重刷 `linux` |
| 桌面起不来但能进 tty | `sudo systemctl restart getty@tty1`；查看 `journalctl -b -1` |
| 蓝牙适配器消失 | `sudo systemctl restart elish-bt-mac`；确认 `/etc/modules-load.d/elish-bluetooth-hid.conf` 生效 |
| 根分区没扩容 | 确认刷的是本 Release 的 boot.img，重启后看 `sudo dmesg \| grep elish-init` |
| 完全刷坏 | 只能回到 fastboot 重刷 `boot_b` + `linux`；**不要动其它分区** |

> 任何时候都不要 `fastboot erase` 其它分区；恢复只需这两个分区。

---

## 11. 已知限制

- **快充约 10 W**：原厂 33 W 用小米私有 11 V 协议，标准 PD 9 V + 2:1 电荷泵只能到约 10 W（见 `bug.md` B-038~B-040）。
- **无 hexagonrpcd**：ALARM 没有该包，音频录音/自动旋转等受限（B-020）。
- **没有厂商 per-amp 调音（acdb）**：8 颗 CS35L41 用通用固件，音色与厂商不同。
- **FEX-Emu**：已装、RootFS 为 Ubuntu 24.04（glibc），thunks 仍在打磨，x86 程序兼容性有限。
- **未内置**：Firefox、Waydroid、Flatpak 应用等（需要时自行 `pacman`/`flatpak` 安装）。
- `systemd-networkd-wait-online` 会失败导致 `systemctl is-system-running` 显示 degraded（既有问题，不影响使用）。

---

## 12. 从源码重建（维护者参考）

```sh
# 一次性：拉取 GPU 固件（initramfs 早期加载必需，脚本会校验 sha256）
scripts/fetch-gpu-firmware.sh --out ../.build/firmware/elish-gpu

# 内核 + initramfs + boot.img + MANIFEST（不含 rootfs）
scripts/build-release.sh --skip-rootfs --no-publish   # 若脚本无该参数则直接：scripts/build-release.sh --no-publish

# rootfs（需要先准备 --firmware / --mango / --overlay）：
BUILD_ROOT=../.build OUT_DIR=output/<date>/rootfs scripts/build-rootfs.sh \
  --modules output/<date>/kernel/modules.tar.zst \
  --firmware ../.build/firmware/elish-rootfs \
  --mango   ../.build/mango/stage \
  --overlay ../.build/device-overlay \
  --size 16G --jobs 8
```

重新构建后重新运行 `scripts/build-release.sh` 即可把 rootfs/mango/INSTALL.md 一起打进 Release 资产（脚本会自动发现 `output/<date>/rootfs` 与 `output/<date>/mango`）。

---

## 13. 安全与隐私

- 仓库与 Release **不含**任何真实 Wi-Fi 密码、IP、SSH 私钥或代理配置；
- 默认口令仅用于首次登录，请立即修改；
- SSH 默认不开放密码登录（按需自行配置 `authorized_keys`）；
- 所有刷写操作只涉及 `boot_b` 与 `linux` 两个分区。

---

*最后更新：2026-11-04（对应 Release 26/11/04）。遇到本指南未覆盖的问题，请附 `sudo dmesg`、`journalctl -b` 与复现步骤记录到 `bug.md`。*
