# Audio bring-up on xiaomi-elish (SM8250 + 8x CS35L41)

## 已确认的事实（真机 2026-10-25）

| 项 | 证据 |
|---|---|
| 声卡 | `card0: sm8250 - Xiaomi Mi Pad 5 Pro`，唯一 PCM `MultiMedia1`（hw:0,0） |
| 功放 | 8 颗 CS35L41 全部 probe（`snd_soc_cs35l41_i2c 12288 8`），i2c 1-0041/42/43、3-0040… |
| 固件 | `/lib/firmware/cirrus/cs35l41-dsp1-spk-prot*.wmfw|.bin` 存在且加载成功（dmesg 有 `DSP1: Protection: …TLH…bin`） |
| UCM | `/usr/share/alsa/ucm2/Xiaomi/elish/{elish.conf,HiFi.conf}` 与 postmarketOS `device-xiaomi-elish/alsa-ucm-conf/HiFi.conf` **逐字节相同**（diff 为空） |
| 内核配置 | `SND_SOC_QDSP6_{AFE,ADM,ROUTING,ASM,APM,PRM}` 全部打开，`SND_SOC_QCOM/SM8250` 为模块 |
| 现状 | `speaker-test -D hw:0,0 -c2` 与 `aplay` 都能正常打开/跑完，**但完全没有声音** |

## 社区“只有左声道”的根因（代码级）

`sound/soc/qcom/sm8250.c` 的 `TERTIARY_TDM_RX_0` 分支只给 **CPU 侧**设了 TDM：
```c
snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x03, 8, 32);
snd_soc_dai_set_channel_map(cpu_dai, 0, NULL, channels, tdm_slot_offset);
```
**从未对 8 颗 CS35L41 调用 `set_channel_map`**，而 codec 侧的 `cs35l41_set_channel_map()`
正是写 `CS35L41_SP_FRAME_RX_SLOT` 的唯一途径 —— 不调用，所有功放都停在上电默认 slot 0，
因此所有喇叭只放左声道。这就是社区 bug 的根因。

## 已打的补丁（未刷机，只换了内核模块）

```c
for_each_rtd_codec_dais(rtd, j, codec_dai) {
        ...
        rx_slot = ((j % 4) < 2) ? 0 : 1;   /* DT 顺序 tlh,tll,trh,trl,blh,bll,brh,brl */
        ret = snd_soc_dai_set_channel_map(codec_dai, 0, NULL, 1, &rx_slot);
}
```

- 只重编 `snd-soc-sm8250.ko`（`make -C <src> ARCH=arm64 LLVM=1 M=sound/soc/qcom modules`），
  zstd 压缩后替换 `/lib/modules/7.2.0/kernel/sound/soc/qcom/snd-soc-sm8250.ko.zst`，`depmod -a` + `modprobe -r/-i`。
- 原模块备份在设备 `/root/snd-soc-sm8250.ko.zst.orig`。
- 变体：slot 0/4 与 slot 0/1 都试过 —— **两者都没有声音**，说明“无声”不是 slot 映射造成的，
  而是更上游（功放使能/供电/TDM 下行）的问题；slot 映射只决定“有声之后是单声道还是立体声”。

## 音量（重要：默认非常吵）

- `<AMP> Digital PCM Volume`：0..913，**默认 817**；`<AMP> Analog PCM Volume`：0..20，默认 18。
- 日常音量建议：Digital 150~250、Analog 8~10。用 `amixer -c 0 cset "name='BLH Digital PCM Volume'" 150` 设置（注意 `sset` 找不到这些控件，必须用 `cset` + 完整 name）。

## 下一步（待办）

1. 用 `/sys/kernel/debug/asoc/Xiaomi Mi Pad 5 Pro/dapm/` 观察播放时功放 widget 是否 powered（本次 glob 路径写错，未取到）。
2. 检查功放供电：dmesg 里 `supply VA/VP not found, using dummy regulator` —— DT 没有定义 VA/VP 稳压器，
   若 pmOS 真机有声，说明其 DT 或 UCM 另有使能路径（需对比其 kernel patch 集）。
3. 检查 q6 AFE 的 TDM 端口是否真的在跑：`cat /proc/asound/card0/pcm0p/sub0/status` + ADSP `apr` 日志。
4. 确认 `speaker-test` 走的是 UCM 使能后的路径（`alsaucm ... set _enadev Speaker` 之后再播放）。

## 寄存器级证据（播放中实测，2026-10-25 11:30）

| 位置 | 值 | 含义 |
|---|---|---|
| `/proc/asound/card0/pcm0p/sub0/status` | `state: RUNNING`, `owner_pid: 62211`(pipewire) | 音频数据确实在送 |
| amp `PWR_CTRL1` 0x2014 | `0x00000001` | 已上电 |
| amp `PWR_CTRL2` 0x2018 | `0x00003721` | 各使能位已置 |
| amp `AMP_OUT_MUTE` 0x2024 | `0x00000000` | **未静音** |
| amp `ASP_ENABLES` 0x4800 | `0x00010000` | **ASP RX1 已使能** |
| amp `SP_FRAME_RX_SLOT` 0x4820 | `0x00000100` | 双声道槽位 0/1 已配置 |
| pinmux gpio133/134/135/137 | `function mi2s2_sck/data0/ws/data1`, device `sound` | TDM 引脚已 mux |

即：**数据在跑、功放上电、未静音、ASP 接收已开、引脚已 mux、槽位已配** —— 但仍无声。
剩下的可疑点收敛到两处：

1. **ADSP 侧 TDM 端口是否真的在发波形**（q6afe TERTIARY_TDM_RX_0）。APPS 侧看不到它的时钟，需要看 ADSP 日志或 `apr` 调试。
2. **功放的 speaker protection 固件用的是通用版**：dmesg 里 8 颗功放全部 `Subsystem ID not found`，
   因此加载的是 `cs35l41-dsp1-spk-prot.wmfw`（通用），而不是设备专属的 `cs35l41-dsp1-spk-prot-10251826-*.bin`。
   厂商 DT 每颗功放还带 `cirrus,fast-switch = "TRH-music.txt" ...` 与 `cirrus,tuning-has-prefix`，我们的（pmOS 上游）DT 没有。
   保护 DSP 未按设备调音时，输出被静音是已知行为。

### 建议的下一步（按性价比排序）
1. 给 DT 的 8 个功放节点补 `cirrus,tuning-has-prefix` + `cirrus,fast-switch`（对应 `/lib/firmware/cirrus/` 里的 txt/bin），重编 dtb（只换 boot 分区）。
2. 用 `apr`/ADSP 侧调试确认 TDM 端口在跑（`cat /sys/kernel/debug/remoteproc/*/trace0` 或 `q6afe` 调试）。
3. 若确认 ADSP 没发 TDM，则问题在 `sm8250.c` 的 AFE 端口配置（`snd_soc_dai_set_sysclk`/TDM_BCLK_RATE 与 vendor 的实现不同）。

## 与 postmarketOS 的逐项对比结论（2026-10-25 11:35）

| 对比项 | 结果 |
|---|---|
| `sound/soc/qcom/sm8250.c` | 与 pmOS `7.2.0-dev` **只有我加的补丁这一处差异**（pmOS 上游同样没有 per-codec TDM slot → 印证“单声道”是上游行为） |
| 内核配置（`CONFIG_SND*` / `CONFIG_QCOM*`） | 与我们逐项比对：**0 处差异**（用 pmOS `config-postmarketos-qcom-sm8250.aarch64` 全量对比） |
| UCM `HiFi.conf` / `elish.conf` | 与 pmOS `device-xiaomi-elish/alsa-ucm-conf/` **逐字节相同** |
| 固件 | pmOS `firmware-xiaomi-elish` 装的是 `adsp*/cdsp*/slpi*/venus/a650_*/novatek/nt36523/cirrus/cs35l41*`，并**主动删除** `acdb/`；我们已具备同类文件 |
| DT | 同一内核树同一份 `sm8250-xiaomi-elish-common.dtsi` |

**因此在“静态可对比”的层面上我们已经和 pmOS 完全一致，但本机仍然无声。**
剩下的差异只能在运行时才能看出来（例如 ADSP 侧 AFE 端口是否接受 TDM 配置、ACDB/调音是否真的加载成功、
或 pmOS 真机其实也无声 —— wiki 上“speakers do not operate in stereo”这句可能已经过时）。

### 若要继续，下一步只能是两种
1. **对照真机 pmOS**：用 pmOS 的 boot 镜像启动同一台机器（不写 rootfs），确认它到底有没有声音；有声音再抓它的 dmesg/mixer 差异。
2. **ADSP 侧调试**：在 `sm8250.c` 的 TDM 分支加 `dev_info` 打印 AFE 端口打开/时钟设置返回值，并把 ADSP 的 `apr` 交互日志拉出来。

## ✅ 修复（2026-10-25 11:45）：出声了，并且切到直通后变响变清

### 关键发现
1. 之前的“完全无声”有一半是我把 PipeWire 搞挂了（我手动 kill + 重载模块，`/proc/asound/cards` 变成 `--- no soundcards ---`），
   导致所有播放根本没到硬件。重载模块 + `systemctl --user restart wireplumber` 后，
   **调试日志证明 TDM 通路真正跑起来了**：
   ```
   Tertiary TDM Playback: elish-tdm: codec 0..7 (cs35l41-pcm) slot 0/0/1/1/0/0/1/1 -> 0
   Tertiary TDM Playback: elish-tdm: hw_params ch=2 slots=8 width=32 set_tdm_slot=0
   ```
2. 出声后是**很小很闷**：原因是 UCM 默认让功放走内部保护 DSP（`PCM Source = DSP`），
   而我们只有通用保护固件（`Subsystem ID not found`）。把 `PCM Source` 切成 **ASP 直通**后，
   音量与清晰度都明显改善（实测对比，用户确认）。

### 落地改动
| 改动 | 位置 |
|---|---|
| 机器驱动给 8 颗 CS35L41 分配 TDM RX slot（左 slot0、右 slot1，按 DT 顺序每两颗一组） | `configs/kernel/patches/0001-sm8250-per-codec-tdm-slots.patch`；设备上已编成 `snd-soc-sm8250.ko` 替换（备份 `/root/snd-soc-sm8250.ko.zst.orig`） |
| UCM 默认走 ASP 直通 | `configs/rootfs/usr/share/alsa/ucm2/Xiaomi/elish/HiFi.conf`（设备上已覆盖 `/usr/share/alsa/ucm2/Xiaomi/elish/HiFi.conf`，原文件备份为 `.orig-dsp`） |

### 日常音量（实测合适）
- `<AMP> Digital PCM Volume`: **780 ~ 900** / 913（默认 817 已经很响）
- `<AMP> Analog PCM Volume`: **14 ~ 18** / 20
- PipeWire sink：60 ~ 90%

### 仍未做
- 立体声分离的正确性还需听感确认（现在每两颗一组分配 slot 0/1，理论上左右各两颗）。
- 保护 DSP（`PCM Source=DSP`）路径偏小偏闷，若要更好音质需要厂商的 per-amp 调音文件 + 驱动支持。

## 右声道修复（2026-10-25，已验证双扬声器正常）

### 根因

`CS35L41_SP_FRAME_RX_SLOT`（regmap `0x4820`）**不是 TDM 槽位编号，而是 ASP RX 的声道选择器**：

| 值 | 含义 |
|---|---|
| `0x00` | 播放左声道 |
| `0x02` | 播放右声道 |

主线的 `sm8250.c` 从不调用 `snd_soc_dai_set_channel_map()` 设置它，于是 8 颗功放全部保持上电默认值（左声道），
表现即「8 个喇叭都在响，但只有左声道」。

### 关键事实

1. 器件树里 `&i2c1` 挂 `BRH/BLH/BRL/BLL`（**右侧**喇叭），`&i2c3` 挂 `TRH/TLH/TLL/TRL`（**左侧**喇叭）。
2. dai-link 的 `sound-dai` 顺序是 `tlh, tll, trh, trl, blh, bll, brh, brl`，所以 `for_each_rtd_codec_dais()` 的 `j`：**`j = 0..3` 左侧，`j = 4..7` 右侧**。
3. 修复：`rx_slot = (j < 4) ? 0 : 2;`，再对每个 codec DAI 调 `snd_soc_dai_set_channel_map(codec_dai, 0, NULL, 1, &rx_slot)`。

验证（`/sys/kernel/debug/regmap/<dev>/registers`）：

```
1-0040..1-0043  0004820: 00000102    # i2c1 = 右侧 -> 右声道
3-0040..3-0043  0004820: 00000100    # i2c3 = 左侧 -> 左声道
```

### 走过的弯路（均已被实测否掉，勿重复）

| 假设 | 实测结果 |
|---|---|
| 右声道功放需要不同 TDM **槽位**（1、4、8…） | 全都只有左声道；8 颗功放统一指向 slot 1 时**完全无声** → 该字段不是槽位 |
| `tx_mask` 改 `0x11`/`0xff` | `TERTIARY_TDM_RX_0` 是偶数 DAI，`q6tdm_set_tdm_slot()` 只用 `rx_mask`，`tx_mask` 被忽略 |
| 播放时设置 TX channel map | `q6tdm_set_channel_map()` 对偶数 DAI 只接受 `rx_slot`，传 TX 直接 `-EINVAL`，播放流失败 |
| `DSP RX1 Source` 改 `ASPRX2` | 无声；ASP 只有一路，真正的选择在功放 `0x4820` |
| TDM 帧长用 2 个 slot | 无效 |
| DSP 只发单声道 | 错；DSP 发的是立体声，是功放都选了左声道 |

### 模块参数（bring-up 用）

`/etc/modprobe.d/50-snd-soc-sm8250.conf` 固化：

```
options snd_soc_sm8250 tdm_slots=8 tdm_width=32 rx_mask=3 left_slot=0 right_slot=2 tx_mask=0 amp_slot_mode=0
```

| 参数 | 默认 | 含义 |
|---|---|---|
| `left_slot` / `right_slot` | 0 / 2 | 左/右两组功放的 ASP RX 声道选择（0=左，2=右） |
| `amp_slot_mode` | 0 | 0 = 按左右分组；1 = 每颗功放用 `j` 作选择值 |
| `tx_mask` / `rx_mask` | 0 / 3 | `TERTIARY_TDM_RX_0` 为偶数 DAI，只有 `rx_mask` 生效 |
| `tdm_slots` / `tdm_width` | 8 / 32 | TDM 帧长与槽宽（硬件只支持 16/32 位） |

### 补丁

`configs/kernel/patches/0001-sm8250-per-codec-tdm-slots.patch`（基于 `sm8250-7.2.0_git20261010` 原始源码生成）。

### 响度（2026-10-25）

UCM 的 Speaker enable 序列每次开流都会重设功放增益，默认值偏小，这是「音量+ 拉满仍觉得小」的原因：

| 控件 | 原值 | 现值 | 说明 |
|---|---|---|---|
| AMP Digital PCM Volume | 822（驱动默认 817） | 913 | 范围 0..913，取最大 |
| AMP Analog PCM Volume | 18 | 20 | 范围 0..20，约 +20.5 dB |
| AMP DRE Switch | 1 | 1 | 动态范围扩展，保持开启 |
| AMP PCM Source | ASP | DSP | 走功放内部扬声器保护算法 |

合计约 +3 dB 硬件增益；软件音量保持 100% 即可，不要用大于 100% 的软件增益（会削波失真）。

设备树升压参数保持 cirrus,boost-peak-milliamp = 4000（寄存器 0x3808 = 0x40 已确认写入生效），
若将来还需要更大声，可在 DTS 里提高到 5000 并重新刷 boot_b。
