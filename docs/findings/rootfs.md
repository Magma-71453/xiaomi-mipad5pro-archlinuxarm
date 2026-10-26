# rootfs 模块发现记录（xiaomi-elish / Arch Linux ARM aarch64）

本文件由 rootfs 模块产出，供 dispatcher 合并进 `bug.md`。所有数字均为本机实测。
构建脚本：`scripts/build-rootfs.sh`；配置：`configs/rootfs/**`；产物：`output/20261024/rootfs/`。

## 1. 结论摘要

* 用 ALARM 官方根包 + 官方仓库包**离线**组装出可直接作为 linux 分区根文件系统的树，
  固化为 (a) `tar.zst` 归档 (b) **f2fs raw 镜像**（主产物，4096 字节块）；
  另可用 `--also-ext4` 生成 ext4 镜像（次要产物）。
* 全程无 root：解包用 `bsdtar`，整轮构建在 `fakeroot` 下执行（使 mke2fs/sload/bsdtar
  看到 uid/gid = 0），aarch64 二进制通过 x86_64 的 `qemu-aarch64-static` 以用户态执行。
* 追加 185 个 ALARM 官方包（含 wlroots0.20 0.20.2-1、scenefx 0.5-1，符合裁决 D-013 路线），
  依赖闭包由脚本内置的离线解析器从 core/extra/alarm 三个包数据库算出。
* f2fs 镜像的 sector-size 字段有一个**必须记录的与直觉不同的事实**（见 §7.3）：
  `mkfs.f2fs 1.17.0` 对**普通文件**镜像忽略 `-w`，并且 `fsck.f2fs`/`dump.f2fs` 每次打开
  都会按宿主设备重新写回该字段。因此“让 superblock 记录 4096 字节扇区”只能用最后一步
  定点改写实现；该改写只变 4 个字节，且内核侧的两个校验均与设备逻辑扇区无关。

## 2. 输入来源（实测）

| 输入 | URL | 大小 | 耗时/备注 |
|---|---|---|---|
| ALARM aarch64 根包 | http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz → 302 → http://ca.us.mirror.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz | 829,367,415 B | 137 s，平均 6.06 MB/s，exit 0 |
| ALARM 包数据库 core | http://mirror.archlinuxarm.org/aarch64/core/core.db.tar.gz | 256,711 B | 3.7 s |
| ALARM 包数据库 extra | .../extra/extra.db.tar.gz | 10,614,719 B | 6.5 s |
| ALARM 包数据库 alarm | .../alarm/alarm.db.tar.gz | 62,203 B | 2.8 s |
| qemu-user-static（x86_64 宿主工具） | https://geo.mirror.pkgbuild.com/extra/os/x86_64/qemu-user-static-11.1.2-1-x86_64.pkg.tar.zst | 13,192,422 B | 包内 `usr/bin/qemu-aarch64-static` 7,271,256 B，`qemu-aarch64 version 11.1.2` |
| f2fs-tools（x86_64 宿主工具） | https://geo.mirror.pkgbuild.com/extra/os/x86_64/f2fs-tools-1.17.0-1-x86_64.pkg.tar.zst | 282,422 B | `mkfs.f2fs 1.17.0 (2026-09-24)`，含 mkfs/fsck/dump/sload/f2fslabel/defrag/resize/… |
| linux-firmware-qcom（ALARM core，GPU/DSP 固件） | http://mirror.archlinuxarm.org/aarch64/core/linux-firmware-qcom-20260916-1-any.pkg.tar.xz | 172,772,972 B | 43.7 s；解包 562,614,193 B / 491 成员 |
| elish 固件（可选，见 §10） | https://github.com/lujianhua/xiaomi-elish-firmware/archive/51e9ac8cd91d88de43fb016530b9421a2713467a.tar.gz | 27,263,264 B | 解包 62 MB，按 pmaports `firmware-xiaomi-elish` 的 APKBUILD 布局整理 |

宿主工具版本：e2fsprogs 1.47.4、kmod 34.2、bsdtar 3.8.10（libarchive 3.8.10）、zstd 1.5.7。

## 3. 追加软件包清单（185 个，全部来自 ALARM 官方仓库，逐包来源+版本）

解析方式：脚本内嵌的离线依赖解析器读取 `core/extra/alarm` 的 `desc`+`depends` 元数据，
对 configs/rootfs/packages.list 求解依赖闭包，跳过底包里已有的 165 个包，
并对 `opengl-driver` 等虚名做偏好（`mesa`/`libglvnd`），显式拒绝 nvidia-*/vulkan-vendor 驱动包。
每个包下载后按数据库中的 `SHA256SUM` 校验通过。

关键版本（其余见下表）：

| 包 | 版本 | 仓库 |
|---|---|---|
| mesa | 1:26.2.5-1 | extra |
| wayland | 1.26.0-1 | extra |
| wayland-protocols | 1.49-1 | extra |
| wlroots0.20 | 0.20.2-1 | extra |
| scenefx | 0.5-1 | extra |
| seatd | 0.9.3-1 | extra |
| libinput | 1.32.0-1 | extra |
| libdrm | 2.4.134-1 | extra |
| pixman | 0.46.4-1.1 | extra |
| libxkbcommon | 1.13.2-1 | extra |
| xorg-xwayland | 24.1.14-1 | extra |
| networkmanager | 1.58.1-2 | extra |
| bluez / bluez-utils | 5.87-2 | extra |
| pipewire / pipewire-pulse / pipewire-alsa | 1:1.6.9-1 | extra |
| wireplumber | 0.5.18-1 | extra |
| alsa-ucm-conf / alsa-utils | 1.2.16.1-1 / 1.2.16-1 | extra |
| polkit | 128-1 | extra |
| sudo | 1.9.17.p2-6 | core |
| git | 2.56.0-1 | extra |
| base-devel | 1-2 | core |
| meson / cmake / ninja | 1.12.1-2 / 4.4.4-1 / 1.13.2-3 | extra |
| f2fs-tools | 1.17.0-1 | core |

完整清单（`name version repo`）：

| 包 | 版本 | 仓库 |
|---|---|---|
| abseil-cpp | 20260817.0-2 | extra |
| alsa-card-profiles | 1:1.6.9-1 | extra |
| alsa-lib | 1.2.16.1-1 | extra |
| alsa-topology-conf | 1.2.5.1-4 | extra |
| alsa-ucm-conf | 1.2.16.1-1 | extra |
| alsa-utils | 1.2.16-1 | extra |
| autoconf | 2.73-1 | core |
| automake | 1.19-1 | core |
| avahi | 1:0.9rc5-2 | extra |
| base-devel | 1-2 | core |
| bison | 3.8.2-8 | core |
| bluez | 5.87-2 | extra |
| bluez-libs | 5.87-2 | extra |
| bluez-utils | 5.87-2 | extra |
| boost-libs | 1.92.0-1 | extra |
| cairo | 1.18.6-1 | extra |
| cjson | 1.7.19-1 | extra |
| cmake | 4.4.4-1 | extra |
| cpio | 2.15-3 | extra |
| cppdap | 1.58.0-3 | extra |
| db5.3 | 5.3.28-7 | core |
| dconf | 51.0-1 | extra |
| debugedit | 5.3-2 | core |
| default-cursors | 3-1 | extra |
| duktape | 2.7.0-7 | extra |
| elfutils | 0.197-1 | core |
| f2fs-tools | 1.17.0-1 | extra |
| fakeroot | 1:1.37.2-3 | core |
| fftw | 3.3.11-1 | extra |
| flac | 1.5.0-1 | extra |
| flex | 2.6.4-6 | core |
| fontconfig | 2:2.18.3-2 | extra |
| freetype2 | 2.15.0-1 | extra |
| fribidi | 1.0.17-1 | extra |
| gc | 8.2.12-1 | core |
| gcc | 16.1.1+r12+g301eb08fa2c5-1 | core |
| gdb | 18.1-1 | extra |
| gdb-common | 18.1-1 | extra |
| git | 2.56.0-1 | extra |
| graphite | 1:1.3.15-1 | extra |
| groff | 1.24.2-1 | core |
| gtest | 1.18.0-1 | extra |
| guile | 3.0.11-1 | core |
| harfbuzz | 14.6.0-1 | extra |
| hicolor-icon-theme | 0.18-1 | extra |
| htop | 3.5.3-1 | extra |
| iw | 6.17-1 | core |
| jbigkit | 2.2-1 | extra |
| jsoncpp | 1.9.8-1 | extra |
| lame | 4.0-1 | extra |
| lcms2 | 2.19.1-1 | extra |
| libasyncns | 1:0.8+r3+g68cd5af-3 | extra |
| libdaemon | 0.14-6 | extra |
| libdatrie | 0.2.14-1 | extra |
| libdecor | 0.2.5-1 | extra |
| libdeflate | 1.26-1 | extra |
| libdisplay-info | 0.3.0-1 | extra |
| libdrm | 2.4.134-1 | extra |
| libebur128 | 1.2.6-2 | extra |
| libei | 1.6.0-2 | extra |
| libepoxy | 1.5.10-3 | extra |
| libevdev | 1.14.0-1 | extra |
| libfdk-aac | 3.0.0-1 | extra |
| libfontenc | 1.1.9-1 | extra |
| libfreeaptx | 0.2.2-1 | extra |
| libglvnd | 1.7.0-3 | extra |
| libgudev | 238-4 | extra |
| libinput | 1.32.0-1 | extra |
| libisl | 0.28-1 | core |
| libjpeg-turbo | 3.2.0-2 | extra |
| liblc3 | 1.1.3-2 | extra |
| libldac | 2.0.2.6-1 | extra |
| libliftoff | 0.5.0-1 | extra |
| libmm-glib | 1.24.2-2 | extra |
| libmpc | 1.4.1-1 | core |
| libmysofa | 1.3.5-1 | extra |
| libndp | 1.9-1 | extra |
| libnewt | 0.52.25-2 | extra |
| libnm | 1.58.1-2 | extra |
| libogg | 1.3.6-1 | extra |
| libpciaccess | 0.19-1 | extra |
| libpgm | 5.3.128-4 | extra |
| libpipewire | 1:1.6.9-1 | extra |
| libpng | 1.6.59-1 | extra |
| libpulse | 17.0+r134+g77d25a1e6-1 | extra |
| libsamplerate | 0.2.2-3 | extra |
| libsndfile | 1.2.2-4 | extra |
| libsodium | 1.0.22-1 | extra |
| libteam | 1.32-3 | extra |
| libthai | 0.1.30-1 | extra |
| libtiff | 4.7.2-2 | extra |
| libtool | 2.6.0+r23+gb08cb0a0-1 | core |
| libuv | 1.52.1-2 | extra |
| libvorbis | 1.3.7-4 | extra |
| libwacom | 2.20.0-1 | extra |
| libwebp | 1.6.0-2 | extra |
| libwireplumber | 0.5.18-1 | extra |
| libx11 | 1.8.14-1 | extra |
| libxau | 1.0.12-1 | extra |
| libxcb | 1.17.0-1 | extra |
| libxcvt | 0.1.3-1 | extra |
| libxdmcp | 1.1.5-2 | extra |
| libxext | 1.3.7-1 | extra |
| libxfont2 | 2.0.9-1 | extra |
| libxft | 2.3.9-1 | extra |
| libxkbcommon | 1.13.2-1 | extra |
| libxkbfile | 1.2.0-1 | extra |
| libxrandr | 1.5.5-1 | extra |
| libxrender | 0.9.13-1 | extra |
| libxshmfence | 1.3.3-1 | extra |
| libxxf86vm | 1.1.7-1 | extra |
| lilv | 0.28.0-1 | extra |
| llvm-libs | 23.1.3-4.1 | extra |
| lm_sensors | 1:3.6.2-1 | extra |
| lua | 5.5.1-1 | extra |
| lua54 | 5.4.9-1 | extra |
| lv2 | 1.18.10-2 | extra |
| lzo | 2.10-5 | core |
| m4 | 1.4.21-2 | core |
| make | 4.4.1-3 | core |
| mesa | 1:26.2.5-1 | extra |
| meson | 1.12.1-2 | extra |
| mobile-broadband-provider-info | 20251101-1 | extra |
| mpdecimal | 4.0.1-3 | core |
| mpg123 | 1.33.7-1 | extra |
| mtdev | 1.1.7-1 | extra |
| networkmanager | 1.58.1-2 | extra |
| ninja | 1.13.2-3 | extra |
| nspr | 4.40-1 | core |
| nss | 3.131.1-1 | core |
| opus | 1.6.1-1 | extra |
| pango | 1:1.58.2-1 | extra |
| patch | 2.8-1 | core |
| pcre | 8.45-5 | core |
| pcsclite | 2.5.3-1 | extra |
| perl | 5.44.0-1 | core |
| perl-error | 0.17030-3 | extra |
| perl-mailtools | 2.22-3 | extra |
| perl-timedate | 2.35-1 | extra |
| pipewire | 1:1.6.9-1 | extra |
| pipewire-alsa | 1:1.6.9-1 | extra |
| pipewire-audio | 1:1.6.9-1 | extra |
| pipewire-pulse | 1:1.6.9-1 | extra |
| pipewire-session-manager | 1:1.6.9-1 | extra |
| pixman | 0.46.4-1.1 | extra |
| pkgconf | 3.0.10-1 | core |
| polkit | 128-1 | extra |
| python | 3.14.8-1 | core |
| python-tqdm | 4.70.1-1 | extra |
| rhash | 1.4.6-1 | extra |
| sbc | 2.2-1 | extra |
| scenefx | 0.5-1 | extra |
| seatd | 0.9.3-1 | extra |
| serd | 0.32.10-1 | extra |
| slang | 2.3.3-4 | extra |
| sord | 0.16.22-1 | extra |
| source-highlight | 3.1.9-19 | extra |
| spandsp | 0.0.6-7 | extra |
| spirv-tools | 1:1.4.363.0-1 | extra |
| sratom | 0.6.22-1 | extra |
| sudo | 1.9.17.p2-6 | core |
| texinfo | 7.3-3 | core |
| vulkan-icd-loader | 1.4.363.0-1 | extra |
| wayland | 1.26.0-1 | extra |
| wayland-protocols | 1.49-1 | extra |
| webrtc-audio-processing-1 | 1.3-6 | extra |
| wireplumber | 0.5.18-1 | extra |
| wlroots0.20 | 0.20.2-1 | extra |
| wpa_supplicant | 2:2.12-1 | core |
| xcb-proto | 1.17.0-4 | extra |
| xcb-util-errors | 1.0.1-2 | extra |
| xcb-util-renderutil | 0.3.10-2 | extra |
| xcb-util-wm | 0.4.2-2 | extra |
| xkeyboard-config | 2.48-1 | extra |
| xorg-fonts-encodings | 1.1.0-2 | extra |
| xorg-server-common | 21.1.25-2 | extra |
| xorg-setxkbmap | 1.3.5-1 | extra |
| xorg-xkbcomp | 1.5.0-1 | extra |
| xorg-xwayland | 24.1.14-1 | extra |
| xorgproto | 2026.1-1 | extra |
| xxhash | 0.8.4-1 | extra |
| zeromq | 4.3.5-3 | extra |
| zix | 0.8.2-1 | extra |
| zlib-ng | 2.3.3-1 | extra |

共 185 个包，全部来自 ALARM 官方 core/extra/alarm 仓库（按数据库 SHA256SUM 校验）。

## 4. aarch64 执行环境（qemu-user-static）

宿主无 root、无 binfmt，因此 aarch64 二进制一律**显式指定解释器**执行：

```
# 重建动态链接缓存
qemu-aarch64-static -L <rootfs> <rootfs>/usr/bin/ldconfig -r <rootfs>
# 编译 locale（glibc localedef 自带 --prefix，用于把输出写进 rootfs）
qemu-aarch64-static -L <rootfs> <rootfs>/usr/bin/localedef --prefix=<rootfs> -i en_US -f UTF-8 en_US.UTF-8
qemu-aarch64-static -L <rootfs> <rootfs>/usr/bin/localedef --prefix=<rootfs> -i zh_CN -f UTF-8 zh_CN.UTF-8
# 校验 sudoers 语法（aarch64 visudo）
qemu-aarch64-static -L <rootfs> <rootfs>/usr/bin/visudo -c -f <rootfs>/etc/sudoers
```

实测输出摘要：

* `ldconfig`：exit 0，生成 `/etc/ld.so.cache`（关键行：`ld.so.cache: <size> bytes`）。
  唯一噪声：`无法对 /usr/lib64 进行 stat 操作`（ALARM aarch64 无 /usr/lib64，无害）。
* `localedef`：en_US.UTF-8 约 19 s、zh_CN.UTF-8 约 20 s；产出
  `/usr/lib/locale/locale-archive`；`--list-archive` 可列出已编译 locale。
  这比“只写 /etc/locale.conf”更强：locale 数据真实存在，但**Arch 的 `locale-gen`
  脚本本身未被逐字执行**（它用绝对路径 `/usr/lib/locale/`，无 root 无法 chroot），
  等价效果由上述 `localedef --prefix` 达成，已在 §12 记录。
* `visudo -c`：`/etc/sudoers：解析正确`、`/etc/sudoers.d/10-wheel：解析正确`。
  噪声行：`visudo: /etc/sudo.conf is owned by uid 1000, should be 0` —— 因为
  qemu-user 使用裸 syscall，绕过 fakeroot 的 LD_PRELOAD，aarch64 进程看到真实 uid 1000；
  不影响解析结果（详见 §6 的 uid 说明）。

## 5. 离线定制结果（全部通过写文件完成，脚本幂等）

| 项 | 落地内容 |
|---|---|
| /etc/fstab | `PARTLABEL=linux / f2fs rw,relatime,background_gc=on,inline_xattr,inline_dentry,nodiscard,errors=remount-ro 0 1` + proc/sysfs/devpts/tmpfs（依据见 §8） |
| /etc/hostname | `elish` |
| /etc/hosts | `127.0.0.1 localhost elish`、`::1 localhost elish` |
| 用户 | 删除底包自带的 `alarm` 用户/组（uid/gid 1000），新建 `magma` uid=1000 gid=1000、`/bin/bash`、`/home/magma`（0755，owner magma），加入 wheel,audio,video,input,render,kvm,lp,optical,rfkill,storage,network,power,games,users 中**实际存在**的组；`alarm` 也从所有组成员表中剔除 |
| root | `/etc/shadow` 中 root 口令字段前缀 `!`（锁定，保留可解锁的哈希） |
| magma 口令 | 使用 `openssl passwd -6 -salt elishmagma magma` 生成 SHA-512 crypt（`$6$`），**默认口令 magma**，首次登录后必须修改；哈希/方法记录在此以便审计 |
| sudoers | `/etc/sudoers` 中 `%wheel ALL=(ALL:ALL) ALL` 由注释改为生效（文件 0440），另加 drop-in `/etc/sudoers.d/10-wheel`（0440）内容相同 |
| locale | `/etc/locale.gen` 取消注释 `en_US.UTF-8 UTF-8`、`zh_CN.UTF-8 UTF-8` 并用 localedef 真实编译；`/etc/locale.conf`：`LANG=en_US.UTF-8` |
| 时区 | `/etc/localtime -> ../usr/share/zoneinfo/Asia/Shanghai`，并写 `/etc/timezone` |
| DNS | `/etc/resolv.conf` 由符号链接改为静态文件（223.5.5.5 / 1.1.1.1），运行期由 NetworkManager 接管 |
| systemd 启用 | `NetworkManager.service`、`systemd-timesyncd.service`、`bluetooth.service`、`seatd.service`、`fstrim.timer`、`systemd-oomd.service`；链接全部按单元文件 `[Install]` 段（WantedBy/RequiredBy/Alias）生成**相对符号链接**，目标路径均在 rootfs 内 |
| 网络栈选择 | 选 **NetworkManager** 而非 systemd-networkd：设备联网是 ath11k 802.11ax + 蓝牙，NM 自带 nmcli/配置文件/polkit 集成与默认路由处理；networkd 需要手写 .network 且另起 wpa_supplicant 单元。脚本同时**显式删除** systemd-networkd 的 wants 链接以避免两套栈并存 |
| 设备相关文件 | `/usr/lib/udev/rules.d/81-libssc-xiaomi-elish.rules`、`/usr/share/alsa/ucm2/Xiaomi/elish/{elish.conf,HiFi.conf}` + `conf.d/sm8250/Xiaomi Mi Pad 5 Pro.conf` 符号链接、`/usr/share/hexagonrpcd/hexagonrpcd-sdsp.conf`（取自 pmaports `device/testing/device-xiaomi-elish` 原文） |
| /boot | 底包内的 `Image`(44.1 MB)、`Image.gz`(16.9 MB)、`initramfs-linux.img`(15.4 MB) 与 `dtbs/`(193 MB) 全部删除，只保留空 `/boot`：内核与 initramfs 走 **boot_b**，本 rootfs 不含任何内核 |

## 6. 未执行的 `.INSTALL` 脚本清单与影响

`bsdtar` 直接解包不会执行包内 `.INSTALL`。脚本解包时检出并记录（`build.log` + BUILD-REPORT）：

| 包 | 版本 | 仓库 | 未执行该脚本的影响 |
|---|---|---|---|
| alsa-lib | 1.2.16.1-1 | extra | 只跑 ldconfig；本模块已用 qemu 重建 /etc/ld.so.cache |
| dconf | 51.0-1 | extra | 编译 dconf/GSettings 缓存；非启动必需 |
| fontconfig | 2:2.18.3-2 | extra | 跑 fc-cache -s；缺失时应用首次启动自建字体缓存 |
| git | 2.56.0-1 | extra | 调整 git-core templates 权限；不影响使用 |
| pipewire | 1:1.6.9-1 | extra | systemd-sysusers 建系统用户；开机 systemd-sysusers.service 自动执行 |
| pipewire-pulse | 1:1.6.9-1 | extra | 同上（sysusers/tmpfiles），开机自动补齐 |
| polkit | 128-1 | extra | sysusers 建 polkitd；开机自动执行 |
| sudo | 1.9.17.p2-6 | core | 安装期 sudoers 检查；本模块自行写入并置 0440，visudo -c 已通过 |
| wireplumber | 0.5.18-1 | extra | sysusers/tmpfiles 相关，开机自动补齐 |

影响评估（逐条）：

* **systemd 相关的 user/tmpfiles 不需要手动跑**：systemd 在开机时由
  `systemd-sysusers.service` 处理 `/usr/lib/sysusers.d/*.conf`、由
  `systemd-tmpfiles-setup.service` 处理 `tmpfiles.d`，因此 polkitd 等系统用户在
  首启时会自动建立。
* **ldconfig**：由本模块用 qemu 显式重建（§4），不依赖 glibc 的 `.INSTALL`。
* **ca-certificates**：底包已自带 `/etc/ssl/certs/ca-certificates.crt`（121 张根证书，
  指向 `/etc/ca-certificates/extracted/tls-ca-bundle.pem`），无需 update-ca-trust。
* **glib 的 schema 编译（glib-compile-schemas）未执行**：安装的 GUI 依赖里若使用 GSettings，
  首次运行可能提示 schema 缺失；需要时在设备上以 root 执行 `glib-compile-schemas /usr/share/glib-2.0/schemas`。
* **file capabilities 丢失**（与 .INSTALL 无关但同类影响）：底包中只有
  `/usr/bin/newuidmap`、`/usr/bin/newgidmap` 两个文件带 `security.capability` xattr；
  非 root 无法写入 capability，故解包时用 `--no-xattrs --no-acls` 跳过（否则 bsdtar 退出码非 0）。
  影响：`newuidmap/newgidmap` 无 rootless 容器所需的 cap；补救：
  `setcap cap_setuid+ep /usr/bin/newuidmap` 等（首启 root shell 里执行）。
* **pacman 数据库不含追加包**：追加包是 bsdtar 解包，未写入 `/var/lib/pacman/local`，
  因此 `pacman -Q` 不会列出它们，后续 `pacman -Syu` 需要注意文件归属冲突。
  为此在 rootfs 内写了 `/etc/elish-packages.txt`（name version repo 全量清单），
  并把“用本地仓库 + pacman -U 重做”列为遗留项（§12）。

### 6.1 非 root 解包会丢 suid/sgid，本模块的修复（重要，容易漏）

实测：普通用户用 `bsdtar -x` 解包 ALARM 根包后 `find <rootfs> -perm -4000` = 0，
而根包里带 suid 的条目有 15 个以上（sudo/passwd/mount/su/pkexec/chsh/chfn/chage/newgrp/gpasswd/umount/ksu）。
即 libarchive 在非 root 下丢弃 setuid/setgid 位（解包后 passwd 是 0755 而不是 4755）；不处理就等于交付一个
sudo/passwd/mount 全部不可用的 rootfs。

修复（脚本 collect_special_modes + apply_special_modes）：用 python tarfile 遍历根包 + 185 个包的原始上包头，
抽出所有 mode & 0o6000 的条目写成 mode+path 清单（本次 20 条），再逐条 chmod 回真实 inode。结果：
树内 suid 15 / sgid 6，tar 内同样 15 / 6。清单：
4755 chfn chage mount newgrp chsh passwd ksu gpasswd umount su sudo pkexec、6755 unix_chkpwd、
4110 usr/lib/dbus-daemon-launch-helper、4711 usr/lib/ssh/ssh-keysign、2755 write wall var/log/journal、
2775 var/lib/libuuid var/lib/tpm2-tss/system/keystore。

另有两个路径必须用 make_tree_readable 处理，否则 bsdtar/sload 直接报权限错退出：

* /var/lib/tpm2-tss/system/keystore：包头 mode 2775 但附带 ACL（ls 显示 d---------+），libarchive 按 ACL
  落成 0000，之后归档/镜像工具无法读取（原始报错：`bsdtar: ./var/lib/tpm2-tss/system/keystore: Couldn't visit directory: 权限不够`）。
  ACL 无法以非 root 还原（本模块用 --no-acls），最终该目录 2775 无 ACL；tpm2-tss 的 tmpfiles.d 会在首启重建它。
* /usr/lib/dbus-daemon-launch-helper：包头 4110（owner 无读位），归档器读不到内容，故补 u+r → 4510。

结论：任何非 root 组装 rootfs 的流程都必须做 suid/sgid 回填。
## 7. f2fs 镜像制作（主产物）

### 7.1 `mkfs.f2fs -h` 实测原文（参数只能由此推导）

```
Usage: mkfs.f2fs [options] device [sectors]
[options]:
  -b filesystem block size [default:4096]
  -c [device_name[@alias_filename]] up to 7 additional devices, except meta device
  -d debug level [default:0]
  -e [cold file ext list] e.g. "mp3,gif,mov"
  -E [hot file ext list] e.g. "db"
  -f force overwrite of the existing filesystem
  -g add default options
  -H support write hint
  -i extended node bitmap, node ratio is 20% by default
  -l label
  -U uuid
  -m support zoned block device [default:0]
  -o overprovision percentage [default:auto]
  -O feature1[,feature2,...] e.g. "encrypt"
  -C [encoding[:flag1,...]] Support casefolding with optional flags
  -q quiet mode
  -r set checkpointing seed (srand()) to 0
  -R root_owner [default: 0:0]
  -s # of segments per section [default:1]
  -S sparse mode
  -t 0: nodiscard, 1: discard [default:1]
  -T timestamps
  -w wanted sector size
  -z # of sections per zone [default:1]
  -V print the version number and exit
  -Z # of reserved sections [default:auto]
sectors: number of sectors [default: determined by device size]
```

man 8 mkfs.f2fs 对两个易混选项的原文：

* `-b block size`：`Currently, the kernel is only able to mount f2fs filesystems where the
  block size matches the page size. The default value is 4096.`
* `-w wanted-sector-size`：`Specify the sector size in bytes. Without it, the sectors will be
  calculated by device sector size.`
* `-s #-of-segments-per-section`：`Specify the number of segments per section. A section
  consists of multiple consecutive segments, and is the unit of garbage collection.`

### 7.2 参数与取舍（最终命令）

```bash
truncate -s 17179869184 linux-rootfs-f2fs.img          # 16 GiB（--size 可覆盖）
mkfs.f2fs -f -l linux -b 4096 -w 4096 -R 0:0 \
          -O extra_attr,inode_checksum,flexible_inline_xattr linux-rootfs-f2fs.img
sload.f2fs -f <rootfs 目录> -P linux-rootfs-f2fs.img
fsck.f2fs -f linux-rootfs-f2fs.img                     # 结构校验
```

| 参数 | 含义（以 -h/man 为准） | 为什么这样选 |
|---|---|---|
| `-f` | 已存在文件系统也强制覆盖 | 镜像文件可能残留上一次构建 |
| `-l linux` | 卷标 | 与 `PARTLABEL=linux` 呼应，便于识别；`fsck.f2fs -l` 可读回 |
| `-b 4096` | 文件系统块大小 | man：内核只挂载“块大小 = 页大小”的 f2fs；设备页 4096，故必须 4096（也是 f2fs 默认） |
| `-w 4096` | 期望的**扇区**大小 | 设备 UFS 逻辑扇区 4096（deviceinfo_rootfs_image_sector_size=4096）。注意：对**普通文件**镜像该值不生效，见 §7.3 |
| `-R 0:0` | 根目录属主 | man：`-R` 缺省取 getuid()/getgid()；显式 0:0 保证 rootfs 根目录属主为 root |
| `-O extra_attr,inode_checksum,flexible_inline_xattr` | 启用特性 | 只用 7.2 内核无条件支持的三个特性（`fs/f2fs/f2fs.h: F2FS_FEATURE_FUNCS(extra_attr/inode_chksum/flexible_inline_xattr)`）。**不启用** encrypt/verity/casefold/compression：内核 .config 无 `CONFIG_F2FS_FS_ENCRYPTION`、`CONFIG_FS_VERITY`、`CONFIG_UNICODE`、`CONFIG_F2FS_FS_COMPRESSION`，启用会直接无法挂载 |
| `-t`（未显式给出） | 默认 1 = mkfs 时对设备发 discard | 对 image 文件无害，保留默认 |
| `-S`、`-s`、`-z`、`-Z`、`-m` | 稀疏模式 / 每 section 段数 / zoned | **不使用**：`-s` 是“每 section 的段数”，不是扇区大小（见 §7.3）；UFS 非 zoned 设备 |


本次构建实际输出（完整见 output/20261024/rootfs/BUILD-REPORT.txt）：

```
Info: Segments per section = 1
Info: Sections per zone = 1
Info: sector size = 512            <-- -w 4096 对文件镜像无效，见 7.3
Info: total sectors = 33554432 (16384 MB)
Info: block size = 4096
Info: Overprovision ratio = 1.150%
Info: Overprovision segments = 99 (GC reserved = 93)
Info: format successful
```

sload.f2fs 需要单独记录：它在结束时跑内部一致性检查并打印
  `[FSCK] valid_block_count matching with CP [Fail] [0xd6004, 0xd51c1]`，随后 fix_checkpoint() 修正并以非零状态退出。
紧接着的独立 fsck.f2fs -f 全部 [Ok..]、exit 0，因此脚本把 sload 的退出码降级为 WARN，以 fsck.f2fs -f 作为唯一验收门。
可直接复制到刷机指南的命令（宿主，无需 root，工具在 `../.build/tools/f2fs/usr/bin`）：

```bash
IMG=output/20261024/rootfs/linux-rootfs-f2fs.img
truncate -s 16G "$IMG"
LD_LIBRARY_PATH=../.build/tools/f2fs/usr/lib ../.build/tools/f2fs/usr/bin/mkfs.f2fs \
    -f -l linux -b 4096 -w 4096 -R 0:0 -O extra_attr,inode_checksum,flexible_inline_xattr "$IMG"
LD_LIBRARY_PATH=../.build/tools/f2fs/usr/lib ../.build/tools/f2fs/usr/bin/sload.f2fs -f <rootfs 目录> -P "$IMG"
LD_LIBRARY_PATH=../.build/tools/f2fs/usr/lib ../.build/tools/f2fs/usr/bin/fsck.f2fs -f "$IMG"
fastboot flash linux "$IMG"      # 只允许 boot_b 与 linux 两个分区
```

### 7.3 关于 “-s 12 / log sectorsize 4096” 的实测结论（重要，与裁决前的假设不同）

1. **`-s` 不是扇区大小**：`-h` 明确写 `-s # of segments per section [default:1]`；
   扇区大小对应的是 `-w wanted sector size`（man 8 亦同）。因此 `-s 12` 会把
   “每 section 12 个段(24 MiB)”写进布局，**不会**改变扇区大小，反而影响 GC/SSA 布局；
   本模块**不采用** `-s 12`。
2. **`-w 4096` 对普通文件镜像无效**：实测 `mkfs.f2fs -f -b 4096 -w 4096` 的输出仍是
   `Info: sector size = 512`，`fsck.f2fs -l` 读回 `log_sectorsize: 9 / log_sectors_per_block: 3`。
   原因：工具只能通过 `BLKSSZGET` 问块设备要扇区大小，普通文件查询失败即回落 512。
   对照实验：`-g android`（内部含 `-w 4096`）在文件上同样是 512，而且会打开
   encrypt/verity 等我们内核挂不上的特性，所以也不能用。
3. **任何 f2fs 工具都会重写该字段**：把 superblock 的 `log_sectorsize` 手工改成 12 后，
   `fsck.f2fs -l` 一打开就按宿主设备改回 9（实测 od 前 12/0、后 9/3）；
   `dump.f2fs` 同样打印 `[update_superblock: 943] Info: Done to update superblock`。
   所以在“普通文件镜像”这条路上，**只有最后一步定点改写**才能让镜像带着 4096 扇区字段。
4. **内核侧并不要求该字段等于设备逻辑扇区**：`fs/f2fs/super.c` 对 `log_sectorsize` 只有两处校验
   （`F2FS_MIN_LOG_SECTOR_SIZE=9` ≤ 值 ≤ `F2FS_MAX_LOG_SECTOR_SIZE=PAGE_SHIFT=12`，且
   `log_sectorsize + log_sectors_per_block == PAGE_SHIFT`），全树 grep 无
   “与 bdev_logical_block_size 比较”的检查；512 与 4096 两种取值都合法（9+3=12、12+0=12）。
5. 因此本模块的处理是：**工具产出可校验的镜像（`log_sectorsize=9`）→ `fsck.f2fs -f` 通过 →
   最后一步把两个冗余 superblock 的 2 个字改写为 12/0**（`--f2fs-sector-size 4096`，默认），
   并用 `cmp -l` 证明交付镜像与“fsck 校验过的镜像”**只差 4 个字节**。
   交付镜像再跑任何 f2fs 工具都会把这个字段按宿主设备改回 9 —— 在真机（4096 扇区）上
   开机 fsck 时它会稳定为 12，方向一致。
6. **回退路径**：若真机挂载报 sector size 不匹配（预期不会，见第 4 点），
   用 `scripts/build-rootfs.sh --f2fs-sector-size 512` 重制即可得到“工具原生、可被 fsck 复核”
   的镜像（`log_sectorsize=9`），其它字节完全不变。

### 7.4 首启扩容：resize.f2fs 的正确用法

实测证据：resize.f2fs 内含 `f2fs_devs_are_umounted` 检查，已挂载设备直接报
`Error: Not available on mounted device!`；man 8 resize.f2fs 写明
`-t target sectors   Specify the size in sectors`、
`-t ... [default: device size]`、
`-F  Skip caution dialogue and resize partition directly`、
`Current version only supports expanding the prebuilt filesystem.`。
（内核虽有 F2FS_IOC_RESIZE_FS ioctl：fs/f2fs/file.c:3675、gc.c:2318，但该工具仍要求未挂载。）

镜像 16 GiB，分区 /dev/sda36 = 205,039,616 KiB（≈195.5 GiB）。推荐流程（initramfs shell 或 rescue 环境，分区未挂载）：

```bash
resize.f2fs /dev/sda36                      # -t 缺省 = 扩到设备末尾
resize.f2fs -F /dev/sda36                   # 跳过确认对话
# 需要显式目标扇区时（f2fs sector_size = 4096B）：
resize.f2fs -t $(( $(blockdev --getsz /dev/sda36) / 8 )) /dev/sda36
```

只在扩展方向支持（缩容才需要 -s）。已挂载根上执行会被拒绝，所以刷机指南必须把这一步放在
“首次启动前 / initramfs 阶段”。rootfs 内已预装 f2fs-tools 1.17.0-1，可直接调用。

### 7.5 --f2fs-sector-size 512 回退路径的实测验证（256 MiB 小镜像）

```
mkfs.f2fs -f -l linux -b 4096 -w 512 -R 0:0 -O extra_attr,inode_checksum,flexible_inline_xattr fb.img
  Info: sector size = 512  /  Info: block size = 4096  /  Info: format successful
sload.f2fs -f <小样本树> -P fb.img          -> Done
fsck.f2fs -f fb.img                        -> exit 0
  [FSCK] valid_block_count matching with CP  [Ok..] [0x2236]
  [FSCK] valid_inode_count matched with CP   [Ok..] [0xf7]
  [FSCK] other corrupted bugs                [Ok..]
fsck.f2fs -l fb.img -> log_sectorsize: 9 / log_sectors_per_block: 3 / Filesystem volume name: linux
od -j 1024 -N 16 fb.img -> ... 09 00 00 00 03 00 00 00
```

## 8. /etc/fstab 挂载选项的内核文档依据

依据文件：`.build/kbuild/src/linux-sm8250-7.2.0_git20261010/Documentation/filesystems/f2fs.rst`
（7.2.0 源码树自带），配合 `fs/f2fs/super.c` 的选项表交叉验证
（`fsparam_flag_no("discard"…)`、`fsparam_flag_no("inline_xattr"…)`、
`fsparam_flag_no("inline_dentry"…)`、`fsparam_enum("errors"…)`、`fsparam_flag("atgc"…)`）。

| 选项 | 文档原文要点 | 用/不用 |
|---|---|---|
| `background_gc=on` | “Turn on/off cleaning operations … Default value for this option is on.” | 用（显式化默认值，便于审计） |
| `inline_xattr` | “Enable the inline xattrs feature.” | 用：xattr 存进 inode 块，省元数据块 |
| `inline_dentry` | “Enable the inline dir feature: data in newly created directory entries can be written into inode block.” | 用：rootfs 目录多且小，目录项内联可减少目录 I/O |
| `nodiscard` | “discard/nodiscard Enable/disable real-time discard in f2fs, if discard is enabled, f2fs will issue discard/TRIM commands when a segment is cleaned.” | 用 `nodiscard`：镜像里已启用 `fstrim.timer` 周期 trim，避免 segment 回收时同步 discard 带来的延迟抖动 |
| `errors=remount-ro` | “Specify f2fs behavior on critical errors … ‘remount-ro’ … remount the partition in read-only mode. By default it uses ‘continue’ mode.” | 用：根分区遇错转只读比“带错继续”更可预期（等价 ext4 的 `errors=remount-ro`） |
| `relatime` | VFS 通用挂载选项（不在 f2fs 选项表里，由 VFS 处理） | 用：常规 atime 策略 |
| `inline_data` | “inline data feature is enabled by default”（`noinline_data` 才是显式关闭） | **不写**：保持内核默认 |
| `atgc` / `gc_merge` | “Enable age-threshold garbage collection, it provides high effectiveness and efficiency on background GC.” | **不用**：16 GiB 根分区收益有限，多一条较新的 GC 代码路径；默认后台 GC 足够 |
| `compress_cache` / `compress_*` | 需要 compression 特性 | **不用**：内核无 `CONFIG_F2FS_FS_COMPRESSION`，镜像也未启用该特性 |
| `checkpoint=disable`, `fastboot`, `nobarrier` | 分别降低/关闭检查点、牺牲性能换挂载速度、不发 cache_flush | **不用**：根分区不接受掉电/性能换安全的取舍 |
| `mode=lfs` | zoned block device 专用 | **不用**：UFS 非 zoned |
| `heap`/`no_heap` | “Deprecated.” | **不用** |
| `user_xattr`/`acl` | 文档：默认已开启（`CONFIG_F2FS_FS_XATTR=y`、`CONFIG_F2FS_FS_POSIX_ACL=y`） | **不写**：保持默认 |

## 9. 内核模块与 depmod（实测）

内核 .config 实测：`CONFIG_F2FS_FS=y`、`CONFIG_EXT4_FS=y`、
`CONFIG_MODULE_COMPRESS=y`、`CONFIG_MODULE_COMPRESS_ZSTD=y`、
`CONFIG_MODULE_COMPRESS_ALL=y`、`CONFIG_MODULE_DECOMPRESS=y`。
因此 /lib/modules/<ver> 内没有也不应有 f2fs.ko/ext4.ko —— 这是预期，不是异常。

本次装配实测（`--modules output/20261024/kernel/modules.tar.zst`）：

```
[10:41:36] === stage: kernel modules (from the kernel module build) ===
[10:41:36] unpacked modules archive: <workspace>/elish-archlinux/output/20261024/kernel/modules.tar.zst
[10:41:36] modules in the tree BEFORE install: 7.1.6-1-aarch64-ARCH
[10:41:36] removing stale module tree lib/modules/7.1.6-1-aarch64-ARCH (not the running kernel)
[10:41:37] modules in the tree AFTER install: 7.2.0
[10:41:37] copied 587 module files into lib/modules/7.2.0
[10:41:37] modules.dep lines: 587 ; modules.dep.bin: 72306 bytes
[10:41:37] modules.alias lines: 7240
[10:41:37] panel module present: lib/modules/7.2.0/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko.zst (23544 bytes)
```

要点：

* 清理策略：ALARM 根包自带 `lib/modules/7.1.6-1-aarch64-ARCH`（vmlinuz 模块，与我们的内核无关），
  脚本在安装前删除除目标 kver 之外的所有目录，避免 depmod/模块搜索拿到两套模块。
  清理前后：BEFORE = 7.1.6-1-aarch64-ARCH；AFTER = 7.2.0。
* 安装方式：解包模块归档 → `cp -a <src>/7.2.0 <rootfs>/lib/modules/` → `depmod -b <rootfs> 7.2.0`。
* depmod 结果：modules.dep 587 行、modules.dep.bin 72306 B、modules.alias 7240 行（无 WARN/ERROR）。
* 面板驱动为模块：`lib/modules/7.2.0/kernel/drivers/gpu/drm/panel/panel-novatek-nt36523.ko.zst`（23544 B）确认存在；
  触控驱动 `kernel/drivers/input/touchscreen/nt36523/nt36523_ts.ko.zst` 同样在树内。
* `*.ko.zst` 支持：宿主 kmod 34.2 与 rootfs 内 kmod 34.2-1 同版本，实测 depmod 对压缩模块正常索引
  （样本：压缩后的 rpcsec_gss_krb5.ko → modules.dep 记录 `.../rpcsec_gss_krb5.ko.zst:`、modules.alias 正常生成、modinfo 可读）。
* mkinitcpio：rootfs 内虽有 `mkinitcpio 41-4`（ALARM 底包自带），本模块不写其配置、不执行、不产出 initramfs；
  initramfs 由内核/initramfs 模块产出并写入 boot_b。
## 10. 设备固件（已打入镜像）

固件来自三处，全部在包解包之后写入 rootfs 的 /usr/lib/firmware 与 /usr/share/qcom：

### 10.1 来源 1：pmaports 设备固件（设备专用，proprietary）

* pmaports 包 `device/testing/firmware-xiaomi-elish`，pkgver 20240917，license=proprietary；上游源
  `https://github.com/lujianhua/xiaomi-elish-firmware/archive/51e9ac8cd91d88de43fb016530b9421a2713467a.tar.gz`（27,263,264 B，已离线取到）。
* 按该 APKBUILD 的 package() 逐条复刻，共 **227 个文件 / 35,366,961 B**：

| 源目录 | 目标路径 | 文件数 |
|---|---|---|
| sm8250/{a650_zap.mbn,adsp*,cdsp*,slpi*,venus.mbn} | /usr/lib/firmware/qcom/sm8250/xiaomi/elish/ | 10 |
| cirrus/cs35l41* | /usr/lib/firmware/cirrus/ | 9 |
| novatek/nt36523-* | /usr/lib/firmware/novatek/ | 2 |
| hexagonfs/{sensors,socinfo}（APKBUILD 删掉 acdb/ 与 dsp/） | /usr/share/qcom/sm8250/Xiaomi/elish/ | 205 |
| 30-dsp-firmware.files（mkinitfs 专用清单，Arch 不用 mkinitfs） | /usr/share/elish/ | 1 |

### 10.2 来源 2：ALARM `linux-firmware-qcom 20260916-1`（本次新加入包清单）

* 为什么必须加：底包 `linux-firmware` 的分包只有 amdgpu/atheros/broadcom/cirrus/intel/mediatek/nvidia/other/radeon/realtek/whence，
  **没有 qcom 分包**，因此底包里 `/usr/lib/firmware/qcom` 整个目录不存在，Adreno 的 GPU 固件缺失。
* 该包 172,772,972 B（491 成员，解包 562,614,193 B），提供 `qcom/a650_gmu.bin`（41,548 B）、
  `qcom/a650_sqe.fw`（31,964 B）以及 qcom/sm8250/* 参考设备 blobs。
* 许可：`/usr/share/licenses/linux-firmware-qcom/{LICENSE.qcom,LICENSE.qcom-2,LICENSE.qcom_yamato,NOTICE.qcom}`（可再分发、仅二进制）。

### 10.3 来源 3：底包已有的 Wi-Fi/BT 固件

* Wi-Fi（ath11k WCN6855）：`ath11k/WCN6855/hw2.0/{amss.bin,board-2.bin,m3.bin,regdb.bin}`，来自 `linux-firmware-atheros`。
* 蓝牙（hci_qca WCN6855）：`qca/htbtfw20.tlv` 与 `qca/nvm_usb_*.bin`，来自 `linux-firmware-other`（qca/ 共 138 文件）。

### 10.4 GPU / Wi-Fi+BT / DSP 三类到位情况

| 类别 | 需要的文件 | 落位 | 状态 |
|---|---|---|---|
| GPU（Adreno 650） | `qcom/a650_gmu.bin` + `qcom/a650_sqe.fw`（linux-firmware-qcom）；`qcom/sm8250/xiaomi/elish/a650_zap.mbn`（设备专用签名 zap） | /usr/lib/firmware/qcom/ 与 …/xiaomi/elish/ | **到位** |
| Wi-Fi（ath11k WCN6855） | ath11k/WCN6855/hw2.0/{amss.bin,board-2.bin,m3.bin,regdb.bin} | /usr/lib/firmware/ath11k/ | **到位**（底包） |
| 蓝牙（WCN6855 hci_qca） | qca/htbtfw20.tlv、qca/nvm_usb_*.bin | /usr/lib/firmware/qca/ | **到位**（底包） |
| DSP（ADSP/CDSP/SLPI/venus） | adsp.mbn adspr.jsn adspua.jsn cdsp.mbn cdspr.jsn slpi.mbn slpir.jsn slpius.jsn venus.mbn | …/qcom/sm8250/xiaomi/elish/ | **到位** |
| 触控（novatek） | nt36523-boe.bin、nt36523-csot.bin | /usr/lib/firmware/novatek/ | **到位** |
| 音频 codec（cirrus） | cs35l41-dsp1-spk-prot{,-blh,-bll,-brh,-brl,-tlh,-tll,-trh,-trl} | /usr/lib/firmware/cirrus/ | **到位** |
| hexagonfs（传感器/SSC 文件系统） | 205 个文件（sensors/、socinfo/） | /usr/share/qcom/sm8250/Xiaomi/elish/ | 文件到位，但消费进程 hexagonrpcd 缺失（§13.1） |

两点必须写清楚（避免刷机指南写错）：

1. elish 的 SoC 是 SM8250、GPU 是 **Adreno 650**，所以文件是 `a650_*`；a690 属于 SM8450（骁龙 8 Gen 1），本机**不存在 a690 固件**。
2. 设备是 Wi-Fi 版平板，没有 modem，不需要 mba.mbn/modem 固件；`wlanmdsp.mbn` 属 ath10k/WCN3990，ath11k 的 elish 不需要。

调用：`scripts/build-rootfs.sh --firmware ../.build/firmware/elish-rootfs`。
## 11. MangoWM 运行时装配（mango 模块产出 + 依赖闭合）

安装映射（`--mango output/20261024/mango/stage`，脚本 install_mango 落位）：

| 源 | 目标 |
|---|---|
| bin/mango, bin/mangonag, bin/mmsg | /usr/bin/（0755） |
| etc/mango/config.conf, config.toml | /etc/mango/（0644） |
| share/wayland-sessions/mango.desktop | /usr/share/wayland-sessions/ |
| share/man/man1/{mangonag.1,mmsg.1} | /usr/share/man/man1/ |
| share/xdg-desktop-portal/mango-portals.conf | /usr/share/xdg-desktop-portal/ |
| lib/systemd/user/mango-session.target | /usr/lib/systemd/user/ |

运行时依赖（`LC_ALL=C readelf -d` 实测 DT_NEEDED，脚本逐个在 rootfs 内查找）：

```
ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libcjson.so.1 libglib-2.0.so.0
libgobject-2.0.so.0 libpango-1.0.so.0 libpangocairo-1.0.so.0 libcairo.so.2
libpcre2-8.so.0 libpixman-1.so.0 libinput.so.10 libxkbcommon.so.0
libwayland-client.so.0 libwayland-server.so.0 libwlroots-0.20.so
libscenefx-0.5.so libxcb.so.1 libxcb-icccm.so.4 libxcb-randr.so.0
```

结果：**31/31 条 DT_NEEDED 全部在 rootfs 内可解析**（三个二进制合计 31 条，去重 20 个库名）。
其中 `libcjson.so.1` 是唯一不被现有依赖链覆盖的库，已加入 packages.list（ALARM `cjson` 包）；
`libwlroots-0.20.so` 来自 `wlroots0.20 0.20.2-1`、`libscenefx-0.5.so` 来自 `scenefx 0.5-1`，
其余（pango/cairo/glib2/pcre2/libxcb/xcb-util-wm）由依赖闭包带入。

会话/seat 权限：

* `seatd 0.9.3-1` 已安装，`seatd.service` 已按单元 [Install] 段生成相对符号链接启用
  （`/etc/systemd/system/multi-user.target.wants/seatd.service`）。
* 新建 `seat` 组（脚本自动挑空闲 gid：本次 968，因为 997/998/999 已被占用），并把 `magma` 加入
  `wheel,audio,video,input,render,kvm,lp,optical,rfkill,storage,network,power,games,users,seat` 中实际存在的组；
  实测 /etc/group 结果：wheel:x:998:magma / video:x:983:magma / seat:x:968:magma / magma:x:1000:。
* XDG_RUNTIME_DIR 由 logind 按 PAM 会话分配（/run/user/1000），本模块不额外创建；首启自检项见 §14。
## 12. 验收证据

见 `output/20261024/rootfs/BUILD-REPORT.txt`（脚本自动生成，含 fsck/dump/计数/sha256 原文）。
摘要：

产物（`output/20261024/rootfs/`）：

| 文件 | 表观大小 | 实际占用 | SHA256 |
|---|---|---|---|
| linux-rootfs-f2fs.img | 17,179,869,184 B (16 GiB) | 3.3 GiB（稀疏） | b1a888c24c0055b915100819b7f01f13a19dde44e6da4a177c17869c8aae2ba8 |
| ArchLinuxARM-aarch64-elish-rootfs.tar.zst | 904,653,975 B (863 MiB) | 863 MiB | 5face63afce60cd1f86eb415a5c9e25d60bb97b16a98f8c12674b107c237ae72 |

* `MANIFEST.rootfs` 自校验：`sha256sum -c` → 两个文件均“成功”。
* `fsck.f2fs -f`：exit 0，全部 [Ok..]：
  `valid_inode_count matched with CP [Ok..] [0x133c5]`（=78789）、
  `valid_node_count matching with CP (nat lookup) [Ok..] [0x134b0]`（=79024）、
  `valid_block_count matching with CP [Ok..] [0xc9eb6]`（=827062 块 = 3,387,645,952 B）、
  `other corrupted bugs [Ok..]`。
* 计数一致性：源目录 find 计数 80641、去重 inode 78789；镜像 valid_inode_count 78789，**完全一致**
  （差额来自硬链接条目）。
* `fsck.f2fs -l`（补丁前）：Filesystem volume name: linux、log_blocksize 12(4096)、log_sectorsize 9、
  valid_block_count 827062、uuid f9e9c280-…（本次构建新建）。
* `dump.f2fs`：superblock features = 68 : extra_attr inode_checksum flexible_inline_xattr；
  total FS sectors = 33554432 (16384 MB)。
* **交付镜像**的两个冗余超级块（od 实测；补丁后未再被任何 f2fs 工具打开）：
  `0001024  10 20 f5 f2 01 00 11 00 0c 00 00 00 00 00 00 00`
  `0005120  10 20 f5 f2 01 00 11 00 0c 00 00 00 00 00 00 00` → log_sectorsize=12、log_sectors_per_block=0。
* 补丁精确性：`cmp -l` 恰好 4 行，八进制偏移与值：
  `1033: 011→014`、`1037: 3→0`、`5129: 011→014`、`5133: 3→0`。
* 归档：tar.zst 条目数 80641（= 源目录）；含 15 suid / 6 sgid；/boot 仅空目录。
* 模块：`/lib/modules/7.2.0` 587 个 .ko.zst，depmod 后 modules.dep 587 行、modules.dep.bin 72306 B、
  modules.alias 7240 行；panel-novatek-nt36523.ko.zst 存在（23544 B）。
* MangoWM：10 个文件落位；`mango`/`mangonag`/`mmsg` 的 DT_NEEDED 31/31 全部在 rootfs 内可解析。

### 12.1 f2fs 洁净态（P1）

顺序固定为：`mkfs.f2fs → sload.f2fs → fsck.f2fs -f（修复写回交付镜像）→ 4 字节扇区改写 → 校验`；
所有证据都在副本上采集（fsck -l 与 dump.f2fs 会改写超级块，绝不能碰交付镜像）：

```
[FSCK] fixing SIT types
[11:02:10] fixing lines in the first pass: 1
[11:02:28] copy-of-delivered fsck.f2fs -f: 'fixing SIT types' lines=1 (informational, always 1), [Fail] lines=0 (must be 0)
[11:02:42] bytes fsck changed in the copy: 4 at octal offsets [1033 1037 5129 5133 ] (expect 4 at 1033 1037 5129 5133 = the two superblock sector-size words)
```

关于 `[FSCK] fixing SIT types` 的实测澄清：

* 该行不是腐蚀信号 —— 对同一个文件连续跑两次 `fsck.f2fs -f`，两次都各打印 1 次；对交付镜像的副本同样 1 次。
  它是 fsck 在 -f 模式下无条件执行的 SIT 归一化步骤（本机 f2fs-tools 1.17.0 实测）。
* 真正的洁净证据：(a) [FSCK] 检查表全部 [Ok..]、`[Fail]` 行数 0、exit code 0；
  (b) fsck 对交付镜像副本只改动 4 个字节，偏移恰为 1033/1037/5129/5133（两个超级块的扇区大小字）——
  即交付镜像在结构上已是 fsck 的不动点，fsck 除了把 sb 的 sector-size 改回宿主设备值外无事可做。

### 12.2 产物与校验和（P3）

| 文件 | 大小(B) | SHA256 |
|---|---|---|
| rootfs/linux-rootfs-f2fs.img | 17179869184 | 8558f0c5e4488939548731150a8aafb0b823d0752774f9bd0d5bdd76192ffd6d |
| rootfs/ArchLinuxARM-aarch64-elish-rootfs.tar.zst | 1149395534 | 8ab2439cd17189ff6e0177b17077a36cbfc5a7eeb6522b5fec58c90777235cc2 |

机器可校验的 `SHA256SUMS`（同目录）：

```
8558f0c5e4488939548731150a8aafb0b823d0752774f9bd0d5bdd76192ffd6d  rootfs/linux-rootfs-f2fs.img
8ab2439cd17189ff6e0177b17077a36cbfc5a7eeb6522b5fec58c90777235cc2  rootfs/ArchLinuxARM-aarch64-elish-rootfs.tar.zst
```

* `cd output/20261024 && sha256sum -c SHA256SUMS` → 两行均为“成功”。
* `MANIFEST.rootfs` 保留人读表格（竖线分隔），机器校验走 SHA256SUMS。
### 12.3 固件计数（P2）

* 设备固件：227 文件 / 35,366,961 B（elish 专用 10 + cirrus 9 + novatek 2 + hexagonfs 205 + 清单 1）。
* `linux-firmware-qcom`：491 成员 / 562,614,193 B（a650_gmu.bin、a650_sqe.fw 等）。
* 树表观大小：无固件 3,040,491,817 B → 含固件后 3,638,418,615 B。


## 13. 已知限制与未决清单

### 13.1 hexagonrpcd 缺失（未决，已定级：非致命，破坏音频与传感器）

事实：ALARM 仓库没有 `hexagonrpcd`，也没有 qrtr/rmtfs/pd-mapper/tqftpserv 这些 QRTR 用户态组件，
只有 `libqrtr-glib` 库。设备配置 `/usr/share/hexagonrpcd/hexagonrpcd-sdsp.conf` 已装到位，但没有进程消费它。

| 功能 | 依赖 | 缺失后的具体表现 |
|---|---|---|
| 内建扬声器 / 耳机播放（ADSP 音频） | hexagonrpcd + adsp.mbn + cirrus cs35l41 + UCM2 profile | 完全没有声音。PipeWire/WirePlumber 能起来，但 UCM2 找不到 DSP 后端设备，硬件音量控制不可用 |
| 麦克风 / 录音（CDSP，本机无通话） | hexagonrpcd + cdsp.mbn | 录音不可用，arecord -l 无可用 PCM |
| 传感器融合（SSC：加速度计、光线） | hexagonrpcd + hexagonfs(sensors,socinfo) + libssc | 屏幕自动旋转、自动亮度不可用；udev 规则 81-libssc-xiaomi-elish.rules 已就位但无数据源 |
| GPU 渲染 / MangoWM 出图 | 不需要 hexagonrpcd（freedreno + a650_gmu.bin + a650_zap.mbn） | **不受影响**，固件已就位 |
| 触控 / 显示 / 相机 | 不需要 hexagonrpcd（novatek、DPU、venus） | **不受影响** |

下一步动作：为 aarch64 打包 hexagonrpcd（上游 linux-msm/hexagonrpc），并同时提供 qrtr/rmtfs/pd-mapper，
否则 hexagonrpcd 起来也拿不到 DSP 通道；之后 `systemctl enable hexagonrpcd`。

### 13.2 其它限制
1. **追加包未登记进 pacman 数据库**（`/var/lib/pacman/local`）。补救方向：把 185 个包做成
   本地仓库并 `pacman -U --overwrite '*' `（需要 aarch64 chroot，可后续用 bwrap+userns+qemu 实现）。
   当前已在 `/etc/elish-packages.txt` 记录清单。
2. **Arch 的 `locale-gen` 未逐字执行**（用 `localedef --prefix` 等价实现，locale 数据真实存在）。
3. **file capabilities 丢失**（仅 newuidmap/newgidmap 受影响，见 §6）。
4. **glib schema 未编译**、**icon/desktop 数据库未更新**（无 GUI 应用依赖当前即可用）。
5. **f2fs sector-size 字段**为最后一步定点改写（§7.3）；如需“工具原生可复核”的镜像请用
   `--f2fs-sector-size 512`。
6. **未装 hexagonrpcd**（ALARM 无此包），音频/DSP 属后续工作；**固件默认未打入**（用 `--firmware`）。
7. **首启扩容需在未挂载状态跑 `resize.f2fs`**（§7.4），需刷机/首启文档配合说明。
8. 本模块从不调用 `fastboot`，也不生成任何擦除/写其它分区的命令；产物元数据里目标分区
   只出现 `linux`。

## 14. rootfs 装配核对表（逐项实测）

| 项目 | 期望 | 实测 | 结论 |
|---|---|---|---|
| 模块目录名 | 只有 7.2.0 | `ls /lib/modules` → 7.2.0（安装前有 7.1.6-1-aarch64-ARCH，已删除） | PASS |
| 模块数量 | 587 | 587 个 .ko.zst | PASS |
| depmod | 无错误、索引生成 | modules.dep 587 行 / modules.dep.bin 72306 B / modules.alias 7240 行 | PASS |
| 面板驱动 | panel-novatek-nt36523.ko.zst | 存在于 kernel/drivers/gpu/drm/panel/（23544 B） | PASS |
| mango 落位 | 10 个文件 | /usr/bin 3 + /etc/mango 2 + wayland-sessions 1 + man1 2 + xdg-desktop-portal 1 + systemd/user 1 | PASS |
| mango 依赖 | 全部可解析 | DT_NEEDED 31/31 在 rootfs 内找到 | PASS |
| wlroots / scenefx | 0.20.2-1 / 0.5-1 | wlroots0.20 0.20.2-1、scenefx 0.5-1 已在 185 包清单内 | PASS |
| seatd | 安装 + 启用 + seat 组 | seatd 0.9.3-1；seatd.service → multi-user.target.wants；seat:x:968:magma | PASS |
| f2fs 镜像 | 4096 块、label=linux | mkfs -b 4096；fsck -l `Filesystem volume name: linux`、`log_blocksize: 12` | PASS |
| 镜像 sector size | 4096（12） | 交付镜像两个 sb 均为 0c/00（12/0），与 fsck 校验版本恰差 4 字节 | PASS |
| fsck | 通过 | fsck.f2fs -f exit 0、全部 [Ok..] | PASS |
| 计数一致 | 源=镜像 | 源 78789 去重 inode = 镜像 valid_inode_count 78789 | PASS |
| 扩容工具 | rootfs 内有 resize.f2fs | /usr/bin/resize.f2fs（f2fs-tools 1.17.0-1，指向 fsck.f2fs 的链接） | PASS |
| suid/sgid | 15 / 6 | 树与 tar 都是 15 suid / 6 sgid | PASS |
| /boot | 无内核/initramfs | tar 内 /boot 只有空目录条目 | PASS |
| 固件（设备） | 227 文件 | 35,366,961 B：elish 专用 10 + cirrus 9 + novatek 2 + hexagonfs 205 + 清单 1 | PASS |
| 固件（GPU） | a650_gmu.bin + a650_sqe.fw | 41,548 B + 31,964 B（linux-firmware-qcom）；另有设备签名 zap a650_zap.mbn | PASS |
| 固件（Wi-Fi/BT） | ath11k WCN6855 + qca BT | ath11k/WCN6855/hw2.0 四文件；qca/ 共 138 文件（含 htbtfw20.tlv） | PASS |
| 固件（DSP） | adsp/cdsp/slpi/venus | 9 个文件在 …/xiaomi/elish/ | PASS |
| hexagonrpcd | 有二进制 | ALARM 无此包；confd 已装但无消费进程 | FAIL（§13.1 已定级） |
| f2fs 洁净态 | fsck 不动点 | [Fail] 0 行；fsck 对副本只改 4 字节（1033/1037/5129/5133） | PASS |
| magma 口令 | $6$ SHA-512 | /etc/shadow 第二字段 100 字节，前缀 $6$；root 前缀 ! 锁定 | PASS |
| tty1 autologin | drop-in + enable | /etc/systemd/system/getty@tty1.service.d/autologin.conf；getty@tty1.service → getty@.service | PASS |
| serial-getty@ttyGS0 | opt-in | 默认无 getty.target.wants 链接，`elish.fixroot=1` 不再创建；需要时可 `systemctl enable --now serial-getty@ttyGS0.service` | PASS |
| mango-session | 0755 | /usr/local/bin/mango-session（XDG_RUNTIME_DIR 0700、MOZ_ENABLE_WAYLAND、exec mango） | PASS |
| .bash_profile | tty1 且 MANGO_RUNNING 未设 | /home/magma/.bash_profile 0644 属主 1000:1000 | PASS |
| NetworkManager | enabled + nmcli | 链接已建；/usr/bin/nmcli 存在；无写死 WiFi 口令（模板 + 助手） | PASS |
| sshd | enabled、镜像内无 host key | sshd.service + sshdgenkeys.service 链接已建；/etc/ssh 内 0 个 ssh_host_* | PASS |
| SHA256SUMS | 可机器校验 | cd output/20261024 && sha256sum -c SHA256SUMS 两行“成功” | PASS |
| MANIFEST | SHA256 与实际一致 | `sha256sum -c` 两文件均“成功” | PASS |

## 15. 真机可用性：口令 / tty1 自动登录 / MangoWM 自启 / 网络

### 15.1 magma 口令（root 仍锁定）

生成命令（确定性 salt，便于复现；在宿主执行）：

```bash
openssl passwd -6 -salt elishmagma magma
```

输出（写入 `/etc/shadow` 第二字段）：

```
$6$elishmagma$/j1XVDHVwIPK6GtMC33GfSHbdXr42SJN9SS7eBF53kFA8LDjql3lLD9dPSTmVbom1p5RmCjEp3vMW0wx8UMTZ1
```

* magma 的 shadow 字段长度 100 字节，以 `$6$` 开头（SHA-512 crypt，libxcrypt 支持），默认口令 `magma`。
* root 的字段以 `!!$y$j` 开头 → 锁定，root 不能登录。
* tty1 自动登录不需要口令；ssh 用 magma/magma（首次登录后请 passwd 修改）。

### 15.2 tty1 自动登录（getty drop-in）

* 文件：`/etc/systemd/system/getty@tty1.service.d/autologin.conf`（0644）

```ini
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin magma --noclear %I $TERM
```

* 实例启用：`/etc/systemd/system/getty.target.wants/getty@tty1.service` → `../../../usr/lib/systemd/system/getty@.service`（相对链接）。
* 必须先写空的 `ExecStart=` 清掉原值，否则会叠加两个 agetty。

### 15.3 MangoWM 会话启动器 /usr/local/bin/mango-session（0755）

```sh
uid=$(id -u)
runtime="${XDG_RUNTIME_DIR:-/run/user/$uid}"
[ -d "$runtime" ] || mkdir -p "$runtime"
chmod 700 "$runtime"; chown "$uid:$(id -g)" "$runtime"
export XDG_RUNTIME_DIR="$runtime"
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=mango XDG_SESSION_DESKTOP=mango
export MOZ_ENABLE_WAYLAND=1 QT_QPA_PLATFORM=wayland SDL_VIDEODRIVER=wayland GDK_BACKEND=wayland
exec /usr/bin/mango "$@" > /tmp/mango.log 2>&1
```

* 这里**不启动 seatd**：seatd.service 由 systemd 负责（已 enable），logind 也会给 tty1 会话 seat ACL。
* mango 的输出落在 `/tmp/mango.log`。

### 15.4 登录后自动起会话：/home/magma/.bash_profile（0644，属主 1000:1000）

```sh
if [ -z "${MANGO_RUNNING:-}" ] && [ "$(tty)" = "/dev/tty1" ] && [ -x /usr/local/bin/mango-session ]; then
    export MANGO_RUNNING=1
    exec /usr/local/bin/mango-session
fi
```

* 只在 tty1 且 `MANGO_RUNNING` 未设置时接管；其它 tty 与 ssh 保持普通 shell。
* 组权限：magma ∈ seat, video, input, render, wheel, audio, kvm, lp, optical, rfkill, storage, network, power, games, users。

### 15.5 网络与 sshd

* `NetworkManager.service` 已 enable；`nmcli` 在 `/usr/bin/nmcli`（networkmanager 包）；systemd-networkd 保持禁用。
* 无线 profile **不写死口令**：镜像只带模板 `/usr/share/elish/wifi-profile.nmconnection.template` 与交互助手
  `/usr/local/bin/elish-wifi-setup`（root 运行，问 SSID/口令后调 `nmcli device wifi connect`），
  生成的 `/etc/NetworkManager/system-connections/<SSID>.nmconnection` 由 NM 以 0600 保存。
* sshd：`sshd.service` 与 `sshdgenkeys.service` 已 enable；镜像内 host key 数量 **0**，首启由 `ssh-keygen -A` 自动生成，未复制宿主密钥。

### 15.6 首启后应看到什么（检查清单）

1. 不出现 `login:` 提示，直接进入 magma 的 shell 并被 mango 接管；
2. 短暂黑屏后出现 **MangoWM 空白平铺桌面**（无面板/壁纸属正常）；
3. 若回退到 shell：看 `cat /tmp/mango.log`、`journalctl -u seatd -b`、`ls -l /dev/dri/`（应有 card0/renderD128）、`loginctl session-status`；
4. 身份/权限：`id magma`（含 seat/video/input/render）、`ls -ld /run/user/1000`（0700 magma）；
5. 网络：`nmcli device status`、`sudo elish-wifi-setup`、`ping -c1 223.5.5.5`；
6. 远程：`systemctl status sshd`（首启生成 host key 后监听 22，用 magma/magma）。

### 15.7 首启扩容（真机事实：镜像 16 GiB，/dev/sda36 ≈195.5 GiB）

必须未挂载时执行（见 §7.4），例如 initramfs shell：

```bash
resize.f2fs /dev/disk/by-partlabel/linux      # -t 缺省 = 扩到设备末尾
reboot
```

验证：

* `df -h /` → 从 16G 变约 193G；
* 离线核验：`dump.f2fs /dev/disk/by-partlabel/linux` 的 `total FS sectors`，或 `fsck.f2fs -l /dev/disk/by-partlabel/linux` 的 `block_count`；
* 也可 `resize.f2fs -t <sectors> <dev>` 显式指定目标扇区（4096 字节扇区）。
