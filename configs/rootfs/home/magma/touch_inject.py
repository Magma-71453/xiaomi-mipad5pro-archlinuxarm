#!/usr/bin/env python3
"""touch_inject.py — drive MangoWM from a script through a temporary virtual touchscreen.

Every run creates a uinput touchscreen, injects one gesture and destroys the
device again, so nothing is left behind in /dev/input or in libinput's device
list.  The uinput initialisation sequence is the one verified in
local/waydroid/utap.c (LOCAL ONLY skeleton, ported here as the real tool).

  sudo python3 /home/magma/touch_inject.py tap   640 400
  sudo python3 /home/magma/touch_inject.py drag  600 95 760 320
  sudo python3 /home/magma/touch_inject.py drag  600 95 760 320 500
  sudo python3 /home/magma/touch_inject.py swipe 3 400 600 400 300 400

Coordinates are *logical* MangoWM coordinates (output 1280x800, scale 2, panel
rotated 90 deg).  to_dev() reverse-maps them onto the raw panel axes; the
panel's real ABS ranges are MT_X 0..1599 / MT_Y 0..2559 (measured with
EVIOCGABS on /dev/input/event4), not 1279/799.

root is required (/dev/uinput is root-only); magma has NOPASSWD sudo.
"""

import argparse
import ctypes
import fcntl
import os
import struct
import sys
import time

# ---------------------------------------------------------------- geometry --
LOG_W, LOG_H = 1280, 800          # logical output after rr:3 + scale 2
DEV_MAX_X, DEV_MAX_Y = 1599, 2559  # raw EVDEV ABS_MT_POSITION_X/Y maximum

DEVICE_NAME = "mango-virtual-touch"
SETTLE = 0.8    # let libinput hotplug and classify the node before events
LINGER = 0.8    # let the closing SYN_REPORT land before UI_DEV_DESTROY
TAP_HOLD = 0.09

# ------------------------------------------------------------ linux/uinput --
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0x00
BTN_TOUCH = 0x14A
ABS_MT_SLOT = 0x2F
ABS_MT_TOUCH_MAJOR = 0x30
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39
INPUT_PROP_DIRECT = 0x01
BUS_VIRTUAL = 0x06


class InputAbsInfo(ctypes.Structure):
    _fields_ = [("value", ctypes.c_int32), ("minimum", ctypes.c_int32),
                ("maximum", ctypes.c_int32), ("fuzz", ctypes.c_int32),
                ("flat", ctypes.c_int32), ("resolution", ctypes.c_int32)]


class UinputAbsSetup(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint16), ("pad", ctypes.c_uint16),
                ("absinfo", InputAbsInfo)]


class InputId(ctypes.Structure):
    _fields_ = [("bustype", ctypes.c_uint16), ("vendor", ctypes.c_uint16),
                ("product", ctypes.c_uint16), ("version", ctypes.c_uint16)]


class UinputSetup(ctypes.Structure):
    _fields_ = [("id", InputId),
                ("name", ctypes.c_char * 80),
                ("ff_effects_max", ctypes.c_uint32)]


_IOC_NONE, _IOC_WRITE = 0, 1


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("U") << 8) | nr


UI_DEV_CREATE = _ioc(_IOC_NONE, 1, 0)
UI_DEV_DESTROY = _ioc(_IOC_NONE, 2, 0)
UI_DEV_SETUP = _ioc(_IOC_WRITE, 3, ctypes.sizeof(UinputSetup))
UI_ABS_SETUP = _ioc(_IOC_WRITE, 4, ctypes.sizeof(UinputAbsSetup))
UI_SET_EVBIT = _ioc(_IOC_WRITE, 100, 4)
UI_SET_KEYBIT = _ioc(_IOC_WRITE, 101, 4)
UI_SET_ABSBIT = _ioc(_IOC_WRITE, 103, 4)
UI_SET_PROPBIT = _ioc(_IOC_WRITE, 110, 4)

EVENT = struct.Struct("llHHi")   # struct input_event on 64-bit


def to_dev(x, y):
    """Logical (1280x800) -> raw panel coords (0..1599 / 0..2559)."""
    dev_x = (LOG_H - y) * DEV_MAX_X // LOG_H
    dev_y = x * DEV_MAX_Y // LOG_W
    return (max(0, min(DEV_MAX_X, dev_x)), max(0, min(DEV_MAX_Y, dev_y)))


class Touchscreen:
    def __init__(self, name=DEVICE_NAME):
        try:
            self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        except OSError as exc:
            sys.exit("touch_inject: cannot open /dev/uinput (%s); run it with sudo" % exc)
        self.name = name

    def _ioctl_struct(self, request, obj):
        buf = ctypes.create_string_buffer(bytes(obj), ctypes.sizeof(obj))
        fcntl.ioctl(self.fd, request, buf)

    def _set_abs(self, code, maximum):
        ab = UinputAbsSetup()
        ab.code = code
        ab.absinfo.maximum = maximum
        self._ioctl_struct(UI_ABS_SETUP, ab)

    def create(self):
        fd = self.fd
        for ev in (EV_SYN, EV_KEY, EV_ABS):
            fcntl.ioctl(fd, UI_SET_EVBIT, ev)
        fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
        for abs_code in (ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X,
                         ABS_MT_POSITION_Y, ABS_MT_TOUCH_MAJOR):
            fcntl.ioctl(fd, UI_SET_ABSBIT, abs_code)
        # mandatory: without DIRECT libinput treats the node as a touchpad
        fcntl.ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)

        self._set_abs(ABS_MT_SLOT, 9)
        self._set_abs(ABS_MT_TRACKING_ID, 65535)
        self._set_abs(ABS_MT_POSITION_X, DEV_MAX_X)
        self._set_abs(ABS_MT_POSITION_Y, DEV_MAX_Y)
        self._set_abs(ABS_MT_TOUCH_MAJOR, 255)

        us = UinputSetup()
        us.id.bustype = BUS_VIRTUAL
        us.id.vendor = 0x1234
        us.id.product = 0x5678
        us.name = self.name.encode()[:79]
        self._ioctl_struct(UI_DEV_SETUP, us)
        fcntl.ioctl(fd, UI_DEV_CREATE)
        time.sleep(SETTLE)

    def emit(self, etype, code, value):
        os.write(self.fd, EVENT.pack(0, 0, etype, code, value))

    def sync(self):
        self.emit(EV_SYN, SYN_REPORT, 0)

    def slot_down(self, slot, tracking_id, dev_x, dev_y):
        self.emit(EV_ABS, ABS_MT_SLOT, slot)
        self.emit(EV_ABS, ABS_MT_TRACKING_ID, tracking_id)
        self.emit(EV_ABS, ABS_MT_POSITION_X, dev_x)
        self.emit(EV_ABS, ABS_MT_POSITION_Y, dev_y)
        self.emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 8)

    def slot_move(self, slot, dev_x, dev_y):
        self.emit(EV_ABS, ABS_MT_SLOT, slot)
        self.emit(EV_ABS, ABS_MT_POSITION_X, dev_x)
        self.emit(EV_ABS, ABS_MT_POSITION_Y, dev_y)

    def slot_up(self, slot):
        self.emit(EV_ABS, ABS_MT_SLOT, slot)
        self.emit(EV_ABS, ABS_MT_TRACKING_ID, -1)

    def destroy(self):
        time.sleep(LINGER)
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)


def _steps(duration_ms):
    return max(2, min(120, int(duration_ms) // 15 or 2))


def _interp(a, b, i, n):
    return a + (b - a) * i // n


def tap(ts, p0):
    """One contact, pressed and released in place."""
    dev_x, dev_y = to_dev(*p0)
    ts.slot_down(0, 101, dev_x, dev_y)
    ts.emit(EV_KEY, BTN_TOUCH, 1)
    ts.sync()
    time.sleep(TAP_HOLD)
    ts.slot_up(0)
    ts.emit(EV_KEY, BTN_TOUCH, 0)
    ts.sync()


def play(ts, fingers, p0, p1, duration_ms):
    """Move len(fingers) simultaneous contacts from p0 to p1 in one gesture."""
    dev0 = to_dev(*p0)
    dev1 = to_dev(*p1)
    steps = _steps(duration_ms)
    for slot, tracking_id in enumerate(fingers):
        ts.slot_down(slot, tracking_id, dev0[0], dev0[1])
    ts.emit(EV_KEY, BTN_TOUCH, 1)
    ts.sync()

    for step in range(1, steps + 1):
        dev_x = _interp(dev0[0], dev1[0], step, steps)
        dev_y = _interp(dev0[1], dev1[1], step, steps)
        for slot in range(len(fingers)):
            ts.slot_move(slot, dev_x, dev_y)
        ts.sync()
        time.sleep(duration_ms / 1000.0 / steps)

    for slot in range(len(fingers)):
        ts.slot_up(slot)
    ts.emit(EV_KEY, BTN_TOUCH, 0)
    ts.sync()


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="touch_inject.py",
        description="Inject a tap / drag / multi-finger swipe through a temporary "
                    "uinput touchscreen. Coordinates are logical MangoWM pixels "
                    "(1280x800); needs root.")
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="COMMAND")

    p = sub.add_parser("tap", help="single-finger tap")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)

    p = sub.add_parser("drag", help="single-finger drag")
    p.add_argument("x0", type=int)
    p.add_argument("y0", type=int)
    p.add_argument("x1", type=int)
    p.add_argument("y1", type=int)
    p.add_argument("ms", type=int, nargs="?", default=300, metavar="MS",
                   help="duration in ms (default 300)")

    p = sub.add_parser("swipe", help="N-finger swipe, all fingers together")
    p.add_argument("n", type=int, help="finger count (2..10)")
    p.add_argument("x0", type=int)
    p.add_argument("y0", type=int)
    p.add_argument("x1", type=int)
    p.add_argument("y1", type=int)
    p.add_argument("ms", type=int, nargs="?", default=400, metavar="MS",
                   help="duration in ms (default 400)")
    args = ap.parse_args(argv)

    if os.geteuid() != 0:
        sys.exit("touch_inject: /dev/uinput is root-only, run it with sudo")
    if args.cmd == "swipe" and not 2 <= args.n <= 10:
        sys.exit("touch_inject: swipe needs 2..10 fingers")

    if args.cmd == "tap":
        fingers, p0, p1, duration = 1, (args.x, args.y), (args.x, args.y), 0
    elif args.cmd == "drag":
        fingers, p0, p1, duration = 1, (args.x0, args.y0), (args.x1, args.y1), args.ms
    else:
        fingers = args.n
        p0, p1, duration = (args.x0, args.y0), (args.x1, args.y1), args.ms

    ts = Touchscreen()
    ts.create()
    try:
        if args.cmd == "tap":
            tap(ts, p0)
        else:
            play(ts, [101 + i for i in range(fingers)], p0, p1, duration)
    finally:
        ts.destroy()

    print("%s logical %s -> %s device %s -> %s fingers=%d" % (
        args.cmd, p0, p1, to_dev(*p0), to_dev(*p1), fingers))
    return 0


if __name__ == "__main__":
    sys.exit(main())
