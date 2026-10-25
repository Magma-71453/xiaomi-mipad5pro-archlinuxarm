# MangoWM 触摸桌面 — 全部改动总览（2026-10-25）

> 这份文档是**当前状态的唯一入口**：所有做过的改动、最终架构、每处修复的根因、验收方式与遗留问题。
> 更细的历史细节在 [`mango.md`](mango.md) §6–§9，交接约定在 [`handoff-mango-shell.md`](handoff-mango-shell.md)，
> dock 自定义在 [`dock-howto.md`](dock-howto.md)。

---

## 1. 一句话现状

Xiaomi Pad 5 Pro（`elish`，Arch Linux ARM，mango 0.18.3）现在是**纯触摸友好的浮动窗口桌面**：

- 屏幕：1600×2560@120Hz 面板，旋转 90° 顺时针 + 2 倍缩放 → **1280×800 逻辑**
- 所有窗口**默认悬浮**，圆角 12、阴影小、金边
- 底部**居中磨砂 dock**（5 个图标，可配置文件改）
- 触摸手势：3 指左右切 tag、3 指上滑 overview
- **编辑模式**：dock 指针按钮进入 → 点窗口置顶 → 底边 × ○ ✋ + 四角缩放抓手 → 右上「完成」退出
- 屏幕键盘（居中 680 宽、停在 dock 上沿）、音量键 + OSD、图片壁纸
- **没有** mangobar / waybar / 平铺 / 整理模式 / 自绘标题栏

---

## 2. 演进过程（三版）

| 版本 | 时间 | 形态 | 结局 |
|---|---|---|---|
| v1 | 10-25 上午 | 平铺 + 全宽 mangobar + 左下 dock + 整理模式 + 平铺编辑模式 | 用户反馈"对平板太逞强"，整体废弃 |
| v2 | 10-25 晚 | 全部悬浮 + 自绘 macOS 标题条 + 居中 dock | 标题条"突兀"、CSD 应用叠两层，标题条删除 |
| v2.1（当前） | 10-25 夜 | 悬浮 + 无自绘装饰 + 居中 dock + 编辑模式（3 钮 + 四角缩放） | 定稿 |

---

## 3. 最终架构

### 3.1 启动链

```
agetty --autologin magma (tty1)
  └─ fish -l                          ~/.config/fish/config.fish
      └─ /usr/bin/mango               XDG_RUNTIME_DIR=/run/user/<uid>
          └─ exec_once=/usr/local/bin/mango-shell-start    ← 唯一入口
              ├─ mango-touchd          触摸手势（evdev）
              ├─ mango-keys            音量键（evdev）
              ├─ mango-dock            居中磨砂 dock
              ├─ mango-osd             音量 OSD
              ├─ mango-place           新窗口尺寸 + 居中 + 级联
              ├─ supervisor → mango-actions   编辑模式覆盖层
              └─ swaybg -i ~/.wallpaper.jpg   壁纸
```

### 3.2 组件职责

| 组件 | 类型 | 作用 |
|---|---|---|
| `mango-shell-start` | sh | 清理残留 → 启动全部组件；`XDG_RUNTIME_DIR` 跟随会话（logind 的 `/run/user/<uid>`） |
| `mango-dock` | C + cairo | 底部居中磨砂胶囊，图标来自 `~/.config/mango-dock/apps.conf`，`exclusive_zone=82`，overlay 层 |
| `mango-osd` | C + cairo | 顶部居中音量胶囊，轮询 `mango-osd-value` 的 mtime，1.5s 自动隐藏，overlay 层 |
| `mango-actions` | C + cairo | 编辑模式：全屏 overlay 覆盖层，选窗/三钮/四角缩放；`focusid` 置顶 |
| `mango-place` | python | 新浮动窗口 resize 到 11/16×7/10、`centerwin`、级联偏移 26px |
| `mango-touchd` | python | 3 指手势：左右切 tag、上滑 overview |
| `mango-keys` | python | evdev 音量键 115/114/113 → `mango-volume` |
| `mango-volume` | sh | ALSA 8 个功放 `*Digital PCM`，±5%，mute=记值设 0% |
| `mango-osk-toggle` | sh | wvkbd 开关（`WVKBD_CENTER_W` + `WVKBD_BOTTOM_MARGIN=82`） |
| `mango-dispatch` / `mango-run` | sh | 自动找活的 mango socket 后转发 `mmsg` / `exec` 程序 |
| `mango-actions-toggle` | sh | 写/删 `$XDG_RUNTIME_DIR/mango-actions` 标志 |

### 3.3 图层占用

```
background/wallpaper    swaybg
overlay/mango-actions   编辑模式覆盖层（按需）
overlay/mango-dock      dock
overlay/mango-osd       音量 OSD（短暂）
overlay/wvkbd           屏幕键盘（按需）
```

### 3.4 运行时文件（`$XDG_RUNTIME_DIR` = `/run/user/<uid>`）

| 文件 | 说明 |
|---|---|
| `mango-actions` | 编辑模式开关（存在=开） |
| `mango-actions.log` | 覆盖层日志（supervisor 重启用） |
| `mango-osd-value` | 当前音量百分比 |
| `mango-volume` / `mango-volume.muted` | 音量状态 |
| `mango-dock.geom` | dock 几何 `x y w h hidden` |
| `mango-*.sock` | mango 自己的 IPC |

---

## 4. `/etc/mango/config.conf` 增量

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

观感：`border_radius=12`、`shadows=1`、`shadows_size=8`、`shadows_blur=12`、`focus_color=0xc9b890ff`。

---

## 5. 交互说明

### 5.1 日常

| 操作 | 效果 |
|---|---|
| 点 dock 图标 | 启动对应程序（kitty / firefox / rofi / 键盘 / 编辑模式） |
| 三指左右滑 | 切换有窗口的 tag |
| 三指上滑 | overview |
| 音量键 | 调音量 + 顶部 OSD |

### 5.2 编辑模式

dock 指针按钮 → 淡遮罩 + 右上「完成」：

| 操作 | 效果 |
|---|---|
| 点窗口 | 先 `focusid` 置顶，再显示控件 |
| 底边 × | `killclient` 关窗 |
| 底边 ○ | `togglefullscreen` 全屏/还原 |
| 底边 ✋ | 按住拖动窗口 |
| 四角抓手 | 从该角缩放（对角固定） |
| 空白处 | 取消选中 |
| 右上「完成」 | 退出（覆盖层在 overlay 层，dock 被盖住，所以只能用它退出） |

---

## 6. 踩过的坑（根因 → 修法）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 重启后"配置全没了" | 会话 `XDG_RUNTIME_DIR` 从 `~/.xdg` 变成 `/run/user/1000`，脚本硬编码旧路径，连到僵尸 socket | 所有守护进程动态发现 runtime dir，`~/.xdg` 仅兜底 |
| 2 | fish 里编译 dock 报一屏 undefined reference | **fish 的命令替换不按空格切分**，`$(pkg-config ...)` 变成一个参数 | 用 `sh build-dock.sh`，或 `... \| string split " "` |
| 3 | 屏幕键盘点不出来 | 脚本里裸写 `wvkbd-mobintl`，桌面 PATH 没有 `/usr/local/bin` | 改绝对路径 |
| 4 | 键盘压住 dock | layer margin 底部为 0 | wvkbd 补丁 `WVKBD_BOTTOM_MARGIN=82` |
| 5 | 两个壁纸层 | 反复重启留下多个 swaybg | 启动前 `pkill -x swaybg` |
| 6 | 编辑模式退不出来 | 覆盖层在 overlay 层盖住了 dock | 加「完成」按钮；覆盖层改回 overlay 以便全屏也能用 |
| 7 | 全屏时编辑模式不可用 | 覆盖层在 top 层，被全屏窗口盖住 | 同上，改 overlay 层 |
| 8 | 编辑模式点后面的窗口不置顶 | 覆盖层吃掉点击，只选中不 focus | 选中时 `dispatch focusid`（mango focus 带 lift） |
| 9 | Firefox 拖边不能缩放 | mango `client_resize_edge_at()` 跳过"指针在子 surface 上"，GTK CSD 边缘铺了子 surface | 不改 mango，编辑模式加四角抓手（`movewin`+`resizewin`） |
| 10 | 自绘标题条突兀 / CSD 叠两层 | 独立浮层条 + 所有窗口都加 | 删除自绘条，控制交给应用自己 |
| 11 | 按钮之间冒斜线 | cairo 路径没 `new_path`，上个图标路径连到下一个圆 | 每段绘制前 `cairo_new_path` |
| 12 | 多个 actions 覆盖层叠加 | 多个 supervisor 各拉一个 | supervisor 带唯一标记，启动前 `pkill -f mango-actions-super` |
| 13 | dock 拉 toggle 留下僵尸 | dock 没处理 SIGCHLD | `signal(SIGCHLD, SIG_IGN)` |
| 14 | 重启 getty 后桌面起不来 | 反复 restart 导致 tty1 上两个 agetty，无登录会话 | 重启设备恢复；已记录，避免连续 restart |
| 15 | OSD/覆盖层 `layer_surface has never been configured` | 首次 configure 前就 attach buffer | 加 `configured` 标志，未 configure 不绘制 |

---

## 7. 构建方式

### 7.1 mango 外壳 C 程序（dock / osd / actions）

```sh
cd ~/elish-build/src/mango-shell
sh build-dock.sh          # 只构建 dock
sh build-dock.sh install  # 构建 + 安装 + 重启 dock
```

协议文件缺失时脚本会自动跑 `wayland-scanner`（wlr-layer-shell + xdg-shell）。

### 7.2 wvkbd（两处补丁）

`~/elish-build/src/wvkbd/main.c`，`WVKBD_CENTER_W` 分支：

```c
int bottom_margin = 0;
if (getenv("WVKBD_BOTTOM_MARGIN"))
    bottom_margin = atoi(getenv("WVKBD_BOTTOM_MARGIN"));
zwlr_layer_surface_v1_set_margin(layer_surface, 0, 0, bottom_margin,
                                 (sw - (int)req_w) / 2);
```

### 7.3 其它

- `swaybg` / `grim` / `kitty` / `firefox` / `rofi`：pacman 或源码构建（`grim` 为源码）
- mangobar：已删除

---

## 8. 验收命令

```sh
export XDG_RUNTIME_DIR=/run/user/$(id -u); export WAYLAND_DISPLAY=wayland-0

ps -eo args | grep -E 'mango-(dock|osd|actions|place|touchd|keys)' | grep -v grep
mmsg get all-layers            # wallpaper / mango-dock（+ 按需 actions/wvkbd）
mmsg get all-monitors          # 1280x800 scale 2
mango -p -c /etc/mango/config.conf; echo rc=$?
mango-volume up; sleep 0.5; mmsg get all-layers | grep -c mango-osd   # 1
mango-osk-toggle; sleep 1;  mmsg get all-layers | grep -c wvkbd      # 1
```

---

## 9. 已知问题 / 未做

1. **dock 无看门狗**：崩溃后不会自动重启，需 `mango-shell-start` 恢复
2. **编辑模式的四角缩放在超小窗口**（< 2×抓手）时抓手会重叠，逻辑上仍可用但不好点
3. **wvkbd 补丁只在设备源码树**（`~/elish-build/src/wvkbd/main.c`），换机构建需重新打
4. **没有触屏拖窗手势**（编辑模式的 ✋ 之外）：日常拖窗仍要 `SUPER`+左键
5. **mango 没有 raise/restack 的 IPC**，置顶只能通过 `focusid` 间接实现
6. `mango-session`、`arr`、`elish-fastcharge` 等是其它会话留下的脚本，未整理
