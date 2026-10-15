#!/usr/bin/env python3
"""Regression tests for src/vantool (Linux and DOS builds) against the Python reference.

  test_vantool.py [--no-dos] [--no-van2flc]

1. Python writes test-pattern VANs (WN/PC, several sizes).
2. Linux vantool: info, extract (BMP and PCX; pixels and palette checked independently),
   make back from BMP/PCX (must be byte-identical to the source VAN), slice, repair.
3. DOS VANTOOL.EXE in dosemu2: the same commands; outputs must match the Linux build byte for byte.
4. VAN2FLC end-to-end on the VANs vantool made.
"""
import argparse
import filecmp
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vanfile import VanFile, VanWriter, testpattern_frame, testpattern_palette  # noqa: E402

ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "src")
LINUX_BIN = os.path.join(SRC, "vantool")
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
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True)


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


def make_inputs(work):
    for name, (w, h), n, var in CASES:
        out = VanWriter(os.path.join(work, name + ".VAN"), w, h, testpattern_palette(), 0x13, var)
        for i in range(n):
            out.write_frame(testpattern_frame(w, h, i, n))
        out.close()


def linux_tests(work):
    for name, (w, h), n, var in CASES:
        src = os.path.join(work, name + ".VAN")
        v = VanFile(src)
        pal8 = bytes((c << 2) | (c >> 4) for c in v.palette6)
        r = run([LINUX_BIN, "info", src], work)
        check(r.returncode == 0 and f"{w} x {h}" in r.stdout and f"frames        {n}" in r.stdout,
              f"linux info {name}")
        for kind in ("bmp", "pcx"):
            pre = name[:2] + kind[0].upper()
            r = run([LINUX_BIN, "extract", src, pre, "-t", kind], work)
            check(r.returncode == 0, f"linux extract {name} {kind}")
            files = [os.path.join(work, f"{pre}{i:04d}.{kind.upper()}") for i in range(n)]
            ok = True
            for i, fpath in enumerate(files):
                iw, ih, ipal, ipix = (read_bmp if kind == "bmp" else read_pcx)(fpath)
                ok &= (iw, ih) == (w, h) and ipal == pal8 and ipix == v.read_frame(i)
            check(ok, f"linux extract {name} {kind}: pixels+palette match reference")
            out = os.path.join(work, f"{name[:4]}{kind[0]}.VAN")
            args = [LINUX_BIN, "make", out, "-m", "0x13"] + (["-pc"] if var == "PC" else []) + files
            r = run(args, work)
            check(r.returncode == 0 and filecmp.cmp(out, src, shallow=False),
                  f"linux make from {kind} {name}: byte-identical to source")
        sl = os.path.join(work, name[:4] + "S.VAN")
        r = run([LINUX_BIN, "slice", src, sl, "1", "2"], work)
        v2 = VanFile(sl)
        check(r.returncode == 0 and v2.frame_count == min(2, n - 1) and v2.read_frame(0) == v.read_frame(1),
              f"linux slice {name}")
    # pattern input, palette mismatch, repair
    r = run([LINUX_BIN, "make", "PAT.VAN", "-p", "A3B%04d.BMP", "-n", "4", "-m", "0x13"], work)
    check(r.returncode == 0 and filecmp.cmp(os.path.join(work, "PAT.VAN"), os.path.join(work, "A320.VAN"), False),
          "linux make -p pattern")
    shutil.copy(os.path.join(work, "A320.VAN"), os.path.join(work, "R.VAN"))
    r = run([LINUX_BIN, "repair", "R.VAN", "-pc"], work)
    check(r.returncode == 0 and VanFile(os.path.join(work, "R.VAN")).variant == "PC", "linux repair -pc")
    r = run([LINUX_BIN, "info", "NOSUCH.VAN"], work)
    check(r.returncode == 1 and "cannot open" in r.stderr, "linux error on missing file")


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
    lines.append("VANTOOL make PAT.VAN -p A3B%%04d.BMP -n 4 -m 0x13")  # %% for COMMAND.COM
    open(os.path.join(d, "T.BAT"), "w").write("\r\n".join(lines) + "\r\n")
    r = subprocess.run(DOSEMU + ["--Fdrive_c", d, "-E", "T.BAT"], capture_output=True, text=True,
                       errors="replace", timeout=300, stdin=subprocess.DEVNULL)
    produced = {f.upper(): os.path.join(d, f) for f in os.listdir(d)}
    for f in sorted(os.listdir(work)):
        if f.upper().endswith((".BMP", ".PCX")) or (f.upper().endswith(".VAN") and f.upper() not in
                                                    {c[0] + ".VAN" for c in CASES} | {"R.VAN"}):
            other = produced.get(f.upper())
            check(other is not None and filecmp.cmp(os.path.join(work, f), other, shallow=False),
                  f"dos output {f} identical to linux")
    for name, *_ in CASES:
        inf = produced.get(name[:6].upper() + ".INF")
        lin = run([LINUX_BIN, "info", name + ".VAN"], work).stdout.replace("\n", "\r\n")
        dos = open(inf, newline="").read() if inf else ""
        check(dos.split("\r\n")[1:] == lin.split("\r\n")[1:], f"dos info {name} matches linux")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--no-dos", action="store_true")
    p.add_argument("--no-van2flc", action="store_true")
    p.add_argument("--keep", action="store_true", help="keep the work directory")
    a = p.parse_args()
    work = tempfile.mkdtemp(prefix="vantool_test_")
    try:
        make_inputs(work)
        linux_tests(work)
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
