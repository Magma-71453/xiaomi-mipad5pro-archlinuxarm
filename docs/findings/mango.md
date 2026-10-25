# findings: 在 elish 上原生构建 MangoWM 0.18.3 (aarch64)

实测记录，对应交付物：
- 产物：output/20261024/mango/{stage/**, MANIFEST.mango, mango-0.18.3-aarch64.tar.zst}
- 一键脚本：scripts/build-mango-on-device.sh（可重入，见 `--rebuild` / `--clean`）
- rootfs 装配要点：docs/mango.md

---

## 1. 版本判定（不是猜的）

上游 tag 用 `git ls-remote --tags https://github.com/mangowm/mango` 确认：
**0.18.3 → 8b876cb31d4b0a393f4438e5a89b2f33a865d77b**（该依赖线最新 tag，0.18.x 之后即换线）。
源码 tarball sha256 `9b9c66c55ba83131cc2e7b847239f0f7520207f448a8c580c8ca796ec9e13e41`。

上游 meson.build 的依赖约束（0.18.3 实测行号）：

| 项 | 约束 | 来源 |
|---|---|---|
| wlroots | `dependency('wlroots-0.20', version: '>=0.20.0')` | meson.build:34 |
| scenefx | `dependency('scenefx-0.5', version: '>=0.5.0')` | meson.build:39 |
| wayland-server | `>=1.23.1` | meson.build:33 |
| libinput | `>=1.27.1` | meson.build:36 |
| 其它 | xkbcommon / wayland-client / libcjson / pangocairo / pixman-1 / libdrm / libpcre2-8 | meson.build:32-41 |
| 可选 | xwayland 默认 enabled → xcb / xcb-icccm / xcb-randr | meson_options.txt |

meson.build 里**没有** `meson_version:` 声明，所以 meson 版本不构成硬门槛（实测 1.12.1 可用）。

## 2. 依赖来源：ALARM 包 + sysroot，不碰 pacman

设备（ArchLinuxARM aarch64，936 个包已装）**没有** wlroots/scenefx，
且决策 D-015 只允许在 `~/elish-build` 内活动、禁止 pacman 安装、禁止写 /usr。

做法：解析 ALARM 包数据库 `mirror.archlinuxarm.org/aarch64/{core,extra}.db.tar.gz`，
以 wlroots0.20+scenefx 为根做**依赖闭包**（114 个包），再减去设备 `pacman -Qq` 已装的 108 个，
得到**真正需要补的只有 5 个包**（另有 libgcc/libstdc++ 由设备的 gcc-libs 提供，不重复引入）：

| 包 | 版本 | sha256（本次下载物，已在脚本内固定） |
|---|---|---|
| wlroots0.20 | 0.20.2-1 | 31bf5f2ea29001643fffd3df21930e4254e51f30bc1a1684c476449d4ebdedc3 |
| scenefx | 0.5-1 | 98966e25d489d4a72afb4b99403c5d962240cba9a3be943bd59c0f12ccd6e71a |
| libliftoff | 0.5.0-1 | 77c8cf79c256c0b118ee14250ba16b7e322525566daa16a9fcf048153a1214df |
| wayland-protocols | 1.49-1 (arch=any) | 9678c0d371ecfb5041b1b1d8a7986977bd82b7233984dbe25b33a54da0dbee27 |
| cjson | 1.7.19-1 | 3d303d194b6b96103718441345acd16a269f31f450a493e353cf8518cc0299d9 |

解包进 `~/elish-build/sysroot`（保留 usr/ 布局，4.3 MiB，6 个 .pc）。

设备侧**已有**（pacman 系统库，未改动）：wayland 1.26.0、libinput 1.32.0、pixman 0.46.4、
libdrm 2.4.134、xkbcommon 1.13.2、libdisplay-info 0.3.0、mesa 1:26.2.5、libglvnd 1.7.0、
systemd-libs 262.1(libudev)、libseat 0.9.3、pango 1:1.58.2、glib2 2.90.1、pcre2 10.49、
xcb-util-wm 0.4.2、xorg-xwayland 24.1.14。

## 3. meson 的获取：pip 被 PEP 668 挡住 → 解 wheel 到私有目录

设备无 meson（`command -v meson` 空），python3 3.14.8 / pip 26.2.2。
`python3 -m pip install --user meson` **失败**（原文）：

```
error: externally-managed-environment
note: If you believe this is a mistake, please contact your Python installation or OS distribution provider.
You can override this, at the risk of breaking your Python installation or OS, by passing --break-system-packages.
```

不用 `--break-system-packages`（会写 ~/.local，超出 D-015 边界）。改为：
宿主下载 wheel `meson-1.12.1-py3-none-any.whl`（sha256 930bc7542cbd...8d8b02，取自 pypi.org JSON API）
→ 推送到设备 → `bsdtar -xf` 解到 `~/elish-build/mesonlib` → 生成包装器 `~/elish-build/bin/meson`：

```sh
#!/bin/sh
PYTHONPATH="$HOME/elish-build/mesonlib\${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH
exec python3 -m mesonbuild.mesonmain "$@"
```

全程离线、不写系统路径、不需要 pip。实测 `meson --version` = **1.12.1**。

## 4. sysroot 的关键坑：不要用 PKG_CONFIG_SYSROOT_DIR

第一版用 `PKG_CONFIG_SYSROOT_DIR=$SYSROOT` + `PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig`。
pkg-config 解析对（wlroots-0.20 0.20.2 / scenefx-0.5 0.5.0），但编译第 1 个 TU 就炸：

```
sysroot/usr/include/scenefx-0.5/scenefx/types/wlr_scene.h:22:10: fatal error: pixman.h: 没有那个文件或目录
   22 | #include <pixman.h>
compilation terminated.
```

原因：`PKG_CONFIG_SYSROOT_DIR` 会把**系统包**的 cflags 也前缀化，pixman-1.pc 的
`-I/usr/include/pixman-1` 变成 `-I$SYSROOT/usr/include/pixman-1`（sysroot 里没有 pixman）。

修正：只改写 sysroot 内 6 个 .pc 的绝对前缀，系统 .pc 保持原样：

```sh
sed -i -e "s|^prefix=/usr|prefix=$SYS/usr|" -e "s|^libdir=/usr|libdir=$SYS/usr|" \
       -e "s|^includedir=/usr|includedir=$SYS/usr|" -e "s|\${pc_sysrootdir}||g" "$f"
```

改写后 `pkg-config --cflags wlroots-0.20` =
`-I$SYS/usr/include/wlroots-0.20 -I/usr/include/pixman-1 -I$SYS/usr/include -I/usr/include/libdrm`，
且 wayland-protocols 的 `pkgdatadir` 正确指向 `$SYS/usr/share/wayland-protocols`（protocols/meson.build 需要读 xml）。

## 5. 构建结果

```
meson setup build/mango src/mango-0.18.3 --prefix=$HOME/elish-build/stage --buildtype=release --wrap-mode=nodownload
ninja -C build/mango -j8      # 147 targets
meson install -C build/mango
```

- 工具链：gcc 16.2.1 20261021 / ld.bfd 2.47.0 / ninja 1.13.2 / pkgconf 3.0.10 / wayland-scanner /usr/bin
- **error 0 / warning 0**（`grep -c '^FAILED'`、`grep -c 'error:'`、`grep -c 'warning:'` 均为 0；
  全量日志 output/20261024/mango/logs/build.log，147 行进度、无可疑告警）
- 耗时：首次全量 **8s**；`--rebuild` 冷重编 **20s**（同为 -j8，机器负载差异）
- 编译期宏：`-DWLR_USE_UNSTABLE -DXWAYLAND -DSYSCONFDIR="/etc" -DVERSION="0.18.3(release)"`
- 两次构建（首建 / 冷重编）产出的 `bin/mango` sha256 完全一致：`18e38c750e001dcb...` → 可复现

产物 10 个文件 / 3.6 MiB（明细见 MANIFEST.mango）：

| 文件 | 字节 |
|---|---|
| stage/bin/mango | 3355704 |
| stage/bin/mangonag | 164992 |
| stage/bin/mmsg | 133296 |
| stage/etc/mango/config.conf | 7191 |
| stage/etc/mango/config.toml | 7721 |
| stage/lib/systemd/user/mango-session.target | 184 |
| stage/share/man/man1/{mmsg,mangonag}.1 | 4001 / 3707 |
| stage/share/wayland-sessions/mango.desktop | 125 |
| stage/share/xdg-desktop-portal/mango-portals.conf | 151 |

设备占用：`~/elish-build` 共 **37 MiB**（sysroot 4.3M / build 14M / stage 3.6M / pkgs 2.2M / mesonlib 9.4M），可整体删除。

## 6. 失败与解决（按出现顺序）

1. **ssh 命令被 fish 与引号拆坏**：`dbclient ... 'mkdir -p a b'` 报 `mkdir: 缺少操作对象`。
   → 多行脚本一律 `dbclient ... sh <<'EOF'`；单命令必须作为**单个 argv** 传（`dbclient ... "sh -c '...'"`）；
   文件传输统一走 `tar cf - | dbclient ... "sh -c 'tar xf - -C <dir>'"`。
2. **wayland-protocols 下载 404**：它是 `arch=any`，文件名是 `-any.pkg.tar.xz` 而非 `-aarch64...`。
3. **pip install --user meson 被 PEP 668 拒**（见 §3）→ wheel + bsdtar。
4. **pixman.h 找不到**（见 §4）→ 改 .pc 前缀而不是 PKG_CONFIG_SYSROOT_DIR。
5. **空参数位错位**：dbclient 把 argv 用空格拼成一条远端命令，`""` 会塌陷，
   导致 `sh -s -- BASE "" 0.18.3 pkg.tar.gz` 变成 `$2=0.18.3`，远端
   `tar xzf "$PKGS/"` 报 `tar: ... 是一个目录`。→ 空值改传 `"${force:--}"` 哨兵，并在远端断言 `$4` 非空。

## 7. 未决事项

- **未在设备上真跑 mango**（需要 seatd/DRM 会话，属"装配后"验证），因此只有静态证据
  （file/ldd/pkg-config），没有运行时冒烟测试。
- 设备上除这 5 个包外没有 libliftoff/cjson/wlroots0.20/scenefx，因此**在设备上**运行必须
  `LD_LIBRARY_PATH=~/elish-build/sysroot/usr/lib`；rootfs 里则应正常安装这 5 个包（见 docs/mango.md）。
- 二进制**未 strip**（3.35 MB，带 debug_info）：上游 meson.build 无条件加 `-g`。
  若要体积优化，装配时可 `strip --strip-unneeded`（会改 sha256，MANIFEST 需重算）。
- 只验证了 wlroots **0.20 线**；0.19/0.18 线（不同 tag）未构建。
- `~/elish-build`（37 MiB）仍留在设备上，随时可 `rm -rf` 清理；本任务未删除以便复查。
