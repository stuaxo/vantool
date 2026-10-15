#!/usr/bin/env python3
"""Pixel-exact tests for vanplay (Linux SDL build and DOS VANPLAY.EXE).

  test_vanplay.py [--no-dos] [--no-sdl]

SDL:  runs the Linux build headless (SDL_VIDEODRIVER=dummy) with -n 1 -c capture.bmp and compares
      the rendered capture with the reference frame at scales 1 and 2.
DOS:  runs VANPLAY.EXE paused (-p -s N) in dosbox-x on Xvfb :99, screenshots, and compares; also
      checks that Right and Home keys step frames.  Covers mode 13h, VESA 0x100/0x101/0x107 (banked),
      PC and WN files, and centring.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from imgio import read_png  # noqa: E402
from vanfile import VanFile, VanWriter, testpattern_frame, testpattern_palette  # noqa: E402

ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
GUI = os.path.join(HERE, "gui")
VISTAPRO = os.environ.get("VISTAPRO", os.path.join(ROOT, "vistapro"))   # VistaPro 3.20 directory (optional)
PIC = os.path.join(VISTAPRO, "pic.van")
failures = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


def dac(v):
    return [tuple((c << 2) | (c >> 4) for c in v.palette6[i * 3:i * 3 + 3]) for i in range(256)]


def compare(v, frame, getpx, x0, y0, sc):
    pal, ref, w = dac(v), v.read_frame(frame), v.width
    return sum(pal[ref[y * w + x]] == getpx(x0 + x * sc + sc // 2, y0 + y * sc + sc // 2)
               for y in range(v.height) for x in range(w)) == w * v.height


def read_bmp32(path):
    d = open(path, "rb").read()
    off, (w, h), bpp = struct.unpack_from("<I", d, 10)[0], struct.unpack_from("<ii", d, 18), d[28]
    st = (w * bpp // 8 + 3) & ~3
    def px(x, y):
        o = off + ((h - 1 - y) if h > 0 else y) * st + x * (bpp // 8)
        return (d[o + 2], d[o + 1], d[o])
    return abs(w), abs(h), px


def make_files(work):
    files = {}
    for name, (w, h), n, var in [("T320", (320, 200), 6, "WN"), ("T640", (640, 480), 4, "WN"),
                                 ("T640PC", (640, 480), 4, "PC"), ("T400", (400, 300), 4, "WN"),
                                 ("T201", (201, 151), 4, "WN")]:
        path = os.path.join(work, name + ".VAN")
        out = VanWriter(path, w, h, testpattern_palette(), {320: 0x13, 640: 0x101, 400: 0x100, 201: 0x13}[w], var)
        for i in range(n):
            out.write_frame(testpattern_frame(w, h, i, n))
        out.close()
        files[name] = path
    if os.path.exists(PIC):        # 3 real 1280x1024 VP3 frames, VESA 0x107
        path = os.path.join(work, "BIG.VAN")
        with open(PIC, "rb") as src, open(path, "wb") as dst:
            dst.write(src.read(0x400 + 3 * 1280 * 1024))
        files["BIG"] = path
    return files


def sdl_tests(files, work):
    exe = os.path.join(SRC, "vanplay")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    # read-ahead ring: play a run of frames through budgets of 0, 2 slots, 5 slots and all frames,
    # wrapping past the end; the last frame shown must be exact
    v = VanFile(files["T640"])
    slot_kb = (v.frame_size + 1023) // 1024
    for kb in (0, 2 * slot_kb, 5 * slot_kb, 8192):
        start, count = 2, 7                                     # frames 2..8 then wraps (4 frames total)
        last = (start + count - 1) % v.frame_count
        cap = os.path.join(work, f"ring_{kb}.bmp")
        r = subprocess.run([exe, files["T640"], "-s", str(start), "-n", str(count), "-r", "60", "-b", str(kb),
                            "-x", "1", "-c", cap], env=env, capture_output=True, text=True, timeout=120)
        ok = r.returncode == 0 and os.path.exists(cap)
        if ok:
            _, _, px = read_bmp32(cap)
            ok = compare(v, last, px, 0, 0, 1)
        check(ok, f"sdl read-ahead budget {kb} KB: frames {start}..{start + count - 1} (wrapping), last exact")
    for name, path in files.items():
        v = VanFile(path)
        for frame, scale in ((v.frame_count - 1, 1), (1, 2)):
            cap = os.path.join(work, f"{name}_{frame}_{scale}.bmp")
            r = subprocess.run([exe, path, "-s", str(frame), "-n", "1", "-x", str(scale), "-c", cap],
                               env=env, capture_output=True, text=True, timeout=120)
            ok = r.returncode == 0 and os.path.exists(cap)
            if ok:
                cw, ch, px = read_bmp32(cap)
                ok = (cw, ch) == (v.width * scale, v.height * scale) and compare(v, frame, px, 0, 0, scale)
            check(ok, f"sdl {name} frame {frame} scale {scale}")


class DosBox:
    """VANPLAY.EXE in dosbox-x on Xvfb :99; screenshots via xwd."""

    def __init__(self, drive):
        self.drive, self.proc = drive, None
        self.env = dict(os.environ, DISPLAY=":99")

    def x(self, *args):
        return subprocess.run(list(args), env=self.env, capture_output=True, text=True).stdout

    def start(self, cmd, wait):
        self.stop()
        self.proc = subprocess.Popen([os.path.join(GUI, "run-dos.sh"), self.drive, cmd],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        time.sleep(wait)
        self.win = self.x("xdotool", "search", "--name", "DOSBox").split()[0]
        self.x("xdotool", "windowmove", self.win, "0", "0")
        time.sleep(1)

    def key(self, k, wait):
        self.x("xdotool", "windowfocus", self.win)
        self.x("xdotool", "key", k)
        time.sleep(wait)

    def shot(self, path):
        geo = self.x("xdotool", "getwindowgeometry", self.win)
        w, h = map(int, geo.split("Geometry:")[1].split()[0].split("x"))
        xwd = subprocess.run(["xwd", "-root", "-silent"], env=self.env, capture_output=True).stdout
        subprocess.run([sys.executable, "-I", os.path.join(GUI, "shot.py"), path, f"0,0,{w},{h}"], input=xwd)
        sw, sh, rgb = read_png(path)
        return sw, sh, lambda x, y: tuple(rgb[(y * sw + x) * 3:(y * sw + x) * 3 + 3])

    def stop(self):
        subprocess.run(["pkill", "-9", "-x", "dosbox-x"])
        time.sleep(1)


def dos_tests(files, work):
    drive = os.path.join(work, "dos")
    os.makedirs(drive)
    shutil.copy(os.path.join(SRC, "VANPLAY.EXE"), drive)
    for path in files.values():
        shutil.copy(path, drive)
    box = DosBox(drive)
    # name -> (mode width, mode height, display scale, start frame)
    cases = {"T320": (320, 200, 2, 3), "T640": (640, 480, 1, 2), "T640PC": (640, 480, 1, 1),
             "T400": (640, 400, 1, 2), "T201": (320, 200, 2, 1), "BIG": (1280, 1024, 1, 1)}
    try:
        for name, (mw, mh, sc, frame) in cases.items():
            if name not in files:
                continue
            v = VanFile(files[name])
            box.start(f"VANPLAY {name}.VAN -p -s {frame}", 25 if name == "BIG" else 8)
            sw, sh, px = box.shot(os.path.join(work, f"dos_{name}.png"))
            menubar = sh - mh * sc                       # dosbox-x menu bar above the DOS screen
            x0, y0 = (mw - v.width) // 2 * sc, menubar + (mh - v.height) // 2 * sc
            check(compare(v, frame, px, x0, y0, sc), f"dos {name} frame {frame} (mode {mw}x{mh})")
            if name in ("T640", "BIG", "T201"):
                box.key("Right", 20 if name == "BIG" else 4)
                _, _, px = box.shot(os.path.join(work, f"dos_{name}_r.png"))
                check(compare(v, frame + 1, px, x0, y0, sc), f"dos {name} Right -> frame {frame + 1}")
                box.key("Home", 20 if name == "BIG" else 4)
                _, _, px = box.shot(os.path.join(work, f"dos_{name}_h.png"))
                check(compare(v, 0, px, x0, y0, sc), f"dos {name} Home -> frame 0")
                # while paused the read-ahead ring fills from idle time; stepping then draws from XMS
                for _ in range(5):
                    box.key("Right", 1)
                time.sleep(20 if name == "BIG" else 4)
                _, _, px = box.shot(os.path.join(work, f"dos_{name}_r5.png"))
                check(compare(v, 5 % v.frame_count, px, x0, y0, sc),
                      f"dos {name} 5x Right (read-ahead) -> frame {5 % v.frame_count}")
    finally:
        box.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--no-dos", action="store_true")
    p.add_argument("--no-sdl", action="store_true")
    p.add_argument("--keep", action="store_true")
    a = p.parse_args()
    work = tempfile.mkdtemp(prefix="vanplay_test_")
    try:
        files = make_files(work)
        if not a.no_sdl:
            sdl_tests(files, work)
        if not a.no_dos:
            dos_tests(files, work)
    finally:
        if a.keep:
            print("work dir:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
