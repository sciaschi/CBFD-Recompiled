#!/usr/bin/env python3
"""
Drives a game window from a script (X11: run the game with SDL_VIDEODRIVER=x11 under Wayland), for checking
what it draws: finds the window of the process --pid, then does each step in turn.

    xwin.py --pid <pid> [--scale 1.25] shot <file.png> | click <x> <y> | key <name> | hold <name> <seconds>
                                       | type <text> | wait <seconds> ...

Coordinates are the window's own. --scale is the desktop's scaling, by which the pointer is off under
XWayland. Keys are X keysym names (Return, space, Shift_L, a...); the game's default keyboard is Enter =
Start, Space = A, Left Shift = B, WASD the stick. Needs python-xlib, and ImageMagick's import for shots.
"""
import argparse
import subprocess
import sys
import time

from Xlib import X, XK, display
from Xlib.ext import xtest


def find_window(d, pid):
    pid_atom = d.intern_atom("_NET_WM_PID")
    found = []

    def walk(window):
        prop = window.get_full_property(pid_atom, X.AnyPropertyType)
        if prop and prop.value[0] == pid and window.get_geometry().width > 200:
            found.append(window)
        for child in window.query_tree().children:
            walk(child)

    walk(d.screen().root)
    return found[-1] if found else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("steps", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    d = display.Display()
    window = find_window(d, args.pid)
    if window is None:
        sys.exit(f"no window for process {args.pid}")
    origin = window.translate_coords(d.screen().root, 0, 0)
    left, top = -origin.x, -origin.y

    def keycode(name):
        return d.keysym_to_keycode(XK.string_to_keysym(name))

    steps = list(args.steps)
    while steps:
        step = steps.pop(0)
        if step == "shot":
            subprocess.run(["import", "-window", hex(window.id), steps.pop(0)], check=True)
        elif step == "click":
            x, y = int(steps.pop(0)), int(steps.pop(0))
            xtest.fake_input(d, X.MotionNotify, x=int((left + x) / args.scale), y=int((top + y) / args.scale))
            d.sync(); time.sleep(0.15)
            xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.08)
            xtest.fake_input(d, X.ButtonRelease, 1); d.sync(); time.sleep(0.3)
        elif step in ("key", "hold"):
            code = keycode(steps.pop(0))
            seconds = float(steps.pop(0)) if step == "hold" else 0.15
            xtest.fake_input(d, X.KeyPress, code); d.sync(); time.sleep(seconds)
            xtest.fake_input(d, X.KeyRelease, code); d.sync(); time.sleep(0.2)
        elif step == "type":
            names = {".": "period", ":": "colon", "-": "minus", " ": "space", "[": "bracketleft", "]": "bracketright"}
            for ch in steps.pop(0):
                shift = ch.isupper() or ch in ":[]"
                if shift:
                    xtest.fake_input(d, X.KeyPress, keycode("Shift_L"))
                code = keycode(names.get(ch, ch))
                xtest.fake_input(d, X.KeyPress, code)
                xtest.fake_input(d, X.KeyRelease, code)
                if shift:
                    xtest.fake_input(d, X.KeyRelease, keycode("Shift_L"))
                d.sync(); time.sleep(0.05)
        elif step == "wait":
            time.sleep(float(steps.pop(0)))
        else:
            sys.exit(f"unknown step {step}")


if __name__ == "__main__":
    main()
