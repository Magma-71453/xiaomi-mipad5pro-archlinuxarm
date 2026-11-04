# dock 自动显隐与手势控制

**结论先行**：dock 不再常驻——任何可见窗口全屏或最大化时自动隐藏，恢复后自动显示；三指上滑可以手工把 dock 藏起来（粘滞）或叫回来（临时覆盖）。三个进程靠两个信号 + 两个文件协作：`mango-dock`（C，layer-shell 表面）只负责"隐藏/显示"两件幂等的事，`mango-dock-autohide`（Python 守护）读窗口状态决定该隐藏还是显示，`mango-dock-toggle`（Python 一次性工具）处理三指上滑。**实测**：全屏关闭 → 23 ms 内隐藏，最大化打开 → 53 ms 内隐藏；空闲 10 秒 CPU jiffies 增量 = 0。

---

## 1. 三个组件

| 组件 | 仓库位置 | 设备位置 | 职责 |
|---|---|---|---|
| `mango-dock` | `configs/mango-shell/mango-dock.c` | `/usr/local/bin/mango-dock` | 画 dock；接收 SIGUSR1/SIGUSR2 做**幂等**隐藏/显示；把自己的几何与隐藏态写进 `mango-dock.geom` |
| `mango-dock-autohide` | `configs/rootfs/usr/local/bin/mango-dock-autohide` | 同路径 | 订阅 `mmsg watch all-clients` 流 + 1 s tick；按"任一可见窗口全屏/最大化"发信号；按状态文件重新施加手动态 |
| `mango-dock-toggle` | `configs/rootfs/usr/local/bin/mango-dock-toggle` | 同路径 | 三指上滑的目标：按 `mango-dock.geom` 的 hidden 字段翻转显隐，并写状态文件（`flock` 防并发） |

调用链：

```
三指上滑 → mango-touchd ──exec──> mango-dock-toggle ──写 state 文件 + kill -USR1/2──> mango-dock
                                                                                          ↑
窗口变化 → mango 的 all-clients 流 → mango-dock-autohide ──kill -USR1/2──────────────────┘
                                     ↑
                             读 mango-dock.geom（先看已经是什么状态，再去发信号）
```

### 信号与文件约定

| 名称 | 路径 / 值 | 生产者 | 消费者 | 含义 |
|---|---|---|---|---|
| `SIGUSR1` | —— | autohide / toggle | `mango-dock` | 隐藏：`set_exclusive_zone(0)` + input region 置空 + 同一块缓冲重绘成**全透明**；表面**不 unmap**（见第 8 节） |
| `SIGUSR2` | —— | autohide / toggle | `mango-dock` | 显示：恢复 zone 82 (= `DOCK_H` 68 + `MARGIN_BOT` 14) + input region 置 `NULL`（整面可交互）+ 立刻重绘 |
| `mango-dock.geom` | `$XDG_RUNTIME_DIR/mango-dock.geom` | `mango-dock`（每次状态变化） | autohide / toggle | 五个空格分隔的整数：`x y w h hidden`；**最后一个字段 = dock 自己认为的隐藏态** |
| `mango-dock-autohide.state` | `$XDG_RUNTIME_DIR/mango-dock-autohide.state` | toggle 写、autohide 消费 | autohide | 文本，取值见下表；`auto` 时 autohide 不做任何 re-assert |
| `mango-dock-autohide.state.tmp` | 同上 + `.tmp` | autohide / toggle | —— | 写入中间文件再 `os.replace()`，避免读到半行 |
| `mango-dock-toggle.lock` | `$XDG_RUNTIME_DIR/mango-dock-toggle.lock` | toggle | toggle | `flock(LOCK_EX\|LOCK_NB)`，并发第二次调用直接退出 0 |
| `mango-dock-autohide.log` | `$XDG_RUNTIME_DIR/mango-dock-autohide.log` | autohide | 人 | 每次连接/施加都记一行；超过 64 KiB 自截断 |
| `mango-dock.log` | `$XDG_RUNTIME_DIR/mango-dock.log` | `mango-dock-super` 循环 | 人 | dock 自己的 stderr + 每次退出的 rc/时间；超过 128 KiB 在下一轮启动前截断 |

`$XDG_RUNTIME_DIR` 由 logind 给出，实机（本仓库测试设备）为 `/run/user/1000`；`mango-shell-start` 沿用这个值，不自己设。
本文下面的命令统一写成 `/run/user/$(id -u)`——`1000` 只是默认用户 uid，多用户/多会话环境这样写才不会跑错目录。
两个 Python 脚本在环境变量缺失/目录不存在时也回落到 `/run/user/$(id -u)`。

**信号只在目标状态与 `mango-dock.geom` 的 hidden 字段不一致时才发**——这条是 autohide 与 toggle 不互相打架的前提，也是重复调用安全的原因。

---

## 2. 自动显隐规则

- 判定条件：**任一 `is_visible` 的 client 满足 `is_fullscreen` 或 `is_maximized`** → 要隐藏；否则要显示。
- 驱动一：`mmsg watch all-clients` 的流式输出，每来一行 fold 一次（即时响应，窗口事件到 dock 隐藏 / 显示 23–53 ms）。
- 驱动二：1 s tick 的 `idle()`，只做流不可能触发的事——dock 被重启（pid 集合变了）后按快照重新施加规则；手动态生效期间重新核对几何。空闲时只做一次 `kill(pid, 0)` 活性检查 + 两次小文件读，**不进 /proc 全扫**。
- mango 重启后流会 EOF，守护进程按 `BACKOFF = 0.7 s` 重连，然后重新取一次 `mmsg get all-clients` 快照播种状态。

---

## 3. 手动态与优先级

状态文件取值：

| 值 | 谁写 | 语义 |
|---|---|---|
| （文件不存在） | —— | 等价 `auto` |
| `auto` | autohide | 完全由窗口规则决定 |
| `manual-hidden` | toggle | 用户手工隐藏，**粘滞**：忽略窗口规则，直到下一次三指上滑；dock 重启也会被重新施加 |
| `manual-shown N` | toggle | 用户手工显示，N = 上滑那一刻"是否有可见全屏/最大化窗口"（`mmsg` 问不到时只有 `manual-shown`，无 N） |

**优先级：手动态 > 窗口规则**，但 `manual-shown` 只是"按住一次"而不是永久粘滞：

- autohide 读到 `manual-shown N` → 立刻写回 `auto` 并保持显示；同时把规则值"钉"在 N 上；
- 只有当窗口状态**离开 N** 时，自动规则才重新生效（否则在全屏窗口上把 dock 叫回来，下一个事件就又被藏起来）。

状态机：

| 当前 | 事件 | 动作 | 之后 |
|---|---|---|---|
| （无 / auto） | 可见窗口变为全屏或最大化 | `kill -USR1`（仅当 geom hidden=0） | auto |
| auto | 全屏/最大化全部解除 | `kill -USR2`（仅当 geom hidden=1） | auto |
| auto | 三指上滑，dock 可见 | `kill -USR1` | manual-hidden |
| auto | 三指上滑，dock 隐藏 | `kill -USR2` + 记录 N | manual-shown N |
| manual-hidden | 任何窗口事件 / dock 重启 | 只 re-assert 隐藏，忽略规则 | manual-hidden |
| manual-hidden | 三指上滑（dock 隐藏） | `kill -USR2` + 记录 N | manual-shown N |
| manual-shown N | autohide 下一次事件或 tick | 写回 auto，保持显示，规则钉在 N | auto |
| auto（钉在 N） | 窗口状态离开 N | 按新规则施加 | auto |

---

## 4. 手势绑定（`mango-touchd`）

| 手势 | 动作 | 说明 |
|---|---|---|
| 三指上滑 | `/usr/local/bin/mango-dock-toggle` | 手工翻转 dock 显隐（粘滞手动态） |
| 三指左右滑 | `dispatch viewprev_have_client` / `viewnext_have_client` | 在有窗口的 tag 之间切换，与旧版一致 |
| 四指上滑 | `dispatch toggleoverview` | 进入 overview |
| 四指下滑 | `dispatch toggleoverview` | 离开 overview（同一个命令，靠状态判断方向） |

判定阈值：起止点位移 `>= SWIPE_MIN = 120` 逻辑像素；横向位移更大 → 横向动作，否则按手指数分派。
坐标是**逻辑坐标**（本仓库测试机型为 1280×800；面板原生 1600×2560、旋转 90°、scale 2 同为本机型实测值）。
换成其它设备时先用 `mango-dispatch get all-monitors` 确认本机的逻辑分辨率与 scale，再换算阈值。

---

## 5. 构建与安装

本仓库开发所用的 x86_64 主机（个人机或 CI）**没有 aarch64 交叉工具链**（`aarch64-linux-gnu-gcc`、`qemu-aarch64` 都不存在），所以 dock 必须在设备上 `cc` 编译；两个 Python 脚本直接 `install`。

```sh
# 1) 只打包改动文件传过去（见 AGENTS.md 第 5 节）
cd <仓库根目录>
tar -czf /tmp/dock-patch.tar.gz \
    configs/mango-shell/mango-dock.c \
    configs/rootfs/usr/local/bin/mango-dock-autohide \
    configs/rootfs/usr/local/bin/mango-dock-toggle \
    configs/rootfs/usr/local/bin/mango-touchd \
    configs/rootfs/usr/local/bin/mango-shell-start

# 2) 设备上编译 dock（POSIX shell！fish 不会按空格切分 $(pkg-config ...)）
ssh magma@<device-ip> 'bash -s' <<'EOF'
set -e
mkdir -p ~/elish-build/mango-shell && cd ~/elish-build/mango-shell
# 假设源码与协议文件已就位
sh build-dock.sh install      # 编译 + install 到 /usr/local/bin + 重启 dock
EOF

# 3) 脚本类直接 install（都在设备上跑）
ssh magma@<device-ip> 'bash -s' <<'EOF'
set -e
sudo -n install -m755 /tmp/mango-dock-autohide /usr/local/bin/
sudo -n install -m755 /tmp/mango-dock-toggle   /usr/local/bin/
sudo -n install -m755 /tmp/mango-touchd        /usr/local/bin/
sudo -n install -m755 /tmp/mango-shell-start   /usr/local/bin/
EOF
```

安装后**必须核对内容真的变了**（`install` 链接里的静默失败最常见）：

```sh
ssh magma@<device-ip> 'grep -c "dock_hide" /usr/local/bin/mango-dock 2>/dev/null; ls -l /usr/local/bin/mango-dock* /usr/local/bin/mango-touchd'
ssh magma@<device-ip> 'grep -c dock-toggle /usr/local/bin/mango-touchd'   # 期望 1（DOCK_TOGGLE 常量）
```

整套重启：`/usr/local/bin/mango-shell-start`（它会 `pkill` 锚定旧实例，再按顺序拉起 dock → autohide → osd → place）。

---

## 6. 验证方法

以下命令都在设备上执行（`ssh magma@<device-ip>`）。

> `$XDG_RUNTIME_DIR` 实机是 `/run/user/1000`（`1000` 是默认用户 uid）。下面的片段按 POSIX shell 写，
> 用 `$(id -u)` 而不是写死 uid；远端登录 shell 是 fish，请用 `bash -s` 跑，或直接把
> `$(id -u)` 换成 `$XDG_RUNTIME_DIR`。

### 6.1 组件是否都在跑

```sh
pgrep -a -f mango-dock
```
期望三行：`mango-dock`、`python3 /usr/local/bin/mango-dock-autohide`、**没有**多余的旧实例。

```sh
cat /run/user/$(id -u)/mango-dock-autohide.log | tail -5
```
期望能看到 `start pid=... rt=/run/user/<uid>`、`watch connected /run/user/<uid>/mango-<pid>.sock`，
以及每次施加时的 `dock -> hidden (was 0)` / `dock -> shown (was 1)`。

### 6.2 信号幂等性（旧版 bug 的回归用例）

```sh
for i in 1 2 3 4; do
  kill -USR1 $(pgrep -x mango-dock); sleep 0.4
  cat /run/user/$(id -u)/mango-dock.geom       # 期望最后一个字段 = 1
  kill -USR2 $(pgrep -x mango-dock); sleep 0.4
  cat /run/user/$(id -u)/mango-dock.geom       # 期望最后一个字段 = 0
done
```
**实测**：4 轮 USR1/USR2 反复切换全部生效（旧版只有第一轮生效，第二轮起 dock 再也不出现）。

### 6.3 自动显隐

让某个窗口全屏/最大化，然后：

```sh
mmsg get all-clients | grep -E 'is_fullscreen|is_maximized'
cat /run/user/$(id -u)/mango-dock.geom          # 期望 hidden=1
```
解除后 hidden 回到 0。**实测**：全屏关闭 23 ms 内隐藏、最大化打开 53 ms 内隐藏。

### 6.4 手动态

```sh
# 触摸注入脚本在设备上，路径为设备侧 /home/magma/touch_inject.py（仓库真值源
# configs/rootfs/home/magma/touch_inject.py）。下面这行是在设备上执行，不是主机。
sudo -n python3 /home/magma/touch_inject.py swipe 3 300 600 300 200   # 三指上滑
cat /run/user/$(id -u)/mango-dock-autohide.state    # 期望 manual-hidden
cat /run/user/$(id -u)/mango-dock.geom              # 期望 hidden=1
```
此时再让窗口全屏/恢复，state 应**保持** manual-hidden、dock 不被窗口事件影响；
再上滑一次 → `manual-shown N`（下一秒被 autohide 消费回 `auto`）、dock 显示。
重启 dock（`pkill -x mango-dock; setsid env DOCK_SCALE=2 /usr/local/bin/mango-dock &`）
后 **0.5 秒内**手动态重新施加。

### 6.5 空闲 CPU

```sh
pid=$(pgrep -f mango-dock-autohide | head -1)
awk '{print $14+$15}' /proc/$pid/stat; sleep 10; awk '{print $14+$15}' /proc/$pid/stat
```
**实测**：10 秒两次读数差值 = 0（jiffies delta 0，即真正空闲）。

---

## 7. 已知限制

1. **物理面板的真手指手势没有验证过**。只用电容屏 `uinput` 注入验证了判定逻辑；真手指是否被 libinput 以相同顺序报 slot、是否会出现额外的 slot 抖动，未实测。
2. **`mango-touchd` 只打开物理面板节点**（按 `name=NVTCapacitiveTouchScreen` 在 `/sys/class/input` 找并长期持有 fd），**不会打开注入出来的 uinput 节点**。所以"注入手势验证 touchd"必须把 touchd 直接指到那个 event 节点（root，且 `XDG_RUNTIME_DIR` 指向只有 `wayland-*` + 假 `mango-99999.sock` 的临时目录），否则验证不到。
3. **dock 由 `mango-dock-super` 循环盯着**（`mango-shell-start` 里起；退出 1 s 后重启，stderr 落 `$XDG_RUNTIME_DIR/mango-dock.log`）。手工 `setsid mango-dock` 起的实例没有这层保护。
4. **隐藏时表面仍然 mapped**：`mango-dispatch get all-layers` 会一直列出 `mango-dock`，不能再拿它判断 dock 可见性；唯一可信的是 `mango-dock.geom` 最后一个字段（1 = 隐藏）。
5. **mango 0.18.3 的 maximize 窗口不受 layer exclusive_zone 影响**：实测 zone=82 与 zone=0 下，最大化窗口都是 1260×730。所以隐藏时把 zone 归零是**协议上正确的行为**，而不是这个 bug 的成因——别指望靠调 zone 让 dock 不遮挡最大化窗口，遮挡与否由 layer 顺序决定。

---

## 8. 关键坑：dock 绝对不能 unmap（协议错误会静默杀掉它）

**结论：显隐只改属性 + 重绘，永远不做 `attach(NULL)`。**

历史上三版都踩过这个 family 的坑：

| 版本 | 现象 | 原因 |
|---|---|---|
| B-054 之前 | 隐藏一次之后再也回不来 | `relayout()` 不重新 attach，表面永远停在 unmapped |
| B-054 引入的写法 | 平时正常，**全屏窗口在场时**偶发（30 轮 hide/show 崩 10 轮）：dock 消失、`ps` 里没有 `mango-dock`、geom 停在 hidden=1 | 见下 |
| 现在（B-056） | —— | 不再 unmap |

`attach(NULL)` 是 unmap 提交，wlroots `layer_surface_role_commit()` 会走
`layer_surface_reset()`，把**整个 `configure_list` 清空**；此后 client 再 ack 任何
"这次提交之前发出的" configure，就命中：

```
zwlr_layer_surface_v1: error 0: wrong configure serial: N
```

合成器立刻断开连接 —— dock 是干净退出（无 core、无日志；stderr 当年还被丢进
`/dev/null`），外部看起来和"被隐藏"完全一样，于是"三指上滑也呼不出来"。

同一份代码里另有一条同源限制：unmap 之后**直接** attach 缓冲会被拒
（`error 2: layer_surface has never been configured`），必须空提交换 configure。

现行写法（`dock_hide()` / `dock_show()`）：

1. 隐藏 = `set_exclusive_zone(0)` + `set_input_region(空 region)` + 同一块缓冲重绘成
   全透明；显示 = zone 82 + `set_input_region(NULL)` + 立刻重绘；
2. 表面**始终 mapped** ⇒ 不会 reset，两类协议错误都不会出现；
3. `configure` 只在回调里记录，该次 `wl_dispatch` 结束后由 `apply_configure()`
   **只 ack 最新的一个**（wlroots 的 ack 会把列表里更旧的条目一并丢弃，逐个 ack 会自己
   踩自己）；
4. 首个 configure 被 ack 之前（`acked == 0`）只能空提交属性。

配套：`mango-shell-start` 用 `sh -c 'while :; do …; done' mango-dock-super` 盯着
dock，退出 1 s 后重启，stderr 落 `$XDG_RUNTIME_DIR/mango-dock.log`。

回归细节记在 `bug.md` B-054 / B-056。
