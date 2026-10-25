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

## 系统托盘是怎么实现的

mango 没有 SNI（StatusNotifierItem）协议、也没有 minimize/tag 客户端命令以外的窗口管理 API，所以托盘由 **bar 自己托管**：

- 隐藏：`long press 窗口胶囊` → `mango-dispatch dispatch tag,client,<id>,9`（把窗口丢到 9 号标签，界面上看不见）；
- 还原：`mango-dispatch dispatch tag,client,<id>,<原标签>` + `view,<原标签>` + `focusid client,<id>`；
- 托盘列表、折叠状态都由 bar 进程内维护，托盘图标因此随 bar 重启而清空（窗口仍在 9 号标签上，可用 `view,9` 找回）。

真正对接第三方「退出到托盘」的 SNI 宿主需要实现 `org.kde.StatusNotifierWatcher`，属于另一个量级的工程，暂未做。

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
