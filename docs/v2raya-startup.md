# v2rayA 开机 TUN 启动时序

## 现象与原因

包自带的 unit 有 `After=network-online.target`，但只有 `Wants=network.target`；`After=` 只排序，不会拉入目标。首次启动时 network-online 未激活，v2rayA 在约 10 秒与 NetworkManager / wpa_supplicant 同时启动。

直接等待 NetworkManager 后的重启验收显示：NM waiter 在启动约 17 秒完成，v2rayA 随后启动，但 tun0 仍缺失；timesyncd 到约 48 秒才同步时钟（设备无 RTC）。时钟同步后手动重启 v2rayA 并发起请求，tun0 出现且 HTTP 返回 200。启动竞态包含网络和系统时间两部分；应用日志没有明确的 TUN 创建错误。

## 修复

`configs/rootfs/etc/systemd/system/v2raya.service.d/30-network-online.conf` 显式 Wants / After 两个等待单元：

- `NetworkManager-wait-online.service`：等待 Wi-Fi manager 报告 startup complete；使用精确 waiter，避免 broad `network-online.target` 拉起设备上启用的 networkd waiter（可等待 120 秒）。
- `systemd-time-wait-sync.service`：等待系统时间同步，避免无 RTC 时 v2rayA 早于 timesyncd 启动。

设备专用 `20-confdir.conf` 未修改；代理 profile / 节点配置不在仓库。

## 同步与验证

在仓库根目录同步配置时，把占位符换成设备地址：

```sh
DEVICE_IP="<device-ip>"
set -o pipefail
tar -czf - -C configs/rootfs \
  etc/systemd/system/v2raya.service.d/30-network-online.conf \
  | dbclient -i ~/.ssh/dropbear_ed25519_host_key "magma@${DEVICE_IP}" \
      'sudo -n tar -xzf - --no-same-owner -C /'
```

设备上执行：

```sh
sudo -n systemctl daemon-reload
sudo -n systemd-analyze verify --man=no v2raya.service
sudo -n systemctl restart v2raya.service
systemctl show v2raya.service -p Wants -p After
systemctl is-active NetworkManager-wait-online.service systemd-time-wait-sync.service v2raya.service
journalctl -b -u NetworkManager-wait-online.service -u systemd-time-wait-sync.service -u v2raya.service -o short-monotonic --no-pager
```

重启实测：NM waiter 于约 13.36 秒完成；systemd-time-wait-sync 于约 13.40 秒完成；v2rayA 于约 13.42 秒启动。NTPSynchronized=yes，启动后 tun0 已存在且有 4 条路由，代理请求返回 HTTP 200（响应正文未输出）。

## 限制

- NetworkManager waiter 的 `NM_ONLINE_TIMEOUT=60`；系统时间 waiter 的 `TimeoutStartSec=infinity`。若无 NTP，v2rayA 会保持 pending，直到系统时钟同步。
- broad `network-online.target` 在此设备上还会拉入 networkd waiter，可能触发 systemd-networkd 并等待 120 秒；本配置刻意只等 NetworkManager。
- 仓库与提交不包含真实 IP、代理节点、订阅、凭据或测试响应正文。
