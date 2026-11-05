# 从零重建（新机器 / 新克隆仓库）

本文回答一个问题：**把仓库 `git clone` 到一台新机器上，能不能直接开始编译出可发布的产物？**
结论分三段：能、能但要额外条件、不能（需要补充）。

---

## 0. 先说结论

| 产物 | 新克隆能否直接编译 | 前置条件 |
|---|---|---|
| 内核 `Image.gz` + dtb | ✅ 能 | 宿主需装 LLVM 工具链等（见 §1） |
| `initramfs.cpio.gz` | ✅ 能 | 需要内核模块产物（上一步） |
| `boot.img` / `boot-boe-alt.img` | ✅ 能 | 需要上两步产物 + GPU 固件（脚本自动下载） |
| `rootfs`（tar.zst + f2fs 镜像） | ⚠️ 能，但需要 `--mango` / `--overlay` 两个目录 | 见 §3，这两个目录**不在仓库里** |
| `mango-0.18.3` 运行时 | ⚠️ 能，但要在 **aarch64 设备**上编 | 见 §4 |
| 外壳组件（dock / actions / osd） | ✅ 能（源码 + 构建脚本都在仓库） | 需 aarch64 环境，见 §4 |
| `qbootctl`（切槽位） | ✅ 能 | 仓库有 `scripts/build-qbootctl.sh` |
| `wvkbd-mobintl` / `grim` | ❌ **不能** | 上游项目、仓库不含源码，见 §5 |
| 发布到 GitHub Release | ✅ 能 | 需 `gh auth login` + 写权限 |

> **对最终用户**：完全不需要编译，直接用 Release 里的现成资产按 `INSTALL.md` 安装。
> 本文面向**要自己出镜像的维护者**。

---

## 1. 宿主机准备（x86_64 Arch Linux）

```sh
# 编译内核 + 打包镜像所需
/usr/bin/pacman -S --needed \
  clang lld llvm bc dtc flex bison pahole make \
  python3 perl cpio gzip zstd libarchive curl openssl \
  e2fsprogs f2fs-tools fakeroot git

# 发布用（可选）
/usr/bin/pacman -S --needed github-cli
/usr/bin/gh auth login
```

脚本里 `need_cmd` 检查的完整命令集：
`bc bison bsdtar clang cmp cpio curl debugfs dtc dumpe2fs e2fsck fakeroot flex
gh git gzip install ld.lld llvm-ar llvm-nm llvm-objcopy make mke2fs openssl
pahole perl python3 realpath sha256sum truncate zstd`

缺哪个脚本会直接报 `required command not found: <name>`。

**不需要 root**：rootfs 全程在 `fakeroot` 下组装（`bsdtar` 解包 + `sload.f2fs` 建镜像）。

---

## 2. 能直接跑的部分

```sh
cd <仓库根>

# 2.1 内核（首次会下载 ~265 MB 源码包并交叉编译，约 10 分钟；增量约 10 秒）
BUILD_DIR=/abs/path/.build/kbuild /abs/path/scripts/build-kernel.sh

# 2.2 GPU 固件（initramfs 早期加载必需，脚本自带 sha256 校验）
/abs/path/scripts/fetch-gpu-firmware.sh --out /abs/path/.build/firmware/elish-gpu

# 2.3 三件套一键（内核 → initramfs → 两份 boot.img）
/abs/path/scripts/build-release.sh --no-publish
```

产物落在 `output/<YYYYMMDD>/`，中间树在仓库**外**的 `.build/`（不要放仓库内，`audit-hygiene.sh` 会判成污染）。

---

## 3. rootfs：需要补两个目录

`build-rootfs.sh` 本身能跑，但它有两个**可选**参数，缺了产物就不完整：

```sh
BUILD_ROOT=/abs/path/.build OUT_DIR=/abs/path/output/<date>/rootfs \
  /abs/path/scripts/build-rootfs.sh \
    --modules  /abs/path/.build/kbuild/modinstall/lib/modules \
    --firmware /abs/path/.build/firmware/elish-gpu \
    --mango    /abs/path/.build/mango-stage          # ← 不在仓库
    --overlay  /abs/path/.build/device-overlay       # ← 不在仓库
    --size 16G --jobs 8
```

- `--modules` 必须指向**包含 `<kernelrelease>` 的那一层**（即 `.../lib/modules`），
  指到 `.../lib/modules/7.2.0` 会被脚本明确拒绝（早期版本会静默跳过，导致镜像缺面板驱动 → 首启黑屏）。
- `--firmware` 用 `fetch-gpu-firmware.sh` 的产出。
- `--mango` 与 `--overlay` 见下面两节。

> 仓库约定 `packages.list` 是该装的包；`build-rootfs.sh` 会自动对比**底包 tarball**解析出需要追加的包
> （当前 292 个），联网从 ALARM 镜像下载。镜像站只保留最新版，若报 404 用环境变量覆盖：
> `QEMU_PKG_FILE=qemu-user-static-<ver>-1-x86_64.pkg.tar.zst`。

---

## 4. mango 与外壳组件：必须在 aarch64 上编

`mango` 及外壳都是 aarch64 原生二进制，仓库**不跟踪二进制**，所以要自带构建能力。

### 4.1 外壳组件（dock / actions / osd）——仓库已完备

```sh
# 在 aarch64 设备上（或 aarch64 chroot 里）
cd <仓库>/configs/mango-shell
/usr/bin/sh build-shell.sh          # 产出 mango-dock / mango-actions / mango-osd
/usr/bin/sh build-shell.sh install  # 安装到 /usr/local/bin（需免密 sudo）
```

依赖：`wayland`（wayland-scanner + wayland-client）、`cairo`、`cjson`。
注意 ALARM 里 cJSON 的 pkg-config 名是 **`libcjson`**（脚本已兼容 `cjson` 与 `libcjson` 两种）。

> 已验证：用该脚本在设备上从仓库源码重新构建的三个组件，与设备现装版本的
> **sha256 逐个一致**，说明源码与构建参数是完整可复现的。

### 4.2 mango 本体（0.18.3）——用设备原生构建

```sh
DEV_HOST=<device-ip> DEV_USER=<user> \
  /abs/path/scripts/build-mango-on-device.sh
```

该脚本在宿主下载 ALARM 的 aarch64 包（wlroots0.20 / scenefx / libliftoff / cjson /
wayland-protocols）解到设备侧 sysroot，再在设备上 `meson` + `ninja` 编译，最后打包到
`<OUT_DIR>/stage/`。产物结构正好可以作为 `build-rootfs.sh --mango` 的入参：

```text
<OUT_DIR>/stage/
├── bin/{mango,mangonag,mmsg}
├── etc/mango/config.conf
└── share/wayland-sessions/mango.desktop
```

也可以直接复用**任何一台已装好的同版本设备**上的产物：

```sh
# 从设备取（只在版本一致时安全：mango 0.18.3 + wlroots0.20 + scenefx 0.5）
/usr/bin/dbclient -i <key> <user>@<device-ip> \
  'tar -cf - /usr/bin/mango /usr/bin/mangonag /usr/bin/mmsg \
           /usr/share/wayland-sessions/mango.desktop \
           /usr/share/xdg-desktop-portal/mango-portals.conf' > mango-runtime.tar
```

验证依赖是否自洽（`build-rootfs.sh` 会自动做这件事）：

```sh
LC_ALL=C readelf -d <stage>/bin/mango | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'
# 期望全部能在目标 rootfs 的 /usr/lib 下找到；构建日志会打印 "mango DT_NEEDED resolution: 31/31"
```

### 4.3 device-overlay（仓库里没有）

`--overlay` 是把「仓库里的配置 + 上面编出来的二进制」摊到 rootfs 里的一次性装配。
仓库**没有**提供生成它的脚本（这是当前最大的复现缺口）。手工装配方式：

```sh
OV=/abs/path/.build/device-overlay
rm -rf "$OV"; mkdir -p "$OV"

# 1) 文本资产：直接来自仓库（真值源）
cp -a <仓库>/configs/rootfs/usr  "$OV/usr"
cp -a <仓库>/configs/rootfs/home "$OV/home"
cp -a <仓库>/configs/rootfs/etc  "$OV/etc"

# 2) 编译产物：来自 §4.1 / §4.2
cp <shell-build>/mango-dock <shell-build>/mango-actions <shell-build>/mango-osd "$OV/usr/local/bin/"

# 3) 注意：overlay 是在 customise() 之后应用的，会覆盖 /etc 下同名文件。
#    若不想覆盖 build-rootfs 已处理的 fstab/hostname/hosts/locale.conf/
#    resolv.conf/sudoers.d/10-wheel，把这 6 个从 "$OV/etc" 里删掉：
cd "$OV/etc" && rm -f fstab hostname hosts locale.conf resolv.conf sudoers.d/10-wheel
```

---

## 5. 仓库缺源码的组件（需要补充）

以下在设备 `/usr/local/bin` 里存在，但**仓库没有源码也没有构建脚本**，
新机器无法从零复现。补齐方式：要么把源码纳入仓库，要么把预编译产物作为
Release 资产分发（当前 Release 走的是后者，只分发了 mango 运行时）。

| 组件 | 用途 | 现状 | 需要补充什么 |
|---|---|---|---|
| `wvkbd-mobintl` | 屏幕键盘 | 上游 `jjsullivan5196/wvkbd`，仓库无源码 | 加一个带版本锁 + sha256 的构建脚本（meson 项目） |
| `grim` | 截图 | 上游 `emersion/grim`，仓库无源码 | 同上（meson 项目） |
| `qbootctl` | 切换 A/B 槽位 | **仓库已有 `scripts/build-qbootctl.sh`** | 无需补充，直接用 |
| `arr` / `mango-arrange` | mangobar 时代的整理模式（Python） | **已废弃**，当前外壳用 `mango-actions` 的 ✋ 按钮；`mango-shell-start` 里只剩 legacy `pkill` | 无需补充 |

> 若你只关心「装到设备上能用」，不关心「从源码复现」，那 `wvkbd` / `grim` 可以忽略：
> 它们已经内置在 Release 的 rootfs 镜像里了。

---

## 6. 一次性完整流程（有设备可用时）

```sh
R=/abs/path/to/elish-archlinux
B=/abs/path/.build
D=$(TZ=Asia/Shanghai date +%Y%m%d)

# 1) 内核 + 固件 + initramfs + boot.img
BUILD_DIR=$B/kbuild $R/scripts/build-kernel.sh
$R/scripts/fetch-gpu-firmware.sh --out $B/firmware/elish-gpu
$R/scripts/build-release.sh --no-publish

# 2) mango 运行时（在设备上编，或从同版本设备取）
DEV_HOST=<device-ip> DEV_USER=<user> $R/scripts/build-mango-on-device.sh

# 3) 外壳组件（在设备上编，产出后拷回）
#    见 §4.1；把 mango-dock / mango-actions / mango-osd 放进 $B/device-overlay/usr/local/bin/

# 4) 装配 device-overlay（见 §4.3）

# 5) rootfs
ELISH_DEFAULT_PASSWORD=<发布口令> \
BUILD_ROOT=$B OUT_DIR=$R/output/$D/rootfs \
  $R/scripts/build-rootfs.sh \
    --modules  $B/kbuild/modinstall/lib/modules \
    --firmware $B/firmware/elish-gpu \
    --mango    $B/mango-stage \
    --overlay  $B/device-overlay \
    --size 16G --jobs 8

# 6) 打包 mango 运行时为 Release 资产
mkdir -p $R/output/$D/mango
tar -C $B/mango-stage -cf - . | zstd -T0 -19 -o $R/output/$D/mango/mango-0.18.3-aarch64.tar.zst

# 7) 汇总 + 发布
RELEASE_DEFAULT_PASSWORD=<发布口令> $R/scripts/build-release.sh --skip-kernel
```

`build-release.sh` 会自动：压缩 f2fs 镜像（GitHub 单资产 2 GiB 上限）、生成
`MANIFEST` / `SHA256SUMS.txt` / `RELEASE-NOTES.md`、创建 Release 并上传。

---

## 7. 踩过的坑（重建时最容易卡住的地方）

| 现象 | 根因 | 处理 |
|---|---|---|
| `resolver produced no packages` 且镜像缺包 | 依赖解析基线用了"当前树"而非底包 tarball | 已在 `build-rootfs.sh` 修复（以底包元数据为基线并缓存） |
| 镜像里没有面板/GPU 模块，首启黑屏 | `--modules` 指到了 `lib/modules/<kver>` | 改为指 `lib/modules`，脚本现已明确报错 |
| 构建在 `stage: device firmware` 后静默退出 | 硬编码了不存在的固件子目录 + `set -e`/`pipefail` | 已改为按实际文件 find |
| `xargs: $1: unbound variable` | 待下载清单为空时 GNU xargs 仍执行一次 | 已加空清单判断 |
| `sha256sum -c` 报 `rootfs/...` 不存在 | `MANIFEST`/`SHA256SUMS` 的根目录没跟随 `OUT_DIR` | 已改为跟随 `OUT_DIR` |
| `MISMATCH ... kernel/SHA256SUMS` | 交叉校验按 basename 全仓库搜，撞上同名汇总文件 | 已改为按产物根相对路径解析 |
| qemu-user-static 下载 404 | 镜像站只保留最新版 | 用 `QEMU_PKG_FILE=` 覆盖版本号 |
| `pkg-config: cjson` 找不到 | ALARM 的 .pc 名是 `libcjson` | `build-shell.sh` 已兼容两种名字 |
| mango 装进去但 DT_NEEDED 大量未解析 | rootfs 缺 pango/cairo 等传递依赖 | 必须确认构建日志 `mango DT_NEEDED resolution: N/N` 全解析 |

---

## 8. 提交前自检

```sh
/abs/path/scripts/audit-hygiene.sh    # 期望 0 failure
/abs/path/tests/run.sh                # 期望 0 failed
git -C <仓库> status --porcelain      # 期望为空
```
