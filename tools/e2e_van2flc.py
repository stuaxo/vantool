#!/usr/bin/env python3
"""End-to-end check of VAN files against the original VAN2FLC.EXE (no screen capture).

  e2e_van2flc.py FILE.VAN... [--van2flc PATH] [--flags "+b"]

For each file: copy it with VAN2FLC.EXE to a temp DOS drive, run VAN2FLC headless in
dosemu2, decode the FLC with tools/flc.py, and require that every frame, the
loop-back frame and the palette match what tools/vanfile.py reads.  Exit status 0 means
all files passed.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flc import Flc
from vanfile import VanFile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_VAN2FLC = os.path.join(os.environ.get("VISTAPRO", os.path.join(HERE, "..", "vistapro")), "VAN2FLC.EXE")
DOSEMU = ["dosemu", "-dumb", "-td", "-I", '$_cpu_vm = "emulated"', "-I", '$_cpu_vm_dpmi = "emulated"',
          "-I", "$_sound = (off)"]


def run_van2flc(van_path, exe, flags):
    drive = tempfile.mkdtemp(prefix="e2e_")
    try:
        shutil.copy(exe, os.path.join(drive, "VAN2FLC.EXE"))
        shutil.copy(van_path, os.path.join(drive, "IN.VAN"))
        cmd = f"VAN2FLC.EXE IN.VAN OUT.FLC {flags}".strip()
        r = subprocess.run(DOSEMU + ["--Fdrive_c", drive, "-E", cmd], capture_output=True, text=True,
                           errors="replace", timeout=900, stdin=subprocess.DEVNULL)
        out = [f for f in os.listdir(drive) if f.lower() == "out.flc"]
        log = r.stdout[r.stdout.find("VAN2FLC Version"):].strip()
        if not out:
            return None, log
        dst = tempfile.mktemp(suffix=".flc")
        shutil.move(os.path.join(drive, out[0]), dst)
        return dst, log
    finally:
        shutil.rmtree(drive, ignore_errors=True)


def check(van_path, exe, flags):
    v = VanFile(van_path)
    note = ""
    if v.width % 2 and "+b" not in flags.lower():
        # VAN2FLC 3.10's SS2 encoder never updates the last column of odd-width images
        # (no 0x8000 last-pixel words); BRUN-only output is correct.  See docs/VAN_SPEC.md 9.2.
        flags = (flags + " +b").strip()
        note = " [odd width: used +b]"
    flc_path, log = run_van2flc(van_path, exe, flags)
    if not flc_path:
        return False, "VAN2FLC produced no FLC:\n" + log
    try:
        f = Flc(flc_path)
        problems = []
        if (f.width, f.height) != (v.width, v.height):
            problems.append(f"size {f.width}x{f.height} != {v.width}x{v.height}")
        if f.frames != v.frame_count:
            problems.append(f"FLC frames {f.frames} != VAN frames {v.frame_count}")
        n = 0
        for n, (pal, pix) in enumerate(f.iter_frames()):
            ref = v.read_frame(n % v.frame_count)  # last FLC frame loops back to frame 0
            if pal != v.palette8:
                problems.append(f"frame {n}: palette differs")
            if pix != ref:
                bad = sum(a != b for a, b in zip(pix, ref))
                problems.append(f"frame {n}: {bad} pixels differ")
        checked = n + 1
    finally:
        os.unlink(flc_path)
    if problems:
        return False, "; ".join(problems[:5])
    return True, (f"{v.width}x{v.height} VANIM{v.variant}, {v.frame_count} frames + ring frame checked "
                  f"({checked} FLC frames){note}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("files", nargs="+")
    p.add_argument("--van2flc", default=DEFAULT_VAN2FLC)
    p.add_argument("--flags", default="", help='VAN2FLC switches, e.g. "+b" or "+s"')
    a = p.parse_args()
    ok_all = True
    for path in a.files:
        ok, msg = check(path, a.van2flc, a.flags)
        ok_all &= ok
        print(f"{'PASS' if ok else 'FAIL'}  {os.path.basename(path)}  {msg}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
