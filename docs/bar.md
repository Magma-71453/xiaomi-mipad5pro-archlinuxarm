# 顶部状态栏（waybar）

设计稿：`docs/bar-design.svg`（浅色「TABLET STATUS BAR」）。真机逻辑分辨率 1280×800、scale 2。

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ (图标)(图标)(图标)          10/26 00:37:00        (托盘) (🔊) (📶) (蓝牙) (⏻)  │
└──────────────────────────────────────────────────────────────────────────────┘
```

## 布局

| 区域 | 模块 | 说明 |
|---|---|---|
| 左 | `wlr/taskbar` | 每个窗口一个图标胶囊（`{icon}`，16px），当前窗口白底+描边；**纯图标**是为了让条目宽度有界，窗口再多也挤不动中间时钟 |
| 中 | `clock` | `MM/DD HH:MM:SS`，配合 `fixed-center` 固定在正中 |
| 右 | `tray` | StatusNotifierItem 托盘（bilibili/微信等「关闭到托盘」的图标） |
| 右 | `pulseaudio` / `network` / `bluetooth` | 点击分别 `kitty -1 -e alsamixer` / `nmtui` / `bluetoothctl` |
| 最右 | `custom/power` | `systemctl poweroff` |

外观：贴边占满整宽（margin 全 0）、高 38、圆角 16、浅色 `#F7F8FA`／描边 `#E2E6ED`。

## 图标为什么用 Nerd Font 码位

系统没有彩色 emoji 字体，`📶 🌐 📵 🔈 🔉 🔊` 全部渲染成方块。改用已安装的
`/usr/share/fonts/TTF/SymbolsNerdFont-Regular.ttf`：

| 用途 | 码位 |
|---|---|
| 音量（静音/小/大） | `U+F026` `U+F027` `U+F028` |
| Wi‑Fi / 有线 / 断网 | `U+F1EB` `U+F0AC` `U+F00D` |
| 蓝牙 | `U+F293` |
| 电源 | `U+F011` |

CSS 里必须同时声明字体族，否则字形缺失仍会回退成方块：

```css
font-family: "Noto Sans CJK SC", "Symbols Nerd Font", "Noto Sans Symbols 2", sans-serif;
```

## 性能

| 项 | 值 |
|---|---|
| 空闲 CPU | ≈0.4%（20 秒内 1 jiffy） |
| 内存 | 74–110 MB（GTK 底噪） |

压 CPU 的两件事：

1. **不用 `custom/clock` 脚本**——那样每秒 fork 一个 shell+date；改用内置 `clock`（进程内更新）；
2. `network` / `bluetooth` 轮询从 10s/15s 放宽到 **30s**。

## GTK3 的硬限制

`max-width` 在 GTK3 里不存在（waybar 0.15 链接 GTK 3.24，写了会报
`'max-width' is not a valid property name`）。所以「锁宽度」只能靠：

- 用短标签（`{app_id}` 或 `{icon}`）让条目天然有界；
- `min-width` 统一最小脚印；
- 完整标题放 `tooltip-format`。

## 文件

- `configs/rootfs/home/magma/.config/waybar/config.jsonc`
- `configs/rootfs/home/magma/.config/waybar/style.css`
- 会话启动：`configs/rootfs/usr/local/bin/mango-shell-start`（先 pkill 旧的 mango-bar/waybar，再拉起 waybar）

## 历史：mango-bar

`configs/mango-shell/mango-bar.c` 是自己写的 layer-shell + cairo 状态栏（含 SNI + dbusmenu 宿主）。
它在 mango 上收不到任何输入事件（layer 表面与 xdg_toplevel 都试过，pointer/touch 均为 0），
因此归档不用；实现与 bug 记录保留备查。

## 电源菜单（mango-powermenu）

waybar 最右侧电源键 (`custom/power`) 唤起一个 Material 3 风格的 GTK3 弹窗：

```
┌─────────────────────────────────────────┐
│  (🌙)      (⏻)      (↻)      (🤖)     │
│  睡眠      关机      重启     Android   │
└─────────────────────────────────────────┘
```

每个方块点下去都会切换到**二次确认**视图（红色 filled「确认」/ 浅灰「取消」），确认后才执行：

| 项 | 命令 |
|---|---|
| 睡眠 | `systemctl suspend` |
| 关机 | `systemctl poweroff` |
| 重启 | `systemctl reboot` |
| Android | `sudo -n /usr/local/bin/qbootctl -s 0 && reboot`（切到 slot `_a`） |

### qbootctl

切换 A/B slot 需要 `qbootctl`（上游在 GitLab，被 Cloudflare 挡住普通 curl，因此用官方 GitHub 镜像 `linux-msm/qbootctl`）：

```sh
sh scripts/build-qbootctl.sh
```

验证（只读）：

```sh
sudo qbootctl -x 0     # -> _a  (Android)
sudo qbootctl -x 1     # -> _b  (Linux)
sudo qbootctl          # 当前 slot 与两个 slot 的 active/successful/bootable
```
