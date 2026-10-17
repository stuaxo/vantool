#!/usr/bin/env python3
"""Regression tests for src/vantool (Linux and DOS builds) against the Python reference.

  test_vantool.py [--no-dos] [--no-van2flc] [--exe PATH [--wrapper CMD]]

1. Python writes test-pattern VANs (WN/PC, several sizes).
2. Linux vantool (or another build given with --exe, e.g. a Windows one run with --wrapper wine): info, extract (BMP and PCX; pixels and palette checked independently),
   make back from BMP/PCX (must be byte-identical to the source VAN), slice, repair.
   flc: every frame (and the loop-back frame) decoded by tools/flc.py and, if installed, ffmpeg.
3. DOS VANTOOL.EXE in dosemu2: the same commands; outputs must match the Linux build byte for byte.
   Also reads and writes long filenames (dosemu2 provides the DOS long filename API).
4. VAN2FLC end-to-end on the VANs vantool made.
"""
import argparse
import random
import filecmp
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vanfile import VanFile, VanWriter, testpattern_frame, testpattern_palette  # noqa: E402
from flc import Flc  # noqa: E402

ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
LINUX_BIN = os.path.join(SRC, "vantool")
VANTOOL = [LINUX_BIN]   # command for the build under test; set by --exe/--wrapper
NAME = "linux"
ENV = None              # environment for VANTOOL; None inherits ours
DOS_EXE = os.path.join(SRC, "VANTOOL.EXE")
DOSEMU = ["dosemu", "-dumb", "-td", "-I", '$_cpu_vm = "emulated"', "-I", '$_cpu_vm_dpmi = "emulated"',
          "-I", "$_sound = (off)"]

# name, size, frames, variant  (8.3-safe names for DOS)
CASES = [("A320", (320, 200), 4, "WN"), ("B320PC", (320, 200), 3, "PC"),
         ("C400", (400, 300), 3, "WN"), ("D640", (640, 480), 2, "WN"), ("E33", (33, 7), 3, "WN")]

failures = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


def run(args, cwd):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, env=ENV)


def read_bmp(path):
    d = open(path, "rb").read()
    off, w, h = struct.unpack_from("<I", d, 10)[0], *struct.unpack_from("<ii", d, 18)
    pal = b"".join(bytes((d[54 + i * 4 + 2], d[54 + i * 4 + 1], d[54 + i * 4])) for i in range(256))
    stride = (w + 3) & ~3
    rows = [d[off + y * stride: off + y * stride + w] for y in range(abs(h))]
    return w, abs(h), pal, b"".join(reversed(rows) if h > 0 else rows)


def read_pcx(path):
    d = open(path, "rb").read()
    w = struct.unpack_from("<H", d, 8)[0] + 1
    h = struct.unpack_from("<H", d, 10)[0] + 1
    bpl = struct.unpack_from("<H", d, 66)[0]
    out, p = bytearray(), 128
    while len(out) < bpl * h:
        c = d[p]; p += 1
        if c >= 0xC0:
            out += bytes([d[p]]) * (c & 0x3F); p += 1
        else:
            out.append(c)
    pix = b"".join(out[y * bpl:y * bpl + w] for y in range(h))
    assert d[-769] == 0x0C
    return w, h, d[-768:], pix


FFMPEG = shutil.which("ffmpeg")


def check_flc(out, van, fli, what):
    """Decode out with tools/flc.py (and ffmpeg) and compare every frame, plus the loop-back frame."""
    v = VanFile(van)
    want = [v.read_frame(i) for i in range(v.frame_count)] + [v.read_frame(0)]
    pal8 = bytes((c << 2) | (c >> 4) for c in v.palette6)
    try:
        fl = Flc(out)
        got = list(fl.iter_frames())
    except (OSError, ValueError, IndexError, struct.error):
        check(False, what)
        return
    pal = bytes(c << 2 for c in v.palette6) if fli else pal8          # flc.py scales 6-bit by 4
    ok = (fl.magic == (0xAF11 if fli else 0xAF12) and fl.frames == v.frame_count and len(got) == len(want)
          and all(p == pal and px == w for (p, px), w in zip(got, want)))
    if ok and FFMPEG:
        r = subprocess.run([FFMPEG, "-v", "error", "-i", out, "-fps_mode", "passthrough", "-f", "rawvideo",
                            "-pix_fmt", "rgb24", "-"], capture_output=True)
        size = v.width * v.height * 3
        ok = r.returncode == 0 and all(
            r.stdout[i * size:(i + 1) * size] == b"".join(pal8[p * 3:p * 3 + 3] for p in want[i])
            for i in range(v.frame_count))
    check(ok, what + (" (and ffmpeg)" if FFMPEG else ""))


def flc_tests(work):
    """Awkward content for the FLC/FLI encoder: repeated frames, a change in only the last pixel of an
    odd-width line, changes far apart, every pixel changing, a single frame, and an odd-width line
    needing more than 255 LC packets."""
    d = os.path.join(work, "flc")
    os.makedirs(d)
    rnd = random.Random(1)

    def van(name, w, h, frames):
        path = os.path.join(d, name)
        out = VanWriter(path, w, h, testpattern_palette(), 0x13, "WN")
        for f in frames:
            out.write_frame(bytes(f))
        out.close()
        return path

    w, h = 321, 120
    base = bytearray(rnd.randrange(256) for _ in range(w * h))
    last = bytearray(base)
    last[w * 5 - 1] ^= 1
    apart = bytearray(last)
    for i in (10, w * 60 + 2, w * 119 + 300):
        apart[i] ^= 9
    noise = bytearray(rnd.randrange(256) for _ in range(w * h))
    flat = bytearray([7]) * (w * h)
    wide = bytearray(rnd.randrange(256) for _ in range(1281 * 20))
    wide2 = bytearray(wide)
    for i in range(0, len(wide2), 4):
        wide2[i] ^= 0x55
    cases = [("edge.van", van("edge.van", w, h, [base, base, last, apart, noise, flat, flat]), False),
             ("even.van", van("even.van", 320, 200, [testpattern_frame(320, 200, i, 3) for i in range(3)]
                                                        + [testpattern_frame(320, 200, 0, 3)]), False),
             ("one.van", van("one.van", 320, 200, [testpattern_frame(320, 200, 0, 1)]), True),
             ("wide.van", van("wide.van", 1281, 20, [wide, wide2, wide]), False)]
    for name, path, fli in cases:
        for ext in (".FLC", ".FLI") if fli else (".FLC",):
            out = path[:-4] + ext
            r = run([*VANTOOL, "flc", path, out], d)
            check(r.returncode == 0, f"{NAME} flc {name} -> {ext}")
            check_flc(out, path, ext == ".FLI", f"{NAME} flc {name} -> {ext}: all frames decode exactly")
    r = run([*VANTOOL, "flc", os.path.join(d, "edge.van"), os.path.join(d, "edge.fli")], d)
    check(r.returncode == 1 and "320x200" in r.stderr, f"{NAME} flc refuses FLI that is not 320x200")


def make_inputs(work):
    for name, (w, h), n, var in CASES:
        out = VanWriter(os.path.join(work, name + ".VAN"), w, h, testpattern_palette(), 0x13, var)
        for i in range(n):
            out.write_frame(testpattern_frame(w, h, i, n))
        out.close()


def linux_tests(work):
    r = run([*VANTOOL], work)                  # no arguments: usage
    words = [w for w in ("DOS", "Linux", "Windows") if w in r.stdout]
    check(r.returncode == 2 and "vantool extract" in r.stdout and not words,
          f"{NAME} help has no instructions for other systems" + (f" (mentions {words})" if words else ""))
    for name, (w, h), n, var in CASES:
        src = os.path.join(work, name + ".VAN")
        v = VanFile(src)
        pal8 = bytes((c << 2) | (c >> 4) for c in v.palette6)
        r = run([*VANTOOL, "info", src], work)
        check(r.returncode == 0 and f"{w} x {h}" in r.stdout and f"frames        {n}" in r.stdout,
              f"{NAME} info {name}")
        for kind in ("bmp", "pcx"):
            pre = name[:2] + kind[0].upper()
            r = run([*VANTOOL, "extract", src, pre, "-t", kind], work)
            check(r.returncode == 0, f"{NAME} extract {name} {kind}")
            files = [os.path.join(work, f"{pre}{i:04d}.{kind.upper()}") for i in range(n)]
            ok = True
            for i, fpath in enumerate(files):
                iw, ih, ipal, ipix = (read_bmp if kind == "bmp" else read_pcx)(fpath)
                ok &= (iw, ih) == (w, h) and ipal == pal8 and ipix == v.read_frame(i)
            check(ok, f"{NAME} extract {name} {kind}: pixels+palette match reference")
            out = os.path.join(work, f"{name[:4]}{kind[0]}.VAN")
            args = [*VANTOOL, "make", out, "-m", "0x13"] + (["-pc"] if var == "PC" else []) + files
            r = run(args, work)
            check(r.returncode == 0 and filecmp.cmp(out, src, shallow=False),
                  f"{NAME} make from {kind} {name}: byte-identical to source")
        for ext in (".FLC", ".FLI") if (w, h) == (320, 200) else (".FLC",):
            out = os.path.join(work, name[:6] + ext)
            r = run([*VANTOOL, "flc", src, out, "-r", "15"], work)
            check(r.returncode == 0, f"{NAME} flc {name} -> {ext}")
            check_flc(out, src, ext == ".FLI", f"{NAME} flc {name} -> {ext}: all frames decode exactly")
        sl = os.path.join(work, name[:4] + "S.VAN")
        r = run([*VANTOOL, "slice", src, sl, "1", "2"], work)
        v2 = VanFile(sl)
        check(r.returncode == 0 and v2.frame_count == min(2, n - 1) and v2.read_frame(0) == v.read_frame(1),
              f"{NAME} slice {name}")
    # pattern input, palette mismatch, repair
    r = run([*VANTOOL, "make", "PAT.VAN", "-p", "A3B%04d.BMP", "-n", "4", "-m", "0x13"], work)
    check(r.returncode == 0 and filecmp.cmp(os.path.join(work, "PAT.VAN"), os.path.join(work, "A320.VAN"), False),
          f"{NAME} make -p pattern")
    shutil.copy(os.path.join(work, "A320.VAN"), os.path.join(work, "R.VAN"))
    r = run([*VANTOOL, "repair", "R.VAN", "-pc"], work)
    check(r.returncode == 0 and VanFile(os.path.join(work, "R.VAN")).variant == "PC", f"{NAME} repair -pc")
    r = run([*VANTOOL, "info", "NOSUCH.VAN"], work)
    check(r.returncode == 1 and "cannot open" in r.stderr, f"{NAME} error on missing file")


def dos_tests(work):
    """Run the same commands with VANTOOL.EXE in one dosemu session; compare with the Linux outputs."""
    d = os.path.join(work, "dos")
    os.makedirs(d)
    shutil.copy(DOS_EXE, d)
    for name, *_ in CASES:
        shutil.copy(os.path.join(work, name + ".VAN"), d)
    lines = []
    for name, (w, h), n, var in CASES:
        lines.append(f"VANTOOL info {name}.VAN > {name[:6]}.INF")
        for kind in ("bmp", "pcx"):
            pre = name[:2] + kind[0].upper()
            lines.append(f"VANTOOL extract {name}.VAN {pre} -t {kind}")
            files = " ".join(f"{pre}{i:04d}.{kind.upper()}" for i in range(n))
            lines.append(f"VANTOOL make {name[:4]}{kind[0]}.VAN -m 0x13 {'-pc ' if var == 'PC' else ''}{files}")
        lines.append(f"VANTOOL slice {name}.VAN {name[:4]}S.VAN 1 2")
        for ext in (".FLC", ".FLI") if (w, h) == (320, 200) else (".FLC",):
            lines.append(f"VANTOOL flc {name}.VAN {name[:6]}{ext} -r 15")
    lines.append("VANTOOL /? > HELP.TXT")
    lines.append("VANTOOL make PAT.VAN -p A3B%%04d.BMP -n 4 -m 0x13")  # %% for COMMAND.COM
    shutil.copy(os.path.join(work, "A320.VAN"), os.path.join(d, "Long Source Name.van"))
    lines.append('VANTOOL info "Long Source Name.van" > LONGNAME.INF')
    lines.append('VANTOOL slice "Long Source Name.van" "Long Slice Output.van" 1 2')
    open(os.path.join(d, "T.BAT"), "w").write("\r\n".join(lines) + "\r\n")
    r = subprocess.run(DOSEMU + ["--Fdrive_c", d, "-E", "T.BAT"], capture_output=True, text=True,
                       errors="replace", timeout=300, stdin=subprocess.DEVNULL)
    produced = {f.upper(): os.path.join(d, f) for f in os.listdir(d)}
    for f in sorted(os.listdir(work)):
        if f.upper().endswith((".BMP", ".PCX", ".FLC", ".FLI")) or (f.upper().endswith(".VAN") and f.upper() not in
                                                    {c[0] + ".VAN" for c in CASES} | {"R.VAN"}):
            other = produced.get(f.upper())
            check(other is not None and filecmp.cmp(os.path.join(work, f), other, shallow=False),
                  f"dos output {f} identical to {NAME}")
    for name, *_ in CASES:
        inf = produced.get(name[:6].upper() + ".INF")
        lin = run([*VANTOOL, "info", name + ".VAN"], work).stdout.replace("\n", "\r\n")
        dos = open(inf, newline="").read() if inf else ""
        check(dos.split("\r\n")[1:] == lin.split("\r\n")[1:], f"dos info {name} matches {NAME}")
    hlp = produced.get("HELP.TXT")
    check(hlp is not None and "keep PREFIX to 4 characters" in open(hlp, errors="replace").read(),
          "dos help has the DOS-only note")
    inf = produced.get("LONGNAME.INF")
    check(inf is not None and "Long Source Name.van" in open(inf).read(), "dos info reads a long filename")
    out = os.path.join(d, "Long Slice Output.van")
    check(os.path.exists(out) and filecmp.cmp(out, os.path.join(work, "A320S.VAN"), shallow=False),
          "dos slice writes a long filename")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--no-dos", action="store_true")
    p.add_argument("--no-van2flc", action="store_true")
    p.add_argument("--keep", action="store_true", help="keep the work directory")
    p.add_argument("--exe", help="vantool build to test instead of src/vantool")
    p.add_argument("--wrapper", default="", help="command to run --exe with, e.g. wine")
    a = p.parse_args()
    if a.exe:
        global VANTOOL, NAME, ENV
        VANTOOL = shlex.split(a.wrapper) + [os.path.abspath(a.exe)]
        # Wine 10 (Ubuntu 10.0~repack) aborts with "free(): invalid pointer" whenever TMPDIR is set
        ENV = {k: v for k, v in os.environ.items() if k != "TMPDIR"}
        NAME = os.path.basename(os.path.dirname(VANTOOL[-1])) or "exe"
    work = tempfile.mkdtemp(prefix="vantool_test_")
    try:
        make_inputs(work)
        linux_tests(work)
        flc_tests(work)
        if not a.no_dos:
            dos_tests(work)
        van2flc = os.path.join(os.environ.get("VISTAPRO", os.path.join(ROOT, "vistapro")), "VAN2FLC.EXE")
        if not a.no_van2flc and not os.path.exists(van2flc):
            print("SKIP  van2flc e2e: VAN2FLC.EXE not found (set VISTAPRO to a VistaPro 3.20 directory)")
        elif not a.no_van2flc:
            made = [os.path.join(work, f) for f in sorted(os.listdir(work)) if f.endswith("b.VAN")]
            r = subprocess.run([sys.executable, os.path.join(HERE, "e2e_van2flc.py"), "--van2flc", van2flc] + made,
                               capture_output=True, text=True)
            for line in r.stdout.splitlines():
                check(line.startswith("PASS"), "van2flc e2e " + line[6:])
    finally:
        if a.keep:
            print("work dir:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print(f"\n{'ALL PASSED' if not failures else str(len(failures)) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
