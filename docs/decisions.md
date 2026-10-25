# 决策记录（Decision Log）

所有决策都必须给出可复核的证据（commit / 文件 / 命令输出）。变更需追加新条目，不覆盖旧条目。

## D-001 内核版本：选用 `sm8250-7.2.0_git20261010`（最新版），不回退
- 证据 1：`device/testing/linux-postmarketos-qcom-sm8250/APKBUILD`（pmaports main `c7940da`，2026-10-23）
  `pkgver=7.2.0_git20261010`，源码 `https://gitlab.postmarketos.org/soc/qualcomm-sm8250/linux` tag `sm8250-7.2.0_git20261010`。
- 证据 2：tag 已存在且可下载（HTTP 200）。
- 证据 3：所谓"26.06 支持等级下调"来自 commit `c66aa62e`（2026-05-03，MR !8398），原文原因 =
  "kernel package ... does not meet the community kernel version requirements" + 维护者对 issue #4448 无响应，
  属于**维护策略**问题，无 elish 硬件回归描述。
- 结论：按任务书"优先最新版"取值；只有出现明确的 elish 回归证据才回退到 6.17.0 / 6.16.0。

## D-002 构建方式：无 root 的交叉编译 + 离线 rootfs 组装（不使用 pmbootstrap）
- 事实：宿主 `sudo` 被 `no_new_privs` 拒绝；未安装 pmbootstrap；无 docker/podman；
  但 `unshare -Ur` 可用、`fakeroot`/`bwrap` 存在、`mke2fs 1.47.4` 支持 `-d`、网络可达 Arch/Alarm/AUR。
- 决策：内核用宿主 `clang 23.1.3 + lld` 交叉编译（`make LLVM=1 ARCH=arm64`）；
  rootfs 用 ALARM 官方 aarch64 根包 + 官方仓库包解包组装；镜像用 `mke2fs -d` 直接产出（无需 mount）。
- 影响：与上游 `pmbootstrap` 流程不同，因此 `bug.md` B-004 的 super 分区流程被自然规避；
  同时 `pmbootstrap zap` 清理要求以 `scripts/audit-hygiene.sh` 的等价洁净度检查代替（本机从未产生 chroot）。

## D-003 根文件系统：ext4，4096 字节块/扇区对齐
- 证据：`deviceinfo_rootfs_image_sector_size="4096"`（UFS 存储）。
- 理由：`fastboot flash linux` 直接吃 ext4 镜像；ext4 在 ARM64 上最稳、救援工具最全。
- 回退：若真机挂载失败，改用 f2fs（需内核 `CONFIG_F2FS_FS`，config 中核查）。

## D-004 启动链路：自建最小 initramfs，按 `PARTLABEL=linux` 挂载根分区
- 理由：拒绝 pmOS 的 super 分区/动态分区流程（B-004）；不依赖 `mkinitcpio`（需执行 aarch64 二进制）。
- 细节：静态 busybox + init 脚本；ext4 若为模块则把 `ext4.ko` 一并放入 initramfs；
  cmdline 固定在 boot.img 头部（上游 `kernel-cmdline.conf` 仅 `quiet`）。

## D-005 Git 提交身份
- 使用仓库本地身份 `elish-builder <elish-builder@localhost>`，不修改全局 git 配置。

## D-006 未执行真机刷入
- 事实：宿主未安装 fastboot/adb，且未连接设备。
- 决策：`scripts/flash-elish.sh` 默认 dry-run；真机刷入与开机验证标记为"待硬件"，
  相关验收项在 `docs/plan.md` 中标记 `PENDING-DEVICE`。

## D-007 产物与工作区隔离
- 仓库内只允许 `scripts/ configs/ patches/ perf/ docs/ bug.md README.md .gitignore`；
  源码/工具链/中间产物全部在 `../.build/`；产物只在 `output/<date>/`。

## D-008 MangoWM 采用 0.18.3 + ALARM wlroots0.20 / scenefx 0.5（偏离任务书字面版本约束）
- 证据 1（上游）：MangoWM 提交 `0fc7559c3c`（2026-06-19，标题 "version: chase wlroots 0.20"）之后，
  `0.15.0`~`0.18.3` 的 `meson.build` 依赖为 `wlroots-0.20 >= 0.20.0` 与 `scenefx-0.5 >= 0.5.0`；
  `wlroots-0.19 >= 0.19.0` + `scenefx-0.4 >= 0.4.1` 只出现在 `<= 0.14.4` 的旧版 `meson.build`（第 32/37 行）。
- 证据 2（发行版）：Arch Linux ARM 仓库现有 `wlroots0.20 0.20.2-1`、`scenefx 0.5-1`；
  无 0.19.x 的 wlroots 包。AUR `wlroots0.19`（0.19.3，orphan）、`scenefx0.4`（0.4.1，2026-03-16 起 OOD）需自建。
- 决策（用户 2026-10-24 裁决）：走 0.18.3 + 现成仓库依赖，零自建；任务书的 "wlroots 0.19.2 / scenefx 0.4.1"
  视为对**当时**上游状态的描述，而非永久锁版。
- 影响：S3 不再需要自建 wlroots/scenefx；对 AUR 版本的回退配方不进入主链路。
- 复核方式：`scripts/audit-hygiene.sh` 之外，S3 的构建脚本必须打印实际解析到的 wlroots/scenefx 版本并存档。

## D-009 dtbo_b 一次性 erase 例外（用户显式授权）
- 背景：Armbian Flashing-Guide 与 pmOS wiki 的 elish 流程要求 `fastboot erase dtbo_b`；
  本项目的分区硬边界原本绝对禁止该操作（B-004）。
- 决策（用户 2026-10-24）：**授权一次性例外**——仅 `dtbo_b` 的 erase 允许执行，且必须：
  (a) 由显式命令行开关 `--allow-dtbo-erase` 触发，默认永不执行；(b) 执行前二次确认并打印将执行的完整命令；
  (c) 结果写入 `output/<date>/FLASH-LOG` 与 `bug.md`；(d) 脚本仍然拒绝除 `boot_b`/`linux` 之外任何分区的 `flash`，
  也拒绝除 `dtbo_b` 之外任何分区的 `erase`（包括 `system_b`/`userdata`/`vendor_boot`/`super`）。
- 未放宽的部分：分区硬边界的其余部分不变；本次授权只覆盖当前设备的一次刷入，不构成默认行为。

## D-010 linux 分区文件系统改用 f2fs（用户 2026-10-24 指定，取代 D-003 的 ext4）
- 事实：内核 config 为 `CONFIG_EXT4_FS=y`、`CONFIG_F2FS_FS=m`（pmaports main `c7940da`）。
- 决策 1（内核侧）：追加 config 片段 `CONFIG_F2FS_FS=y`，使根文件系统驱动内置——首次开机的根挂载不再依赖
  initramfs 里的 f2fs.ko 依赖链，减少一个不可在无设备环境下验证的失败点。这是相对上游 config 的唯一偏差。
- 决策 2（镜像侧）：`linux` 分区镜像用 f2fs；无 root 无法 mount，因此用 `mkfs.f2fs` + `sload.f2fs` 从目录填充。
  f2fs-tools 由 Arch 官方包解包到 `../.build/tools`（用户级安装）。
- 决策 3（参数）：`mkfs.f2fs` 参数以 `mkfs.f2fs -h` 实测语义为准；重点对齐 UFS 4096 逻辑扇区
  （`deviceinfo_rootfs_image_sector_size=4096`）；**不启用 compression 特性**（内核未开 `CONFIG_F2FS_FS_COMPRESSION`）。
- 决策 4（挂载）：`/etc/fstab` 用 f2fs + `PARTLABEL=linux`；挂载选项逐条依据内核
  `Documentation/filesystems/f2fs.rst`，禁止照抄博客。
- 回退：若真机上 f2fs 不可用，退回 ext4（rootfs 模块保留 `--also-ext4` 产物）。

## D-011 性能方案：不采用 gamemodrun 式核心调度（用户 2026-10-24 明确指示）
- 用户判断：工作区既有 `gamemod-elish-src` 属 gamemodrun 一类工具——运行 Steam 游戏时把调度器切成
  `performance` 并把 `steamwebhelper` 绑到小核；实际收益有限，做法有大量可优化空间。
- 决策：**不作为核心调度器**。新方案 `perf/schedplace` = 按进程/线程名的 cpuset + CPU affinity 规则，
  不锁频、不停核、不改全局 governor；借鉴旧代码的进程名匹配/配置解析/Steam 识别思路，核心机制重写。
- 允许保留的旧行为仅限"可选运行时 profile"（用户显式开启才生效），默认关闭，且必须在 bug.md 记录预期收益与回退方式。
- 复核方式：`perf/schedplace` 自带可在 x86_64 宿主运行的单元测试（规则匹配用 mock /proc 数据），
  决策输出到 journald，便于真机前后对比。

## D-012 initramfs 携带面板模块（早期出图），模块以解压后的 .ko 打包
- 事实 1：`CONFIG_DRM_MSM=y`（内置），但 `CONFIG_DRM_PANEL_NOVATEK_NT36523=m`（模块）；
  两个 elish dtb 的 panel compatible 为 `xiaomi,elish-boe-nt36523` / `xiaomi,elish-csot-nt36523`（同一驱动）。
- 事实 2：上游 config 开启 `CONFIG_MODULE_COMPRESS_ALL=y` + `MODULE_COMPRESS_ZSTD=y`，模块为 `*.ko.zst`；
  busybox 的 insmod 不能解 zstd。
- 决策：initramfs 打包时把需要的 `.ko.zst` 用宿主 zstd 解压成 `.ko` 再放进 cpio，`/init` 里 insmod 面板模块后
  再挂根分区并 switch_root（insmod 失败仅告警，不阻塞挂根）。目的：无 adb 时也能靠屏幕看到早期日志，且面板尽早点亮。
- 边界：initramfs 不含根文件系统驱动模块（F2FS/EXT4 均内置），因此根挂载不依赖任何模块。

## D-013 旧系统（192.168.1.90）实测事实（2026-10-24，只读采集）
采集方式：`dbclient -y -i ~/.ssh/id_ed25519 elish@192.168.1.90 sh`（对端登录 shell 是 fish，
必须显式执行 `sh` 并把脚本走 stdin，否则 POSIX 语法会被 fish 拒绝；`blockdev` 等需要 root 的命令取不到值）。

| 项 | 实测值 | 影响 |
|---|---|---|
| 机型/面板 | `/sys/firmware/devicetree/base/model` = "Xiaomi Mi Pad 5 Pro (CSOT)" | **默认 dtb 用 `sm8250-xiaomi-elish-csot`**，boe 只作备选产物 |
| root 分区 | `/dev/disk/by-partlabel/linux -> ../../sda36`，205,039,616 KiB ≈ 195.5 GiB，当前 ext4，`findmnt` 显示 `/dev/sda36 on /` | linux 分区**已存在且极大**（B-016 关闭）；镜像按实际需要定尺寸，首启后可用 `resize.f2fs` 扩到分区末尾 |
| boot_b | `/dev/disk/by-partlabel/boot_b -> ../../sde37`，393,216 扇区 = 192 MiB | boot.img 必须 < 192 MiB；B-013 的 sde37 映射已确认 |
| boot_a | `sde12` | 仅作对照，本项目不写 |
| CPU 拓扑 | cpu_capacity: cpu0-3 = 284，cpu4-6 = 871，cpu7 = 1024 | schedplace 的 little/big/prime 聚类目标值（运行时发现，不硬编码） |
| 现用 cmdline | `root=UUID=7dc6b70a-… rootwait rw slot_suffix=_b console=tty0 loglevel=4` | 我们可用 `PARTLABEL=linux` 等价替代；ABL 会再追加 androidboot.* 等参数 |
| 旧内核 | `7.2.0-xiaomi-elish+ #4 SMP PREEMPT Sat Oct 3 17:36:59 CST 2026` | **7.2.0 已在该机型上实测可跑**，进一步支持 D-001 不降版 |
| 旧系统工具 | 有 gcc/g++/make/cmake/ninja/git/makepkg/python3/rustc；**无 meson**；8 核 / 7.5 GiB / 剩余 32 GiB | 可作 aarch64 原生构建宿主（见 D-014，待用户授权） |
| 旧系统缺 f2fs 工具 | 无 `mkfs.f2fs`/`fsck.f2fs`/`sload.f2fs`；`/lib/modules/7.2.0-xiaomi-elish+/` 下有 `f2fs.ko` 与两个 panel 模块 | f2fs 镜像必须在宿主侧制作（sload.f2fs），不能在设备上格式化 |
| 旧桌面 | `/usr/share/wayland-sessions/` = kwin-steam.desktop / plasma.desktop（Plasma），无 wlroots/scenefx/mangowm | 与本项目 MangoWM 路线无冲突 |

- 采集命令示例（可复核）：`grep -E ' sda36$| sde37$' /proc/partitions`、`cat /sys/class/block/sde37/size`、
  `cat /boot/elish-cmdline`、`findmnt -no SOURCE,FSTYPE,OPTIONS /`。
- 未取到（需 root 或额外授权）：`/sys/class/block/sda36/queue/logical_block_size`（读为空）→ f2fs 的 `-s`
  仍按 `deviceinfo_rootfs_image_sector_size=4096` 取 12，并在真机首启日志里复核。

## D-014 `fastboot --set-active=b` 交由人工执行，不进入脚本
- 理由：切换活动槽写的是 ABL/bootloader 的槽位元数据（不是 `boot_b` 也不是 `linux`），落在本项目
  “只写 boot_b + linux”的允许写集合之外。即使结果对用户有利，也不应由脚本代劳。
- 实现：`scripts/flash-elish.sh` 只允许 `flash`(boot_b/linux)、`getvar`(只读)、`reboot-bootloader`、
  以及 D-009 门控的 `erase dtbo_b`；`docs/flash-guide.md` 把 `--set-active=b` 与回滚 `--set-active=a`
  写成人工步骤，并说明 a 槽本项目不改不擦。
- 复核：`bash scripts/flash-elish.sh --op set-active=b` 必须被拒绝（exit 2）。

## D-015 授权把旧系统（192.168.1.90）作为 aarch64 原生构建宿主（用户 2026-10-24）
- 授权范围：只在 `/home/elish/elish-build/` 内活动；不安装系统包、不写 /usr、不改 systemd、不碰任何分区。
- 用途：MangoWM 0.18.3 必须在 aarch64 上编译（AUR `mangowm-git` 只声明 x86_64），而本机 x86_64 且无 root。
  设备侧已验证具备 gcc/g++/make/cmake/ninja/git/makepkg/python3/rustc（缺 meson，纯 Python 可在 $HOME 跑），
  8 核 / 7.5 GiB / 剩余约 32 GiB。依赖（wlroots0.20、scenefx、wayland 头文件）以 ALARM 包解包进
  `~/elish-build/sysroot` 的方式提供，不 pacman 安装。
- 限制：构建产物只回传到宿主 `output/<date>/mango/`；设备侧目录可整体删除；禁止在设备上执行
  `mkfs`/`mount`/`dd` 到任何分区。
- 回退：若构建破坏旧系统可用性（不预期），改用交叉编译或 qemu-system 路线，并在 bug.md 记录。

## D-016 首次刷入时机：产物齐备后由用户决策/执行（用户 2026-10-24）
- 交付物形态：`output/<date>/{kernel,rootfs,boot}/` + `MANIFEST` + `docs/flash-guide.md`，达到"可一键执行"状态。
- 约束：任何真机写入都必须在用户在场确认后执行；在此之前所有 fastboot 调用只走 dry-run。
- 记录：刷入结果（成功/失败/回退）由用户回报后写入 bug.md 与 FLASH-LOG。

### D-010 勘误（2026-10-24，由 rootfs 模块实测指出，dispatcher 已独立复核）
- 勘误内容：D-010 原文写的 "`mkfs.f2fs` 的 `-s 12` 对齐 4096 扇区"是**错的**。
  宿主 `mkfs.f2fs -h` 实测：`-s # of segments per section [default:1]`，扇区大小对应的是 `-w wanted sector size`
  （man 8 亦同）。用 `-s 12` 会错误地把"每 section 12 段（24 MiB）"写进布局。
- 复核证据（dispatcher 亲手执行）：`mkfs -h` 输出含 `-s # of segments per section`、`-w wanted sector size`；
  64 MiB 试探镜像的 `fsck.f2fs -l` 显示 `log_sectorsize: 9`、`log_sectors_per_block: 3`（9+3=12）；
  `dump.f2fs -f` 显示 `superblock features = 0`。
- 修正后的做法：
  1. `-w 4096` 对**普通文件镜像无效**（工具对文件强制用 512），`-g android` 亦然；
  2. 内核侧只要求 `9 <= log_sectorsize <= PAGE_SHIFT(12)` 且 `log_sectorsize + log_sectors_per_block == 12`，
     不与设备逻辑扇区比较（`fs/f2fs/super.c`）；
  3. 交付镜像 = 工具原生镜像（fsck 通过）→ 再把两个冗余超级块的该字段改为 12/0，并用 `cmp -l` 证明只有 4 字节差异；
  4. **关键安全前提**：`superblock features = 0`（未启用 `sb_checksum`），因此裸改超级块不会破坏校验和；
     若日后启用该 feature，必须先重算 CRC 再改，或改用 `--f2fs-sector-size 512`。
  5. 回退：`--f2fs-sector-size 512` 一条命令回到工具原生镜像；真机若报 sector size 不匹配即用它重制。
- 结论：D-010 的文件系统选择（f2fs）、compression 关闭、内置 `CONFIG_F2FS_FS=y` 均不变，仅扇区参数实现方式更正。

## D-017 f2fs 镜像校验的操作性约束（S1 返工得出的硬规则）
- 事实（rootfs 模块返工时的根因分析）：`fsck.f2fs -l` 与 `dump.f2fs` **会改写超级块**（打印 "Done to update superblock"），
  所以"对交付镜像跑只读查询"这件事本身会改变交付镜像的字节。
- 规则：
  1. 所有校验（fsck/dump/od 读值）一律在**副本**上做：`cp --sparse=always` 复制后再跑；
  2. 交付镜像的生成顺序固定为 `mkfs.f2fs -> sload.f2fs -> fsck.f2fs -f（写回）-> 4 字节扇区改写 -> 计算 SHA256`；
  3. 洁净判据不是 `fixing SIT types` 行数（-f 模式每次都会打印，实测连续两次均为 1），而是
     **`[Fail]` 行数 = 0 且 exit code = 0**，外加"交付镜像与 fsck 副本 `cmp -l` 仅差 4 字节"这一不动点性质。
- 复核方式：`bash scripts/audit-hygiene.sh` 校验 SHA256；S1 收口时的实测结果记录在 bug.md 第八节。

## D-018 boot.img 的 dtb 必须紧跟内核（禁止任何填充）；initramfs 必须把根重新挂为 rw 并搬移伪文件系统
- 证据（真机实测 2026-10-24）：
  (a) 最初把 dtb 以 2048 字节对齐追加到内核段之后，ABL 对 fastboot boot 以及刷入 boot_b 后的镜像都报
      'Failed to load/authenticate boot image: Load Error'；且 slot-retry-count:b 不递减（说明 ABL 在装载阶段就拒绝，根本没进内核）；
  (b) 改成与 postmarketOS boot-deploy 完全一致的做法 —— cat Image.gz <dtb> > payload（零填充）—— 之后设备立刻离开 fastboot 进入内核：
      面板点亮、panel_novatek_nt36523 insmod 成功、f2fs 根分区挂载成功。
- 参考实现证据：boot-deploy 0.25.0 / boot-deploy-functions.sh 第 410-415 行 cat "$work_dir/$kernel_filename" $_dtb > "$_outfile"。
- 规则：
  1. 内核段负载一律 cat kernel dtb，不得插入任何对齐/填充；
  2. initramfs 用 -o ro 探测挂载后必须 mount -o remount,rw（cmdline 是 rw，systemd 需要可写根）；
  3. switch_root 前必须把 /dev /proc /sys /run 用 mount --move 搬进新根，否则新根里是空目录；
  4. 真机调试镜像用 panic=0 便于读屏，正式镜像保持 quiet。
