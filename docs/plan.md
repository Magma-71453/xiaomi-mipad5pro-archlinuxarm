# 阶段计划与验收状态（2026-10-24 更新）

> 本文件是**历史阶段记录**。系统当前的真实状态（哪些子系统已在真机验证可用）以仓库根目录的 `README.md` 为准；
> 下文 “PENDING-DEVICE” 清单是刷机之前写的，很多项后来已经通过并记录在 `bug.md`/`docs/audio.md` 等文档里。

| 阶段 | 内容 | 状态 | 证据 |
|------|------|------|------|
| S0 | 仓库、`.gitignore`、`bug.md`、决策记录 D-001..D-017 | 完成 | 提交 S0…S0.7 |
| S1 | 内核交叉编译 + ALARM rootfs（f2fs）+ boot.img + initramfs | 完成（构建侧） | `MANIFEST` 11 条目；`sha256sum -c SHA256SUMS` 全通过；`fsck` 副本 `[Fail]`=0 且 `cmp -l` 仅 4 字节 |
| S2 | 刷入链路（只写 boot_b + linux） | 完成（脚本侧） | `tests/run.sh` 5/5；拒绝矩阵 rc=2；D-009 门控 `erase dtbo_b` 三重校验 |
| S3 | MangoWM 0.18.3（wlroots-0.20 / scenefx-0.5） | 完成（构建+装配） | 设备原生编译；`readelf` NEEDED 31/31 可解析；10 文件落位 rootfs |
| S4 | 性能：schedplace + schedutil/噪声审计 | 完成（代码+单测） | 375 checks；0 warning；`nm -u` 无 sched_setscheduler |
| S5 | FEX-Emu（x86 兼容层） | 延后 | AUR 无 `fex-emu`、官方无预编译资产；需设备上源码构建 |
| S6 | 交付收口（摘要、真机清单） | 进行中 | 本文件与 `bug.md` |

## 真机待验清单（PENDING-DEVICE，未验证前不得宣称通过）

1. 刷入：`fastboot erase dtbo_b`（可选，需 `--allow-dtbo-erase`，D-009）→ `flash linux` → `flash boot_b` → `--set-active=b`（人工，D-014）。
2. 开机：进入 systemd，屏幕点亮（CSOT 面板，面板模块已在 initramfs 内）。
3. 图形：freedreno + a650 固件加载成功，MangoWM 会话可启动（需 seatd 正常工作）。
4. 输入：触摸（novatek）、音量/电源键、霍尔键。
5. 网络：Wi-Fi（ath11k WCN6855）、蓝牙（qca/htbtfw20）。
6. 音频：受 `hexagonrpcd` 缺失限制（B-020），预期无声 —— 需确认是否为唯一原因。
7. 电源：suspend/resume、充电（bq25970）。
8. 存储：首启后 `resize.f2fs` 扩容至 195.5 GiB 分区末尾；`fsck.f2fs` 在真机上的表现。
9. 性能：schedplace 的 threaded cpuset 实测、busy 阈值标定、A/B 测量（帧时间/迁移次数）。

## 洁净度自检（每阶段）

- `scripts/audit-hygiene.sh`：仓库零产物 + MANIFEST SHA256 复校 + FLASH-LOG 审计。
- `scripts/collect-manifest.sh`：由磁盘实际字节生成唯一 `MANIFEST` 与 `SHA256SUMS`，并交叉校验各 `MANIFEST.*` 片段。
