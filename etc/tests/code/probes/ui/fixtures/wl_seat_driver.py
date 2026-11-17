#!/usr/bin/env python3
"""Drives a Wayland compositor's seat like a user and an input method.

A dependency-free Wayland client (it speaks the wire protocol directly) for
probes that must go through the compositor rather than the app's own
listeners: a virtual keyboard (zwp_virtual_keyboard_v1), a virtual pointer
(zwlr_virtual_pointer_v1), and an input method (zwp_input_method_v2) that
sends preedit, commit and delete-surrounding to the focused text-input-v3
client.

usage: wl_seat_driver.py probe
       wl_seat_driver.py run SCRIPT
`probe` exits 0 when the compositor offers all three protocols, 3 otherwise.
SCRIPT holds one step per line:
  wait_file PATH            wait until PATH exists (the app's ready marker)
  move X Y                  pointer to surface-of-output position X Y
  click X Y                 move there and click the left button
  type TEXT                 type ASCII through the virtual keyboard
  key CODE                  press and release one evdev key code
  wait_active               wait until the focused client enables text input
  preedit TEXT CURSOR       show a composition (CURSOR in bytes, -1 hides)
  commit TEXT               commit text (ends any composition)
  delete BEFORE AFTER       delete bytes around the caret
  cancel                    end the composition without committing
  sleep SECONDS
  log TEXT                  print a marker
Each IME step is followed by zwp_input_method_v2.commit with the serial of
the last `done`, as the protocol requires.
"""
import array
import os
import socket
import struct
import subprocess
import sys
import time


def u32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def i32(v):
    return struct.pack("<i", v)


def wstr(s):
    b = s.encode("utf-8") + b"\0"
    pad = (4 - len(b) % 4) % 4
    return u32(len(b)) + b + b"\0" * pad


def rd_u32(p, o):
    return struct.unpack_from("<I", p, o)[0], o + 4


def rd_str(p, o):
    n, o = rd_u32(p, o)
    s = p[o:o + n - 1].decode("utf-8", "replace") if n else ""
    return s, o + ((n + 3) & ~3)


class Wayland:
    def __init__(self):
        path = os.path.join(os.environ["XDG_RUNTIME_DIR"], os.environ["WAYLAND_DISPLAY"])
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.next_id = 2
        self.buf = b""
        self.handlers = {1: self._display_event}
        self.globals = {}
        self.registry = self.new_id()
        self.handlers[self.registry] = self._registry_event
        self.send(1, 1, u32(self.registry))
        self.roundtrip()

    def new_id(self):
        i = self.next_id
        self.next_id += 1
        return i

    def send(self, obj, op, payload=b"", fds=()):
        msg = u32(obj) + u32(((8 + len(payload)) << 16) | op) + payload
        if fds:
            self.sock.sendmsg([msg], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))])
        else:
            self.sock.sendall(msg)

    def dispatch(self, timeout):
        self.sock.settimeout(timeout)
        try:
            data, _, _, _ = self.sock.recvmsg(65536, socket.CMSG_SPACE(16 * 4))
        except socket.timeout:
            return False
        if not data:
            raise SystemExit("compositor closed the connection")
        self.buf += data
        while len(self.buf) >= 8:
            obj, so = struct.unpack_from("<II", self.buf, 0)
            size, op = so >> 16, so & 0xFFFF
            if len(self.buf) < size:
                break
            payload, self.buf = self.buf[8:size], self.buf[size:]
            h = self.handlers.get(obj)
            if h:
                h(op, payload)
        return True

    def roundtrip(self):
        cb = self.new_id()
        done = []
        self.handlers[cb] = lambda op, p: done.append(1)
        self.send(1, 0, u32(cb))
        end = time.time() + 5
        while not done and time.time() < end:
            self.dispatch(0.2)
        if not done:
            raise SystemExit("roundtrip timed out")

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.dispatch(max(0.01, end - time.time()))

    def _display_event(self, op, p):
        if op == 0:
            obj, o = rd_u32(p, 0)
            code, o = rd_u32(p, o)
            msg, _ = rd_str(p, o)
            raise SystemExit("protocol error on object %d code %d: %s" % (obj, code, msg))

    def _registry_event(self, op, p):
        if op == 0:
            name, o = rd_u32(p, 0)
            iface, o = rd_str(p, o)
            ver, _ = rd_u32(p, o)
            self.globals.setdefault(iface, (name, ver))

    def bind(self, iface, version):
        name, ver = self.globals[iface]
        new = self.new_id()
        self.send(self.registry, 0, u32(name) + wstr(iface) + u32(min(version, ver)) + u32(new))
        return new


KEYS = {c: k for c, k in zip("1234567890", [2, 3, 4, 5, 6, 7, 8, 9, 10, 11])}
for row, start in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for i, c in enumerate(row):
        KEYS[c] = start + i
KEYS.update({" ": 57, "-": 12, "=": 13, ",": 51, ".": 52, "/": 53, ";": 39, "'": 40})
SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
           "(": "9", ")": "0", "_": "-", "+": "=", "<": ",", ">": ".", "?": "/", ":": ";", '"': "'"}


class Seat:
    def __init__(self, wl):
        self.wl = wl
        self.seat = wl.bind("wl_seat", 1)
        self.t0 = time.time()
        vkm = wl.bind("zwp_virtual_keyboard_manager_v1", 1)
        self.kbd = wl.new_id()
        wl.send(vkm, 0, u32(self.seat) + u32(self.kbd))
        km = subprocess.run(["xkbcli", "compile-keymap", "--layout", "us"],
                            capture_output=True, check=True).stdout + b"\0"
        fd = os.memfd_create("ny-keymap")
        os.write(fd, km)
        wl.send(self.kbd, 0, u32(1) + u32(len(km)), fds=[fd])
        os.close(fd)
        vpm = wl.bind("zwlr_virtual_pointer_manager_v1", 1)
        self.ptr = wl.new_id()
        wl.send(vpm, 0, u32(self.seat) + u32(self.ptr))
        imm = wl.bind("zwp_input_method_manager_v2", 1)
        self.im = wl.new_id()
        self.active = False
        self.serial = 0
        wl.handlers[self.im] = self._im_event
        wl.send(imm, 0, u32(self.seat) + u32(self.im))
        wl.roundtrip()
        self.extent = (int(os.environ.get("NY_DRIVER_OUTPUT_W", "1280")),
                       int(os.environ.get("NY_DRIVER_OUTPUT_H", "720")))

    def now(self):
        return int((time.time() - self.t0) * 1000) & 0xFFFFFFFF

    def _im_event(self, op, p):
        if op == 0:
            self.pending_active = True
        elif op == 1:
            self.pending_active = False
        elif op == 5:
            self.serial += 1
            if hasattr(self, "pending_active"):
                self.active = self.pending_active
        elif op == 6:
            raise SystemExit("input method unavailable (another one is bound)")

    def key(self, code, mods=0):
        if mods:
            self.wl.send(self.kbd, 2, u32(mods) + u32(0) + u32(0) + u32(0))
        self.wl.send(self.kbd, 1, u32(self.now()) + u32(code) + u32(1))
        self.wl.pump(0.03)
        self.wl.send(self.kbd, 1, u32(self.now()) + u32(code) + u32(0))
        if mods:
            self.wl.send(self.kbd, 2, u32(0) * 4)
        self.wl.pump(0.03)

    def type(self, text):
        for c in text:
            shift = c.isupper() or c in SHIFTED
            base = SHIFTED.get(c, c.lower())
            if base not in KEYS:
                raise SystemExit("no key for %r" % c)
            if shift:
                self.wl.send(self.kbd, 1, u32(self.now()) + u32(42) + u32(1))
            self.key(KEYS[base], 1 if shift else 0)
            if shift:
                self.wl.send(self.kbd, 1, u32(self.now()) + u32(42) + u32(0))

    def move(self, x, y):
        w, h = self.extent
        self.wl.send(self.ptr, 1, u32(self.now()) + u32(x) + u32(y) + u32(w) + u32(h))
        self.wl.send(self.ptr, 4)
        self.wl.pump(0.05)

    def click(self, x, y):
        self.move(x, y)
        self.wl.send(self.ptr, 2, u32(self.now()) + u32(0x110) + u32(1))
        self.wl.send(self.ptr, 4)
        self.wl.pump(0.05)
        self.wl.send(self.ptr, 2, u32(self.now()) + u32(0x110) + u32(0))
        self.wl.send(self.ptr, 4)
        self.wl.pump(0.1)

    def ime(self, payloads):
        for op, payload in payloads:
            self.wl.send(self.im, op, payload)
        self.wl.send(self.im, 3, u32(self.serial))
        self.wl.pump(0.1)


def run(script):
    wl = Wayland()
    seat = Seat(wl)
    for raw in open(script, encoding="utf-8").read().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cmd, _, rest = line.partition(" ")
        args = rest.split(" ") if rest else []
        if cmd == "wait_file":
            end = time.time() + 120
            while not os.path.exists(rest) and time.time() < end:
                wl.pump(0.1)
            if not os.path.exists(rest):
                raise SystemExit("timed out waiting for " + rest)
        elif cmd == "move":
            seat.move(int(args[0]), int(args[1]))
        elif cmd == "click":
            seat.click(int(args[0]), int(args[1]))
        elif cmd == "type":
            seat.type(rest)
        elif cmd == "key":
            seat.key(int(args[0]))
        elif cmd == "wait_active":
            end = time.time() + 30
            while not seat.active and time.time() < end:
                wl.pump(0.1)
            if not seat.active:
                raise SystemExit("the focused client never enabled text input")
        elif cmd == "preedit":
            text, cursor = " ".join(args[:-1]), int(args[-1])
            seat.ime([(1, wstr(text) + i32(cursor) + i32(cursor))])
        elif cmd == "commit":
            seat.ime([(0, wstr(rest))])
        elif cmd == "delete":
            seat.ime([(2, u32(int(args[0])) + u32(int(args[1])))])
        elif cmd == "cancel":
            seat.ime([(1, wstr("") + i32(0) + i32(0))])
        elif cmd == "sleep":
            wl.pump(float(rest))
        elif cmd == "log":
            print(rest, flush=True)
        else:
            raise SystemExit("unknown step: " + line)
    wl.pump(0.3)
    print("DRIVER-DONE", flush=True)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "probe":
        try:
            wl = Wayland()
        except (KeyError, OSError):
            return 3
        need = ("zwp_input_method_manager_v2", "zwp_virtual_keyboard_manager_v1",
                "zwlr_virtual_pointer_manager_v1", "wl_seat")
        missing = [n for n in need if n not in wl.globals]
        print("missing: " + ",".join(missing) if missing else "seat driver: ok")
        return 3 if missing else 0
    if len(sys.argv) == 3 and sys.argv[1] == "run":
        run(sys.argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
