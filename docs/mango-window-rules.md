# MangoWM 窗口规则（window_rule）—— 自行添加指南

结论先行：MangoWM 里「某类窗口默认全屏 / 浮动 / 指定尺寸」这类行为，全由
`/etc/mango/config.conf` 里的 `window_rule=` 行控制。想给某个 app 定制打开行为，
就是加一行 `window_rule=`，`mango -p` 校验 → 重载配置 → **关掉再重开该窗口**验证。

> 本文是通用操作手册。仓库的 `configs/rootfs/etc/mango/config.conf` **不含**任何 app
> 个人化规则（例如 waydroid 全屏），那属于设备本地偏好，按本指南手动加到设备即可，
> 不必（也不建议）提交进共享仓库。

---

## 1. 生效的是 config.conf，不是 config.toml

mango 裸启动（无 `-c`）时按默认路径找配置。二进制里编译进去的默认路径是
`%s/mango/config.conf`（编译期 `-DSYSCONFDIR="/etc"` → 即 `/etc/mango/config.conf`）。

| 文件 | 是否生效 | 判据 |
|---|---|---|
| `/etc/mango/config.conf` | **是** | `exec_once=/usr/local/bin/mango-shell-start` 在这里（第 333 行），桌面确实起来了；`window_rule=app_id:.*,is_floating:1` 也在这里。实测改它 + 重载 + 重开窗口即生效 |
| `/etc/mango/config.toml` | 否（另一格式的冗余副本） | 没有 `exec_once`、没有 `window_rule`，改它没反应 |

两个文件是同一套配置的两种写法（`key=value` vs TOML）。**本机读的是 key=value 的
`config.conf`，只改它。**

改完先做语法校验（**必做**）：

```sh
mango -p        # 无输出 + exit 0 = 解析通过；报错会打印出错行号
```

---

## 2. 一行规则的语法

```sh
window_rule=参数:值,参数:值,...,app_id:<正则>,title:<正则>
window_rule_once=参数:值,...,app_id:<正则>      # 只在窗口第一次打开时生效
```

- 匹配目标写 `app_id:` 和 / 或 `title:`，**都支持正则**；两者都写时需同时匹配才套用。
- `window_rule` = 每次窗口打开都套用；`window_rule_once` = 只在首次打开套用一次。
- 其余全是「参数:值」对，逗号分隔，一行可设多个参数。

### 2.1 常用参数（完整表见 <https://mangowm.github.io/docs/window-management/rules/>）

| 参数 | 值 | 作用 |
|---|---|---|
| `is_floating` | `0`/`1` | 强制浮动 / 不浮动 |
| `is_fullscreen` | `0`/`1` | 强制**真全屏**（铺满、盖住状态栏/dock） |
| `is_fake_fullscreen` | `0`/`1` | 假全屏：铺满但仍是一般窗口（dock / 标题条可交互、可再调整） |
| `width` / `height` | 浮点 | 窗口尺寸；**值 <1 表示占屏幕比例**，≥1 是像素 |
| `offset_x` / `offset_y` | 整数 | 相对屏幕中心的偏移（%），100 = 贴边 |
| `is_global` | `0`/`1` | 全局窗口（跨 tag 常驻） |
| `no_border` / `no_shadow` / `no_radius` | `0`/`1` | 去边框 / 阴影 / 圆角 |

> 选 `is_fullscreen` 还是 `is_fake_fullscreen`：游戏、看视频要沉浸 → `is_fullscreen:1`；
> 想铺满但保留 dock / 标题条、还想拖动缩放 → `is_fake_fullscreen:1`。

---

## 3. 两个必须知道的坑

### 3.1 规则优先级：**写在后面的覆盖前面的**

多条 `window_rule` 都匹配同一个窗口时，对**同一个参数**，靠后那条赢。本机有一条全浮动兜底：

```sh
# /etc/mango/config.conf（第 324 行附近）
window_rule=app_id:.*,is_floating:1        # 兜底：所有窗口默认浮动（平板触控友好）
```

要给某类窗口「不浮动 + 全屏」，你的规则**必须写在这条兜底之后**，否则会被它盖回去：

```sh
window_rule=app_id:.*,is_floating:1                    # 兜底（先）
window_rule=app_id:waydroid.*,is_floating:0,is_fullscreen:1   # 例外（后，赢）
```

### 3.2 `mango-place` 会缩放浮动窗口

本机常驻的 `mango-place` 会把每个 **`is_floating` 的可见窗口**缩到 11/16 × 7/10 并居中
（实测 1280×800 屏上开出来就是 880×560）。所以：

- 想让窗口**全屏 / 指定尺寸**，必须设 `is_floating:0`，否则 `mango-place` 会把它拉回去缩小；
- 只有「浮动小窗」才该留 `is_floating:1`（那正是 `mango-place` 存在的意义）。

---

## 4. 添加窗口规则的完整流程

以「waydroid 的 Android 应用默认全屏」为例（这是设备本地偏好，仓库不含）。

### 4.1 先查目标窗口的 app_id

拉起该应用，然后列窗口看 `appid` 字段：

```sh
# 连上设备（远端 shell 是 fish，命令一律包一层 bash -s）
dbclient -i ~/.ssh/dropbear_ed25519_host_key magma@<device-ip>
# 设备上执行：
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
waydroid app launch com.android.calculator2      # 拉起一个目标 app
sleep 5
mango-dispatch get all-clients                   # 看 appid
```

实测 waydroid 应用的 `appid` 形如 `waydroid.<android 包名>`：

```json
{"appid":"waydroid.com.android.calculator2","is_fullscreen":false,"is_floating":true,
 "x":252,"y":172,"width":880,"height":560}
```

所以正则 `waydroid.*` 能匹配所有 waydroid 应用窗口（其它原生 app 的 app_id 不含
`waydroid`，不会误伤）。

### 4.2 改 `/etc/mango/config.conf`

在全浮动兜底规则**之后**加一行：

```sh
# --- waydroid Android apps open fullscreen, not a floating mini-window ---
# app_id = waydroid.<android.packageName>；is_floating:0 防 mango-place 缩小；is_fullscreen:1 铺满
window_rule=app_id:waydroid.*,is_floating:0,is_fullscreen:1
```

> 设备上 `/etc/mango/config.conf` 属 root，`sudo` 编辑；改前先备份：
> `sudo cp -a /etc/mango/config.conf /etc/mango/config.conf.bak.$(date +%Y%m%d%H%M%S)`

### 4.3 校验 + 重载 + 重开窗口

```sh
mango -p                                   # 语法校验，exit 0 才继续
mango-dispatch dispatch reload_config      # 重载（等价 mmsg dispatch reload_config）
# 规则在「窗口打开」时套用，旧窗口不会追溯变化 → 关掉旧窗口再重开：
mango-dispatch dispatch killclient         # 关掉当前聚焦的旧窗口
waydroid app launch com.android.calculator2   # 重开一个新窗口
```

> `reload_config` 不重启合成器，安全。若改动没生效（极少数冷门参数只在启动读取），
> 才考虑重启 mango（`SUPER+m` 退出后由 tty1 登录脚本重拉，或 `mango-session`）。

### 4.4 验证（看 JSON 里状态位和几何）

```sh
sleep 5; mango-dispatch get all-clients
```

期望输出（关键三项）：

```json
{"appid":"waydroid.com.android.calculator2","is_fullscreen":true,"is_floating":false,
 "x":0,"y":0,"width":1280,"height":800}
```

`is_fullscreen:true` + `is_floating:false` + 坐标铺满整屏（0,0 到 1280×800）= 成功。
改完记得把测试窗口关掉。

---

## 5. 已知限制 / 注意

- **规则在窗口打开时套用**：改完配置必须**关掉再重开**目标窗口，旧窗口不变。
- **优先级靠后赢**：个性化规则必须写在 `app_id:.*` 兜底之后（见 §3.1）。
- **想铺满就别留 `is_floating:1`**：否则 `mango-place` 会把它缩成 11/16×7/10（见 §3.2）。
- **app_id 的写法随应用类型而异**：waydroid 是 `waydroid.<包名>`；原生 Wayland app 一般
  是反向域名（如 `firefox`、`com.tencent.WeChat`）；XWayland 窗口可能要用 `title:` 匹配。
  不确定就用 §4.1 的 `get all-clients` 现查。
- **真全屏会盖住状态栏/dock**：这是 `is_fullscreen` 的预期行为（`mango-dock-autohide`
  也会自动藏 dock）；要留 dock 用 `is_fake_fullscreen:1`。
- 本机那条 waydroid 全屏规则是**设备本地改动**，没进仓库；若日后按 AGENTS.md §5 从仓库
  重新同步 `/etc/mango/config.conf`，会被覆盖掉，需按本指南重新加一行。
- 完整参数表与更多示例见上游 wiki：
  <https://github.com/DreamMaoMao/mango/wiki/> 、<https://mangowm.github.io/docs/window-management/rules/> 。
