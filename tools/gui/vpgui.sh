#!/bin/bash
# Drive VP3 in dosbox-x on Xvfb :99. Coordinates are DOS-screen pixels (640x480).
# The dosbox-x window sits at root 192,184 with a 17px menubar; override with VPGUI_X/VPGUI_Y.
#   vpgui.sh shot OUT.png
#   vpgui.sh click X Y [wait]          click (buttons, list items, dialog buttons)
#   vpgui.sh menu MX MY IX IY [wait]   pull-down: press on menu MX,MY, drag to item IX,IY, release
#   vpgui.sh key KEYSYM...             e.g. Escape
# Each action except shot also writes $VPGUI_SHOT (default ./cur.png).
# Gotchas: kill dosbox-x with `pkill -9 -x dosbox-x` (SIGTERM opens a confirm dialog; never pkill -f).
# Dialogs move around, so always screenshot before clicking.
OX=${VPGUI_X:-192}; OY=$(( ${VPGUI_Y:-184} + 17 )); OUT=${VPGUI_SHOT:-cur.png}
HERE=$(dirname "$0")
export DISPLAY=:99
shot() { xwd -root -silent | python3 -I "$HERE/shot.py" "$1" $OX,$OY,640,480; }
cmd=$1; shift
case $cmd in
  shot)  shot "$1"; exit ;;
  click) xdotool mousemove $(($1+OX)) $(($2+OY)) sleep 0.4 mousedown 1 sleep 0.3 mouseup 1; sleep ${3:-1.5} ;;
  menu)  xdotool mousemove $(($1+OX)) $(($2+OY)) sleep 0.4 mousedown 1 sleep 0.8 \
                 mousemove $(($3+OX)) $(($4+OY)) sleep 0.6 mouseup 1; sleep ${5:-2} ;;
  key)   xdotool key "$@"; sleep 1.5 ;;
  *) echo "unknown command $cmd" >&2; exit 2 ;;
esac
shot "$OUT"
