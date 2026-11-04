# 远端构建与发布（GitHub Actions）

**结论**：`.github/workflows/build-bootimg.yml` 手动触发一次，就在 GitHub 的 Arch 容器里交叉编译内核 + 打 initramfs + 生成
**两份 boot.img**（CSOT / BOE 各一份），然后以 **`yy/mm/dd`** 为标签和标题发布到 GitHub Release。
刷写规则不变：**只写 `boot_b` 与 `linux` 两个分区**（以 `docs/flash-guide.md` 为唯一真源）。

---

## 1. 它做了什么

| Job | 运行环境 | 干什么 | 失败即中断 |
|---|---|---|---|
| `meta` | ubuntu-latest | 算标签（`TZ=Asia/Shanghai date +%y/%m/%d`），校验必须匹配 `^[0-9]{2}/[0-9]{2}/[0-9]{2}$` | 是 |
| `build` | `archlinux:base-devel` 容器 | 装依赖 → `tests/test-bootimg.sh` / `test-flash-safety.sh` → `scripts/build-kernel.sh` → static busybox + `scripts/build-initramfs.sh` → 打两份 `boot.img` 并 `--verify` → 汇总资产 | 是 |
| `release` | ubuntu-latest | `gh release create`（同日重跑则 `upload --clobber` + `edit`）| 是 |

只用手动触发（`workflow_dispatch`）：一次内核编译在 4 核 runner 上要几十分钟，挂到 `push` 上是纯浪费。

## 2. 怎么触发

```sh
# 今天（标签 = 当天 yy/mm/dd）
gh workflow run build-bootimg.yml

# 指定日期：补发/重发某一天
gh workflow run build-bootimg.yml -f release_tag=26/11/08

# 只出 artifacts，不建 Release（先看产物再接发布）
gh workflow run build-bootimg.yml -f publish=false

gh run watch                     # 跟进度
```

Web UI：Actions → build-bootimg → Run workflow → 填 `release_tag`（留空=今天）→ Run。

> `release_tag` 是**硬校验**的：只接受 `yy/mm/dd`。打别的格式会直接 `exit 1`，避免 Release 列表无法按日期排序。

## 3. 产物

| 资产 | 用途 |
|---|---|
| `boot-csot.img` | CSOT 面板（本机面板）的 boot 镜像，`fastboot flash boot_b` 直接刷 |
| `boot-boe.img` | BOE 面板的 boot 镜像，同上 |
| `initramfs.cpio.gz` | 已经打进上面两个 img 的 initramfs（含面板驱动），单独放出来便于比对 |
| `modules.tar.zst` | 内核模块，解到 `linux` 分区的 `/lib/modules`（不刷 boot 分区） |
| `SHA256SUMS.txt` | 上面四个的校验和 |

两份 img 的差别**只有内核段里追加的 dtb**：`sm8250-xiaomi-elish-csot.dtb` 或 `...-boe.dtb`
（`scripts/make-bootimg.py` 按 2048 字节对齐追加，`kernel_size` 含 dtb，与 legacy mkbootimg 一致）。

命名说明：仓库历史产物叫 `boot.img`（=CSOT 默认）+ `boot-boe-alt.img`。Release 里刻意改成
`boot-csot.img` / `boot-boe.img`——两个文件同时下载时，`boot.img` 是"哪个面板"无法自证，而刷错面板 = 黑屏。
本地脚本、`collect-manifest.sh` 仍沿用旧名，不受影响（`flash-elish.sh` 只吃路径）。

## 4. 内部步骤与关键参数

每一步都能在本地原样跑；CI 只是把它们串起来。

| Workflow 步骤 | 本地等价 | 关键环境变量 |
|---|---|---|
| 交叉编译内核 | `bash scripts/build-kernel.sh` | `ROOT_DIR=$PWD`、`OUT_DIR=$PWD/.build/out/kernel`、`MANIFEST=…/MANIFEST.kernel`、`JOBS=$(nproc)` |
| 取 static busybox | 从 `BUSYBOX_URL`（`build-initramfs.sh` 里的常量）下载 .deb | 资产缓存到 `.build/dl` |
| 挑面板模块 | `scripts/pick-initramfs-modules.sh --modules-root …/lib/modules/<krel> --dest …/modstage` | 结果路径直接喂 `--modules` |
| 打 initramfs | `bash scripts/build-initramfs.sh --busybox … --modules … --out …/initramfs.cpio.gz` | `BUILD_ROOT=.build`（**必须显式给**） |
| 打两份 boot.img | `python3 scripts/make-bootimg.py --kernel … --ramdisk … --dtb …` | `--max-size` 默认 192 MiB |

三个"踩过就会静默出错"的点：

1. **`BUILD_ROOT` 必须显式设置**：`build-initramfs.sh` 缺省把它写到 `仓库上一级/.build`，在 CI 里那是容器根目录下的另一棵树，产物照样"成功"但不在预期位置。
2. **`JOBS` 用 `$(nproc)`**：`build-kernel.sh` 缺省 `-j16`，在 4 核 runner 上会超订并可能 OOM 被杀。
3. **面板驱动要靠依赖闭包挑，不能只挑一个 .ko**：`scripts/pick-initramfs-modules.sh` 从 `modules.dep` 做 BFS，
   把 `kernel/drivers/gpu/drm/panel/*` 以及它们的依赖一起打进去，并把 `modules.dep` 重写成"只含真正打进去的行"
   ——否则 `/init` 会对着几百个不存在的路径逐个 `insmod`（失败不致命但日志噪音极大）。

缓存只覆盖 `.build/dl`（内核源码 tar.gz ≈ 200 MB + busybox .deb），key 是
`hashFiles('scripts/build-kernel.sh','scripts/build-initramfs.sh')`：脚本一改就重建缓存，避免拿到旧归档。

## 5. 验证

### 5.1 Workflow 内部（不通过就不发 Release）

```sh
python3 scripts/make-bootimg.py --verify --input boot-csot.img   # 生成后立刻跑
grep -qx 'verify.status = OK' boot-csot.verify.txt               # 不 OK 就 exit 1
```

每份镜像的完整 `header.*` / `layout.*` / `sha256.*` 也写进该次运行的 Step Summary；verify 日志随 artifact 上传。

### 5.2 本地复核 Release 产物

```sh
gh release download 26/11/08 -p 'boot-csot.img' -p 'SHA256SUMS.txt'
sha256sum -c SHA256SUMS.txt
python3 scripts/make-bootimg.py --verify --input boot-csot.img
```

期望（实测值随内核大小变化，只断言结构与上限）：

| 字段 | 期望 |
|---|---|
| `verify.status` | `OK` |
| `header.magic` | `ANDROID!` |
| `header.page_size` | `4096` |
| `header.kernel_size` | 等于 `Image.gz + 对齐填充 + 1 个 dtb` |
| `header.cmdline` | `quiet rootwait root=PARTLABEL=linux console=tty0 loglevel=4` |
| `layout.file_size` | `≤ 201326592`（192 MiB，`boot_b` 硬上限），且为 4096 的整数倍 |
| `sha256.file` | 与 `SHA256SUMS.txt` 一致 |

## 6. 权限与安全

- `permissions: contents: write`：只有 `GITHUB_TOKEN`，不存任何个人 token / PAT。
- 外部 action 只用官方三个：`actions/checkout`、`actions/cache`、`actions/(upload|download)-artifact`；
  发 Release 用 runner 自带的 `gh`（不引第三方 release action）。
- 标签由日期决定，**同日重跑是幂等的**：`gh release upload --clobber` 覆盖资产、`gh release edit` 刷新说明，不新建重复 Release。
- `concurrency: build-bootimg`：排队而不是并发/取消，避免两次内核编译互相打断。
- 仓库里不出现任何凭据：workflow 只用内置 `GITHUB_TOKEN`，不含任何 PAT / IP / SSID / 密码字面量。

## 7. 已知限制

1. **只产 boot.img，不产 rootfs 镜像**。`build-rootfs.sh` 要 `fakeroot` + `qemu-aarch64-static` + 16 GiB 稀疏镜像，
   单次 runner 的磁盘/时长预算不合适；rootfs 继续本地构建（`docs/flash-guide.md`）。
2. **`configs/kernel/patches/*.patch` 目前不会被应用**：`build-kernel.sh` 里没有打补丁这一步。
   CI 忠实复现这一点（CI 产物 == 本地产物）；等脚本补上 patch 步骤，workflow 自动继承，无需改动。
3. **没有 ccache / 内核对象缓存**：每次从零编译。首次最慢（还要下 ~200 MB 源码归档），后续靠 `.build/dl` 缓存省掉下载。
4. **工具链随镜像漂移**：clang/lld 版本由 `archlinux:base-devel` 决定，不同日期构建的字节不保证完全可复现。
5. **需要 Workflow permissions 为读写**：仓库 Settings → Actions → General → Workflow permissions 选 "Read and write"，
   否则 `gh release create` 返回 403（Release job 会红）。
6. **构建时长**：4 核 runner 上内核编译是主要耗时；总时长上限 `timeout-minutes: 360`。

## 8. 排障

| 症状 | 原因 | 处理 |
|---|---|---|
| `invalid or corrupted package (PGP signature)` | 镜像里的 keyring 过期 | workflow 已 `pacman-key --init/--populate` + 先装 `archlinux-keyring`；仍失败就 Rerun |
| `面板驱动没编出来（modules.dep 里找不到）` | config 里 `CONFIG_DRM_PANEL_*=m` 被关掉 | 检查 `configs/kernel/fragments/` 与基础 config |
| `内核未产出 boe 面板 dtb` | dtb 名不对或 make dtbs 失败 | 看 kernel step 日志里 `make` 的失败点 |
| `boot image is N bytes, which exceeds the 201326592-byte` | initramfs 太大 / 多带了 dtb | 压 initramfs（`gzip -9`）、只带一个 dtb |
| `release_tag 必须是 yy/mm/dd` | 输入格式错 | 用 `26/11/08` 这种形式 |
| Release job 403 | Workflow permissions 只读 | 见第 7 节第 5 条 |
| 想只验证不发布 | —— | `-f publish=false`，或直接下 artifact |

## 9. 变更记录

- 新增 `.github/workflows/build-bootimg.yml`：`meta`/`build`/`release` 三段，标签规则 `yy/mm/dd`。
- 新增 `scripts/pick-initramfs-modules.sh`：从 `modules.dep` 裁出 initramfs 要带的模块子集（面板驱动 + 依赖），
  本地也可用：`scripts/pick-initramfs-modules.sh --modules-root <modinstall>/lib/modules/<krel> --dest /tmp/stage`。
