# bug.md — 缺陷与已知问题追踪中心

格式：`[状态] 现象 / 触发条件 / 根因（若已知）/ 临时规避 / 计划修复阶段`
状态取值：`开放` / `阻塞` / `规避中` / `已修复` / `已验证`
本文件是唯一追踪中心。产物溯源见 `output/<date>/MANIFEST`。

---

## 一、内核与版本选择

### B-001 设备在 postmarketOS 26.06 周期被从 community 降级为 testing
- **[已知-非硬件回归]** 现象：`device-xiaomi-elish` 于 2026-05-03 由 `device/community/` 移入 `device/testing/`。 /
  触发条件：pmaports 维护策略审查（MR !8398）。 / 根因：提交信息原文为
  "The kernel package used by this device does not meet the community kernel version requirements
  anymore and the maintainer has not responded to the issue #4448" —— 即**内核版本时效性**不满足
  community 的版本要求 + 维护者失联，**不是** elish 硬件/驱动回归。 /
  临时规避：无需回退内核版本。 / 计划修复阶段：S1（记录于决策 D-001）。
- **结论依据**：`gitlab.postmarketos.org/postmarketOS/pmaports` commit `c66aa62e`（2026-05-03）。

### B-002 内核版本选择
- **[开放]** 现象：需选定 `linux-postmarketos-qcom-sm8250` 的构建版本。 / 触发条件：内核源码有多条 tag
  （7.2.0_git20261010 / 7.2.0 / 7.1.0 / 6.17.0 / 6.16.0 / 6.13.0）。 /
  根因：N/A。 / 临时规避：按"优先最新版"规则取 `sm8250-7.2.0_git20261010`，并把 B-001 的真实原因写入决策记录；
  若无 B-001 所述的硬件回归证据，则不回退。 / 计划修复阶段：S1。

### B-003 内核构建工具链已切换为 LLVM
- **[风险]** 现象：pmaports 于 2025-11-03 起将该内核包切换为 LLVM 工具链（clang / lld / pahole / elfutils）。 /
  触发条件：构建内核时。 / 根因：上游主线内核对 LLVM 的支持已成熟，pmOS 统一工具链。 /
  临时规避：使用宿主 clang 23.1.3 + lld 23.1.3 + pahole，`make LLVM=1 ARCH=arm64`。 / 计划修复阶段：S1。

## 二、刷入与分区（硬约束相关）

### B-004 super 动态分区流程被本项目禁止
- **[阻塞-流程]** 现象：上游 `deviceinfo` 声明 `deviceinfo_super_partitions="/dev/sda29 /dev/sda29"`，
  常规 pmOS 安装流程会向 super（system/vendor）分区写入逻辑分区镜像。 /
  触发条件：任何沿用 pmOS 安装脚本的路径。 / 根因：pmOS 依赖 Android 动态分区；本项目硬性禁止。 /
  临时规避：**不走 pmOS 安装流程**。根文件系统放在独立的 `linux` 分区，内核/引导镜像写入 `boot_b`，
  由自建 initramfs 按分区标签探测根分区。 / 计划修复阶段：S2（若某步骤仍生成需写入其它分区的产物，必须先记录并改造该步骤）。
  例外（用户于 2026-10-24 显式授权）：仅允许 **一次性 `fastboot erase dtbo_b`**，用于清除 Android 遗留的 dtbo overlay；
  必须由显式开关 `--allow-dtbo-erase` 触发，脚本默认永不执行，且该操作强制写入审计日志。见 D-009。

### B-005 rootfs 镜像扇区大小必须 4096
- **[已裁决 → f2fs，见 B-014 / D-010]** 现象：设备使用 UFS 存储，`deviceinfo_rootfs_image_sector_size="4096"`。 /
  触发条件：制作 `linux` 分区的 ext4 镜像时。 / 根因：UFS 逻辑扇区 4096，镜像/文件系统块大小不匹配会导致挂载失败。 /
  临时规避：`mke2fs -b 4096`，镜像按 4096 对齐。 / 计划修复阶段：S1（rootfs 打包）。

### B-006 面板有两种变体（BOE / CSOT）
- **[已知]** 现象：dtb 分 `qcom/sm8250-xiaomi-elish-boe` 与 `-csot` 两支。 /
  触发条件：设备实际屏幕供应商不同时。 / 根因：两种面板的显示链路参数不同。 /
  临时规避：boot 镜像内同时携带两个 dtb（`deviceinfo_append_dtb="true"`），
  必要时提供两个 boot 镜像由用户选择。 / 计划修复阶段：S1。

## 三、桌面与用户态

### B-007 MangoWM 的 wlroots / scenefx 版本约束
- **[已裁决 → 见 B-012 / D-008]** 现象：`mangowm-git`（AUR）要求 wlroots 0.19.2 与 scenefx 0.4.1。 /
  触发条件：在 Arch Linux ARM 上构建 MangoWM 时；Arch 滚动仓库中的 wlroots 版本可能已高于约束。 /
  根因：上游锁定 API 版本。 / 临时规避：待核实 ALARM 仓库当前版本；若已超出约束，改用 AUR 的固定版本依赖
  或自行构建 wlroots 0.19.2/scenefx 0.4.1。 / 计划修复阶段：S3。

### B-008 FEX-Emu 在 aarch64 上需自建
- **[风险]** 现象：Arch Linux ARM 无官方 FEX-Emu 二进制包，需从 AUR 源码构建（大型 C++ 项目）。 /
  触发条件：启用 x86 兼容层时。 / 根因：AUR 仅提供源码构建；aarch64 上编译耗时长。 /
  临时规避：先在第一阶段不纳入 rootfs，保留 `perf/` 与 `docs` 中的集成方案；在设备首次开机后用 AUR 构建。 /
  计划修复阶段：S4。

## 四、构建环境（本机会话）

### B-009 主机无 root、无 pmbootstrap、无设备连接
- **[阻塞-环境]** 现象：`sudo` 被 `no_new_privs` 阻止；宿主未安装 pmbootstrap / fastboot / adb；未连接平板。 /
  触发条件：任何需要 root（chroot/mount/mkfs 挂载）或需要向设备写入的步骤。 / 根因：当前会话沙箱策略。 /
  临时规避：无 root 路线——交叉编译内核；`mke2fs -d` 直接由目录生成 ext4 镜像（无需 mount）；
  用 `qemu-user-static` 显式调用 aarch64 二进制生成 ldcache；**刷入与真机验证记录为待硬件执行**。 /
  计划修复阶段：S2（构建链路）；真机验证标注为"待设备"。

### B-010 `bc` / `dtc` / `rsync` / `pigz` 缺失
- **[开放]** 现象：宿主缺少 bc、dtc、rsync、pigz。 / 触发条件：内核构建与镜像打包。 /
  根因：宿主最小化安装。 / 临时规避：从 Arch 官方镜像下载包并解包到 `../.build/tools/`（非 root 用户级安装），
  通过 `PATH` 注入。 / 计划修复阶段：S1。

---

## 变更记录

| 日期 | 变更 |
|------|------|
| 2026-10-24 | 初始化：写入 B-001..B-010（依据 pmaports main c7940da、设备包 commit 历史、本机环境探测） |

---

## 五、用户裁决记录（2026-10-24）

### B-012 MangoWM 依赖版本路线（已裁决）
- **[已裁决]** 现象：任务书要求 wlroots 0.19.2 + scenefx 0.4.1，但 MangoWM 自提交 `0fc7559c3c`
  （2026-06-19 "version: chase wlroots 0.20"）起改用 wlroots>=0.20 / scenefx>=0.5，旧约束只对应 <= 0.14.4。 /
  触发条件：S3 构建 MangoWM。 / 根因：上游 API 迁移；ALARM 仓库只有 wlroots0.20 0.20.2-1 与 scenefx 0.5-1，
  没有 0.19.x 的 wlroots 包（AUR wlroots0.19 已 orphan、scenefx0.4 已 OOD）。 /
  处置：**采用 MangoWM 0.18.3 + ALARM 现成 wlroots0.20 / scenefx 0.5**（用户裁决，零自建）；
  偏离任务书字面版本约束的理由与证据记入 D-008。 / 计划修复阶段：S3。

### B-013 dtbo overlay 与启动风险（已裁决为例外操作）
- **[已裁决]** 现象：Armbian Flashing-Guide 与 pmOS wiki 的 elish 刷入步骤均要求先 `fastboot erase dtbo_b`，
  否则 ABL 可能把 Android 遗留 dtbo overlay 叠加到自带 dtb 上，导致面板/UFS 配置不匹配而起不来。 /
  触发条件：真机首次刷入。 / 根因：小米 ABL 对 dtbo 分区的套用行为。 /
  处置：用户授权**一次性** `erase dtbo_b` 例外（D-009），其余分区写/擦除仍然绝对禁止；脚本默认永不执行该操作。 /
  计划修复阶段：S2/S5（真机阶段，PENDING-DEVICE）。

---

## 六、2026-10-24 用户指示引起的变更

### B-014 linux 分区改用 f2fs（含内核配置偏差）
- **[已裁决]** 现象：原先按 ext4 规划 rootfs，用户指定改用 f2fs。 / 触发条件：制作 linux 分区镜像。 /
  根因：用户对日常桌面场景的文件系统选择。 / 处置：内核 config 追加 `CONFIG_F2FS_FS=y`（上游为 =m），
  镜像用 `mkfs.f2fs` + `sload.f2fs` 目录填充；compression 特性不启用
  （内核未开 `CONFIG_F2FS_FS_COMPRESSION`）。 / 计划修复阶段：S1（内核 / rootfs）。
  **勘误 (2026-10-24)**: mkfs.f2fs 的 −s 不是扇区大小（是 segments per section）；正确做法见 D-010 勘误：
  工具原生镜像 + 把两个冗余超级块的 log_sectorsize/log_sectors_per_block 改为 12/0，并用 cmp -l 证明仅 4 字节差异
  （安全前提：默认 superblock features = 0，未启用 sb_checksum）；回退开关 --f2fs-sector-size 512。

### B-015 gamemod-elish 类方案的局限（不采用为核心调度器）
- **[已裁决]** 现象：`gamemod-elish-src` 是 gamemodrun 式工具——跑 Steam 游戏时切 `performance` 调度器
  并把 `steamwebhelper` 绑小核。 / 触发条件：作为系统核心调度方案时。 / 根因：全局 governor 覆盖会与
  schedutil/EAS 决策冲突；单一"绑小核"启发式缺乏实测收益；对交互延迟关键路径（FEX JIT、合成器线程）无针对性。 /
  处置：不采用为核心调度器（D-011），改为 `perf/schedplace`（进程/线程名 -> cpuset/affinity，规则化、可观测、
  不锁频不停核），仅借鉴其进程识别与配置解析思路。 / 计划修复阶段：S4。

### B-016 linux 分区来源与位置（已核实）
- **[已核实]** 现象：Armbian 的 elish Flashing-Guide 中 `linux` 分区是**手工创建**的
  （parted 缩减 userdata -> mkpart linux），并非出厂 GPT 自带；本项目脚本不得触碰 userdata。 /
  触发条件：真机首次刷入且 `/dev/disk/by-partlabel/linux` 不存在时。 / 根因：出厂分区表无 linux 分区。 /
  临时规避：刷机指南把该步骤列为**用户本人执行**的前置条件（给命令与风险说明），本仓库任何脚本都不执行它。 /
  计划修复阶段：S2（文档）/ S5（真机确认）。

### B-017 FEX-Emu 无现成 aarch64 二进制
- **[已确认-延后]** 现象：AUR 无 `fex-emu`（`fex-emu-wine-git` 仅为 Wine ARM64EC DLL）；
  FEX 官方 Release 无二进制资产，一键安装只覆盖 Ubuntu PPA。 / 触发条件：引入 x86 兼容层时。 /
  根因：上游分发策略。 / 临时规避：S1 不含 FEX；首次开机后在设备上源码构建（页大小/Proton 兼容矩阵待实测）。 /
  计划修复阶段：S5。

---

## 七、S1 收口状态刷新（2026-10-24）

各条目当前状态一览（详细证据见对应决策与 findings；真机项一律为 PENDING-DEVICE）：

| 编号 | 主题 | 当前状态 | 处置/证据 |
|---|---|---|---|
| B-001 | 26.06 降级为 testing | 已确认·非硬件回归 | 维护策略问题；旧系统实跑 7.2.0-xiaomi-elish+（D-013）佐证无需降版 |
| B-002 | 内核版本选择 | 已解决 | 取 7.2.0_git20261010；产物 Image.gz 15,489,546B + 两个 dtb（S1-a） |
| B-003 | LLVM 工具链 | 已验证 | clang/lld 23.1.3 + pahole；make LLVM=1 ARCH=arm64，578s，warning=3（均为 fork 自带驱动） |
| B-004 | super 分区流程被禁 | 已解决·流程 | 不走 pmOS 安装流程；根文件系统放 linux 分区 + 自建 initramfs 按 PARTLABEL 挂载 |
| B-005 | rootfs 扇区 4096 | 已解决 | f2fs -b 4096 + 两个冗余超级块定点改 12/0；cmp -l 证明仅 4 字节差异（D-010 勘误） |
| B-006 | BOE / CSOT 双面板 | 已解决 | 两个 dtb 均构建；实测本机为 CSOT（D-013），boot.img 默认 csot + boe 备选 |
| B-007 | MangoWM 依赖版本 | 已解决 | 采用 0.18.3 + wlroots-0.20 0.20.2 / scenefx-0.5 0.5.0（D-008，实测 pkg-config 解析） |
| B-008 | FEX-Emu 无现成包 | 已确认·延后 S5 | AUR 无 fex-emu、官方无预编译资产（B-017） |
| B-009 | 无 root 构建环境 | 已缓解 | 无 root 路线跑通：内核交叉编译 + sload.f2fs 组镜像 + qemu-user-static 生成 ldcache |
| B-010 | 宿主缺 bc/dtc 等 | 已解决 | 从官方镜像解包到 .build/tools（用户级，未系统安装） |
| B-012 | MangoWM 版本路线 | 已裁决·已构建 | 0.18.3 在设备原生编译成功（S3-b），NEEDED 含 libwlroots-0.20.so / libscenefx-0.5.so |
| B-013 | dtbo overlay 风险 | 已裁决·待真机 | D-009 一次性 erase dtbo_b，脚本三重门控；真机执行前需用户在场（D-016） |
| B-014 | f2fs 化 + 内核偏差 | 已解决（勘误后） | CONFIG_F2FS_FS=y 片段；compression 未启用；扇区参数按 D-010 勘误实现 |
| B-015 | gamemod 类方案局限 | 已裁决·已替代 | perf/schedplace 重写并 375 checks 通过；旧行为仅作默认关闭 profile |
| B-016 | linux 分区来源 | 已核实·已关闭 | /dev/sda36，≈195.5 GiB，已存在（D-013）；脚本零触碰 userdata |
| B-017 | FEX 无 aarch64 二进制 | 已确认·延后 | 首次开机后设备上源码构建 |

### 新增（本轮发现）

### B-018 f2fs 镜像的 SIT 修复未持久化
- **[开放-返工中]** 现象：把交付镜像复制一份跑 `fsck.f2fs -f` 会出现 `[FSCK] fixing SIT types`；同一副本再跑第二/三次则 0 次修复（收敛），
  但**从交付镜像重新复制又会复现**。 / 触发条件：`sload.f2fs` 填充后未执行 fsck 落盘。 / 根因：工具产物 SIT 类型需由 fsck 归一。
- 处置：顺序改为 sload -> fsck -f（落盘）-> 再打 4 字节扇区补丁 -> 校验；验收要求"复制交付镜像后一次 fsck 的 fixing 行数为 0"。 / 计划修复阶段：S1 返工。

### B-019 设备固件未打入 rootfs
- **[开放-返工中]** 现象：首版镜像未包含 `/lib/firmware` 下的 elish 固件（227 文件 / 35 MB 已就绪）。 / 触发条件：freedreno GPU 初始化、Wi-Fi/BT、DSP。
- 根因：构建未传 `--firmware`。 / 处置：用固件重跑镜像并在 findings 记录来源与三类固件到位情况。 / 计划修复阶段：S1 返工。

### B-020 hexagonrpcd 在 ALARM 无包
- **[开放-低频]** 现象：ALARM 仓库没有 `hexagonrpcd`（pmOS 用于与 ADSP 通信）。 / 触发条件：音频/DSP 相关功能。
- 处置：记录为已知缺失，首启后评估替代（自建包或放弃音频录制类功能）。 / 计划修复阶段：S5。

### B-021 首启后需扩容根分区
- **[开放-已文档化]** 现象：镜像 16 GiB，而 linux 分区约 195.5 GiB。 / 触发条件：首次开机后。
- 处置：在未挂载环境执行 `resize.f2fs /dev/disk/by-partlabel/linux`（用法见 docs/findings/rootfs.md §7.4 与刷机指南）；脚本不自动执行。 / 计划修复阶段：S5 真机。

---

## 八、S1 返工复验（2026-10-24，dispatcher 独立执行）

| 编号 | 原状态 | 复验结论 | 证据 |
|---|---|---|---|
| B-018 | 开放-返工中 | **已修复** | 交付镜像副本上连跑两次 `fsck.f2fs -f`：`rc=0`、`[Fail]` 行 = 0、`[Ok]` = 10；`cmp -l` 交付镜像 vs fsck 副本 = **恰好 4 行**（1033/1037/5129/5133，仅两个超级块的扇区字）。根因也查清：旧版在 fsck 之后又对交付镜像跑 `fsck -l`/`dump.f2fs`，这两个工具会改写超级块（D-017）。附带澄清：`fixing SIT types` 在 -f 模式下两次都打印 1 次，不是腐蚀信号，不能当判据 |
| B-019 | 开放-返工中 | **已修复** | tar 内 `/usr/lib/firmware` 共 6,937 文件；GPU `qcom/a650_gmu.bin`、`qcom/a650_sqe.fw`、`qcom/sm8250/xiaomi/elish/a650_zap.mbn` 到位；Wi-Fi `ath11k/WCN6855/hw2.0/*`、BT `qca/htbtfw20.tlv` 到位；另把 ALARM 底包**缺失**的 `linux-firmware-qcom 20260916-1` 补入包清单（491 成员）。**勘误：elish 是 Adreno 650（SM8250），不存在 a690 固件（a690 属 SM8450）** |
| B-020 | 开放-低频 | 仍开放（已定级） | 缺 `hexagonrpcd` → 扬声器/耳机无声、录音不可用、自动旋转/亮度不可用；GPU/显示/触控/相机不受影响 |
| B-021 | 开放-已文档化 | 仍开放（真机项） | 首启后 `resize.f2fs`；镜像内已有 `mkfs.f2fs/fsck.f2fs/resize.f2fs`（tar 实测 3 个二进制） |

### 单次验收快照（全部由 dispatcher 亲手执行）
- `sha256sum -c SHA256SUMS`（output/20261024）：**11 条全部"成功"**；consolidated `MANIFEST` 11 条目，含目标分区 boot_b / linux。
- `make-bootimg.py --verify`：`kernel_size=15,628,099`、`cmdline = quiet rootwait root=PARTLABEL=linux console=tty0 loglevel=4`、`verify.status=OK`。
- `bash tests/run.sh`：**5 步 0 失败，ALL TESTS PASSED**。
- `bash scripts/audit-hygiene.sh`：**0 failure / 2 warning**（warning 为"尚未发生 D-009 erase"与"MANIFEST 覆盖提示"，属预期）。
- 镜像内容抽检：tar 81,362 条目；`/usr/lib/modules/7.2.0/` 810 条、无 7.1.x 残留；`/usr/bin/{mango,mmsg,mangonag}` 齐；`/boot` 为空（内核走 boot_b）。

### B-022 ABL 拒绝装载 boot.img（Load Error）——dtb 填充导致
- **[已修复]** 现象：fastboot boot 报 'Failed to load/authenticate boot image: Load Error'；刷入 boot_b 后重启仍回 fastboot，slot-retry-count:b 保持 6 不递减。 / 触发条件：内核段 = Image.gz + 2048 字节对齐 + dtb。 / 根因：ABL 要求 dtb 紧跟内核负载，任何填充都会让装载失败。 / 修复：改为与 boot-deploy 一致的 cat Image.gz dtb（零填充）；实测后设备立刻进入内核（面板点亮、f2fs 挂载成功）。 / 证据：decisions.md D-018。 / 计划修复阶段：已完成。

### B-023 PID 1 崩溃（kernel panic: Attempted to kill init! exitcode=0x0000000b）
- **[修复中]** 现象：内核与 initramfs 都正常（f2fs 挂载成功、switch_root 执行），随后 PID 1 死于 SIGSEGV(11)，内核 panic。 / 触发条件：switch_root 到 rootfs 的 systemd 261.2-1（glibc 2.43）。 / 根因（候选，已按顺序处理）：根分区以 ro 挂载且从未 remount rw；/dev /proc /sys 未搬移进新根。 / 修复：init 里加 remount rw + mount --move + /dev/console 兜底 + switch_root 前直接 chroot newroot systemd --version 打印诊断行。 / 复核：宿主 binfmt 下 chroot /tmp/rf systemd --version 正常（systemd 261 / glibc 2.43），说明二进制本身没问题，问题在 PID1 运行环境。 / 计划修复阶段：S1 真机迭代。

### B-023 更新：已修复（2026-10-24 真机）
- **[已修复]** 用户现场确认：修过的 init 镜像启动后 **停在 tty 登录界面**（此前是 PID 1 SIGSEGV panic）。
  证据链：ABL Load Error 消失 → 内核启动 → 面板点亮 → f2fs 挂载 → switch_root → **systemd 起来并拉起 getty**。
  修复项即 D-018 的三条：dtb 零填充、remount rw、mount --move /dev /proc /sys。

### B-024 能启动但登不进去、也没有桌面会话
- **[开放-进行中]** 现象：tty 停在登录提示，但 `magma` 用户没有设置密码（root 被锁），因此无法登录；同时没有任何 display-manager/session 会自动拉起 MangoWM。
  / 触发条件：首次开机。 / 根因：rootfs 组装时只建了用户与组，没设密码，也没配置自动登录与 mango 会话启动器。
  / 处置：rootfs 重做——给 magma 设密码、tty1 自动登录、加 mango-session 启动器（seatd + XDG_RUNTIME_DIR + exec mango）、启用 NetworkManager 以便配置 Wi-Fi；重出镜像并重刷。
  / 计划修复阶段：S1 真机迭代（本轮）。

### B-025 刷入后大文件数据与镜像不一致（当前唯一阻塞）
- **[开放-调查中]** 现象：设备上 `md5sum` 读到的 `libcrypto.so.3`(6.5MB)=`079c675d…`、`libpython3.14.so.1.0`= `13dae948…`，
  而同一份镜像内为 `aa8b062b…` / `f5927fa4…`；相反 `bash`(1.3MB)、`mango`(3.3MB) 完全一致。
  / 触发条件：用 `fastboot flash linux` 刷 **sparse**（img2simg 产物）。 / 根因：待定（sparse 分块写入 或 sload 阶段）。
  / 已排除：`img2simg`↔`simg2img` 往返 md5 完全一致；fsck 通过；f2fs 挂载与小文件读取正常。
  / 后果：systemd/python3/openssl/pacman/kmod 全部 SIGSEGV（依赖这些大 .so），系统无法进入用户态。
  / 下一步：raw 直刷同一镜像后复测 md5；若仍不一致则改用 loop 挂载（需 root）生成镜像。
  / 计划修复阶段：S1 真机迭代（当前）。

### B-026 根因确认：`sload.f2fs` 会写坏部分大文件（镜像内容错，不是刷新问题）
- **[已定位-待修]** 证据链：
  1. 设备上 `systemd`/`python3`/`openssl` 一执行就 SIGSEGV，`bash`/`ls`/`cat` 正常；
  2. core dump 显示崩在 **ld.so 的 `__tunable_get_val`**（`si_addr=0x8`，空指针），即加载期而非程序自身；
  3. `dump.f2fs -r` 直接从镜像导出文件树后与 tar/base 逐字节比对：
     `usr/lib/systemd/libsystemd-shared-261.2-1.so` 在镜像里 **大小相同（6,566,056 B）但 md5 不同**
     （tar/base = `5549bd1b…`，镜像 = `964df557…`），且 `readelf -d` 报 **no .dynamic section in the dynamic segment**，
     动态段条目从 31 变为 1 —— ELF 的 .dynamic 被摧毁，加载器必然崩；`libcrypto.so.3`、`libpython3.14.so.1.0` 同样被写坏；
     `bash`、`mango`、`md5sum` 等小文件完好。
  4. `sload.f2fs` 的最小复现（6.5MB/3MB/100KB）往返完全正确，说明是**大镜像/多文件布局下**才触发的写入缺陷。
- 影响：只要镜像由 `sload.f2fs` 生成，就有部分大 .so 内容损坏 → 用户态随机 SIGSEGV。与刷入方式（sparse/raw）无关。
- 修复方案：改用 **loop 挂载 + tar 解包** 生成镜像（不经过 sload），脚本 `scripts/make-f2fs-image-root.sh`（需 root）。
- 验证方式：新镜像用 `dump.f2fs -r` 导出后与 tar 逐文件比对 md5，必须全等；再 flash 到设备复测 systemd。

## 九、真机达成：MangoWM 已运行（2026-10-24 深夜）

| 编号 | 主题 | 状态 | 证据 / 处置 |
|---|---|---|---|
| B-023 | PID1 panic | 已修复 | remount rw + mount --move /dev /proc /sys（D-018） |
| B-024 | 能启动但登不进去、没有桌面 | 已修复 | 见下方 B-027/B-028；现在 tty1 自动登录后直接进入 MangoWM |
| B-025 | 「刷入后大文件损坏」 | 结论撤销 | 镜像==设备，差异来自旧 tar；见 B-026 |
| B-026 | sload.f2fs 写坏大文件 | 已修复 | 改用 loop 挂载 + tar 解包生成镜像（scripts/make-f2fs-image-root.sh） |
| B-027 | 自动登录不生效 | 已修复 | tar 由普通用户打包 → /etc 属主 uid 1000 → **systemd 忽略非 root 拥有的 unit/drop-in**；镜像构建时 `chown -R 0:0` 并恢复 setuid 位 |
| B-028 | mango 退出 rc=1（Wayland socket 加不上） | 已修复 | logind 未创建 `/run/user/1000` → `XDG_RUNTIME_DIR` 指向不存在目录；改为 `$HOME/.xdg`（0700），日志落 `$HOME/mango.log` |
| B-029 | tty1/Mango 未启动 | 已修复 | `serial-getty@ttyGS0` 持有 `/dev/console` 的 `flock`，阻塞 tty1；默认移除串口 getty 自动启用 |

### B-029 tty1 被 USB 串口 getty 锁住（2026-10-26）

- 根因：`getty@tty1` 的 systemd executor 阻塞在 `/dev/console` 的 `flock`；`serial-getty@ttyGS0` 持有该独占锁，导致 tty1 自动登录与 MangoWM 启动链未运行。
- 修复：rootfs 不再启用 `serial-getty@ttyGS0`，`elish.fixroot=1` 分支也不再每次开机重建其 wants 链接；服务模板仍保留，可按需手动启用。
- 真机验证：串口 getty 停止后 tty1 登录会话及 `/usr/bin/mango` 正常运行。当前设备的旧 initramfs 仍带 `elish.fixroot=1`，故设备端实例额外使用可逆 mask；重启用时先 `systemctl unmask serial-getty@ttyGS0.service`，再 `systemctl enable --now serial-getty@ttyGS0.service`（启用后可能再次与桌面争用 console 锁）。

### 真机运行态证据（2026-10-24 23:4x，经 USB 网络 ssh）
- `ps -u magma`：`/usr/bin/mango` 在 tty1、`Xwayland :0 -rootless` 同时在跑；
- `$HOME/.xdg/wayland-0` socket 存在；`/dev/dri/card0` + `renderD128` 可访问；
- `magma` 已在 `video/render/input/seat` 等组；`seatd -g seat` 运行中；
- 屏幕：MangoWM 平铺桌面（用户确认）。

### 遗留（下一步）
- `dbus-broker` 未运行 → `systemctl`/`loginctl` 不可用（不影响桌面，但要修）；
- `resize.f2fs` 把根从 16 GiB 扩到 195 GiB（真机执行）；
- 固件 GPU 早期加载失败（`a650_sqe.fw` error -2，initramfs 里没有固件）—— 需把 GPU 固件放进 initramfs；
- 音频因缺 `hexagonrpcd` 预期无声；Wi-Fi/BT 未配置；suspend/触摸/相机未验证；
- FEX-Emu（S5）与性能守护进程上机标定。

## B-029 dbus 未启动 → systemctl/nmcli 全部失效（真机）

- 现象：`systemctl is-system-running` / `nmcli` 一律 `Failed to connect to system scope bus: Connection refused`；`/run/dbus/system_bus_socket` 存在但无监听。
- 根因（journalctl 实证）：`dbus-broker.service: Failed to set up mount namespacing: /boot: No such device` → `status=226/NAMESPACE`。
  systemd 的 **gpt 自动生成器** 把 Android 的 ESP 类型分区 `/dev/sda35` 生成 `boot.automount` 挂到 `/boot`，
  该分区不是可挂载的 FAT（`FAT-fs: bogus number of reserved sectors`），挂载失败后 `dbus-broker` 的 mount namespace 初始化失败，系统总线永远起不来。
- 处置：屏蔽两个自动生成的单元（`/etc/systemd/system/boot.mount`、`boot.automount` → `/dev/null`），重启后 `systemctl is-system-running` = **running**，`dbus-broker` 正常。
- 预防：内核 cmdline 追加 `systemd.gpt_auto=no`（我们不需要 /boot，内核与 initramfs 都在 boot_b）。
- 顺带：Wi-Fi 侧已验证可用 —— `nmcli device status` 显示 `wlp1s0 wifi disconnected`，`nmcli device wifi list` 能扫到一堆 2.4 GHz 热点。

## B-031 设备上 pacman 任何操作都失败（Landlock / 文件冲突）

- 现象：`pacman -S clang` → `error: restricting filesystem access failed because Landlock is not supported by the kernel!` / `switching to sandbox user 'alpm' failed!`
- 根因：pacman 7 的沙箱依赖内核 Landlock（`CONFIG_SECURITY_LANDLOCK`），pmOS 的 sm8250 内核未开启。
- 处置：`/etc/pacman.conf` 里打开 `DisableSandboxFilesystem` 与 `DisableSandboxSyscalls`。
- 次生问题：装包时报 `... exists in filesystem`（镜像由 tar 装配，文件无 pacman 记录），用 `--overwrite "*"` 安装。
- 影响记录：这些改动只存在于真机，重建镜像时应把 `DisableSandbox*` 写进 `configs/rootfs/etc/pacman.conf`。

## B-032 pacman 装任何包都报 `... exists in filesystem`（真机）

- 现象：`sudo pacman -S git neovim firefox` → 大量 `pkg: /path exists in filesystem`，`Errors occurred, no packages were upgraded`。
- 根因：镜像由 rootless 流水线用 `tar` 解包装配，**只给一部分包写了 `/var/lib/pacman/local/<pkg>-<ver>/`**（真机上 `pacman -Q` 只有 262 条，实际磁盘上有 496 个包的文件）。
  pacman 认为这些文件“无人拥有”，因此拒绝覆盖 → 报 exists in filesystem；`pacman -Qo <file>` → `No package owns ...`，`pacman -Q libuv` → `package not found`，即 DB 里根本没有这条记录。
- 现场修复（已验证）：用构建时解析出的包清单 `pkgs.tsv`（185 条）做一次全量重装，把文件与 DB 记录对齐：
  ```
  cut -f3 .build/rootfs/.state/pkgs.tsv | sort -u > /tmp/pkgs.txt   # 宿主上生成
  sudo pacman -S --noconfirm --overwrite '*' $(cat /tmp/pkgs.txt)  # 设备上执行
  ```
  结果：`pacman -Q` 262 → **496**；随后 `htop` 重装无冲突、`tree`/`ripgrep` **全新安装成功**。
- 次生问题：中途失败会残留 `/var/lib/pacman/db.lck`，需 `sudo rm -f /var/lib/pacman/db.lck`；个别包的 DB 目录可能只剩半个（如 `gnu-free-fonts` 缺 `desc`），用 `pacman -S --overwrite '*' <pkg>` 重建。
- **根治（已实现）**：`scripts/build-rootfs.sh` 现在每解包一个包就调用 `mkdbentry.py`，用包内 `.PKGINFO`/`.FILES`/`.MTREE` 生成 `var/lib/pacman/local/<name>-<ver>/{desc,files,mtree}`。已用真实包 `bc-1.08.2-1` 验证：宿主 `pacman --root ... -Qi bc` 能正确解析（名字/版本/文件列表正常）。

## B-033 重启后 Wi-Fi 消失（wlp1s0 不可用 / MAC 全 0）

- 现象：`nmcli device status` → `wlp1s0 wifi 不可用`；`ip link show wlp1s0` → `link/ether 00:00:00:00:00:00`、state DOWN。
- 内核日志（本次与前一次成功的启动**完全一致**）：
  ```
  ath11k_pci 0000:01:00.0: qca6390 hw2.0
  mhi mhi0: Requested to power ON / Power on setup success
  mhi mhi0: Wait for device to enter SBL or Mission mode      <-- 之后没有任何 Mission mode 日志
  ath11k_pci 0000:01:00.0: chip_id 0x0 chip_family 0xb board_id 0xff soc_id 0xffffffff
  ```
- 判断：WCN6855 没有进入 Mission mode（MHI 等待超时），因此固件没跑、MAC 读出来是全 0。
  同样的日志在“能用 Wi-Fi 的那次启动”里也出现过，说明**不是驱动/固件文件缺失**，而是**芯片状态**问题：
  热重启（reboot）后 WCN6855 可能停在异常状态，需要**完全断电冷启动**才能复位。
- **真正根因（2026-10-25 12:30 定位）**：接口 MAC 为 `00:00:00:00:00:00`。内核拒绝把这样的接口拉起来：
  `wpa_supplicant: Could not set interface wlp1s0 flags (UP): Cannot assign requested address` → `Failed to initialize driver interface`
  → NetworkManager 报 `supplicant interface keeps failing, giving up` → `wlp1s0 wifi 不可用`。
  冷启动并不能解决，因为 MAC 依旧读不出来（ath11k 的 `soc_id 0xffffffff` 说明 QMI soc info 读取失败）。
- **处置（已验证）**：给接口设一个本地管理 MAC 后立即恢复：
  ```
  sudo ip link set wlp1s0 address 02:00:00:12:34:56
  sudo ip link set wlp1s0 up
  # -> wlp1s0 UP, nmcli 自动连回 SSID <wifi-ssid>, 192.168.1.90, ping 外网 0%% 丢包
  ```
  已固化为 `wlp-mac.service`（开机在 NetworkManager 之前设 MAC 并 up，可在 `configs/rootfs/etc/systemd/system/` 找到）。
- 完全关机（长按电源）再开机仍然必要——用于 WCN6855 的 MHI 卡死；但**即使冷启动，MAC 仍需要被设置**。
- 体验改进（待做）：给 `wlp1s0` 加一个 systemd 恢复单元（检测 `link/ether 00:00:00:00:00:00` 时 unbind/bind PCI 设备或重新 modprobe ath11k_pci），避免每次都要冷启动。

## B-034 扬声器只有左声道（已修复）

- 现象：8 颗 CS35L41 都有声音，但全部只放左声道；右声道完全无声。
- 根因：`CS35L41_SP_FRAME_RX_SLOT`（regmap `0x4820`）是 **ASP RX 声道选择器**（`0`=左，`2`=右），
  不是 TDM 槽位号。主线 `sm8250.c` 从不设置它，功放保持上电默认值 → 全部播左声道。
- 修复：在 `sm8250_snd_startup()` 的 TERTIARY_TDM 分支里，为每个 codec DAI 调
  `snd_soc_dai_set_channel_map(codec_dai, 0, NULL, 1, &rx_slot)`，其中
  `rx_slot = (j < 4) ? 0 : 2`（DT 里前 4 个是 T* 系列 = i2c3 = 左侧，后 4 个是 B* = i2c1 = 右侧）。
- 验证：`/sys/kernel/debug/regmap/1-004x/registers` 的 `0x4820` = `0x102`，`3-004x` = `0x100`；双扬声器实测正常。
- 弯路（勿重复）：改 `tx_mask` 无效（偶数 DAI 只用 `rx_mask`）；给播放设 TX channel map 会 `-EINVAL`；
  把槽位当 1/4/8 试都没用（该字段不是槽位）；`DSP RX1 Source=ASPRX2` 也没用。详见 `docs/audio.md`。

## B-035 设备卡在 fastboot（boot_b 内容损坏，已恢复）

- 现象：重启后只进 bootloader，无串口设备、无网络；`fastboot boot` 报 `Failed to load/authenticate boot image`。
- 处置：`fastboot flash boot_b output/20261024/boot/boot-fix.img`（md5 `abb2f60a…`）后正常启动，
  根文件系统与所有配置均未受影响。
- 结论：`fastboot boot`（内存启动）对自建镜像不可用；ABL 只接受写入分区的镜像。

## B-036 扬声器音量偏小（已修复）

- 现象：硬件增益看似拉满仍偏小；系统音量到 100% 仍觉得小，用软件增益到 150% 则失真。
- 根因：UCM 的 Speaker enable 序列每次都把 AMP Digital PCM Volume 重设为 822、AMP Analog PCM Volume 重设为 18，
  手动 amixer 调大后一开流就被覆盖。
- 修复：在 configs/rootfs/usr/share/alsa/ucm2/Xiaomi/elish/HiFi.conf 里把数字音量写死 913、模拟音量写 20，
  PCM Source 改回 DSP（保护算法开启），DRE Switch 保持 1。约 +3 dB，软件音量 100% 即可且不失真。
- 备注：升压电流 4000 mA 已确认生效（寄存器 0x3808 = 0x40），不是瓶颈。

## B-037 插充电器充不进电（已修复）

- 现象：插上充电器后 `pm8150b-charger` 显示 Charging，但输入被限 500 mA、电池电流仍为 -235 mA（在放电）。
- 根因：调试用的 USB gadget（`usb-net-debug.service` → ACM 串口 + ECM 网卡）一直挂在 UDC 上，
  端口被当作 USB 数据设备，充电器只能按 SDP 500 mA 供电，而整机耗电约 300 mA 以上。
- 修复：`systemctl disable --now usb-net-debug.service` + `echo "" > /sys/kernel/config/usb_gadget/g1/UDC`，
  并新增 `usb-gadget-off.service`（开机解绑 UDC）。**同时要拔插一次充电器**让充电器重新做识别。
- 结果：`usb_type` 从 Unknown/SDP 变为 **DCP**，`current_max` 500 mA → **1.6 A**，
  电池从 -235 mA 变为 **+731 mA 充电中** ✓
- 备注：串口控制台随之关闭（`/dev/ttyACM*` 不再出现），后续维护走 SSH（192.168.1.90）或 fastboot。

## B-038 快充（33 W）不可用的原因链（部分结论）

### 已确认的硬件/驱动事实

1. PD 已经谈成 **9 V**（`pm8150b voltage_now ≈ 8.8 V`，`tcpm voltage_max = 9 V`），走的是 PM8150B **线性**充电，
   实测输入约 285 mA、电池约 700 mA（约 2.7 W），与 5 V 档基本持平。
2. 快充芯片 **bq2597x 是 2:1 电荷泵**（DT：`bus-ovp 12 V`、`bus-ocp 3.8 A`），需要 VBUS ≈ 2×VBAT + 0.3~0.4 V。
   - 手动使能（sysfs `…/15-0065/registers` 写 `0c c7`）后，芯片立刻清掉使能位并把 `Reg[0A]` 置为 `0x10` = **VBUS_ERRORHI**。
   - 当前 VBAT 3.85 V → 理想 VBUS ≈ 8.1 V，实际 8.8 V 偏高 → 芯片拒绝启动。
3. **PPS 接口其实存在**（不需要改内核）：`/sys/class/power_supply/tcpm-source-psy-*/online` 写 `2` 即进入 PPS，
   再用可写的 `voltage_now` / `current_now` 请求目标电压电流（对应 `tcpm_pps_activate()` / `tcpm_aug_set_out_volt()`）。
4. 但本充电器 **不提供 PPS APDO**：写 `online=2` 返回 `EOPNOTSUPP`（`port->pps_data.supported == false`），
   因此只能停在 9 V 固定档，无法把 VBUS 降到 8.1 V。

### 结论

- **当前充电器+线无法触发 33 W 快充**：缺 PPS 档位，8.8 V 对电荷泵又偏高。
- 电池接近满电（VBAT ≥ 4.2 V，2×4.2+0.4 = 8.8 V）时，9 V 固定档理论上可以启用电荷泵，但那时意义不大。
- 下一步可试：换一个 **支持 PPS（3.3–11 V）** 的充电器，然后把 `online=2` + `voltage_now=2×VBAT+300mV` + 使能 bq2597x 串起来，
  做成开机自启的充电管理守护进程。

### 已做的安全设置

- 把 bq2597x 的电池过流保护打开到 5 A（`Reg[02] = 0x1e`，原来 OCP 是关闭的）。
- 实验结束后主充电器已恢复（`charging_enabled=1`），`tcpm online` 保持 `1`（固定档）。

## B-039 快充打通（bq2597x 电荷泵，已实测）

### 结果

| 项目 | 普通充电（PM8150B 线性） | 快充（bq2597x 电荷泵） |
|---|---|---|
| 电池电流 | ≈ 0.70 A | **≈ 1.75 A** |
| 输入 | 5 V 档 / 9 V 线性 | PD **9 V**（20 W 充电器） |
| 电池温度 | 34 ℃ | **34.3 ℃**（无异常发热） |

### 关键条件（实测得出）

1. **必须让主充电器让路**：先 `charging_enabled=0`，再写 bq2597x `Reg[0C] = 0xC7`，否则芯片会以 `VBUS_ERRORHI` 拒绝启动。
2. **PD 必须到 9 V**：`tcpm-source-psy-*/voltage_max >= 8 V`。5 V 档时电荷泵输入不足（`VBUS_ERRORLO`）。
3. **PPS 不是必须的**：9 V 固定档就能跑（VBAT ≤ 4.2 V 时 2×VBAT+余量 ≈ 8.4~8.7 V，正好在 9 V 档内）。
   但请求 PPS 时要注意 **电流不能超过充电器上限**（例如 iPhone 20 W 是 2.22 A），否则整条 PPS 请求被 `EINVAL` 拒绝、电压不生效。
4. 电池过流保护从关闭改为 **5 A**（`Reg[02] = 0x1E`），避免电荷泵裸奔。

### 固化

- `/usr/local/bin/elish-fastcharge` + `elish-fastcharge.service`（开机自启、崩溃自动重启）：
  每 10 秒检查 USB 在线、PD 合约 ≥ 8 V、VBAT 在 3.4~4.35 V、温度 ≤ 42 ℃，满足则使能电荷泵，否则关闭并回退到 PM8150B。
- 仓库同步：`configs/rootfs/usr/local/bin/elish-fastcharge`、`configs/rootfs/etc/systemd/system/elish-fastcharge.service`。

### 备注

- 小米原装充电器**没有 PPS APDO**（写 `online=2` 返回 `EOPNOTSUPP`），但只要有 **9 V 固定档**同样可以跑电荷泵；
  换用 iPhone 20 W 充电器实测成功（`vmax=9 V, imax=2.22 A`）。
- 充满到 ~90% 后守护进程会自动停用电荷泵（VBAT > 4.35 V），改回线性充电收尾。

## B-040 快充功率上限分析（Xiaomi 原装 vs iPhone 20W）

### 实测（Xiaomi 原装，电荷泵运行中）

| 时刻 | IBAT | VBAT | VBUS(PMIC 实测) | 温度 | 电量 |
|---|---|---|---|---|---|
| 稳定值 | **2.53 A** | 4.086 V | 8.29 V | 35.1 ℃ | 33% |

- 功率 ≈ **10.3 W** 入电池（改造前线性充电只有 0.70 A ≈ 2.7 W）；
- PD 合约：**9 V / 3 A**（`tcpm voltage_max=9000000, current_max=3000000`）；
- 电荷泵：`Reg[0C]=0xc7`、`Reg[0A]=0x04`（变换器开关中）、`Reg[02]=0x1e`（电池 OCP 5 A）。

### 为什么到不了原厂的 20 W

1. **原厂 20 W/33 W 用的是小米私有快充握手（Mi Turbo Charge，11 V+）**，不是标准 PD；
   主线 tcpm 只能拿到充电器广播的标准 PDO —— 本充电器就是 **9 V/3 A（27 W）**，实测电缆压降约 0.7 V，PMIC 端只剩 8.29 V。
2. **bq2597x 是 2:1 电荷泵**：输出电压 ≈ VBUS/2 = 4.15 V，而电池已经 4.086 V，
   只有 ~60 mV 余量 → 电流被回路阻抗限制在约 2.5 A。想再大就必须提高 VBUS（11 V）或降低线损。
3. 换 PPS 也帮不上：原装充电器 `online=2` 返回 `EOPNOTSUPP`（无 PPS APDO）。

### 结论

- 当前方案已把可用的标准 PD 功率吃满：**约 10 W（0.7 A → 2.53 A，3.6 倍）**，温度 35 ℃ 很安全；
- 要达到 20 W+，需要 11 V 档（PD 3.0 11 V PDO 或 PPS 9–11 V）或实现小米私有协议；
- 换一根**更短/线径更粗的原装线**可以减小 0.7 V 压降，能再榨出一点电流。

## B-041 schedplace 常驻占用一整个核心（已修复）

- 现象：`schedplace` 长期 90~99% CPU（`ps` 里 99.4%，etimes 35 分钟）。
- 定位：`/proc/<pid>/stat` 显示 **80% 是内核态时间**（3 秒里 utime+60、stime+236 jiffies）；系统当时有 **1162 个线程**（Firefox 占大头）。
- 根因：`lib/sp_daemon.c` 里 **inotify 每来一个事件就置 `rescan = true` 并执行一次全量 `/proc` 扫描**。
  Firefox 的线程反复创建/退出，事件不断 → 守护进程几乎不停地在扫全部 1162 个线程。
  调大 `probe_ms`（100 → 1000）和 `interval_ms`（500 → 2000）**完全无效**，因为瓶颈在事件路径而不是定时器路径，这也印证了上面的判断。
- 修复：给事件驱动的重扫描加节流 —— 只有距上次扫描超过 `interval_ms` 才真正执行，
  否则交给周期性扫描处理（`perf/schedplace/lib/sp_daemon.c`，`last_rescan_ms` + `rescan_is_event`）。
- 效果：CPU 从 **90~99% → 1.5%**（10 秒内 utime +5、stime +15 jiffies），功能不变。
- 顺带：`configs/perf/schedplace.conf` 同步为 `probe_ms = 1000`、`interval_ms = 2000`。
- 备注：`mango-keys`（键盘守护）也曾出现 98% CPU，重启后恢复 0.3%；推测是 evdev 设备被移除后 fd 一直可读、`os.read` 返回空导致忙等，
  后续可在 `os.read` 返回空时关闭该 fd。

## B-042 mango-keys 长期吃满一个核心（已移除）

- 现象：`python3 /usr/local/bin/mango-keys` 常驻 **96.1% CPU**（实测 `etimes` 7342 s、`%CPU 96.1`），是继 schedplace（B-041）之后的第二个 CPU 大户。
- 程序行为：遍历所有 `/dev/input/event*`，`select()` 0.5 s 超时 + 每 2 s 重扫热插拔，把音量键 115/114/113 转给 `mango-volume`。
- 推测根因：与 B-041 同类 —— 某个 evdev 设备被移除/阻塞后 fd 持续可读、`os.read()` 返回 0 字节，循环不再等待而变成忙等；
  重启守护会短暂恢复（曾观察到重启后 0.3%），随后再次爬升。
- 处置：**直接移除**（用户决定，音量的收益不值得一个核）：
  - 设备：`pkill -f mango-keys` + 删除 `/usr/local/bin/mango-keys` + 从 `/usr/local/bin/mango-shell-start` 去掉启动/清理行；
  - 仓库：删除 `configs/rootfs/usr/local/bin/mango-keys`，同步 `mango-shell-start`，文档（`docs/mango.md`、`handoff-mango-shell.md`、`mango-shell-summary.md`、`README.md`）标注已移除。
- 效果：`ps` 前几名变为 `wechat 10.8%`、`mango 7.1%`，不再有单核常驻占用。
- 替代方案：音量用 dock / OSD 调节；若将来要恢复硬件按键，正确做法是在 `mango` 配置里绑定 keysym（不用轮询守护）。

## B-043 reboot 卡住不关机 / tty 满屏内核日志（已修复）

### 关机卡住的真凶：fstrim 处于 D 状态

- 现象：`reboot` / `poweroff` 后系统不再响应，只能长按电源强制断电；journal 里**完全没有关机序列**（没有 `Stopping …`，`--list-boots` 显示上一次启动的结束时间就是断电瞬间）。
- 定位：`ps -eo pid,stat,comm` 发现 **`fstrim` 处于 `D` 状态（不可中断 I/O）**：
  ```
  1305 Ds  87  /usr/bin/fstrim --listed-in /etc/fstab:/proc/self/mountinfo --verbose --quiet-unsupported
  ```
  `fstrim.timer`（Arch 默认启用，每周）在开机后立刻触发，对 **196 GB 的 f2fs 根分区**做 TRIM。
- 为什么会导致关机卡死：D 状态的进程**连 SIGKILL 都不响应**，systemd 停止 `fstrim.service` 时只能一直等（默认 90 s 后仍无法回收），
  随后 `systemctl reboot` 卡在 stop job 上，看起来就是“卡住不关机”。
- 修复：
  ```
  systemctl disable --now fstrim.timer
  systemctl mask fstrim.timer          # 防其它单元/依赖再次拉起
  ```
  并新增 `/etc/systemd/system.conf.d/10-timeout.conf`：`DefaultTimeoutStopSec=20s`（把任何 stop job 的最坏等待从 90 s 收敛到 20 s）。

### tty 满屏日志

- 原因：`kernel.printk = 4 4 1 7` → console loglevel 4，警告级内核消息（如 SLPI 崩溃）直接打到 tty1；再叠加 `getty --noclear` 保留开机日志。
- 修复：
  - `/etc/sysctl.d/99-quiet-console.conf`：`kernel.printk = 3 4 1 7`（只有 error 以上上控制台）；
  - 会话启动脚本在 `exec mango` 前清屏：`printf '\033[H\033[2J\033[3J' > /dev/tty1`。

### 备注

- 本次开机 SLPI 仍崩溃 2 次（`sensor_process` 的 USER-PD DOG），属于**固件侧**问题，只影响传感器/PDM 麦克风，不影响音频播放；
  控制台安静后不再刷屏。

## B-044 传感器 DSP（SLPI）崩溃循环（已永久关闭）

- 现象：`remoteproc0`（slpi）每 ~40 秒崩一次 —— `PDM: service 'sensor_process' crash: ... USER-PD DOG detects stalled initialization`，
  内核不断 `recovering slpi`；旧固件问题，本系统没有任何场景使用它（音频/视频分别走 ADSP/CDSP，均正常）。
- 附带影响：崩溃日志刷 tty、反复的 remoteproc/fastrpc 重建在关机时可能拖住 I/O。
- 处置（无需重编内核）：
  ```sh
  echo disabled > /sys/class/remoteproc/remoteproc0/recovery   # 禁止自动恢复
  echo stop     > /sys/class/remoteproc/remoteproc0/state      # 断电
  ```
  并固化为 `slpi-off.service`（开机执行，按 `name` 匹配 slpi，不误伤 cdsp/adsp）。
- 验证：`state=offline recovery=disabled`；20 秒内崩溃计数 23 → 23；CDSP/ADSP 仍 `running`。

## B-045 状态栏只画出窗口胶囊（cairo UTF-8 错误态）

- 现象：mango-bar 只有左侧窗口胶囊可见，时钟/托盘/电源全都不出现。
- 定位：把 bar 的 shm 缓冲直接 dump 成 PNG，缓冲区里同样缺这些元素 → 是绘制问题不是合成器问题；
  在绘制各段之间打印 `cairo_status()` 得到 `input string not valid UTF-8`。
- 根因：cairo 一旦收到非法 UTF-8 字符串就进入错误态，**之后所有绘制调用被静默丢弃**；
  窗口标题来自合成器（中文/截断的多字节序列），`ellipsize()` 也可能在字节截断时切断了一个字符。
- 修复：`sanitize_utf8()` 清洗客户端标题与拼接后的胶囊文字、截断后再清洗一次；
  并在画完胶囊后检查 `cairo_status()`，异常则重建 cairo context 兜底。
- 验证：dump 与真机截图均完整显示胶囊 / 时间 / 托盘三图标 / 电源键。

## B-046 编辑模式（mango-actions）里拖动/缩放窗口很卡

- 现象：从 dock 打开编辑模式后，拖动窗口与拖角缩放明显掉帧。
- 根因（两个，都在 `configs/mango-shell/mango-actions.c`）：
  1. `draw()` 每次 motion 都重画整块**全屏** ARGB 缓冲（2560×1600 ≈ 16 MB），并
     `wl_surface_damage_buffer(0,0,整屏)` 提交整屏 → 每个事件都要重传整张纹理；
  2. `cmd()` → `ipc()` 每条命令都 `socket()`+`connect()`+`send()`+**同步 `recv()`**+`close()`，
     拖一个角每帧要发 2 条（movewin + resizewin），等于每帧 2 次阻塞往返。
- 修复：
  1. `cmd()` 改为**长连接 + 只发不收**（发送失败才重连），拖动不再等 compositor 回包；
  2. 位移累积，**最多每 10 ms 发一次**（`cmd_allowed()` + `flush_move()`），抬手时冲掉余量；
  3. `draw()` 只把**选区矩形（含旧位置）+ 按钮条**标脏，而不是整屏。
- 构建注意：`mango-actions` 需要 `libcjson`（`pkg-config --libs libcjson`）；
  `build-dock.sh` 只构建 dock，actions 需单独 `cc`。
