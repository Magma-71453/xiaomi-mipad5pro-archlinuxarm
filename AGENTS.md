# AGENTS.md — 交接说明（给后续接手这个仓库的 agent）

本文件是**操作手册 + 踩坑记录**。先读完再动手，能省掉大量重复试错。

---

## 1. 这个仓库是什么

为 **Xiaomi Mi Pad 5 Pro（代号 xiaomi-elish，SoC sm8250）** 维护一套**日常可用**的 Arch Linux ARM（aarch64）系统：

| 组成 | 说明 |
|---|---|
| 内核 | postmarketOS 的 linux-postmarketos-qcom-sm8250，带本仓库补丁（configs/kernel/patches/） |
| 桌面 | **MangoWM**（wlroots，dwm 风格）+ **waybar** 状态栏 + 自写 dock / 编辑模式 / 电源菜单 |
| x86 兼容 | FEX-Emu |
| 存储 | /dev/disk/by-partlabel/linux（约 196 GiB f2fs），引导走 boot_b |

---

## 2. 硬性约束（违反即报废）

1. **刷写只允许 boot_b 和 linux 两个分区**，其它分区一律不许碰。
2. 构建产物只放 output/YYYYMMDD/，并写 MANIFEST。
3. **仓库不跟踪任何二进制**。
4. **bug.md 是唯一的缺陷台账**（格式见第 8 节）。
5. **每个阶段单独一次 commit**，不要攒在一起。
6. **不要重新刷 rootfs**：所有改动都是「仓库里改 + 设备上直接同步」，见第 5 节。
7. 仓库要能公开：**不写真实 IP / SSID / 密码 / 代理配置**。IP 用占位后缀，SSID 写 <wifi-ssid>，密码写 <redacted>。

---

## 3. 仓库结构

```
configs/rootfs/            直接对应设备根文件系统（etc/ usr/ home/）
configs/kernel/            内核补丁与配置片段
configs/mango-shell/       dock / 编辑模式 / 归档的 mango-bar 源码 + 构建脚本
docs/                      设计文档与专题说明（bar.md / mango.md ...）
scripts/                   一次性工具脚本（如 build-qbootctl.sh）
output/YYYYMMDD/           构建产物（不入库）
bug.md                     缺陷台账（唯一）
```

**真相源原则**：configs/rootfs/... 是真相源，设备上的 /etc/... 只是副本。改完必须**同时**更新仓库和设备并 commit。

---

## 4. 怎么连设备

```sh
# 交互式（远端登录 shell 是 fish！）
ssh magma@<device-ip>            # 密码现场问，仓库不留

# 自动化（推荐）：askpass 免交互
cat > /tmp/ap.sh <<'EOF'
#!/bin/sh
echo <password>
EOF
chmod +x /tmp/ap.sh
SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force setsid -w \
  ssh -o StrictHostKeyChecking=no -o PreferredAuthentications=password \
      -o PubkeyAuthentication=no magma@<device-ip>
```

**必须知道的几条：**

- **远端 shell 是 fish**。带 $变量 / $( ) / for 的命令要包一层：`ssh ... 'bash -s' < script.sh`，否则报 `fish: 在此 $ 之后应有变量名称`。
- magma 有 **NOPASSWD sudo**（`sudo -n` 直接可用）。
- **由 waybar / 桌面启动的进程 PATH 只有 /usr/bin**，脚本里调用 /usr/local/bin/* 必须写绝对路径；检测程序是否存在也不能只靠 `command -v` / `shutil.which()`。
- `mango-dispatch` 需要 `XDG_RUNTIME_DIR=/run/user/1000`，socket 是 `/run/user/1000/mango-<pid>.sock`。
- 截图：设备上没有 imagemagick，用 `grim /tmp/x.png`，再 `ssh 'cat /tmp/x.png' > /tmp/x.png` 到主机裁剪。

---

## 5. 在设备上改东西的标准流程（不重刷）

```sh
# 1) 仓库里改；2) 只打包改动文件
tar -czf /tmp/patch.tar.gz configs/mango-shell/mango-bar.c ...
# 3) 传过去并解包构建
cat /tmp/patch.tar.gz | ssh ... 'cat > /tmp/patch.tar.gz'
ssh ... 'bash -s' < run.sh
# 4) 安装并重启对应进程
ssh ... 'bash -s' < install.sh
```

### ⚠️ 最容易踩的坑（踩过两次，代价很大）

**`a && b && c` 链条里任何一步失败，后面的 install 就静默不执行**，而你以为装好了。典型触发：`chmod` 对 root 拥有的文件报 `不允许的操作`。

**规矩：每次 install 之后必须验证内容真的变了**：

```sh
ssh ... 'grep -c "关键字" /usr/local/bin/xxx'   # 事先想好期望值
ssh ... 'ls -l /usr/local/bin/xxx'              # 看时间戳
```

同一个坑也出现在 `/usr/local/bin/mango-shell-start` 上（设备上长期是旧版本、启动的是已归档的 mango-bar）。**改完外壳/UI 相关的东西，一定核对设备上真正在跑的文件版本。**
---

## 6. 怎么验证（不要靠“应该可以”）

| 目的 | 手段 |
|---|---|
| 看界面 | `grim` 截图 + 主机 `magick -crop` 裁剪，再用 read_image 自己看 |
| 看某一帧内容（不经合成器） | 程序里 `MANGO_BAR_DUMP=/tmp/x.png` 把 shm 缓冲直接写成 PNG |
| 看窗口几何 | `mango-dispatch get all-clients`（含 x/y/width/height/is_focused/tags） |
| 看图层 | `mango-dispatch get all-layers` |
| 看日志 | `journalctl -b -1 --no-pager -o short-monotonic \| tail -30` |
| **模拟触摸**（唯一的自动化输入手段） | `/home/magma/touch_inject.py`（uinput 虚拟触摸屏，需 root） |

### 触摸注入的坐标映射（关键）

面板物理 1600×2560、旋转 90°、scale 2，虚拟设备坐标要**反变换**：

```
逻辑坐标 (X,Y) → 设备 dx = (800 - Y) * 1279/800 , dy = X * 799/1280
```

`touch_inject.py` 已内置 `to_dev()`，直接给逻辑坐标：

```sh
sudo python3 /home/magma/touch_inject.py tap 640 400          # 点一下
sudo python3 /home/magma/touch_inject.py drag 600 95 760 320  # 拖一段
```

> 必须设 `INPUT_PROP_DIRECT`，否则 libinput 把它当触控板而不是触摸屏。

---

## 7. 外壳组件清单与现状

| 组件 | 位置（仓库） | 现状 |
|---|---|---|
| 状态栏 | `configs/rootfs/home/magma/.config/waybar/{config.jsonc,style.css}` | **waybar**：浅色圆角、贴边全宽、高 38、窗口图标胶囊、中文时钟、SNI 托盘、音量/网络/蓝牙、电源键 |
| 电源菜单 | `configs/rootfs/usr/local/bin/mango-powermenu` | Python3+GTK3，Material 3 卡片 4×1（睡眠/关机/重启/Android），二次确认，带执行日志与重启看门狗 |
| 编辑模式 | `configs/mango-shell/mango-actions.c` | 全屏 overlay，选中窗口后 ✕ / ⛶ / ✋，已优化拖动性能 |
| dock | `configs/mango-shell/mango-dock.c` | 底部图标条 + 编辑模式入口 |
| 会话启动 | `configs/rootfs/usr/local/bin/mango-shell-start` | 由 fish 登录脚本调用；**待改成 systemd 服务**（见第 10 节） |
| 归档 | `configs/mango-shell/mango-bar.c` | 自写 C 状态栏，**已废弃**（mango 不给它派发输入），保留参考 |
| 槽位切换 | `scripts/build-qbootctl.sh` | 编译 qbootctl；`-s 0` = _a = Android，`-s 1` = _b = Linux |

---

## 8. 写文档的约定（.md）

### 8.1 语言与风格

- **中文正文**，技术名词保留英文原文（waybar / layer-shell / polkit）。
- 结论先行：**现象 → 根因 → 修法 → 验证**，不要写成叙事日记。
- 参数、对比、清单用**表格**；命令用 ```sh 代码块```，且**必须是真正跑通过的**。
- 引用现场证据：贴真实命令输出（可截断），不要写「应该」「大概」。

### 8.2 文件分工

| 文件 | 写什么 |
|---|---|
| `bug.md` | **唯一**缺陷台账。每条 `## B-0NN 标题`，内含 现象 / 根因 / 修复 / 验证。编号递增，不改旧条目 |
| `docs/<主题>.md` | 某一块的完整说明：设计、参数表、构建方法、限制（如 bar.md、mango.md） |
| `docs/handoff-*.md` | 面向人的交接记录 |
| `AGENTS.md` | 面向 agent 的操作手册（本文件） |
| commit message | `type(scope): 一句话结论` + 正文写做了什么/为什么/怎么验证 |

### 8.3 必须写进文档的三件事

1. **怎么构建/安装**（能复制粘贴直接跑）；
2. **怎么验证**（命令 + 期望输出）；
3. **已知限制**（明确写“没做/做不到”，比让后人猜要好）。
---

## 9. 已知坑（血泪清单，按杀伤力排序）

### 9.1 mango 的 IPC：**必须读回复**

```c
// ❌ 只发不收：mango 会丢弃命令（实测 focusid 完全不生效）
send(fd, cmd, len); close(fd);
// ✅ 必须读到 '}' 回复
send(); while (recv(fd, ...) > 0) { if (buf 以 '}' 结尾) break; } close(fd);
```

每条命令**新开一次连接**（mango 处理完就关）。「不等回复」是编辑模式拖动卡顿的一半原因，但**完全不等会让所有命令失效**——性能要靠**合并发送**（≤100 条/秒）解决。

### 9.2 cairo 遇到非法 UTF-8 会**静默吞掉之后所有绘制**

窗口标题来自合成器，可能是截断的多字节序列。表现：只有前半部分 UI 出现，后面全没了。
修法：解析后 `sanitize_utf8()`，并在绘制分段处检查 `cairo_status()`，异常则重建 context。

### 9.3 mango 不给我们自己的 layer 表面派发输入

自写 C 状态栏在 mango 上**收不到任何 pointer / touch 事件**（layer 表面与 xdg_toplevel 都试过，均为 0），而 dock 和 waybar 可以。
**结论：状态栏用 waybar，不要重写。**（归档代码在 configs/mango-shell/mango-bar.c）

### 9.4 polkit 没有认证代理 → systemctl 静默失败

`CanReboot / CanPowerOff / CanSuspend` 全返回 `challenge`，系统里只有 polkitd、没有 agent。
所以一切需要特权的操作都走 **`sudo -n`**（magma 有 NOPASSWD）。

### 9.5 重启会卡在 “System is rebooting.”

日志停在 `systemd-logind: System is rebooting.` 之后什么都不写（历史上是 fstrim 在 D 状态卡死）。
现在电源菜单带三级兜底：0 s 正常重启 → 25 s `systemctl --force reboot` → 35 s `echo b > /proc/sysrq-trigger`；
`fstrim.timer` 已 mask，`DefaultTimeoutStopSec=20s` 已设。

### 9.6 GTK3 没有 max-width

写进 waybar 的 style.css 会直接报 `'max-width' is not a valid property name`。
限制宽度只能：短标签（`{app_id}` / `{icon}`）+ `min-width` + `tooltip-format`。

### 9.7 没有 emoji 字体

emoji 全变方块。用已安装的 `Symbols Nerd Font` 码位（`U+F1EB` / `U+F011` / `U+F293` / `U+F026` …），
**并在 CSS 里声明字体族**，否则字形找不到照样方块。

### 9.8 qbootctl 上游被 Cloudflare 挡

`gitlab.com/postmarketOS/qbootctl` 对 curl 返回 403（JS 挑战）。用官方镜像
`https://codeload.github.com/linux-msm/qbootctl/tar.gz/refs/heads/main`（见 scripts/build-qbootctl.sh）。
**slot 0 = _a = Android，slot 1 = _b = Linux。**

### 9.9 非正常重启后可能黑屏

现象：设备起来了、背光亮，但 mango / waybar 都没运行（tty1 只剩一个 -fish）。
原因：fish 启动脚本「等时钟 ≥2024（最多 20 秒）→ exec mango」存在竞态（本机没有 RTC）。
**急救：`sudo systemctl restart getty@tty1`。**

### 9.10 mango-actions 需要 libcjson，且必须拿到 socket 路径

- 构建：`cc ... $(pkg-config --cflags --libs wayland-client cairo libcjson) -lm`（build-dock.sh 只构建 dock）；
- `sock_path` 只来自 argv[1] 或 `$MANGO_INSTANCE_SIGNATURE`；手动启动时两者都没有 → `cmd()` 直接 return
  → **所有操作失效但界面照常**。现已加 `glob("$XDG_RUNTIME_DIR/mango-*.sock")` 兜底 + `flock` 单实例锁
  （两个全屏 overlay 会互抢触摸）。

---

## 10. 待办（优先顺序）

1. **会话启动改成 systemd 服务**（`mango-session.service`，`After=systemd-user-sessions.service`，`Restart=on-failure`），
   根治 9.9 的开机竞态；fish 只保留交互式配置。
2. **定位 9.5 的具体卡死单元**：下次卡住后开 `systemd.log_level=debug` 抓关机流程，或查 f2fs/UFS 的 sync 路径。
3. **托盘长按菜单**：mango 不产生右键事件，需要在输入层做「长按 → 右键」注入（uinput），当前按用户决定**暂不做**。
4. **窗口标题条**：现为 `group_bar_height=12` + 全透明（只做拖动热区）。要可视化标题/关闭按钮就改
   `configs/rootfs/etc/mango/config.conf` 的 `group_bar_*`，`mango-dispatch dispatch reload_config` 立即生效。
5. **qbootctl / mango-powermenu 进镜像**：目前是设备上手工安装，需要在 output/ 的打包流程里固定下来。

---

## 11. 提交前自检

```sh
git status --porcelain     # 确认没有构建目录 / 二进制 / 临时文件
git diff --stat            # 改动范围是否符合预期
grep -rnE '<真实密码>|<真实IP>|<SSID>' .   # 敏感信息自查
git commit -m 'type(scope): 结论'
```

**每次 commit 都要回答三个问题**：改了什么 / 为什么 / 怎么验证的。
