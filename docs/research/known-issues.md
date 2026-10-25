# 已知问题（known-issues.md）—— xiaomi-elish / postmarketOS / SM8250

> 调研快照时间：2026-10-24。只读调研，未改动任何仓库文件，未接触设备。
> 影响分级定义：**阻塞** = 影响开机/日常可用；**高频** = 日常几乎必然遇到；**低频** = 特定场景才遇到。
> "7.2.0 是否仍有"列的证据级别：**有/无修复提交**（在内核 tag 内检索到/未检索到相关提交）｜**UNVERIFIED**（无一手证据）。

## 0. 方法与来源清单（可复核）

| 编号 | 来源 | 取法 |
|------|------|------|
| S1 | postmarketOS wiki: Xiaomi Mi Pad 5 Pro (xiaomi-elish) | 线上页面 https://wiki.postmarketos.org/wiki/Xiaomi_Mi_Pad_5_Pro_(xiaomi-elish) 已被 **Anubis** 拦截（HTTP 200 + JS 挑战页，7,475 B；api.php 同样被拦；r.jina.ai 也只拿到挑战页）。改用 Wayback 快照正文：http://web.archive.org/web/20260419194842id_/https://wiki.postmarketos.org/wiki/Xiaomi_Mi_Pad_5_Pro_(xiaomi-elish) （98,674 B，正文 26,727 字符，快照日 2026-04-19） |
| S2 | pmaports 提交历史（device / kernel 包） | https://gitlab.postmarketos.org/api/v4/projects/226/repository/commits?path=device/testing/device-xiaomi-elish&per_page=15&ref_name=main 与 ...path=device/testing/linux-postmarketos-qcom-sm8250... |
| S3 | issue #4448 | https://gitlab.postmarketos.org/api/v4/projects/226/issues/4448 （state=closed，created 2026-04-05T04:27:07Z，closed 2026-05-16T20:16:09Z） |
| S4 | MR !8398 | https://gitlab.postmarketos.org/api/v4/projects/226/merge_requests/8398 （state=merged，merged_at 2026-05-03T19:51:57Z） |
| S5 | sm8250 内核仓（project id 543，默认分支 7.2.0-dev） | https://gitlab.postmarketos.org/api/v4/projects/543/repository/commits?per_page=100&ref_name=7.2.0-dev ；并用 ref_name=sm8250-7.2.0_git20261010 复核"提交是否在 tag 内" |
| S6 | deviceinfo（分区/刷入参数） | https://gitlab.postmarketos.org/api/v4/projects/226/repository/files/device%2Ftesting%2Fdevice-xiaomi-elish%2Fdeviceinfo/raw?ref=main |
| S7 | Armbian elish 板级配置与固件注释 | https://raw.githubusercontent.com/armbian/build/main/config/boards/xiaomi-elish.conf |

**证据边界（必须写明）**：S5 的"7.2.0 是否仍有"结论，来自对 7.2.0-dev 最近 100 条提交按 elish/xiaomi 关键字的筛选，
再用 tag sm8250-7.2.0_git20261010 复核是否在 tag 内；**未检索到相关提交 ≠ 该问题已修复**（很多问题在用户态或驱动侧，
不会出现 elish DTS 提交）。所有"仍有/已修"判断都以该判据为准，无一手证据的一律标 UNVERIFIED。

---

## 1. 设备与维护状态（元问题）

### KI-01 device-xiaomi-elish 于 2026-05-03 由 community 降级为 testing
- **现象**：包目录从 device/community/ 迁到 device/testing/（提交 c66aa62e，2026-05-03）。
- **影响分级**：低频（不影响运行，只影响支持等级与更新策略）。
- **来源**：S2（commit c66aa62e）；S4（MR !8398 merged 2026-05-03）；S3（issue #4448，2026-05-16 关闭）。
  MR 描述原文：kernel 包已不满足 community 内核版本要求，且维护者对 issue #4448 无响应。
- **7.2.0 是否仍有**：与内核版本无关，属**维护策略**问题；当前 pmaports main 仍为 testing（S2 最新一条 = e5536561，2026-07-19）。

### KI-02 内核版本时效性要求（6 个月线）
- **现象**：issue #4448 列出当时不达标的 community 内核，其中 linux-postmarketos-qcom-sm8250 = 6.17.0（2025-09-29）。
- **影响分级**：低频。
- **来源**：S3 原文（"devices in the community category may not use a kernel version older than 6 months"）。
- **7.2.0 是否仍有**：已通过升级解决——pmaports 于 2026-10-13 把该包升到 **7.2.0_git20261010**（S2，commit eb66b2d8）；
  但设备目录仍在 testing（未随升级回迁）。

### KI-03 固件子包被 treewide 移除（影响非 pmOS 流程的固件供给）
- **现象**：pmaports 于 2026-07-19 统一移除 device/testing 的 nonfree-firmware 子包（S2，commit e5536561，
  "treewide: device/testing: remove nonfree-firmware subpackages"）。
- **影响分级**：低频（对 pmbootstrap 流程）；对本项目（自建 rootfs、不走 pmOS）**需自行保证固件到位**。
- **7.2.0 是否仍有**：N/A（打包策略）。参考：Armbian 用完整 linux-firmware + 自己的 a650_sqe.fw（见 KI-09）。

---

## 2. 显示

### KI-04 面板存在 BOE / CSOT 两个变体，dtb 分两支
- **现象**：dtb 分 qcom/sm8250-xiaomi-elish-boe 与 -csot；面板类型由 cmdline 的 msm_drm.dsi_display0 决定
  （BOE = qcom,mdss_dsi_k81_35_02_0b_dual_cphy_video；CSOT = qcom,mdss_dsi_k81_42_02_0a_dual_cphy_video）。
- **影响分级**：**阻塞**（选错变体 = 黑屏）。
- **来源**：S1（"Know your panel variant" 段与 Features 表 Screen=Works）；S7（ABL_DTB_LIST 两支）。
- **7.2.0 是否仍有**：是（结构性差异，非缺陷）。tag 内相关提交：9b10e327（panel follower）、c227bafd（panel reset pin）。
  正确做法 = boot.img 同时带两个 dtb（deviceinfo_append_dtb="true"，S6）。

### KI-05 亮度/休眠相关
- **现象**：wiki Features 表把 Screen 记为 Works（"ideally with sleep mode and brightness control"），未单列 suspend/brightness 缺陷。
- **影响分级**：UNVERIFIED。
- **来源**：S1。
- **7.2.0 是否仍有**：**UNVERIFIED**（wiki 未给 suspend 细节；tag 内有 panel follower 提交，暗示面板电源序列有改动）。

---

## 3. 触摸

### KI-06 触摸屏可用，但依赖 pinctrl 配置（曾有 pin 设置缺失/重命名）
- **现象**：Features 表 Touchscreen=Works。
- **影响分级**：低频。
- **来源**：S1；S5 提交：bf41448c（add pin setting for touchscreen）、19af1b97（novatek,irq-gpio → irq-gpios 改名）。
- **7.2.0 是否仍有**：可用；**在 tag sm8250-7.2.0_git20261010 内已确认包含**上述两条（复核见下 §9）。
  注意改名（irq-gpio→irq-gpios）意味着**旧 dtb/旧用户态混用会失效**，必须整包替换。

---

## 4. 音频

### KI-07 扬声器音质差（高频被截、闷、高音量削波）
- **现象**：原厂 DSP 处理链路导致音质差；把每路扬声器的 PCM source 从 DSP 改为 ASP 可显著改善，但可能关闭 DSP 的扬声器保护。
- **来源**：S1（Issues → Audio Quality），给出做法：amixer -c 0 cset name="BLL PCM Source" 0，对 BLL/BLH/BRL/BRH/TLL/TLH/TRL/TRH 逐路设置。
- **影响分级**：高频（听感问题，人人都会遇到）。
- **7.2.0 是否仍有**：**UNVERIFIED**；tag 内无 cs35l41 相关提交（判据见 §0 证据边界）。

### KI-08 立体声失效：8 个扬声器全部播左声道
- **现象**：所有扬声器播左声道，右声道不出声。根因（wiki 作者分析）：CS35L41 驱动的 set_channel_map() 未被 SM8250 音频驱动调用，
  用户态无法设置 RX channel select（CS35L41_SP_FRAME_RX_SLOT，regmap 地址 0x00004820，默认 0x00000100，最低字节 0x00=左 / 0x02=右）；
  wiki 作者用**打过 regmap 写权限补丁的自编译内核** + debugfs 才能临时验证。
- **来源**：S1（Issues → Stereo Speakers），附外部 commit https://gitlab.com/CalcProgrammer1/linux-sm8250-mainline/-/commit/db5f1bae052acbf7a24cb2487b1724db0e8672a4
- **影响分级**：高频（音频基本不可用于双声道场景）。
- **7.2.0 是否仍有**：**很可能仍有**（tag 内未见 cs35l41/扬声器声道映射相关提交）→ 按证据边界标 **UNVERIFIED**。

### KI-09 录音（麦克风）在 mainline 不工作
- **现象**：原文 "In mainline kernel, it appears that audio recording does not work."
- **来源**：S1（Issues → Audio Recording）。
- **影响分级**：高频（通话/录音/语音输入全废）。
- **7.2.0 是否仍有**：**UNVERIFIED**（tag 内无录音链路提交）。
- 相关：hexagonrpcd-sdsp（ADSP 侧服务）在 Armbian 里以服务形式启用（S7），说明该链路仍依赖额外用户态服务。

---

## 5. 相机

### KI-10 相机 Broken（wiki Features 表）
- **现象**：Camera = Broken；Camera Flash = Works。
- **来源**：S1。
- **影响分级**：低频（桌面/游戏用途不需要）。
- **7.2.0 是否仍有**：**内核侧已大幅补齐，用户态可用性 UNVERIFIED**。tag 内相机相关提交（S5，均已复核在 tag 内）：
  e64a2eaf（ov13b10 广角）、5cc57130（ov8856 前摄）、32034c9e（hi556 景深）、165ee57e（pm8008 PMIC）、31b8f429（pm8150l ldo2）。
  相机要跑起来还需要 libcamera + IPU6/CAMSS 用户态管线，本调研未验证。

---

## 6. USB-C / 供电

### KI-11 快充（bq25970）限制多
- **现象**（wiki 原文三条）：① 快充只认小米原装充电器；② bq25970 仅在电量 > 30% 时工作；
  ③ 插上 USB 后要等 30 秒才启动（作者指出该 30s 是**在其自维护分支里硬编码**的）。
- **来源**：S1（Issues → Charging），并给出自维护源码链接
  https://gitlab.postmarketos.org/jianhua/sm8250-mainline/-/blob/6.13.0/drivers/power/supply/bq25970_charger.c#L2086
- **影响分级**：高频（充电体验）。
- **7.2.0 是否仍有**：**UNVERIFIED**。注意：该 30s 硬编码位于 jianhua 的 **sm8250-mainline 分支**，
  与本次要构建的 sm8250 内核仓（project 543，默认分支 7.2.0-dev）是否同源同码，本轮**未逐行核对**。
- 相关：Battery = Partial（S1）。

### KI-12 USB-C 角色切换 / OTG
- **现象**：Features 表 USB OTG = Works；USB Networking = Works（可 ssh 172.16.42.1）。
- **来源**：S1。
- **影响分级**：低频（正向能力，不是缺陷）。
- **7.2.0 是否仍有**：正向能力，UNVERIFIED（与内核版本无关的常规结论）。

---

## 7. GPU

### KI-13 3D 加速可用，但依赖专用固件（a650_sqe.fw / a650_gmu.bin）
- **现象**：Features 表 3D Acceleration = Works；Armbian 板级配置注释写明要用完整 linux-firmware **外加自己的** qcom/a650_sqe.fw，
  并在 initramfs hook 里显式加入 a650_sqe.fw 与 a650_gmu.bin。
- **来源**：S1；S7（注释 "Use the full firmware, complete linux-firmware plus Armbian's (for qcom/a650_sqe.fw)"，以及 hook 里的 add_firmware 行）。
- **影响分级**：**阻塞**（缺固件 = 无 GPU，MangoWM 无法起 3D 会话）。
- **7.2.0 是否仍有**：属固件供给问题，与本项目 rootfs 组装强相关（必须自行保证这两个 fw 文件到位）。

---

## 8. 电源 / 休眠

### KI-14 电源域与休眠（PMIC / sleep pinctrl）
- **现象**：tag 内有多条电源与低功耗相关提交：165ee57e（pm8008 PMIC）、31b8f429（pm8150l ldo2）、
  8d965c42（BT/WLAN 的 sleep pinctrl 状态）。
- **影响分级**：高频（休眠/待机功耗）；具体缺陷现象**UNVERIFIED**（wiki 未列 suspend 缺陷）。
- **来源**：S5；S1（Features 表未单列 suspend）。
- **7.2.0 是否仍有**：**UNVERIFIED**（有修复提交 ≠ 问题全消；需要真机 suspend/resume 测试）。

---

## 9. 传感器

### KI-15 霍尔传感器（Hall Effect）= Broken
- **现象**：Features 表 Hall Effect = Broken。wiki 列出 4 个霍尔/磁传感器用途：GPIO 136/138 = 手写笔吸附方向检测
  （可能用于笔的无线充电，downstream 由 IDT P9418 驱动使用）、GPIO 110 = 磁吸保护盖、GPIO 121 = 位置不明。
- **来源**：S1（Issues → Hall Sensors，附 downstream dts 链接）。
- **影响分级**：低频。
- **7.2.0 是否仍有**：部分改善但未证实——tag 内提交 9de59793（"add hall key"）把霍尔**按键**接进 input，
  但 IIO 霍尔**传感器**是否可用 **UNVERIFIED**。

### KI-16 加速度计 / 环境光
- **现象**：Accelerometer = Works，Ambient Light = Works。
- **来源**：S1。
- **影响分级**：正向能力。
- **7.2.0 是否仍有**：UNVERIFIED（wiki 未给具体器件型号）。

---

## 10. 按键 / 键盘（pogo pin 键盘）

### KI-17 pogo pin 键盘可用（自 6.11.0），但会额外注册一个鼠标设备
- **现象**：键盘注册 keyboard + mouse 两个设备，屏幕上出现光标。启用/关闭/复位靠 GPIO 127（电源）、141（复位）、83（中断），
  或写 sysfs xiaomi_keyboard_conn_status（enable_keyboard / disable_keyboard / reset / host_reset）。
- **来源**：S1（Issues → Keyboard，附 downstream xiaomi_keyboard.c 链接与 gpioset 用法）。
- **影响分级**：低频。
- **7.2.0 是否仍有**：**结构性仍在**（键盘 MCU 需用户态上电），内核侧有两次相反动作：
  5157db18（2026-08-19，"drop bogus keyboard enable pinctrl"）→ bcadd8a3（2026-08-25，"add keyboard enable pinctrl again"），
  两条**均在** tag sm8250-7.2.0_git20261010 内 ⇒ 说明 6.19→6.25 之间出现过键盘 pinctrl 回归并已修复（S5）。

### KI-18 手写笔（Xiaomi Smart Pen）
- **现象**：Features 表 Stylus = Works（含笔的按键等）。
- **来源**：S1。
- **影响分级**：正向能力。
- **7.2.0 是否仍有**：UNVERIFIED；注意笔的无线充电依赖 IDT P9418 驱动（见 KI-15），与霍尔传感器状态相关。

---

## 11. Wi-Fi / 蓝牙

### KI-19 蓝牙适配器可能不出现，需要写 MAC 地址
- **现象**：bluetoothctl list 里没有适配器时，需 btmgmt --index 0 public-addr 11:22:33:44:55:66，或安装 bootmac 包；
  wiki 作者指出 bootmac 的 mac_bluetooth() 需要加 sleep 1（elish 的 BT 芯片启动稍慢），并已开 upstream 工单 bootmac#4。
- **来源**：S1（Issues → Bluetooth）。
- **影响分级**：高频（蓝牙鼠标/键盘/耳机全废）。
- **7.2.0 是否仍有**：**内核侧已补 GPIO/电源控制，用户态 MAC 供给问题仍在**（UNVERIFIED）：
  tag 内提交 269e507f（WiFi 节点加 label）、e98f7f8f（BT 加 enable/swctrl GPIO）、c0fba720（qca6390 PMU 加 BT swctrl-gpios）、
  fececdde（修复 wlan-default-state GPIO）、8d965c42（BT/WLAN sleep pinctrl）。

### KI-20 Wi-Fi
- **现象**：Features 表 WiFi = Works。
- **来源**：S1；S5（fececdde 修 wlan-default-state GPIO、269e507f WiFi 节点 label）。
- **影响分级**：正向能力（曾是问题，tag 内有修复提交）。
- **7.2.0 是否仍有**：可用；wlan-default-state GPIO 修复**在 tag 内**（S5 复核）。

### KI-21 GPS = Broken
- **现象**：Features 表 GPS = Broken。
- **来源**：S1。
- **影响分级**：低频（平板通常不用）。
- **7.2.0 是否仍有**：**UNVERIFIED**（tag 内未见 GNSS 相关 elish 提交）。

---

## 12. 其它（与构建/刷入相关的已知约束）

- **KI-22 只写 boot_b + linux 的可行性与 dtbo 冲突** → 见 flash-layout.md 的 CONFLICT-2（erase dtbo_b）。
- **KI-23 MangoWM 依赖版本冲突** → 见 versions.md 的 CONFLICT-1（wlroots 0.19.2 / scenefx 0.4.1 只对应 MangoWM ≤ 0.14.4）。
- **KI-24 内核构建已切 LLVM 工具链**：pmaports 的 linux-postmarketos-qcom-sm8250 自 2025-11-03 起用 clang/lld（见 bug.md B-003），
  本次核对的 APKBUILD（ref=main）makedepends 含 clang/llvm（S2 对应 raw 文件 device/testing/linux-postmarketos-qcom-sm8250/APKBUILD，pkgver=7.2.0_git20261010）。

---

## 13. 7.2.0_git20261010 tag 内已复核的 elish 提交（证据表）

复核方法：拉取 ref_name=sm8250-7.2.0_git20261010 的提交列表（per_page=100），逐一比对 short_id 是否命中。

| short_id | 日期 | 标题 | 归类 |
|----------|------|------|------|
| bcadd8a3 | 2026-08-25 | arm64: dts: qcom: sm8250-xiaomi-elish: add keyboard enable pinctrl again | 键盘（回归修复） |
| c4f9babf | 2026-08-19 | ... drop bogus tablet-mode switch | 输入/模式 |
| 5157db18 | 2026-08-19 | ... drop bogus keyboard enable pinctrl | 键盘 |
| dbed552a | 2026-08-19 | ... correct the board ID | 平台识别 |
| 32034c9e | 2026-08-19 | ... Add hi556 depth camera | 相机 |
| 31b8f429 | 2026-08-19 | ... Add pm8150l ldo2 | 电源 |
| e64a2eaf | 2026-08-19 | ... Add ov13b10 wide camera | 相机 |
| 9b10e327 | 2026-08-19 | ... Add panel follower | 显示 |
| 5cc57130 | 2026-08-19 | ... Add ov8856 front camera | 相机 |
| 165ee57e | 2026-08-19 | ... Add pm8008 PMIC | 电源 |
| 19af1b97 | 2026-08-19 | ... rename novatek,irq-gpio to irq-gpios | 触摸 |
| 9de59793 | 2026-08-19 | ... add hall key | 按键 |
| bf41448c | 2026-08-19 | ... add pin setting for touchscreen | 触摸 |
| c227bafd | 2026-08-19 | ... add reset pin setting for panel | 显示 |
| 269e507f | 2026-08-19 | ... add label to WiFi node | 无线 |
| c0fba720 | 2026-08-19 | ... add BT swctrl-gpios to qca6390 PMU | 蓝牙 |
| e98f7f8f | 2026-08-19 | ... add enable and swctrl GPIOs to BT | 蓝牙 |
| fececdde | 2026-08-19 | ... fix wlan-default-state GPIO | Wi-Fi |
| 8d965c42 | 2026-08-19 | ... add sleep pinctrl states for BT and WLAN | 电源 |
| 5c14265c | 2026-08-19 | ... Extend device tree for Xiaomi Mi Pad 5 Pro | 汇总 |

tag 头提交：3c5440e9（2026-10-02，"Merge branch '7.2.0' into '7.2.0-dev'"）。

---

## 14. 供 bug.md 合并的条目（dispatcher 可直接摘用）

- KI-N1 **[开放]** 立体声失效（8 扬声器全左声道，CS35L41 RX slot 未暴露）— 来源 S1；7.2.0 未证修复。
- KI-N2 **[开放]** 录音不工作 — 来源 S1；依赖 hexagonrpcd-sdsp 用户态。
- KI-N3 **[开放]** 音质差（可用 amixer PCM source DSP→ASP 规避，牺牲扬声器保护）— 来源 S1。
- KI-N4 **[阻塞-固件]** GPU 需要 a650_sqe.fw / a650_gmu.bin（Armbian 额外提供）— 来源 S7。
- KI-N5 **[高频]** 蓝牙需人工写 MAC（btmgmt 或 bootmac + sleep 1）— 来源 S1。
- KI-N6 **[阻塞-选型]** 面板 BOE/CSOT 必须匹配，否则黑屏 — 来源 S1/S7。
- KI-N7 **[低频]** 快充三条限制（原装充电器 / >30% 电量 / 30s 延迟）— 来源 S1 + jianhua 分支源码链接。
- KI-N8 **[低频]** 相机内核节点已补齐但用户态未验证 — 来源 S1/S5。

## 15. UNVERIFIED 清单（known-issues.md）

- U1 任何"7.2.0 是否仍有"结论，凡未在 tag 内找到直接修复提交者，均为 UNVERIFIED（判据见 §0）。
- U2 bq25970 30 秒硬编码是否存在于 7.2.0-dev（project 543）源码 —— 未逐行核对 jianhua 分支与本仓差异。
- U3 相机在 7.2.0 + libcamera 用户态是否真正可用。
- U4 suspend/resume 与亮度控制在 7.2.0 的实际表现。
- U5 霍尔传感器（IIO）是否随 9de59793 一并可用。
- U6 wifi/bt 固件组合（qca6390）在本项目自建 rootfs 下的完整文件清单。
