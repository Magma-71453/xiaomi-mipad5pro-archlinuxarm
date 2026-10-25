# 刷入布局（flash-layout.md）

> 调研快照时间：2026-10-24。全部为只读抓取；**本文件不构成对设备的写操作**，
> 涉及的所有 fastboot 命令均**未执行**（宿主无 fastboot、无设备）。
> 证据分级：**已验证**（附 URL / 文件行号）｜**UNVERIFIED**（无一手证据，禁止当结论使用）。

## 0. 结论速览

- **内核/引导镜像 → boot_b**：已验证（Armbian 官方刷入指南与 pmOS wiki 都这么做）。
- **根文件系统 → linux 分区**：已验证（Armbian 指南 fastboot flash linux ...），
  但 **linux 不是出厂分区**：它是用户先用 parted 缩小 userdata 后 **手工新建** 的 GPT 分区。
- **boot_b = /dev/block/sde37：UNVERIFIED**。没有任何一手来源把 boot_b 钉到 sde37；
  可验证的是 GPT 分区名就是 boot_b，Linux 侧一律通过 /dev/disk/by-partlabel/boot_b（或 /dev/block/by-name/boot_b）访问。
- **dtbo 是否需要写/擦**：上游两套流程都要求**先 erase dtbo_b**，与本项目"禁止任何 erase、只写 boot_b+linux"的硬约束**直接冲突**
  → 见文末 **CONFLICT-2** 一节（已上报 dispatcher / 用户裁决）。

---

## 1. 来源 A：postmarketOS wiki（elish 页面）

- 页面 URL：https://wiki.postmarketos.org/wiki/Xiaomi_Mi_Pad_5_Pro_(xiaomi-elish)
- **抓取限制（重要，供复核者参考）**：该站点现已挂 Anubis 反爬。直接 GET 返回 HTTP 200 但正文是 JS 挑战页
  （"Making sure you're not a bot!"，7,475 B）；api.php 同样被拦；r.jina.ai 代理也只拿到挑战页。
  本轮改用 Wayback 存档正文：
  http://web.archive.org/web/20260419194842id_/https://wiki.postmarketos.org/wiki/Xiaomi_Mi_Pad_5_Pro_(xiaomi-elish)
  （快照 2026-04-19，98,674 B；正文 26,727 字符）。CDX 列表显示 2026-06-02 之后的快照只有约 3 KB 的拦截页
  （http://web.archive.org/cdx/search/cdx?url=wiki.postmarketos.org/wiki/Xiaomi_Mi_Pad_5_Pro_(xiaomi-elish)&output=json&collapse=timestamp:6），
  即 Anubis 是 2026-06 之后才上的。

wiki 给出的**三种** rootfs 刷法（原文顺序；三种的内核都写 boot_b）：

1. 刷到 userdata（会抹掉 Android 用户数据）：
       pmbootstrap flasher flash_rootfs --partition userdata
       pmbootstrap flasher flash_kernel --partition boot_b
       fastboot set_active b
2. 刷到 super（不抹 Android 数据，空间受限）：
       pmbootstrap flasher flash_rootfs --partition super
       pmbootstrap flasher flash_kernel --partition boot_b
       fastboot set_active b
3. 刷到 system_b（双系统）：
       fastboot set_active a
       fastboot reboot fastboot
       pmbootstrap flasher flash_rootfs --partition system_b
       pmbootstrap flasher flash_kernel --partition boot_b
       fastboot reboot bootloader
       fastboot set_active b

三种之前都有一句前置步骤（原文）：

> "Before installing rootfs, please erase dtbo_b (dtbo_a reserved for android)"

       fastboot erase dtbo_b

同页其它已验证事实：
- 进 bootloader：**Power + Volume Down**。
- 面板两变体，按 cat /proc/cmdline 的 msm_drm.dsi_display0 判断：
  BOE = qcom,mdss_dsi_k81_35_02_0b_dual_cphy_video；CSOT = qcom,mdss_dsi_k81_42_02_0a_dual_cphy_video。
- 页面里的示例 cmdline 含 androidboot.slot_suffix=_b（该机以 B 槽启动），与"内核写 boot_b"一致。
- **该页正文从不出现 linux 分区，也不出现 sde37**（正文 grep：仅 /dev/block/by-name/super 出现 2 次）。

## 2. 来源 B：Armbian 的 elish 刷入实践（决定性来源）

- wiki 页：https://github.com/amazingfate/armbian-xiaomi-elish/wiki/Flashing-Guide
- 原文 raw：https://raw.githubusercontent.com/wiki/amazingfate/armbian-xiaomi-elish/Flashing-Guide.md （5,957 B / 169 行）

### 2.1 linux 分区从哪来 —— 用户用 parted 新建（已验证）

原文步骤（命令原样摘录，未改写）：

    adb push parted /cache/
    adb shell "chmod 755 /cache/parted"
    adb shell
    cd /cache
    ./parted /dev/block/sda
    (parted) print
    # 示例输出：Number 34 / Start 2048MB / End 122GB / Name userdata
    (parted) resizepart 34         # 把 userdata 缩到 32GB（起点 2GB → 新大小 30G）
    (parted) mkpart esp fat32 32GB 32.5GB
    (parted) set 35 esp on
    (parted) mkpart linux ext4 32.5GB 100%
    (parted) quit

原文明确两句：
- "We are going to resize userdata partition of android and create two partitions for our armbian installation."
- "**This procedure will wipe all your Android data!!!**"

⇒ **linux 的 GPT 名称 = linux；大小 = 32.5GB → UFS 末尾（100%）**；由 parted 的 mkpart 创建
（或由 Armbian recovery 镜像 initramfs 内置的等效分区命令创建，原文："Latest armbian also provides a recovery image with partition command in its initramfs"）。
它**不是**出厂分区，也**不是**某个 repartition 脚本的产物 —— 是手工/交互式分区，需要人来执行。
同一步还会额外创建 **esp（fat32，35 号）**，本项目白名单里没有它，任何情况下都不应写入。

编号推导（标注 UNVERIFIED，仅作理解用）：按示例 userdata=34、esp=35，则 linux ≈ /dev/block/sda36；
这只在"分区表与示例完全一致"时成立，**禁止脚本硬编码块号**，必须用 /dev/disk/by-partlabel/linux。

### 2.2 刷入命令（原文顺序，已验证）

    # 1) 先擦 dtbo_b —— 原文："First erase dtbo_b"
    fastboot erase dtbo_b
    # 2) 根文件系统 → linux 分区
    fastboot flash linux Armbian_23.11.0-trunk_Xiaomi-elish_jammy_sm8250_6.5.9_gnome_desktop.rootfs.img
    # 3) 内核 → boot_b（按面板二选一）
    fastboot flash boot_b Armbian_..._boot_sm8250-xiaomi-elish-boe.img
    fastboot flash boot_b Armbian_..._boot_sm8250-xiaomi-elish-csot.img
    # 4) 切到 B 槽，让 ABL 从 boot_b 启动
    fastboot set_active b

⇒ **"内核写 boot_b、rootfs 写 linux"这一说法成立**：两条 fastboot 行与上述 raw 文件逐字对应。

### 2.3 运行期如何引用 boot_b（boot_b 的真正"身份"）

来源：https://raw.githubusercontent.com/armbian/build/main/packages/bsp/xiaomi-elish/zz-update-abl-kernel （2,005 B，内核 postinst 钩子）

- 从 /boot/armbianEnv.txt 读取变量 **abl_boot_partition_label**（值域形如 boot_a / boot_b），
  并以 slot_suffix= 加（去掉 boot 前缀后的 _a/_b）写进 cmdline；
- 真正落盘的那一行是：

      dd if=/boot/armbian-kernel-<panel>.img of=/dev/disk/by-partlabel/<abl_boot_partition_label>

- 板级配置 https://raw.githubusercontent.com/armbian/build/main/config/boards/xiaomi-elish.conf 声明：
  BOARDFAMILY="sm8250"、BOOTCONFIG="none"、IMAGE_PARTITION_TABLE="gpt"、
  ABL_DTB_LIST=("sm8250-xiaomi-elish-boe" "sm8250-xiaomi-elish-csot")
  —— 即 Armbian 明确按"ABL 直接启动 Linux（无 U-Boot）"设计，boot 分区靠 **GPT 名字** 定位，不靠块号。

同类机旁证（Xiaomi Pad 6 / pipa，同 SM8250 家族）：
https://raw.githubusercontent.com/TheMojoMan/xiaomi-pipa/main/pipa-multiboot.md
备份/写入 boot 分区一律用 /dev/disk/by-partlabel/boot_a|boot_b（第 55–62 行）或 /dev/block/by-name/boot_a|boot_b（第 69–76 行），
fastboot 侧用 fastboot flash boot_a|boot_b（第 84–87 行）。**同样没有任何 sde 编号。**

### 2.4 boot_b = /dev/block/sde37 的判定

- **UNVERIFIED**。已检索的一手来源（pmOS wiki 2026-04 正文、Armbian Flashing-Guide、armbian/build 的 elish 板级配置与 postinst 脚本、
  pipa 多系统文档）**全部不出现 sde37**；仓库 deviceinfo 也只声明 deviceinfo_flash_method="fastboot"（按名字刷）。
- 要拿到一手证据只有两条路（本轮都不具备）：
  1. 真机上执行 ls -l /dev/block/by-name/boot_b 与 ls -l /dev/disk/by-partlabel/boot_b；
  2. 解析 Xiaomi 官方 fastboot 包里的 gpt_main*.bin / rawprogram*.xml（记录每个分区所在 LUN 与序号）。
- **工程结论（不依赖 sde37 是否成立）**：脚本一律用**名字**（fastboot 分区名 boot_b；Linux 侧 /dev/disk/by-partlabel/boot_b），
  **绝不硬编码 /dev/block/sdeNN**；若将来确需块号，先在真机读 by-name 链再落常量，并在 bug.md 留痕。

---

## 3. 可操作的 fastboot 序列（只写 boot_b 与 linux）

前置（一次性、**由人决定并执行**的分区准备，见 §2.1）：linux 分区必须先存在，否则 fastboot flash linux 会直接报分区不存在。
**本项目脚本不得执行 §2.1 的任何步骤**（会改 userdata，属白名单外分区），只做前置检查。

脚本应实现的分区白名单（硬编码；白名单外一律拒绝）：

    ALLOWED_FLASH=(boot_b linux)
    FORBIDDEN_SUBSTR=(erase dtbo_a dtbo_b system_b vendor_boot userdata super misc boot_a esp super_empty)

建议刷入序列（默认 dry-run；真机由人确认后逐条执行）：

    # 0) 只读自检
    fastboot devices
    fastboot getvar current-slot            # 期望 a（Android 在 a 槽）→ b 槽空闲
    fastboot getvar partition-size:boot_b  # 只读，确认 boot_b 存在
    fastboot getvar partition-size:linux   # 若报错 → linux 分区未创建：停止，交回用户

    # 1) 内核/引导镜像 → boot_b（本项目 boot.img 内含两个 elish dtb）
    fastboot flash boot_b output/20261024/kernel/boot.img

    # 2) 根文件系统 → linux
    fastboot flash linux output/20261024/rootfs/rootfs.img

    # 3) 只改槽位标记，不写数据分区
    fastboot set_active b

    # 4) 重启
    fastboot reboot

脚本必须**拒绝**的动作：fastboot erase 任意分区、fastboot -w、fastboot format、
fastboot flash 到 boot_a / dtbo_a / dtbo_b / system_b / userdata / super / vendor_boot / misc / esp。

---

## 4. 引导镜像侧的相关事实（deviceinfo，已验证）

来源：https://gitlab.postmarketos.org/api/v4/projects/226/repository/files/device%2Ftesting%2Fdevice-xiaomi-elish%2Fdeviceinfo/raw?ref=main

| 字段 | 值 | 与刷入的关系 |
|------|-----|--------------|
| deviceinfo_flash_method | fastboot | 按 GPT 名刷，不涉及块号 |
| deviceinfo_append_dtb | true | dtb 追加进 boot.img（不是写 dtbo 分区）→ 这是"不写 dtbo 也有可能启动"的技术前提 |
| deviceinfo_flash_pagesize | 4096 | boot.img 页大小 |
| deviceinfo_flash_offset_base / kernel / ramdisk / second / tags | 0x0 / 0x8000 / 0x01000000 / 0x00f00000 / 0x100 | mkbootimg 参数，与 Armbian 的 zz-update-abl-kernel 完全一致（该脚本用 --base 0x0 --second_offset 0x00f00000 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 --pagesize 4096） |
| deviceinfo_rootfs_image_sector_size | 4096 | rootfs 镜像按 4096 对齐（bug.md B-005） |
| deviceinfo_super_partitions | /dev/sda29 /dev/sda29 | 仅 pmOS 的 super 流程用；本流程不触碰（bug.md B-004） |

---

## 5. 风险点清单（所有可能触碰白名单外分区的路径）

| 编号 | 风险 | 证据 | 影响 | 处置 |
|------|------|------|------|------|
| R1 | **erase dtbo_b 是上游两套流程共同的前置步骤** | pmOS wiki 原文；Armbian 指南 "First erase dtbo_b" | 与"禁止 erase"硬约束冲突；不擦的后果未确证（UNVERIFIED，见 CONFLICT-2） | 上报 dispatcher / 用户裁决 |
| R2 | linux 分区不存在（未做 §2.1 准备） | Armbian 指南：分区手建 | 刷入直接失败，不会误写别处 | 脚本先 getvar partition-size:linux 探测，失败即停 |
| R3 | pmbootstrap flasher 会写 super / system_b / userdata | pmOS wiki 三种方法 | 违反硬约束 | 不采用 pmbootstrap 刷入流程（与 D-002 一致） |
| R4 | fastboot set_active b 改写槽位标记 | wiki 与 Armbian 都要求 | 不改数据分区，但改变默认启动槽 | 保留为必要步骤；失败时不要用变体重试 |
| R5 | Armbian 流程额外创建 esp（fat32，35 号） | Flashing-Guide 原文 | 误写会破坏 Boot | 白名单不含 esp；文档明确"不要写 esp" |
| R6 | 镜像大于目标分区 | fastboot 行为 | 失败，不误写 | 打包阶段校验 rootfs.img 尺寸 < linux 分区尺寸（用户自定） |
| R7 | 硬编码 /dev/block/sdeNN | sde37 无一手证据 | 可能 dd 到错误分区（最危险） | 一律用 GPT 名 / by-partlabel；脚本内禁止出现 /dev/block/sd 字面量 |
| R8 | 面板变体选错（BOE vs CSOT） | wiki 两种 msm_drm.dsi_display0 | 黑屏，但不涉及分区 | boot.img 同时带两个 dtb（append_dtb=true），或提供两个 boot.img |

---

## 6. CONFLICT-2：erase dtbo_b —— 硬约束 vs 上游流程

**本项目硬约束（README / 任务书）**：只允许写 boot_b 与 linux 两个分区；**任何 erase 一律禁止**。

**上游要求（两处，均为一手原文）**：

1. postmarketOS wiki（elish 页面，Wayback 快照 2026-04-19 正文）：
   > "Before installing rootfs, please erase dtbo_b (dtbo_a reserved for android)"
   紧随其后就是命令 fastboot erase dtbo_b。
   URL：http://web.archive.org/web/20260419194842id_/https://wiki.postmarketos.org/wiki/Xiaomi_Mi_Pad_5_Pro_(xiaomi-elish)
   （在线页 https://wiki.postmarketos.org/wiki/Xiaomi_Mi_Pad_5_Pro_(xiaomi-elish)，现被 Anubis 拦截）
2. Armbian Flashing-Guide（raw 原文），"## Flash images" 段第一句：
   > "First erase dtbo_b:"      （原文 dtbo_b 两侧带 Markdown 反引号）
   紧接命令 fastboot erase dtbo_b。
   URL：https://raw.githubusercontent.com/wiki/amazingfate/armbian-xiaomi-elish/Flashing-Guide.md

**不 erase dtbo_b 时的可能后果（逐条标注证据级别）**：

- 机理推测（**UNVERIFIED**）：ABL 启动 slot b 时会读取 dtbo_b 并把其中的 overlay/DFT 应用到内核 dtb。
  我们自带的 dtb 是**追加在 boot.img 里**的（deviceinfo_append_dtb="true"，见 §4）；若 ABL 仍叠加 dtbo_b 的 Android overlay，
  可能与我们的 dtb 冲突（面板/时钟节点被改），表现为黑屏或早期 hang。
- pmOS 原文给出的唯一理由是 "dtbo_a reserved for android"：即只保留 a 槽 dtbo 给 Android，把 b 槽 dtbo 让给 Linux。
  这暗示"擦掉 b 槽 dtbo"的目的是**避免 Android 的 overlay 生效**，而不是"必须写一份 Linux 专用 dtbo"。
- 没有任何一手来源说明"不擦就一定失败"，也没有说明"擦掉就一定成功"——两侧都缺真机证据。
- 备选方案（需用户决策，本子代理不执行）：
  (a) 严格遵守硬约束：不擦 dtbo_b，真机验证能否启动；失败则把现象写入 bug.md；
  (b) 由用户书面放宽硬约束，允许**仅** erase dtbo_b 一次（仍不触碰其它分区）；
  (c) 若最终必须写入 dtbo，属**违反本项目硬约束**，必须回到 dispatcher 重新裁决（本文件不提供该命令）。

**已落定的裁决（用户 2026-10-24）**：采纳方案 (b) 的受限版本 = `docs/decisions.md` **D-009** "dtbo_b 一次性 erase 例外"：
仅允许对 `dtbo_b` 执行一次 erase，且必须 (i) 由显式开关 `--allow-dtbo-erase` 触发（默认永不执行）；(ii) 执行前二次确认并打印完整命令；
(iii) 结果写入 `output/<date>/FLASH-LOG` 与 `bug.md`；(iv) 除 `boot_b`/`linux` 外任何 `flash`、除 `dtbo_b` 外任何 `erase` 仍一律拒绝。
本文件的分区白名单与风险清单据此保持：**erase 只有 `dtbo_b` 一个例外，其余不变。**

---

## 7. UNVERIFIED 清单（flash-layout.md）

- U1 boot_b 的块设备号是否为 /dev/block/sde37 —— 无一手证据（§2.4）。
- U2 linux 分区对应的 /dev/block/sda36（或其它编号）—— 仅由 Armbian 示例编号推导。
- U3 不 erase dtbo_b 是否必然导致启动失败，以及 ABL 是否真会把 dtbo_b 的 overlay 叠加到 boot.img 内的 dtb 上。
- U4 fastboot set_active b 在 elish 上是否总能成功（部分机型 ABL 有限制）。
- U5 linux 分区的 GPT 类型 GUID 与文件系统是否必须 ext4 —— Armbian 示例为 mkpart linux ext4，其它类型未验证。
