# MangoWM 触摸屏友好配置调研（elish / Xiaomi Pad 5 Pro）

- 调研时间：2026-10-25；**当天已按本文 §6 直接装进系统并验收通过**
- 目标机：`magma@192.168.1.90`（Arch Linux ARM / aarch64 / mango 0.18.3 / DSI-1 1600x2560）
- 结论先说：**互联网上不存在"拿来就能用的触摸屏版 mango 配置"**。mango 从 0.16.0 才有原生触摸，
  官方只给了 2 个开关 + 设备级映射；触屏手势不在 compositor 里（`gesturebind` 只对触摸板生效）。
  真正可抄的是平板整机 dotfiles（目前只找到 Lenovo Yoga Book 一例）＋ 通用外挂件（wvkbd / lisgd），
  而且必须按 0.18 的新键名改写，否则老配置会被**静默忽略**。

---

## 1. 本机实测基线（都是在这台机器上跑出来的）

| 项目 | 实测值 | 证据 |
|---|---|---|
| compositor | mango 0.18.3（tag `8b876cb`，本仓库 `.build/mango/src/mango-0.18.3` 同源） | `mango -v`；`docs/mango.md` |
| 输出（安装前） | `DSI-1` 1600x2560，`scale=1`，无旋转，9 个 tag | `mmsg get all-monitors` |
| 输出（安装后） | 同一面板 `scale=2` → 逻辑 800x1280 | `mmsg get all-monitors` |
| 触摸屏 | `NVTCapacitiveTouchScreen` → `/dev/input/event4`，`ID_INPUT_TOUCHSCREEN=1`，`LIBINPUT_DEVICE_GROUP=1c/0/0:input/ts` | `udevadm info`、`/proc/bus/input/devices` |
| 触控笔 | `NVTCapacitivePen` → `/dev/input/event7`，`ID_INPUT_TABLET=1`，152x243mm | 同上 |
| mango 是否接管 | 已打开 `event0…event8`（含 4/7）；seat 能力 `pointer=1 keyboard=1 **touch=1**` | `/proc/678/fd`；自编 wayland 探针（见 §8） |
| 会话协议 | `zwlr_layer_shell_v1`、`zwp_virtual_keyboard_manager_v1`、`zwp_input_method_manager_v2`、`zwp_tablet_manager_v2` 全部在场 | 探针输出 |
| 配置文件 | 只有 `/etc/mango/config.conf`（已直接修改，备份 `.bak-20261025`）；`~/.config/mango/` 仍不存在 | `ls` |
| 现有配置合法性 | 118 个键全部是 0.18.3 的合法键（无静默失效） | 与 `src/config/option_defs.c` 全量比对 |
| 字体 | 系统**零字体**（`fc-list` 为空，wvkbd 画出来全是豆腐块）→ 已装 Noto Sans CJK SC + Noto Sans Symbols2 到 `~/.local/share/fonts` | `fc-list` |
| 现状缺口 | 仍无终端/启动器（`foot`、`rofi` 未装，`Alt+Return`/`Alt+space` 是空绑定）；OSK(wvkbd)、手势(lisgd)、截图(grim) 已就位 | `command -v` |

> 注意：`/etc/mango/config.conf` 里 `Alt+space → rofi`、`Alt+Return → foot` 指向的程序并不存在，
> 是这份最小 rootfs 的遗留绑定，不是触摸问题。

---

## 2. 上游触摸能力时间线（官方 release）

| 版本 | 触摸相关变化 |
|---|---|
| 0.16.0 | **原生触摸支持**（`touch_enable`、`touch_enable_mouse_emulation`） |
| 0.16.1 | 触摸可映射到指定显示器；新增按设备覆盖的 `devicerule` |
| 0.17.0 | **删除** `touch_map_to_mon` / `tablet_map_to_mon`，改为 `devicerule=…,monitor:…` |
| 0.18.0 | **配置键大规模改名**（官方给 `migrate-config-keys.sh`）；新增 `auto_reload_config` |

两次命名变更正是"抄网上配置"最大的坑：Yoga Book 那套是 0.16/0.17 的键名（`devicerule`、`monitorrule`、
`tagrule`、`gappih`、`warpcursor`），直接丢进 0.18.3 会被当成未知键忽略——而且**不报错**。

---

## 3. 0.18.3 真正与触摸有关的配置项

| 键 | 作用 | 建议值 |
|---|---|---|
| `touch_enable` | 总开关；关掉后 seat 不再通告 TOUCH 能力 | `1` |
| `touch_enable_mouse_emulation` | 触摸落在**不接受 `wl_touch`** 的 surface 上时，合成为"左键按下/移动/抬起"（只由第一个触点模拟指针） | 平板上建议 `1` |
| `device_rule=name:<设备名>,monitor:<输出>` | 把触摸/笔钉到某个输出；不设则跟随"当前焦点屏" | 本机 `…,monitor:DSI-1` |
| `gesturebind=MOD,方向,3\|4指,动作` | **仅触摸板**（wlroots 只对触摸板产生 swipe 事件） | 保持现状 |
| `gesture_live` / `gesture_swipe_distance` / `gesture_swipe_cancel_ratio` / `gesture_swipe_min_speed_to_force` / `swipe_min_threshold` | 手势跟随、翻页距离、取消比例、快速甩动阈值 | 按手感微调 |

实现细节（读 `src/input/touch.c` 得到，决定了配置该往哪写）：

- 触摸命中「接受 touch 的 surface」→ 走 `wl_touch` 协议（GTK4 / Qt6 / 浏览器都属此列，体验最好）；
- 否则若 `touch_enable_mouse_emulation=1` → 合成鼠标左键（XWayland、Electron、部分 SDL/旧 Qt 靠这条才能点）；
- 落点会 `wlr_cursor_warp_absolute` 到手指位置（首个触点），也就是"指针跟着手指"。

---

## 4. 网上实际能抄到的东西（逐个评估过）

| 来源 | 是什么 | 对 0.18.3 的可用性 |
|---|---|---|
| [mango 官方 docs: Input Devices](https://mangowm.github.io/docs/configuration/input/) | 唯一权威文档：两个 touch 开关 + `device_rule` 的 monitor 映射 | ✅ 直接可用 |
| [mango 官方 docs: Mouse & Gestures](https://mangowm.github.io/docs/bindings/mouse-gestures/) | `gesturebind` 语法，明确写的是 **trackpad swipe** | ✅ 可用（触摸板） |
| [andrespalumbo311/yogabook-linux-settings](https://github.com/andrespalumbo311/yogabook-linux-settings) | **Lenovo Yoga Book 一代**（纯触摸平板）整套 mango 配置：`touch_enable=1`、触摸钉 `DSI-1`、scroller 满宽单列、wvkbd 常驻 + `toggle-keyboard`、铰链自动旋转守护、GTK layer-shell 控制中心/启动器 | ⚠️ 思路可用，**键名是 0.16/0.17 的**，必须迁移；`inputrule/devicerule/monitorrule/tagrule/gappih/warpcursor` 在 0.18.3 里全是未知键 |
| [crackerjacques/anbernic-mangowm](https://github.com/crackerjacques/anbernic-mangowm) | 掌机（Anbernic）上的 mango 安装脚本 | ❌ 只有手柄/缩放，无触摸逻辑 |
| 主流 dotfiles（hakuspace / cebem1nt / kevsantamaria / kohagizpk / diggingforce…） | 只写触摸板 `gesturebind` 和外观，全文 grep 无 `touch_enable`/OSK/平板内容 | ❌ 无可抄 |
| [mango issue #1514](https://github.com/mangowm/mango/issues/1514) | "Switch focus on touch"：点未聚焦窗口只发触摸、**不改焦点**（仍 open） | ⚠️ 已知缺陷 |
| [mango issue #1274](https://github.com/mangowm/mango/issues/1274) | 多屏 + 分数缩放时触摸落点偏移（0.16 时代） | ⚠️ 单屏无影响 |

一句话：**网上只有两篇"论文"——官方 2 个开关，和 Yoga Book 那一套；没有第三方 curated 的触屏 mango 配置。**

---

## 5. 触屏"手感"其实靠这三件外挂

| 需求 | 方案 | 在本机的可行性 |
|---|---|---|
| 屏幕键盘 | **wvkbd**（wlroots virtual-keyboard；`SIGUSR1` 隐藏 / `SIGUSR2` 显示 / `SIGRTMIN` 切换；`--auto` 需要 `zwp_input_method_v2`） | 协议齐备（探针已确认），但 ALARM 没打包，需自己编（cairo/pango/xkbcommon/wayland-client，设备上 gcc + pkg-config 都在） |
| 屏幕键盘（备选） | **squeekboard**（`extra/squeekboard 1.43.1`，可直接 pacman 装） | 需实测：它依赖 input-method 协议，mango 有 v2，但没有官方验证记录 |
| 触屏手势 | **lisgd**（libinput **合成**手势守护，专为触摸屏设计；不 grab、`send_events=ENABLED`，所以单击仍能到应用）→ 回调里 `mmsg dispatch` 驱动 mango | 可选；需编译（无 AUR 包） |
| 自动旋转 | `iio-sensor-proxy` + `wlr-randr --output DSI-1 --transform 90` | 仓库都有，本机有 3 个 iio 设备 |
| 缩放 | `monitor_rule=…,scale:2` 或 `wlr-randr --output DSI-1 --scale 2` | 现在 `scale=1`，2560 高在 11 吋上字明显偏小，这是"触摸友好"收益最大的一项 |

`mmsg` 从会话外调用时必须带签名（socket 路径），否则报 `MANGO_INSTANCE_SIGNATURE is not set`：

```sh
export XDG_RUNTIME_DIR=/home/magma/.xdg
MANGO_INSTANCE_SIGNATURE=/home/magma/.xdg/mango-678.sock mmsg dispatch viewnext_have_client
```

---

## 6. 已落地（2026-10-25，直接写系统配置，不用 overlay）

用户明确要求"不要 overlay、直接安装"，所以全部改的是系统真身：

| 路径 | 内容 |
|---|---|
| `/etc/mango/config.conf` | 在原 304 行后追加 touch / scale / OSK / gesture 段；备份 `/etc/mango/config.conf.bak-20261025` |
| `/usr/local/bin/wvkbd-mobintl` | 屏幕键盘（设备本机编译，1.0 MB） |
| `/usr/local/bin/lisgd` | 触屏手势守护（设备本机编译，75 KB；**不 grab 设备**，单击仍进应用） |
| `/usr/local/bin/grim` | 截图工具（设备本机编译，用于验收） |
| `/usr/local/bin/mango-dispatch` | 自动找 mango socket 的 mmsg 包装（lisgd 回调、SSH 都能直接用） |
| `/usr/local/bin/mango-osk-toggle` | wvkbd 显隐切换（`SIGRTMIN`），给 `SUPER+k` 和四指下滑用 |
| `/usr/local/bin/mango-touch-gestures` | 按设备名解析触摸 event 节点 → 启 lisgd 并绑定手势，由 `exec_once` 启动 |
| `~/.local/share/fonts/NotoSansCJKsc-Regular.otf`、`NotoSansSymbols2-Regular.ttf` | 系统原本**一个字体都没有**（wvkbd 全是豆腐块），这两个覆盖中英文 + 键盘符号 |

配置增量（0.18.3 合法键，`mango -p` rc=0）：

```ini
touch_enable=1
touch_enable_mouse_emulation=1
device_rule=name:NVTCapacitiveTouchScreen,monitor:DSI-1
device_rule=name:NVTCapacitivePen,monitor:DSI-1
monitor_rule=name:DSI-1,width:1600,height:2560,refresh:120,x:0,y:0,scale:2
bind=SUPER,k,spawn,/usr/local/bin/mango-osk-toggle
exec_once=/usr/local/bin/mango-touch-gestures
```

- `refresh:120` 是 `modetest -c` 读到的真实模式（1600x2560@120，preferred）；`monitor_rule` 的
  width/height/refresh 必须同时给，否则报 `Monitor rule must set width, height and refresh together`。
- `scale=2` → 逻辑分辨率 **800x1280**（竖屏，实测；`wlr-randr` 用不了，mango 没实现
  `zwlr_output_manager_v1`）。
- 手势：3 指左右滑 = 上/下一个有窗口的 tag；3 指上滑 / 4 指上滑 = overview；4 指下滑 = 屏幕键盘。
- wvkbd 参数 `-H 520 -L 360 --fn "Noto Sans CJK SC 22"`：竖屏键盘高 520 逻辑像素（屏幕 40%），横屏 360。

**验收证据（全部实机跑出来）**

| 检查 | 结果 |
|---|---|
| `mango -p -c /etc/mango/config.conf` | `rc=0` |
| `mmsg get all-monitors` | `scale=2 logical=800x1280` |
| `mmsg get all-layers`（开键盘后） | `{"layer":"overlay","name":"wvkbd"}` |
| uinput 注入 3 指滑动 → lisgd | `Cfg (3/2) <=> Evt (3/2)` + 命令执行（PASS） |
| uinput 点屏幕键盘 `q` / 空格 | wvkbd 日志 `get key: +39+216` → `q`、`release key 16`；空格 `release key 57` |
| grim 截图 | 键盘全键位正常（含 ⌫、⌨），无豆腐块 —— 见 `mango-touch-osk.png` |
| 重启会话后 | `exec_once` 自动拉起 lisgd（`-d /dev/input/event4`），`scale=2` 保持 |

**调参与回滚**

```sh
# 键盘高度：-H 竖屏 / -L 横屏（逻辑像素）
sudo sed -i 's/-H 520 -L 360/-H 440 -L 320/' /usr/local/bin/mango-osk-toggle

# 缩放：1.5 更宽松，2 最锐利（改完重启会话）
sudo sed -i 's/scale:2/scale:1.5/' /etc/mango/config.conf

# 手势手感：mango-touch-gestures 里 -t 250（像素阈值） -m 800（毫秒超时）

# 横屏使用（当前是竖屏 800x1280）：monitor_rule 末尾加 rr:90 或 rr:270

# 回滚
sudo cp /etc/mango/config.conf.bak-20261025 /etc/mango/config.conf
sudo rm -f /usr/local/bin/mango-dispatch /usr/local/bin/mango-osk-toggle /usr/local/bin/mango-touch-gestures
sudo systemctl restart getty@tty1
```

---

## 7. 已知坑（踩过或代码里确认过）

1. **`mmsg get all-devices` 永远看不到触摸屏**：`server.input_devices` 只由 keyboard/pointer/switch 填充，
   touch/tablet 不进这个链表。找设备名只能靠 `/proc/bus/input/devices` 或 `udevadm info`。
2. **0.18 键名改名**：老配置静默失效；`migrate-config-keys.sh` 可批量迁移。
3. **`mango -p` 必须指向真实存在的文件**：文件不存在/为空时也可能返回 0，别把它当"配置正确"的证据；
   真正的错误长这样：`[ERROR] Unknown keyword: bordr_px` + rc=1。
4. **重复绑定即冲突**：叠加配置时，再写一遍已有的 4 指 `gesturebind` 或 `SUPER+btn_left` 会让整份配置 rc=1
   （本次安装的增量段刻意避开了已有绑定）。
5. **触摸屏不走 `gesturebind`**：wlroots 只对触摸板发 swipe 事件；触屏手势必须外挂 lisgd 之类。
6. **点未聚焦窗口不改焦点**（#1514）：目前只能靠键盘/tag 切换，或对走鼠标模拟的窗口（触摸即"点击"）间接聚焦。
7. **多屏 + 分数缩放**下触摸坐标可能偏（#1274）；单屏 + `scale` 整数倍最稳。

---

## 8. 复现与验证

```sh
# 1) 配置语法 + 绑定冲突（唯一可信的离线检查）
mango -p -c ~/.config/mango/config.conf ; echo rc=$?

# 2) 运行时状态
export XDG_RUNTIME_DIR=/home/magma/.xdg
S=$(ls ~/.xdg/mango-*.sock | while read s; do MANGO_INSTANCE_SIGNATURE=$s mmsg get all-monitors >/dev/null 2>&1 && echo $s && break; done)
MANGO_INSTANCE_SIGNATURE=$S mmsg get all-monitors
MANGO_INSTANCE_SIGNATURE=$S mmsg get all-devices

# 3) seat 能力 / 协议在场（本次调研用的探针，源码 .build/research/touch/probe.c）
gcc -O0 probe.c -o probe $(pkg-config --cflags --libs wayland-client)
XDG_RUNTIME_DIR=/home/magma/.xdg WAYLAND_DISPLAY=wayland-0 ./probe
# → SEAT_CAPS pointer=1 keyboard=1 touch=1
# → zwlr_layer_shell_v1 / zwp_virtual_keyboard_manager_v1 / zwp_input_method_manager_v2

# 4) 回滚
rm ~/.config/mango/config.conf      # 回到 /etc/mango/config.conf
mmsg dispatch reload_config         # 或 SUPER+r
```

本次安装的验收记录（同一轮里全部通过）：

- `mango -p -c /etc/mango/config.conf` → `rc=0`
- `mmsg get all-monitors` → `scale=2 logical=800x1280`
- `mmsg get all-layers` → 开键盘后 `{"layer":"overlay","name":"wvkbd"}`
- uinput 三指滑动 → lisgd `Cfg (3/2) <=> Evt (3/2)`，回调命令执行成功（marker 文件生成）
- uinput 点击屏幕键盘 → wvkbd 日志 `get key: +39+216` → `q` / `release key 16`；空格 `release key 57`
- 重启 `getty@tty1` 后：`exec_once` 自动拉起 lisgd，缩放仍为 2，键盘可切换

---

## 9. 参考链接

- mango 文档 · 输入设备：<https://mangowm.github.io/docs/configuration/input/>
- mango 文档 · 鼠标与手势：<https://mangowm.github.io/docs/bindings/mouse-gestures/>
- release 0.16.0（原生触摸）：<https://github.com/mangowm/mango/releases/tag/0.16.0>
- release 0.16.1（触摸映射 + devicerule）：<https://github.com/mangowm/mango/releases/tag/0.16.1>
- release 0.17.0（删 `touch_map_to_mon`）：<https://github.com/mangowm/mango/releases/tag/0.17.0>
- release 0.18.0（键名迁移脚本 + `auto_reload_config`）：<https://github.com/mangowm/mango/releases/tag/0.18.0>
- issue #1514（触摸聚焦，open）：<https://github.com/mangowm/mango/issues/1514>
- issue #1274（触摸坐标偏移）：<https://github.com/mangowm/mango/issues/1274>
- Yoga Book 触屏平板全套配置：<https://github.com/andrespalumbo311/yogabook-linux-settings>
- wvkbd：<https://github.com/jjsullivan5196/wvkbd>
- lisgd：<https://git.sr.ht/~mil/lisgd>（镜像 <https://github.com/jjsullivan5196/lisgd>）
- ArchWiki MangoWM（无触摸章节）：<https://wiki.archlinux.org/title/MangoWM>
