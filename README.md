# elish-archlinux

在 **Xiaomi Mi Pad 5 Pro（`xiaomi-elish`，SM8250）** 上从零搭建的 **Arch Linux ARM（aarch64）** 日常系统：
MangoWM 桌面、8 颗 CS35L41 组成立体声、bq2597x 电荷泵快充、FEX-Emu 跑 x86 应用。

> An Arch Linux ARM (aarch64) system for the Xiaomi Mi Pad 5 Pro (`xiaomi-elish`, SM8250):
> postmarketOS/mainline kernel, MangoWM desktop, stereo speakers driven by 8× CS35L41
amplifiers, bq2597x charge-pump fast charging and an x86 layer via FEX-Emu.
> Only two partitions are ever written: `boot_b` and `linux`.

---

## 硬边界（不可妥协）

1. **只写两个分区**：`boot_b`（内核 + dtb + initramfs）与 `linux`（根文件系统，f2fs / ext4）。
   `scripts/flash-elish.sh` 内置分区白名单，白名单外的 `fastboot flash` 与一切 `fastboot erase` 都会被拒绝。
2. **仓库不跟踪任何二进制**（镜像、`.ko`、`.dtb`、`.tar.zst` 等），产物只落到 `output/`，已被 `.gitignore` 忽略。
3. **缺陷追踪只有一份**：`bug.md`；决策记录在 `docs/decisions.md`。

## 当前状态（真机已验证）

| 子系统 | 状态 | 关键点 |
|---|---|---|
| 引导链 | ✅ | ABL → `boot.img`（`Image.gz` + 零填充 dtb + initramfs）→ f2fs 根分区（`PARTLABEL=linux`） |
| 显示 | ✅ | CSOT 面板 1600×2560@120 Hz、单 mode、`Transform 270`，缩放交给合成器 |
| 触摸 | ✅ | `novatek,NVT-ts-spi`（音量改由 dock / OSD 调节；原 evdev 音量守护已因 CPU 占用移除） |
| 桌面 | ✅ | MangoWM + 自制外壳（mangobar / dock / OSD / 手势 / 屏幕键盘），见 `docs/mango.md` |
| Wi-Fi | ✅ | ath11k WCN6855；含「MAC 全 0 导致接口起不来」的修复（`wlp-mac.service`） |
| 网络 | ✅ | NetworkManager + 公共 DNS 兜底 + `timesyncd`（代理客户端按需自行配置，仓库不含任何订阅/节点信息） |
| 音频 | ✅ 立体声 | 8× CS35L41 走 TDM；内核补丁让每颗功放选对 ASP RX 声道（0=左，2=右） |
| 音量 | ✅ | UCM 固定数字 913/913、模拟 20/20、`PCM Source=DSP`；软件音量 100% 不失真 |
| 充电 | ✅ 快充 | bq2597x 电荷泵 + PD 9 V：电池 0.7 A → **≈2.5 A（约 10 W）**，35 ℃ 上下 |
| 性能 | ✅ | `perf/schedplace` 线程放置守护进程（只做 affinity/cpuset/nice，不碰 governor） |
| x86 应用 | ⚠️ | FEX-Emu 已装可用；thunks / glibc rootfs 仍在打磨（`docs/fex.md`） |

## 仓库结构

```text
scripts/           构建、打包、刷入脚本（可重入，刷入默认 dry-run）
configs/           要落位到产物里的配置
  kernel/          内核 config + 需要打到上游源码的补丁（configs/kernel/patches/）
  initramfs/       静态 busybox init（按 PARTLABEL=linux 挂载根分区）
  rootfs/          直接覆盖进根文件系统的文件树（UCM、systemd unit、/usr/local/bin 脚本…）
  perf/            schedplace 运行配置
  mango-shell/     桌面外壳源码（mangobar 补丁、dock / OSD / 协议代码）
perf/schedplace/   线程放置守护进程（C11，源码 + 单测 + systemd unit）
docs/              设计、验证记录、调研与交接文档
tests/             刷入脚本拒绝矩阵等回归测试
bug.md             缺陷与已知问题（唯一真源）
```

## 从零构建

依赖：`clang` + `lld`（`LLVM=1` 交叉编译）、`zstd`、`python3`、`fastboot`、`e2fsprogs`、`f2fs-tools`。
中间产物落在被忽略的 `.build/`，镜像落在 `output/<YYYYMMDD>/`（同样被忽略）。

```sh
scripts/build-kernel.sh       # 交叉编译 sm8250-7.2.0_git20261010（LLVM=1，out-of-tree）
scripts/build-initramfs.sh    # 静态 busybox initramfs
scripts/build-rootfs.sh       # Arch Linux ARM aarch64 rootfs -> tar.zst + f2fs 镜像（4096 B 扇区）
scripts/make-bootimg.py       # Image.gz + dtb + initramfs -> boot.img
scripts/collect-manifest.sh   # 生成 MANIFEST / SHA256SUMS
scripts/flash-elish.sh --dry-run   # 默认演练，确认后才会真的写分区
```

内核迭代不必重刷根文件系统：把新模块拷进 `/lib/modules/$(uname -r)/...`、`depmod -a`、`modprobe -r/-i` 即可；
只有 `Image` / dtb 变化才需要重刷 `boot_b`。

## 关键实现

### 音频：8 颗功放怎么出立体声
`CS35L41_SP_FRAME_RX_SLOT`（regmap `0x4820`）不是 TDM 槽位号，而是 **ASP RX 声道选择器**：`0` = 左声道，`2` = 右声道。
主线 `sm8250.c` 从不设置它，于是 8 颗功放全按默认播左声道。
`configs/kernel/patches/0001-sm8250-per-codec-tdm-slots.patch` 在 `sm8250_snd_startup()` 里按器件树顺序
（前 4 颗 = i2c3 = 左侧，后 4 颗 = i2c1 = 右侧）为每个 codec DAI 调 `snd_soc_dai_set_channel_map()`，
并暴露 `left_slot` / `right_slot` / `amp_slot_mode` / `tdm_*` / `rx_mask` 等模块参数便于现场调参。
完整推导与走过的弯路见 `docs/audio.md`。

### 充电：为什么需要一个守护进程
`bq2597x` 是 2:1 电荷泵，VBUS 必须 ≈ 2×VBAT；它默认关闭，且与 PM8150B 线性充电器互斥。
`/usr/local/bin/elish-fastcharge` 每 10 秒检查（USB 在线、PD ≥ 8 V、VBAT 3.4–4.35 V、温度 ≤ 42 ℃），
满足则使能电荷泵，否则回退 PM8150B。实测 0.7 A → ≈2.5 A。功率上限分析见 `bug.md` B-038…B-040。

### 性能：schedplace
只做「把某些线程放到某个核簇」，频率与能效仍归内核 schedutil + EAS。
`configs/perf/schedplace.conf` 用 `interval_ms = 2000`、`probe_ms = 1000`：线程很多时（Firefox 常驻上千线程），
过密的事件驱动 `/proc` 扫描会吃满一个核（`bug.md` B-041）。

## 已知限制

- **快充到不了原厂的 20 W / 33 W**：原厂用小米私有 11 V 协议，标准 PD 只有 9 V，2:1 电荷泵余量有限（≈10 W）。
- **内核改动需要重刷 `boot_b`**，根文件系统改动可在线完成；仓库不含任何镜像。
- **没有厂商的 per-amp 保护调音（acdb）**，只用 CS35L41 通用固件。
- **FEX-Emu** 的 thunks / glibc rootfs 仍在打磨，并非所有 x86 应用都能跑。

## ⚠️ 默认凭据

构建脚本会给 `magma` 用户一个**已知的默认口令**（见 `scripts/build-rootfs.sh`），仅用于首次登录调试。
**首次开机后请立即 `passwd` 修改**，并按需替换 `configs/rootfs/usr/share/elish/wifi-profile.nmconnection.template`。
仓库里**不包含**任何真实 Wi-Fi 密码或密钥。

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/plan.md` | 阶段计划（含早期 PENDING-DEVICE 清单，实时状态以本 README 为准） |
| `docs/decisions.md` | 决策记录 D-001… |
| `docs/audio.md` | 音频链路推导、右声道修复、响度调校 |
| `docs/mango.md`、`docs/handoff-mango-shell.md` | 桌面外壳设计、实现与交接边界 |
| `docs/fex.md` | FEX-Emu 构建与安装 |
| `docs/perf.md` | 性能方案（schedplace / schedutil / 噪声审计） |
| `docs/flash-guide.md` | 刷入流程与回滚 |
| `docs/findings/` | 原始调研：rootfs、内核、固件、显示等 |
| `bug.md` | 全部缺陷与修复记录（B-001…） |

## 致谢

- 内核来自 [postmarketOS](https://postmarketos.org/) 的 `linux-postmarketos-qcom-sm8250` 分支；
- 桌面基于 [MangoWM](https://github.com/mangowm/mango)；
- 音频关键线索来自 postmarketOS wiki 上关于 CS35L41 `SP_FRAME_RX_SLOT` 的实测记录；
- 快充拓扑参考 TI bq2597x 与 Qualcomm PM8150B 的公开资料。
