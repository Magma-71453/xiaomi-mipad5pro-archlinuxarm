#!/bin/sh
# Bring up the USB Ethernet gadget on elish so the device is reachable from the
# host over the USB cable (10.11.12.2/24 + IPv6 link-local). Debug helper.
set -u
modprobe libcomposite 2>/dev/null || true
modprobe u_ether 2>/dev/null || true
modprobe usb_f_ecm 2>/dev/null || true
mkdir -p /sys/kernel/config
mount -t configfs configfs /sys/kernel/config 2>/dev/null || true
G=/sys/kernel/config/usb_gadget/g1
mkdir -p "$G" 2>/dev/null || true
echo 0x1d6b > "$G/idVendor" 2>/dev/null || true
echo 0x0104 > "$G/idProduct" 2>/dev/null || true
mkdir -p "$G/strings/0x409" 2>/dev/null || true
echo elish > "$G/strings/0x409/serialnumber" 2>/dev/null || true
echo elish > "$G/strings/0x409/product" 2>/dev/null || true
echo postmarketOS > "$G/strings/0x409/manufacturer" 2>/dev/null || true
mkdir -p "$G/configs/c.1/strings/0x409" 2>/dev/null || true
echo ecm > "$G/configs/c.1/strings/0x409/configuration" 2>/dev/null || true
mkdir -p "$G/functions/ecm.usb0" 2>/dev/null || true
ln -sf "$G/functions/ecm.usb0" "$G/configs/c.1/" 2>/dev/null || true
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -n "${UDC:-}" ]; then echo "$UDC" > "$G/UDC" 2>/dev/null || true; fi
ip link set usb0 up 2>/dev/null || true
ip addr add 10.11.12.2/24 dev usb0 2>/dev/null || true
exit 0
