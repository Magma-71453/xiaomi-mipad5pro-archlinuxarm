# MangoWM 0.18.3 (aarch64) —— 构建产物与 rootfs 装配要点

- 构建方式：**在 elish 设备上原生编译**（aarch64 → aarch64，非交叉）
- 产物目录：`output/20261024/mango/`
  - `stage/` —— meson install 出来的完整安装树（可按键 rootfs 的 / 直接铺开）
  - `MANIFEST.mango` —— 文件 / 目标分区 / SHA256 / 构建时间 / 大小（`sha256sum -c` 可验）
  - `mango-0.18.3-aarch64.tar.zst` —— stage + logs 打包（1.2 MB）
  - `logs/` —— setup / build / install 原始日志
- 复现脚本：`scripts/build-mango-on-device.sh`（见 §4）
- 构建细节与踩坑：`docs/findings/mango.md`

版本：mango 0.18.3（upstream tag `8b876cb31d4b0a393f4438e5a89b2f33a865d77b`），
依赖线 wlroots-0.20 **0.20.2** / scenefx-0.5 **0.5.0**（满足上游 `>=0.20.0` / `>=0.5.0`）。

---

## 1. 装配路径表（全部 target=linux，路径相对 rootfs 的 /）

| 产物（stage/ 下） | 装到 rootfs | 权限 | 说明 |
|---|---|---|---|
| bin/mango | /usr/bin/mango | 0755 | 合成器主程序，3355704 B |
| bin/mmsg | /usr/bin/mmsg | 0755 | IPC 客户端 |
| bin/mangonag | /usr/bin/mangonag | 0755 | 配置/消息辅助工具 |
| share/wayland-sessions/mango.desktop | /usr/share/wayland-sessions/mango.desktop | 0644 | **会话入口**：`Exec=mango`，`DesktopNames=mango;wlroots`；显示管理器（SDDM/ly/greetd）靠它列出会话 |
| etc/mango/config.conf | /etc/mango/config.conf | 0644 | 默认配置。编译期 `-DSYSCONFDIR="/etc"` → 路径固定 /etc/mango/ |
| etc/mango/config.toml | /etc/mango/config.toml | 0644 | TOML 版配置 |
| share/xdg-desktop-portal/mango-portals.conf | /usr/share/xdg-desktop-portal/mango-portals.conf | 0644 | portal 后端（ScreenCast/Screenshot 走 wlr） |
| lib/systemd/user/mango-session.target | /usr/lib/systemd/user/mango-session.target | 0644 | 让 `WantedBy=graphical-session.target` 的用户服务随会话启动 |
| share/man/man1/mmsg.1 | /usr/share/man/man1/mmsg.1 | 0644 | 手册 |
| share/man/man1/mangonag.1 | /usr/share/man/man1/mangonag.1 | 0644 | 手册 |

装配（在 rootfs 内做）：

```sh
# stage 的布局就是 prefix 根：bin/ share/ lib/ + 一个 /etc
cp -a stage/. "$ROOTFS/usr/"                                   # bin/ share/ lib/
mkdir -p "$ROOTFS/etc" && cp -a stage/etc/. "$ROOTFS/etc/"     # /etc/mango 必须落 /etc/mango
```

> 关键：`stage/etc/mango/` **不要**放进 `/usr/etc/`。构建时 prefix 是设备上的
> `$HOME/elish-build/stage`，而 `sysconfdir` 被 meson.build 归一化成 `/etc`，
> 所以运行期只从 **/etc/mango/** 读配置。

## 2. 运行时依赖包清单（rootfs 要能装上）

由 `ldd bin/mango` / `readelf -d` 的 NEEDED 反推，按 ArchLinuxARM 包名：

**必装（当前设备 pacman 里缺的就是这 4 个）**

```sh
wlroots0.20     # /usr/lib/libwlroots-0.20.so —— 注意是**不带 .so.N 的 soname**，由主包直接提供
scenefx         # /usr/lib/libscenefx-0.5.so，同样没有 .so.N
libliftoff      # libliftoff.so.0（wlroots 的 KMS plane 后端）
cjson           # libcjson.so.1
```

**其余（标准 ALARM 已装，确认存在即可）**

```sh
wayland libinput libxkbcommon libdrm pixman mesa libglvnd lcms2
libdisplay-info systemd-libs          # libudev
seatd                                 # libseat
vulkan-icd-loader
pango cairo glib2 pcre2 freetype2 fontconfig harfbuzz libpng
xcb-util-wm                           # xcb-icccm
xcb-util-renderutil xcb-util-errors
xorg-xwayland                         # 编译时 -DXWAYLAND，XWayland 集成已编进去
```

`wayland-protocols` **只是构建期依赖**（供 wayland-scanner 读 xml），rootfs 运行期不需要。

运行前置：
- 需要 seat/DRM 会话：`seatd` 或 logind；显示管理器会给 `XDG_SESSION_TYPE=wayland`
- 用户需在 `video` / `render` / `input` 组（本设备 elish 已在 983 video / 987 render / 992 input）
- 从 TTY 手起：`XDG_RUNTIME_DIR=/run/user/$(id -u) mango`（或 `dbus-run-session mango`）
- 首次启动读 `/etc/mango/config.conf`（或 `config.toml`），缺失则用内置默认

## 3. 校验

```sh
cd output/20261024/mango && sha256sum -c MANIFEST.mango    # 10/10 OK
file stage/bin/mango
#  ELF 64-bit LSB pie executable, ARM aarch64, dynamically linked,
#  interpreter /lib/ld-linux-aarch64.so.1, BuildID[sha1]=ebfbc887..., with debug_info, not stripped
```

双证据（编译期解析 vs 运行期实际）：

| 库 | pkg-config --modversion | ldd 实际指向（设备 sysroot） |
|---|---|---|
| wlroots-0.20 | 0.20.2 | /home/elish/elish-build/sysroot/usr/lib/libwlroots-0.20.so |
| scenefx-0.5 | 0.5.0 | /home/elish/elish-build/sysroot/usr/lib/libscenefx-0.5.so |
| libcjson | 1.7.19 | .../sysroot/usr/lib/libcjson.so.1 |
| libliftoff | 0.5.0 | .../sysroot/usr/lib/libliftoff.so.0 |

构建质量：147 个 target，**error 0 / warning 0**；首次全量 8s、冷重编 20s（-j8）；
两次构建 `bin/mango` sha256 一致：`18e38c750e001dcb9fe6a0ec0a652c96706901e72333740b350b02f00ec27894`。

## 4. 重新构建（宿主一键，可重入）

```sh
bash elish-archlinux/scripts/build-mango-on-device.sh            # 幂等增量
bash elish-archlinux/scripts/build-mango-on-device.sh --rebuild  # 丢弃远端构建树冷重编
bash elish-archlinux/scripts/build-mango-on-device.sh --clean    # 连远端 `/elish-build 一起清
```

流程：预检 → 下载（sha256 固定）→ 推送（只传远端哈希不一致的）→ 远端解包 sysroot 并改写 .pc 前缀
→ 装 meson（wheel 解包，不进系统）→ meson setup / ninja / meson install → 远端 tar.zst 取回
→ 生成 MANIFEST.mango → 验收（file / pkg-config / ldd / sha256sum -c）。

可覆盖变量（全部带默认值，无裸硬编码路径）：

| 变量 | 默认 |
|---|---|
| DEV_HOST / DEV_USER / DEV_SSH_KEY | 192.168.1.90 / elish / `~/.ssh/id_ed25519 |
| DEV_BASE | /home/elish/elish-build（唯一允许活动的设备路径） |
| DEV_JOBS / BUILD_TYPE | 8 / release |
| MANGO_VER / MANGO_URL / MANGO_SHA256 | 0.18.3 / codeload / 9b9c66c5… |
| MESON_WHEEL_VER / MESON_WHEEL_URL / MESON_WHEEL_SHA256 | 1.12.1 / pypi files / 930bc754… |
| ALARM_MIRROR / ALARM_SUBDIR | http://mirror.archlinuxarm.org / extra |
| ROOT_DIR / REPO_DIR / BUILD_DIR / OUT_DIR | git-build / elish-archlinux / .build/mango / output/20261024/mango |

## 5. 未决事项

- 未在设备上真跑合成器（需要 seatd + 控制台会话）；当前只有静态证据（file/ldd/pkg-config/sha256）。
- 二进制未 strip（3.35 MB；上游 meson.build 无条件加 `-g`）。若空间敏感可 `strip --strip-unneeded`，
  但那会改 sha256，需要重算 MANIFEST。
- 设备上那 5 个依赖包以 sysroot 形式存在，**rootfs 必须走 pacman 正常安装**（§2），
  不要把 `/elish-build/sysroot` 拷进 rootfs。
- rootfs 侧尚未验证 seatd/权限/XDG_RUNTIME_DIR 是否齐备。

## 6. 触摸 / 平板能力（2026-10-25 装入并验收）

mango 0.16.0 起有原生触摸，但只有两个开关 + 设备级映射；触屏手势必须外挂。本次直接改系统配置
（不用 overlay），明细与调研结论见 [`docs/research/mango-touch.md`](research/mango-touch.md)。

| 位置 | 内容 | 来源 |
|---|---|---|
| `/etc/mango/config.conf` | 追加 touch / scale / OSK / gesture 段（原文件备份 `.bak-20261025`） | 本仓库 `configs/rootfs/etc/mango/config.conf` |
| `/usr/local/bin/mango-dispatch` | 自动找 mango socket 的 mmsg 包装 | `configs/rootfs/usr/local/bin/` |
| `/usr/local/bin/mango-osk-toggle` | wvkbd 显隐切换（`SIGRTMIN`），`SUPER+k` / 四指下滑 | 同上 |
| `/usr/local/bin/mango-touch-gestures` | 解析触摸 event 节点 → 启 lisgd，`exec_once` 自启 | 同上 |
| `/usr/local/bin/wvkbd-mobintl` | 屏幕键盘，设备本机 `make wvkbd-mobintl` | 上游 jjsullivan5196/wvkbd（**未入库，需重编**） |
| `/usr/local/bin/lisgd` | 触屏手势守护，设备本机 `make` | 上游 jjsullivan5196/lisgd（**未入库，需重编**） |
| `/usr/local/bin/grim` | 截图（验收用），设备本机 meson 构建 | 上游 emersion/grim（**未入库，需重编**） |
| `~/.local/share/fonts/NotoSansCJKsc-Regular.otf`、`NotoSansSymbols2-Regular.ttf` | 系统原本零字体，OSK 全是豆腐块 | notofonts（**未入库**） |

关键增量配置：

```ini
touch_enable=1
touch_enable_mouse_emulation=1
device_rule=name:NVTCapacitiveTouchScreen,monitor:DSI-1
device_rule=name:NVTCapacitivePen,monitor:DSI-1
monitor_rule=name:DSI-1,width:1600,height:2560,refresh:120,x:0,y:0,scale:2
bind=SUPER,k,spawn,/usr/local/bin/mango-osk-toggle
exec_once=/usr/local/bin/mango-touch-gestures
```

- 缩放：`scale=2` → 逻辑 800x1280（竖屏）。`mango` 未实现 `zwlr_output_manager_v1`，`wlr-randr` 不可用，
  只能用 `monitor_rule`；width/height/refresh 必须同时给（`modetest -c` 读到 1600x2560@120 preferred）。
- 手势：3 指左右滑切 tag、3/4 指上滑 overview、4 指下滑切键盘；lisgd 不 grab，单击仍进应用。
- 屏幕键盘：wvkbd `-H 520 -L 360 --fn "Noto Sans CJK SC 22"`；竖屏高 520 逻辑像素（40% 屏高）。

重建触摸外挂件（设备上）：

```sh
cd ~/elish-build/src
git clone --depth 1 https://github.com/jjsullivan5196/wvkbd && (cd wvkbd && make wvkbd-mobintl)
git clone --depth 1 https://github.com/jjsullivan5196/lisgd && (cd lisgd && make)
sudo install -m755 wvkbd/wvkbd-mobintl lisgd/lisgd /usr/local/bin/
```

验收（实机全过）：`mango -p` rc=0；`mmsg get all-monitors` scale=2 / 800x1280；`mmsg get all-layers`
有 `wvkbd` overlay；uinput 注入三指滑动触发 lisgd 回调；uinput 点键盘 `q`/空格，wvkbd 输出 keycode 16/57；
重启 `getty@tty1` 后 `exec_once` 自动拉起 lisgd。截图：`docs/research/mango-touch-osk.png`（实机 grim 抓的竖屏键盘）。

## 7. 桌面外壳：横屏 / 全宽 mangobar / 左下 dock / 编辑模式（2026-10-25 第三版）

### 7.1 横屏

```ini
monitor_rule=name:DSI-1,width:1600,height:2560,refresh:120,x:0,y:0,scale:2,rr:3
```

`rr:3` = 270°（画面顺时针 90°）→ 逻辑 1280x800。触摸标定：`x_log = y_dev/2`、`y_log = (1600 - x_dev)/2`。

### 7.2 窗口：回到平铺（不再全局浮动）

删掉了 `window_rule=app_id:.*,is_floating:1`；保留 `border_radius=24`、`shadows=1`。
平铺窗口实测 `10,52 1260x738 float=False`（bar 的 exclusive zone 已让出顶部 36px）。

### 7.3 mangobar：全宽

`style.css` 的 `#bar { margin: 6px 0px; }`，可视宽度回到 1280；`mango-shell-watch` 的
`BAR_RECT=(0,0,1280,48)` 同步更新。编辑模式仍是 `config-edit.jsonc`（多一个橙色 `editor` 按钮）。

### 7.4 mango-dock：左下角、4 个图标、动态长度

- 锚点 `BOTTOM|LEFT`，左边距 14、下边距 14（不再水平居中）
- 图标：kitty / firefox / rofi ｜分隔线｜ edit（编辑模式开关）
- 宽度随图标数量变化：4 个图标 = 296 逻辑像素，几何写入 `$XDG_RUNTIME_DIR/mango-dock.geom`
- `exclusive_zone = 82`：dock 自己占住底部一条，平铺窗口不会再压到它，因此 dock 不需要再自动隐藏
  （bar 的自动避让仍保留）

### 7.5 编辑模式（重新设计触发方式）

三指长按 3 秒太别扭，已改成：

- **进**：点 dock 最右边的橙色四向箭头，或 `SUPER+e`
- **行为**：单指滑动 = 拖动手指下的窗口；若窗口原本是平铺的，会先 `togglefloating` 再拖
- **出**：点 mangobar 右侧的 `editor` 胶囊，或再按一次 `SUPER+e`

实现注意：编辑模式**不再**关闭 `touch_enable`（否则 mangobar 的 editor 按钮也点不到）；
拖动灵敏度加了 1.6 倍系数 + 小数累积，避免 `movewin` 取整导致"黏手"。

### 7.6 验收

| 检查 | 结果 |
|---|---|
| 布局 | 全宽 bar（1280）+ 左下角磨砂 dock（296x68 @ x=14,y=718） |
| 平铺 | kitty `10,52 1260x738 float=False` |
| dock 编辑按钮 | uinput 点第 4 个图标 → `mango-edit-mode` 出现、bar 切 `config-edit.jsonc`、出现 `editor` |
| 拖动 | 编辑模式下拖平铺窗口会先转浮动再移动（`togglefloating` + `movewin client,<id>`） |

### 7.7 整理模式：拖动方向与 bar 消失的修复（2026-10-25）

两个真 bug：

1. **拖动方向反了** —— `movewin` 的 dy 是"屏幕向下为正"，之前代码里写了 `-dy`，所以手指上滑窗口反而下滑。
   实测：`movewin,+0,+100` 让窗口 y 从 -208 → -108（下移），确认后改成直接用 `dy`。
   修复后注入验证：手指上移 100 逻辑像素 → 窗口 y 从 56 → -44（上移 100）✓
2. **整理模式里 mangobar 消失** —— `mango-shell-watch` 把"窗口压到 BAR_RECT"当成要隐藏 bar，而整理模式的
   格子就在 y=56 附近，于是 bar 被停掉、pencil 按钮点不到、无法退出。现在**bar 永不自动隐藏**；
   另外格子区域下移到 `AREA_TOP=56`（bar 高 48），整理模式下 `mangobar` 保持在 `overlay` 层、位于捕获层之上。

顺带加固：

- `mango-arrange` 把浮动窗口 id 写进 `$XDG_RUNTIME_DIR/mango-arrange.state`，守护进程崩了再启动会自动
  把这些窗口 `togglefloating` 回平铺，避免"卡在浮动状态出不来"
- 覆盖层不再同时处理 pointer + touch（触摸设备下 pointer 是模拟事件，会造成位移翻倍）
- 覆盖层去重：`pkill -f '^/usr/local/bin/mango-arrange-overlay'`（`pkill -x` 对 >15 字符的进程名无效）

实测闭环：平铺 `10,52 1260x738 float=False` → 进整理模式（bar 在、窗口浮动 (16,56)）→ 单指拖动方向正确
→ 退出后精确回到 `10,52 1260x738 float=False`。

遗留：整理模式里窗口**尺寸**没落到格子大小（`resizewin` 相对增量会被浮动窗口的约束吃掉，实测 1164x250
而不是 1236x628），位置是对的。要精确尺寸得改成"先算目标尺寸再一次性设置"，mango 目前只暴露相对
`resizewin`，可能需要给 mango 加一个绝对 resize 的 dispatch。

### 7.8 退出整理模式时按位置重排平铺（2026-10-25）

需求：整理模式里把 A 拖到 B 右边，退出后平铺顺序也要变成 B、A。

实现（`mango-arrange.retile()`）：

1. **先在浮动状态下记录"人眼顺序"**：按 `(round(y/140), x)` 排序 —— 先分行（行高 140 逻辑像素），行内按 x；
   这样多行多列的情况也能读成"从上到下、从左到右"的线性顺序
2. **再全部 `togglefloating` 回平铺**（此时位置会变回布局位置）
3. **最后用相邻交换排序**：`exchange_client,left client,<id>` 一次挪一格，把 mango 的 client list 排成目标顺序
   （冒泡式，复杂度 O(n²)，n 是当前 tag 的窗口数，几十个窗口都没问题）

关键坑：**`exchange_client` 只对平铺窗口生效** —— 窗口还是浮动状态时调它，顺序不变。所以顺序必须是
"先取消浮动、再交换"，而"人眼顺序"必须在那之前用浮动态坐标算好。

实测（tag 3 上两个 kitty）：

| 步骤 | 结果 |
|---|---|
| 初始平铺 | `[(14, x=10), (13, x=716)]` |
| 进整理模式，把 13 拖到 x=-60（14 左边） | `[(14, x=16), (13, x=-60)]` |
| 退出 | `[(13, x=10), (14, x=716)]` —— 13 成为 master，14 到右边 ✓ |

tag 1 的 firefox / deskflow 全程未受影响。

### 7.9 悬浮小键盘 + dock 按钮（2026-10-25）

原来的 wvkbd 会 `set_exclusive_zone(height)`，一弹出就把平铺窗口挤小。wvkbd 本身有开关：

```sh
wvkbd-mobintl --non-exclusive -H 360 -L 210 --alpha 235 --fn "Noto Sans CJK SC 18"
```

- `--non-exclusive` → `exclusive_zone` 不再设置，键盘浮在窗口之上，**窗口尺寸不变**
- `-L 210`（横屏高度，逻辑像素）+ `--alpha 235` → 更小、半透明，不挡视线
- 实测：键盘弹出前后 firefox 738 / deskflow 366 / kitty 367 完全不变 ✓

dock 现在有 5 个图标，顺序：`kitty | firefox | rofi ‖ 键盘 | 整理`，键盘在整理左边：

| 图标 | 动作 |
|---|---|
| `kbd.png`（键盘线框） | `mango-osk-toggle`（首次点击直接显示，不再需要点两下） |
| `arrange.png`（橙色四向箭头） | `mango-arrange-toggle` |

另外：
- dock 挪到 layer `overlay`，这样整理模式的全屏捕获层（layer `top`）盖不住它，键盘/整理按钮在整理模式下照样能点
- mangobar 右上角的整理按钮从 `\ue70f` 换成 `✔`（Nerd Font 下 U+E70F 渲染成九宫格，不像"整理"）
- 去掉了 wvkbd 的开机自启（否则会以 `--hidden` 旧参数占位，第一次点按钮只发 SIGRTMIN 不会显示）

### 7.10 键盘并入 mangobar、整理模式图标与进入失败的修复（2026-10-25）

- **键盘按钮从 dock 移回 mangobar**：`modules-right: ["custom/kbd", "custom/editor", "network"]`，
  键盘 chip 用 `⌨`（U+2328）、深灰底；整理 chip 换成 `▦`（U+25A6，网格语义），橙色底
- dock 回到 4 个图标：`kitty | firefox | rofi ‖ 整理`（橙色四向箭头）
- 键盘改成**居中**：给 wvkbd 打了个小补丁，环境变量 `WVKBD_CENTER_W=1280` 时锚定
  `BOTTOM` 并把左边距设为 `(屏宽 - 键盘宽)/2`，配合 `--width 680` 得到 680x220 居中键盘；
  仍带 `--non-exclusive`，不占窗口空间
- **整理模式进不去**：原因是守护进程在之前的调试里被 `pkill` 掉覆盖层后，`self.server/self.conn`
  指向已失效的 socket，`enter()` 里 `start_overlay()` 失败但状态仍标记为 active，之后 toggle 只会来回删/建
  flag 而不重建覆盖层。修复：
  - `enter()` 一律先 `stop_overlay()` 再 `start_overlay()`（清掉陈旧 socket 和进程）
  - 主循环检测 `overlay.poll()`，覆盖层死了自动重建
  - 实测：点 bar 上的 `▦` → flag 出现、`mango-arrange` 层出现（`mmsg get all-layers` 可见）

### 7.11 音量键 → 设备音量（2026-10-25）

设备音频链路现状：

- `snd_soc_sm8250`（Xiaomi Mi Pad 5 Pro），1240 个 ALSA 控件，其中 8 个放大器
  `BLL/BLR/BLH/BRH/TLL/TLR/TLH/TRH Digital PCM`（0..913，可写，**没有 switch**）
- PipeWire 能起来但只有 `Dummy Output`（没有可用 profile/UCM），所以音量走 **ALSA 直控**
- 设备树里 gpio-keys 只有 `key-vol-up`（`linux,code=115` = KEY_VOLUMEUP）和 `switch-lid`

mango 的键盘绑定按 keysym 匹配，而内部音量键经当前 keymap **不解析成 `XF86AudioRaiseVolume`**
（实测：注入 `KEY_F1` 能触发 `bind=none,F1`，注入 `KEY_VOLUMEUP` 不触发任何绑定），所以改成 evdev 守护：

| 文件 | 作用 |
|---|---|
| `/usr/local/bin/mango-keys` | 监听所有 `/dev/input/event*`（每 1s 重扫热插拔），KEY_VOLUMEUP/DOWN/MUTE(115/114/113) → `mango-volume` |
| `/usr/local/bin/mango-volume` | `up/down/mute/set N`：读当前 `BLL Digital PCM` 百分比，±5%，写入全部 8 个 `*Digital PCM`；mute 因为没有 switch，实现为"记下当前值→设 0%"，再按一次恢复 |
| `mango-shell-start` | 与 touchd/arrange/dock/watcher 一起拉起 |

实测（uinput 注入按键）：

```
45% --KEY_VOLUMEUP--> 50% --KEY_VOLUMEDOWN--> 45%
45% --KEY_MUTE--> 0% --KEY_MUTE--> 45%
```

注意：设备树里只有 `key-vol-up`，**音量减键在这个内核里没有节点**；守护进程对 114 也做了处理，
如果物理音量减键实际发出 KEY_VOLUMEDOWN 就会生效，若按下无反应则是内核 keymap 缺这一项。

### 7.12 音量 OSD + 整理模式网格修正（2026-10-25）

**音量 OSD**：新程序 `mango-osd`（C + cairo，wlr-layer-shell）

- 屏幕中上方 360x92 的圆角胶囊，draw 出喇叭图形 + 橙色电平条 + 百分比
- `mango-volume` 每次改音量把值写进 `$XDG_RUNTIME_DIR/mango-osd-value` 并 `SIGUSR1` OSD
- OSD 自己计时 1.5 s 后 `attach(NULL)` 隐藏，不需要外部定时器
- 图层 `overlay`、`exclusive_zone=-1`，不占空间；`TOP_MARGIN=58` 避开 48px 的 bar
- 启动方式：`mango-shell-start` 里与 dock 一起拉起

坑：第一次实现时在 `main()` 里先 commit 再 roundtrip 又 attach(NULL)，触发
`zwlr_layer_surface_v1: wrong configure serial`。改法：初始只 commit 一次、不 attach；
`ls_configure` 里按 `visible` 决定画内容还是挂 NULL。

**整理模式网格修正**（之前窗口尺寸贴不准）：

- 列数按数量决定：1 个 1 列、2 个 2 列、n 个 `ceil(sqrt(n))` 列；行数 `ceil(n/cols)`
- 布局用"最多 3 轮修正"：每轮重新读取实时几何，`resizewin` 差值 > 8px、`movewin` 差值 > 4px 就再发一次，
  直到不再变化；这解决了浮动窗口尺寸异步生效导致"只贴准位置、贴不准尺寸"的老问题
- 实测 3 个窗口 → `(16,56,612x302)`、`(640,56,612x302)`、`(16,370,612x302)`，2x2 网格、不重叠 ✓

### 7.13 整理模式拖动"阻尼"根因与修复（2026-10-25）

现象：手指拖动窗口时移动量只有手指位移的 ~1/7，像有很重的阻尼。

根因有两层，都在 mango 的 IPC 用法上：

1. **每次 dispatch 都新建连接，发送后立刻 close**，但服务端会回一个 `{"success":true}`；
   带着未读数据 close 会让内核发 **RST**，请求在服务端被丢弃 —— 50 次拖动只生效约 7 次。
2. 试过"复用一条长连接"：mango 的 IPC **每个连接只处理一条命令**，后续命令被静默忽略
   （实测发 50 条 `movewin` 窗口纹丝不动）。

最终实现（`dispatch_fast`）：**每条命令一条连接，发完必须把回复读到 `}` 再 close**。

实测（注入 1 秒、200 逻辑像素的拖动）：

| 实现 | 窗口实际位移 |
|---|---|
| 旧：新建连接 + 立即 close | +28px / +56px |
| 复用长连接 | 0px（只有第一条生效） |
| 现在：每条命令读回复再关 | **+200px（与手指 1:1）** |

顺带修的：`mango-shell-start` 现在显式 `export XDG_RUNTIME_DIR=$HOME/.xdg`，各 python 守护进程的
`rt()` 也优先用带 `wayland-0` 的 `~/.xdg` —— 之前从 ssh 手动拉起守护进程时会去 `/run/user/1000`
找 flag 文件，表现为"点了整理模式没反应"。

### 7.14 平铺窗口编辑模式（dock 十字）— 2026-10-25 进行中

需求：点 dock 的十字 → 进入"平铺编辑模式"，屏蔽应用输入；拖窗口中心 = 抓起并放到别的窗口位置（交换布局），
拖窗口边缘 = 改大小。

已实现（`mango-arrange` 现在支持两个模式，用两个 flag 文件区分）：

| flag | 模式 | 触发 |
|---|---|---|
| `$XDG_RUNTIME_DIR/mango-arrange` | 网格整理（原有） | mangobar 的 `▦` |
| `$XDG_RUNTIME_DIR/mango-tileedit` | 平铺编辑（新） | dock 的十字 |

平铺编辑逻辑：

- `tile_down`：命中窗口 → 算点击点离哪条边最近（≤28px）→ 记 `edge`；顺手 `togglefloating` + `focusid`
- `tile_move`：有 `edge` 就 `resizewin`（按边方向取正负），否则 `movewin` 跟手
- `tile_up`：没拖边缘且落点在另一个窗口上 → 先把浮动窗口收回平铺，再用 `reorder_to()`（交换排序）把
  被拖窗口插到目标窗口的位置；拖边缘则直接收回平铺

验证状态（诚实记录）：

- ✅ 模式切换（日志 `tileedit mode on/off`）、覆盖层捕获输入、命中测试（日志 `tile down 299,400 -> 35`、
  `tile up ... target=26`）
- ❌ 落点交换尚未跑通：`exchange_client` 在"刚取消浮动"的窗口上仍不总是生效；且最近两次注入拖动
  连 `tile down` 都没进日志（覆盖层 socket 连接偶发没建立），需要再排查 accept/连接时序

结论：骨架和交互判定都在，但**交换顺序这一步还没验收**，暂时不要把 dock 十字当稳定功能用。

### 7.15 整理/平铺编辑模式的"回弹抖动"与可靠性修复（2026-10-25）

**回弹抖动**（松手后窗口先回到原位、下一帧才进新布局）根因：`exchange_client` 在源码里
（`src/dispatch/client.c:179`）第一句就是 `if (!c || c->isfloating) return 0;` —— 浮动窗口不能重排，
所以必须先取消浮动再交换。之前是"取消浮动 → sleep 0.45s → 交换"，那 0.45 秒就是用户看到的"回弹"。

修复：

- 取消浮动和交换拆成两次 `batch()`：第一次只发 unfloat，**隔 20 ms**（低于 60 Hz 的一帧）再发交换
- `batch()` 在**一条连接**里发多条命令（实测 mango 一个连接可以处理多条），避免多次连接往返
- 交换序列在前端用本地列表模拟，算出每步 `exchange_client,left client,<id>`，一次发完

**可靠性**：

- 覆盖层的 socket 连接失败时不再放弃：主循环里每 500 ms 重试 connect
- 守护进程侧 accept 持续生效，连接断了会重新接受

**可视反馈**：

- 覆盖层新增 `box x y w h` / `nobox` 命令，拖动时给落点窗口画一圈橙色描边（`box` 已实现，还没截图验收）

**实测（两个窗口）**：

```
拖动前  [39@(10,52) 39 为 master, 38@(1019,52)]
拖动    master 中心 -> 38 所在位置
拖动后  [38@(10,52) 38 变 master, 39@(1019,52)]   ✅ 顺序交换成功
```

仍未验收：边缘缩放（测试时起点没落在 28px 边缘区内，误触发了交换）、`box` 高亮的视觉效果。

### 7.16 重启后"配置全没了"的真相：XDG_RUNTIME_DIR 变了（2026-10-25）

用户重启桌面后发现 dock / bar / 键盘全不见，但 `/etc/mango/config.conf`、`/usr/local/bin/mango-*`、
`~/.config/mangobar/` 都还在（核对过时间戳和内容）。

根因：`~/.bash_profile` 被改成优先用 **logind 的 `/run/user/<uid>`**（为了让 PipeWire/Firefox 找到 socket）：

```sh
if [ -d "/run/user/$(id -u)" ]; then rt="/run/user/$(id -u)"; else rt="$HOME/.xdg"; fi
```

而我的脚本是硬编码 `$HOME/.xdg` 的：
- `mango-shell-start` 强制 `export XDG_RUNTIME_DIR=$HOME/.xdg`
- 各守护进程的 `rt()` 去 `~/.xdg` 找 `mango-*.sock` / flag 文件

结果：mango 的 socket 在 `/run/user/1000/mango-864.sock`，而守护进程在 `~/.xdg` 里扫到一堆**上次会话的僵尸
socket**，connect 失败 → `mangobar` / `mango-dock` 启动即退出（变成 defunct），touchd/arrange/keys 全部
连不上实例。所以看起来"配置没了"，其实是"组件起来了但连错了地方"。

修复（全部改成动态发现，`~/.xdg` 只作兜底）：

```python
def rt():
    env = os.environ.get("XDG_RUNTIME_DIR")
    if env and os.path.isdir(env) and any(n.startswith("wayland-") for n in os.listdir(env)):
        return env
    return os.path.expanduser("~/.xdg")
```

`mango-shell-start` 同样改为"跟随会话的 XDG_RUNTIME_DIR"，不再强制 `~/.xdg`。

顺带修了 OSD 的一个真 bug：`layer_surface has never been configured` —— 启动时因为文件 mtime 初始化触发
`show_value()` → `draw()` 早于第一次 `configure`。现在加 `configured` 标志，未 configure 前只记录状态不绘制。

验证：重启 shell 后 `mango-touchd / mango-dock / mango-osd / mango-arrange / mango-keys / mango-shell-watch /
mangobar` 七个组件全部在，`mmsg get all-layers` 有 mangobar + mango-dock，改音量 OSD 正常弹出并在 1.5s 后消失。

### 7.17 会话入口切到 fish、去掉 waybar、删除 mango-startup.sh（2026-10-25）

设备默认会话 shell 是 **fish**（`~/.config/fish/config.fish` 里 `status is-login` 分支启动 mango），
并且这个分支会优先把 `XDG_RUNTIME_DIR` 设成 logind 的 `/run/user/<uid>` —— 这是标准做法，**保留不动**。

本次调整：

| 项 | 处理 |
|---|---|
| `/usr/local/bin/mango-startup.sh` | **删除**（仓库里的副本也删了）。它原来做三件事：`wlr-randr` 设 scale 1.4（mango 没实现 output-management，本来就是空操作）、起 swaybg、起 waybar |
| fish 里的 `-s mango-startup.sh` | 去掉。mango 现在不带 `-s`，桌面组件由 `/etc/mango/config.conf` 的 `exec_once=/usr/local/bin/mango-shell-start` 拉起 |
| waybar | **不再启动**（`mango-shell-start` 里显式 `pkill -x waybar`），状态栏统一用 mangobar |
| 壁纸 | 移到 `mango-shell-start`：`pgrep -x swaybg || setsid swaybg -c '#1a1a2e'` |
| XDG_RUNTIME_DIR | **不改**，跟随 logind；`mango-shell-start` 只在变量为空/目录不存在时才回退 `~/.xdg` |

重启 `getty@tty1` 后的实测进程表：

```
mango-touchd / mango-dock / mango-osd / mango-arrange / mango-keys / mango-shell-watch / mangobar / swaybg
（无 waybar）
```

图层：`background/wallpaper` + `overlay/mangobar` + `overlay/mango-dock`；
mango 进程的 `XDG_RUNTIME_DIR=/run/user/1000`（未被改写）。

---

## 8. 交接声明

桌面外壳（mangobar / dock / OSD / 手势 / 音量 / 编辑模式）与会话入口（fish / 壁纸 / mango 启动）
分属两路维护时，以 [`docs/handoff-mango-shell.md`](handoff-mango-shell.md) 为准：
那里列了全部新增文件、`/etc/mango/config.conf` 增量、`$XDG_RUNTIME_DIR` 下的运行时契约、
被删除/接管的对象，以及"合并时不要破坏的 6 条约束"。

---

## 9. v2 桌面重写（2026-10-25 晚，v2.1 去掉自绘标题条）

> v2.1：macOS 风格自绘标题条已删除（用户反馈视觉突兀、CSD 应用叠加两层）。
> 窗口控制交还应用：kitty 用自带 tab bar（`~/.config/kitty/kitty.conf`），Firefox 用自带标签栏。
> 新增 `mango-place` 只做"新窗口 11/16×7/10 + 居中 + 级联 26px"，不绘制任何 UI。
> kitty 的 `window_title_bar` 和 tab bar 最终都关掉了（标题只有 cwd 的 `~`，没实用信息）：
> `window_title_bar_min_windows 0` + `tab_bar_style hidden`。窗口一律不加自绘标题栏。

v1 的平铺 + 整理/编辑模式整体废弃。v2：

- 窗口默认悬浮（`window_rule=app_id:.*,is_floating:1`），新窗口由 `mango-decor` 自动 resize 到 880x560、
  居中、下移 34px，给标题条留位置
- `mango-decor`：每窗口一条 layer-surface（top 层）做 macOS 风格标题条 —— 红/黄/绿三键
  （`killclient` / `minimized` / `togglemaximizescreen`）+ 整条拖动（`movewin`）
- dock 居中，4 个图标：kitty / firefox / rofi / 键盘
- mangobar 及其配置、整理模式、编辑模式、watcher、lisgd、`mango-startup.sh` 全部删除
- 触摸手势只剩：3 指左右切 tag、3 指上滑 overview

标题条与窗口紧贴（间隙 0、只圆上方两角、底边不画线），边框随焦点变金/灰，视觉上是窗口的一部分。
新窗口由装饰器 resize 到屏幕 11/16 × 7/10 再 `centerwin`，实测 kitty `200,120 880x560 is_floating=true`。

CSD 应用（Firefox/Chromium/GNOME 系…）通过 `~/.config/mango-decor/exempt` 豁免，不再叠两层。
实测：kitty 有装饰条、firefox 只有自己的标签栏。

细节与交接约定见 [`docs/handoff-mango-shell.md`](handoff-mango-shell.md)。

### 9.1 窗口操作模式（mango-actions）

dock 第五个（指针图标）开关：开启后点窗口，会在窗口随机一边弹出 × / ○ / ✋ 三个圆钮：

- × → `killclient`
- ○ → `togglefullscreen`
- ✋ → 按住拖动窗口（`movewin` 跟随手指）

实现是 `configs/mango-shell/mango-actions.c`（layer overlay 全屏输入捕获 + cairo 绘制），
开关是 `mango-actions-toggle` 写 `$XDG_RUNTIME_DIR/mango-actions`。

实测（注入触摸）：点窗口后三个钮出现在上边缘；× 使窗口数 1→0；○ 后 `is_fullscreen=true`
（1280x800）；✋ 拖动后窗口从 (200,120) 移到 (155,212)。截图 `docs/research/mango-shell-actions.png`。

**退不出来的问题已修**：覆盖层原来在 overlay 层、盖住了 dock，导致模式里点不到 dock 按钮。
现在覆盖层降到 **top 层**（dock 保持在 overlay，永远在上），并额外在右上角画了「完成」胶囊。
两条退出路径都实测：dock 指针按钮 → 标志消失、层消失；点「完成」→ 同样。

### 9.2 阴影收小 + Firefox 拖边不能 resize 的原因

- 阴影：`/etc/mango/config.conf` 里 `shadows_size 18 -> 8`、`shadows_blur 28 -> 12`，`reload_config` 生效
- resize：mango 的边框抓取（`enable_border_resize=1`, `border_resize_size=10`）在
  `client_resize_edge_at()` 里会跳过"指针落在子 surface 上"的情况（`surface != client_surface(c)`）。
  Firefox 的 GTK CSD 在边缘铺了子 surface → 抓不到；kitty 单 surface → 能抓。
  解决办法不是改 mango，而是编辑模式里加了第 4 个按钮 **⤢**，用 `resizewin` 拖大小。
  实测：Firefox 880x560 → 916x632。

### 9.3 编辑模式点窗口会置顶

之前点被压住的窗口（比如 kitty 后面的 Firefox）只选中不置顶，因为覆盖层吃掉了点击。
现在 `on_press` 命中窗口后先 `dispatch focusid client,<id>` —— mango 的 focus 走 `client_focus(c, lift=true)`，
会 `client_raise_group()` 把窗口提到栈顶。实测：kitty 在前时，编辑模式点 Firefox → Firefox 出现在最前，按钮画在它右侧边缘
（截图 `docs/research/mango-shell-actions.png`）。普通模式下点击窗口本来就会置顶（mango 自己的
pointer focus 带 lift），问题只出在编辑模式的覆盖层吃掉了点击。

### 9.4 编辑模式改版：底边按钮 + 四角缩放

- 按钮位置不再随机，固定排在窗口**底边居中**（× ○ ✋ ⤢）
- 新增**四角抓手**：拖哪个角就从哪个角缩放，对角保持不动
  - TL：`movewin(+dx,+dy)` + `resizewin(-dx,-dy)`
  - TR：`movewin(0,+dy)` + `resizewin(+dx,-dy)`
  - BL：`movewin(+dx,0)` + `resizewin(-dx,+dy)`
  - BR：`resizewin(+dx,+dy)`
- 实测 Firefox：880x560 →（拖 BR -32,-35）848x525 →（拖 TL +36,+35）814x490

点击置顶：普通模式下 mango 在 press 阶段就 `client_focus(c, 1)`（带 lift），实测点 Firefox 会立刻到最前；
编辑模式吃掉点击后由 `mango-actions` 补 `dispatch focusid`，同样置顶。

### 9.5 编辑模式定稿

- 按钮回到**三个**：× ○ ✋，固定在窗口底边居中；⤢ 去掉了（改大小交给四角抓手）
- 四角抓手：拖哪个角就从哪个角缩放，对角不动
- 覆盖层改回 **overlay 层**：全屏窗口会盖住 top 层，只有 overlay 层在它上面，否则全屏时编辑模式不可用
- 代价：overlay 层在 dock 之上，模式里点不到 dock 按钮 → 退出统一用右上角「完成」胶囊
- 底部按钮自动避让 dock（`win_h - 96` 以上）

实测：全屏窗口下进编辑模式，覆盖层正常显示、三钮和四角都在，点「完成」退出（flag off、层消失）。

### 9.6 屏幕键盘打不开的修复

`mango-osk-toggle` 里启动命令写的是裸 `wvkbd-mobintl`，而桌面环境（dock / 无登录 shell）的 PATH
里没有 `/usr/local/bin`，于是 `setsid ... wvkbd-mobintl` 直接 command not found，什么也没发生。
改成绝对路径 `/usr/local/bin/wvkbd-mobintl` 后正常。

顺带把 `mango-shell-start` 里的壁纸改成"先 `pkill -x swaybg` 再起"，避免出现两个 `wallpaper` 图层。

### 9.7 键盘不再压住 dock

给打过补丁的 wvkbd 增加了 `WVKBD_BOTTOM_MARGIN` 环境变量（`main.c` 的 `WVKBD_CENTER_W` 分支里，
`set_margin(top, right, bottom, left)` 的 bottom 段），`mango-osk-toggle` 里传 `WVKBD_BOTTOM_MARGIN=82`
（= dock 高 68 + 下边距 14），键盘整体上移，正好停在 dock 上沿。

补丁片段（`~/elish-build/src/wvkbd/main.c`）：

```c
int bottom_margin = 0;
if (getenv("WVKBD_BOTTOM_MARGIN"))
    bottom_margin = atoi(getenv("WVKBD_BOTTOM_MARGIN"));
zwlr_layer_surface_v1_set_margin(layer_surface, 0, 0, bottom_margin,
                                 (sw - (int)req_w) / 2);
```

### 9.8 壁纸改为图片

`~/.wallpaper.jpg`（6700x4467 JPEG）已在设备上，`mango-shell-start` 改成：

```sh
pkill -x swaybg 2>/dev/null; sleep 0.3
if [ -f "$HOME/.wallpaper.jpg" ]; then
	setsid swaybg -i "$HOME/.wallpaper.jpg" </dev/null >/dev/null 2>&1 &
else
	setsid swaybg -c '#101014' </dev/null >/dev/null 2>&1 &
fi
```

实测：`swaybg -i /home/magma/.wallpaper.jpg`，图层 `wallpaper` 正常，截图见 `docs/research/mango-shell-keyboard.png`。

### 9.9 dock 配置化 + 构建脚本 + fish 的坑

- dock 应用列表原来编译在 `mango-dock.c` 的 `default_apps[]` 里，现在**优先读**
  `~/.config/mango-dock/apps.conf`（格式 `图标 | 命令 [| sep]`），改完只需重启 dock，不用编译；
  文件不存在时回退到内置默认表。
- 新增 `configs/mango-shell/build-dock.sh`（`sh build-dock.sh [install]`）：自动补协议文件、编译、
  可选安装并重启；关键是它用 **POSIX sh** 运行。
- **fish 的坑**：fish 的命令替换不按空格切分，`$(pkg-config --cflags --libs wayland-client cairo)`
  会作为**一个参数**传给 cc，导致 `-lwayland-client -lcairo` 全部失效，报一屏
  `undefined reference to wl_proxy_get_version / cairo_arc ...`。在 fish 里必须
  `(pkg-config ... | string split " ")`，或者直接用 `sh build-dock.sh`。
- 实测：删掉 `apps.conf` 里 firefox 那行并重启 dock，图标从 5 个变 4 个且自动重新居中。

## 10. 总览文档

所有改动的汇总（架构 / 演进 / 逐条根因 / 验收 / 遗留）见
[`mango-shell-summary.md`](mango-shell-summary.md)；dock 自定义见 [`dock-howto.md`](dock-howto.md)；
与其它会话的交接约定见 [`handoff-mango-shell.md`](handoff-mango-shell.md)。
