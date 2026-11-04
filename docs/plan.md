# 阶段计划与验收状态（2026-10-24 制定）

> 本文件是**历史阶段记录**。系统当前的真实状态（哪些子系统已在真机验证可用）以仓库根目录的 `README.md` 为准；
> 下文 “PENDING-DEVICE” 清单是刷机之前写的，很多项后来已经通过并记录在 `bug.md`/`docs/audio.md` 等文档里。

| 阶段 | 内容 | 状态 | 证据 |
|------|------|------|------|
| S0 | 仓库、`.gitignore`、`bug.md`、决策记录 D-001..D-017 | 完成 | 提交 S0…S0.7 |
| S1 | 内核交叉编译 + ALARM rootfs（f2fs）+ boot.img + initramfs | 完成（构建侧） | `MANIFEST` 11 条目；`sha256sum -c SHA256SUMS` 全通过；`fsck` 副本 `[Fail]`=0 且 `cmp -l` 仅 4 字节 |
| S2 | 刷入链路（只写 boot_b + linux） | 完成（脚本侧） | `tests/run.sh` 5/5；拒绝矩阵 rc=2；D-009 门控 `erase dtbo_b` 三重校验 |
| S3 | MangoWM 0.18.3（wlroots-0.20 / scenefx-0.5） | 完成（构建+装配） | 设备原生编译；`readelf` NEEDED 31/31 可解析；10 文件落位 rootfs |
| S4 | 性能：schedplace + schedutil/噪声审计 | 完成（代码+单测） | 375 checks；0 warning；`nm -u` 无 sched_setscheduler |
| S5 | FEX-Emu（x86 兼容层） | 完成（设备端源码构建 + 真机验证） | 设备原生编译 8150 个目标；静态/动态 x86-64 程序均跑通，见 `docs/fex.md` |
| S6 | 交付收口（摘要、真机清单） | 完成 | `README.md` 当前状态表 + `INSTALL.md` + `bug.md`；Release 26/11/04 已发布 |

## 真机待验清单（PENDING-DEVICE，制定于刷机前）

> 下表“现状”列按已落地的实机证据回填；**没有通过记录的项一律标「未验证」，不得宣称通过**。

| # | 待验项 | 现状 | 证据 |
|---|---|---|---|
| 1 | 刷入：`fastboot erase dtbo_b`（可选，需 `--allow-dtbo-erase`，D-009）→ `flash linux` → `flash boot_b` → `--set-active=b`（人工，D-014） | 已完成 | `bug.md` B-060：从新 boot_b 启动，`dd` 回读 sha256 与 Release 产物一致 |
| 2 | 开机：进入 systemd，屏幕点亮（CSOT 面板，面板模块已在 initramfs 内） | 已完成 | 同上；面板模块在 initramfs 内 |
| 3 | 图形：freedreno + a650 固件加载成功，MangoWM 会话可启动（需 seatd 正常工作） | 已完成 | `README.md` 显示/桌面行；initramfs 必带 `a650_gmu.bin`/`a650_sqe.fw`/`a650_zap.mbn`（B-058） |
| 4 | 输入：触摸（novatek）、音量/电源键、霍尔键 | 触摸与音量键已完成；电源键/霍尔键未验证 | `README.md` 触摸行（音量键位 → `mango-volume` → OSD） |
| 5 | 网络：Wi-Fi（ath11k WCN6855）、蓝牙（qca/htbtfw20） | 已完成 | `README.md` Wi-Fi / 蓝牙行（含 MAC 全 0 的两处修复） |
| 6 | 音频：受 `hexagonrpcd` 缺失限制（B-020），预期无声 —— 需确认是否为唯一原因 | 立体声已实现；B-020 本身仍开放 | `docs/audio.md`：8× CS35L41 走 TDM 出声；缺 `hexagonrpcd` 不影响该通路 |
| 7 | 电源：suspend/resume、充电（bq25970） | 充电已完成；suspend/resume 未验证 | `README.md` 充电行（≈2.5 A / 约 10 W） |
| 8 | 存储：首启后 `resize.f2fs` 扩容至 `linux` 分区末尾（容量以设备实际分区为准）；`fsck.f2fs` 在真机上的表现 | 扩容已完成 | `bug.md` B-021 复验：设备根已扩到 `linux` 分区末尾 |
| 9 | 性能：schedplace 的 threaded cpuset 实测、busy 阈值标定、A/B 测量（帧时间/迁移次数） | 未执行 | `docs/findings/perf.md` §9 未决问题 |

## 洁净度自检（每阶段）

- `scripts/audit-hygiene.sh`：仓库零产物 + MANIFEST SHA256 复校 + FLASH-LOG 审计。
- `scripts/collect-manifest.sh`：由磁盘实际字节生成唯一 `MANIFEST` 与 `SHA256SUMS`，并交叉校验各 `MANIFEST.*` 片段。
