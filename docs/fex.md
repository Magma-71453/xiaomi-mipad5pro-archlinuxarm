# FEX-Emu on xiaomi-elish (aarch64 host, x86-64 guest)

真机验证（2026-10-25，Xiaomi Mi Pad 5 Pro / sm8250 / Arch Linux ARM）：

```
$ FEX /tmp/hello-x86_64            # 静态 x86-64
hello from x86-64 on FEX!
uname-ish: sizeof(long)=8

$ FEX /opt/fex/RootFS/default/bin/busybox uname -m   # 动态 x86-64 (musl)
x86_64
```

## 构建要点（设备端源码构建，8 核约 12 分钟）

| 依赖 | 为什么 |
|---|---|
| **clang + lld** | FEX 的 CMake 明确拒绝 GCC（`FEX doesn't support GCC! Use Clang instead`），实测 clang 23.1.3 可用 |
| **nasm** | CMake 要求 `ASM_NASM`（FEX 的 x86 汇编测试/前端） |
| **python-setuptools** | FEX 的版本脚本 `import pkg_resources`；缺了会导致 `string sub-command STRIP requires two arguments` |
| `-DBUILD_FEXCONFIG=FALSE` | FEXConfig 需要 Qt5；不需要 GUI 就别装 Qt |

源码**不能在设备上 clone**：设备所在网络连不上 github.com（`curl 28 Failed to connect after 133 s`）。
做法是在宿主 `git clone --depth 1 --recursive` 后打 tar（排除 `.git`，222 MB）经 Wi-Fi 传到设备解包。

```sh
# 设备上
cd ~/fex && mkdir -p build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_FEXCONFIG=FALSE ..
ninja -j8          # 8150 个目标
sudo ninja install  # -> /usr/bin/{FEX,FEXBash,FEXServer,...} + /usr/share/fex-emu/
```

## x86-64 rootfs

动态链接的 x86-64 程序需要一个 rootfs（提供 ld.so 与 libc）。设备可达的镜像里用 Alpine 最小根：

```sh
curl -fSL https://mirrors.aliyun.com/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz -o /tmp/alpine-x86.tgz
sudo mkdir -p /opt/fex/RootFS/default && sudo tar -xzf /tmp/alpine-x86.tgz -C /opt/fex/RootFS/default
mkdir -p ~/.fex-emu/RootFS && ln -sfn /opt/fex/RootFS/default ~/.fex-emu/RootFS/default
printf '{\n  "Config": {\n    "RootFS": "default"\n  }\n}\n' > ~/.fex-emu/Config.json
# root 也要有同样的 config（FEXBash 需要 chroot -> 必须 root）
sudo mkdir -p /root/.fex-emu/RootFS && sudo ln -sfn /opt/fex/RootFS/default /root/.fex-emu/RootFS/default
sudo cp ~/.fex-emu/Config.json /root/.fex-emu/Config.json
```

注意：
- Alpine rootfs 是 **musl**，只能跑 musl 程序；要跑 glibc 程序（多数 x86 商业软件/Steam）需要换成 glibc rootfs（Arch/Debian x86-64）。
- `FEXBash` 会 chroot 进 rootfs，必须以 root 运行；普通用户可直接用 `FEX <binary>` 跑动态程序（FEX 自己解析 ld.so）。
- 本次未构建 thunks（`BUILD_THUNKS=OFF`），即还没有 Windows/WOW64 支持；要跑 Windows 程序需另行开启并准备 32-bit 环境。
