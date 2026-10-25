# mango-bar — 轻量状态栏

设计稿：`docs/bar-design.svg`（浅色「TABLET STATUS BAR」）。真机逻辑分辨率 1280×800、scale 2。

```
┌────────────────────────────────────────────────────────────────────────────────┐
│ (bilibili · 哔哩…) (kitty · probe)   周日,10/26 00:00:12   [T…] (⌄) (🔉 📶 ᛒ) (⏻) │
└────────────────────────────────────────────────────────────────────────────────┘
```

## 分区与交互

| 区域 | 内容 | 交互 |
|---|---|---|
| 左 taskbar | 当前工作区每个窗口一个圆角胶囊，`appid · 标题`；当前窗口白底+描边，其余灰底 | **单击**聚焦；**长按（≥550ms）藏入系统托盘** |
| 中间 | `周几,MM/DD HH:MM:SS` | — |
| 系统托盘 | 被长按藏起来的窗口，图标显示 appid 首字母；位于托盘胶囊左侧 | **单击**还原到原工作区并聚焦；**长按**弹出 rofi 菜单（重新显示 / 关闭应用） |
| 托盘折叠键 | `<`/`^` 形状的箭头按钮（托盘图标左边） | **单击**折叠/展开整列托盘图标（隐藏后仍可通过它再展开） |
| 状态胶囊 | 扬声器 / Wi‑Fi / 蓝牙 三个独立热区 | 分别打开 `kitty -1 -e alsamixer` / `nmtui` / `bluetoothctl` |
| 电源键 | 圆形红色 | 点一次进入待确认（实心），3 秒内再点执行 `systemctl poweroff` |

## 系统托盘（真正的 StatusNotifierItem 宿主）

`mango-bar` 在会话总线上实现 **`org.kde.StatusNotifierWatcher`**，因此支持「关闭时退到托盘」的应用（Qt/Electron，如 bilibili、微信）
在关闭时能真正把图标注册进来，而不会因为找不到托盘宿主直接退出。

| 方向 | 我们提供 |
|---|---|
| Watcher 方法 | `RegisterStatusNotifierItem(s)`、`RegisterStatusNotifierHost(s)` |
| Watcher 属性 | `RegisteredStatusNotifierItems`（as）、`IsStatusNotifierHostRegistered`（b，恒为 true）、`ProtocolVersion`（i） |
| Watcher 信号 | `StatusNotifierItemRegistered(s)` / `StatusNotifierItemUnregistered(s)` |
| Item 侧 | 读取 `Id` / `Title` / `Status` / `IconName` / `IconPixmap`（a(iiay)，取最大一张），并订阅 `NewIcon` / `NewTitle` / `NewStatus` |
| 交互 | 单击 → `Activate(0,0)`（回到窗口）；长按 → `ContextMenu(0,0)` |

验证方式：

```sh
busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
        org.kde.StatusNotifierWatcher IsStatusNotifierHostRegistered     # -> b true
busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
        org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems     # -> as N "..."
```

### 尚未实现

- **dbusmenu 渲染**：`ContextMenu` 只是通知应用「弹出你的菜单」，菜单内容需要宿主实现 `com.canonical.dbusmenu`（`GetLayout` 等）并自己画出来。
  目前长按只会发出请求，Qt 应用的菜单不会显示；要做的话是下一块工作。
- Legacy **XEmbed** 托盘（老式 GTK2/X11 图标）不支持。

### 窗口「藏入托盘」的补充功能

窗口胶囊长按仍会把窗口挪到 9 号标签（`dispatch tag,client,<id>,9`），单击还原；这是 WM 侧的快捷隐藏，和上面的 SNI 托盘互不影响。

## 动作配置

`~/.config/mango-bar/actions.conf`（改完无需重编译，重启 bar 生效）：

```
audio=kitty -1 -e alsamixer
network=kitty -1 -e nmtui
bluetooth=kitty -1 -e bluetoothctl
```

## 实现要点

- 单文件 C（`configs/mango-shell/mango-bar.c`，约 900 行），`wlr-layer-shell` + cairo + shm，无 GTK/Qt；图层 `top`，`exclusive_zone = 54`；
- **双缓冲轮转**（单缓冲会在第一帧后死锁）；外部命令一律绝对路径；
- **UTF-8 清洗 + cairo 错误态自愈**：非法标题会让 cairo 静默丢弃之后所有绘制（表现为只剩窗口胶囊）；
- 空闲开销：1 Hz 时钟 + 1 Hz 客户端 + 2 Hz 状态，内容变化才重绘；长按/单击在按下时记录、抬起时按按住时长判定；
- 调试：`MANGO_BAR_DUMP=/tmp/bar.png mango-bar` 直接把当前帧写成 PNG。

## 构建 / 安装

```sh
cd configs/mango-shell
sh build-bar.sh            # 生成 ./mango-bar
sh build-bar.sh install    # 安装到 /usr/local/bin 并重启
```
