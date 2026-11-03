# 终端三件套：fastfetch / htop / btop 的小米平板 5 Pro 适配

**结论先行**：平板横屏是 **1280×800 逻辑像素、scale 2**，默认 `foot` 窗口只有 **107×43 格**
（实测 `stty size` = `43 107`），而且**没有 emoji 字体**（只有 `Symbols Nerd Font` +
`Noto Sans CJK SC`）。

两类的做法不同：

- **htop / btop**：**窄、短、单行、用 Nerd Font 图标代替文字**，配色落在 waybar 同一套色板
  （`#7AA2F7` 蓝 / `#73DACA` 青 / `#565F89` 灰 / `#D04F61` 红）。
- **fastfetch**：本机个人版——**图片 logo（`kitty-direct`）+ 分组配色**，见 2.2。

| 工具 | 仓库里的文件 | 关键改动 |
|---|---|---|
| fastfetch | `configs/rootfs/home/magma/.config/fastfetch/config.jsonc` + `configs/rootfs/home/magma/.local/share/fastfetch/tux.png` | `logo.type=kitty-direct` 显示 PNG（`auto` 只出 `PNG` 占位符）；`key.type=both` + `width=16` 对齐；分组配色 |
| htop | `configs/rootfs/home/magma/.config/htop/htoprc` | `color_scheme=7`（Nord）；`AllCPUs2` + 温度；列改成 `PID USER RES SHR S CPU% MEM% TIME+ Command` |
| btop | `configs/rootfs/home/magma/.config/btop/{btop.conf,themes/elish.theme}` | 自写 `elish` 主题；`update_ms=1000`；固定温度传感器与电池 |

---

## 1. 这台机器上必须知道的约束

| 事实 | 实测值 | 影响的工具 |
|---|---|---|
| 逻辑分辨率 | 1280×800，scale 2（`mango-dispatch get all-monitors`） | 全部 |
| 默认 foot 窗口 | 107 列 × 43 行（`stty size` → `43 107`） | 全部：宽度超过 ~100 列就会折行 |
| 字体 | `FreeMono`（monospace 默认）、`Adwaita Mono`、`Noto Sans CJK SC`、`Symbols Nerd Font` | 图标只能用 Nerd Font 码位 |
| emoji | **没有** emoji 字体，emoji 一律方块 | 不能用 🙂 之类 |
| 电池 | **两个** fuel gauge：`bq27z561-0` / `bq27z561-1` | fastfetch `battery`、btop `show_battery` |
| 温度传感器 | 没有 `coretemp`；传感器名形如 `cpu0_thermal`、`cluster0_thermal` | htop `show_cpu_temperature`、btop `cpu_sensor` |
| 版本 | fastfetch 2.70.0 / htop 3.5.3 / btop 1.4.7 | 配置语法按这三个版本写 |

> ⚠️ **`fc-list ':charset=xxxx'` 要说真话**：图标是否存在，只用这条命令验证。
> 例如 `U+F3FA`（平板）和 `U+F538`（内存条）在 `Symbols Nerd Font` 里**不存在**，
> 写上就是空心方块——现在用的是 `U+F10B`（手机）和 `U+F233`（服务器内存）。

---

## 2. fastfetch

### 2.1 为什么不是默认配置

`fastfetch --gen-config` 生成的默认模块表有 30 项，在本机实测会输出：

- 两条一模一样的 `Battery:` 行（两个 fuel gauge）；
- 一行空的 `PowerAdapter:`（没插充电器时）；
- `Font` / `Cursor` / `Icons` / `Theme` 这些在 mango 下没有意义的项；
- 加上 arch 大 logo 后整体 40+ 行，在 43 行的窗口里顶到屏幕边缘。

### 2.2 现在的做法

conf 是**本机个人版**（`fastfetch --gen-config` 起手后自行改写），不是仓库早期那版
「窄、短、单行」的最小化配置。要点：

```jsonc
"logo": {
  "type": "kitty-direct",   // 关键：必须显式写协议
  "source": "/home/magma/.local/share/fastfetch/tux.png",
  "width": 25, "height": 16,
  "padding": { "left": 2, "top": 1 }
}
"display": { "separator": "", "key": { "width": 16, "type": "both" } }
```

| 设定 | 值 | 原因 |
|---|---|---|
| `logo.type` | `kitty-direct` | **`auto` 渲染不了 PNG**：它把图片当成名为 `PNG` 的文字占位符打印。实测 `auto` 与 `kitty-direct` 的区别就是「一个 `PNG` 字样」与「真图」 |
| `logo.source` | `~/.local/share/fastfetch/tux.png` | 图片资产放在 `~/.local/share/fastfetch/`，与 `~/.config` 分离 |
| `logo.width/height` | 25 / 16 | 以字符格为单位（不是像素） |
| `key.type` | `both` | 图标 + 英文标签同时显示 |
| `key.width` | 16 | 标签列对齐 |
| `keyColor` | `32` / `33` / `31` / `34` | 按分组用终端 16 色：绿=系统、黄=负载、红=桌面、蓝=运行时 |

**图片必须是不透明或自带透明通道**。本机 `tux.png` 是**白底不透明**（四角均
`(255,255,255,255)`，全图透明像素数 0），所以在深色终端里显示为一块白方块。
若要去底：

```sh
magick tux.png -fuzz 8% -transparent white tux.png   # 注意：会连白肚皮一起抠掉
```

### 2.3 验证

```sh
# 1) 配置能否被解析
fastfetch --config ~/.config/fastfetch/config.jsonc

# 2) 图片是否真的显示（而不是 PNG 占位符）
#    在 kitty 里跑，窗口右侧应出现图片；若只有 "PNG" 字样就是 type 写错了
kitty -1 -e fastfetch

# 3) 图标码位是否真有字形（key.type=both 时用到）
for cp in f015 f1c0; do
    printf '%s %s\n' "$cp" "$(fc-list ":charset=$cp" family 2>/dev/null | head -1)"
done
```

期望：图片正常显示、标签列对齐、四组颜色区分明显。

---

---

## 3. htop

### 3.1 配色：`color_scheme=7`（Nord）

> ⚠️ **不要照抄 `strings` 的顺序猜编号。** `strings /usr/bin/htop` 打出来的方案名顺序
> 与 `color_scheme` 的数字**不对应**（中间夹着 `MC` = Midnight 等未在列表里的项）。
> 唯一可靠的办法是打开 `F2 → Colors` 看哪个方案带 `[x]`：

```sh
# 在设备上确认真值：改 color_scheme，进 F2 -> Colors，看勾在哪一行
sed "s/^color_scheme=.*/color_scheme=7/" ~/.config/htop/htoprc > /tmp/x
HTOPRC=/tmp/x htop      # 按 F2，方向键下移到 Colors
```

实测映射（htop 3.5.3-1-arch，`F2 → Colors` 页直接读出来的）：

| `color_scheme` | 菜单里显示的名字 | 本机实测评价 |
|---|---|---|
| 0 | Default | 默认深灰底；进程名带绿/青分色，和 waybar 浅色卡片不搭 |
| 1 | Monochromatic | 纯灰阶，CPU 高低看不出 |
| 2 | Black on White | 白底，与深色终端冲突 |
| 3 | Light Terminal | 白底 |
| 4 | MC（Midnight） | **亮青底（`#24ACD4`），刺眼** |
| 5 | Black Night | 纯黑底，对比度可但无强调色 |
| 6 | Broken Gray | 绿/紫混色，进程表偏花 |
| **7** | **Nord** | **深底 + 青/黄强调色，与 fastfetch/btop 同色系 ← 采用** |
| ≥8 | （回落到 Default） | 越界值直接按 0 处理 |

### 3.2 头部：`AllCPUs2` + 温度，不用频率

```
0[|||    9.0% 33°C]  4[|||    8.6% 33°C]  Tasks: 154, 1065 thr, 199 kthr; 2 running
1[|||    8.5% 33°C]  5[|||    7.6% 33°C]  Load average: 0.86 1.55 1.35
2[|||   10.0% 34°C]  6[|||    8.6% 34°C]  Battery: 76.5%
3[|||   11.1% 33°C]  7[|||    1.4% 33°C]
Mem[|||||||||||||||||||||||||||||||3.73G/7.38G]
Swp[|                                            221M/10.0G]
```

- `AllCPUs2` = 2 列 × 4 行，8 核一屏看完；`AllCPUs4` 会折成 3 行把 Mem/Swp 挤出窗口，
  `AllCPUs8` 只剩数字没有柱状图。
- `show_cpu_frequency=0`：频率是逐核独立的（实测同一时刻 8 核读出 5 个不同值：
  `614MHz`–`2.84GHz` 之间连续变化），加进去每行多 9 列，把 `AllCPUs2` 右侧的
  `Tasks/Load/Battery` 顶掉。温度信息量更大，只保留 `show_cpu_temperature=1`。
- `column_meters_1=Tasks LoadAverage Battery`：`Battery` 是 htop 3.x 才有的 meter，
  sm8250 的两个 fuel gauge 会被自动合并成一个百分比，正好补上平板最关心的信息。

### 3.3 进程表列

```
PID USER  RES SHR S CPU%▽MEM% TIME+  Command
```

| 改动 | 原因 |
|---|---|
| 去掉 `PRI NI VIRT PRIV` | 默认 1280 宽下已经截断到 `/usr/local/bin/mango-dock-autohide` 外的部分，去掉不常用的 4 列后 Command 能多显示 ~30 字符 |
| `show_program_path=0` | 只显示 basename，`/usr/local/bin/mango-place` 变成 `mango-place`；`highlight_base_name=1` 会把它高亮 |
| `hide_kernel_threads=1` | 去掉约 200 个 kthread（开机后实测 109–217 个浮动），进程表多出 5 屏有效内容 |
| `delay=20`（2 s） | 2 s 刷新在 8 核上几乎看不出滞后，省一整轮 `/proc` 扫描 |

### 3.4 验证

```sh
# 配置是否被真正读取（htop -C 忽略 htoprc，这里故意不加）
HTOPRC=~/.config/htop/htoprc htop &
```

期望：头部第一行是 `0[...% 33°C] 4[...]`，最后一列 `Battery: NN.N%`，
进程表第一行是 `py3 mango-dock-autohide` 这类 basename 形式。

> **htop 会重写 `htoprc`**：界面上按 `F2` 改任何设置，退出时整份文件被重写、
> 注释全部丢失。想让改动进仓库，请改仓库里的文件再同步，而不是在界面上调。

---

## 4. btop

### 4.1 自写主题 `elish.theme`

btop 自带 40 个主题（`/usr/share/btop/themes/`），但都按桌面显示器的对比度设计，
在这块 11 寸平板上要么太灰（`greyscale`）要么太饱和（`HotPurpleTrafficLight`）。
`elish.theme` 直接取 fastfetch/waybar 的色值：

| 用途 | 颜色 | 与谁对齐 |
|---|---|---|
| 标题 / CPU 中段 | `#7AA2F7` | fastfetch key、waybar 标签 |
| 高亮 / 进程杂项 | `#73DACA` | fastfetch 用户名 |
| 边框 / 分隔 | `#3B4261` | 比正文更暗一档，四个盒子读成一个整体 |
| 峰值 / 满载 | `#D04F61` | waybar 电源键的强调红 |

安装位：`~/.config/btop/themes/elish.theme`，配置里 `color_theme = "elish"`。

### 4.2 配置要点

| 键 | 值 | 原因 |
|---|---|---|
| `shown_boxes` | `"cpu mem net proc"` | 四块都要；`net` 放左下，正好对上 `wlp1s0` |
| `update_ms` | `1000` | 默认 2000 ms 在触摸翻页时像"卡住" |
| `proc_gradient` | `false` | 渐变要逐行算颜色，8 核满载时是纯开销 |
| `proc_filter_kernel` | `true` | 去掉 kthread，进程表从 365 行降到 154 行 |
| `disks_filter` | `"/"` | `/var/log`(log2ram) 与 waydroid 的 bind mount 会重复出现 |
| `cpu_sensor` | `"cpu0_thermal"` | 不写死时 btop 会 "No good candidate ... using random"，读数每次都换 |
| `selected_battery` | `"bq27z561-0"` | 两个 fuel gauge，固定一个才有可比性 |
| `show_battery_watts` / `show_cpu_watts` | `false` | 需要 setuid 才能读 hwmon 功率，否则恒显示 0.0 W |
| `freq_mode` | `"range"` | 显示 `614 MHz - 1.6 GHz`，比单值更能反映大小核调度 |

### 4.3 验证

```sh
# 主题是否被解析（写错键名 btop 仍会启动，只是颜色回退到默认）
grep 'color_theme' ~/.config/btop/btop.conf    # -> color_theme = "elish"
ls ~/.config/btop/themes/elish.theme
```

期望：标题栏 `cpu / menu / preset` 是蓝色，进程表选中行背景为 `#2C3444`，
CPU 柱状图是 青→蓝→红 三段渐变。

> **btop 也会重写 `btop.conf`**：`save_config_on_exit` 默认 `true`，
> 在界面里按过 `Esc` 进菜单改设置后，退出时整份文件重写、注释丢失。

---

## 5. 同步到设备（不重刷）

仓库里 `configs/rootfs/home/magma/` 是真相源。同步三个工具的配置 + 一张图片：

```sh
DEVICE_IP="<device-ip>"
set -o pipefail
tar -czf - -C configs/rootfs/home/magma \
  .config/fastfetch/config.jsonc \
  .local/share/fastfetch/tux.png \
  .config/htop/htoprc \
  .config/btop/btop.conf \
  .config/btop/themes/elish.theme \
  | dbclient -i ~/.ssh/dropbear_ed25519_host_key "magma@${DEVICE_IP}" \
      'tar -xzf - -C "$HOME"'
```

> **图片资产为什么入库**：`tux.png`（114 KiB）是 fastfetch 的**运行时依赖**，不是文档插图。
> AGENTS.md §2 第 3 条「仓库不跟踪任何二进制」针对的是构建产物（`*.img` / `*.ko` /
> `pkg.tar.*`，见 `scripts/audit-hygiene.sh` 的 `ARTIFACT_GLOBS`）；一份 114 KiB 的
> PNG 不会让仓库膨胀，却能让设备复现出同样的界面。仓库里 `docs/research/*.png`
> 已是同样性质的文件。**新增图片请先确认体积 < 200 KiB**。
>
> 设备上有两张图：`tux.png`（入库）与 `<wallpaper>.png`（758 KiB，**个人偏好，不入库**）。
> 配置里 `logo.source` 指向后者时，clone 下来的仓库无法复现——所以入库的是 `tux.png`。

### 必须核对内容真的变了

`a && b && c` 链条里任何一步失败，后面的步骤会静默不执行——这是本仓库踩过的坑（AGENTS.md §5）。
每次同步后跑一遍：

```sh
dbclient -i ~/.ssh/dropbear_ed25519_host_key "magma@${DEVICE_IP}" 'bash -s' <<'EOF'
sha256sum ~/.config/{fastfetch/config.jsonc,htop/htoprc,btop/btop.conf,btop/themes/elish.theme}
wc -l   ~/.config/{fastfetch/config.jsonc,htop/htoprc,btop/btop.conf,btop/themes/elish.theme}
EOF
```

期望值与仓库一致：

```sh
git hash-object configs/rootfs/home/magma/.config/fastfetch/config.jsonc   # blob 哈希，非 sha256
# 或直接比对
for f in .config/fastfetch/config.jsonc .config/htop/htoprc .config/btop/btop.conf .config/btop/themes/elish.theme; do
    diff -u "configs/rootfs/home/magma/$f" <(dbclient -i ~/.ssh/dropbear_ed25519_host_key "magma@${DEVICE_IP}" "cat ~/$f")
done
```

`diff` 无输出即为同步成功。

---

## 6. 已知限制

1. **图标依赖字体**：所有图标来自 `Symbols Nerd Font`。换成没有 Nerd Font 的终端
   （例如 SSH 到别的机器）会全部变方块。fastfetch 可以把 `display.key.type` 改回 `"string"`。
2. **`Display` 显示 2560×1600**：fastfetch 读的是 DRM 物理分辨率，不是逻辑分辨率。
   mango 的 scale 2 它看不到，没有可行的覆盖手段（除非用 `command` 模块自己拼）。
3. **htop 没有电池瓦数**：`Battery` meter 只给百分比，功率要 `battery` 模块/`upower`。
4. **btop GPU 盒子不可用**：把 `shown_boxes` 设成 `"cpu gpu0"` 实测**被静默忽略**，
   btop 回落到默认四盒（cpu/mem/net/proc）。`gpu0` 需要 `nvidia-smi` / `rocm-smi`，
   Adreno 650（FD650）走 `/sys/class/devfreq/3d00000.gpu`，btop 1.4.7 不认识这条路径。
5. **两个工具都会重写配置**：htoprc 与 btop.conf 在界面上改过设置后会被整体重写，
   注释丢失。以仓库为准，不要反向从设备拉。
6. **fish 交互式 shell 里不能直接跑带 `$` 的多行命令**：远端登录 shell 是 fish，
   批量命令一律 `dbclient ... 'bash -s' < script.sh` 或包 `sh -c '...'`。
