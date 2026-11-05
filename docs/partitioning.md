# 分区详解：用 parted 创建 `linux` 分区

> 本文是 `INSTALL.md` 第 3 章的**展开版**。只想把系统装上的读者按 `INSTALL.md` 走即可；
> 想弄清「为什么这么算」、或者分区已经写坏需要判断怎么救，再看这份。
>
> 适用范围：Xiaomi Mi Pad 5 Pro（`xiaomi-elish`，SM8250），UFS，GPT 分区表，`userdata` 位于磁盘尾部。
> 全文以 `/dev/block/sda`（UFS LUN 0）为例；实际节点以 `readlink -f /dev/block/by-name/userdata` 为准。

---

## 0. 一页速览

| 问题 | 答案 |
|---|---|
| `linux` 分区是出厂自带的吗 | **不是**。必须自己在 `userdata` 尾部腾出空间后创建 |
| 能在主机上分区吗 | 不能。只能在这台设备上、对 `/dev/block/sda` 有写权限的环境里做 |
| 在哪做 | 临时启动的 recovery（OFRP），用 `adb` 进 root shell，跑 push 进去的静态 `parted` |
| 会丢数据吗 | 会。Android 侧 `userdata` 全部失效，且不可逆 |
| 要动几个分区 | 两个：`userdata`（缩小/重建）与新建的 `linux`。其它分区一律不碰 |
| 失败能救吗 | 分区项被删但数据还在盘上 → 按记录的原起止扇区 `mkpart` 重建即可；分区表彻底写坏 → EDL，本项目未验证 |
| 最关键的三个数 | `S`（`userdata` 起始）、`E`（`userdata` 结束）、`N`（`userdata` 分区号）。**先抄在纸上再动手** |

时间：10–20 分钟，其中命令执行只占几秒，其余都是核对。

---

## 1. GPT 与扇区：先把单位搞清楚

### 1.1 磁盘上的 GPT 长什么样

GPT 不是「一张表」，而是**两套表 + 一个保护性 MBR**：

```text
LBA 0           保护性 MBR（告诉老工具"这块盘被占用了"）
LBA 1           主 GPT 头（磁盘 GUID、分区项数组的位置与数量、CRC32）
LBA 2 … 33      主分区项数组（128 项 × 128 字节 = 16 KiB = 32 个 512 B 扇区）
LBA 34 …        第一个可用扇区
…
LBA T-34        最后一个可用扇区
LBA T-33 … T-2  备份分区项数组（32 个扇区）
LBA T-1         备份 GPT 头
```

（`T` = 磁盘总扇区数。上面按 512 B 逻辑扇区画；若逻辑扇区是 4096 B，这些 LBA 号不变，只是每 LBA 变成 4096 字节。）

由此得到两条常被忽略的事实：

1. **扇区 0–33 不可用**（保护 MBR + 主 GPT 头 + 主分区项数组）；
2. **最后 33 个扇区不可用**（备份分区项数组 + 备份 GPT 头）。

所以「能建分区的扇区范围」是 `34 … T−34`。这也是为什么 `T−34` 是 `userdata` 的结束扇区。

> 实测依据：在一份 500118192 扇区的磁盘映像上，`parted` 接受 `4096000s … 500118158s`（= `T−34`）作为最后一个分区，
> 再往后加 1 个扇区就会越界。

### 1.2 三种「扇区」，不要混

排障时 90% 的困惑来自这三种扇区混用：

| 名称 | 单位 | 出现在哪 | 会不会随设备变 |
|---|---|---|---|
| **GPT 逻辑扇区** | 512 B 或 4096 B | `parted print` 的 `Sector size (logical/physical)`、`parted unit s` 的 `s` | **会**。UFS 上两种都可能 |
| **sysfs 扇区** | 恒为 512 B | `/sys/class/block/<dev>/size`、`/proc/partitions` 的换算基准 | 不会，内核固定用 512 B 计价 |
| **fastboot 的"扇区"** | 各机型自定义 | `fastboot getvar partition-size:<part>` 实际返回的是**字节**（十六进制） | 值随分区变，单位是字节 |

由此推出三条实用规则：

1. **`parted unit s` 里的数字只能和 `parted print` 里的数字比**，不要和 `/sys` 的值直接比。
2. `boot_b` 的 393216 扇区 = 192 MiB = `0xC000000` 字节 —— 这个 393216 是**按 512 B 计**的（393216 × 512 = 201326592）。`parted` 若显示 4096 B 逻辑扇区，同一个 `boot_b` 在 `parted print` 里会是 49152s。
3. 判断自己的设备属于哪种，只看一行输出：

```text
Sector size (logical/physical): 512B/4096B      ← 逻辑 512 B，本文算例适用
Sector size (logical/physical): 4096B/4096B     ← 逻辑 4096 B，见 §5.4
```

（`/4096B` 那半边是物理扇区，只影响对齐性能，不影响扇区编号。）

### 1.3 含端点与相邻关系

`Start` 与 `End` 都是**含端点**的扇区号。分区占用的扇区数是：

```text
扇区数 = End − Start + 1
```

相邻两个分区必须满足：

```text
下一个分区.Start = 上一个分区.End + 1
```

少算 1（新分区的 `Start` 正好等于上一个分区的 `End`）→ 重叠，`parted` 报 `Error: You requested a partition from …`。
多算 1（中间留 1 个空扇区）→ 不报错，但浪费 512 B，且容易在核对时被自己搞糊涂。

### 1.4 对齐

`parted` 对未按 1 MiB（2048 个 512 B 扇区）对齐的分区会给警告：

```text
Warning: The resulting partition is not properly aligned for best performance: 400001s % 2048s != 0s
```

- **不阻止创建**，只是闪存写入会跨擦除块，长期性能和寿命略差；
- 消除办法：让 `Start` 是 2048 的整数倍（§5.3 给了取整公式）；
- `userdata` 的 `Start` 通常天生对齐（示例 `4096000s` = 2000 MiB，÷2048 = 2000），所以只需要管 `linux` 的 `Start`。

---

## 2. 为什么必须在设备侧的 recovery 里做

| 方案 | 可行性 | 原因 |
|---|---|---|
| 主机上 `parted` | ❌ | 主机看不到设备的 UFS。`/dev/block/sda` 是设备内核里的节点，不是主机上的块设备 |
| `fastboot` 命令 | ❌ | fastboot 协议只有 `flash`/`erase`/`getvar` 等，**没有编辑分区表**的操作 |
| Android 运行时 `parted` | ❌ | `userdata` 被挂载中，`parted` 拒绝写；且普通 `adb shell` 是 `shell` 用户，没有块设备权限 |
| **recovery（OFRP）+ root adb + 静态 parted** | ✅ | 没有任何分区被业务挂载，adbd 以 root 运行，`parted` 能直接操作 `/dev/block/sda` |
| 刷第三方「分区脚本」zip | ⚠️ | 脚本行为不可审计，可能顺手 `format`/`wipe` 别的分区。**不要用** |

recovery 里没有 `/usr/bin`（那是 Arch 的布局），工具在 `/system/bin`、`/sbin`，PATH 里都有；`parted` 用你 push 进去的 `/cache/parted`。所以**这一章的命令用裸命令名，只有 parted 写绝对路径**。

> 反过来：不要在 Android 系统里装「分区助手」类 App，也不要在 fastboot 里 `fastboot flash gpt <文件>`。
> 后者会用一整份分区表覆盖当前表，等于把 Android 的 a 槽布局也一起重写，属于本项目明确禁止的操作。

---

## 3. 环境准备

一次性清单（做完打勾）：

| # | 准备项 | 命令 / 动作 | 通过标准 |
|---|---|---|---|
| 1 | recovery 镜像 | 下载 OFRP for elish，放到 `/srv/elish/recovery.img` | 文件存在且非 0 字节 |
| 2 | 临时启动 recovery | `/usr/bin/fastboot boot /srv/elish/recovery.img` | 屏幕出现 OrangeFox 界面 |
| 3 | 备用方案（可选） | `/usr/bin/fastboot flash boot_b /srv/elish/recovery.img` | 重启进 recovery；事后必须刷回 `boot.img` |
| 4 | adb 可见 | `/usr/bin/adb devices` | 列出 `<device-serial>	recovery` |
| 5 | adb 是 root | `/usr/bin/adb shell id` | `uid=0(root) gid=0(root)` |
| 6 | 静态 parted | `/usr/bin/adb push /srv/elish/parted /cache/parted` 后 `chmod 755` | `/cache/parted --version` 有输出 |
| 7 | `/data` 已卸载 | `/usr/bin/adb shell "umount /sdcard; umount /data"` | `mount \| grep ' /data '` 无输出 |
| 8 | 磁盘节点 | `/usr/bin/adb shell "readlink -f /dev/block/by-name/userdata"` | 形如 `/dev/block/sda36` |

两个容易踩的点：

**parted 必须是静态链接的 aarch64 二进制。** 动态链接版在 Android 上会报 `no such file or directory` 或 `CANNOT LINK EXECUTABLE`——它找不到 glibc 的 `ld-linux-aarch64.so.1`。确认方法：

```sh
/usr/bin/file /srv/elish/parted
```

期望 `ELF 64-bit LSB executable, ARM aarch64, ... statically linked`。

**`parted` 版本决定 `mkpart` 的第一个参数是什么。**

| `parted --version` | `mkpart` 语义 | 建完要不要补 `name` |
|---|---|---|
| 3.x（绝大多数 recovery） | `mkpart NAME [FS-TYPE] START END`，第一个参数**就是分区名** | 不需要 |
| 2.x（老 recovery） | `mkpart PART-TYPE [FS-TYPE] START END`，第一个参数是 `primary`/`logical` | **必须**：`name <编号> linux` |

不确定就跑 `parted --version`。本文命令按 3.x 写。

---

## 4. 只读侦察与记录

### 4.1 三条只读命令

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"
/usr/bin/adb shell "/cache/parted /dev/block/sda print"
/usr/bin/adb shell "cat /proc/partitions"
```

第一条给精确扇区号，第二条给人类可读容量（用来交叉核对），第三条给内核当前看到的分区列表（用来对照「磁盘上的表」与「内核里的视图」是否一致）。

### 4.2 输出解读

```text
Model: SCSI DISK (scsi)
Disk /dev/block/sda: 500118192s
Sector size (logical/physical): 512B/4096B
Partition Table: gpt
Disk Flags:

Number  Start       End         Size        File system  Name      Flags
 1      8192s       131071s     122880s                  ...
34      4096000s   500118158s  496022159s               userdata
```

| 字段 | 含义 | 要注意什么 |
|---|---|---|
| `Model` | 设备型号字符串 | 无关 |
| `Disk …: Ns` | 总扇区数 `T`（`unit s` 模式下带 `s`） | 末尾可用扇区 = `T − 34` |
| `Sector size` | 逻辑/物理扇区 | 决定 `s` 是多少字节（§1.2） |
| `Partition Table: gpt` | 分区表类型 | 若是 `msdos`/`unknown`，**立刻停止**，说明走错设备或表已损坏 |
| `Number` | 分区号 | `rm`/`name`/`resizepart` 用它 |
| `Start`/`End` | 含端点的扇区号 | 记录、计算 |
| `Size` | 扇区数 | 与 `End − Start + 1` 对得上才说明没抄错 |
| `File system` | **探测到的**文件系统 | 空 = 探测不到（加密/未格式化/内容被清）；与 GPT 里的「类型提示」无关 |
| `Name` | GPT 分区名 | 内核 `PARTLABEL=linux` 靠它 |
| `Flags` | 标志位（`boot`/`esp`/…） | 不要给 `linux` 加任何 flag |

### 4.3 记录模板（建议照抄到纸上）

```text
设备/日期：____________________
磁盘节点  /dev/block/sda        总扇区 T = ____________
逻辑扇区  ______ B              （512 或 4096）

userdata：Number N = ____
          Start  S = ____________s
          End    E = ____________s
          Size      = ____________s

保留给 Android：______ GiB
新 userdata End U = ____________s
linux   Start  L = ____________s
linux   End      = E = ____________s
linux   预计字节 = ____________
```

### 4.4 边界值核对

在动手前先做三条自检，能把大部分算错提前拦住：

1. `E = T − 34`？若 `userdata` 的 `End` 明显小于 `T − 34`，说明磁盘尾部还有别的分区（例如 `metadata`、厂商分区），**不要假设 `userdata` 是最后一个**——此时 `linux` 的结束扇区不能直接用 `E`，而要沿用 `userdata` 原来的 `End`（本文公式本来就是用 `E`，所以没问题），但「磁盘尾部没被占满」这件事要在 `print` 里确认清楚。
2. `E − S + 1` 与 `Size` 列相等？
3. `S` 是 2048 的整数倍？（示例 `4096000` ÷ 2048 = 2000，天然对齐）

---

## 5. 容量规划与扇区计算

### 5.1 约束

| 约束 | 数值 | 来源 |
|---|---|---|
| `linux` 最小容量 | **16 GiB** | rootfs 镜像构建时固定 16 GiB，分区小于镜像则 `fastboot flash` 失败 |
| `linux` 建议容量 | ≥ 32 GiB | 首启扩容后才有可用空间 |
| `userdata` 最小容量 | 建议 ≥ 8 GiB | Android 本身仍要能格式化、能开机 |
| 不能越界 | `End ≤ T − 34` | 备份 GPT 必须留在原处 |

### 5.2 公式

以 512 B 逻辑扇区为例（1 GiB = 2097152 扇区）：

```text
KiB_PER_GIB   = 1024 × 1024 = 1048576
SECTORS_PER_GIB = 1048576 ÷ (逻辑扇区字节数)

U = S + 保留GiB × SECTORS_PER_GIB − 1        # 新 userdata 结束
L = U + 1                                     # linux 起始
linux 结束 = E
linux 扇区数 = E − L + 1
linux 字节数 = linux 扇区数 × 逻辑扇区字节数
```

| 逻辑扇区 | 1 GiB 的扇区数 | 32 GiB 的扇区数 | 64 GiB 的扇区数 |
|---|---|---|---|
| 512 B | 2097152 | 67108864 | 134217728 |
| 4096 B | 262144 | 8388608 | 16777216 |

### 5.3 对齐取整

想让 `L` 落在 1 MiB 边界上：

```text
L = ceil((U + 1) / 2048) × 2048
   = ((U + 1 + 2047) ÷ 2048) × 2048      # 整数除法
U = L − 1
```

`U` 变小几个扇区，`userdata` 少几 KiB，可以忽略。

### 5.4 算例

**算例 A：256 GB 机型，逻辑扇区 512 B，给 Android 留 32 GiB**

| 步骤 | 计算 | 结果 |
|---|---|---|
| `S` | 来自 `print` | 4096000 |
| `E` | 来自 `print` | 500118158 |
| `T` | 来自 `print` | 500118192 |
| 边界自检 | `T − 34` | 500118158 ✅ |
| 32 GiB 的扇区数 | 32 × 2097152 | 67108864 |
| `U` | 4096000 + 67108864 − 1 | **71204863** |
| `L` | 71204863 + 1 | **71204864** |
| 对齐 | 71204864 ÷ 2048 | 34768 ✅ |
| `linux` 扇区数 | 500118158 − 71204864 + 1 | 428913295 |
| `linux` 字节数 | 428913295 × 512 | 219603607040 |
| `linux` 容量 | 219603607040 ÷ 1024³ | **204.52 GiB** |

**算例 B：128 GB 机型，逻辑扇区 512 B，给 Android 留 32 GiB**

| 步骤 | 计算 | 结果 |
|---|---|---|
| `T` | 典型 128 GB UFS | 250069680 |
| `E` | `T − 34` | 250069646 |
| `U` | 4096000 + 67108864 − 1 | 71204863 |
| `L` | `U + 1` | 71204864 |
| `linux` 扇区数 | 250069646 − 71204864 + 1 | 178864783 |
| `linux` 容量 | 178864783 × 512 ÷ 1024³ | **85.29 GiB** |

**算例 C：256 GB 机型，逻辑扇区 512 B，给 Android 留 64 GiB**

| 步骤 | 计算 | 结果 |
|---|---|---|
| `U` | 4096000 + 134217728 − 1 | 138313727 |
| `L` | `U + 1` | 138313728（÷2048 = 67536 ✅） |
| `linux` 扇区数 | 500118158 − 138313728 + 1 | 361804431 |
| `linux` 容量 | 361804431 × 512 ÷ 1024³ | **172.52 GiB** |

**算例 D：逻辑扇区 4096 B 的假设设备，总扇区 62514774，留 32 GiB**

| 步骤 | 计算 | 结果 |
|---|---|---|
| `T` | 假设值 | 62514774 |
| `E` | `T − 34` | 62514740 |
| 32 GiB 的扇区数 | 32 × 262144 | 8388608 |
| `U` | 512000 + 8388608 − 1 | 8900607 |
| `L` | `U + 1` | 8900608 |
| `linux` 扇区数 | 62514740 − 8900608 + 1 | 53614133 |
| `linux` 容量 | 53614133 × 4096 ÷ 1024³ | **204.52 GiB** |

对比 A 与 D：**同样的 256 GB 磁盘、同样留 32 GiB，两种扇区大小下 `U`/`L` 的数值完全不同（71204863 vs 8900607），但最终容量一致**。这就是为什么「照抄别人的扇区号」一定会出事。

**算例 E（反例）：把 A 的扇区号用在 4096 B 设备上**

`mkpart linux f2fs 71204864s 500118158s` 在总扇区 62514774 的设备上会被 `parted` 直接拒绝：

```text
Error: The location 500118158s is outside of the device /dev/block/sda.
```

→ 这是好事：越界会被拦。真正危险的是**方向相反**的那种错：把 4096 B 设备的扇区号（同一个物理位置上，它只有 512 B 扇区号的 1/8）用到 512 B 设备上，`parted` 不会报错，但 `linux` 只有预期的八分之一——装到一半才发现太小。所以务必用目标设备 `print` 出来的数字重算。

### 5.5 四个常见算术错误

| 错误 | 表现 | 后果 |
|---|---|---|
| `U = S + 保留GiB × 2097152`（忘了 −1） | 每个边界都偏 1 个扇区 | **不会报错**：`userdata` 多 1 扇区、`linux` 少 1 扇区。只有核对表能发现（这也是核对表存在的理由） |
| `L = U`（忘了 +1） | `linux.Start` = `userdata.End` | 重叠，直接报错 |
| `linux` 结束写成 `T`（而不是 `T−34`） | 越界 | `Error: The location …s is outside of the device …` |
| 4096 B 设备沿用 512 B 的 `2097152` | 保留空间变成 1/8 | 不报错，但 `userdata` 只剩 4 GiB、Android 起不来 |

---

## 6. 文件系统收缩（可选分支）

> 默认路径**不做**这一节：直接重建分区，接受 Android 数据丢失。
> 只有想保留 Android 数据（双系统共存）时才需要读。

### 6.1 先判定 `userdata` 是什么

```sh
/usr/bin/adb shell "blkid /dev/block/sda36"
```

| 输出 | 判定 | 能缩吗 |
|---|---|---|
| `TYPE="ext4"` | 明文 ext4 | ✅ 可以（§6.2） |
| `TYPE="f2fs"` | f2fs | ❌ f2fs 没有 shrink 工具 |
| 空 | 未格式化，或被 metadata encryption / FBE 包着（裸设备上是密文） | ❌ `resize2fs` 读到的是密文，会报 `Bad magic number in super-block` |
| `Permission denied` | 不是 root shell | 回 §3 第 5 项 |

**为什么加密的 `userdata` 不能缩**：现代小米平板启用 metadata encryption（`dm-default-key`）+ 文件级加密（FBE）。文件系统挂在 dm 设备上，而**裸分区 `sdaNN` 上存的是密文**。`e2fsck`/`resize2fs` 直接读裸分区，看到的不是 ext4 超级块，因此必然失败——不是工具不好用，是层次不对。

### 6.2 明文 ext4 的收缩流程

**顺序不可颠倒**：先缩文件系统，再缩分区。反过来文件系统会超出分区边界。

```sh
# 1) 强制检查（-f 即使干净也检查；-y 自动回答 yes，但要看清它改了什么）
/usr/bin/adb shell "e2fsck -f -y /dev/block/sda36"

# 2) 缩到比目标分区略小（目标 32 GiB → 缩到 31G，留 1 GiB 余量）
/usr/bin/adb shell "resize2fs /dev/block/sda36 31G"

# 3) 复核
/usr/bin/adb shell "dumpe2fs -h /dev/block/sda36 | head -20"
```

| 步骤 | 期望 | 失败时 |
|---|---|---|
| `e2fsck` | 以 `… clean, N/N free blocks` 之类结尾，退出码 `0`（干净）或 `1`（已修正） | 退出码 `≥2` 就是错误，必须先修好，**不要**带错误继续缩 |
| `resize2fs` | `The filesystem on /dev/block/sda36 is now N (4k) blocks long.` | 报 `Nothing to do!` = 已经比目标小；报 `Bad magic number` = 加密或走错设备 |
| `dumpe2fs` | `Block count` × `Block size` ≤ 32 GiB | 超了说明第 2 步没生效 |

代价与风险：

- `resize2fs` 的收缩**不可逆地移动数据块**；中途断电可能损伤文件系统；
- 缩完再扩回去（`resize2fs <dev>` 不带尺寸）数据通常还在，但**没有备份就不要做**；
- 收缩后 Android 首次开机会走一次 `fsck`，可能需要几分钟。

### 6.3 收缩之后 Android 会怎样

即使文件系统缩成功了，Android 还要面对「分区变小 + 加密元数据可能对不上」：

- `metadata` 分区里存着加密密钥材料，分区几何变了不一定会自动适配；
- 多数情况下 Android 会进 recovery 提示「需要清除数据」；
- 想让它正常开机，通常仍要在 recovery 里 **Format Data**——也就是说**收缩并不能真正保住数据**，只是让「后悔药」多一个选项。

结论：**除非你明确知道自己要双系统共存，否则跳过本节。** 直接重建分区更干净。

---

## 7. 执行分区

### 7.1 前置确认（每条都要过）

```sh
/usr/bin/adb shell "mount | grep -E ' /data | /sdcard '"
/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"
```

- 第一条必须**无输出**（`/data` 未挂载）；
- 第二条的 `Partition Table` 必须是 `gpt`。

### 7.2 交互模式逐条执行

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda"
```

进入 `(parted)` 提示符后：

```text
(parted) unit s
(parted) print
```

核对 `N`/`S`/`E` 与纸上记录一致。

```text
(parted) rm 34
```

| 项 | 说明 |
|---|---|
| 作用 | 删除 34 号分区的 **GPT 表项**，不写数据区 |
| 回显 | 成功时**没有任何输出** |
| 失败 | `Error: Partition doesn't exist.` → 分区号错 |
| 数据 | 分区内容仍在盘上，只是没人指向它。**这就是救援的立足点** |

```text
(parted) mkpart userdata ext4 4096000s 71204863s
```

| 项 | 说明 |
|---|---|
| 参数 1 | 分区名（parted 3.x），写 `userdata` |
| 参数 2 | 文件系统类型**提示**，`ext4` 或 `f2fs` 都行，不影响实际内容 |
| 参数 3/4 | 起始/结束扇区，**带 `s` 后缀** |
| 回显 | 成功时无输出；可能有对齐警告 |
| 注意 | 分区号由 parted 分配（会复用最小可用编号），**不要假设还是 34**，用 `print` 看 |

```text
(parted) print
```

确认新 `userdata` 的编号与起止扇区。若 `Name` 列为空（parted 2.x 的 `mkpart primary` 写法），补：

```text
(parted) name 34 userdata
```

```text
(parted) mkpart linux f2fs 71204864s 500118158s
```

| 项 | 说明 |
|---|---|
| 参数 1 | 分区名 `linux`（**大小写敏感**，必须全小写） |
| 回显 | 成功时无输出 |
| 若报 `invalid token: f2fs` | 你的 parted 不认这个类型名，换成 `ext4`（只是提示），不影响后续 |
| 若报 `You requested a partition from …` | 起止与已有分区冲突，回 §5 重算，**不要接受 "closest location"** |

```text
(parted) print
```

最终核对（全教程最关键的一张表）：

| 检查 | 期望 |
|---|---|
| 分区总数 | 比改动前多 1 |
| `userdata` `Start` | = `S` |
| `userdata` `End` | = `U` |
| `linux` `Start` | = `L` |
| `linux` `End` | = `E` |
| `linux` `Name` | `linux` |
| 相邻 | `userdata.End + 1 = linux.Start` |
| `linux.Size` | = `E − L + 1` |
| 末尾 | `linux.End ≤ T − 34` |

```text
(parted) quit
```

`quit` **不是保存**——`parted` 的每条命令都立即写盘。`quit` 只是退出。

可能出现的收尾提示：

```text
Information: You may need to update /etc/fstab.
```

与本项目无关（根分区按 `PARTLABEL` 挂载，不写 UUID），忽略。

### 7.3 脚本模式（可选）

命令多的时候可以用 `-s` 非交互执行，一条一条来（**不要**写成一个 `&&` 链——中间失败会静默跳过后续）：

```sh
/usr/bin/adb shell "/cache/parted -s /dev/block/sda unit s rm 34"
/usr/bin/adb shell "/cache/parted -s /dev/block/sda unit s mkpart userdata ext4 4096000s 71204863s"
/usr/bin/adb shell "/cache/parted -s /dev/block/sda unit s mkpart linux f2fs 71204864s 500118158s"
/usr/bin/adb shell "/cache/parted -s /dev/block/sda unit s print"
```

每执行一条，**下一条之前先 `print` 一次**。脚本模式不会给你「确认一次」的机会，出了错就是直接落盘。

### 7.4 写入时机（为什么没有「撤销」）

`parted` 没有事务、没有日志、没有 undo。每条命令执行完就 `commit` 到磁盘。这一点已用两个独立进程验证过：一个进程里 `rm 1`，另一个进程 `print` 就已经看不到该分区。

所以操作纪律是：

1. 数字先写纸上；
2. 一次只输入一条命令；
3. 每条之后 `print`；
4. 任何一条命令的回显与纸上数字对不上，立刻 `quit` 重来，不要在提示符上「试试看」。

---

## 8. 验证（四层）

单看一层都可能被骗（内核缓存、ueventd 未刷新、标签写错）。四层都过才算真的成功。

### 第 1 层：磁盘上的 GPT

recovery 里：

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"
```

看 `Number`/`Start`/`End`/`Name` 与 §7.2 的核对表一致。

### 第 2 层：内核视图

```sh
/usr/bin/adb shell "cat /proc/partitions"
```

期望出现新编号的分区（如 `sda34`、`sda35`），大小（1 KiB 块）与 `Size × 扇区字节数 ÷ 1024` 接近。

若 `parted` 报了 `… have been unable to inform the kernel of the change`，这一层会看到旧的分区列表——**重启即可**，GPT 本身是对的。recovery 里通常没有 `partprobe`，不要在这上面浪费时间。

### 第 3 层：by-name 链接（仅 Linux 启动后）

```sh
/usr/bin/ls -l /dev/disk/by-partlabel/
/usr/bin/ls -l /dev/block/by-name/       # Android/recovery 侧的等价物
```

期望 `linux -> ../../sdaNN`、`userdata -> ../../sdaNN`。

- recovery 里的 `by-name` 是 ueventd 在**启动时**按 GPT 建的，改完分区不重启不会更新；
- Arch 里的 `by-partlabel` 是 udev 建的，同理。

### 第 4 层：fastboot（权威）

```sh
/usr/bin/fastboot getvar partition-size:linux
/usr/bin/fastboot getvar partition-size:userdata
```

GPT 由 bootloader 直接读取，不受内核缓存影响，所以这是最可信的一层。

```text
partition-size:linux: 0x3321651E00
partition-size:userdata: 0x800000000
```

十六进制转字节核对：

```sh
printf '%d\n' 0x3321651E00      # 219603607040
printf '%d\n' 0x800000000       # 34359738368
```

| 分区 | 期望字节 | 对应 |
|---|---|---|
| `linux` | `(E − L + 1) × 扇区字节数` | 算例 A：428913295 × 512 = 219603607040 |
| `userdata` | `(U − S + 1) × 扇区字节数` | 算例 A：67108864 × 512 = 34359738368（正好 32 GiB） |

对不上：不要继续刷写，回 §5 重算并重做分区。

---

## 9. 回滚到 Android

三种目标，代价完全不同。**先想清楚要哪一个再动手。**

| 目标 | 需要做什么 | Linux 数据 | Android 数据 |
|---|---|---|---|
| A. 只想 Android 能开机 | recovery 里 Format Data | 保留（分区还在） | 丢 |
| B. 把 `linux` 的空间还给 Android | 删 `linux` + `userdata` 扩回末尾 + Format Data | 丢 | 丢 |
| C. 完全恢复出厂 | 刷小米官方 fastboot 包 | 丢 | 丢（重新初始化） |

### 9.1 目标 A：只让 Android 能开机

`userdata` 被重建过，原文件系统对不上新分区，Android 会卡在开机/恢复模式。处理：

1. 进 recovery（`fastboot boot /srv/elish/recovery.img`）；
2. Wipe → **Format Data**（只格式化 data，不要选其它分区）；
3. 重启，等 Android 走完开机向导。

`boot_a`、`super`、`modem` 都没被动过，所以 Android 本身是完好的，差的只是用户数据。

### 9.2 目标 B：把空间还给 Android（保留分区表，删掉 Linux）

1. 进 recovery，卸载 `/data`：

```sh
/usr/bin/adb shell "umount /sdcard; umount /data"
```

2. 记下 `linux` 的 `Start`/`End` 与 `userdata` 的 `Start`（万一要撤销）：

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"
```

3. 删掉 `linux`，把 `userdata` 扩回原来的末尾 `E`：

```sh
/usr/bin/adb shell "/cache/parted -s /dev/block/sda unit s rm 35"
/usr/bin/adb shell "/cache/parted -s /dev/block/sda unit s resizepart 34 500118158s"
```

（`35`/`34`/`500118158s` 换成你的实际值。`resizepart` 的第二个参数是**新的结束扇区**。）

4. 核对：

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda unit s print"
```

期望只有 `userdata`，且 `End` = `E`（= `T − 34`）。

5. 同一个 recovery 里 Wipe → **Format Data**（菜单不可用就重启回 recovery 再做）；
6. 重启进 Android。

> 已实测：在一份 500118192 扇区的磁盘映像上（`userdata` 是 1 号、`linux` 是 2 号），`rm` 掉 `linux` 后
> `resizepart 1 500118158s` 能把 `userdata` 恢复成 `4096000s … 500118158s`（`Size` = 496022159s），与出厂布局一致。

### 9.3 目标 C：刷官方包恢复出厂

小米官方的 fastboot 包会**重写全部分区**（含 `super`、`boot_a`、`dtbo`、`userdata`），等于整盘重置。

- 这一步**会覆盖 `linux` 分区**，Linux 侧数据全丢；
- 它属于「放弃 Linux」的路径，与本项目「只写两个分区」的约束冲突，本文不提供命令；
- 刷之前先把 `linux` 里要留的东西拷出来（设备能开机时用 `scp`，不能开机时只能在 recovery 里挂载后拷）。

### 9.4 误删分区项的救援（重要）

`rm` 只删 GPT 表项，**不写数据区**。所以：

```text
误删 linux 分区项 → 用 mkpart 按原 Start/End 重建同名分区 → 数据通常还能挂回来
```

前提与步骤：

1. **必须记过原 `Start`/`End`**（这就是 §4.3 那张记录表的用途）；
2. 重建时起止扇区**必须完全一致**，差一个扇区都可能挂不上；
3. 重建后**不要再格式化**，先试着挂载：

```sh
/usr/bin/adb shell "mkdir -p /tmp/mnt && mount -t f2fs /dev/block/sda36 /tmp/mnt"
```

（`f2fs` 换成实际文件系统；recovery 的 toybox `mount` 支持 `-t f2fs` 与否取决于 recovery 内核。）

4. 挂上了就先把数据拷出来，再考虑下一步。

同理适用于误删 `boot_b`：按原起止重建后，`boot_b` 里的 `boot.img` 还在，可以直接启动。

---

## 10. 失败场景手册

每条都是「现象 → 原因 → 处理」。`text` 代码块里的报错是 `parted` 的真实输出文本（已在 parted 3.8 上复现）。

### F1 `Error: Could not stat device /dev/block/sda - No such file or directory.`

- **原因**：磁盘节点写错，或 recovery 里该 LUN 不叫 `sda`。
- **处理**：`readlink -f /dev/block/by-name/userdata` 重新确认磁盘节点；`ls /dev/block/ | grep -E '^sd'` 看有哪些。

### F2 打开设备被拒（`Permission denied` 一类）

- **原因**：不在 root shell（`adb shell id` 显示 `uid=2000`）。
- **处理**：回 §3 第 5 项。OFRP 设置里开启 root adb；或换支持 root adb 的 recovery。

### F3 `Error: /dev/block/sda36: unrecognised disk label`

- **原因**：把**分区**当成了**磁盘**（`sda36` 是分区，`sda` 才是磁盘），或该处确实是空。
- **处理**：去掉末尾数字，用 `/dev/block/sda`。若 `sda` 也报这个错，说明 GPT 真的坏了 → 见 F9。

### F4 分区表写入被内核拒绝

```text
Error: Partition(s) … have been written, but we have been unable to inform the kernel
of the change, probably because it/they are in use. As a result, the old partition(s)
will remain in use. You should reboot now before making further changes.
```

- **原因**：`/data`（或别的分区）还挂着，内核不肯重读分区表。
- **处理**：**磁盘上的 GPT 已经写好了**。立刻 `reboot`，重启后再验证。不要在未重启时反复改分区表。
- **验证**：重启后 `cat /proc/partitions` 应出现新分区。

### F5 `Error: Partition doesn't exist.`

- **原因**：`rm`/`name`/`resizepart` 的编号不存在（抄错号，或前面的 `rm` 已经把它删了）。
- **处理**：`print` 看当前编号再操作。**不要凭记忆猜编号。**

### F6 `Error: Can't have the end before the start! (start sector=… length=…)`

- **原因**：起止写反（`Start > End`）。
- **处理**：按 `L` → `E` 的顺序重输。

### F7 `Error: You requested a partition from …s to …s (sectors a..b). The closest location we can manage is …`

- **原因**：请求的范围与已有分区重叠（最常见：`L` 写成 `U`，少加了 1）。
- **处理**：**不要**接受 "closest location"——它会建出一个不是你想要的边界的分区。`print` 看实际空闲区间，回 §5 重算。

### F8 `Error: The location …s is outside of the device /dev/block/sda.`

- **原因**：结束扇区超过了 `T − 34`（或扇区单位搞错，把 4096 B 的算例用在 512 B 设备上）。
- **处理**：核对 `T`、`E`、扇区大小。回 §5。

### F9 备份 GPT 不在末尾

```text
Warning: Not all of the space available to /dev/block/sda appears to be used,
you can fix the GPT to use all of the space (an extra N blocks) or continue
with the current setting?
```

- **原因**：分区表被别的工具改过（例如整盘镜像写入、或磁盘容量与 GPT 记录不一致）。
- **处理**：输入 `Fix`，让 `parted` 把备份 GPT 移到磁盘末尾；`Ignore` 只读不改。改完必须重启。

### F10 对齐警告

```text
Warning: The resulting partition is not properly aligned for best performance: 400001s % 2048s != 0s
```

- **原因**：起始扇区不是 2048 的整数倍。
- **处理**：可继续（性能略差）；想干净就 `rm` 掉重算（§5.3）。

### F11 误敲 `mklabel`

```text
Warning: The existing disk label on /dev/block/sda will be destroyed and all
data on this disk will be lost. Do you want to continue?
```

- **原因**：`mklabel gpt` 会重建**整张分区表**，所有分区项消失。
- **处理**：看到这个提示立刻输 `n`/`No`。若已经确认执行了：数据区还在，但所有分区项都没了，需要按原样逐个 `mkpart` 重建——**这就是为什么本文反复强调先记录**。没有完整记录时，基本只能靠 EDL/官方包恢复。

### F12 误删了 `boot_b` / `super` / `metadata` 等分区项

- **原因**：`rm` 时编号抄错。
- **处理**：立刻停止一切写操作。用记下的原 `Start`/`End` `mkpart` 重建同名分区，**不要格式化**。内容通常还在。
- 若没有记录：`parted` 无法反推原边界，只能靠官方包/EDL。**这是本节最贵的一课。**

### F13 `linux` 建成了别的名字

- 现象：fastboot 里 `getvar partition-size:linux` 报变量不存在；Linux 启动停在 initramfs 等 `PARTLABEL=linux`。
- **处理**：

```sh
/usr/bin/adb shell "/cache/parted /dev/block/sda name 35 linux"
```

改名后重启（让 udev/ueventd 重建链接），再验证。

### F14 建完看不到 `/dev/block/by-name/linux`

- **原因**：正常现象。ueventd 只在启动时读 GPT。
- **处理**：重启后复查；或用第 4 层（fastboot `getvar`）验证，那一层不依赖内核。

### F15 `fastboot flash linux` 报找不到分区

- **原因**：GPT 里没有 `linux`（没建、建错名、或建在了另一个 LUN）。
- **处理**：`getvar partition-size:linux` 确认；没有值就回 §7 重做。

### F16 `linux` 小于 16 GiB

- **现象**：`fastboot flash linux` 报空间不足；或刷进去后 `resize.f2fs` 无事可做。
- **原因**：保留给 `userdata` 的空间太多，或算错扇区。
- **处理**：`rm` 掉 `linux` 与 `userdata`，重新规划（`linux` ≥ 16 GiB，建议 ≥ 32 GiB）。

### F17 两个分区重叠

- **现象**：`print` 里上一行 `End` ≥ 下一行 `Start`。
- **原因**：`+1`/`−1` 算错。
- **处理**：`rm` 后建的那个，重算后重建。重叠状态下**不要**刷写任何东西。

### F18 写到一半断电

- **现象**：再次进 recovery 时 `parted print` 报主/备 GPT 不一致，或直接 `unrecognised disk label`。
- **处理**：先 `print` 让它提示修复（`Fix`）；修不好就按 F11 的思路重建分区表。数据区通常还在，但需要完整记录才能恢复。

### F19 4096 B 扇区设备按 512 B 算

- **现象**：`linux` 容量只有预期的 1/8（不报错），或 `mkpart` 越界（报错）。
- **原因**：1 GiB 用了 2097152 而不是 262144。
- **处理**：回 §5.2 用目标设备的扇区大小重算。**这一类错误不会自己暴露**，只能靠 §8 第 4 层核对字节数。

### F20 建完分区但 Android 起不来（预期行为）

- **现象**：Android 卡在开机动画/恢复模式。
- **原因**：`userdata` 分区几何变了，原文件系统与加密元数据对不上。
- **处理**：recovery 里 Wipe → **Format Data**（§9.1）。**这不是故障**，是分区重建的必然结果。

---

## 11. 附录

### 11.1 `parted` 命令速查

| 目的 | 交互式 | 脚本式 |
|---|---|---|
| 看分区表（扇区） | `unit s` + `print` | `parted -s <dev> unit s print` |
| 看分区表（容量） | `print` | `parted -s <dev> print` |
| 删分区项 | `rm 34` | `parted -s <dev> rm 34` |
| 建分区（带名字） | `mkpart linux f2fs 71204864s 500118158s` | 同左，前面加 `-s <dev>` |
| 改名字 | `name 35 linux` | `parted -s <dev> name 35 linux` |
| 改结束扇区 | `resizepart 34 500118158s` | `parted -s <dev> unit s resizepart 34 500118158s` |
| 修备份 GPT | `print` → `Fix` | 不支持（必须交互） |
| 退出 | `quit` | — |

注意：**`quit` 不是保存**；`parted` 每条命令立即写盘。

### 11.2 记录表（打印出来填）

```text
日期：__________   设备：__________   面板：CSOT / BOE
recovery：__________   parted 版本：__________

磁盘节点：/dev/block/sda    逻辑扇区：______ B
总扇区 T = ____________     末尾可用 = T − 34 = ____________

原 userdata：N=____  S=____________s  E=____________s  Size=____________s
保留 GiB：______    新 U=____________s   L=____________s
预计 linux 字节：____________（= (E−L+1) × 扇区字节数）

parted print 核对：userdata S/E ✅❌   linux S/E ✅❌   Name ✅❌
fastboot 核对：linux = 0x__________ → ______ 字节  ✅❌
                 userdata = 0x__________ → ______ 字节  ✅❌
```

### 11.3 未验证清单（诚实标注）

| 编号 | 事项 | 状态 |
|---|---|---|
| P-1 | 目标设备 GPT **逻辑扇区到底是 512 B 还是 4096 B** | 未实测。本文两套算例都给了，以 `parted print` 为准 |
| P-2 | 全新（未擦 `dtbo_b`）设备按本文流程能否一次启动 | 未验证；测试设备的 `dtbo_b` 已为空。见 `INSTALL.md` 4.5 |
| P-3 | 加密 `userdata` 上 `resize2fs` 的真实报错文本 | 由机理推断为 `Bad magic number`，未在真机复现 |
| P-4 | 分区表损坏后的 EDL（9008）救援 | 本项目未验证，按不可逆处理 |
| P-5 | `parted` 在 OFRP 里的具体版本与 `f2fs` 类型名支持 | 未确认；不认就用 `ext4` |
| P-6 | 重建 `userdata` 后 Android 是否需要连 `metadata` 一起清 | 未验证；本文只建议 recovery 的 Format Data |

---

*相关文档：`INSTALL.md` 第 3 章（操作流程）、`docs/flash-guide.md`（只读核对与刷入序列）、
`docs/research/flash-layout.md` §2.1（上游 Armbian 的分区做法与出处）。*
