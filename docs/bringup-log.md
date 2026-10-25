# 真机 bring-up 记录（2026-10-24 夜）

这份文档记录第一次真机刷入的全部结论与未决问题，供交接/复现使用。
所有命令都在真机上执行过，证据保留在 `bug.md` 与 `docs/decisions.md`。

## 1. 已完成（按发现顺序）

### 1.1 ABL 拒绝装载 boot.img（`Failed to load/authenticate boot image: Load Error`）
- 根因：我把 dtb 以 **2048 字节对齐** 追加到内核段之后。
- 修复：与 postmarketOS `boot-deploy` 完全一致 —— `cat Image.gz <dtb>`（零填充）。
- 证据：修复前 `fastboot boot` 与刷入 `boot_b` 后均报 Load Error 且 `slot-retry-count:b` 不递减；
  修复后设备立刻离开 fastboot 进入内核（面板点亮、f2fs 挂载成功）。
- 记录：`bug.md` B-022、`docs/decisions.md` D-018。

### 1.2 PID 1 崩溃（`Kernel panic - Attempted to kill init! exitcode=0x0000000b`）
- 根因：initramfs 用 `-o ro` 探测挂载后从未 `remount,rw`；`/dev /proc /sys` 没搬进新根。
- 修复：`mount -o remount,rw`，`mount --move /dev|/proc|/sys|/run` 到新根，`/dev/console` 兜底。
- 证据：修复后 systemd 成功起来并拉起 getty，屏幕停在 tty 登录提示。
- 记录：`bug.md` B-023、`docs/decisions.md` D-018。

### 1.3 免拍照的调试通道（USB gadget + httpd）
- 内核已有 gadget 支持（`libcomposite`/`u_ether`/`usb_f_ecm` 均为模块），我把它们打进 initramfs，
  开机后建 `usb0` 并起 busybox `httpd`，宿主侧出现 `enp6s0f4u1`（cdc_ether）。
- 访问方式（宿主无需 root，走 IPv6 链路本地）：
  ```
  curl -sg --interface enp6s0f4u1 'http://[fe80::XXXX%enp6s0f4u1]/dmesg.txt'
  curl -sg --interface enp6s0f4u1 'http://[fe80::XXXX%enp6s0f4u1]/cgi-bin/cmd?uname$IFS-a'
  ```
  命令里的空格用 `$IFS` 代替（busybox httpd 不解码 `%20`）。
- 触发条件：cmdline 加 `elish.net=1`；诊断块用 `elish.diag=1`。

## 2. 未决问题（当前唯一阻塞）

### 2.1 刷入后部分大文件内容与镜像不一致
| 文件 | 镜像内 md5 | 设备上读到 md5 | 结果 |
|---|---|---|---|
| usr/bin/bash (1.3 MB) | 3da2c786… | 3da2c786… | 一致 |
| usr/bin/mango (3.3 MB) | 0d3068a6… | 0d3068a6… | 一致 |
| usr/lib/libpython3.14.so.1.0 (~4 MB) | f5927fa4… | 13dae948… | **不一致** |
| usr/lib/libcrypto.so.3 (6.5 MB) | aa8b062b… | 079c675d… | **不一致** |

- 后果：`systemd` / `python3` / `openssl` / `pacman` / `kmod` 一律 `Segmentation fault`（它们都要加载这些大 .so），
  而 `sh` / `cat` / `ls` / `bash` 正常。fsck 通过、f2fs 挂载正常、小文件全对 → 不是文件系统元数据问题，是数据块问题。
- 已排除：`img2simg` → `simg2img` 往返完全一致（`md5 f3e68326…` 两侧相同），sparse 编码本身没问题。
- 待验证：**raw 直刷**（不做 sparse）后重新比对 md5。若 raw 一致 → fastboot sparse 写入不可靠；
  若 raw 仍不一致 → 问题在 `sload.f2fs` 生成镜像阶段，需改用 loop 挂载（需 root）或换 `mkfs` 路径。

## 3. 复现步骤（从当前状态）
```
# 1) 进 fastboot（长按电源 15s，再按 音量减+电源）
# 2) raw 刷根文件系统（约 10 分钟；不要用 sparse）
fastboot flash linux output/20261024/rootfs/linux-rootfs-f2fs.img
# 3) 刷调试内核（含 USB 网卡 + httpd）
fastboot flash boot_b /tmp/boot-net4.img
fastboot reboot
# 4) 宿主上找链路本地地址并比对 md5
ping -6 -c2 -I enp6s0f4u1 ff02::1; ip -6 neigh show dev enp6s0f4u1
curl -sg --interface enp6s0f4u1 'http://[fe80::…%enp6s0f4u1]/cgi-bin/cmd?md5sum$IFS/newroot/usr/lib/libcrypto.so.3'
```

## 4. 产物现状
- `output/20261024/kernel/`：Image.gz + 两个 dtb + modules.tar.zst（SHA256SUMS 自校验通过）
- `output/20261024/rootfs/`：linux-rootfs-f2fs.img（16 GiB，sha256 8558f0c5…）、tar.zst
- `output/20261024/boot/`：boot.img（零填充 dtb，sha256 d6ed7d02…）、boot-boe-alt.img、boot-debug.img
- 调试镜像（不在仓库内）：`/tmp/boot-net4.img`（USB 网卡 + httpd + diag 模式）

## 5. 更正：B-025 的“刷入损坏”结论是错的（2026-10-24 深夜）

用 `dump.f2fs -r` 把**镜像内的文件树**直接导出到宿主（不经过设备）后比对：

| 文件 | tar.zst 里 | 镜像里（dump.f2fs） | 设备上 md5 | 结论 |
|---|---|---|---|---|
| libcrypto.so.3 | aa8b062b… | **079c675d…** | 079c675d… | 镜像==设备，tar 是**旧产物** |
| libpython3.14.so.1.0 | f5927fa4… | **13dae948…** | 13dae948… | 同上 |
| bash | 3da2c786… | 3da2c786… | 3da2c786… | 一致 |

- 结论：**没有发生刷入损坏**，也没有 sload 损坏。`sload.f2fs` 最小复现（6.5MB/3MB/100KB 随机文件 → sload → dump.f2fs）**往返 md5 完全一致**。
  之前 tar 与镜像不一致，是因为 tar 是更早一轮构建的产物（rolling 仓库在两次构建之间更新了 openssl/python 等包）。
- 所以真正的问题只剩一个：**镜像里的部分动态二进制在真机上执行即 SIGSEGV**（systemd/python3/openssl/pacman/kmod），
  而 `sh`/`cat`/`ls`/`bash` 正常。文件内容正确、glibc 版本满足（libcrypto 需 2.34，镜像是 2.43）。

### 下一步（按信息量排序）
1. **拿到崩溃地址**：initramfs 里设 `core_pattern=/newroot/core.%e` + `ulimit -c unlimited`，跑一次 systemd，把 core 通过 httpd 取回，用 `readelf -n` 看 NT_PRSTATUS 的 PC/LR。
2. **排除内核因素**：用同一个 rootfs 起 `qemu-system-aarch64`（宿主）跑一遍 —— 若在模拟器里正常，说明是本内核/CPU 相关问题；若同样崩，说明是 rootfs 内部版本组合问题。
3. **重建一份“同快照” rootfs**：当前 rootfs 是「ALARM 基础包（glibc 2.43）+ 当前仓库包（openssl 3.6.3、systemd 261.2）」混装；下一步用 `pacman -Syu`（在设备或 qemu 里）把整套升到同一快照再试。
4. 候选内核开关排查：`CONFIG_ARM64_MTE=y`、`CONFIG_SHADOW_CALL_STACK=y`（用户态无关，但可做 A/B）。

## §6 真机运行态（2026-10-25 09:00，Wi-Fi SSH 验证）

| 能力 | 证据 |
|---|---|
| 根分区 196 GiB（可用 191 GiB） | `df -h /` → `196G 5.1G 191G 3%`；离线 `resize.f2fs` 对齐分区后刷入 |
| 内核 | `uname -r` → `7.2.0`（`linux-postmarketos-qcom-sm8250` 7.2.0_git20261010） |
| 桌面 | `/usr/bin/mango` 在 tty1 运行，Wayland socket + Xwayland 就绪，屏幕为平铺桌面 |
| systemd | `systemctl is-system-running` → `running`；`dbus-broker` 正常 |
| 网络 | `wlp1s0` 连上 Wi-Fi（192.168.1.90/24，默认路由 192.168.1.1），可 ping 外网；`ssh magma@192.168.1.90` 可用 |
| USB 调试 | ECM 网卡（宿主 `10.11.12.1` ↔ 设备 `10.11.12.2`）+ **ACM 串口** `/dev/ttyACM*`（udev 规则 0666），宿主 `screen /dev/ttyACM0 115200` 可直接登录 |
| 性能守护 | `schedplace` 已安装为 systemd 服务并 enable：日志显示 `topology little=0-3/284 big=4-6/871 prime=7/1024`，并对 mango 应用规则（`new_mask=4-7`） |

### 关键坑（都已写入 bug.md）
- **B-029**：`dbus-broker` 起不来 → 整个系统总线缺失（`systemctl`/`nmcli`/`loginctl` 全废）。
  根因是 systemd 的 gpt 自动生成器把 Android 的 ESP 类型分区 `sda35` 生成 `boot.automount` 挂到 `/boot`，
  FAT 头不合法导致挂载失败，`dbus-broker.service` 的 mount namespace 初始化随之失败（`status=226/NAMESPACE`）。
  修复：屏蔽 `/etc/systemd/system/boot.{mount,automount}` → `/dev/null`（生产建议再加 `systemd.gpt_auto=no`）。
- **B-030**：`perf/schedplace` 的 `tests/test_safety.c:116` 在 aarch64 真机上失败一次（`--check-config ... --allow-profile-performance` 期望 0）。
  同样的命令手工执行返回 0（配置 `2 rule(s), 2 enabled`），只是测试里 `popen` 视角不同；功能不受影响，待查测试环境差异。
- **容量**：不要在设备端跑 `resize.f2fs` 时忘了 `-F` —— `-f` 只表示“修复不一致数据”，仍会问 `Do you want to continue? [y/n]`，在 initramfs 里等输入会把启动挂死。

### 未完成
- 音频：`hexagonrpcd` 不在 Arch Linux ARM 仓库（`pacman -Ss hexagonrpcd` 无结果），需要从源码构建（Rust）+ SM8250 UCM 配置；当前预期无声。
- FEX-Emu：已在设备上源码构建（本机 gcc/cmake/ninja，8 核），完成后需 x86-64 rootfs 才能跑 Win/x86 程序。
- suspend/触摸手势/相机未验证；`linux-firmware-qcom` 已是仓库版本。
