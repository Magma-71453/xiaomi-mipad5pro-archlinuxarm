# schedplace — 原始发现与证据（S4 性能阶段）

本文件记录实现过程中**实际探测到的事实**、测试输入/输出、以及未决问题。
环境：Arch Linux x86_64 宿主（16 核、无 root、无 schedutil），目标机为
Xiaomi Mi Pad 5 Pro（SM8250）。宿主数据一律标注“宿主”。

## 1. 旧实现 `gamemod-elish-src` 结构摘要

单文件 `gamemod-elish.c`（947 行，C11 + POSIX），关键函数：

| 函数 | 行 | 作用 |
|---|---|---|
| `read_small_file` / `read_proc_stat` / `read_comm` / `read_proc_info` | 94-180 | `/proc` 读取；`stat` 解析用 `strrchr(line, ')')` 处理 comm 里的空格/括号，取 field 22 作为 starttime |
| `parse_cpu_list` / `detect_cpu_sets` / `print_cpu_set` | 195-290 | 解析 `online`、按最小 `cpu_capacity` 找小核簇（只找一簇） |
| `enumerate_policy_dirs` / `read_governor` / `write_governor` / `verify_governor` | 305-453 | 逐 policy 读写 `scaling_governor` 并校验 |
| `scan_processes` / `update_game_processes` / `is_protected_desktop_process` | 516-600 | 进程枚举、游戏进程树跟踪、桌面/音频保护名单 |
| `pin_to_little_cores` / `lower_scheduler_priority` / `apply_thread_tuning` | 626-712 | **对 `steamwebhelper*` 的所有线程**写小核掩码；非游戏线程设 `SCHED_BATCH` |
| `restore_task_states` | 732-762 | 退出时按 tid+starttime 恢复 affinity / 调度策略 |
| `run_game` | 774-920 | wrapper 主流程：切 governor → 起子进程 → 循环调优 → 退出恢复 |

配套：`install-root-helper.sh`（安装 root-owned cpufreq helper + sudoers 白名单）、
`install-schedutil-boot.sh`（用 drop-in 覆盖 `cpu-performance.service` 的 ExecStart）、
`tests/test-affinity.sh` + `tests/fake-steamwebhelper.c`（dummy 进程验证“绑小核→恢复”）。

判读：旧实现是 game-wrapper + 全局 governor 切换 + 单簇（小核）全族绑定；没有规则
模型、没有事件驱动、没有 cgroup cpuset、没有 dry-run、失败即拒绝运行。
新实现保留了 `/proc` 解析思路与“按 starttime 校验存活”，其余机制重写。

## 2. 宿主能力探测（2026-10-24，全部为真实命令输出）

| 探测项 | 命令 | 结果 |
|---|---|---|
| 内核 | `uname -r` | `7.2.11-1-cachyos-bore-lto` |
| 编译器 | `gcc --version` | gcc 16.2.1 |
| 拓扑 | `/sys/devices/system/cpu/cpu*/cpu_capacity` | 16 个 CPU **全部 1024**（同构，单簇） |
| 频率 | `cpufreq/cpuinfo_max_freq` | 全部 3201000 |
| cpufreq 驱动 | `.../policy0/scaling_driver` | `amd-pstate-epp` |
| governor | `.../policy0/scaling_governor` | `performance`，可选 `performance powersave`（**无 schedutil**） |
| schedutil 参数 | `.../policy0/schedutil/rate_limit_us` | 不存在 |
| uclamp | `/proc/sys/kernel/sched_util_clamp_min|max` | 存在（只读，值 1024） |
| EAS 开关 | `/proc/sys/kernel/sched_energy_aware` | 存在（空文件，只读） |
| cgroup | `/proc/self/cgroup` | cgroup v2 统一层级；`/sys/fs/cgroup/cpuset.cpus.effective = 0-15` |
| core_ctl / msm_performance / sched_boost / cpu_boost / task_boost | 见 `perf-audit-noise.sh` | 全部**不存在**（宿主属预期） |
| upmigrate/downmigrate | `/proc/sys/kernel/sched_upmigrate` 等 | **不存在**；`sched_migration_cost_ns` 也不存在 |
| inotify on procfs task dir | 自写 C 探针：watch `/proc/self/task` + `pthread_create` + `poll(800ms)` | **`poll_ready=0`**：procfs 不产生线程创建事件 |
| 同 uid 跨进程放置权限 | 自写 C 探针：`sched_setaffinity(子进程)` / `setpriority(子进程)` | 均成功（`EPERM=0`），因此测试可以真实改变 helper 的 affinity；生产环境仍需 root 才能作用于别的用户的进程 |
| 进程数 | `ls /proc | grep -c '^[0-9]'` | 4（沙箱环境，故扫描计数很小） |

## 3. 拓扑聚类用例（mock sysfs，输入/期望/实际）

夹具由 `tests/sp_fixture.h` 在 `BUILD_DIR/tmp/sp-test-XXXXXX/sys` 生成，
`sp_topo_discover()` 直接读它（`--sysfs-root` 同样可指向）。

| 用例 | 输入（cpu_capacity / 在线集） | 期望 | 实际（断言通过） |
|---|---|---|---|
| A 目标机 SM8250 | 0-3=284, 4-6=871, 7=1024；online 0-7 | 3 簇：little 0-3 / big 4-6 / prime 7，`source=cpu_capacity` | 一致；`perf=4-7`、`auto+background=0-3`、`auto+latency=4-7` |
| B 两档机型 | 0-3=512, 4-7=1024 | 2 簇：little 0-3，big=prime=4-7，warning 含 "aliases" | 一致 |
| C 同构 | 全部 1024 | 单簇降级：little=big=prime=0-7，`degraded=1`，warning 非空 | 一致 |
| D 只有频率 | 无 `cpu_capacity`，`cpuinfo_max_freq`=1.0/1.8/2.4GHz | 3 簇，`source=cpufreq_max_freq` | 一致（prime 容量 2400000） |
| E 数据缺失 | 既无 capacity 也无 freq，只有 online | 单簇降级，`source=none`，warning 显示 "0 of 8" | 一致 |
| F 离线 CPU | online=0,1,3-7；cpu2 有 1024 但离线 | 聚成 little 0-1 / big 3 / prime 4-7，不含 cpu2 | 一致 |
| G 覆盖不全 | 只有 cpu0=284、cpu1=871 | 单簇降级，warning 显示 "2 of 8" | 一致 |
| H sysfs 根不存在 | `<tmp>/does-not-exist` | 回退 `sysconf(_SC_NPROCESSORS_ONLN)`，`source=fallback`，单簇 | 一致（宿主 16 个在线 CPU） |

补充：宿主真实运行 `--show-topology` 输出

```
topology online=0-15 source=cpu_capacity levels=1 little=0-15/1024 big=0-15/1024 \
prime=0-15/1024 degraded=1 warning="single capacity level (1024) for all 16 online CPUs; ..."
```

即：同构宿主按设计降级为单簇并告警，不会伪造出一个“大核簇”。

## 4. 测试证据

`make -C perf/schedplace test`（`-Wall -Wextra -Wpedantic -Werror`，0 warning）：

| 测试二进制 | 覆盖点 | 通过数 |
|---|---|---|
| `test_util` | cpulist 解析/格式化/严格性、路径拼接、写入白名单（governor/online/max_freq 拒绝、缺失节点不创建、legacy 例外） | 92 |
| `test_rules` | 进程名/线程名/路径正则/uid/cgroup 过滤、busy_lt/busy_gt、首条命中、禁用规则、auto 校验、进程级候选预过滤 | 32 |
| `test_topology` | 上述 A–H 用例 + `sp_topo_resolve`（little/big/prime/perf/auto/keep） | 96 |
| `test_apply` | cpuset 组路径/创建/写 `cpuset.cpus`+`cpuset.mems`/写 `cgroup.threads`；dry-run 不建组不写文件；真实 apply 后 affinity 生效并在 restore 时恢复；`cpuset.cpus` 被放宽回在线全集 | 58 |
| `test_safety` | 配置解析错误（未知键/坏正则/缺 cluster/重名/未知 section）、legacy profile 无 flag 时 `--check-config` 退出 2、带 flag 退出 0、未知选项退出 1、`nm -u` 确认未引用 `sched_setscheduler/sched_setattr/sched_setparam` | 36 |
| `test_end_to_end` | 真实二进制 + mock sysfs + mock cgroup：dry-run 日志含 `new_mask=4-7/0-3`、磁盘零改动、sysfs 指纹不变；真实运行写出 `cpuset.cpus` 并把 helper 线程 affinity 改成 4-7/0-3；busy>30 的 helper 线程被豁免；100ms 线程探针在 2000ms 扫描间隔下仍能发现新线程 | 61 |

合计 **375 checks，0 失败**（`ALL TESTS PASSED`）。

## 5. 宿主实测开销（x86_64，非目标机）

命令：默认配置 + `--dry-run` 常驻 60s，采样 `/proc/<pid>/status:VmRSS` 与
`/proc/<pid>/stat` 的 utime+stime：

```
pid=6 hz=100 rss_first_kb=2556 rss_last_kb=2556 ticks_delta=4 cpu_percent=0.067
schedplace: summary scans=121 processes=484 threads=484 candidates=0 decisions=0 \
affinity=0 cgroup=0 nice=0 unchanged=0 errors=0 refused_writes=0
```

结论：RSS 恒定 2.5MiB，CPU ≈ 0.07% 单核（扫描间隔 500ms）。目标机 8 核、7.5GiB
内存下开销可忽略；实际放置时每次决策只多一次 `sched_setaffinity` 系统调用。

## 6. `scripts/perf-audit-noise.sh` 实际输出（宿主，截断）

```
== perf-audit-noise ==
host: Linux 7.2.11-1-cachyos-bore-lto x86_64 / 16 CPUs / ...
mode: print-only（不修改任何文件）
checks: 12
== [1/12] cpu-hotplug ==            状态: 不存在
== [3/12] core-ctl ==               状态: 不存在
== [4/12] sched-boost ==            状态: 不存在
== [8/12] migrate-thresholds ==     状态: 不存在
  结论    : 不可调，勿套用安卓数值（主线 EAS 没有 upmigrate/downmigrate 这类阈值节点）
== [10/12] sched-energy-aware ==    状态: 存在  /proc/sys/kernel/sched_energy_aware
== [11/12] sched-rt-runtime ==      状态: 存在  /proc/sys/kernel/sched_rt_runtime_us
summary: 12 项检查，存在 2 项，缺省 10 项，未做任何修改（print-only）
```

退出码 0。每项都输出“状态/路径/建议/预期收益/回退”。

## 7. `scripts/perf-schedutil.sh --show` 实际输出（宿主，截断）

```
== schedutil probe ==
cgroup v2: unified hierarchy present (/sys/fs/cgroup/cgroup.controllers)
cpufreq: 16 policy dir(s), scaling_driver=amd-pstate-epp
  POLICY     DRIVER         GOVERNOR      CUR        MAX        MIN        RATE_LIMIT_US
  policy0    amd-pstate-epp performance   3792753    4465261    1102866    n/a
  ...
global knobs (probed, never assumed):
  present ro  /proc/sys/kernel/sched_util_clamp_min = 1024
  present ro  /proc/sys/kernel/sched_energy_aware = ?
verdict: schedutil is NOT available on this host.
         ... reason: this platform uses a different cpufreq driver ...
         Nothing to do, exit 0.
```

退出码 0（要求：没有 schedutil 也要优雅退出并说明原因）。
`--apply` 不带数值时只打印用法提示（不写死任何数值）；带数值但节点不存在时
逐项 SKIP 并最后以 1 退出；写入前必须备份到 `--out`/默认目录。

## 8. 构建产物与仓库洁净度

* `BUILD_DIR` 默认 `$(abspath ../../../.build/perf/schedplace)` =
  `<workspace>/.build/perf/schedplace`（**仓库外**），
  可用 `make BUILD_DIR=... test` 覆盖；测试夹具也放 `$(BUILD_DIR)/tmp`。
* 源码目录叫 `lib/` 而不是 `src/`：仓库根 `.gitignore` 用 `src/` 忽略任何同名目录，
  放 `perf/schedplace/src/` 会让全部 C 源码从 git 视野里消失。
* `make clean` 删除 `$(BUILD_DIR)`、历史遗留的仓库内 `.build` 与旧二进制。
* 仓库 `scripts/audit-hygiene.sh` 复核：`summary: 0 failure(s), 3 warning(s)`
  （3 条 warning 均为其它模块既有的 MANIFEST/FLASH-LOG 项，与 perf 无关）。
* `bash -n` 对两个新脚本均通过；两者都不依赖外部工具（除 systemctl 用于 --apply）。
* `make install DESTDIR=<tmp> PREFIX=/usr` 安装 bin/config/unit 三个文件（见验证输出）。

## 9. 未决问题

1. **threaded cpuset 子树**：线程级 `cgroup.threads` 迁移要求 cpuset 组处于 threaded
   子树；目标机内核是否默认允许、以及是否需要 `cgroup.type` 调整为 threaded，
   尚未在设备上验证（宿主无 root）。当前实现检测 `cgroup.threads` 是否存在，
   缺失/被拒时告警一次并退化为仅 affinity。
2. **busy 阈值标定**：`steamwebhelper` 的 20%/30% 阈值来自经验值，需在设备上用
   `/proc/<tid>/schedstat` 与实际 Steam 界面负载校准；规则可热改（重启服务）。
3. **目标机 inotify 行为**：只在宿主内核 7.2.11 上验证了 procfs task 目录不产生事件；
   目标机内核（SM8250 主线/发行版内核）未验证，因此实现不依赖它。
4. **A/B 未执行**：设备尚未跑过 8 节的 A/B 方案（本轮只交付工具与方案）。
5. **systemd 单元的系统调用过滤**：`@system-service` 不包含 `sched_setaffinity/setpriority`（属 `@resources`）。首版 unit 会让守护进程在 systemd 下被 EPERM，已改为 `SystemCallFilter=@system-service @resources`。
6. **systemd unit 未安装验证**：宿主无 root，`systemd-analyze verify schedplace.service`
   只报 `/usr/bin/schedplace` 不存在（预期），未做真实启动验证。
7. **foreground marker 的上游**：`/run/schedplace/foreground` 需要合成器（MangoWM）
   写一个 marker 文件；当前只定义了约定，尚未实现合成器侧的 hook。
7. **规则里 `mango`/`wl_event_loop` 等名字**来自目标机生态约定，未在设备上逐一确认真实
   comm（例如 MangoWM 的实际线程名）；首次上机时应先用 `schedplace --once --dry-run`
   观察真实匹配结果再收紧规则。
