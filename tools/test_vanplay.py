#!/usr/bin/env python3
"""Pixel-exact tests for vanplay (Linux SDL build and DOS VANPLAY.EXE).

  test_vanplay.py [--no-dos] [--no-sdl] [--exe PATH [--wrapper CMD]]

SDL:  runs the Linux build headless (SDL_VIDEODRIVER=dummy) with -n 1 -c capture.bmp and compares
      the rendered capture with the reference frame at scales 1 and 2.
DOS:  runs VANPLAY.EXE paused (-p -s N) in dosbox-x on Xvfb :99, screenshots, and compares; also
      checks that Right and Home keys step frames.  Covers mode 13h, VESA 0x100/0x101/0x107 (banked),
      PC and WN files, and centring.
"""
import argparse
import os
import random
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from imgio import read_png  # noqa: E402
from flc import Flc  # noqa: E402
import flcenc  # noqa: E402
from vanfile import VanFile, VanWriter, testpattern_frame, testpattern_palette  # noqa: E402

ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
VANPLAY = [os.path.join(SRC, "vanplay")]   # SDL build under test; set by --exe/--wrapper
NAME = "sdl"
DROP_ENV = ()
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
    env = {k: v for k, v in os.environ.items() if k not in DROP_ENV}
    env["SDL_VIDEODRIVER"] = "dummy"
    # read-ahead ring: play a run of frames through budgets of 0, 2 slots, 5 slots and all frames,
    # wrapping past the end; the last frame shown must be exact
    v = VanFile(files["T640"])
    slot_kb = (v.frame_size + 1023) // 1024
    for kb in (0, 2 * slot_kb, 5 * slot_kb, 8192):
        start, count = 2, 7                                     # frames 2..8 then wraps (4 frames total)
        last = (start + count - 1) % v.frame_count
        cap = os.path.join(work, f"ring_{kb}.bmp")
        r = subprocess.run([*VANPLAY, files["T640"], "-s", str(start), "-n", str(count), "-r", "60", "-b", str(kb),
                            "-x", "1", "-c", cap], env=env, capture_output=True, text=True, timeout=120)
        ok = r.returncode == 0 and os.path.exists(cap)
        if ok:
            _, _, px = read_bmp32(cap)
            ok = compare(v, last, px, 0, 0, 1)
        check(ok, f"{NAME} read-ahead budget {kb} KB: frames {start}..{start + count - 1} (wrapping), last exact")
    r = subprocess.run([*VANPLAY, "--help"], env=env, capture_output=True, text=True, timeout=60)
    words = [w for w in ("DOS", "Linux", "Windows", "XMS") if w in r.stdout]
    check(r.returncode == 0 and "-x SCALE" in r.stdout and "-m MODE" not in r.stdout and not words,
          f"{NAME} help shows only this system's options" + (f" (mentions {words})" if words else ""))
    r = subprocess.run([*VANPLAY, "-modes"], env=env, capture_output=True, text=True, timeout=60)
    check(r.returncode == 0 and all(m in r.stdout for m in ("0x13", "0x100", "0x101", "0x103", "0x105", "0x107")),
          f"{NAME} -modes lists the VistaPro modes")
    for name, path in files.items():
        v = VanFile(path)
        for frame, scale in ((v.frame_count - 1, 1), (1, 2)):
            cap = os.path.join(work, f"{name}_{frame}_{scale}.bmp")
            r = subprocess.run([*VANPLAY, path, "-s", str(frame), "-n", "1", "-x", str(scale), "-c", cap],
                               env=env, capture_output=True, text=True, timeout=120)
            ok = r.returncode == 0 and os.path.exists(cap)
            if ok:
                cw, ch, px = read_bmp32(cap)
                ok = (cw, ch) == (v.width * scale, v.height * scale) and compare(v, frame, px, 0, 0, scale)
            check(ok, f"{NAME} {name} frame {frame} scale {scale}")


def _disp(v6):
    return (v6 << 2) | (v6 >> 4)


def make_anim(path, w, h, kinds, fli=False, pal_events=None, stamp=False, seed=1):
    """Write a test FLI/FLC; returns (frames, palette per frame as displayed RGB tuples).
    kinds: encoding of each frame, then of the ring frame.  pal_events: {frame: palette chunks}."""
    rng = random.Random(seed)
    n = len(kinds) - 1
    frames = []
    for i in range(n):
        if kinds[i] == "black":
            f = bytearray(w * h)
        elif i == 0 or kinds[i - 1] in ("black", "copy", "brun"):
            f = bytearray((x // 8 + y // 8 + 3 * (x // 40)) & 0xFF for y in range(h) for x in range(w))
        else:
            f = bytearray(frames[-1])
        if kinds[i] != "black":
            y0 = rng.randrange(h // 2)
            for y in range(y0, y0 + h // 4):                  # a band of rows changes; the rest do not
                for x in range(rng.randrange(w // 2), w, 3):
                    f[y * w + x] = rng.randrange(256)
            f[(h - 1) * w + w - 1] = rng.randrange(256)       # the odd-width last pixel
        frames.append(bytes(f))
    pal = [(0, 0, 0)] * 256
    pals = []
    plan = []
    # some files hold 8-bit levels in COLOR_64 chunks: once one chunk has a level above 63, all do
    wide64 = any(max(c) > 63 for chunks in (pal_events or {}).values() for (ck, _, rgb) in chunks
                 if ck == "64" for c in rgb)
    for i, kind in enumerate(kinds):
        step = {"kind": kind, "stamp": stamp and i == 0}
        step["pal"] = (pal_events or {}).get(i, [])
        for (ck, start, rgb) in step["pal"]:
            for j, c in enumerate(rgb):
                v6 = [(x >> 2) if ck == "256" or wide64 else x for x in c]
                pal = pal[:start + j] + [tuple(_disp(x) for x in v6)] + pal[start + j + 1:]
        plan.append(step)
        if i < n:
            pals.append(list(pal))
    flcenc.build(path, w, h, frames, plan, fli)
    return frames, pals


def ramp(k):
    return [((i * k) & 255, (i * 3) & 255, 255 - i) for i in range(256)]


def anim_cases(work):
    rng = random.Random(7)
    cases = []
    def add(name, w, h, kinds, fli=False, ev=None, stamp=False):
        path = os.path.join(work, name.replace(" ", "_") + (".FLI" if fli else ".FLC"))
        frames, pals = make_anim(path, w, h, kinds, fli, ev, stamp, seed=len(cases) + 1)
        cases.append((name, path, w, h, frames, pals))
    add("FLC BRUN+SS2, palette changes", 320, 200, ["brun"] + ["ss2"] * 5 + ["ss2"],
        ev={0: [("256", 0, ramp(1))], 3: [("256", 20, [(255, 0, 0)] * 30)],
            6: [("256", 20, ramp(1)[20:50])]})        # the ring frame puts the palette back
    add("FLC COPY+SS2, odd 201x151, stamp chunk", 201, 151, ["copy"] + ["ss2"] * 4 + ["ss2"],
        ev={0: [("256", 0, ramp(2))]}, stamp=True)
    add("FLI BRUN+LC with a BLACK frame", 320, 200, ["brun", "lc", "black", "lc", "copy", "lc", "lc"],
        fli=True, ev={0: [("64", 0, [(i % 64, (i * 5) % 64, 63 - i % 64) for i in range(256)])]})
    add("FLI with 8-bit levels in COLOR_64 chunks", 320, 200, ["brun", "lc", "lc", "lc", "lc"], fli=True,
        ev={0: [("64", 0, ramp(1))], 2: [("64", 10, [(8, 16, 32)] * 5)], 4: [("64", 0, ramp(1))]})
    add("FLC BLACK first, then LC and SS2", 320, 200, ["black", "lc", "ss2", "lc", "ss2"],
        ev={0: [("256", 0, ramp(3))], 2: [("256", 100, ramp(5)[:100])]})
    add("FLC 640x480 BRUN+SS2", 640, 480, ["brun", "ss2", "ss2", "ss2"], ev={0: [("256", 0, ramp(1))]})
    return cases


def vantool_cmd():
    """vantool to make FLC/FLI test files with: the one next to the vanplay under test, or None."""
    if len(VANPLAY) == 1 and VANPLAY[0] == os.path.join(SRC, "vanplay"):
        exe = os.path.join(SRC, "vantool")
        return [exe] if os.path.exists(exe) else None
    here = os.path.dirname(VANPLAY[-1])
    for name in ("vantool", "vantool.exe"):
        if os.path.exists(os.path.join(here, name)):
            return VANPLAY[:-1] + [os.path.join(here, name)]
    return None


def flc_tests(files, work):
    env = {k: v for k, v in os.environ.items() if k not in DROP_ENV}
    env["SDL_VIDEODRIVER"] = "dummy"
    n = [0]

    def shown(path, *args):
        """Play with -c and return (rc, width, height, px) of the capture."""
        n[0] += 1
        cap = os.path.join(work, f"flc_{n[0]}.bmp")
        r = subprocess.run([*VANPLAY, path, *args, "-x", "1", "-c", cap], env=env, capture_output=True,
                           text=True, timeout=120)
        if r.returncode != 0 or not os.path.exists(cap):
            return r.returncode, 0, 0, None
        return (r.returncode, *read_bmp32(cap))

    def exact(path, w, h, frame_pix, pal, *args):
        rc, cw, ch, px = shown(path, *args)
        return (rc == 0 and (cw, ch) == (w, h) and
                all(pal[frame_pix[y * w + x]] == px(x, y) for y in range(h) for x in range(w)))

    for name, path, w, h, frames, pals in anim_cases(work):
        c = len(frames)
        # the decoder agrees with the independent Python one, so the test data means what it should
        ref = [px for _, px in Flc(path).iter_frames()]
        check(ref[:c] == frames and ref[c] == frames[0], f"{NAME} {name}: test file decodes as built")
        ok = all(exact(path, w, h, frames[f], pals[f], "-s", str(f), "-n", "1") for f in range(c))
        check(ok, f"{NAME} {name}: all {c} frames exact (picture and palette)")
        check(exact(path, w, h, frames[0], pals[0], "-s", str(c - 1), "-n", "2", "-r", "60"),
              f"{NAME} {name}: last frame loops to the first by the ring frame")
        f = (c - 2) % c
        last = (f + 4) % c
        check(exact(path, w, h, frames[last], pals[last], "-s", str(f), "-n", "5", "-r", "60"),
              f"{NAME} {name}: plays on after the ring frame (frames {f}..{f + 4} wrapping)")

    vt = vantool_cmd()
    if vt and "T320" in files:
        v = VanFile(files["T320"])
        for ext in ("FLC", "FLI"):
            path = os.path.join(work, "T320." + ext)
            r = subprocess.run([*vt, "flc", files["T320"], path, "-r", "10"], env=env, capture_output=True, text=True)
            ok = r.returncode == 0 and os.path.exists(path)
            if ok:
                dp = dac(v)
                ok = all(exact(path, v.width, v.height, v.read_frame(f), dp, "-s", str(f), "-n", "1")
                         for f in (0, 3, v.frame_count - 1))
            check(ok, f"{NAME} {ext} written by vantool flc plays the same as the .VAN")
        r = subprocess.run([*VANPLAY, os.path.join(work, "T320.FLC"), "-p", "-n", "1", "-v"], env=env,
                           capture_output=True, text=True, timeout=60)
        check(r.returncode == 0 and "FLC decoded as played" in r.stdout and "asked 10)" in r.stdout,
              f"{NAME} FLC plays at the speed in its header")
    else:
        print("SKIP  vantool flc round trip (no vantool next to the player)")

    # real files, if FLC_SAMPLES names a directory of them (they are not part of the repository)
    samples = os.environ.get("FLC_SAMPLES")
    if samples and os.path.isdir(samples):
        for fn in sorted(os.listdir(samples)):
            if not fn.lower().endswith((".fli", ".flc")):
                continue
            path = os.path.join(samples, fn)
            ref = Flc(path)
            c = ref.frames
            want = sorted({0, 1, c // 2, c - 1})
            decoded = {i: (pal, px) for i, (pal, px) in enumerate(ref.iter_frames()) if i in want or i == c}
            ok = True
            for f in want:
                pal, px = decoded[f]
                rgb = [tuple(_disp(pal[i * 3 + k] >> 2) for k in range(3)) for i in range(256)]
                ok = ok and exact(path, ref.width, ref.height, px, rgb, "-s", str(f), "-n", "1")
            pal, px = decoded[0]
            rgb = [tuple(_disp(pal[i * 3 + k] >> 2) for k in range(3)) for i in range(256)]
            ok = ok and exact(path, ref.width, ref.height, px, rgb, "-s", str(c - 1), "-n", "2", "-r", "60")
            check(ok, f"{NAME} real file {fn} ({ref.width}x{ref.height}, {c} frames): frames {want} and the loop exact")

    # damaged files must be refused or played without crashing
    path = anim_cases(work)[0][1]
    data = open(path, "rb").read()
    rng = random.Random(3)
    bad = []
    for i in range(40):
        d = bytearray(data[:rng.choice((len(data), len(data), rng.randrange(1, len(data))))])
        for _ in range(rng.randrange(1, 12)):
            d[rng.randrange(len(d))] = rng.randrange(256)
        bad.append(bytes(d))
    crashed = 0
    for i, d in enumerate(bad):
        p = os.path.join(work, "bad.flc")
        open(p, "wb").write(d)
        r = subprocess.run([*VANPLAY, p, "-n", "8", "-r", "60"], env=env, capture_output=True, text=True, timeout=60)
        if r.returncode not in (0, 1):
            crashed += 1
    check(crashed == 0, f"{NAME} 40 damaged FLC files do not crash the player")


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
    p.add_argument("--exe", help="SDL vanplay build to test instead of src/vanplay")
    p.add_argument("--wrapper", default="", help="command to run --exe with, e.g. wine")
    a = p.parse_args()
    if a.exe:
        global VANPLAY, NAME, DROP_ENV
        VANPLAY = shlex.split(a.wrapper) + [os.path.abspath(a.exe)]
        NAME = os.path.basename(os.path.dirname(VANPLAY[-1])) or "exe"
        DROP_ENV = ("TMPDIR",)   # Wine 10 (Ubuntu 10.0~repack) aborts whenever TMPDIR is set
    work = tempfile.mkdtemp(prefix="vanplay_test_")
    try:
        files = make_files(work)
        if not a.no_sdl:
            sdl_tests(files, work)
            flc_tests(files, work)
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
