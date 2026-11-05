#!/bin/sh
# 构建全部 MangoWM 外壳组件（dock / 编辑模式 overlay / 音量 OSD）。
#
#   sh build-shell.sh            # 全部构建到当前目录
#   sh build-shell.sh install    # 构建并安装到 /usr/local/bin
#
# 为什么有这个脚本：mango-dock / mango-actions / mango-osd 三个 C 程序原先只有
# build-dock.sh 覆盖 dock，另外两个的构建命令只散落在文档里，新机器克隆仓库后
# 无法复现镜像里的二进制。这里把三个组件的构建收敛成一处，协议代码也只生成一次。
#
# 目标架构：本脚本产出的是**当前机器**架构的二进制。要在 x86_64 主机上得到
# aarch64 版本，需要在 aarch64 设备上跑（推荐，见 scripts/build-mango-on-device.sh），
# 或用 aarch64 交叉工具链并让 pkg-config 指向 aarch64 sysroot。
#
# 不要在 fish 里直接粘贴 cc 行：fish 不会按空格拆分 "$(pkg-config ...)"，
# 每个 flag 都会变成一个错误参数，链接时报一屏 undefined reference。
set -e
cd "$(dirname "$0")"

# ---------------------------------------------------------------- 协议代码生成 --
gen_protocols() {
	[ -f wlr-layer-shell-unstable-v1-protocol.c ] && return 0
	command -v wayland-scanner >/dev/null 2>&1 || {
		echo "缺少 wayland-scanner（Arch: pacman -S wayland）" >&2; exit 2; }
	wayland-scanner client-header wlr-layer-shell-unstable-v1.xml \
		wlr-layer-shell-unstable-v1-client-protocol.h
	wayland-scanner private-code wlr-layer-shell-unstable-v1.xml \
		wlr-layer-shell-unstable-v1-protocol.c
	wayland-scanner private-code \
		/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
		xdg-shell-protocol.c
	wayland-scanner client-header \
		/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
		xdg-shell-client-protocol.h
}

# pkg-config 少一个包时给出人话错误，而不是让 cc 抛 undefined reference。
# 注意 ALARM 的 cJSON 包，.pc 名是 libcjson（不是 cjson）。
need_pkg() {
	command -v pkg-config >/dev/null 2>&1 || {
		echo "缺少 pkg-config（Arch: pacman -S pkgconf）" >&2; exit 2; }
	for p in "$@"; do
		pkg-config --exists "$p" && continue
		# cjson 的包名在不同发行版/版本里可能是 cjson 或 libcjson，两者都试
		case "$p" in
			cjson) pkg-config --exists libcjson && continue ;;
			libcjson) pkg-config --exists cjson && continue ;;
		esac
		echo "缺少 pkg-config 包: $p" >&2
		echo "Arch/ALARM 上：wayland-client 在 wayland 包，cairo 在 cairo 包，" >&2
		echo "cJSON 的 .pc 名是 libcjson（包名 cjson）" >&2
		exit 2
	done
}

# 返回实际可用的 pkg-config 名（cjson / libcjson 二选一）
pc_name() {
	case "$1" in
		cjson)
			if pkg-config --exists libcjson; then echo libcjson; else echo cjson; fi ;;
		*) echo "$1" ;;
	esac
}

gen_protocols

# ------------------------------------------------------------------- dock --
need_pkg wayland-client cairo
cc -O2 -Wall -o mango-dock mango-dock.c \
	wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
	$(pkg-config --cflags --libs wayland-client cairo) -lm
echo "built $(pwd)/mango-dock"

# --------------------------------------------------------- 编辑模式 overlay --
need_pkg wayland-client cairo cjson
cc -O2 -Wall -o mango-actions mango-actions.c \
	wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
	$(pkg-config --cflags --libs wayland-client cairo "$(pc_name cjson)") -lm
echo "built $(pwd)/mango-actions"

# ------------------------------------------------------------- 音量 OSD --
need_pkg wayland-client cairo
cc -O2 -Wall -o mango-osd mango-osd.c \
	wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
	$(pkg-config --cflags --libs wayland-client cairo) -lm
echo "built $(pwd)/mango-osd"

# ------------------------------------------------------------- 安装 --
if [ "${1:-}" = "install" ]; then
	# 目标机上 wheel 组配置了 NOPASSWD，所以不需要喂口令。
	# 这里绝不写死口令：旧脚本用 `printf 'magma\n' | sudo -S`，等于把口令
	# 明文留在仓库里，且换口令后整个安装流程会静默失败。
	sudo -n install -m755 mango-dock    /usr/local/bin/ || {
		echo "install 失败：目标需要免密 sudo（sudo -n 可用）" >&2; exit 1; }
	sudo -n install -m755 mango-actions /usr/local/bin/
	sudo -n install -m755 mango-osd     /usr/local/bin/
	echo "installed to /usr/local/bin/"

	# 安装后立刻核对磁盘上的版本确实变了（见 AGENTS.md §5「安装必须可验证」）
	for f in mango-dock mango-actions mango-osd; do
		printf '  %-14s %s bytes  %s\n' "$f" \
			"$(stat -c%s "/usr/local/bin/$f")" \
			"$(sha256sum "/usr/local/bin/$f" | cut -c1-16)…"
	done
fi
