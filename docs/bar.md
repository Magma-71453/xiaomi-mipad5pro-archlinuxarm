# mango-bar — 轻量状态栏

设计稿：`docs/bar-design.svg`（浅色版「TABLET STATUS BAR」，1600×72 比例）。

## 外观（真机 1280×800 逻辑分辨率）

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ (foot · ~) (bilibili · 哔哩哔哩…) (bilibili · 【突破…)   周日,10/25 23:50:56 (🔉 📶 ᛒ) (⏻) │
└──────────────────────────────────────────────────────────────────────────────┘
```

| 区域 | 内容 |
|---|---|
| 左侧 taskbar | 当前工作区每个窗口一个圆角矩形胶囊，文字 `appid · 标题`；当前窗口白底 + 描边 `#D8DCE3`，其余 `#ECEEF2` 灰底 |
| 中间 | 时钟 `周几,MM/DD HH:MM:SS`（中日韩字体 `Noto Sans CJK SC`） |
| 右侧托盘 | 一个胶囊内 **三个独立点击区**：扬声器 / Wi‑Fi / 蓝牙 |
| 最右 | 圆形电源键（`#FBEAEC` 底、`#D04F61` 图标），**点一次进入待确认（变实心红），3 秒内再点才关机** |

## 托盘交互

托盘胶囊按三等分切成三个独立热区，点击分别执行 `~/.config/mango-bar/actions.conf` 里的命令（缺省值）：

```
audio=foot -e alsamixer
network=foot -e nmtui
bluetooth=foot -e bluetoothctl
```

即：点扬声器开音频混音器、点 Wi‑Fi 开 NetworkManager TUI、点蓝牙开 bluetoothctl。
想换成图形化前端（如 `pavucontrol`、`nm-connection-editor`、`bluetuith`）只改这个文件即可，不用重编译。

## 实现

- 单文件 C（`configs/mango-shell/mango-bar.c`，约 740 行），Wayland `wlr-layer-shell` + cairo + shm，无 GTK/Qt；
- 图层 `top`，`exclusive_zone = 54`，锚定 上/左/右；
- **双缓冲轮转**：compositor 只在新 buffer 提交后释放旧 buffer，单缓冲会在第一帧后死锁；
- 所有外部命令用绝对路径（会话 `PATH` 里可能没有 `/usr/local/bin`）；
- **UTF-8 清洗**：窗口标题非法时 cairo 会进入错误态并静默丢弃之后所有绘制（表现为「只有窗口胶囊、没有时钟/托盘/电源」），
  解析后统一 `sanitize_utf8()`，并在画完胶囊后检查 `cairo_status()`，异常则重建 context 兜底；
- 空闲开销：1 Hz 时钟 + 1 Hz 客户端列表 + 2 Hz 状态，**内容变化才重绘**。

## 构建 / 安装

```sh
cd configs/mango-shell
sh build-bar.sh            # 生成 ./mango-bar
sh build-bar.sh install    # 装到 /usr/local/bin 并重启
```

会话启动由 `/usr/local/bin/mango-shell-start` 负责（mango 的 `exec_once` 调用）。
调试：`MANGO_BAR_DUMP=/tmp/bar.png mango-bar` 会把当前帧直接写成 PNG，便于离线检查。
