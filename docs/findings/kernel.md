# 内核构建 findings（module: kernel）

工具链: clang/lld/LLVM 23.1.3（LLVM=1），ARCH=arm64，OUT-OF-TREE 构建。
脚本: scripts/build-kernel.sh（幂等、可 --rebuild、trap 打印阶段）。
所有中间产物: .build/kbuild（obj 树 5.0G，kbuild 总计 6.8G），仓库内只有源码/配置/脚本/文档。

## 0. 结论速览

| 项目 | 结果 |
|---|---|
| 源码 | qualcomm-sm8250/linux sm8250-7.2.0_git20261010，sha512 与 pmaports APKBUILD **完全一致**（265,984,302 B） |
| 内核 release | 7.2.0（Makefile VERSION/PATCHLEVEL/SUBLEVEL = 7/2/0，CONFIG_LOCALVERSION=""） |
| Image.gz | 15,489,546 B；file = gzip compressed；解压后 46,928,384 B = **Linux kernel ARM64 boot executable Image, little-endian, 4K pages** |
| dtb | sm8250-xiaomi-elish-boe 137,023 B / -csot 137,027 B，均为 Device Tree Blob v17 |
| 模块 | 587 个 .ko.zst（CONFIG_MODULE_COMPRESS_ZSTD=y → modules_install 产出压缩模块） |
| 全量编译耗时 | **578 s**（16 线程，10747 行日志，warning=3） |
| 失败轮次 | 最终版本 0 次失败；过程中 1 次主动中止（见 §9） |

## 1. 实际执行的命令（全文）

    # 依赖（用户级，无系统级安装）：bc 1.08.2-1、dtc 1.8.1-1 从
    #   https://geo.mirror.pkgbuild.com/extra/os/x86_64/ 下载，bsdtar 解包到 .build/tools/usr
    # 构建
    make -C .build/kbuild/src/linux-sm8250-7.2.0_git20261010 \
         O=.build/kbuild/obj ARCH=arm64 LLVM=1 -j16 Image.gz dtbs modules
    # 后续
    make -C <src> O=<obj> ARCH=arm64 LLVM=1 INSTALL_MOD_PATH=.build/kbuild/modinstall INSTALL_MOD_STRIP=1 modules_install
    make -s -C <src> O=<obj> ARCH=arm64 LLVM=1 kernelrelease          # => 7.2.0

- **未使用 CROSS_COMPILE**：LLVM=1 已让 kbuild 用 clang/ld.lld/llvm-* 直接产出 aarch64 目标，实测通过；全程没有引入 aarch64 gcc。
- config 生成：scripts/kconfig/merge_config.sh -m -O <obj> <base config> <fragments...>，随后 make olddefconfig。选 merge_config.sh 而非 scripts/config 的理由：fragment 文件本身就是仓库内可审阅的配置差异，合并顺序确定，且会打印"被覆盖的原值"，便于审计。

## 2. 产物（output/20261024/kernel/）

| 文件 | 字节 | SHA256 | 目标分区 |
|---|---|---|---|
| Image.gz | 15,489,546 | c73258818fc58dca513ef01209041e721c9ae8c8ed448d36297a3e5d4f2e5e60 | boot_b |
| dtbs/qcom/sm8250-xiaomi-elish-boe.dtb | 137,023 | ff999e835d837b78e545e9a5e4bb7ab6705ee5b321a4a892ff53233e3efc5077 | boot_b |
| dtbs/qcom/sm8250-xiaomi-elish-csot.dtb | 137,027 | 9dbb9ba5537904181ab6138048abb9c041e8b871128be32ee0c3aaf8ae2c2445 | boot_b |
| modules.tar.zst | 16,056,895 | f5b3d40b3f56d11ae4eda41bd81ded8fcabc40ff1c221eba62b0a75423a7251f | linux |
| config-7.2.0_git20261010 | 248,865 | ed8754bf7a135f1e05c51a1dae22b69500a921df63bf14d39c2c8b37eb60df4b | -（不进设备） |

sha256sum -c SHA256SUMS：5/5 成功；MANIFEST.kernel 内 6 行的 SHA256/大小与磁盘文件逐一比对一致。
模块 tar 结构：lib/modules/7.2.0/...，811 个条目，587 个 .ko.zst，无 build/source 软链。

## 3. 体积预算（boot_b 只有 192 MiB）

| 组成 | 字节 | 折合 |
|---|---|---|
| Image.gz | 15,489,546 | 14.77 MiB |
| 单个 dtb（CSOT 机型） | 137,027 | 0.13 MiB |
| 内核部分合计 | 15,626,573 | **14.90 MiB（192 MiB 的 7.8%）** |
| 剩余给 initramfs + boot image header | 185,700,019 | **177.1 MiB** |

结论：Image.gz 不算大，boot_b 不是瓶颈（CONFIG_DEBUG_INFO=y 不进 Image；Image 是去掉调试信息的原始内核，解压后 44.75 MiB）。
注意：**CSOT 面板用 -csot.dtb**；两个 dtb 不必同时放进 boot.img。

## 4. config 核查表（取自构建目录 .build/kbuild/obj/.config；=y 2066 个、=m 571 个）

| 符号 | 值 | 影响 |
|---|---|---|
| CONFIG_ARM64 | =y | 目标架构正确 |
| CONFIG_MODULES | =y | 有外部模块 |
| CONFIG_EXT4_FS | **=y** | ext4 直接内置 → **initramfs 不需要 ext4.ko** |
| CONFIG_F2FS_FS | **=y（本地 fragment 改，上游为 m）** | f2fs 根分区可内置挂载 → initramfs 不需要 f2fs.ko |
| CONFIG_F2FS_FS_XATTR / _POSIX_ACL | =y / =y | 随 fragment 保留 |
| CONFIG_F2FS_FS_COMPRESSION | not set | **不支持 f2fs 压缩**，mkfs 不要开 compression |
| CONFIG_DRM_MSM | =y（DRM_MSM_DSI/DPU/MDSS=y，DRM_PANEL_BRIDGE=y） | 显示主控内置 |
| 面板驱动 | 无 CONFIG_DRM_PANEL_XIAOMI_ELISH 这一符号；等价驱动是 **CONFIG_DRM_PANEL_NOVATEK_NT36523=m**（另有 NT36532=m） | 见 §5：是**模块** |
| CONFIG_PSI | =y（PSI_DEFAULT_DISABLED=n） | 内存压力观测可用 |
| CONFIG_SCHED_MC | =y | MC 调度域存在，性能调优可用 |
| CONFIG_DEBUG_FS | =y | debugfs 可用 |
| CONFIG_SCHED_DEBUG | **7.2.0 上游已删除该符号**（全树无 config SCHED_DEBUG 定义） | 不是我方漏配：kernel/sched/debug.c 无条件编译，/sys/kernel/debug/sched/ 仍存在 |
| CONFIG_SCHEDSTATS | not set（符号仍在 lib/Kconfig.debug:1396） | 无 sched 统计计数；性能阶段若需要请加 fragment |
| CONFIG_CGROUP_* | FREEZER/SCHED/CPUACCT/BPF=y，BLK_CGROUP=y，MEMCG=y | cgroup 控制族基本齐全 |
| CONFIG_ZRAM / CONFIG_TMPFS | =y / =y | zram、tmpfs 内置 |
| CONFIG_MODULE_COMPRESS / _ZSTD | =y / =y | modules_install 产出 .ko.zst |
| CONFIG_KERNEL_GZIP | =y | 保证 arch/arm64/boot/Image.gz 存在 |

## 5. 显示（面板）要点 —— 供 boot/initramfs 模块使用

两个 elish dtb 的面板 compatible 都落在 novatek NT36523 驱动里：
- sm8250-xiaomi-elish-boe.dts → compatible = "xiaomi,elish-boe-nt36523", "novatek,nt36523"（dtb 内 model = "Xiaomi Mi Pad 5 Pro (BOE)"）
- sm8250-xiaomi-elish-csot.dts → compatible = "xiaomi,elish-csot-nt36523", "novatek,nt36523"（dtb 内 model = "Xiaomi Mi Pad 5 Pro (CSOT)"）
- 驱动 of_match 位置：drivers/gpu/drm/panel/panel-novatek-nt36523.c:1249 与 :1253

因为它是 =m，要开机即出图，initramfs 必须带 panel-novatek-nt36523.ko.zst（在模块 tar 的
lib/modules/7.2.0/kernel/drivers/gpu/drm/panel/ 下）。触摸驱动 nt36523_ts.ko.zst 同样是模块。

## 6. 相对 pmaports 上游 config 的偏差（唯一一处）

- 文件：configs/kernel/fragments/50-f2fs-builtin.config
- 内容：一行注释（根文件系统驱动必须内置的理由）+ CONFIG_F2FS_FS=y
- 原始上游 config configs/kernel/config-postmarketos-qcom-sm8250.aarch64 **未修改**
  （sha256 47af492a30666c5c43b40109bd10336d45b108ff1c80cb1d49d3ad765a8fbe5f）
- merge 证据（.build/kbuild/config-merge.log 原文）：

      Using .../config-postmarketos-qcom-sm8250.aarch64 as base
      Merging .../fragments/50-f2fs-builtin.config
      Value of CONFIG_F2FS_FS is redefined by fragment .../50-f2fs-builtin.config:
      Previous value: CONFIG_F2FS_FS=m
      New value: CONFIG_F2FS_FS=y

## 7. 依赖与工具链备注（缺失项的用户级解决）

| 缺失项 | 解决方式 | 是否真的需要 |
|---|---|---|
| bc | Arch 包 bc-1.08.2-1-x86_64.pkg.tar.zst 解包到 .build/tools/usr，PATH 注入 | 必需（kernel/Makefile 用 bc） |
| dtc | 同上 dtc-1:1.8.1-1（镜像文件名带 epoch，URL 必须写成 dtc-1%3A1.8.1-1） | 非必需：arm64 make dtbs 用内核自带 scripts/dtc；仅作补充 |
| rsync / pigz | 未安装，也未用到 | 不需要 |
| CROSS_COMPILE | 未使用 | 不需要，LLVM=1 已足够 |

## 8. 模块压缩差异（与旧系统对比）

设备旧系统 /lib/modules 是未压缩 .ko（其 config 未开 MODULE_COMPRESS），本产物是 .ko.zst。
已与协调方确认：这不影响内核产物本身，只影响 initramfs/rootfs 处理模块的方式 —— 处理侧必须使用支持 zstd 解压的 kmod（Arch 的 kmod 默认支持），或用 fragment 关掉 MODULE_COMPRESS 重编。

## 9. 过程记录（失败/中止，如实）

1. 第 1 次全量编译（18:00:52 起）在完成约 3220 个目标文件后被**主动中止**：需要追加 f2fs 内置片段，继续跑只会得到错误 config 的产物。中止时未产出任何文件；已检查无 0 字节 .o 残留。
2. 一次"幂等空跑"曾截断上一轮 build.log，导致 3 条 warning 原文丢失 → 已给脚本加日志轮转（build.log → build.log.prev）；最终一轮全量日志完整保留在 .build/kbuild/build.log（10747 行，warning=3）。
3. 最终 3 条 warning 全部来自下游 fork 自己的驱动，无内核核心代码告警：
   - drivers/gpu/drm/panel/panel-samsung-amb655x.c:193:17: warning: unused variable 'dev' [-Wunused-variable]
   - drivers/power/supply/qcom_fg.c:1072:13: warning: no previous prototype for function 'qcom_fg_handle_soc_delta' [-Wmissing-prototypes]
   - drivers/power/supply/qcom_fg.c:1083:13: warning: no previous prototype for function 'qcom_fg_handle_mem_avail' [-Wmissing-prototypes]
4. merge_config.sh 会在 cwd 生成 .tmp.config.* 临时文件 → 脚本里显式 cd "$OBJ_DIR" 再调用，避免污染源码树。

## 10. 幂等性实测

第二次直接重跑脚本（不做任何清理）：

    [build-kernel] tarball cached & sha512 ok: 265984302 bytes
    [build-kernel] source tree cached: .../src/linux-sm8250-7.2.0_git20261010
    [build-kernel] config cached (2066 =y symbols)
    [build-kernel] build ok in 5s, warnings=0
    [build-kernel] modules: 587 module files (.ko.zst), tar=16055962 bytes
    [build-kernel] MANIFEST: .../MANIFEST.kernel (6 kernel rows)

即：不重复下载、不重复解压、不重复配置、make 无实际工作；MANIFEST 的 kernel 行被替换而非追加（仍 6 行）。
--rebuild 路径同样实测有效（最终一轮即 --rebuild：清 obj/modinstall/build.log 后全量重编 578 s）。

## 11. 刷入边界（脚本约束）

- 产物只针对 **boot_b**（Image.gz + dtb）与 **linux**（modules.tar.zst）两个分区。
- 本模块脚本**不调用 fastboot、不 erase、不触碰** system_b/dtbo_b/userdata/vendor_boot/super 等分区；MANIFEST 只声明"该文件应写到哪个分区"，真正刷入由 boot/flash 模块负责。
- MANIFEST.kernel 与其他模块共用：写入时用 flock 串行化，且只替换 kernel/ 前缀行，保留其它模块的行与统一表头。字段：路径|目标分区|SHA256|构建时间(UTC)|大小(字节)。

## 12. 遗留 / 风险

1. **模块目录名 = 7.2.0**，与设备旧系统的 7.2.0-xiaomi-elish+ 不同名。rootfs/initramfs 最终只能有一个 /lib/modules/7.2.0；旧目录残留会让 depmod/modprobe 找不到或加载错版本 —— 由 rootfs 模块负责清理。
2. 面板/触摸驱动是模块：不放进 initramfs，就只能等 udev 起来后才出图（且前提是根分区已挂载）。
3. CONFIG_SCHEDSTATS 未启用；CONFIG_SCHED_DEBUG 已在 7.2.0 上游移除。性能阶段若需要 schedstats，请加 fragment 后重编（不改上游 config）。
4. 产物不可复现：构建时间会嵌入镜像（同一 config 两轮 Image.gz 相差 15 B，两个 dtb 字节完全一致）。需要可复现构建时再引入 KBUILD_BUILD_TIMESTAMP。
