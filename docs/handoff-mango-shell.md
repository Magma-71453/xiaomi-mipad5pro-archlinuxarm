# MangoWM 触摸桌面 — 改动声明（v2：macOS 式浮窗桌面）

> 单向声明，供维护 fish 会话入口 / 壁纸 / 系统镜像的那一路会话合并时避让。
> 适用版本：mango 0.18.3，Xiaomi Pad 5 Pro（elish），2026-10-25 重写。

---

## 0. 这一版是什么

v1 的平铺 + 整理/编辑模式被**整体废弃**（对触摸平板太逞强）。v2 定位：

- **所有窗口默认悬浮**（`window_rule=app_id:.*,is_floating:1`）
- **没有自绘标题条**（macOS 风格那版已删除，用户反馈"太奇怪"）。窗口控制交给应用自己：
  kitty 用它的 tab bar 当标题条（见 `kitty.conf`），Firefox 等用自带标签栏。
- `mango-place` 只负责"新窗口放到舒服的位置和大小"（屏幕 11/16 × 7/10，居中），不画任何东西
- **dock 居中**，图标：kitty / firefox / rofi / 屏幕键盘
- 顶部不再有状态栏（mangobar 已删除），壁纸用 swaybg

---

## 1. 归属

| 范围 | 归属 |
|---|---|
| `/etc/mango/config.conf` 的触摸/缩放/浮窗/键位/exec_once 段 | 我 |
| `/usr/local/bin/mango-*`（decor/dock/osd/touchd/keys/volume/dispatch/run/shell-start/osk-toggle） | 我 |
| `~/.local/share/fonts/*`（Noto CJK + Symbols2） | 我 |
| fish 会话入口、壁纸进程、系统镜像 | 你那一路（我只改了 fish 里的一行，见 §4） |

---

## 2. 文件清单

### 2.1 脚本 / 守护进程

| 设备 | 仓库 | 作用 |
|---|---|---|
| `/usr/local/bin/mango-shell-start` | `configs/rootfs/usr/local/bin/` | **唯一启动入口**：decor / dock / osd / touchd / keys / 壁纸；顺带清理旧组件与 waybar |
| `mango-touchd` | 同上 | 触摸手势：3 指左右切 tag、3 指上滑 overview（**没有任何启动软件的手势**） |
| `mango-keys` | 同上 | 音量键 115/114/113 → `mango-volume` |
| `mango-volume` | 同上 | ALSA 8 个功放 `*Digital PCM`，±5%，mute=记值设 0% |
| `mango-dispatch` / `mango-run` | 同上 | 自动找活的 mango socket 后转发 `mmsg` / exec 程序 |
| `mango-osk-toggle` | 同上 | wvkbd 开关（`WVKBD_BOTTOM_MARGIN=82` 让它停在 dock 上沿）（`--non-exclusive --width 680`，居中补丁） |

### 2.2 C 程序（源码在 `configs/mango-shell/`）

| 设备 | 源码 | 作用 |
|---|---|---|
| `/usr/local/bin/mango-place` | — | 新窗口放置：resize 到 11/16×7/10，居中并**级联偏移 26px**，避免窗口完全叠在一起 |
| `/usr/local/bin/mango-dock` | `mango-dock.c` | 居中磨砂 dock，4 图标，`exclusive_zone=82` |
| `/usr/local/bin/mango-osd` | `mango-osd.c` | 音量 OSD（顶部居中，1.5s 自动隐藏） |

构建：

```sh
cd configs/mango-shell   # 或设备 ~/elish-build/src/mango-shell
wayland-scanner client-header wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1-client-protocol.h
wayland-scanner private-code  wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1-protocol.c
wayland-scanner private-code  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.c
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-client-protocol.h
cc -O2 -o mango-dock mango-dock.c wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
   $(pkg-config --cflags --libs wayland-client cairo) -lm
```

### 2.3 配置文件与构建脚本

| 路径 | 说明 |
|---|---|
| `~/.config/mango-dock/apps.conf` | dock 应用列表（`图标 | 命令 [| sep]`），改完重启 dock 即可 |
| `configs/mango-shell/build-dock.sh` | dock 构建脚本，**必须用 sh 跑**（fish 不会切分 `$(pkg-config ...)`） |
| `docs/mango-shell-summary.md` | 全部改动总览（架构 / 根因 / 验收 / 遗留） |

### 2.4 已删除（v1 遗留，**不要恢复**）

`mangobar`（二进制 + `~/.config/mangobar/` 全部配置）、`mango-arrange*`、`mango-arrange-overlay`、
`mango-tileedit-toggle`、`mango-edit-toggle`、`mango-edit-off`、`mango-shell-watch`、`mango-touch-gestures`、
`lisgd`、`/usr/local/bin/mango-startup.sh`、`/usr/share/mango-dock/icons/{arrange,edit}.png`。

---

## 3. `/etc/mango/config.conf` 增量

```ini
touch_enable=1
touch_enable_mouse_emulation=1
device_rule=name:NVTCapacitiveTouchScreen,monitor:DSI-1
device_rule=name:NVTCapacitivePen,monitor:DSI-1

monitor_rule=name:DSI-1,width:1600,height:2560,refresh:120,x:0,y:0,scale:2,rr:3

window_rule=app_id:.*,is_floating:1

bind=SUPER,k,spawn,/usr/local/bin/mango-osk-toggle
bind=none,XF86AudioRaiseVolume,spawn,/usr/local/bin/mango-volume up
bind=none,XF86AudioLowerVolume,spawn,/usr/local/bin/mango-volume down
bind=none,XF86AudioMute,spawn,/usr/local/bin/mango-volume mute

exec_once=/usr/local/bin/mango-shell-start
```

观感值：`border_radius=12`、`shadows=1`、`shadows_size=8`、`shadows_blur=12`
（原来是 18/28，阴影太大，已收小）。

---

## 4. 会话入口约定（fish）

- fish 里启动命令是 `/usr/bin/mango`，**不要带 `-s <脚本>`**；桌面组件只由 `exec_once` 拉起。
- `XDG_RUNTIME_DIR` 保持 logind 的 `/run/user/<uid>`（**不要改成 `~/.xdg`**）。
  我的脚本只在变量为空/目录不存在时回退 `~/.xdg`。
- 壁纸：`mango-shell-start` 里先 `pkill -x swaybg`，然后有 `~/.wallpaper.jpg` 就
  `swaybg -i "$HOME/.wallpaper.jpg"`，没有则回退纯色 `swaybg -c '#101014'`。

---

## 5. 运行时文件（`$XDG_RUNTIME_DIR`）

| 名称 | 含义 |
|---|---|
| `mango-osd-value` | 音量百分比（OSD 轮询 mtime） |
| `mango-dock.geom` | dock 几何 `x y w h hidden`（备用，当前无 watcher 消费） |
| `mango-volume` / `mango-volume.muted` | 音量状态 |
| `mango-*.sock` | mango IPC（`mmsg`/装饰器用） |

图层：

```
background/wallpaper    swaybg
overlay/mango-dock      居中 dock
overlay/mango-osd       音量 OSD（短暂）
overlay/wvkbd           屏幕键盘（按需）
```

---

### 5.1 窗口一律不加自绘标题栏

kitty 自带的 `window_title_bar` 和 tab bar 都关掉了（标题只有 cwd 的 `~`，没有实用信息）：

```ini
window_title_bar_min_windows 0
tab_bar_style hidden
window_padding_width 10
```

其他应用（Firefox 等）保留自己的控制栏。窗口移动靠 `SUPER` + 左键拖（mango 的 mousebind）。

### 5.2 窗口操作模式（mango-actions）

dock 最右边的指针按钮 → 生成 `$XDG_RUNTIME_DIR/mango-actions` 标志 → `mango-actions` 显示一层
全屏覆盖（**overlay 层**，0.18 黑遮罩）：点任意浮动窗口，先 `focusid` 置顶，然后：

- 三个圆钮固定排在窗口**底边居中**：× 关窗 / ○ 全屏 / ✋ 拖动
- 窗口**四个角**各一个圆抓手，拖哪个角就从哪个角缩放（对角固定）

| 操作 | 动作 |
|---|---|
| × | `killclient` |
| ○ | `togglefullscreen` |
| ✋ | 按住拖动窗口（`movewin`） |
| 四角抓手 | `movewin` + `resizewin` 组合，见下表 |

| 角 | 命令 |
|---|---|
| ↖ TL | `movewin(+dx,+dy)` + `resizewin(-dx,-dy)` |
| ↗ TR | `movewin(0,+dy)` + `resizewin(+dx,-dy)` |
| ↙ BL | `movewin(+dx,0)` + `resizewin(-dx,+dy)` |
| ↘ BR | `resizewin(+dx,+dy)` |

**退出**：右上角「完成」胶囊。覆盖层在 overlay 层、位于 dock 之上，所以模式里点不到 dock 的按钮，
这点是刻意的 —— 只有 overlay 层能盖住全屏窗口，否则全屏时编辑模式完全不可用。
底部三个圆钮会自动上移，避开 dock 那条带子（`win_h - 96`）。

启动由 `mango-shell-start` 里的 supervisor 守护（带唯一标记 `mango-actions-super`）。

注意：mango 把触摸走指针模拟，所以交互全部在 `wl_pointer` 回调里（`wl_touch` 收不到事件）。

### 5.3 为什么 Firefox 拖边不能改大小

mango 有 `enable_border_resize=1`（默认开，抓取条 `border_resize_size=10`），但
`src/input/pointer.c:client_resize_edge_at()` 里有一条：

```c
if (surface && surface != client_surface(c))
    return 0;   /* 指针在子 surface 上就不算边框 */
```

Firefox（GTK CSD）在窗口边缘铺了子 surface，指针落上去就被判定为"子 surface"，
所以拖边不会触发 resize；kitty 是单 surface，所以能拖。

**结论**：不要试图改这条逻辑（要改就得重编 mango）。触屏上改大小用编辑模式的 **⤢** 按钮，
它走 `resizewin`，对任何浮动窗口都有效（实测 Firefox 880x560 → 916x632）。

## 6. 合并约束

1. fish 不要再传 `-s`；`exec_once` 是唯一入口。
2. 不要改 `XDG_RUNTIME_DIR`。
3. 不要再装回 mangobar / waybar：现在没有顶部状态栏，`mango-shell-start` 会 `pkill -x waybar`。
4. 新增启动项请另写 `exec_once=<脚本>`，不要塞进 `mango-shell-start`。
5. 缩放只认 `monitor_rule=...,scale:2`，`wlr-randr` 在这版 mango 上无效。

---

## 7. 验收

```sh
export XDG_RUNTIME_DIR=/run/user/$(id -u); export WAYLAND_DISPLAY=wayland-0
ps -eo args | grep -E 'mango-(decor|dock|osd|touchd|keys)|swaybg'
mango-dispatch get all-layers        # 期望 wallpaper / mango-dock
mango-dispatch get all-monitors      # 期望 1280x800 scale 2
mango -p -c /etc/mango/config.conf; echo rc=$?
kitty & sleep 3; mango-dispatch get all-clients   # 期望 x=200 y=154 w=880 h=560 is_floating=true
```

---

## 8. 已知问题

1. kitty 的标题条是它自己的 tab bar，只有左上角一小块（单 tab 时），不是全宽条；想要全宽需要改 kitty 源码或用别的终端。
2. `mango-dock` 偶发启动即退出（无看门狗了），重启会话即可恢复。
3. 没有窗口拖动条了：拖窗口要靠 `SUPER+左键`（mango 的 mousebind）或应用自己的标题栏。
