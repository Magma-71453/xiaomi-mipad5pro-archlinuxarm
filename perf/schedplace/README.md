# schedplace

规则化**线程放置**守护进程（C11，无第三方依赖）。只做两件事：

1. 用标量匹配键（进程名 / 线程 comm / 可执行路径正则 / cgroup / uid / CPU 占用）选出线程；
2. 把它们放到运行时发现的某个核簇：`sched_setaffinity` 掩码，可选加入 cgroup v2
   cpuset 组，可选调整 nice。

它**不**锁频、**不**停核、**不**改 governor、**不**写 CPU hotplug、**不**改
`SCHED_*` 策略。频率与能效仍归内核 schedutil + EAS。

## 构建 / 测试 / 安装

构建产物一律落在仓库之外（默认 `../../../.build/perf/schedplace`），
可以用 `BUILD_DIR=` 覆盖：

```sh
make -C perf/schedplace                  # 默认 BUILD_DIR 在仓库外
make -C perf/schedplace test             # 单元测试 + 端到端测试
make -C perf/schedplace BUILD_DIR=/tmp/sp test
sudo make -C perf/schedplace install DESTDIR=/          # 装 bin/config/unit
```

二进制路径：`${BUILD_DIR:-../../../.build/perf/schedplace}/schedplace`。
`make -C perf/schedplace print-build-dir` 会打印实际目录。

## 运行

```sh
# 只观察，不写任何文件（不需要 root）
./schedplace --once --dry-run --config ../../configs/perf/schedplace.conf
./schedplace --dry-run --config ../../configs/perf/schedplace.conf   # 常驻观察

# 真正生效（需要 root：写 cpuset + 给别的进程设 affinity）
sudo ./schedplace --config /etc/schedplace/schedplace.conf

# 查拓扑与配置
./schedplace --show-topology --sysfs-root /sys/devices/system/cpu
./schedplace --check-config --config ../../configs/perf/schedplace.conf

# 运行中看状态（SIGUSR1 打印拓扑、计数器、逐规则命中数、被跟踪线程）
kill -USR1 $(pidof schedplace)
```

退出码：0 正常、1 用法/配置错误、2 安全拒绝（例如配置要求 legacy performance
profile 但没给 `--allow-profile-performance`）、3 运行时失败。

## 文档

* 设计、规则表、A/B 测量、回退：`docs/perf.md`
* 宿主探测、测试证据、未决问题：`docs/findings/perf.md`
