# log2ram 与 zram-generator

## 配置结论

| 项目 | 配置 | 目的 |
|---|---|---|
| log2ram | `configs/rootfs/etc/log2ram.conf`，`SIZE=1G` | 现场原始 `/var/log` 约 409 MiB，留出增长余量 |
| Journald | `SystemMaxUse=512M` | 日志上限小于 log2ram tmpfs 容量 |
| log2ram 写回 | `USE_RSYNC=true`、`JOURNALD_AWARE=true`、`ZL2R=false` | rsync 回写并轮转 journald；独立 zram swap 由 generator 管理 |
| zram swap | `10240` MiB、`lz4`、priority 100 | 提供 10 GiB 逻辑容量；容量按 MiB 表达 |

包来源：`zram-generator` 来自 Arch Linux ARM 的 `extra`；`log2ram` 使用 AUR `log2ram 1.7.2-1`，其 PKGBUILD 固定上游归档 SHA-256；`rsync` 是 log2ram 的可选依赖并已安装。

## 安装与同步

先在设备上以 magma 用户安装软件包；AUR 包需用 `yay`：

```sh
yay --noconfirm --needed -S log2ram zram-generator rsync
```

然后在仓库根目录同步配置，把占位符换成设备地址：

```sh
DEVICE_IP="<device-ip>"
set -o pipefail
tar -czf - -C configs/rootfs \
  etc/log2ram.conf \
  etc/systemd/zram-generator.conf \
  etc/systemd/journald.conf.d/10-log2ram.conf \
  | dbclient -i ~/.ssh/dropbear_ed25519_host_key "magma@${DEVICE_IP}" \
      'sudo -n tar -xzf - --no-same-owner -C /'
```

随后在设备上执行：

```sh
sudo -n systemctl daemon-reload
sudo -n systemctl start dev-zram0.swap
sudo -n systemctl enable --now log2ram.service
sudo -n systemctl enable --now log2ram-daily.timer
sudo -n systemctl restart systemd-journald.service
sudo -n systemctl reload log2ram.service
```

`zram-generator` 在启动时自动生成并启用 swap unit；无需手动 enable `dev-zram0.swap`。log2ram timer 每天 23:55 写回，服务停止时也执行回写。

## 验证

设备上执行：

```sh
systemctl is-active dev-zram0.swap log2ram.service log2ram-daily.timer systemd-journald.service
systemctl is-enabled log2ram.service log2ram-daily.timer
swapon --show
zramctl --output NAME,ALGORITHM,DISKSIZE,DATA,COMPR,TOTAL
findmnt -no TARGET,SOURCE,FSTYPE,OPTIONS /var/log
journalctl --disk-usage
systemd-analyze cat-config systemd/journald.conf | grep '^SystemMaxUse='
sudo -n du -sh /var/hdd.log /var/log
```

已验证输出摘要：

```text
active                         # 上述四个 unit 均为 active
enabled                        # log2ram.service 与 log2ram-daily.timer
/dev/zram0 partition 10G 0B 100
/dev/zram0 lz4 10G 4K 69B 20K
/var/log log2ram tmpfs ... size=1048576k,mode=755
SystemMaxUse=512M
415M /var/hdd.log
354M /var/log
```

`sudo -n systemctl reload log2ram.service` 已实际执行成功；回写后 journald 报告 9.8 MiB，磁盘副本仍在 `/var/hdd.log`。

## 已知限制

- log2ram 使用最多 1 GiB RAM；超过此容量可能导致启动/写入失败。调整日志量时应同时评估 `SIZE` 与 `SystemMaxUse`。现场启动时 log2ram unit 的峰值内存为约 725 MiB。
- RAM 中尚未回写的日志可能在断电时丢失；daily timer 在 23:55 写回，正常停止服务也会写回。
- 10 GiB 是 zram 的逻辑 swap 容量，不是预留 10 GiB 实体 RAM；实际 RAM 消耗随压缩数据增长。
- 本版本 `zram-size` 使用 MiB 算术表达式，故 10 GiB 写作 `10240`；`10G` 不是有效的容量写法。重启验证尚未执行，当前确认的是服务 active/enabled 与运行时挂载、swap 状态。
