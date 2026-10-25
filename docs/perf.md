# schedplace 设计文档（S4 性能阶段）

> 目标设备：Xiaomi Mi Pad 5 Pro（elish）/ Snapdragon 870 / SM8250
> （4× Cortex-A55 + 3× Cortex-A77 + 1× Cortex-A77 prime）。
> 目标场景：**日常桌面为主 + Steam 极少量游戏**。

## 1. 范围与定位

`schedplace` 是一个**只接管线程放置**的守护进程：

* 做：`sched_setaffinity` 掩码、cgroup v2 cpuset（`cpuset.cpus` + `cgroup.threads`）、
  可选 `nice`；
* 不做：写 `scaling_governor` / `scaling_max_freq` / `cpu*/online` / `sched_*` 调参节点，
  不设 `SCHED_FIFO` / `SCHED_RR`，不锁频、不停核。

频率与能效决策仍归内核 **schedutil + EAS**；本守护进程只影响“某个线程允许在哪些
CPU 上运行”，从而在不与内核抢决策的前提下改善交互路径的落核质量。

旧实现 `gamemod-elish-src`（`gamemod-elish run -- <cmd>`）属于 `gamemodrun` 一类：
游戏启动时把整机 governor 切成 `performance`，并把 `steamwebhelper` 的**所有**线程绑到
小核。用户 2026-10-24 的裁决是：这类做法收益有限、可优化，**不得作为核心调度器**。
因此新方案：

* 核心机制重写（规则化放置 + 运行时拓扑发现 + 事件/周期扫描）；
* 旧行为仅保留为 `[legacy-performance]`（默认 `enabled = false`，且必须显式
  `--allow-profile-performance` 才能启动），用于 A/B 对照与回退；
* 旧方案的“一刀切绑小核”作为问题点写进规则表（见 3.2 的双重条件）。

## 2. CPU 拓扑发现（运行时，绝不写死）

数据源（按顺序尝试，覆盖不全即降级）：

1. `/sys/devices/system/cpu/online` 得到在线 CPU 集合；
2. `/sys/devices/system/cpu/cpuN/cpu_capacity`；
3. 若 2 覆盖不全，改读 `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq`；
4. 两者都覆盖不全 / 完全缺失 → **单簇降级**并告警（`degraded=1`），
   不会猜测“0-3 是小核”。

聚类规则（对覆盖完整的取值集合）：

| distinct 取值数 | little | big | prime | 说明 |
|---|---|---|---|---|
| ≥ 3 | 最低一档（含更慢的所有档） | 次高档 | 最高档 | 目标机就是这一档 |
| 2 | 低档 | 高档 | **= big（别名）** | 告警：`prime aliases the big cluster` |
| 1 或数据缺失 | 在线全集 | 在线全集 | 在线全集 | 降级 + 告警 |

两档机型把 prime 并入 big 的理由：没有第三档时，“最快的那一档”既承担 big 也承担
prime 的职责；`perf = big ∪ prime` 仍然是正确的“性能核”语义，且不会把 CPU 划到
不存在的簇里。

目标机实测（用户提供，2026-10-24）：

```
cpu0-3: cpu_capacity 284   -> little 0-3
cpu4-6: cpu_capacity 871   -> big    4-6
cpu7:   cpu_capacity 1024  -> prime  7
```

这一组数据已作为 **mock sysfs 夹具**写进 `tests/test_topology.c`
（`test_sm8250`），断言 `levels == 3`、`little=0-3/284`、`big=4-6/871`、
`prime=7/1024`，并断言 `perf = 4-7`。

`cluster = auto` 的语义：`profile = latency` → `perf`（big ∪ prime）；
`profile = background` → `little`；`profile = default` 配 `auto` 是配置错误。
配置里因此只出现簇名，不出现 CPU 编号。

## 3. 规则模型

一条规则 = 若干**匹配键**（AND）+ 一个**动作**。规则按文件顺序求值，第一条命中生效。

匹配键：

| 键 | 含义 |
|---|---|
| `process` / `match_process` | 进程 comm（内核截断到 15 字符） |
| `thread` / `match_thread` | 线程 comm（同上） |
| `exe` / `match_exe` | `/proc/<pid>/exe` 的 POSIX ERE 正则 |
| `cgroup` | cgroup v2 路径正则 |
| `uid` / `exclude_uid` | 真实 uid 过滤 |
| `busy_lt` / `busy_gt` | 相对上一次采样的单核占用百分比（区间条件） |
| `case_insensitive` | 该规则所有正则忽略大小写 |

动作：

| 键 | 含义 |
|---|---|
| `cluster` | `little` / `big` / `prime` / `perf` / `keep` / `auto` |
| `affinity` | 是否写 `sched_setaffinity`（默认 true） |
| `cpuset` | 加入 `<cpuset_root>/<name>`（写 `cpuset.cpus`，再把 tid 写进 `cgroup.threads`） |
| `nice` | 可选 `setpriority`（-20..19） |
| `profile` | `latency` / `background` / `default`（`auto` 的解析依据，也进日志） |

## 4. 默认规则表（`configs/perf/schedplace.conf`）

| 规则 | 匹配 | 目标 | 理由 |
|---|---|---|---|
| `fex-jit` | thread `(FEX|JIT|CPUThread|rexjit|arm64ec)`，忽略大小写 | perf（大核簇） | FEX 的 JIT/CPU 模拟线程对单核延迟最敏感 |
| `wine-render` | thread `(wine|dxvk|vkd3d|vkr|wined3d|d3d11|d3d12|vulkan)` | perf | GPU 提交/present 线程驱动帧节奏，落小核会放大帧时间抖动 |
| `wine-exe` | exe `^/usr/(bin|lib[^/]*)/wine` | perf | 路径正则示例；整套 wine 优先性能核 |
| `mango-wm` | process `^(mango|MangoWM)$` | perf | 合成器主线程在输入→上屏关键路径上 |
| `wayland-input` | thread `^(wl_event_loop|input|InputReader|InputDispatcher|libinput)` | perf | 输入/事件循环唤醒延迟决定“跟手” |
| `steamwebhelper-render-keep` | process `^steamwebhelper$` + thread 渲染名 | keep | 渲染/合成线程（**名称条件**）不能被降级 |
| `steamwebhelper-busy-keep` | process `^steamwebhelper$` + `busy_gt = 30` | keep | 热线程（**占用条件**）不能被降级 |
| `steamwebhelper-idle` | process `^steamwebhelper$` + `busy_lt = 20` | little（cpuset `steam-web`） | 只有“非渲染且不忙”的 helper 线程才回收能效 |
| `background-indexer` | process `^(updatedb|plocate|baloo_file|...|tracker-*)` | little（`background`） | 索引是吞吐型、可延迟；**journald 不在其中** |
| `background-pkg` | process `^(pacman|makepkg|packagekitd|...)` | little（`background`） | 打包/构建可被抢占，避免桌面抖动 |
| `example-nice`（disabled） | — | — | nice 动作示例，默认关闭 |

### 4.1 为什么不像旧方案那样把 `steamwebhelper` 整体绑小核

旧实现 `pin_to_little_cores()` 对 `steamwebhelper*` 的**每个线程**写小核掩码，不看线程
职责。问题在于 `steamwebhelper` 里同时存在：

* 渲染/合成线程（Viz/Compositor/GPU 提交），掉到 A55 会直接抬高界面帧时间与输入延迟；
* 空闲 helper 线程，留在 A77 上纯属浪费能效。

本方案用**名称 + CPU 占用双重条件**区分：先两条 `keep` 规则（渲染名 / busy>30%）
豁免，再让 `busy_lt=20` 的剩余线程降到小核。占用是相对上一次采样的（`--once` 会做
一次 120ms 预热采样），因此第一次扫描不会把热点线程误降级。

## 5. 触发与事件模型

* **周期扫描**：`interval_ms`（默认 500，允许 200..2000），`timerfd` + `epoll`，
  单线程、无忙等；
* **线程数探针**：`probe_ms`（默认 100，允许 50..1000）只对“已命中规则的进程”的
  `/proc/<pid>/task` 做计数比对，变化即触发重算。这样新线程在一个探针周期内生效，
  而不是等下一次全量扫描；
* **inotify**：前台标记文件 `foreground_file`（默认 `/run/schedplace/foreground`）
  的创建/写入会立即触发重算；同时对被跟踪进程的 task 目录尝试加 watch，
  但**当前内核（7.2.11）不会为 procfs task 目录产生 inotify 事件**（已实测），
  所以它只是机会性加速，正确性由上面两条保证；
* `SIGUSR1` 打印状态；`SIGTERM`/`SIGINT` 优雅退出并按 `restore_on_exit` 恢复原掩码。

## 6. 可观测性

每次真正改变（或 dry-run 下会改变）放置时输出**一行**结构化日志到 stderr（journald 捕获）：

```
schedplace: decision tid=17 pid=15 comm=sp-evt-late pcomm=sp-evt-main uid=1000 \
exe=/usr/bin/fake_sp rule=evt profile=background cluster=auto old_mask=0-15 \
new_mask=0-3 cpuset=evt nice=- busy=1.2% result=dry-run err=0 reason="probe test"
```

字段：进程/线程、uid、exe、命中规则、profile/簇、原掩码 → 新掩码、cpuset 组、
nice、busy%、结果（`applied`/`unchanged`/`dry-run`/`keep`/`error`）、errno、reason。

`SIGUSR1` 输出（节选）：

```
schedplace: state pid=... dry_run=0 version=0.1.0
schedplace: topology online=0-7 source=cpu_capacity levels=3 little=0-3/284 big=4-6/871 prime=7/1024 degraded=0
schedplace: config interval_ms=500 cpuset_root=/sys/fs/cgroup/schedplace enable_cgroup=1 restore_on_exit=1 foreground_file=/run/schedplace/foreground legacy_profile=0
schedplace: counters scans=121 processes=484 candidate_threads=484 decisions=0 affinity=0 cgroup=0 nice=0 unchanged=0 errors=0 refused_writes=0 restored=0
schedplace: rule[0] name=fex-jit hits=0 cluster=auto profile=latency
schedplace: tracked tid=... start=... rule=... original_mask=0-15 affinity_changed=1 nice=0
```

宿主实测开销（**x86_64 宿主，不是目标机**；`--dry-run`、默认配置、60s）：

| 指标 | 数值 |
|---|---|
| RSS | 2556 kB（首尾相同，无增长） |
| CPU | 4 jiffies / 60s ≈ **0.067% of one core**（HZ=100） |
| 扫描次数 | 121 次 / 60s（≈500ms 一次） |

## 7. 与 EAS / schedutil 的分工边界

| 决策 | 归属 | schedplace 的立场 |
|---|---|---|
| 频率 / OPP 选择 | schedutil + EAS | 不写、不锁、不改 governor |
| 能效核选择、迁移阈值 | 内核 EAS | 不写 upmigrate/downmigrate（主线也没有这些节点） |
| 唤醒/负载均衡 | 内核调度器 | 不设 SCHED_*、不改 `sched_*` 调参 |
| 线程允许在哪些 CPU 上跑 | **schedplace（cpuset + affinity）** | 唯一职责 |
| 前台是谁 | 合成器（写 marker 文件） | 只读 marker，不反向控制合成器 |

设计上的取舍：cpuset/affinity 是**硬约束**，EAS 只会在允许的核里做选择。因此规则
必须保守——默认只对“明确的渲染/交互”放宽到性能核，对后台任务收敛到小核，
其余线程（绝大多数）**不匹配任何规则，保持原样**。

## 8. 设备上的 A/B 测量方案

目的：验证“放置”是否真的改善交互质量，并确认没有负收益（尤其功耗/发热）。

准备：

1. `scripts/perf-audit-noise.sh` 在设备上跑一遍，确认 core_ctl/msm_performance/
   sched_boost 等噪声源状态在 A/B 两段中一致（否则测的是别的东西）；
2. governor 在两段中都保持 `schedutil`（不要用 performance 掩盖差异）；
3. 固定亮度、关闭后台索引（或让两段都跑同样的索引任务）、同一室温；
4. 记录设备温度与电池状态，避免热降频混入。

流程（A/B/A，每段 ≥ 10 分钟，重复 3 轮，取中位数）：

* A：`systemctl stop schedplace`（基线）
* B：`systemctl start schedplace`（放置生效）
* 同一 workload 依次跑：桌面空闲 2min → 快速滚动/多窗口 3min → Steam 界面 3min →
  FEX 游戏 5min（如可用）。

指标与采集：

| 指标 | 采集方式 | 期望方向 |
|---|---|---|
| 系统级迁移与运行时间 | `cat /proc/schedstat`（每 CPU 的 run_time / wait_time / nr_switches / nr_migrations，取差值） | 交互线程迁移次数下降或持平 |
| 每线程运行队列等待 | `/proc/<tid>/schedstat` 三个字段（run_time, runqueue wait, timeslices） | 渲染/输入线程 runqueue wait 下降 |
| 每线程迁移次数 | `/proc/<tid>/sched`（需 `CONFIG_SCHED_DEBUG`）或 ftrace `sched_migrate_task` 事件计数 | 渲染/输入线程迁移下降 |
| 帧时间 | `MANGOHUD=1` 覆盖层或 `VK_LAYER_MANGOHUD`；或 `perf stat -e ...` 辅助；记录 p50/p95/p99 帧时间 | p95/p99 下降 |
| 输入延迟代理 | `libinput measure` / evtest 时间戳 + 合成器 page-flip 时间戳（合成器日志）；或用 `wayland-info` + 高速摄像 | 端到端延迟下降 |
| 唤醒延迟 | `perf sched latency`（若有）或 `/proc/<tid>/schedstat` 的 wait 时间 | 下降 |
| 功耗 | 电池 `/sys/class/power_supply/*/power_now` 积分，或整机功率计 | 不显著上升 |

判定：以 p95/p99 帧时间与输入延迟代理为主指标，功耗为守门指标（上升 >5% 需要
重新评估规则范围）。统计上用同 workload 的 3 轮中位数与 Mann-Whitney U / bootstrap
置信区间，避免单次抖动误判。

若 A/B 无差异（很可能在“桌面空闲”场景）：说明瓶颈不在放置，应保留规则但不要
继续加码（例如不要因此改成全局 performance）。

## 9. 回退方法

* 临时停用：`systemctl stop schedplace`（`SIGTERM` 会按 `restore_on_exit` 恢复被改线程
  的原 affinity，并把受管 cpuset 组放宽回在线全集）；
* 彻底卸载：`systemctl disable --now schedplace`、`make uninstall`、删除
  `/sys/fs/cgroup/schedplace`（组为空后 `rmdir`）；
* 只回退某条规则：把该规则的 `enabled = false`（或删掉），`systemctl restart`；
* legacy profile：默认关闭；即使打开也必须显式 `--allow-profile-performance`，
  去掉该参数即回到“仅放置”；
* 出现异常时的兜底：
  ```sh
  schedplace --check-config --config /etc/schedplace/schedplace.conf
  schedplace --show-topology --sysfs-root /sys/devices/system/cpu
  ```
  若 `degraded=1`，说明拓扑数据不可用，此时所有放置都退化为单簇（无害但无收益）。

## 10. 明确不采用

| 项 | 原因 |
|---|---|
| WALT（window assisted load tracking）及其 `sched_upmigrate`/`sched_downmigrate` 数值 | 安卓/厂商下游补丁，主线 EAS 没有这些节点；数值不可跨内核迁移，且会与 EAS 的 PELT 决策冲突 |
| 厂商闭源染色（`sched_boost` / `task_boost` / `msm_performance` / core_ctl 的自动策略） | 不可审计、不可复现，且直接覆盖 schedutil 与 EAS；本方案只审计不写入 |
| KGSL / GPU 频率表（`/sys/class/kgsl/...`、devfreq 的 GPU OPP） | 属 GPU 频率域，不在 CPU 放置范围；目标机内核是否暴露也未验证，盲目写入风险高 |
| `SCHED_FIFO` / `SCHED_RR` | 与 schedutil/EAS 抢核，容易饿死桌面与合成器 |
| 全局 `performance` governor | 抬功耗、抢 EAS 决策；旧方案的问题点 |
| 修改 `scaling_max_freq` / `cpu*/online` | 超出“放置”边界，且会与热管理冲突 |

## 11. 从 `gamemod-elish-src` 借鉴了什么 / 没借鉴什么

| 项 | 借鉴 | 原因 |
|---|---|---|
| `/proc/<pid>/stat` 解析（comm 含空格/括号，取 field 22 starttime） | ✅ | 处理 pid 复用与线程存活判断的正确姿势，直接复用思路 |
| comm 读取与 15 字符截断认知 | ✅ | 匹配键必须是内核截断后的名字 |
| 进程枚举（`/proc/[0-9]+` + `/proc/<pid>/task/*`） | ✅ | 简单可靠；本实现加了进程级缓存与候选预过滤 |
| cpu_capacity 聚类识别小核 | ✅（重写） | 思路对，但旧版只找“最小 capacity 一簇”，且失败即拒绝运行；新版聚成 little/big/prime 三档并在失败时降级 |
| steamwebhelper 前缀匹配 | ⚠️ 部分 | 保留“识别 steamwebhelper”的思路，但用名称+占用双重条件替代“整族绑小核” |
| 保护桌面/音频进程名单 | ⚠️ 部分 | 新版不靠硬编码保护名单，而是靠“不匹配任何规则就不动”的默认值 |
| 游戏会话期间切 `performance` governor | ❌ | 全局锁频、抢 EAS；改为默认关闭的 legacy profile |
| `SCHED_BATCH` 批量降级 | ❌ | 改的是调度策略，超出“只放置”的边界；且对交互线程有副作用 |
| 按游戏进程树作用域（wrapper 模式） | ❌ | 守护进程是常驻的、按规则匹配，不需要包裹启动命令；作用域由规则表达 |
| 进程退出后恢复原 affinity | ✅（改为常驻循环内的状态表） | 状态表记录 tid+starttime+原掩码，退出时按需恢复 |
| 依赖 `sudoers` 的 root helper 写 governor | ❌ | 不再有 governor 写入；cgroup 与 affinity 由 root 守护进程直接完成 |
| 单次启动/清理脚本 | ❌ | 改为 systemd 常驻 + 周期/事件驱动 |

## 12. 运维与安全自检

* 启动时校验配置：`interval_ms`/`probe_ms` 范围、规则完整性、cpuset 组名合法性；
  若配置要求 legacy performance profile 而没有 `--allow-profile-performance`，
  直接以退出码 2 拒绝启动；
* 所有写操作走统一的白名单守卫，只允许 `cpuset.cpus`、`cpuset.mems`、`cgroup.threads`；
  `scaling_governor`、`scaling_max_freq`、`online`、`sched_*`、厂商 boost 节点
  一律拒绝并计数（`refused_writes`），且 `open()` 不带 `O_CREAT`（缺失节点不会被创建）；
* cgroup 不可用（非 threaded 子树 / 无 cpuset 控制器 / 非 cgroup 文件系统）时，
  告警一次并退化为仅 affinity，不影响主流程；
* 单元测试覆盖上述负向用例（见 `docs/findings/perf.md`）。
