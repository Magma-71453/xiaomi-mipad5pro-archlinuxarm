# dock 自定义指南

dock 的应用列表有两层：

1. **`~/.config/mango-dock/apps.conf`**（推荐）—— 改完重启 dock 即可，**不用编译**
2. 内置默认表（`mango-dock.c` 里的 `default_apps[]`）—— 上面那个文件不存在时才用

---

## 1. 配置文件写法

`~/.config/mango-dock/apps.conf`，一行一个应用：

```
<图标> | <命令> [| sep]
```

| 字段 | 说明 |
|---|---|
| 图标 | 只写文件名时去 `/usr/share/mango-dock/icons/` 找；也可以写绝对路径 |
| 命令 | 用 `/bin/sh -c` 执行，`rofi -show drun` 这种带参数没问题 |
| sep | 可选第三个字段，写上就在这个图标前面画一条分隔线 |

默认内容（设备上已装好）：

```
kitty.png   | kitty
firefox.png | firefox
rofi.png    | rofi -show drun
kbd.png     | /usr/local/bin/mango-osk-toggle
actions.png | /usr/local/bin/mango-actions-toggle | sep
```

改完重启 dock：

```sh
pkill -x mango-dock; sleep 1
setsid env DOCK_SCALE=2 /usr/local/bin/mango-dock </dev/null >/dev/null 2>&1 &
```

或者图省事：`/usr/local/bin/mango-shell-start`（重启整套桌面组件）。

**加应用**只需要：图标丢进 `/usr/share/mango-dock/icons/`，配置里加一行，重启 dock。
**删应用**：删掉那一行。**排序**：调换行的顺序。**分组**：那一行末尾加 `| sep`。

实测：把 `firefox.png` 那行去掉再重启，dock 从 5 个图标变成 4 个并自动重新居中 ✅

---

## 2. 图标要求

- 格式 PNG（`gdk-pixbuf` 读），透明背景最好
- 尺寸 **256×256**（dock 显示 44×44 逻辑像素，2x 屏）
- 从 SVG 转：`rsvg-convert -w 256 -h 256 -o /usr/share/mango-dock/icons/foo.png foo.svg`

---

## 3. 改源码（改外观/行为时才需要）

外观常量在 `mango-dock.c` 顶部：`DOCK_H`、`MARGIN_BOT`、`GAP`、`SEP_W`、`RADIUS`、颜色等。

### ⚠️ fish 用户注意

**不要把 cc 命令直接粘进 fish**：

```fish
cc ... $(pkg-config --cflags --libs wayland-client cairo) -lm   # ✗ 会失败
```

fish 的命令替换**不按空格切分**，整行 `-I/usr/include/... -lwayland-client -lcairo`
会被当成**一个**参数传给 cc，于是所有 `-l` 都没生效，报一屏
`undefined reference to wl_proxy_get_version / cairo_arc / ...`。

正确做法，二选一：

```sh
# 用 POSIX shell 跑构建脚本（推荐）
sh ~/elish-build/src/mango-shell/build-dock.sh install
```

```fish
# 一定要在 fish 里写，就显式切分
cc -O2 -o mango-dock mango-dock.c wlr-layer-shell-unstable-v1-protocol.c \
   xdg-shell-protocol.c (pkg-config --cflags --libs wayland-client cairo | string split " ") -lm
```

`build-dock.sh`（在源码目录）会自动在缺协议文件时跑 `wayland-scanner`，
并用 `sh` 里的正确写法编译；带 `install` 参数时还会安装并重启 dock。

---

## 4. 排查

```sh
pgrep -a mango-dock                       # 在不在跑
mmsg get all-layers | grep mango-dock     # 图层在不在
```

dock 崩了不会自动重启，跑一次 `/usr/local/bin/mango-shell-start` 恢复。
