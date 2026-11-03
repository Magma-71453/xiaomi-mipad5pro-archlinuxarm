# 构建性能：SM8250 上的 makepkg 参数与实测

> 目标设备：Xiaomi Mi Pad 5 Pro（`xiaomi-elish`，SM8250-AC / 骁龙 870，8 核异构）
> 环境：Arch Linux ARM，kernel 7.2.0，gcc 16.1.1，rustc 1.99.1，pacman 7.1.0，zstd 1.5.7，xz 5.8.4
> 本文记录 `/etc/makepkg.conf` + `/etc/makepkg.conf.d/*` 的按机优化：为什么这么写、实测收益、
> 怎么部署 / 回滚 / 验证。设备副本与仓库 `configs/rootfs/etc/makepkg.conf*` 一致。

## 1. 结论先行

| 项 | 旧值 | 新值 | 理由 / 实测 |
|---|---|---|---|
| CFLAGS/CXXFLAGS arch 部分 | `-march=armv8-a` | `-march=armv8.2-a+dotprod+crc+crypto+fp16+lse+rcpc -mtune=cortex-a77` | 两簇公共 ISA；CoreMark +7.4%（A77）/+4.9%（A55）；原子负载 ~1.7x |
| FFLAGS/FCFLAGS | 同上旧值 | 同上新值 | 与 C 保持一致 |
| RUSTFLAGS | 空 | `-C target-cpu=cortex-a77` | 两簇实跑通过（见 §6） |
| LTOFLAGS | 空 | `-flto=4` | 仅单包 `options=(lto)` 时生效；全局仍 `!lto` |
| BUILDENV | `!ccache` | `ccache` | ccache 4.14.1，缓存上限 10 GB |
| COMPRESSZST | `zstd -c -T0 -` | `zstd -c -z -q -T0 -` | 与上游命名习惯一致，静默输出 |
| PKGEXT | `.pkg.tar.xz` | `.pkg.tar.zst` | 64 MiB 负载打包 23.6 s → 0.62 s |
| MAKEFLAGS | `-j$(nproc)` | 不变（= -j8） | 见 §4.4 |
| `configs/rootfs/packages.list` | — | `+ccache` | 镜像重建时与 BUILDENV 保持一致 |
| 全局 `-O3` / LTO / lld | — | 不启用 | `-O3` 实测更慢；LTO 在 7.4 GiB 上风险高；lld 为可选项 |

> 重要边界：这些参数只影响**本机重新编译**的包。从 ALARM 仓库安装的预编译包仍是通用
> armv8-a 二进制，不受影响。

## 2. 先纠正拓扑：本机不是 8×Cortex-A55

用户提供的 `cpu-info` 输出（cpuinfo r843）把 8 个核全报成 Cortex-A55，与内核实测不符：

| 证据源 | cpu0-3 | cpu4-7 |
|---|---|---|
| `/proc/cpuinfo` MIDR | `0x51df805e`（implementer 0x51 = Qualcomm，part 0x805） | `0x411fd0d0`（implementer 0x41 = ARM，part 0xd0d = Cortex-A77） |
| `lscpu` 型号名称 | Kryo-4XX-Silver（A55 派生） | Cortex-A77 |
| `cpu_capacity` | 284 | 871（cpu4-6）/ 1024（cpu7） |
| cpufreq policy | policy0：cpu0-3，max 1804.8 MHz | policy4：cpu4-6 max 2419.2 MHz；policy7 max 2841.6 MHz |

`/proc/cpuinfo` 的 Features 八核一致（arm64 的系统级 HWCAP 取各核交集）：

```
fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp
```

**结论**：按 `-mcpu=cortex-a55` 或 ALARM 旧默认 `-march=armv8-a` 编都浪费了 A77；但也不能
只按 A77 开全部特性。正确做法是**两簇公共特性 + 面向 A77 调优**。`cpu-info` 的该行输出
不要用于生成编译参数；`lscpu` / MIDR / `cpu_capacity` 才是真相。

## 3. 编译参数：公共 ISA + A77 调优

### 3.1 两簇公共 ISA → GCC 扩展

| `/proc/cpuinfo` Features token | 含义 | 写法 |
|---|---|---|
| `fp` / `asimd` | FP + NEON | armv8.2-a 默认 |
| `aes` / `pmull` / `sha1` / `sha2` | ARMv8 crypto | `+crypto` |
| `crc32` | CRC | `+crc` |
| `atomics` | LSE 原子指令 | `+lse` |
| `asimddp` | int8 dot product | `+dotprod` |
| `asimdrdm` | RDM（QRDMX） | armv8.2-a 默认（宏 `__ARM_FEATURE_QRDMX`） |
| `fphp` / `asimdhp` | FP16 运算 | `+fp16` |
| `lrcpc` | RCpc 加载 | `+rcpc` |
| `evtstrm` / `cpuid` / `dcpop` | 与代码生成无关 | 不用管 |

未出现在 Features 里的（sve、i8mm、bf16、fp16fml、jscvt、sha3、sm4、pauth 等）一律不加。

最终 arch 串：

```sh
-march=armv8.2-a+dotprod+crc+crypto+fp16+lse+rcpc -mtune=cortex-a77
```

### 3.2 为什么不用 `-mcpu=cortex-a77` / `-mcpu=cortex-a55`

- GCC 16.1.1 的 `-mcpu` 双核名只枚举到 `cortex-a76.cortex-a55`，没有
  `cortex-a77.cortex-a55`（实测直接报 `unknown value`）；
- 单用 `-mcpu=cortex-a77` 不会启用 crypto（GCC 的 A77 定义里没有），而本机八核都有
  aes/sha2；
- 用 `-march=...+mtune=cortex-a77` 把每个扩展显式写出来，便于逐条对照 `/proc/cpuinfo`
  审计；`-mtune` 只影响调度，不改变 ISA。

### 3.3 基准（设备实测；微基准临时 `performance` governor + `taskset` 分簇）

CoreMark（12000 iterations，取中位数；越高越好）：

| flags | cpu7（A77 prime） | cpu2（A55 簇） |
|---|---|---|
| 旧 `-march=armv8-a` | 23483 | 6303 |
| `-mcpu=cortex-a76.cortex-a55+crypto` | 25210 | 6645 |
| **本配置**（march + `-mtune=cortex-a77`） | **25210** | **6611** |
| `-march=...+ -mtune=cortex-a76.cortex-a55` | 25210 | 6708 |

→ 相对旧值 **+7.4% / +4.9%**；三种新写法互差 <1.5%，属代码布局噪声。tune 选谁差别不大，
因此选与性能核一致的 A77 tune。

lz4 1.10.0（`-b1 -i3 -B4`，64 MiB 语料，压缩 MB/s，取中位数）：

| flags | cpu7 | cpu2 |
|---|---|---|
| 旧 `-march=armv8-a` | 483.5 | 117.9 |
| **本配置** | **486.2** | **119.7** |
| `-mcpu=cortex-a76.cortex-a55+crypto` | 486.4 | 118.3 |
| `-O3`（A77 tune） | 463.0 | 107.6 |

lz4 基本是内存受限，收益小；**`-O3` 在这台机器上反而慢 ~4.5%，保持 `-O2`**。

LSE 原子负载（8 线程 × 800 万次 relaxed `fetch_add`，wall time）：

| build | 3 次 wall |
|---|---|
| 旧 `-march=armv8-a`（GCC 默认 outline atomics，调用 `__aarch64_ldadd8`） | 8.46 / 9.34 / 9.31 s |
| `-march=armv8.2-a+lse`（纯 LSE） | 5.12 / 5.08 / 5.12 s |
| 本配置 | 5.25 / 5.34 / 5.49 s |

`-march=armv8-a` 下 GCC 生成 outline atomics（libgcc 运行时派发、函数调用）；直接 `+lse`
后是单条 `ldadd`。本机所有核都支持（Features `atomics`），多线程 malloc / 引用计数类负载
明显更快（本微基准 ~1.7x）。

### 3.4 刻意没开的

| 项 | 原因 |
|---|---|
| 全局 LTO（`OPTIONS=(lto)`） | 7.4 GiB 内存，大包（llvm/firefox/qt）`-flto` 易 OOM 且链接时间翻倍；保留 `!lto`，需要时单包 `options+=(lto)`（LTOFLAGS 已给 `-flto=4`） |
| `-O3` | 实测更慢（见上） |
| `-fomit-frame-pointer` | 仓库需要 profile/debug；保留现有 hardening 组合（帧指针 + stack protector + FORTIFY=3 + stack-clash），代价约 1–3% |
| lld / mold 作为默认链接器 | `ld.lld` 23 已装，实测接受现有 LDFLAGS（`-z pack-relative-relocs`、`--sort-common` 均 OK）；个别包对 linker script 的兼容性未知，默认仍用 bfd。想提速可自行在单包环境 `LDFLAGS+=" -fuse-ld=lld"` 验证 |
| i8mm / bf16 / sve / pauth / fp16fml | 本机 HWCAP 没有，加了会在 A55/A77 上出非法指令 |

## 4. 打包与构建吞吐

### 4.1 PKGEXT 换 zstd（`COMPRESSZST` 带 `-T0`）

64 MiB 语料（`tar -C /usr/lib -cf - libLLVM.so.23.1` 后取前 64 MiB）：

| 方式 | wall | 产物 |
|---|---|---|
| `xz -c -6`（旧 PKGEXT `.pkg.tar.xz`） | 23.61 s | 19,726,064 B |
| `zstd -c -T1`（stdin） | 0.684 s | 29,371,375 B |
| `zstd -c -T0`（stdin，makepkg 实际用法） | 0.62 s | 29,371,375 B |
| `zstd -c -T0 <file>` | 0.62 s | 29,371,375 B |

→ 打包快 **~38x**；包体积大 ~49%（zstd level 3 换速度，本机 172 GiB 空闲，值得）。pacman
7.1.0 原生支持 `.pkg.tar.zst`（`pacman -Qip` 已验）。

### 4.2 ccache

装 `extra/ccache`（4.14.1，依赖 hiredis），`BUILDENV` 打开，缓存上限 10 GB：

```sh
sudo pacman -S --needed ccache
ccache --set-config=max_size=10G
```

lz4-local 小包实测（停 schedplace，见 4.3）：

| 场景 | wall | build() | ccache |
|---|---|---|---|
| 冷（`ccache -C`） | 19.07 s | 11.94 s | 7 misses |
| 暖（同源同 flags） | 17.58 s | 10.39 s | 7/7 可缓存调用 direct hit |

小包只省 ~1.5 s：`ccache -s -v` 显示 uncacheable 调用里 80% 是 "called for linking"
（链接不缓存），编译单元越多收益越明显。首次构建无收益。

### 4.3 关键：schedplace 默认把 makepkg 钉在小核（A55 簇）

实测（lz4-local，`CCACHE_DISABLE=1`，冷构建）：

| 状态 | makepkg wall | build() | package() |
|---|---|---|---|
| schedplace 运行 | 53.95 s | 44.1 s | 0.92 s |
| `systemctl stop schedplace` | **18.99 s** | **11.85 s** | 0.59 s |

采样到构建期间 `makepkg`/`make`/`cc1` 的 `Cpus_allowed_list` 全是 `0-3`（小核簇）。原因见
`configs/perf/schedplace.conf` 的 `[rule background-pkg]`：`process = ^(pacman|makepkg|...)$`
→ `cpuset = background`。这是既定设计（构建可被抢占，保护桌面），**不要长期改**；要全速
构建就临时停服务：

```sh
sudo systemctl stop schedplace
makepkg -si
rc=$?
sudo systemctl start schedplace
exit $rc
```

停用期间桌面在重负载下会更抖；`stop` 会按 `restore_on_exit` 恢复线程 affinity（设计见
`docs/perf.md` §9）。

### 4.4 并行度 -j8

`MAKEFLAGS="-j$(nproc)"` = -j8 不变。7.4 GiB 内存 + 10 GiB zram（lz4）足够普通包；巨型
C++/Rust 包若内存紧张，单包用 `MAKEFLAGS=-j4`（经 `makepkg.conf` 覆盖或用
`options=(!makeflags)`）。不建议为散热降 -j：本机靠 EAS/thermal 自调频，不锁频。

## 5. 部署与回滚

真相源：`configs/rootfs/etc/makepkg.conf`、`configs/rootfs/etc/makepkg.conf.d/{fortran,rust}.conf`；
设备路径：`/etc/makepkg.conf`、`/etc/makepkg.conf.d/`。

本次部署（已执行；旧配置备份在 `/etc/makepkg.conf.bak-20261104-004012` 与
`/etc/makepkg.conf.d.bak-20261104-004012`）：

```sh
# 宿主仓库根目录
tar czf /tmp/makepkg-conf.tar.gz -C configs/rootfs \
  etc/makepkg.conf etc/makepkg.conf.d/fortran.conf etc/makepkg.conf.d/rust.conf
cat /tmp/makepkg-conf.tar.gz | dbclient -y -i ~/.ssh/dropbear_ed25519_host_key magma@<device-ip> \
  'cat > /tmp/makepkg-conf.tar.gz'

# 设备上（magma 有 NOPASSWD sudo）
TS=$(date +%Y%m%d-%H%M%S)
sudo cp -a /etc/makepkg.conf "/etc/makepkg.conf.bak-$TS"
sudo cp -a /etc/makepkg.conf.d "/etc/makepkg.conf.d.bak-$TS"
sudo tar -C / -xzf /tmp/makepkg-conf.tar.gz
```

回滚：

```sh
sudo cp -a /etc/makepkg.conf.bak-20261104-004012 /etc/makepkg.conf
sudo rm -rf /etc/makepkg.conf.d
sudo cp -a /etc/makepkg.conf.d.bak-20261104-004012 /etc/makepkg.conf.d
```

ccache 回滚：把 `BUILDENV` 里的 `ccache` 改回 `!ccache`（或 `sudo pacman -Rns ccache` 后同样处理）。

## 6. 验证

```sh
bash -c '. /etc/makepkg.conf; echo "$CFLAGS"; echo "$PKGEXT"; echo "$MAKEFLAGS"'
# 期望含 -march=armv8.2-a+dotprod+crc+crypto+fp16+lse+rcpc -mtune=cortex-a77、.pkg.tar.zst、-j8
bash -c '. /etc/makepkg.conf.d/rust.conf; echo "$RUSTFLAGS"'   # -C target-cpu=cortex-a77
md5sum /etc/makepkg.conf /etc/makepkg.conf.d/*.conf            # 与仓库一致（本次 b77f2d4b… 等）
```

端到端（本次实测产物 `lz4-local-1.10.0-1-aarch64.pkg.tar.zst`，190 KB，`pacman -Qip` 可读）：

```sh
makepkg -f --nodeps --noconfirm
ls *.pkg.tar.zst
bsdtar -xf *.pkg.tar.zst -C /tmp/x && taskset -c 2 /tmp/x/usr/bin/lz4 -b1 -i1 -B4 /dev/urandom
```

A55 合法性（新 ISA 必须在小核上也跑得动）：用 `taskset -c 0-3` 跑构建产物；本次 lz4 与
Rust 示例在 cpu2 上均 exit 0（无 SIGILL）。

## 7. 已知限制

- 只影响**本机重建**的包；ALARM 预编译包不受影响。
- `-march=...+crypto` 绑定本机 HWCAP；换内核/换机型务必重看 `/proc/cpuinfo` Features。
- `cpu-info`（r843）在本机误报 8×A55，不要用它生成 flags。
- zstd 包比 xz 大（本语料 +49%）；磁盘紧时单包可临时改回 `PKGEXT=.pkg.tar.xz`。
- LTO 默认关；单包 `options=(lto)` 时 `-flto=4` 仍可能吃内存或失败，大包建议 `!lto`。
- ccache 不加速链接、不加速 Rust（未接 `RUSTC_WRAPPER`）。
- 构建期间停 schedplace 会牺牲桌面平滑（见 4.3）；恢复命令别忘。
- 无风扇：长时间满载靠内核 thermal 降频；短时构建 sensor 约 38–45 ℃，持续满载会更高。
  未锁频、未改全局 governor。
- 基准数据在桌面（mango/wayland）与其他交互负载并存时采集，微基准有 ±1–2% 噪声；
  大差异（LSE 1.7x、打包 38x、schedplace 2.8x）是稳健的。

## 8. 基准方法（可复现）

- 微基准：`taskset -c 7`（A77 prime）/ `taskset -c 2`（A55 簇）；临时
  `echo performance > /sys/devices/system/cpu/cpufreq/policy{0,4,7}/scaling_governor`，
  测完恢复 `schedutil`（本次已恢复）。
- CoreMark：`gcc -O2 <flags> -I. -Iposix core_list_join.c core_main.c core_matrix.c
  core_state.c core_util.c posix/core_portme.c -DPERFORMANCE_RUN=1 -DFLAGS_STR='"-O2"'`，
  12000 iterations，取中位数。
- lz4：`lz4 -b1 -i3 -B4 64MiB语料`，取中位数。
- 压缩：`xz -6` vs `zstd -T0` 对同一 tar 流计时（stdin）。
- 构建 A/B：同一 PKGBUILD、`CCACHE_DISABLE=1`、分别 `systemctl stop/start schedplace`。
