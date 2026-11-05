# Xiaomi Mi Pad 5 Pro（xiaomi-elish）Arch Linux ARM 安装教程

从一台跑着 Android 的小米平板 5 Pro，到一台能日常使用的 Arch Linux ARM 平板。

全程只写两个分区：`boot_b`（内核 + dtb + initramfs）和 `linux`（根文件系统）。
每一步都给出**要执行的命令、期望看到的输出、失败了怎么办**。不需要事先懂分区表，照着做即可。

| 项目 | 值 |
|---|---|
| 设备 | Xiaomi Mi Pad 5 Pro，代号 `xiaomi-elish`，SoC SM8250 |
| 系统 | Arch Linux ARM（aarch64） |
| 桌面 | MangoWM + waybar |
| 内核 | postmarketOS `linux-postmarketos-qcom-sm8250`（本仓库补丁） |
| 引导链 | ABL → `boot_b` → `root=PARTLABEL=linux` |
| 教程对应 | Release `26/11/05`（资产名与步骤不随版本变化） |
| 面板 | CSOT 或 BOE，**两者镜像不同，刷错黑屏** |

---

## 阅读约定

- 代码块默认在**主机**（x86_64 Linux）的 bash 里执行；标注「设备上」的在平板自己的终端里执行；标注「recovery 里」的在临时启动的 recovery 里执行。
- 工具一律写绝对路径。不在该路径时用 `command -v <工具名>` 查实际位置（`echo`/`cd` 这类 shell 内建除外）。
- 路径一律写绝对路径，不使用 `../` 形式的相对路径。推荐工作目录 `/srv/elish`，后文全部按这个路径写；换成别的绝对路径也可以，但要全文替换。
- 占位符：`<device-serial>`、`<device-ip>`、`<wifi-ssid>`、`<wifi-password>`、`<release-tag>`、`<owner>/<repo>`。尖括号内容需要替换成实际值，不要连尖括号一起粘贴。
- **recovery 环境例外**：recovery 是 Android 内核 + toybox，工具在 `/system/bin/`、`/sbin/`，没有 `/usr/bin`。在 recovery 里执行的命令（第 3 章）用裸命令名，`parted` 用你 push 进去的 `/cache/parted`。

### 整体流程

```text
[主机]  下载 Release 资产 → 校验 SHA256 → 解压 rootfs 镜像            （第 1 章）
   ↓
[设备]  解锁 bootloader（一次性）→ 进 fastboot → 主机侧只读核对        （第 2 章）
   ↓
[设备]  临时启动 recovery → adb root → 卸载 /data
        → parted：缩小 userdata + 新建 linux 分区                     （第 3 章）
   ↓
[设备]  回 fastboot → 刷 boot_b（按面板二选一）                       （第 4 章）
        → 刷 linux（rootfs，16 GiB 镜像，耗时数分钟）                 （第 5 章）
        → 切到 b 槽 → 重启                                           （第 5 章）
   ↓
[设备]  首启：initramfs 自动 resize.f2fs 扩容 → 自动登录 → 改口令
        → 连 Wi-Fi → 逐项验证                                        （第 6、7 章）
   ↓
[日常]  只换内核 / 只换模块 / pacman -Syu                             （第 8 章）
[故障]  黑屏、卡开机、桌面起不来、蓝牙消失、扩容失败…                  （第 9 章）
```

### 时间与容量预估

| 阶段 | 耗时 | 备注 |
|---|---|---|
| 下载资产 | 5–30 分钟 | 取决于带宽；rootfs 压缩包约 1.3 GiB 量级 |
| SHA256 校验 | 1–3 分钟 | 压缩包读完很快；解压后的 16 GiB 镜像还要再读一遍 |
| 解压 rootfs | 1–3 分钟 | 解压后**实占 16 GiB**，不是稀疏文件 |
| 解锁 bootloader | 一次性，可能需等待数天 | 小米账号绑定后有等待期 |
| 分区（第 3 章） | 10–20 分钟 | 主要在核对数字，命令本身几秒 |
| 刷 `linux` | 3–15 分钟 | USB 2.0 慢、USB 3.0 快；16 GiB |
| 首次开机 | 3–5 分钟 | 含扩容 + 蓝牙重连等待 |

主机磁盘至少留 **25 GiB** 空闲（压缩包 + 解压后的 16 GiB 镜像）。

### 目录

- [0. 前置条件与风险声明](#0-前置条件与风险声明)
- [1. 准备：下载、校验、解压](#1-准备下载校验解压)
- [2. 解锁 bootloader 与进入 fastboot](#2-解锁-bootloader-与进入-fastboot)
- [3. 用 parted 创建 `linux` 分区（重点）](#3-用-parted-创建-linux-分区重点)
- [4. 刷写 `boot_b`（区分 CSOT / BOE）](#4-刷写-boot_b区分-csot--boe)
- [5. 刷写 rootfs 到 `linux` 分区](#5-刷写-rootfs-到-linux-分区)
- [6. 首次开机与自动扩容](#6-首次开机与自动扩容)
- [7. 首启必做：改口令、连 Wi-Fi、验证](#7-首启必做改口令连-wi-fi验证)
- [8. 日常更新](#8-日常更新)
- [9. 故障恢复](#9-故障恢复)
- [10. 已知限制](#10-已知限制)
- [附录 A：命令速查](#附录-a命令速查)
- [附录 B：与上游 Armbian 流程的差异](#附录-b与上游-armbian-流程的差异)
- [附录 C：出问题时怎么反馈](#附录-c出问题时怎么反馈)

---

## 0. 前置条件与风险声明

### 0.1 只会写两个分区

| 分区（GPT 名） | 内容 | 大小 | 写入命令 | 允许 |
|---|---|---|---|---|
| `boot_b` | 内核 `Image.gz` + dtb + initramfs | 固定 **192 MiB**（393216 扇区 = `0xC000000` 字节） | `fastboot flash boot_b` | ✅ |
| `linux` | 根文件系统（f2fs） | **由你决定**（≥ 16 GiB，建议 ≥ 32 GiB） | `fastboot flash linux` | ✅ |
| `boot_a` `dtbo_a` `dtbo_b` `system_b` `vendor_boot` `super` `userdata` `persist` `modem` `metadata` `esp` … | — | — | — | ❌ 一律不碰 |

为什么只要两个分区：

- 内核和 initramfs 塞进 Android 的 B 槽 boot 分区，ABL（小米的 bootloader）直接从 `boot_b` 引导 Linux，不需要 U-Boot，也不需要改 `super`；
- 根文件系统单独占一块物理分区，不走 `super` 动态分区，少一层失败面；
- 出问题只要重刷这两个分区，Android 的 a 槽、`super`、`modem`、`persist` 全都没动。

**本教程不包含任何 `fastboot erase`。** 上游 Armbian 与 postmarketOS 的 elish 教程里有一步 `fastboot erase dtbo_b`，本教程不用它，原因见 [4.5](#45-关于-dtbo_b为什么本教程不擦)。

### 0.2 风险声明

| 操作 | 后果 | 可逆性 |
|---|---|---|
| 缩小并重建 `userdata`（第 3 章） | Android 侧全部用户数据（照片、应用、账号、聊天记录）丢失 | **不可逆**，先备份 |
| 刷 `linux`（第 5 章） | `linux` 分区原有内容全部丢失 | **不可逆** |
| 解锁 bootloader | 清空全部数据，失去官方保修 | **不可逆**（可重新上锁，但数据已空） |
| 刷错面板的 `boot.img` | 开机黑屏（系统其实起来了，只是屏幕没点亮） | 可逆，重刷另一个镜像 |
| 分区表写坏 | 可能连 fastboot 都进不去 | **EDL（9008）救援本项目未验证，视为不可逆** |
| 刷小米官方 fastboot 包「恢复 Android」 | 会写全部分区，`linux` 分区被覆盖 | Linux 侧数据全丢 |

一句话：**开始之前，先假设这台设备上所有数据都会消失。** 需要留的先备份到电脑或云端。

### 0.3 两种面板：先确认，再下载

| 面板 | 该刷的产物 | 内核 cmdline 里的标识 |
|---|---|---|
| **CSOT** | `boot.img` | `msm_drm.dsi_display0=qcom,mdss_dsi_k81_42_02_0a_dual_cphy_video` |
| **BOE** | `boot-boe-alt.img` | `msm_drm.dsi_display0=qcom,mdss_dsi_k81_35_02_0b_dual_cphy_video` |

三种查法（按可靠性排序）：

1. Android 里（root shell）：`/usr/bin/adb shell "cat /proc/cmdline"`；
2. recovery 里（第 3 章会进）：`/usr/bin/adb shell "cat /proc/cmdline"`；
3. 已经装好 Linux 的设备：`/usr/bin/sudo /usr/bin/dmesg | /usr/bin/grep -iE 'panel|nt36523'`。

三条都拿不到时，先刷 `boot.img`（CSOT）。**黑屏就重刷 `boot-boe-alt.img`**——刷错面板不会损坏硬件，只是屏幕不亮，设备仍能被 `fastboot devices` 看到。这是一个安全的二分法。

> **BOE 产物的状态（如实说明）**：本仓库测试设备是 CSOT。`boot-boe-alt.img` 由同一条构建流水线产出（只有 dtb 不同），但**尚未在 BOE 真机上验证**。BOE 用户刷完后请按第 7 章逐项核对并回报结果。

### 0.4 你需要准备什么

硬件：

| 物品 | 要求 | 说明 |
|---|---|---|
| 平板 | 电量 ≥ 50% | 刷写中途断电可能写坏分区 |
| USB-C 数据线 | **能传数据** | 很多附赠线只能充电，表现为 `fastboot devices` 无输出 |
| 主机 | x86_64 Linux（Arch 推荐） | 也可以 Ubuntu/Fedora，包名自行替换 |
| 主机磁盘 | ≥ 25 GiB 空闲 | 压缩包 + 解压后的 16 GiB 镜像 |
| USB 键盘 | 可选 | 桌面起不来时用 tty 排障更顺手 |
| Windows 机器/虚拟机 | 仅解锁阶段需要 | 小米的解锁工具只有 Windows 版 |

软件（Arch）：

```sh
/usr/bin/sudo /usr/bin/pacman -S --needed android-tools android-udev zstd python3 7zip curl
command -v fastboot adb zstd sha256sum python3
```

逐行说明：

- 第一条装 `fastboot`/`adb`（`android-tools`）、USB 设备权限规则（`android-udev`）、解压工具（`zstd`）、校验与计算（`python3`）、解压 7z 格式的静态 `parted`（`7zip`）、下载（`curl`）。
- 第二条是 shell 内建，用来确认工具都在 PATH 里；期望打印 5 个绝对路径，例如 `/usr/bin/fastboot`、`/usr/bin/adb`。**若某个为空**，说明对应包没装上，回到第一条。
- 装完 `android-udev` 后重新插拔 USB 线，规则才会生效。

`fastboot` 的默认位置是 `/usr/bin/fastboot`；若你的系统上不在这个路径，用 `command -v fastboot` 查出来，后文照抄该绝对路径。

另外两样东西不在 Release 里，需要自己准备：

| 物品 | 来源 | 用在哪 |
|---|---|---|
| OFRP recovery 镜像（`recovery.img`） | `https://github.com/ymdzq/OFRP-device_xiaomi_elish/releases` | 第 3 章分区时临时启动 |
| 静态 aarch64 `parted`（`parted.7z`） | 上游 Armbian 指南给的链接（renegade-project.tech） | 第 3 章分区时 push 进设备 |

### 0.5 开始前必须确认的三件事

| # | 确认项 | 怎么确认 | 不满足会怎样 |
|---|---|---|---|
| 1 | 数据已备份 | 自己确认 | 第 3 章会清空 Android 数据，无法恢复 |
| 2 | 知道默认登录口令 | Release 说明的「默认登录凭据」一节给出 `magma` 的口令 | 需要口令的操作（如 `su`、SSH）做不了 |
| 3 | 知道自己的面板型号 | 见 [0.3](#03-两种面板先确认再下载) | 黑屏（可重刷恢复），多花 10 分钟 |

第 2 条单独说明：默认用户是 `magma`。**本 Release 的默认口令写在 Release 说明的「默认登录凭据」一节**（本项目约定为 `elish`）。

首次开机时 tty1 会以 `magma` 自动登录并直接进桌面，**不需要输入口令**；但 `su`、SSH 登录等操作需要它。拿到口令后第一件事就是改掉（见 §7.1）。

若你拿到的是自己构建的镜像，构建时若设置了环境变量 `ELISH_DEFAULT_PASSWORD` 就用该值，否则口令是**随机生成、只在构建日志打印一次**的——以构建日志为准。

---

## 1. 准备：下载、校验、解压

### 1.1 建工作目录

```sh
/usr/bin/sudo /usr/bin/mkdir -p /srv/elish
/usr/bin/sudo /usr/bin/chown "$(id -un):$(id -gn)" /srv/elish
/usr/bin/df -h /srv/elish
```

- 第 1、2 行：建目录并把属主改成当前用户（后面所有下载、解压都不再需要 `sudo`）。
- 第 3 行：确认可用空间。期望 `Avail` ≥ `25G`。
- 失败：`df` 显示可用空间不足 → 换一个挂载点（例如 `/home/<user>/elish`），**后续所有路径同步替换**。

### 1.2 资产清单

| 资产 | 是什么 | 必须 |
|---|---|---|
| `boot.img` | **CSOT 面板**的引导镜像（内核 + dtb + initramfs） | ✅ CSOT 必刷 |
| `boot-boe-alt.img` | **BOE 面板**的引导镜像（只有 dtb 不同） | ✅ BOE 必刷 |
| `linux-rootfs-f2fs.img.zst` | rootfs 镜像（f2fs，zstd 压缩，解压后 16 GiB） | ✅ |
| `ArchLinuxARM-aarch64-elish-rootfs.tar.zst` | 同一 rootfs 的归档（chroot / 自制镜像用） | 可选 |
| `initramfs.cpio.gz` | initramfs 单独留档（**已内置在 `boot.img` 里**） | 可选 |
| `modules.tar.zst` | 内核模块（已内置在 rootfs 的 `/lib/modules`，只在单独换模块时需要） | 可选 |
| `mango-0.18.3-aarch64.tar.zst` | MangoWM 运行时（**已内置在 rootfs 里**，只在你自行重建 rootfs 时需要） | 可选 |
| `MANIFEST` | 产物清单：文件名、目标分区、SHA256、大小 | ✅ 建议 |
| `SHA256SUMS.txt` | `sha256sum -c` 用的校验和 | ✅ 建议 |
| `RELEASE-NOTES.md` | 本次发布说明（含**默认登录凭据**、快速安装命令、变更点）。它同时是 Release 页面正文，官网页面直接可见 | ✅ 建议 |

`linux-rootfs-f2fs.img.zst` 之所以是压缩包：GitHub Release 单个资产上限 2 GiB，16 GiB 的镜像必须先压缩。

### 1.3 下载

**方式 A：GitHub CLI（推荐，一条命令拿全部）**

```sh
/usr/bin/gh release download <release-tag> --repo <owner>/<repo> --dir /srv/elish
/usr/bin/ls -l /srv/elish
```

`gh` 不在系统里时：`/usr/bin/sudo /usr/bin/pacman -S --needed github-cli`。
期望：`/srv/elish` 里出现上表全部文件，大小非 0。

**方式 B：curl 循环**

```sh
for f in boot.img boot-boe-alt.img linux-rootfs-f2fs.img.zst \
         ArchLinuxARM-aarch64-elish-rootfs.tar.zst initramfs.cpio.gz \
         modules.tar.zst MANIFEST SHA256SUMS.txt; do
  /usr/bin/curl -fL --output-dir /srv/elish -O \
    "https://github.com/<owner>/<repo>/releases/download/<release-tag>/$f"
done
```

- `-f` 让 HTTP 404/500 直接失败（否则会把错误页存成文件）；
- `-L` 跟随跳转；
- 期望每条都无输出（curl 静默成功）。有 `curl: (22) ... 404` 说明资产名或标签写错。

**下载失败的通用处理**：先确认 `<release-tag>` 与资产名是否与 Release 页面完全一致（大小写敏感）；再检查是否需要代理；最后换方式 B 逐个下。

### 1.4 校验 SHA256

```sh
cd /srv/elish
/usr/bin/sha256sum -c /srv/elish/SHA256SUMS.txt
```

（`sha256sum -c` 会按**当前目录**解析清单里的文件名，所以要先 `cd` 到 `/srv/elish`；`cd` 的目标是绝对路径。）

期望每个文件一行 `<文件名>: OK`，退出码 0：

```text
boot.img: OK
boot-boe-alt.img: OK
linux-rootfs-f2fs.img.zst: OK
```

中文 locale 下 `OK` 会显示成 `成功`，含义相同。

**如果清单里的路径带目录前缀**（例如 `rootfs/linux-rootfs-f2fs.img`），而下载后文件是平铺的，`sha256sum -c` 会报 `No such file or directory`。把前缀去掉再校验：

```sh
/usr/bin/sed 's#  rootfs/#  #' /srv/elish/SHA256SUMS.txt > /srv/elish/SHA256SUMS.flat
/usr/bin/sha256sum -c /srv/elish/SHA256SUMS.flat
```

失败处理：

| 输出 | 含义 | 处理 |
|---|---|---|
| `WARNING: 1 computed checksum did NOT match` | 文件损坏或下载不全 | 删掉该文件重下；反复失败换网络/换方式 |
| `No such file or directory` | 文件名或路径前缀不对 | 用上面的 `sed` 方案，或 `ls -l` 核对文件名 |
| 校验和本身与 `MANIFEST` 里的不同 | 下到了别的版本 | 确认 `<release-tag>` 与 `MANIFEST` 头部一致 |

### 1.5 解压 rootfs

```sh
/usr/bin/zstd -d /srv/elish/linux-rootfs-f2fs.img.zst -o /srv/elish/linux-rootfs-f2fs.img
```

- `-d` 解压；`-o` 指定输出文件（不加 `-o` 会写到当前目录，容易找不到）。
- 期望结束行：`linux-rootfs-f2fs.img.zst: 17179869184 bytes`。
- 耗时 1–3 分钟。**解压结果是 16 GiB 的实际占用**，不是稀疏文件——先确认磁盘够。

立刻核对解压结果（**这步不能省**：磁盘写满时可能得到一个短文件而不报错）：

```sh
/usr/bin/stat -c '%n %s bytes' /srv/elish/linux-rootfs-f2fs.img
/usr/bin/sha256sum /srv/elish/linux-rootfs-f2fs.img
```

| 检查 | 期望 |
|---|---|
| 大小 | `17179869184` 字节（16 GiB） |
| SHA256 | 与 `MANIFEST` 中 `linux-rootfs-f2fs.img` 那一行的值一致 |

不符时：删掉半成品重新解压；`No space left on device` 说明磁盘不够，清空间后重来。

### 1.6 记录本次安装用到的值

后续排障会反复用到，先填好：

| 项 | 值 |
|---|---|
| Release 标签 | |
| 面板型号（CSOT / BOE） | |
| `boot.img` 文件名 | |
| `boot.img` 字节数 | |
| `linux-rootfs-f2fs.img` SHA256（前 8 位） | |
| 默认口令是否有 | 是 / 否 |

---

## 2. 解锁 bootloader 与进入 fastboot

### 2.1 解锁 bootloader（一次性，会清空数据）

小米的解锁流程由官方工具控制，第三方无法绕过。步骤：

1. 设备：设置 → 设备信息（部分版本叫「关于平板」）→ 全部参数 → 连续点击「MIUI 版本」7 次，打开开发者选项；
2. 设置 → 更多设置 → 开发者选项 → 打开「USB 调试」和「OEM 解锁」；
3. 同一页面点「设备解锁状态」→ 按提示绑定小米账号与设备（需要联网，按页面要求操作）；
4. 主机（Windows 或虚拟机）安装小米官方的 Mi Unlock 工具，登录**同一个**小米账号，USB 连接设备，按工具提示解锁；
5. 绑定后通常有一段等待期（历史上为 168 小时），工具会显示剩余时间。等待期与具体规则**以小米账号页面的实际显示为准**，本项目无法更改。

注意事项：

- 解锁会清空设备全部数据（含 `userdata`）；
- Mi Unlock 只有 Windows 版，Linux 用户需要一台 Windows 机器或带 USB 直通的虚拟机；
- 解锁后设备开机第一屏会出现解锁提示（小锁图标/`unlocked` 字样），这是正常的。

验证：

```sh
/usr/bin/fastboot getvar unlocked
```

期望 `unlocked: yes`。若为 `no`，后面所有 `flash` 都会报 `FAILED (remote: 'not allowed in locked state')`，必须回到本节。
若报变量不存在（不同 bootloader 的变量名有差异），改用 `/usr/bin/fastboot getvar all`，在输出里找 `unlocked:` 那一行。

### 2.2 进入 fastboot 的三种方式

| 方式 | 操作 | 适用场景 |
|---|---|---|
| **按键**（最可靠） | 先彻底关机（长按电源约 15 秒），再**按住「音量下 + 电源」**约 3 秒，看到 FASTBOOT 界面/小米吉祥物后松手 | 任何时候 |
| Android 内 | `adb reboot bootloader` | 系统还能正常开机 |
| recovery 内 | OFRP 菜单 Reboot → Bootloader，或 `adb reboot bootloader` | 第 3 章之后 |

界面特征：黑底，屏幕上有 `FASTBOOT` 字样或小米兔子图案。

> 如果屏幕全黑但主机能识别到设备，也可能已经进了 fastboot（面板没点亮）。**以 `fastboot devices` 的输出为准，不要以屏幕为准。**

### 2.3 主机侧只读核对

```sh
/usr/bin/fastboot devices
/usr/bin/fastboot getvar unlocked
/usr/bin/fastboot getvar current-slot
/usr/bin/fastboot getvar partition-size:boot_b
```

| 命令 | 期望输出 | 含义 | 不符时 |
|---|---|---|---|
| `devices` | `<device-serial>	fastboot` | 设备已被识别 | 见 [2.5](#25-fastboot-失败对照表)；先查线和 USB 口 |
| `getvar unlocked` | `unlocked: yes` | bootloader 已解锁 | 回 [2.1](#21-解锁-bootloader一次性会清空数据) |
| `getvar current-slot` | `current-slot: a` | Android 在 a 槽，b 槽留给 Linux | 显示 `b` 也能继续，第 5 章会再设一次 |
| `getvar partition-size:boot_b` | `partition-size:boot_b: 0xC000000` | `boot_b` 存在且为 192 MiB | 若报变量不存在，说明 bootloader/机型不符，停止操作 |

再看一眼 `linux` 分区在不在：

```sh
/usr/bin/fastboot getvar partition-size:linux
```

两种可能，都有意义：

- **报错或值为空**（`FAILED (remote: 'GetVar Variable Not found')`、`unknown variable`）→ 正常，GPT 里还没有 `linux`，第 3 章会建出来；
- **有正常数值** → 设备上已经存在 `linux` 分区（之前装过 Linux）。**跳过第 3 章**，直接从第 4 章开始；只有想调整大小时才需要回第 3 章先删掉旧的。

### 2.4 USB 权限（Arch）

```sh
/usr/bin/sudo /usr/bin/pacman -S --needed android-udev
/usr/bin/sudo /usr/bin/udevadm control --reload-rules
/usr/bin/sudo /usr/bin/udevadm trigger
```

装完重新插拔 USB 线。普通用户即可访问设备（规则用 `uaccess` 给当前登录会话授权）。

已知例外：**通过 SSH 远程登录主机时**没有本地登录会话，`uaccess` 不生效，会报 `no permissions`。此时用 `sudo fastboot ...` 执行，或直接在本地终端操作。

### 2.5 fastboot 失败对照表

| 现象 | 原因 | 处理 |
|---|---|---|
| `< waiting for any device >` 长时间不动 | 设备没进 fastboot；线只能充电；USB 口供电不足 | 换线、换到主板后置 USB 2.0 口、重新按键进 fastboot |
| `no permissions (user in plugdev group; are your udev rules wrong?)` | udev 规则没生效，或当前是 SSH 会话 | 见 [2.4](#24-usb-权限arch)；临时用 `sudo fastboot` |
| `FAILED (remote: 'not allowed in locked state')` | bootloader 没解锁 | 回 [2.1](#21-解锁-bootloader一次性会清空数据) |
| `fastboot: error: cannot load 'xxx.img'` | 路径错或文件不存在 | 用绝对路径，`ls -l` 确认文件存在且非 0 字节 |
| `FAILED (remote: 'unknown command')` | 该机型 ABL 不支持这条命令 | 用等价写法（例如 `--set-active=b` ↔ `set_active b`） |
| 设备总是启动进 Android | 按键时机不对 | 先长按电源 15 秒彻底关机，再按住音量下、再按电源 |

---

## 3. 用 parted 创建 `linux` 分区（重点）

本章是**新设备安装的必经步骤**，不是可选的兜底流程。做完之后 Android 侧数据即失效。

> 想先弄清原理（GPT 结构、扇区换算、算错会怎样、写坏了怎么救），或者要回滚到 Android，见
> [docs/partitioning.md](docs/partitioning.md)——那是本章的展开版。

### 3.1 为什么要缩 `userdata`

`linux` **不是出厂分区**。出厂 GPT 里没有它，所以全新设备上直接执行 `fastboot flash linux` 一定失败。平板的 UFS 上，出厂时最后一个分区是 `userdata`，它一直延伸到磁盘末尾（只给备份 GPT 留 33 个扇区）。

想要一块给 Linux 用的独立空间，只有两个来源：

1. **从 `userdata` 尾部切一块**（本章做法，也是上游 Armbian 的做法）；
2. 占用 `super` 里的动态分区（postmarketOS 的一种做法）——会牵动 `system_b`/`vendor_b`，空间还受 `super` 总大小限制，本项目不采用。

磁盘布局前后对比（扇区号为示例，你的数字不同）：

```text
改动前：
[ p1 … p33 ][ userdata 34 ..................................... 磁盘末尾 ][备份 GPT]
             4096000s                                      500118158s

改动后：
[ p1 … p33 ][ userdata 34 ....... ][ linux 35 ................. 磁盘末尾 ][备份 GPT]
             4096000s      71204863s 71204864s             500118158s
```

关键点：

- 两个分区**不能共用扇区**，所以 `linux` 的起始 = 新 `userdata` 的结束 + 1；
- `linux` 的结束沿用原来的 `userdata` 结束扇区，这样不碰备份 GPT；
- 分区名（GPT `Name`）必须是 `linux`，因为内核 cmdline 用 `root=PARTLABEL=linux` 找根文件系统。

> **先做一次预检**：如果你的 `parted print` 里**已经**有名字为 `linux` 的分区，说明之前装过。不要再建一个，直接跳到第 4 章。本章只适用于「全新、只有 Android」的设备。

### 3.2 为什么必须在 recovery 里做

分区表只能由**运行在这台设备上、对 `/dev/block/sda` 有写权限的进程**修改。fastboot 没有编辑分区表的命令（`fastboot flash` 只能往已存在的分区里写），主机也看不到设备的 UFS。所以：

```text
fastboot（主机） --fastboot boot--> recovery（跑在设备内存里） --adb--> 设备上的 root shell --> parted
```

三个前提，缺一不可：

1. recovery 的 adb 是 **root**（OFRP 默认如此，`adb shell id` 应返回 `uid=0`）；
2. 设备上有一份能跑的 `parted`（Android 自带没有，需要 push 进去）；
3. 要改的分区**没有被挂载**（recovery 默认会挂 `/data`，必须先卸载）。

### 3.3 准备 recovery 与 parted

#### 3.3.1 取得 recovery 镜像

用 OrangeFox Recovery 的 elish 版本：`https://github.com/ymdzq/OFRP-device_xiaomi_elish/releases`，下载 `.img` 文件，放到 `/srv/elish/recovery.img`（文件名以实际为准，下文统一按这个路径写）。

#### 3.3.2 临时启动 recovery（不写任何分区）

```sh
/usr/bin/fastboot boot /srv/elish/recovery.img
```

- `fastboot boot` 把镜像**加载到内存**运行，**不写 flash**，所以不违反「只写两个分区」。
- 期望：约 10–30 秒后屏幕出现 OrangeFox 界面。
- 失败：`FAILED (remote: 'unknown command')` 或 `FAILED (remote: 'Boot image not supported')` → 用 3.3.3 的备用方案。

#### 3.3.3 备用：把 recovery 临时刷到 `boot_b`

```sh
/usr/bin/fastboot flash boot_b /srv/elish/recovery.img
/usr/bin/fastboot reboot
```

- 这只临时占用 `boot_b`（此时 Linux 内核还没刷，本来就是空的），**仍在白名单内**。
- 分区做完后必须刷回 `boot.img`（第 4 章），否则设备会一直进 recovery。
- 如果中途断电，设备停在 recovery：重进 fastboot 重刷即可，不会变砖。

#### 3.3.4 确认 adb 是 root

```sh
/usr/bin/adb devices
/usr/bin/adb shell id
```

- 期望第一行列出设备（`<device-serial>	recovery`），第二行 `uid=0(root) gid=0(root)`。
- 若是 `uid=2000(shell)`：OFRP 设置里打开 root adb，或换一个 recovery。**不要用非 root shell 继续**，`parted` 打不开块设备。

#### 3.3.5 卸载 `/data` 与 `/sdcard`

分区表被挂载中的文件系统占用时，`parted` 会拒绝写入，或内核拒绝重读分区表。所以先看再卸：

```sh
/usr/bin/adb shell "mount | grep -E ' /data | /sdcard '"
```

- 有输出 → 在 OFRP 界面 Mount → 取消勾选 Data；或直接卸载：

```sh
/usr/bin/adb shell "umount /sdcard; umount /data"
```

- 再跑一次第一条命令，期望**没有任何输出**。

#### 3.3.6 push 静态 `parted`

先确认 `/cache` 存在（部分 A/B 机型没有 cache 分区）：

```sh
/usr/bin/adb shell "ls -ld /cache /tmp"
```

两个都有就用 `/cache`（下文按这个写）；`/cache` 不存在就改用 `/tmp`，并把后续所有 `/cache/parted` 换成 `/tmp/parted`。

```sh
/usr/bin/adb push /srv/elish/parted /cache/parted
/usr/bin/adb shell "chmod 755 /cache/parted"
/usr/bin/adb shell "/cache/parted --version"
```

- 期望最后一行打印 `parted (GNU parted) 3.x`。
- `parted` 必须是 **aarch64 静态二进制**。Android 上没有 glibc，动态链接的会报 `no such file or directory` 或 `CANNOT LINK EXECUTABLE`。
- 主机上虽然也有 `/usr/bin/parted`，但**主机版 parted 看不到设备的 UFS**，只能用来演练（见附录 A）。

`parted` 从哪来：上游 Armbian 指南提供的 `parted.7z`（renegade-project.tech），解压即得。解压需要 `7z`，三条命令依次是解压、加执行权限、确认架构：

```sh
/usr/bin/7z x /srv/elish/parted.7z -o/srv/elish
/usr/bin/chmod 755 /srv/elish/parted
/usr/bin/file /srv/elish/parted
```

最后一条命令期望输出 `ELF 64-bit LSB executable, ARM aarch64, statically linked`。

#### 3.3.7 确认目标磁盘

```sh
/usr/bin/adb shell "readlink -f /dev/block/by-name/userdata"
```

期望形如 `/dev/block/sda36` → 磁盘是 `/dev/block/sda`（去掉末尾数字）。

若 `by-name` 不存在：

```sh
/usr/bin/adb shell "ls -l /dev/block/by-name | head"
```

Android 的 ueventd 会建这个目录，OFRP 里一定有。

> **后文统一写 `/dev/block/sda`。** 如果你的实际磁盘节点不同（例如 `/dev/block/sdb`），全文替换。

### 3.4 只读侦察：记录 `userdata` 的起止扇区

这一步**只读**，不改任何东西。先把现场拍下来。

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"
```

输出示例（256 GB 机型；**你的数字一定不同**）：

```text
Model: SCSI DISK (scsi)
Disk /dev/block/sda: 500118192s
Sector size (logical/physical): 512B/4096B
Partition Table: gpt
Disk Flags:

Number  Start       End         Size        File system  Name      Flags
 1      8192s       131071s     122880s                  ...
...
34      4096000s   500118158s  496022159s               userdata
```

逐字段解读：

| 字段 | 含义 | 本教程怎么用 |
|---|---|---|
| `Disk /dev/block/sda: 500118192s` | 磁盘总扇区数（`T`） | 核对「末尾」，也用来判断是否算错 |
| `Sector size (logical/physical): 512B/4096B` | 逻辑/物理扇区大小 | **决定 1 个 `s` 是多少字节** |
| `Number` | 分区号 | `rm`、`name` 要用 |
| `Start` / `End` | 起止扇区，**含端点** | 计算新分区边界 |
| `Name` | GPT 分区名 | `by-partlabel` 与 `PARTLABEL=linux` 靠它 |
| `Size` | 扇区数（= End − Start + 1） | 交叉核对 |

把四个数抄到纸上（或记在主机上）：

```text
userdata 分区号      N = 34
userdata 起始扇区    S = 4096000
userdata 结束扇区    E = 500118158
磁盘总扇区数         T = 500118192
```

再用人类可读的单位交叉核对一次：

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda print"
```

`Size` 列会显示成 `254GB` 这样的值。注意 parted 默认用**十进制**单位（1GB = 10⁹ 字节），不要和 GiB 混；`E − S + 1` 个扇区换算成字节后应该与它接近（差几十 MiB 是正常的，分区表有对齐与保留）。

**关于扇区大小（重要，别跳过）**：

- `parted unit s` 里的 `s` 是 **GPT 逻辑扇区**，不等于固定 512 字节。
- `print` 显示 `512B/4096B` → 1 扇区 = 512 B；显示 `4096B/4096B` → 1 扇区 = 4096 B。
- 本文所有算例都按 **512 B 扇区**（1 GiB = 2097152 扇区）。若你的设备是 4096 B 扇区，1 GiB = 262144 扇区，把下文的 `2097152` 换掉即可；`S`/`E` 直接照抄，不需要换算。
- `fastboot getvar partition-size:boot_b` 的 `0xC000000` 是**字节**，与扇区大小无关。

### 3.5 计算新分区的起止扇区

先决定给 Android 的 `userdata` 留多少。约束：

- `linux` 必须 ≥ **16 GiB**（等于 rootfs 镜像大小，否则第 5 章刷不进去）；
- 建议 `linux` ≥ 32 GiB，这样首启扩容后还有空间装东西；
- `userdata` 留多少只影响 Android 侧可用空间，不影响 Linux。

公式（512 B 扇区，1 GiB = 2097152 扇区）：

```text
U = S + 保留给 userdata 的 GiB × 2097152 − 1     # 新 userdata 的结束扇区
L = U + 1                                        # linux 的起始扇区
linux 结束扇区 = E                               # 沿用原来的末尾
linux 扇区数  = E − L + 1
```

为什么 `−1` / `+1`：`Start` 与 `End` 都是**含端点**的扇区号，两个相邻分区不能共用同一个扇区。少算 1 个扇区（`L = U`）会重叠，`parted` 直接报错；多算 1 个则留下 1 个扇区的空洞，不报错但对不上核对表。

**算例：256 GB 机型，给 Android 留 32 GiB**

| 量 | 计算 | 值 |
|---|---|---|
| 1 GiB 的扇区数 | 1024³ ÷ 512 | 2097152 |
| 保留 32 GiB | 32 × 2097152 | 67108864 扇区 |
| 新 `userdata` 结束 `U` | 4096000 + 67108864 − 1 | **71204863** |
| `linux` 起始 `L` | 71204863 + 1 | **71204864** |
| `linux` 结束 | = `E` | **500118158** |
| `linux` 扇区数 | 500118158 − 71204864 + 1 | 428913295 扇区 |
| `linux` 容量 | 428913295 × 512 | 219603607040 B ≈ **204.5 GiB** |
| 对齐检查 | 71204864 ÷ 2048 | 34768，整除 ✅ |

对齐说明：`parted` 对未按 1 MiB（2048 扇区）对齐的分区会警告

```text
Warning: The resulting partition is not properly aligned for best performance: <start>s % 2048s != 0s
```

这只是性能提示，不阻止创建。想让警告消失，把 `L` 向上取整到 2048 的整数倍，并把多出的扇区从 `userdata` 里扣掉：

```text
L = ((U + 1 + 2047) ÷ 2048) × 2048
U = L − 1
```

**不同机型的参考值**（`T` 与 `E` 以你的 `print` 为准；`E = T − 34`）：

| 机型标称 | 磁盘总扇区 `T` | 给 `userdata` | `U` | `L` | `linux` 容量 |
|---|---|---|---|---|---|
| 128 GB | 250069680 | 32 GiB | 71204863 | 71204864 | ≈ 85.3 GiB |
| 256 GB | 500118192 | 32 GiB | 71204863 | 71204864 | ≈ 204.5 GiB |
| 256 GB | 500118192 | 64 GiB | 138313727 | 138313728 | ≈ 172.5 GiB |

把这四个值也抄下来：

```text
新 userdata 结束   U = 71204863
linux 起始         L = 71204864
linux 结束         E = 500118158
```

### 3.6 （可选）先缩小 `userdata` 的文件系统

**什么时候需要做**：想保留 Android 侧数据（双系统共存），且 `userdata` 是**未加密的 ext4**。
**什么时候跳过**：不在乎 Android 数据（本教程的默认假设），或者 `userdata` 是加密的。

先判定：

```sh
/usr/bin/adb shell "blkid /dev/block/sda36"
```

（`sda36` 换成 3.3.7 查到的 `userdata` 节点）

| 输出 | 结论 |
|---|---|
| 含 `TYPE="ext4"` | 明文 ext4，可以走下面的收缩流程 |
| 含 `TYPE="f2fs"` | f2fs **不支持缩小**，跳过本节 |
| 空输出 | 分区被 metadata encryption / FBE 包着，裸设备上是密文，`resize2fs` 不可能成功，跳过本节 |
| `Permission denied` | 不在 root shell，回 [3.3.4](#334-确认-adb-是-root) |

> 现代小米平板的 `userdata` 默认启用加密，因此**大多数设备会走到「跳过」这一支**。跳过不代表出错：分区重建后 Android 需要重新格式化 data 才能开机，而本教程本来就假设 Android 数据会丢。

收缩三步，**顺序不能颠倒**（先缩文件系统，再缩分区；反过来文件系统会超出分区）：

```sh
/usr/bin/adb shell "e2fsck -f -y /dev/block/sda36"
/usr/bin/adb shell "resize2fs /dev/block/sda36 31G"
/usr/bin/adb shell "dumpe2fs -h /dev/block/sda36 | head -20"
```

- 第 1 步：强制检查文件系统一致性。退出码 `0` = 干净、`1` = 已修正问题（都算通过），`≥2` 是错误，**不要**带着错误继续；`Fix?` 之类交互已被 `-y` 自动回答，但仍要看清楚它改了什么。
- 第 2 步：把文件系统缩到 **31G**（比新分区 32 GiB 小一点，留余量）。期望 `The filesystem on /dev/block/sda36 is now ... blocks long.`
- 第 3 步：`Block count` × `Block size` 应 ≤ 32 GiB。
- `resize2fs` 收缩期间断电或报错可能损伤文件系统。**没有备份就不要执行本节。**

### 3.7 改分区表：删旧 `userdata`，建新 `userdata` + `linux`

> ⚠️ 从这里开始，**每条命令都立即写入磁盘**。`parted` 没有「保存」和「撤销」——`quit` 只是退出，不是提交。
> 所以：数字先抄在纸上，逐条执行，每条之后 `print` 复查。

进入交互式 `parted`：

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda"
```

提示符变成 `(parted)`。逐条输入（`<…>` 换成你抄下来的数字）：

```text
(parted) unit s
(parted) print
```

再核对一遍 `N`、`S`、`E` 没抄错。

```text
(parted) rm 34
```

删除旧 `userdata` 分区项（`34` 换成你的 `N`）。

- 成功时**没有任何输出**，这是正常的；
- 此时分区表里已经没有 `userdata`，下一个命令就把它建回来；
- 若报 `Error: Partition doesn't exist.` → 分区号抄错了，回 `print` 重看。

```text
(parted) mkpart userdata ext4 4096000s 71204863s
```

按 `S` 到 `U` 重建 `userdata`（`4096000s` → 你的 `S`，`71204863s` → 你的 `U`）。

- `mkpart` 的第一个参数**就是分区名**，所以建完 `Name` 列直接显示 `userdata`；
- 第二个参数是「文件系统类型提示」，写 `ext4`/`f2fs` 都只影响提示，不影响实际内容（真正的文件系统由第 5 章的镜像决定）；
- 成功时无输出。

```text
(parted) print
```

确认新 `userdata` 的 `Number`（通常复用被删掉的 34）与起止扇区。**若 `Name` 列为空**，补一条 `name <编号> userdata`。

```text
(parted) mkpart linux f2fs 71204864s 500118158s
```

按 `L` 到 `E` 创建 `linux`（`71204864s` → 你的 `L`，`500118158s` → 你的 `E`）。

可能出现的输出与处理：

| 输出 | 含义 | 处理 |
|---|---|---|
| （无输出） | 成功 | 继续 |
| `Warning: The resulting partition is not properly aligned for best performance: …` | 起始扇区没按 2048 对齐 | 可以继续（性能略差）；想消除就 `rm` 掉重算，见 [3.5](#35-计算新分区的起止扇区) |
| `Error: You requested a partition from …s to …s` + `The closest location we can manage is …` | 起止与已有分区重叠 | 停下来核对 `L`/`E`，不要接受「最接近的位置」，它会建出错误的分区 |
| `Error: Can't have the end before the start!` | 起止写反了 | 重新输入 |
| `Error: The location …s is outside of the device` | 结束扇区超过磁盘 | `E` 抄错了，回 `print` |
| `Error: Partition(s) … have been written, but we have been unable to inform the kernel of the change` | 内核没重读分区表 | **磁盘上的 GPT 是对的**，重启后生效，见 [3.8](#38-验证分区已正确创建) |

```text
(parted) print
```

逐项核对（这是全教程最重要的核对点）：

| 检查项 | 期望 |
|---|---|
| 分区数量 | 比原来多 1 个（`userdata` + `linux`） |
| 新 `userdata` 的 `Start`/`End` | 与 `S` / `U` **完全一致** |
| `linux` 的 `Start`/`End` | 与 `L` / `E` **完全一致** |
| `Name` 列 | `userdata` / `linux`（**大小写必须一致**，内核按名字找根分区） |
| 相邻关系 | `userdata` 的 `End` + 1 = `linux` 的 `Start` |
| `linux` 的 `Size` | `E − L + 1`，与 [3.5](#35-计算新分区的起止扇区) 算的一致 |
| 末尾 | `linux` 的 `End` 不大于原 `userdata` 的 `End` |

```text
(parted) quit
```

退出。可能打印 `Information: You may need to update /etc/fstab.`——与本项目无关（根分区按 `PARTLABEL` 挂载），忽略。

### 3.8 验证分区已正确创建

#### 第一步：重启回 fastboot

```sh
/usr/bin/adb reboot bootloader
```

为什么必须重启：内核与 ueventd 需要重新读取 GPT，`/dev/block/by-name/linux` 与 `/dev/disk/by-partlabel/linux` 才会出现。**在同一个 recovery 会话里看不到新分区节点是正常的，不代表没建成。**

若 `adb reboot bootloader` 不生效：长按电源 15 秒关机 → 音量下 + 电源。

#### 第二步：用 fastboot 做权威校验

GPT 由 bootloader 读取，这是最可信的来源：

```sh
/usr/bin/fastboot getvar partition-size:linux
/usr/bin/fastboot getvar partition-size:userdata
```

期望（十六进制，单位是**字节**）：

```text
partition-size:linux: 0x3321651E00
partition-size:userdata: 0x800000000
```

把十六进制换算成字节核对（把 `0x…` 换成实际打印的值）：

```sh
printf '%d\n' 0x3321651E00
```

- `linux` 应为 `(E − L + 1) × 512`；上面的例子是 `428913295 × 512 = 219603607040`（`0x3321651E00`）。
- `userdata` 应为 `(U − S + 1) × 512`；例子是 `67108864 × 512 = 34359738368`（`0x800000000`，正好 32 GiB）。

对不上就是算错了或抄错了，回 [3.5](#35-计算新分区的起止扇区) 重算；差得离谱时按 [9.7](#97-分区相关故障) 处理。

#### 第三步（可选）：在 recovery 里看内核视图

此时设备在 fastboot 里，没有 `adb`。重新临时启动一次 recovery：

```sh
/usr/bin/fastboot boot /srv/elish/recovery.img
/usr/bin/adb wait-for-device
/usr/bin/adb shell "cat /proc/partitions"
```

应能看到新编号的分区（例如 `sda34`、`sda35`）及其大小（单位 1 KiB 块）。这一步只读，不写 flash。

#### 第四步：Linux 启动后确认标签

```sh
/usr/bin/ls -l /dev/disk/by-partlabel/linux
```

期望 `linux -> ../../sdaNN`。

- 这一步**只能在 Linux 启动之后做**：`/dev/disk/by-partlabel/` 是 udev 建的，recovery 里没有；
- 看不到这个链接，但 `getvar partition-size:linux` 有值 → 标签名不是 `linux`。改名要重新进 recovery 做（运行中的 Linux 占着 `/dev/block/sda`，不能在线改），命令是 `/cache/parted /dev/block/sda name <编号> linux`，改完重启让 udev 重建链接。

### 3.9 风险与救援

| 误操作 | 后果 | 救援 |
|---|---|---|
| `rm` 错了分区号（删掉了 `boot_b`/`super`/`metadata`） | 该分区内容仍在磁盘上，但 GPT 项没了，系统暂时找不到 | 用 `mkpart` **按记下来的原起止扇区**重建同名分区，内容通常还能读。**前提是你记过起止扇区** |
| `mkpart` 起止重叠 / 写反 / 超出磁盘 | `parted` 直接拒绝，不写入 | 重新输入正确值即可 |
| `mkpart` 名字打错（`Linux`、`linuxroot`） | 内核找不到 `PARTLABEL=linux`，开机停在 initramfs | 回 recovery：`parted /dev/block/sda name <编号> linux` |
| 分区表写到一半断电 | 主/备 GPT 不一致 | 重进 recovery 跑 `parted /dev/block/sda print`，按提示 `Fix` 修复备份 GPT |
| 把 `linux` 建在磁盘中间、留下空洞 | 空间浪费，但能用 | 不影响安装，可后续整理 |
| 完全改坏，recovery 也进不去 | 可能只剩 EDL | **EDL（9008）救援本项目未验证**，按不可逆处理 |

两条铁律：

1. **`rm` 之前**，把 `N`/`S`/`E` 抄在纸上（不是只放在屏幕里）。
2. **`quit` 之前**，把 `print` 的每一行与自己算的数字对一遍。

### 3.10 分区完成后的状态

此时磁盘上应该是：

- `userdata`：重建为较小分区，内容已不可用（Android 需要重新格式化 data 才能开机）；
- `linux`：存在、名字正确、大小符合计算；
- `boot_b`：还是空的（或临时装着 recovery，如果你用了 3.3.3）；
- 其它分区：一个都没动。

接下来回 fastboot 刷 `boot_b`（第 4 章）和 `linux`（第 5 章）。

---

## 4. 刷写 `boot_b`（区分 CSOT / BOE）

### 4.1 确认还在 fastboot

```sh
/usr/bin/fastboot devices
/usr/bin/fastboot getvar current-slot
```

- 期望第一行列出设备；
- `current-slot` 为 `a` 或 `b` 都可以，第 5 章会明确设成 `b`。

### 4.2 选对镜像

| 面板 | 文件 | 大小参考 |
|---|---|---|
| CSOT | `/srv/elish/boot.img` | 约 16 MiB（16,789,504 字节，随 Release 变动） |
| BOE | `/srv/elish/boot-boe-alt.img` | 同上 |

`boot_b` 只有 192 MiB（393216 扇区 = `0xC000000` 字节），正常镜像远小于上限。**刷错面板 = 黑屏**，先按 [0.3](#03-两种面板先确认再下载) 确认型号。

### 4.3 刷入

```sh
/usr/bin/fastboot flash boot_b /srv/elish/boot.img
```

BOE 面板把最后一段换成 `/srv/elish/boot-boe-alt.img`。

期望输出：

```text
Sending 'boot_b' (16396 KB)                        OKAY [  0.412s]
Writing 'boot_b'                                   OKAY [  0.108s]
Finished. Total time: 0.536s
```

怎么读这三行：

- 第一行 `Sending … OKAY` = 镜像已传到设备内存，**还没写**；
- 第二行 `Writing … OKAY` = 真的写进分区了，**这行才是成功标志**；
- 第三行只是总耗时。

只有 `Sending` 成功而 `Writing` 报错 = **没写进去**，按 [4.6](#46-失败处理) 处理。

### 4.4 校验

```sh
/usr/bin/fastboot getvar partition-size:boot_b
```

期望 `partition-size:boot_b: 0xC000000`（192 MiB）。

**可选：从设备回读比对 SHA256**（最硬的证据，需要系统能启动，见 [7.4](#74-验证清单逐项打勾)）：

```sh
IMG_BYTES=$(/usr/bin/stat -c '%s' /srv/elish/boot.img)
echo $((IMG_BYTES / 4096))          # 得到要读的 4 KiB 块数，例如 4099
```

设备上（块数用上面的结果）：

```sh
/usr/bin/sudo /usr/bin/dd if=/dev/disk/by-partlabel/boot_b bs=4096 count=4099 | /usr/bin/sha256sum
```

结果应与主机上 `/usr/bin/sha256sum /srv/elish/boot.img` 完全一致。原理：`boot.img` 是 4096 的整数倍长度，`count` 取「字节数 ÷ 4096」正好读完整个镜像，不会带上分区尾部的残留。

### 4.5 关于 `dtbo_b`：为什么本教程不擦

上游的 elish 教程（Armbian、postmarketOS wiki）在刷 `boot_b` 之前都要求：

```text
# 上游 Armbian / postmarketOS wiki 的做法 —— 本教程不执行这条命令
fastboot erase dtbo_b
```

本教程**不包含**这条命令，原因有三：

1. 本项目的硬约束是「只允许写 `boot_b` 与 `linux`，任何 `erase` 都不做」；
2. `boot.img` 内的 dtb 是**追加在镜像里**的（`deviceinfo_append_dtb`），理论上不依赖 `dtbo` 分区；
3. **技术上的不确定性（如实说明）**：ABL 是否仍会把 b 槽的 Android dtbo overlay 叠加到自带 dtb 上，目前没有一手证据（见 `docs/research/flash-layout.md` 的 U3）。本仓库的测试设备上 `dtbo_b` 早已被清空过，所以「全新、未擦 `dtbo_b` 的设备能否直接启动」**未经验证**。

因此排障顺序是：

1. 首次开机黑屏/早期挂死 → **先按 [9.2](#92-黑屏最需要分清的一种) 排除面板刷错**（这是最常见原因）；
2. 面板确认无误、仍然黑屏 → `dtbo` 是可疑项之一，按 `docs/decisions.md` 的 D-009 受控流程处理（一次性、显式开关、留痕），**而不是随手 erase**；
3. 结论与证据请回报到 issue，这一条需要真机数据才能定论。

### 4.6 失败处理

| 现象 | 原因 | 处理 |
|---|---|---|
| `FAILED (remote: 'partition size too small')` | 镜像超过 192 MiB | 不是本 Release 的正常情况，换 Release |
| `FAILED (remote: 'not allowed in locked state')` | 未解锁 | 回 [2.1](#21-解锁-bootloader一次性会清空数据) |
| `Writing 'boot_b' FAILED (remote: …)` | 分区写保护 / 分区名不存在 | 用 `getvar partition-size:boot_b` 确认分区存在；记录完整报错 |
| `Sending` 阶段就失败 | USB 断连、线材问题 | 换线、换 USB 口，重刷 |
| 刷完黑屏但 `fastboot devices` 正常 | 面板刷错 | 重刷另一种 `boot.img` |
| `fastboot: error: cannot load '…'` | 路径或文件问题 | 用绝对路径，`ls -l` 确认存在且非 0 字节 |

---

## 5. 刷写 rootfs 到 `linux` 分区

### 5.1 先算容量，再动手

```sh
/usr/bin/stat -c '%s' /srv/elish/linux-rootfs-f2fs.img
```

期望 `17179869184`（16 GiB）。

```sh
/usr/bin/fastboot getvar partition-size:linux
```

期望一个十六进制字节数。两者比较：

| 情况 | 结论 |
|---|---|
| 分区 ≥ 镜像（≥ 16 GiB） | 可以刷。建议 ≥ 32 GiB，否则首启扩容没有意义 |
| 分区 < 镜像 | **不能刷**，第 3 章算错或分区太小；回 [3.7](#37-改分区表删旧-userdata建新-userdata--linux) 重建 |
| 分区 = 镜像（正好 16 GiB） | 能刷，但没有余量；首启扩容不会带来任何空间 |

把十六进制换成字节核对（把 `0x…` 换成实际值）：

```sh
printf '%d\n' 0x3321651E00
```

### 5.2 刷入

```sh
/usr/bin/fastboot flash linux /srv/elish/linux-rootfs-f2fs.img
```

期望输出形如（16 GiB 会被 fastboot 自动分片传输）：

```text
Sending sparse 'linux' 1/2 (524284 KB)             OKAY [ 12.331s]
Writing 'linux' 1/2                                OKAY [  3.782s]
Sending sparse 'linux' 2/2 (…)
Writing 'linux' 2/2                                OKAY [ … ]
Finished. Total time: …
```

- 分片数量取决于设备的 `max-download-size`，不一定是 2 片；
- 耗时：USB 2.0 约 5–15 分钟，USB 3.0 约 1–3 分钟；
- **全程不要拔线、不要让主机休眠、不要中途 Ctrl-C**；
- 失败后直接重刷即可：`flash` 是覆盖写，不需要先擦除（本项目也不允许擦除）。

### 5.3 切到 b 槽

```sh
/usr/bin/fastboot --set-active=b
```

期望 `Setting current slot to 'b'                        OKAY [  0.007s]`。

等价写法：`/usr/bin/fastboot set_active b`。两种都被拒绝时见 [2.5](#25-fastboot-失败对照表)。

**为什么必须做**：ABL 默认从 `_a` 启动（Android 在 a 槽）。不切槽，刷好的 Linux 不会被引导，设备会直接进 Android。

**这条命令写了什么**：`--set-active` 由 bootloader 把「活动槽」标记写进槽位元数据（`misc` 分区），不改任何文件系统内容，也不写 `boot_a`/`super`/`userdata`。这是 A/B 机型切换启动槽的唯一方式，上游 Armbian 流程同样如此。本项目不 `flash`、不 `erase` `misc`。

### 5.4 失败处理

| 现象 | 原因 | 处理 |
|---|---|---|
| `Not enough space to write` / `FAILED (remote: 'size too large')` | 分区小于镜像 | 回 [5.1](#51-先算容量再动手) 核对；需要重建分区 |
| `Writing 'linux' FAILED (remote: 'No such partition')` 或 `cannot find partition` | `linux` 分区不存在 / 名字不是 `linux` | 回 [3.8](#38-验证分区已正确创建) 用 `getvar partition-size:linux` 复查 |
| 写到一半 `USB disconnect` | 线材/供电/主机休眠 | 换口换线，重刷整条命令 |
| `--set-active=b` 报 `unknown command` | ABL 命令差异 | 试 `set_active b`；仍失败则记录完整输出 |
| 刷完重启进入 Android 而不是 Linux | 槽位没切成功 | `fastboot getvar current-slot` 应为 `b`；不是就重试切槽 |

---

## 6. 首次开机与自动扩容

### 6.1 重启

```sh
/usr/bin/fastboot reboot
```

拔掉 USB 线（不拔也能用，但供电/网络会干扰判断）。

### 6.2 开机时间线

| 时间 | 现象 | 说明 |
|---|---|---|
| 0–5 s | 小米 logo 或短暂黑屏 | ABL 校验并加载 `boot.img` |
| 5–20 s | 屏幕开始滚内核日志 | `console=tty0 loglevel=4` 就是设计如此，日志上屏不是崩溃 |
| 日志早期 | `elish-init: found root device …`、`root mounted read-only as f2fs` | initramfs 找到了 `linux` 分区 |
| 紧接着 | `elish-init: resize: growing f2fs from … to … blocks` | 自动扩容，见 [6.3](#63-自动扩容做了什么) |
| 20 s–3 min | 出现登录提示或直接进桌面 | systemd 启动；`elish-bt-autoconnect` 会做若干轮蓝牙重连，占 1–2 分钟，属正常 |
| ~3 min | MangoWM 桌面 + waybar 状态栏 | 可以开始操作 |

首次开机比之后每次慢：要扩容、要首次初始化 f2fs、还要等蓝牙重连超时。

### 6.3 自动扩容做了什么

- rootfs 镜像构建时**固定 16 GiB**（主机上不可能造一个和目标分区等大的文件，因为分区大小由用户决定）；
- initramfs 在**挂载根分区之前**读取 f2fs 超级块里的块数，与分区扇区数比较；
- 文件系统比分区小 → 先 `fsck.f2fs -fp`，再 `resize.f2fs -f -F`，把文件系统扩到分区末尾；
- 详细日志写到 `/var/log/resize.log`，同时打印到内核日志。

为什么必须在挂载前：`resize.f2fs` 只支持离线扩容。

### 6.4 验证扩容

设备上（登录后）：

```sh
/usr/bin/findmnt /
/usr/bin/df -h /
/usr/bin/cat /var/log/resize.log
/usr/bin/sudo /usr/bin/dmesg | /usr/bin/grep -i 'elish-init: resize'
```

| 检查 | 期望 |
|---|---|
| `findmnt /` | `FSTYPE` 为 `f2fs`，`SIZE` ≈ 你的 `linux` 分区大小 |
| `df -h /` | `Size` 与上一行一致（差几十 MiB 是 f2fs 元数据与保留段，正常） |
| `/var/log/resize.log` | 含 `fs_blocks=…`、`part_sectors=…`、`=== resize.f2fs -f -F ===`、`resize_rc=0` |
| `dmesg` | 有 `elish-init: resize: growing f2fs from N to M blocks` |

扩容成功后，`df -h /` 显示的容量应当是**你的 `linux` 分区实际大小**——不是 16 GiB，也不是任何固定值。

### 6.5 没扩容怎么办

| 日志/现象 | 原因 | 处理 |
|---|---|---|
| `resize: resize.f2fs missing from initramfs, skipping` | 这个 `boot.img` 的 initramfs 没带 f2fs-tools（旧产物） | 重刷本 Release 的 `boot.img` 后重启（第 4 章）；或按下面手工扩容 |
| `resize: filesystem already fills the partition` | 镜像本来就等于分区大小 | 不需要处理 |
| 日志里 `resize_rc=` 非 0 | 扩容执行失败 | 手工扩容，并把日志原文回报 |
| `df -h /` 只有 16 GiB，日志里也没有 `resize` 行 | initramfs 更旧 | 重刷 `boot.img` |

手工扩容（设备上，根已挂载）：

```sh
/usr/bin/sudo /usr/bin/resize.f2fs /dev/disk/by-partlabel/linux
/usr/bin/df -h /
```

- 执行前确认电量 > 30%、`findmnt /` 正常；
- 若 `resize.f2fs` 报文件系统已挂载并拒绝，就回到「重刷 `boot.img` 让 initramfs 在挂载前扩容」这条路。

### 6.6 第一次进桌面

- `tty1` 会自动登录 `magma` 并启动 MangoWM 会话，不需要手动登录；
- 如果屏幕停在文本界面、只有一个 `-fish` 提示符，说明会话没起来 → 见 [9.4](#94-桌面起不来)；
- 触摸、旋转、缩放由合成器处理，不需要手工配置。

---

## 7. 首启必做：改口令、连 Wi-Fi、验证

### 7.1 改口令（第一件事）

```sh
/usr/bin/passwd
```

- 先输入**当前**口令（Release 说明里的默认口令，本项目为 `elish`），再输入两遍新口令；
- 期望 `passwd: password updated successfully`；
- 默认用户是 `magma`，`root` 默认锁定（用 `sudo`，不要用 `su`）。

为什么必须改：默认口令是**同一个镜像共享**的，任何拿到这个 Release 的人都知道它。改完之后，SSH 和其他需要口令的入口才真正属于你。

> 注意：`passwd` 会先要求输入**当前**口令。若你确实不知道当前口令，可以用免密的 `sudo` 直接改（`magma` 属于 `wheel` 且配置了 `NOPASSWD`）：
>
> ```sh
> /usr/bin/sudo /usr/bin/passwd magma
> ```
>
> 这条命令不需要当前口令，只需要物理接触设备（或已在桌面会话里）。

### 7.2 连接 Wi-Fi

图形界面：点 waybar 右侧网络图标 → 选 SSID → 输密码。

命令行（推荐，可验证）：

```sh
/usr/bin/sudo /usr/local/bin/elish-wifi-setup
```

交互式询问 SSID 与密码，由 NetworkManager 写入 profile（权限 0600），密码不会出现在命令行历史里。

也可以直接 `nmcli`。**注意加 `sudo`**：本系统的 polkit 没有认证代理，普通用户改 NetworkManager 配置会被拒绝。

```sh
/usr/bin/sudo /usr/bin/nmcli device wifi list
/usr/bin/sudo /usr/bin/nmcli device wifi connect "<wifi-ssid>" password "<wifi-password>"
/usr/bin/ip -br addr show
```

期望 `ip -br addr show` 里无线接口（`wlan0` 或 `wlp1s0`，以实际输出为准）带 `192.168.x.x/24` 之类的地址。

验证连通：

```sh
/usr/bin/ping -c 3 1.1.1.1
/usr/bin/getent hosts archlinux.org
```

失败对照：

| 现象 | 原因 | 处理 |
|---|---|---|
| `No network with SSID '<wifi-ssid>' found` | SSID 拼错、隐藏网络、5 GHz 信道不被支持 | `/usr/bin/sudo /usr/bin/nmcli device wifi rescan` 后重试；隐藏网络用 `hidden yes` |
| 没有无线接口 | `rfkill` 被 block，或 MAC 全 0 导致接口起不来 | `/usr/bin/rfkill list`；`/usr/bin/sudo /usr/bin/systemctl restart wlp-mac.service` |
| 有接口但拿不到地址 | DHCP 失败 | `/usr/bin/sudo /usr/bin/nmcli device status` 看状态；`/usr/bin/sudo /usr/bin/journalctl -u NetworkManager -b --no-pager \| tail -30` |

### 7.3 （可选）开 SSH

```sh
/usr/bin/sudo /usr/bin/systemctl enable --now sshd
/usr/bin/systemctl status sshd --no-pager
/usr/bin/ip -br addr show
```

主机侧：

```sh
/usr/bin/ssh magma@<device-ip>
```

- 设备上的交互 shell 是 **fish**：粘贴含 `$变量` 或 `$(…)` 的 bash 片段会报 `fish: 在此 $ 之后应有变量名称`。需要时用 `/usr/bin/bash -c '…'` 包一层。
- `magma` 有免密 `sudo`，`sudo -n` 可直接用。

### 7.4 验证清单（逐项打勾）

| # | 检查项 | 命令 | 期望输出 | 不符时 |
|---|---|---|---|---|
| 1 | 内核版本 | `/usr/bin/uname -v` | 非空，且与 Release 说明的构建时间一致 | [9.3](#93-卡开机--反复重启) |
| 2 | 内核 cmdline | `/usr/bin/cat /proc/cmdline` | 含 `root=PARTLABEL=linux` 与 `systemd.gpt_auto=no` | [9.3](#93-卡开机--反复重启) |
| 3 | 根文件系统 | `/usr/bin/findmnt / -o SOURCE,FSTYPE,SIZE` | `f2fs`，`SIZE` ≈ 你的 `linux` 分区大小 | [6.5](#65-没扩容怎么办) |
| 4 | 可用空间 | `/usr/bin/df -h /` | `Size` 与第 3 项一致 | [6.5](#65-没扩容怎么办) |
| 5 | GPU 固件 | `/usr/bin/sudo /usr/bin/dmesg \| /usr/bin/grep -i a650` | 含 `loaded qcom/a650_sqe.fw`、`a650_gmu.bin` | [9.2](#92-黑屏最需要分清的一种) |
| 6 | DRM 节点 | `/usr/bin/ls /dev/dri/` | `card0`、`renderD128` | [9.2](#92-黑屏最需要分清的一种) |
| 7 | 面板驱动 | `/usr/bin/sudo /usr/bin/dmesg \| /usr/bin/grep -i nt36523` | 有 panel 驱动绑定日志 | [9.2](#92-黑屏最需要分清的一种) |
| 8 | 合成器 | `/usr/bin/pgrep -a -x mango` | 一行 `/usr/bin/mango` | [9.4](#94-桌面起不来) |
| 9 | 状态栏 | `/usr/bin/pgrep -a -x waybar` | 一行 `waybar` | [9.4](#94-桌面起不来) |
| 10 | 触摸 | `/usr/bin/cat /proc/bus/input/devices \| /usr/bin/grep -i NVT` | `NVTCapacitiveTouchScreen` | [9.4](#94-桌面起不来) |
| 11 | 声卡 | `/usr/bin/cat /proc/asound/cards` | 含 `sm8250` | [10](#10-已知限制) |
| 12 | 音频服务 | `/usr/bin/wpctl status \| /usr/bin/head -20` | 有 Sink（内置扬声器） | [10](#10-已知限制) |
| 13 | 蓝牙控制器 | `/usr/bin/sudo /usr/bin/btmgmt info` | `Index list with 1 item`，地址非 `00:00:00:00:00:00` | [9.5](#95-蓝牙适配器消失) |
| 14 | 蓝牙服务 | `/usr/bin/systemctl is-active bluetooth` | `active` | [9.5](#95-蓝牙适配器消失) |
| 15 | 失败单元 | `/usr/bin/systemctl --failed --no-pager` | 只有 `systemd-networkd-wait-online.service` | [10](#10-已知限制) |
| 16 | 启动耗时 | `/usr/bin/systemd-analyze` | userspace 2–3 分钟（蓝牙重连等待） | [10](#10-已知限制) |
| 17 | `boot_b` 内容（可选） | 见下方代码块 | 与 Release 的 `boot.img` SHA256 一致 | [4.6](#46-失败处理) |

第 17 项的做法（`count` = `boot.img` 字节数 ÷ 4096）：

```sh
/usr/bin/sudo /usr/bin/dd if=/dev/disk/by-partlabel/boot_b bs=4096 count=4099 | /usr/bin/sha256sum
```

结果与主机上 `/usr/bin/sha256sum /srv/elish/boot.img` 相同即通过。

### 7.5 看一眼系统日志

```sh
/usr/bin/systemctl is-system-running
/usr/bin/systemctl --failed --no-pager
/usr/bin/sudo /usr/bin/journalctl -b -p err --no-pager
```

期望：

- `is-system-running` 输出 `degraded`——**这是已知且正常的**，唯一失败单元是 `systemd-networkd-wait-online.service`；
- `-p err` 里没有新的、与显示/存储/输入相关的错误；既有的上游告警可以忽略。

---

## 8. 日常更新

三类更新，通道完全不同：

| 更新内容 | 走哪个通道 | 需要 fastboot 吗 | 会丢数据吗 |
|---|---|---|---|
| 内核 `Image`、dtb、initramfs | 新的 `boot.img` → `boot_b` | 要 | 不会 |
| 只有内核模块（`.ko`） | `modules.tar.zst` → `/lib/modules` | 不要 | 不会 |
| 用户态软件包 | `pacman -Syu` | 不要 | 不会 |

判断标准：**只有 `Image`/dtb/initramfs 变了才需要碰 `boot_b`**；纯模块补丁只更新 `modules.tar.zst`。

### 8.1 只换内核/引导

设备上关机：

```sh
/usr/bin/sudo /usr/bin/poweroff
```

设备进 fastboot（音量下 + 电源），主机上：

```sh
/usr/bin/fastboot devices
/usr/bin/fastboot flash boot_b /srv/elish/boot.img
/usr/bin/fastboot reboot
```

起来后确认：

```sh
/usr/bin/uname -v
```

- 期望版本/构建时间与之前不同；
- **刷 `boot_b` 不碰 rootfs，数据安全**；
- 回滚：把上一版 `boot.img` 留着，重刷即可（同样不动数据）。

### 8.2 只换内核模块

主机传文件：

```sh
/usr/bin/scp /srv/elish/modules.tar.zst magma@<device-ip>:/home/magma/modules.tar.zst
```

设备上解包：

```sh
/usr/bin/sudo /usr/bin/tar --zstd -xf /home/magma/modules.tar.zst -C /
/usr/bin/sudo /usr/bin/depmod -a
/usr/bin/sudo /usr/bin/systemctl restart bluetooth
```

验证：

```sh
/usr/bin/ls -l /lib/modules/"$(/usr/bin/uname -r)"/kernel/drivers/bluetooth/ | /usr/bin/head
```

**注意**：`sudo tar … && depmod -a` 里的 `depmod` 会以普通用户运行并报 `Permission denied`——`depmod` 前面必须**单独再写一次 `sudo`**。这是最常见的静默失败。

### 8.3 系统包更新

```sh
/usr/bin/sudo /usr/bin/pacman -Syu
```

三条注意事项：

1. `/etc/pacman.conf` 已预置 `DisableSandboxFilesystem` / `DisableSandboxSyscalls`（本内核没有 Landlock），**不要删**，否则 pacman 会因沙箱初始化失败而报错；
2. **内核不由 pacman 管理**：内核与 initramfs 在 `boot_b`，rootfs 里没有内核包。装 `linux-aarch64` 只会往根分区塞一份用不上的内核，不要装；
3. 大更新后重启一次，确认能正常回来。

```sh
/usr/bin/sudo /usr/bin/systemctl reboot
```

### 8.4 更新后自检

```sh
/usr/bin/systemctl --failed --no-pager
/usr/bin/sudo /usr/bin/journalctl -b -p err --no-pager | /usr/bin/tail -30
```

出问题按第 9 章对照处理。

---

## 9. 故障恢复

### 9.1 速查表

| 现象 | 最可能原因 | 处理 | 验证 |
|---|---|---|---|
| 刷完黑屏，但 `fastboot devices` 能看到 | 面板刷错 | 重刷另一种 `boot.img` | 屏幕点亮 |
| 黑屏且 `fastboot devices` 看不到 | 死机/停在 EDL | 长按电源 15 秒，重进 fastboot；仍不行见 [9.8](#98-彻底重来保留-android-a-槽) | `fastboot devices` 有输出 |
| 卡在小米 logo / 反复重启 | `boot.img` 与设备不匹配 | 重刷 `boot_b` | 能滚内核日志 |
| 日志停在 `elish-init: … FATAL` | 找不到/挂不上根分区 | 重刷 `linux` | `findmnt /` 正常 |
| 能进 tty，桌面不起 | 会话启动竞态 | `sudo systemctl restart getty@tty1` | `pgrep -x mango` |
| 桌面在、状态栏没了 | waybar 崩 | 重启会话（同上） | `pgrep -x waybar` |
| 触摸无反应 | 面板驱动或合成器 | 先看 `/proc/bus/input/devices` | 有 NVT 节点 |
| 蓝牙适配器消失 | BD_ADDR 全 0 | `sudo systemctl restart elish-bt-mac.service` | `btmgmt info` 有 1 个 index |
| 蓝牙键鼠连不上 | 多是外设状态 | 见 [9.5](#95-蓝牙适配器消失) | 能建链 |
| Wi-Fi 接口不出现 | MAC 全 0 | `sudo systemctl restart wlp-mac.service` | `ip -br link` |
| 根没扩容 | `boot.img` 旧/缺 f2fs-tools | [6.5](#65-没扩容怎么办) | `df -h /` |
| `flash linux` 报找不到分区 | 第 3 章没做成 | [3.8](#38-验证分区已正确创建) | `getvar partition-size:linux` |
| 分区表报「备份 GPT 不在末尾」 | 分区表被别处改过 | [9.7](#97-分区相关故障) | `parted print` 无警告 |
| 根分区只读 / fsck 报错 | 断电或写坏 | 重刷 `linux`（数据丢） | 能正常挂载 |
| 忘了登录口令 | — | 重刷 `linux`（数据丢）；本项目不提供离线改密 | 能登录 |
| 时间不对 | 无 RTC，NTP 未同步 | `sudo systemctl restart systemd-timesyncd` | `timedatectl` |
| 完全刷坏 | — | [9.8](#98-彻底重来保留-android-a-槽) | 能进 fastboot |

### 9.2 黑屏（最需要分清的一种）

按顺序判断：

1. **`fastboot devices` 有设备吗？**
   - 有 → 设备活着，只是屏幕没亮 → 90% 是面板刷错 → 重刷另一种 `boot.img`（第 4 章）；
   - 没有 → 长按电源 15 秒强制关机 → 音量下 + 电源 → 再查一次。
2. **重刷另一种面板后仍黑？** → 看 [4.5](#45-关于-dtbo_b为什么本教程不擦) 的 `dtbo` 说明。
3. **屏幕亮但只有背光/花屏？** → 驱动起来了但时序不对，同样是 dtb 问题，重刷另一种 `boot.img`。
4. **完全无反应、充电也没反应？** → 可能进了 EDL（9008），见 [10](#10-已知限制) 的说明（救援未验证）。

### 9.3 卡开机 / 反复重启

1. 先进 fastboot，确认两个分区都读得到（见下方第一步的命令）；
2. 两个都有值 → 重刷 `boot_b`（不动数据），重启；
3. 仍不行 → 重刷 `linux`（**数据会丢**）；
4. 能进到 tty 但系统卡住 → 看上一次启动的日志（见下方第二步的命令）。

```sh
# 第一步：确认分区存在
/usr/bin/fastboot getvar partition-size:boot_b
/usr/bin/fastboot getvar partition-size:linux

# 第二步：设备上，看上一次启动的日志
/usr/bin/sudo /usr/bin/journalctl -b -1 --no-pager -o short-monotonic | /usr/bin/tail -50
```

### 9.4 桌面起不来

典型现象：能进 tty1，只有一个 `-fish` 提示符；或黑屏但有光标。

**急救**：

```sh
/usr/bin/sudo /usr/bin/systemctl restart getty@tty1
```

已知根因：登录脚本会「等系统时钟 ≥ 2024（最多 20 秒）→ `exec mango`」，设备没有 RTC，存在竞态。重启 `getty@tty1` 通常能救回来。

进一步排查：

```sh
/usr/bin/pgrep -a -x mango
/usr/bin/pgrep -a -x waybar
/usr/bin/sudo /usr/bin/journalctl -b --no-pager -o short-monotonic | /usr/bin/tail -50
/usr/local/bin/mango-session
```

（最后一条是在 tty 里手动起一次会话，报错会直接打在屏幕上。）

触摸没反应但桌面正常：

```sh
/usr/bin/cat /proc/bus/input/devices | /usr/bin/grep -i NVT
```

- 没有 `NVTCapacitiveTouchScreen` → 驱动没加载，重刷 `boot_b`；
- 有节点但没反应 → 合成器/输入配置问题，看 `journalctl -b` 里 libinput 相关行。

### 9.5 蓝牙适配器消失

```sh
/usr/bin/sudo /usr/bin/systemctl restart elish-bt-mac.service
/usr/bin/sudo /usr/bin/systemctl status elish-bt-mac.service --no-pager
/usr/bin/bluetoothctl list
/usr/bin/sudo /usr/bin/btmgmt info
```

期望 `Controller <derived-mac> elish [default]`，且 `btmgmt info` 有 `Index list with 1 item`。

原理一句话：QCA6390 上报的 BD_ADDR 接近全 0，内核因此不发布 mgmt index，bluez 自然看不到适配器；该 unit 会从设备自身的唯一序列号派生一个地址写回控制器。**不要手写固定 MAC**，那会让所有设备撞号。

蓝牙键鼠连不上时，按三层查：

```sh
/usr/bin/lsmod | /usr/bin/grep hidp
/usr/bin/sudo /usr/bin/journalctl -b -u bluetooth -o cat | /usr/bin/grep 'profiles/input'
```

| 现象 | 含义 | 该查什么 |
|---|---|---|
| `hidp` 没有输出 | HIDP 模块未加载 | 检查 `/etc/modules-load.d/elish-bluetooth-hid.conf` |
| `Host is down (112)` | L2CAP 建链失败 | **外设**没响应，不是这台设备的问题 |
| 没有 `profiles/input` 行 | 没走到 HID profile | 配对状态 / `hidp` |

多数「连不上」是外设自身状态（休眠、被别的主机占着）。**先按键唤醒再试**。

### 9.6 扩容失败

见 [6.5](#65-没扩容怎么办)。

### 9.7 分区相关故障

**`fastboot flash linux` 报找不到分区** → 回 [3.8](#38-验证分区已正确创建)，用 `getvar partition-size:linux` 复查；没有值就回第 3 章重做。

**`parted print` 提示备份 GPT 不在末尾**：

```text
Warning: Not all of the space available to /dev/block/sda appears to be used,
you can fix the GPT to use all of the space (an extra N blocks) or continue
with the current setting?
```

→ 输入 `Fix`（大写 F 开头即可），让 `parted` 把备份 GPT 移到磁盘末尾；`Ignore` 只读不改。改完必须重启，让内核重读分区表。

**分区重叠 / 编号冲突** → `rm` 掉后建的那个，按记下来的 `S`/`E` 重建，不要「让 parted 自己找最近的位置」。

**recovery 里 wipe** → 只 wipe `data`。不要 wipe `system`、`boot`、`persist`、`modem`、`metadata`。

### 9.8 彻底重来（保留 Android a 槽）

按顺序，只碰两个分区：

```sh
/usr/bin/fastboot devices
/usr/bin/fastboot flash boot_b /srv/elish/boot.img
/usr/bin/fastboot flash linux /srv/elish/linux-rootfs-f2fs.img
/usr/bin/fastboot --set-active=b
/usr/bin/fastboot reboot
```

- 分区还在 → 上面的顺序就够了；
- 分区没了 → 先回第 3 章重建；
- 连 fastboot 都进不去 → EDL，本项目未验证。

### 9.9 回到 Android

三种「回去」，代价完全不同：

| 目标 | 做法 | 代价 |
|---|---|---|
| 只想让 Android 能开机 | recovery 里 Wipe → **Format Data** | 清空 `userdata`；a 槽没动过，Android 能起来 |
| 彻底恢复单系统 | 刷小米官方 fastboot 包 | **会写全部分区，`linux` 分区被覆盖，Linux 数据全丢** |
| 保留 Linux，只把空间还给 Android | 见 [docs/partitioning.md](docs/partitioning.md) 第 9 节 | 需要删掉并重建分区，Linux 数据全丢 |

刷官方包属于「放弃 Linux」的路径，与本项目「只写两个分区」的约束冲突，本教程不提供具体命令。**刷之前先把 `linux` 分区里要留的东西拷出来。**

---

## 10. 已知限制

### 10.1 硬件与固件

| 限制 | 说明 |
|---|---|
| 快充约 10 W | 原厂 33 W 走小米私有 11 V 协议；标准 PD 9 V + 2:1 电荷泵只能到约 10 W |
| 无厂商 per-amp 调音 | 8 颗 CS35L41 使用通用固件，音色与原厂不同 |
| 传感器 DSP（SLPI）已永久关闭 | 上游会崩溃循环，关闭后部分传感器不可用 |
| 霍尔传感器/手写笔吸附检测不可用 | 上游状态为 Broken |
| 摄像头不可用 | 无驱动/无固件支持 |
| 无 RTC | 断电后时间可能错，靠 NTP 校正 |
| 待机功耗未系统验证 | 休眠/唤醒的功耗表现缺少真机长测数据 |

### 10.2 系统与软件

| 限制 | 说明 |
|---|---|
| 无 hexagonrpcd | ALARM 没有该包；录音、自动旋转等能力受限 |
| FEX-Emu 兼容性有限 | 已装可用，thunks 仍在打磨，不是所有 x86 程序都能跑 |
| 未预装浏览器/Waydroid/Flatpak 应用 | 需要时自行 `pacman` / `flatpak` 安装 |
| `systemd-networkd-wait-online` 失败 | 导致 `systemctl is-system-running` 显示 `degraded`，不影响 Wi-Fi 与桌面 |
| 内核无 Landlock | 因此 `pacman.conf` 里关掉了沙箱选项 |
| 没有 emoji 字体 | emoji 显示为方块；界面图标使用 Nerd Font 码位 |
| 首次开机 userspace 2–3 分钟 | 主要是蓝牙外设重连的等待，属设计行为 |
| polkit 没有认证代理 | 需要特权的操作走 `sudo`，不要指望图形授权弹窗 |

### 10.3 安装与刷机

| 限制 | 说明 |
|---|---|
| 只能写 `boot_b` 与 `linux` | 任何其它分区的 `flash`/`erase` 都不在支持范围内 |
| 不能在线调整 `linux` 分区大小 | 要 `rm` + `mkpart`，等于重刷 rootfs |
| 缩 `userdata` 后 Android 需重新格式化 data | 这是分区重建的必然结果 |
| 不与 Android 共用同一份 `userdata` | Linux 独占磁盘尾部空间 |
| `dtbo_b` 是否必须清空未验证 | 见 [4.5](#45-关于-dtbo_b为什么本教程不擦) |
| EDL（9008）救援未验证 | 分区表彻底写坏可能无法恢复 |
| BOE 面板产物未真机验证 | 见 [0.3](#03-两种面板先确认再下载) |
| 没有系统内的「重装」入口 | 恢复只能靠 fastboot |

---

## 附录 A：命令速查

| 目的 | 命令 |
|---|---|
| 看设备是否在 fastboot | `/usr/bin/fastboot devices` |
| 看解锁状态 | `/usr/bin/fastboot getvar unlocked` |
| 看当前槽位 | `/usr/bin/fastboot getvar current-slot` |
| 看 `boot_b` 容量 | `/usr/bin/fastboot getvar partition-size:boot_b`（期望 `0xC000000`） |
| 看 `linux` 容量 | `/usr/bin/fastboot getvar partition-size:linux` |
| 刷内核 | `/usr/bin/fastboot flash boot_b /srv/elish/boot.img` |
| 刷 rootfs | `/usr/bin/fastboot flash linux /srv/elish/linux-rootfs-f2fs.img` |
| 切槽 | `/usr/bin/fastboot --set-active=b` |
| 重启 | `/usr/bin/fastboot reboot` |
| 看分区表（recovery） | `/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"` |
| 进 recovery | `/usr/bin/fastboot boot /srv/elish/recovery.img` |
| 从 recovery 回 fastboot | `/usr/bin/adb reboot bootloader` |
| 校验下载 | `cd /srv/elish && /usr/bin/sha256sum -c /srv/elish/SHA256SUMS.txt` |
| 解压 rootfs | `/usr/bin/zstd -d /srv/elish/linux-rootfs-f2fs.img.zst -o /srv/elish/linux-rootfs-f2fs.img` |
| 看根分区 | `/usr/bin/findmnt /` |
| 看剩余空间 | `/usr/bin/df -h /` |
| 看扩容日志 | `/usr/bin/cat /var/log/resize.log` |
| 手工扩容 | `/usr/bin/sudo /usr/bin/resize.f2fs /dev/disk/by-partlabel/linux` |
| 看内核版本 | `/usr/bin/uname -v` |
| 看 cmdline | `/usr/bin/cat /proc/cmdline` |
| 看失败单元 | `/usr/bin/systemctl --failed --no-pager` |
| 看本次启动错误 | `/usr/bin/sudo /usr/bin/journalctl -b -p err --no-pager` |
| 改口令 | `/usr/bin/passwd` |
| 连 Wi-Fi | `/usr/bin/sudo /usr/local/bin/elish-wifi-setup` |
| 重启蓝牙 MAC 修复 | `/usr/bin/sudo /usr/bin/systemctl restart elish-bt-mac.service` |

> 主机上也有 `/usr/bin/parted`，但它**看不到设备的 UFS**，只能用来在镜像文件上演练分区命令。真正改设备分区必须在设备上的 recovery 里执行。

---

## 附录 B：与上游 Armbian 流程的差异

| 步骤 | Armbian 官方流程 | 本教程 | 原因 |
|---|---|---|---|
| 分区方式 | `resizepart` 缩 `userdata`，再 `mkpart linux` + 额外建一个 `esp` | `rm` 掉旧 `userdata`，重建较小 `userdata` + `mkpart linux`，**不建 `esp`** | `rm` + `mkpart` 不依赖文件系统当前状态；`esp` 不在本项目白名单里，永远不写 |
| `erase dtbo_b` | 有，且是第一步 | **没有** | 硬约束：除 `boot_b`/`linux` 外不做任何写与擦除；见 [4.5](#45-关于-dtbo_b为什么本教程不擦) |
| 刷入顺序 | 先 `flash linux`，再 `flash boot_b` | 先 `flash boot_b`，再 `flash linux` | 顺序不影响结果；先刷小的 `boot_b` 能更早发现设备问题 |
| 切槽 | `fastboot set_active b` | `fastboot --set-active=b` | 等价写法；本教程统一用选项形式 |
| 首次登录 | USB 网络 + `ssh root@172.16.42.1`（默认 `root/1234`） | 本地 tty 自动登录 `magma`，口令见 Release 说明 | 不需要额外网络配置；首登后必须改口令 |
| 根文件系统 | ext4 | f2fs | 本项目的选择，见 `docs/flash-guide.md` |
| 分区容量 | 示例写死 32.5GB→100% | **由操作者自己决定** | 不同机型容量差异大，写死会误导 |

---

## 附录 C：出问题时怎么反馈

提 issue 时附上这些（**贴之前把序列号、MAC、IP 打码**）：

```sh
/usr/bin/uname -a
/usr/bin/cat /proc/cmdline
/usr/bin/findmnt /
/usr/bin/df -h /
/usr/bin/sudo /usr/bin/dmesg
/usr/bin/sudo /usr/bin/journalctl -b --no-pager
/usr/bin/systemctl --failed --no-pager
```

分区相关问题再加：

```sh
/usr/bin/fastboot getvar all          # 含序列号，务必打码
/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"
```

按「期望 / 实际 / 复现步骤」三行写清楚，比贴一大段日志有用。

---

*本教程对应 Release `26/11/05`。资产名与步骤不随版本变化；容量、SHA256、版本号以 Release 页面为准。*
