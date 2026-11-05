# 刷机指南（xiaomi-elish · Arch Linux ARM）

本文件是**唯一的真机刷入操作说明**。整个流程只动三个分区：写 `boot_b`、写 `linux`，外加**擦除 `dtbo_b`**（必需）：

| 分区标签 | 设备节点（**用 `by-partlabel` 查**） | 容量 | 内容 | 写入方式 |
|---|---|---|---|---|
| `boot_b` | `/dev/disk/by-partlabel/boot_b`（本仓库测试设备上 → `sde37`） | 393216 扇区 = **192 MiB** = `0xC000000` | Android boot image v0 | `scripts/flash-elish.sh --boot ...` |
| `linux` | `/dev/disk/by-partlabel/linux`（本仓库测试设备上 → `sda36`） | 以 `fastboot getvar partition-size:linux` 为准（测试设备 ≈195.5 GiB） | 根文件系统镜像（f2fs） | `scripts/flash-elish.sh --root ...` |

括号内是**本仓库测试设备的值，你的设备可能不同**：块设备号因设备而异，一律用 `/dev/disk/by-partlabel/` 访问。
实测来源：`docs/decisions.md` D-013（2026-10-24，ssh 只读采集）。本仓库测试设备（Mi Pad 5 Pro）为 **CSOT 面板**
（`model = 'Xiaomi Mi Pad 5 Pro (CSOT)'`），因此默认产物 `boot.img` 用
`sm8250-xiaomi-elish-csot.dtb`；BOE 备选产物是 `boot-boe-alt.img`（文件名即区分）。

`dtbo_b` 的**一次性 erase 是装机必需步骤，不是可选例外**：设备用 _b 槽启动，ABL 会读 `dtbo_b`，
Android 留下的 dtbo overlay 与本项目的 dtb 冲突 → 黑屏/开不了机。实机已确认（`dtbo_b` = `sde43` / 32 MiB，
擦除后全零）。脚本仍把它做成显式门控（`--allow-dtbo-erase` + 二次确认）以免误擦；
本项目对 `dtbo_b` **只擦不写**：`--flash dtbo_b=...` 在任何开关下都 exit 2。

> **所有刷写与格式化都只能在宿主机执行**：设备上的旧系统既没有 `fastboot`/`adb`，也没有 `mkfs.f2fs`。
> 设备侧仅用于"看分区/看日志"。

---

## a) 前置条件检查

### a.1 宿主机

```bash
command -v fastboot          # 必须存在；缺失时 pacman -S android-tools（本指南不代装）
fastboot devices             # 必须列出设备
fastboot getvar current-slot # 期望 a（本项目只写 _b 槽位，见 d.4）
```

### a.2 确认两个分区标签都在（设备侧，只读）

```bash
ls -l /dev/disk/by-partlabel/ | grep -E 'boot_b|linux'
```

两行都应存在（形如 `boot_b -> ../../sdXN`、`linux -> ../../sdXN`）；**链接目标以你设备上的实际编号为准**，
不要拿本仓库测试设备的 `sde37`/`sda36` 去比对。
宿主机侧可只用 fastboot 做只读核对：

```bash
fastboot getvar partition-size:boot_b    # 期望 0xC000000（393216 扇区 × 512 B = 201,326,592 B = 192 MiB）
fastboot getvar partition-size:linux     # 期望你的 linux 分区实际大小
```

### a.3 仅当 a.2 里 **没有** `linux` 时才需要人工建分区

> 现状（D-013，**测试设备快照，非通用前提**）：`linux` 指向 `/dev/sda36`、≈195.5 GiB，**已存在**，缺陷 B-016 已关闭。
> 下面这段是"万一分区丢了"的兜底流程，正常情况下**不要执行**。

`linux` 不是出厂分区，若确实缺失，只能由你从 `userdata` 尾部腾空间后创建。
这是**不可逆的破坏性操作**：

```bash
# ⚠️ 仅作参考；必须由你在设备侧自行执行并自行承担风险
# ⚠️ 本项目脚本绝不执行该操作，也绝不读写 userdata
parted /dev/block/sda print             # 只读：确认 userdata 起止扇区
parted /dev/block/sda unit s print      # 记录 userdata 起始 S 与结束 E
# 1) 先把 userdata 文件系统缩到新尺寸（resize2fs），再改分区表
# 2) parted /dev/block/sda rm <userdata号> mkpart linux f2fs <新起点>s <E>s name <号> linux
```

风险：缩小 `userdata` 会丢数据；分区表写坏后除 fastboot 外的救援路径（EDL）本项目未验证。
`scripts/flash-elish.sh` 与 `scripts/audit-hygiene.sh` 中**没有任何**操作 `userdata` 的代码路径，
`--flash userdata=...`、`--erase userdata` 一律 exit 2（见 `tests/test-flash-safety.sh`）。

---

## b) f2fs 格式化（**只能在宿主机执行**）

`linux` 分区使用 f2fs（D-010）。设备上没有 `mkfs.f2fs`，所以镜像必须在宿主机构造：

```bash
mkfs.f2fs -f \
  -l linux \
  -b 4096 \
  -t 0 \
  -O extra_attr,inode_checksum,sb_checksum \
  -R 0:0 \
  output/20261024/rootfs/linux-rootfs-f2fs.img
```

参数依据（**实测**：`mkfs.f2fs` 1.17.0 (2026-09-24) 的 `-h`，取自 Arch `extra/f2fs-tools 1.17.0-1`）：

| 参数 | 含义 | 依据 |
|---|---|---|
| `-f` | 强制覆盖 | `-f force overwrite of the existing filesystem`；重跑必需 |
| `-l linux` | 卷标 = 分区标签 | 与 PARTLABEL=linux 一致；fstab 用 `PARTLABEL=linux`（`LABEL=linux` 是等价写法） |
| `-b 4096` | 块大小 4096 | `[default:4096]`，与 UFS 4096 逻辑扇区对齐 |
| `-t 0` | 不发送 discard | `-t 0: nodiscard, 1: discard [default:1]`；镜像上无意义且引入非确定性 |
| `-O extra_attr,inode_checksum,sb_checksum` | 额外属性 + 校验和 | 1.17.0 实测合法（`-O nonsense` 报 `Error: Wrong features nonsense`） |
| `-R 0:0` | 根目录属主 root:root | `-R root_owner [default: 0:0]` |

**禁用 compression**：`-O` 里不写 `compression` 即为禁用。实测 `-O compression` 单独用会报
`compression feature should always be enabled with extra attr feature`，取反写法 `-O ^compression` **不被支持**。

**扇区大小（未验证）**：`mkfs.f2fs` 的扇区大小取自底层设备。在构建主机的普通文件上即使加 `-w 4096`
仍输出 `Info: sector size = 512`；要 4096 扇区镜像需要 4096 逻辑扇区的后端（`losetup -b 4096`，需 root）。

### b.2 首启后扩容（设备侧，人工执行）

镜像可能小于分区（`linux` 分区大小以 `fastboot getvar partition-size:linux` 为准），首启进入系统后**手动**扩到分区大小：

```bash
# 设备上执行（rootfs 需自带 f2fs-tools；脚本不会自动做这一步）
findmnt /                                   # 确认根是 f2fs 且已挂载
resize.f2fs /dev/disk/by-partlabel/linux    # 在线扩容到分区末尾
df -h /
```

> 本项目脚本**不会**自动执行 `resize.f2fs`：它修改在线文件系统，必须在人确认 `findmnt` 输出后手动跑。
> 扩容失败（例如 `resize.f2fs` 不在镜像里）时把输出原文回填到 bug.md。

---

## c) `/etc/fstab`

现状（仓库 `configs/rootfs/etc/fstab`，与设备 `/etc/fstab` 一致）：

```fstab
PARTLABEL=linux  /  f2fs  rw,relatime,background_gc=on,inline_xattr,inline_dentry,nodiscard,errors=remount-ro  0 1
```

- `PARTLABEL=linux`：按 GPT 分区标签定位根，分区重建/重新格式化后仍然有效。
- `relatime`：通用 VFS atime 语义（非 f2fs 专有选项），避免每次读都回写 atime。
- `background_gc=on`：f2fs 默认值，显式写出便于审阅。
- `inline_xattr` / `inline_dentry`：xattr 与目录项写进 inode 块，减少小文件/目录的额外块写。
- `nodiscard`：不在每次 segment 清理时发实时 TRIM，改用已启用的 `fstrim.timer`，避免持续抖动。
- `errors=remount-ro`：致命错误时重挂只读（内核默认是 `continue`）。
- 不加 `compress_algorithm=`/`compress_extension=`：内核没有 `CONFIG_F2FS_FS_COMPRESSION`，镜像也未开压缩特性。

`/init` 仍按 f2fs → ext4 的顺序尝试挂载（兼容 ext4 根镜像），但本项目产出的根文件系统是 f2fs。

---

## d) 刷入顺序（逐条含校验与回退）

`IMG_LINUX=output/20261024/rootfs/linux-rootfs-f2fs.img`、`IMG_BOOT=output/20261024/boot/boot.img`（CSOT 默认）。
BOE 机型改刷 `output/20261024/boot/boot-boe-alt.img`。

### d.0 dry-run（不碰设备）

```bash
scripts/flash-elish.sh --root "$IMG_LINUX" --boot "$IMG_BOOT"
```

默认 dry-run 只打印命令、**不调用 fastboot**（`tests/test-flash-safety.sh` 断言 mock 零调用）。

### d.0b 推荐形式：一条命令同时刷两个分区

```bash
scripts/flash-elish.sh \
  --boot output/20261024/boot/boot.img \
  --root output/20261024/rootfs/linux-rootfs-f2fs.img \
  --execute
```

为什么推荐这种形式：

1. **一次校验两个镜像的 SHA256**：`--boot` 走 `--boot-sha256`/`sidecar`/`MANIFEST`，`--root` 走
   `--root-sha256`/`sidecar`/`MANIFEST`，用同一条命令就把两个产物都钉死。
2. **每个分区各一次 `getvar partition-size` 体积检查**，任何镜像超限都在**写入前**中止。
3. **只写 `boot_b` 与 `linux`**：白名单/erase 门控与单分区调用完全一致，不会因为"合并调用"放宽任何策略。
4. dry-run 下整条命令只打印，可先跑一遍确认命令序列再决定是否加 `--execute`。

`--flash PART=IMG` 形式同样受支持，且哈希参数按**目标分区角色**绑定（不是按传参形式）：

```bash
scripts/flash-elish.sh --flash linux=<img>  --root-sha256 <sha>    # --root-sha256 生效
scripts/flash-elish.sh --flash boot_b=<img> --boot-sha256 <sha>    # --boot-sha256 生效
```

校验来源的查找顺序（第一个命中即用）：

1. `<image>.sha256` sidecar —— `scripts/collect-manifest.sh` 为每个产物生成，格式为
   `<sha256>  <相对路径>`（例：`eb9b5510…f843  output/20261024/rootfs/linux-rootfs-f2fs.img`）。
2. `--manifest PATH` 显式指定的清单；给了它就不再搜索目录。
3. 默认搜索：`output/<date>/MANIFEST` → `output/<date>/MANIFEST.boot` → `output/<date>/MANIFEST.*`。
   清单同时支持 `名称 | 目标 | SHA256 | 字节 | 时间` 与空格分隔两种写法，并按 **basename** 匹配，
   所以 `rootfs/linux-rootfs-f2fs.img | linux | …` 对 `--flash linux=…/linux-rootfs-f2fs.img` 同样有效。
4. 全都找不到时：dry-run 只 WARN，并**直接打印可复制的** `--root-sha256 <实际值>` / `--boot-sha256 <实际值>`、
   sidecar 命令与 `scripts/collect-manifest.sh` 三个修法；`--execute` 则直接拒绝执行。

### d.1（必需）擦 dtbo_b —— 不擦则 _b 槽启动黑屏

```bash
scripts/flash-elish.sh --execute --allow-dtbo-erase --confirm-erase dtbo_b --erase dtbo_b
```

三重条件：`--execute` + `--allow-dtbo-erase` + 二次确认（`--confirm-erase dtbo_b` 或交互输入）。
只发出一条 `fastboot erase dtbo_b`，结果写进 `output/20261024/FLASH-LOG`（审计会复核，erase 记录只可能是 dtbo_b）。
回退：dtbo_b 原内容（Android 的 dtbo overlay）不会被本项目备份。回 Android 时需从官方 fastboot 包里
取出原厂 `dtbo.img` 再 `fastboot flash dtbo_b <dtbo.img>`。`boot.img` 里的 DTB 段不受影响；开机异常先重刷 `boot_b`。

### d.2 只刷根文件系统（linux）—— 排障时用，日常用 d.0b

```bash
scripts/flash-elish.sh --execute --root "$IMG_LINUX" --root-sha256 "$(sha256sum "$IMG_LINUX" | cut -d' ' -f1)"
```

写入前必须过三关：文件存在 → SHA256 一致 → `fastboot getvar partition-size:linux` 体积检查。
失败回退：直接重刷（flash 是覆盖写），不要对其它分区 `erase`。

### d.3 只刷引导镜像（boot_b）—— 排障时用，日常用 d.0b

```bash
python3 scripts/make-bootimg.py --verify --input "$IMG_BOOT"   # 先自检
scripts/flash-elish.sh --execute --boot "$IMG_BOOT" --boot-sha256 "$(sha256sum "$IMG_BOOT" | cut -d' ' -f1)"
```

**硬限制：boot.img 必须 < 192 MiB**（`boot_b` = 393216 扇区）。`make-bootimg.py` 在生成时就强制
（超限直接失败并提示"压缩 initramfs 或减少 dtb"），`flash-elish.sh` 再用 getvar 体积做第二道闸。
回退：重刷上一次能开机的 `boot.img`。

### d.4 切到 b 槽并重启

```bash
fastboot --set-active=b
fastboot reboot
```

> `flash-elish.sh` **不执行** `--set-active`：它只允许 `flash`（boot_b/linux）、`getvar`、
> `reboot-bootloader` 与被门控的 `erase dtbo_b`。要脚本自动 `--set-active` 属策略扩张，需先明确裁决。
> `boot_b` 起不来时：`fastboot --set-active=a` 回 a 槽救援（a 槽本项目不改不擦）。

### d.5 手工完整序列（与脚本等价）

```bash
fastboot devices
fastboot getvar partition-size:boot_b
fastboot getvar partition-size:linux
fastboot erase dtbo_b                              # 可选，仅 D-009 需要时
fastboot flash linux output/20261024/rootfs/linux-rootfs-f2fs.img
fastboot flash boot_b output/20261024/boot/boot.img
fastboot --set-active=b
fastboot reboot
```

### d.6 重打 initramfs（`--modules` + `--firmware`）

内核模块产物在 `output/<date>/kernel/modules.tar.zst` 内，路径为
`lib/modules/<krel>/`（本内核 release 是 `7.2.0`，不要写成 `7.2.0_git20261010`），
且因 `MODULE_COMPRESS_ALL=y` + `MODULE_COMPRESS_ZSTD=y` 落地为 `*.ko.zst`。

initramfs 需要两类输入，缺一不可：

1. **面板驱动**：`CONFIG_DRM_PANEL_*=m` 的模块（`panel-novatek-nt36523` 等）及其依赖闭包。
   `DRM_MSM`、`F2FS_FS`、`EXT4_FS` 都是 `=y`，**不要**放 `f2fs.ko`/`ext4.ko`/`msm.ko`。
2. **GPU 固件**（`--firmware`）：`qcom/a650_gmu.bin`、`qcom/a650_sqe.fw`、
   `qcom/sm8250/xiaomi/elish/a650_zap.mbn`。`CONFIG_DRM_MSM=y` 在 rootfs 挂载前（实测 ~1.4 s）
   就会 `request_firmware`，initramfs 里没有会返回 `-2`，GPU/DRM 起不来（B-058）。

约定输入目录（层级与内核产物一致）：

```
.build/initramfs-mods/lib/modules/7.2.0/
└── kernel/drivers/gpu/drm/panel/*.ko.zst
    modules.dep              # 可选；脚本会把 .ko.zst 改写成 .ko
```

```bash
# 1) GPU 固件（自动下载 + sha256 校验；也可以手动放进该目录）
scripts/fetch-gpu-firmware.sh --out .build/firmware/elish-gpu
# 2) 面板模块子集（build-release.sh 会按 modules.dep 自动做依赖闭包）
tar --zstd -xf output/<date>/kernel/modules.tar.zst -C .build/initramfs-mods
# 3) 打包
BUILD_ROOT=$PWD/../.build scripts/build-initramfs.sh \
  --modules .build/initramfs-mods/lib/modules/7.2.0 \
  --firmware .build/firmware/elish-gpu \
  --out output/<date>/boot/initramfs.cpio.gz
```

- `--modules` 同时接受 `.ko` 与 `.ko.zst`（目录或单文件）；`--firmware` 接受固件树根目录。
- **必须**解压 `.ko.zst` → `.ko`：busybox `insmod` 不能解 zstd；`modules.dep` 里的 `.ko.zst` 同步改写。
- 镜像内 `/initramfs.modules` 记录实际打进去的模块；`/lib/firmware/qcom/...` 是 GPU 固件。
- `/init` 里 insmod 失败**不致命**（警告后继续挂载根分区）；模块已内置时正是这种情形。
- 最省事的方式：直接跑 `scripts/build-release.sh`（本脚本已把以上三步串起来并自带自校验）。

---

## e) 禁止清单

| 禁止操作 | 原因 |
|---|---|
| `fastboot flash` 任何非 `boot_b`/`linux` 的分区（`system_b` `dtbo_b` `userdata` `vendor_boot` `super` `boot_a` `modem` `persist` ...） | 只允许两个分区 |
| `fastboot erase` 任何分区 | 唯一例外：`dtbo_b`（D-009 + 二次确认） |
| `fastboot format / oem / wipe / flashall` | 非白名单写操作 |
| 脚本内发起 `fastboot --set-active` | 未获裁决，人工执行 |
| 对 `userdata` / `vendor_boot` / `super` 的**任何**操作 | 零触碰 |

**为什么不做 super 动态分区**：根文件系统直接放物理 `linux` 分区，不经 `super` 与 `fastbootd`/逻辑分区。
理由：少一层失败面；写 `super` 会连带影响 `system_b`/`vendor_b`，与"只允许两个分区"冲突；恢复只需重刷 `linux`。

---

## f) 真机回填清单（刷完必须回报到 `bug.md`）

1. **分区设备名**：`ls -l /dev/disk/by-partlabel/` 中 `boot_b`/`linux` 指向的节点（记录你设备上这两个标签实际指向的节点，不要套用别人的 `sdeNN`/`sdaNN`）。
2. **fastboot 容量**：`fastboot getvar partition-size:boot_b`、`:linux` 的原始输出。
3. **镜像体积**：`ls -l output/<date>/boot/boot.img` 与 192 MiB 的占比。
4. **fsck**：`fsck.f2fs -f /dev/disk/by-partlabel/linux` 完整输出与退出码；开机后 `dmesg | grep -i f2fs`。
5. **挂载与扩容**：`findmnt /`、`df -h /`（扩容前后各一次）、`resize.f2fs` 的输出。
6. **开机关键日志**：`elish-init: found root device ...`、`elish-init: root mounted read-only as f2fs`、
   `elish-init: switch_root ... /sbin/init`、`dmesg | grep -iE 'F2FS|panel|nt36523'`；
   失败时贴 `elish-init: mount -t f2fs failed` 与 `elish-init: FATAL:` 的原文。
7. **FLASH-LOG**：`output/<date>/FLASH-LOG` 全文（含是否动用 D-009）。
8. **不符预期项**：按"期望 / 实际 / 复现步骤"三行写。

---

## 附录：cmdline 与事实边界

本项目 `boot.img` 的 cmdline（`scripts/make-bootimg.py` 默认值）：

```
quiet rootwait root=PARTLABEL=linux console=tty0 loglevel=4 systemd.gpt_auto=no
```

`systemd.gpt_auto=no` 是 B-029 的预防项：Android 的 ESP 分区会被 systemd-gpt-auto-generator
生成 `boot.automount`，该 automount 挂载失败会拖垮 dbus-broker 的 mount namespace。

- 旧系统参考：`root=UUID=... rootwait rw slot_suffix=_b console=tty0 loglevel=4`（ABL 还会追加 `androidboot.*`）。
- 用 `root=PARTLABEL=linux` 而不是 `UUID=`：分区即便重新创建/重新格式化（UUID 会变）也仍然有效，
  与 `/init` 里"等 `/dev/disk/by-partlabel/linux` 出现"的逻辑同源。
- `console=tty0` + `loglevel=4`：屏幕就是日志通道（设备上没有 adb）。

- **已实测（构建主机）**：`mkfs.f2fs` 1.17.0 的 `-h` 选项表；上述命令可格式化 64 MiB 镜像（`Info: format successful`）；
  `fsck.f2fs -f` 返回 0；`-O compression` / `-O ^compression` 的真实报错。
- **已实测（示例设备快照，D-013）**：`boot_b` → `sde37` / 192 MiB；`linux` → `sda36` / ≈195.5 GiB；CSOT 面板。
  这是当次采集的示例值，块设备号与分区大小因设备而异。
- **已实测（真机，2026-10-24）**：真实内核与 DTB 打包、`fastboot` 刷写、开机到 `/sbin/init`、`resize.f2fs`。
- **已知限制**：镜像结构与 initramfs 内容已核对；新 `boot.img` 的实际刷机结果待验证（见 bug.md B-057/B-058）。
- `linux` 分区的创建（缩减 `userdata`）**永远不由本项目脚本执行**。
