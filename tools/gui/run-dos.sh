#!/bin/bash
# run-dos.sh GAMEDIR "DOS COMMAND"  -- start Xvfb :99 (if needed) and run a command in dosbox-x
# with GAMEDIR as C:.  Use a COPY of the game dir if the program writes files.
# CYCLES=fixed 100000 (etc.) for repeatable timing; the default "max" ramps up after start.
# MEMSIZE=MB, XMS=false, DOSBOX_EXTRA="-set foo=bar ..." (no spaces inside values) adjust the machine.
set -e
GAME=${1:?usage: run-dos.sh GAMEDIR "COMMAND"}; CMD=${2:-VP3}
pgrep -x Xvfb >/dev/null || { Xvfb :99 -screen 0 1600x1200x24 >/dev/null 2>&1 & sleep 1; }
cd "$GAME"
DISPLAY=:99 SDL_AUDIODRIVER=dummy exec dosbox-x -fastlaunch -nopromptfolder \
  -set "sdl output=surface" -set "dosbox machine=svga_s3" -set "cpu cycles=${CYCLES:-max}" -set "dosbox memsize=${MEMSIZE:-32}" -set "dos xms=${XMS:-true}" ${VMEMSIZE:+-set "vmemsize=$VMEMSIZE"} \
  $DOSBOX_EXTRA -c "MOUNT C ." -c "C:" -c "$CMD"
