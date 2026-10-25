# 版本事实（versions.md）

> 调研快照时间：2026-10-24（宿主 UTC 未校准，按各源返回的 `committed_date`/`pushed_at` 记录）
> 调研方式：全部为只读 HTTP 抓取 + 本地解包；未使用 pmbootstrap、未执行任何写入设备或写 git 的操作。
> 证据分级：**已验证**（附 URL/commit/文件行号）｜**UNVERIFIED**（无一手证据，禁止当结论使用）

## 0. 结论速览（TL;DR）

1. **MangoWM 上游已从 wlroots 0.19 / scenefx 0.4 迁移到 wlroots 0.20 / scenefx 0.5**。
   分界提交 = `0fc7559c3c`（2026-06-19，标题 "version: chase wlroots 0.20"）。tag 0.14.4（2026-06-16）是最后一个
   `wlroots-0.19` + `scenefx-0.4` 版本；0.15.0（2026-07-09）起为 `wlroots-0.20` + `scenefx-0.5`。
   → 本仓库 README/plan 里写的 "wlroots 0.19.2 + scenefx 0.4.1" 只对应 **MangoWM ≤ 0.14.4**，不对应当前上游 main（0.18.3）。
2. **Arch Linux ARM 只提供 `wlroots0.20` 0.20.2-1 与 `scenefx` 0.5-1，没有 0.19.x 的 wlroots 包**。
   → 若走 wlroots 0.20 路线，用户态库**零自建**；若坚持 0.19 路线，必须自建 wlroots 0.19.3 + scenefx 0.4.1（AUR 对应 PKGBUILD 均只声明 x86_64）。
3. **AUR 不存在名为 `fex-emu` 的包**；同名近似包 `fex-emu-wine-git`（aarch64）只是 "FEX DLLs for enabling Wine's ARM64EC support"，不是 FEX 核心。
   FEX 官方 GitHub Release **没有任何 aarch64 二进制资产**（FEX-2608/2609/2610 的 assets 数均为 0），官方安装路径是 Ubuntu PPA 或源码构建。
   → 第一阶段（S1）**不必包含 FEX**，可在首次开机后在设备上构建/安装；S4 再纳入。

---

## 1. (a) MangoWM 上游对 wlroots / scenefx 的版本要求

仓库：`https://github.com/mangowm/mango`（AUR 包 `mangowm-git` 的 `url` 字段指向此处；**不是** `mangowm/mangowm`，
后者 GitHub API 返回 404：`https://api.github.com/repos/mangowm/mangowm`）。
默认分支 `main`，`pushed_at` = 2026-10-24T04:55:16Z，stars 3896。要求全部来自 `meson.build` 的 `dependency()` 行：

| tag | 日期 | meson.build `version` | wlroots 依赖（文件行号） | scenefx 依赖（文件行号） |
|-----|------|------------------------|--------------------------|--------------------------|
| main（=0.18.3） | 2026-10-24 | `0.18.3`（第 2 行） | `dependency('wlroots-0.20', version: '>=0.20.0')`（第 34 行） | `dependency('scenefx-0.5', version: '>=0.5.0')`（第 39 行） |
| 0.17.4 | 2026-09-28 | `0.17.4`（第 2 行） | 同上（第 34 行） | 同上（第 39 行） |
| 0.16.0 | 2026-08-12 | `0.16.0`（第 2 行） | `wlroots-0.20 >= 0.20.0`（第 33 行） | `scenefx-0.5 >= 0.5.0`（第 38 行） |
| 0.15.0 | 2026-07-09 | `0.15.0`（第 2 行） | `wlroots-0.20 >= 0.20.0`（第 33 行） | `scenefx-0.5 >= 0.5.0`（第 38 行） |
| **0.14.4** | 2026-06-16 | `0.14.4`（第 2 行） | **`dependency('wlroots-0.19', version: '>=0.19.0')`（第 32 行）** | **`dependency('scenefx-0.4', version: '>=0.4.1')`（第 37 行）** |

证据（可直接复核）：
- 原文抓取：`https://raw.githubusercontent.com/mangowm/mango/main/meson.build`（7580 B）、
  `https://raw.githubusercontent.com/mangowm/mango/0.14.4/meson.build`、`.../0.15.0/meson.build`、`.../0.16.0/meson.build`、`.../0.17.4/meson.build`
- 迁移提交：`0fc7559c3c`（2026-06-19，"version: chase wlroots 0.20"），见
  `https://api.github.com/repos/mangowm/mango/commits?path=meson.build&per_page=40`
- tag 列表：`https://api.github.com/repos/mangowm/mango/tags?per_page=30`（最新 0.18.3 = `8b876cb31d`）
- README 只列依赖名称、不写版本号（`README.md` 第 91/93 行分别指向 wlroots 与 scenefx），
  因此**版本约束的唯一权威是 meson.build**。

注意：`scenefx-0.4` / `scenefx-0.5` 是 **pkg-config 模块名**，不是包名；meson 依据 `.pc` 文件解析，
所以"需要 scenefx 0.4.1/0.5.0"实际等价于"需要提供 `scenefx-0.4.pc`/`scenefx-0.5.pc` 的包"。

---

## 2. (b) Arch Linux ARM 仓库当前版本（已验证）

方法（可重放）：
    curl -sSL -o extra.db.tar.gz http://mirror.archlinuxarm.org/aarch64/extra/extra.db.tar.gz   # 10,614,719 B
    curl -sSL -o core.db.tar.gz  http://mirror.archlinuxarm.org/aarch64/core/core.db.tar.gz     #    256,711 B
    tar -xzf extra.db.tar.gz --wildcards 'wlroots*' 'scenefx*' 'wayland*' 'seatd*' 'mesa*'
    # 再从每个 <pkg>/desc 读 %NAME% / %VERSION% / %ARCH% / %DEPENDS%

| 包名 | 版本 | 备注 |
|------|------|------|
| `wlroots0.20` | **0.20.2-1** | 仓库内**只有这一个 wlroots 包**；无 `wlroots`、无 `wlroots0.19`（`extra.db.tar.gz` 顶层目录名全量核对） |
| `scenefx` | **0.5-1** | `%ARCH%` = `aarch64`；描述 "drop-in replacement for the wlroots scene API" |
| `wayland` | 1.26.0-1 | |
| `wayland-protocols` | 1.49-1 | |
| `seatd` | 0.9.3-1 | |
| `mesa` | 1:26.2.5-1 | Adreno 650 走 freedreno/msm，无专有 blob 依赖 |
| `xorg-xwayland` | 24.1.14-1 | MangoWM 的 `xwayland` 选项需要 |
| `libinput` | 1.32.0-1 | MangoWM 要求 `>=1.27.1` ✔ |
| `libxkbcommon` | 1.13.2-1 | |
| `pixman` | 0.46.4-1.1 | |
| `pango` | 1:1.58.2-1 | |
| `cjson` | 1.7.19-1 | |
| `libdrm` | 2.4.134-1 | |
| `libglvnd` | 1.7.0-3 | |
| `glslang` | 1:1.4.363.0-1 | wlroots/scenefx 构建期需要 |
| `pkgconf` | 3.0.10-1 | |
| `meson` | 1.12.1-2 | MangoWM 构建工具 |
| `ninja` | 1.13.2-3 | |
| `python` | 3.14.8-1 | |

（`wayland` 版本 ≥1.23.1 的 MangoWM 要求、`libinput` ≥1.27.1 的要求，ALARM 全部满足。）

---

## 3. (c) AUR 现状（已验证）

方法：`https://aur.archlinux.org/rpc/v5/info?arg[]=...` **可用，未被拒绝**，因此不需要改用 packages 页面抓取；
补充 `https://aur.archlinux.org/rpc/v5/search/<term>?by=name(-desc)` 与 cgit 原始 PKGBUILD
（`https://aur.archlinux.org/cgit/aur.git/plain/PKGBUILD?h=<pkg>`）。

| 查询名 | 是否存在 | 版本 | `arch` | 关键字段 |
|--------|----------|------|---------|----------|
| `mangowm-git` | 是 | 0.17.4.r2.ge979638-2 | **`arch=("x86_64")` 仅此一项** | deps 含 `scenefx`、`libwlroots-0.20.so`；makedepends: git/meson/ninja/wayland-protocols>=1.41 |
| `wlroots0.19` | 是 | 0.19.3-1 | `arch=('x86_64')` | **orphan**（`Maintainer` 为 null）；`provides=("libwlroots-0.19.so")` |
| `scenefx0.4` | 是 | 0.4.1-2 | `arch=("x86_64")` | 维护者 `swayfx`；**OutOfDate = 1773697322（2026-03-16T21:42:02Z）**；`provides=("libscenefx-0.4.so")`，`depends` 含 `wlroots0.19` |
| `scenefx-git` | 是 | r269.87c0e8b-4 | — | `OutOfDate=1787880038`；deps 含 `libwlroots-0.19.so` |
| `fex-emu` | **否** | — | — | `/rpc/v5/info?arg[]=fex-emu` 未返回该包；`search/fex-emu?by=name-desc` 仅 1 条 = `fex-emu-wine-git` |
| `fex-emu-wine-git` | 是 | 2607.r0.g1cc4b93e7-2 | **`arch=('aarch64')`** | 描述 "FEX DLLs for enabling Wine's ARM64EC support"；makedepends: git/cmake/clang/lld/ninja/python；**不是 FEX 核心** |
| `scenefx`（AUR 裸名） | 否 | — | — | 只有 `scenefx-git`/`scenefx0.2`/`scenefx0.3`/`scenefx0.4` |

结论性观察：
- AUR 的 `mangowm-git` **已经跟着上游走到 0.17.4 + `libwlroots-0.20.so`**，与"0.19.2/0.4.1"的记忆不符；
- 三个相关 AUR 包（`mangowm-git`/`wlroots0.19`/`scenefx0.4`）的 `arch` 都**不含 aarch64**，
  意味着即使采用，也只能"借 PKGBUILD 逻辑、自行在 aarch64 上编译"，不能直接 `makepkg` 复用（须改 `arch`）。

---

## 4. (d) 结论：本仓库构建 MangoWM 应如何选依赖（A2 验收表）

| 包名 | 需要的版本 | ALARM 当前版本 | 结论 |
|------|------------|----------------|------|
| `wlroots`（pkg-config `wlroots-0.20`） | ≥0.20.0（MangoWM ≥0.15.0） | `wlroots0.20` 0.20.2-1 | **直接用 ALARM 包，不自建** |
| `scenefx`（pkg-config `scenefx-0.5`） | ≥0.5.0（MangoWM ≥0.15.0） | `scenefx` 0.5-1 | **直接用 ALARM 包，不自建** |
| `wlroots` 0.19 线 | ≥0.19.0 且 <0.20（MangoWM ≤0.14.4） | **无**（ALARM 不提供 0.19.x） | 需自建 wlroots 0.19.3；仅在必须锁 MangoWM ≤0.14.4 时才做 |
| `scenefx` 0.4 线 | ≥0.4.1 且 <0.5 | **无**（ALARM 只有 0.5） | 同上，需自建 scenefx 0.4.1 |
| `MangoWM` | 0.18.3（main 当前） | 不在仓库（需源码构建） | 取 tag `0.18.3` = `8b876cb31d`，用 ALARM 的 wlroots0.20+scenefx 直接 meson 构建 |
| `wayland`/`wayland-protocols`/`seatd`/`mesa`/`libinput` | 见 §1 约束 | 1.26.0 / 1.49 / 0.9.3 / 26.2.5 / 1.32.0 | 均满足，用 ALARM 包 |

**推荐路线（理由）**：
1. **采用 ALARM `wlroots0.20` 0.20.2 + `scenefx` 0.5 + MangoWM 0.18.3**，而不是自建 0.19.2/0.4.1。
   - 理由 A：上游已不再支持 0.19 线（§1 证据），0.19 路线等于把项目永久钉死在 2026-06 的旧 tag 上；
   - 理由 B：ALARM 现成包，省掉两个 ABI 敏感库的自建 + 校验（wlroots 的 SONAME `libwlroots-0.20.so` 与 scenefx `libscenefx-0.5.so` 成对出现，混用会链接失败）；
   - 理由 C：AUR 的 0.19 侧包一个 orphan、一个 OOD，属于无人维护状态，不能作为构建基石。
2. **不引入 Arch 主仓库的 `wlroots`/`scenefx` 之外的私有 ABI 副本**：本项目的 rootfs 来自 ALARM，
   混入自建 `wlroots0.19` 会同时拉进第二份 `libseat`/`libinput` 依赖树，扩大故障面。
3. 若后续一定要回到 0.19（例如某个补丁只对 0.19 生效），回退方式是明确的：
   `wlroots0.19` 0.19.3（AUR，orphan）+ `scenefx0.4` 0.4.1（AUR，OOD）+ MangoWM tag `0.14.4`，三者一起自建。

> 与 `docs/plan.md` S3 的差异（供 dispatcher 决策）：S3 验收写的是 "wlroots 0.19.2 / scenefx 0.4.1"，
> 该组合需要**三个自建包**且上游不再维护；改成 "wlroots0.20 0.20.2 + scenefx 0.5 + MangoWM 0.18.3" 后**零自建**。

---

## 5. FEX-Emu（aarch64 上跑 x86_64 Steam 游戏）

### 5.1 前提条件（已验证，来源逐条）

| 前提 | 结论 | 来源 |
|------|------|------|
| CPU | ARMv8.0-a 或更新，必须含 `FEAT_FP` 与 `FEAT_CRC32` | `https://raw.githubusercontent.com/FEX-Emu/FEX/main/Readme.md`（"Prerequisites" 段）。SM8250 = Cortex-A77/A55（ARMv8.2-a），满足 |
| box64 是否需要 | **不需要**。FEX 与 box64 是同类替代品（站内原文 "similar to qemu-user and box64"） | `https://fex-emu.com/` 首页首段 |
| x86-64 RootFS | **必须**。用 `FEXRootFSFetcher` 下载，或自行生成 | `Readme.md` 第 21-22 行；`https://wiki.fex-emu.com/index.php/Development:Setting_up_RootFS` |
| binfmt_misc | 需要（`binfmt-support` / `ninja binfmt_misc`），用于透明执行 x86 ELF | FEX wiki `...Setting_up_FEX&action=raw` 第 12 行、第 94-106 行 |
| Wine/Proton | 跑 Windows 游戏需要 Wine + **ARM64EC** 通路，对应包 = AUR `fex-emu-wine-git`（提供 FEX 的 ARM64EC thunk DLL：`xtajit64.dll`/`libwow64fex.dll`） | AUR `https://aur.archlinux.org/rpc/v5/info?arg[]=fex-emu-wine-git`；PKGBUILD `arch=('aarch64')` |
| Proton 版本 | x86_64 Proton 通过 FEX 跑可行；官方推荐走 Wine ARM64EC。**具体 Proton 版本对 FEX 的兼容性：UNVERIFIED**（本轮未找到一手矩阵） | — |
| 内核页大小 | FEX 文档未在抓到的段落中给出硬性 4K 页要求 → **UNVERIFIED**（需真机验证） | — |

### 5.2 构建依赖与构建规模

FEX 官方 wiki（Debian/Ubuntu 段）给出的构建依赖（节选，原文逐条）：
`git, cmake, ninja-build, pkgconf, ccache(可选), clang, llvm, lld(ThinLTO), binfmt-support, libssl-dev,
python3-setuptools, g++-x86-64-linux-gnu(构建 thunks 用), libgcc-<ver>-dev-i386-cross/-amd64-cross,
nasm(仅测试), python3-clang, libstdc++-12-dev-{i386,amd64,arm64}-cross, squashfs-tools, squashfuse`；
FEXConfig GUI 另需 qtdeclarative6 系列。
官方构建命令（原文）：
    git clone --recurse-submodules https://github.com/FEX-Emu/FEX.git
    cd FEX && mkdir Build && cd Build
    CC=clang CXX=clang++ cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release \
      -DUSE_LINKER=lld -DENABLE_LTO=True -DBUILD_TESTING=False -DENABLE_ASSERTIONS=False -G Ninja ..
    ninja

规模实测（本轮实际 clone 统计，`git clone --depth 1`，HEAD `9eb5d92`，2026-10-24）：
- tracked 文件 **3652** 个，其中 `*.cpp` **318** 个（`FEXCore/**/*.cpp` 86 个），工作树 37 MB；
- 官方推荐 `ENABLE_LTO=True` + `lld`，属典型重编译负载；
- **构建时长：UNVERIFIED**（本轮未实测）。按 318 个 C++ TU + ThinLTO 在 16 核上的经验区间，粗估 20–60 分钟
  （估算，不是实测，禁止写进 bug.md 当事实）。

### 5.3 是否有预编译 aarch64 二进制？

- **GitHub Release：没有二进制资产**。`https://api.github.com/repos/FEX-Emu/FEX/releases?per_page=3` 返回
  FEX-2610（2026-10-07）、FEX-2609、FEX-2608，三者 `assets` 数量均为 **0**（release body 只是指向博客文章）。
- 官方"一键安装"只覆盖 Ubuntu：`curl --silent https://raw.githubusercontent.com/FEX-Emu/FEX/main/Scripts/InstallFEX.py | python3`
  （PPA 方式，`Readme.md` 第 30-33 行）。
- Arch Linux ARM 仓库内**没有** `fex`/`fex-emu`/`box64` 包（`extra.db.tar.gz`+ `core.db.tar.gz` 全量包名 grep 结果为空）。
- AUR 只有 `fex-emu-wine-git`（Wine ARM64EC DLL），无 FEX 核心包。

### 5.4 结论：第一阶段是否必须包含 FEX？

**不必须，建议首个镜像不含 FEX，首次开机后再装/构建。** 依据：
1. FEX 不参与开机链路：它是用户态 binfmt 处理器，缺它只影响 x86_64 程序，不影响 systemd/MangoWM 启动；
2. 没有可直接落地的 aarch64 二进制（§5.3），任何"含 FEX 的镜像"都意味着**在 x86_64 宿主上交叉构建 aarch64 FEX**
   且需要 x86 交叉 thunk（`g++-x86-64-linux-gnu` 等），复杂度与 S1 的内核/rootfs 目标无关，属 S4；
3. RootFS（`FEXRootFSFetcher` 下载的 x86-64 rootfs，通常数 GB）本身就是运行期资产，不适合塞进只写 `linux` 分区的镜像。

**遗留风险（写进 bug.md 用）**：AUR `fex-emu-wine-git` 声明 `arch=('aarch64')` 而宿主是 x86_64，
"在设备上构建"仍是 S4 的首选路径；任何在宿主上构建都需要 aarch64 交叉工具链或 `unshare`+qemu-user（本机未验证可用）。

---

## 6. UNVERIFIED 清单（versions.md）

- U1 FEX 在 elish（SM8250，4K/16K 页）上的内核页大小硬性要求 → 未找到一手文档。
- U2 Proton 具体版本 × FEX 的兼容矩阵 → 未找到一手证据（官方只提供 Wine ARM64EC 通路）。
- U3 FEX 在 16 核 x86_64 宿主上交叉构建的实际时长 → 未实测，仅为估算。
- U4 MangoWM 0.18.3 在 aarch64 + ALARM `wlroots0.20`/`scenefx` 上能否一次通过编译 → 未实际构建（本轮只读调研）。
- （原 U5「meson/ninja 是否在库内」已在复核中关闭：`meson` 1.12.1-2、`ninja` 1.13.2-3、`pkgconf` 3.0.10-1、`python` 3.14.8-1 均在 ALARM 仓库，见 §2 表。）

---

## 7. CONFLICT-1：任务书版本约束 vs 上游现状

**约束原文（本仓库 README.md / docs/plan.md S3）**：MangoWM 桌面环境要求 "wlroots 0.19.2 + scenefx 0.4.1"。

**上游一手证据（全部为 meson.build 的 dependency() 原文行）**：

- 仓库：https://github.com/mangowm/mango （默认分支 main）
- tag **0.14.4**（2026-06-16），文件 https://raw.githubusercontent.com/mangowm/mango/0.14.4/meson.build
  - 第 32 行原文：wlroots_dep = dependency('wlroots-0.19',version: '>=0.19.0')
  - 第 37 行原文：libscenefx_dep = dependency('scenefx-0.4',version: '>=0.4.1')
- 分界提交：**0fc7559c3c**，日期 **2026-06-19**，提交标题 **"version: chase wlroots 0.20"**
  （来源：https://api.github.com/repos/mangowm/mango/commits?path=meson.build&per_page=40）
- tag **0.15.0**（2026-07-09），文件 .../0.15.0/meson.build
  - 第 33 行原文：wlroots_dep = dependency('wlroots-0.20',version: '>=0.20.0')
  - 第 38 行原文：libscenefx_dep = dependency('scenefx-0.5',version: '>=0.5.0')
- tag **0.16.0**（2026-08-12）：第 33 / 38 行，内容与 0.15.0 相同
- tag **0.17.4**（2026-09-28）与 **main = 0.18.3**（2026-10-24），文件 .../main/meson.build
  - 第 34 行原文：wlroots_dep = dependency('wlroots-0.20',version: '>=0.20.0')
  - 第 39 行原文：libscenefx_dep = dependency('scenefx-0.5',version: '>=0.5.0')
  - 第 2 行：version : '0.18.3'

**判定**：任务书写死的 "wlroots 0.19.2 + scenefx 0.4.1" **只对应 MangoWM <= 0.14.4**；
对 0.15.0 及以后的任何 tag（含当前 main 0.18.3）都不成立——从 0.15.0 起上游要求
wlroots >= 0.20.0 且 scenefx >= 0.5.0（pkg-config 模块名 wlroots-0.20 / scenefx-0.5）。

**后果**：若要维持该约束，必须同时自建 wlroots 0.19.3 与 scenefx 0.4.1（ALARM 仓库没有 0.19 线，见本文件 §2），
并把 MangoWM 钉在 0.14.4（2026-06-16）这个上游已不再修复的 tag 上。这与"零自建"目标冲突。
**处置（已落定）**：dispatcher 已记录用户 2026-10-24 裁决 = `docs/decisions.md` **D-008**——采用 MangoWM 0.18.3 + ALARM wlroots0.20/scenefx 0.5（零自建），并把任务书的 "wlroots 0.19.2 / scenefx 0.4.1" 视为对当时上游状态的描述而非永久锁版。本文件保留原始证据以供复核。

---

## 8. 补充：aarch64 上启用 FEX 的最可行路径（依赖规模）

- **路径 A（推荐）——在设备上用源码构建 FEX 核心**：
  官方构建依赖最小集（FEX wiki Debian 段）= git / cmake / ninja-build / pkgconf / clang / llvm / lld /
  libssl-dev / python3-setuptools（+ 可选 ccache、squashfs-tools、squashfuse、binfmt-support）；
  若要构建 thunk（Wine ARM64EC / 库转发）还需 x86_64 交叉 gcc 与 i386/amd64 交叉 libgcc/libstdc++；
  构建配置 = cmake -DCMAKE_BUILD_TYPE=Release -DUSE_LINKER=lld -DENABLE_LTO=True -DBUILD_TESTING=False -G Ninja。
  规模参考：AUR 的 fex-emu-wine-git（只构建 Wine 用的 DLL，不含核心）makedepends 仅 6 项
  （git / cmake / clang / lld / ninja / python）；FEX 核心本体在 AUR 中**不存在**。
- **路径 B——等上游提供 aarch64 二进制**：现状不可用（FEX-2608/2609/2610 的 Release assets 均为 0 个；
  官方一键安装只覆盖 Ubuntu PPA）。需持续跟踪。
- **路径 C——在 x86_64 宿主交叉构建**：需要 aarch64 交叉工具链 + x86 thunk 双重交叉，本轮未验证可行性（UNVERIFIED）。
- 三条路径的共同前置：x86-64 RootFS（FEXRootFSFetcher 下载，运行期资产，数 GB 量级）与 binfmt_misc 注册。

**结论重申**：S1 镜像不含 FEX；首次开机后按路径 A 在设备上构建/安装，或按路径 B 等二进制。
