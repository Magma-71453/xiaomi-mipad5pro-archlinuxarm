# USB gadget 默认关闭与救援网络

## 现象与原因

`usb0` 是 USB Ethernet gadget（`g_ether`），设备断开时可以无 carrier 但仍留有地址和直连路由。`usb-gadget-off.service` 原本只解绑 ConfigFS 的 UDC，没有清理接口地址。当前 UDC 为空时，内核仍可能显示本机 `ttyGS0` / ConfigFS 函数；这些节点不等于 gadget 已绑定到 USB 控制器并暴露给主机。

## 修复

`configs/rootfs/etc/systemd/system/usb-gadget-off.service` 保留原有 UDC unbind，在后续 oneshot 命令清除 `usb0` 的 IPv4/IPv6 地址与路由；接口不存在或清理失败时不使 unit 失败。`usb-net-debug.service` 和 `usb-net-up.sh` 保留，用于手动 USB-Ethernet 救援；默认仍 disabled。

## 同步与验证

在仓库根目录将占位符改为目标地址后同步 unit：

```sh
DEVICE_IP="<device-ip>"
set -o pipefail
tar -czf - -C configs/rootfs \
  etc/systemd/system/usb-gadget-off.service \
  | dbclient -i ~/.ssh/dropbear_ed25519_host_key "magma@${DEVICE_IP}" \
      'sudo -n tar -xzf - --no-same-owner -C /'
```

设备上执行：

```sh
sudo -n systemctl daemon-reload
sudo -n systemd-analyze verify --man=no usb-gadget-off.service
sudo -n systemctl restart usb-gadget-off.service
systemctl is-active usb-gadget-off.service
sudo -n ip -o address show dev usb0
sudo -n ip -4 route show table all dev usb0
sudo -n cat /sys/kernel/config/usb_gadget/g1/UDC
systemctl is-enabled usb-net-debug.service
```

已在线及重启验证：unit active；usb0 地址数与 IPv4 路由数均为 0；UDC 未绑定，usb-net-debug 仍 disabled。UDC 内容为空时不要把 `ttyGS0` 节点当作主机正在看到的串口设备。

## 限制

- 手动启动 USB 网络救援后，服务会重新绑定 gadget 并添加网络地址；正常重启后的关闭单元不会在手动启动之后再次执行。
- 未连接外部 USB 主机做枚举测试；验证范围是本机 UDC 解绑、接口无地址/路由及服务状态。
- 文档和配置不保存真实地址、设备 MAC、代理信息或凭据。
