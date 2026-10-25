# mango-bar — 轻量状态栏

设计稿：`docs/bar-design.svg`（1720×190 的横屏预览，1600×72 的实机比例）。

## 外观

```
┌────────────────────────────────────────────────────────────────────────────┐
│ [foot · ~] [Firefox · Design] …        周日,10/25 23:37:24   ( 🔉  📶  ᛒ ) ( ⏻ ) │
└────────────────────────────────────────────────────────────────────────────┘
```

- 深色圆角长条（`#171A21`，94% 不透明，1px `#323844` 描边）；
- **左侧**：最多 4 个窗口胶囊，文字为 `appid · 标题`，超出 210px 用 `…` 省略；
  当前窗口高亮（填充 `#30394B`、描边 `#637CA8`、文字 `#F5F7FC`），其余为 `#222730` / `#353C49` / `#CAD0DB`；
- **中间**：`周日,10/25 23:37:24`（中文星期 + 月/日 + 时分秒，每秒刷新）；
- **右侧**：状态胶囊（音量 / Wi‑Fi / 蓝牙，均为 cairo 矢量绘制）+ 圆形电源键（`#33252B`，描边 `#62404A`）。

## 实现

- 单个 C 文件 `configs/mango-shell/mango-bar.c`（~700 行），Wayland `wlr-layer-shell` + cairo + shm；
  不依赖 GTK/Qt，常驻内存几 MB，空闲 CPU 接近 0（1 Hz 时钟 + 1 Hz 客户端刷新 + 2 Hz 状态刷新，**仅在内容变化时重绘**）。
- 图层：`top`，`exclusive_zone = 52`，锚定 上/左/右 → 平铺窗口不会盖住它。
- **双缓冲**：compositor 只有在新 buffer 提交后才释放旧 buffer，单缓冲会在第一帧之后死锁（踩过）。
- 所有外部命令用绝对路径调用（会话 `PATH` 里可能没有 `/usr/local/bin`）。

## 数据来源与交互

| 元素 | 数据 | 点击行为 |
|---|---|---|
| 窗口胶囊 | `/usr/local/bin/mango-dispatch get all-clients`（JSON：`id` / `title` / `appid` / `is_focused`） | `mango-dispatch dispatch focusid client,<id>` |
| 时钟 | 本地时间 | — |
| 音量图标 | `wpctl get-volume @DEFAULT_AUDIO_SINK@`（回退 `pactl`）；静音时画叉 | 切换静音 |
| Wi‑Fi 图标 | `/sys/class/net/wlp1s0/operstate` | — |
| 蓝牙图标 | 是否存在 `/sys/class/bluetooth/hci0` | — |
| 电源键 | — | 第一下进入「待确认」（按钮变红），3 秒内再点一次执行 `systemctl poweroff` |

## 构建 / 安装

```sh
cd configs/mango-shell
sh build-bar.sh            # 生成 ./mango-bar（需要 wayland-scanner / cairo / wayland-client）
sh build-bar.sh install    # 安装到 /usr/local/bin 并重启
```

会话启动由 `/usr/local/bin/mango-shell-start` 负责（`exec_once` 调用），已包含 `mango-bar`；
旧的 `mangobar` 与 `mango-shell-watch` 不再启动。
