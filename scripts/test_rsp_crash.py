#!/usr/bin/env python3
"""
Crash reproducer for RSP toolbar button DCHECK crash.

Reproduces crash4: "new RSP tab → click RSP toolbar button → Chrome crashes".

The crash: ShowWindow() → WM_ACTIVATE dispatched synchronously via
SendMessage while compositor's in_paint_layer_contents() is true →
FocusRing::RefreshLayer → cc::Layer::SetHideLayerAndSubtree →
DCHECK(IsPropertyChangeAllowed()).

Strategy: rapidly open/close the bubble repeatedly to hit the race between
ShowWindow and the compositor paint phase. 50 iterations is enough to
reliably trigger it before the fix; 0 crashes after the fix confirms it.
"""

import ctypes
import os
import re
import shutil
import subprocess
import sys
import time

# DPI awareness so UIA coordinates match physical pixels.
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

try:
    import psutil
    import win32api
    import win32con
    import win32gui
    import win32process
    from pywinauto import Desktop
except ImportError as e:
    print(f"ERROR: missing dependency ({e}). Run: pip install pywinauto psutil")
    sys.exit(1)

CHROME     = r"C:\chromium\src\out\Default\chrome.exe"
PROFILE    = r"C:\Temp\rsp_crash_test_profile"
CHROME_ARGS = [
    "--no-sandbox",
    "--no-first-run",
    "--disable-session-crashed-bubble",
    f"--user-data-dir={PROFILE}",
]

IDC_NEW_RSP_TAB = 34065

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

_chrome_pids: set[int] = set()


def refresh_chrome_pids():
    _chrome_pids.update(
        p.pid for p in psutil.process_iter(["name"])
        if p.name().lower() == "chrome.exe"
    )


def chrome_alive(proc):
    return proc.poll() is None


def kill_chrome():
    for p in psutil.process_iter(["name"]):
        try:
            if p.name().lower() == "chrome.exe":
                p.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    _chrome_pids.clear()
    time.sleep(2.0)


def find_chrome_windows_uia(timeout=60, min_count=1):
    """Wait until at least min_count Chrome windows are visible (UIA + win32 fallback)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        refresh_chrome_pids()
        found = []
        try:
            for w in Desktop(backend="uia").windows(visible_only=True):
                try:
                    if w.process_id() in _chrome_pids and w.window_text():
                        found.append(w)
                except Exception:
                    pass
        except Exception:
            pass
        if len(found) >= min_count:
            return found
        # win32 fallback
        if not found:
            try:
                win32_hwnds = []
                def _cb(hwnd, _):
                    _, pid = win32process.GetWindowThreadProcessId(hwnd)
                    if pid in _chrome_pids and win32gui.IsWindowVisible(hwnd):
                        t = win32gui.GetWindowText(hwnd)
                        if t and "Chrome_WidgetWin_1" in win32gui.GetClassName(hwnd):
                            win32_hwnds.append((hwnd, t))
                win32gui.EnumWindows(_cb, None)
                for hwnd, title in win32_hwnds:
                    try:
                        w = Desktop(backend="uia").window(handle=hwnd)
                        found.append(w)
                    except Exception:
                        pass
            except Exception:
                pass
        if len(found) >= min_count:
            return found
        time.sleep(0.5)
    return []


def find_chrome_window(timeout=60):
    wins = find_chrome_windows_uia(timeout=timeout, min_count=1)
    return wins[0] if wins else None


def find_rsp_window(timeout=15):
    """Wait for a second Chrome window (RSP window)."""
    wins = find_chrome_windows_uia(timeout=timeout, min_count=2)
    return wins[-1] if len(wins) >= 2 else None


def find_toolbar_rsp_button(window, timeout=8):
    """Find the RSP toolbar button (sibling of the Chromium app-menu button)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for ctrl in window.descendants(control_type="Button"):
                try:
                    if ctrl.window_text() == "Chromium" and ctrl.is_visible():
                        parent = ctrl.parent()
                        for sib in parent.children():
                            t = sib.window_text() or ""
                            if "RSP proxy settings" in t and sib.is_visible():
                                return sib
                except Exception:
                    pass
        except Exception:
            pass
        time.sleep(0.3)
    return None


def find_bubble_hwnd(timeout=3):
    """Find the RSP Proxy Settings bubble HWND via win32gui."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        refresh_chrome_pids()
        found = None
        def _cb(hwnd, _):
            nonlocal found
            if not win32gui.IsWindowVisible(hwnd):
                return
            _, pid = win32process.GetWindowThreadProcessId(hwnd)
            if pid in _chrome_pids:
                t = win32gui.GetWindowText(hwnd)
                cls = win32gui.GetClassName(hwnd)
                if "RSP Proxy Settings" in t and "Chrome_WidgetWin" in cls:
                    found = hwnd
        win32gui.EnumWindows(_cb, None)
        if found:
            return found
        time.sleep(0.2)
    return None


def close_bubble(bubble_hwnd):
    """Close the bubble by sending WM_CLOSE."""
    try:
        win32api.PostMessage(bubble_hwnd, win32con.WM_CLOSE, 0, 0)
    except Exception:
        pass


def run_crash_test():
    print("\n=== RSP Bubble Crash Reproducer ===")
    print("Triggers crash4: new RSP tab → click RSP toolbar button\n")

    kill_chrome()
    # Clear the profile so Chrome starts without session-restore prompts.
    shutil.rmtree(PROFILE, ignore_errors=True)
    os.makedirs(PROFILE, exist_ok=True)

    proc = subprocess.Popen([CHROME] + CHROME_ARGS)
    print(f"Chrome launched (PID {proc.pid})")

    # Wait for main window.
    main_win = find_chrome_window(timeout=60)
    if not main_win:
        print(f"[{FAIL}] Chrome window not found within 60s")
        kill_chrome()
        return False
    print(f"[{PASS}] Chrome ready: {main_win.window_text()}")

    # Open RSP tab via WM_COMMAND.
    hwnd = main_win.handle
    win32api.PostMessage(hwnd, win32con.WM_COMMAND, IDC_NEW_RSP_TAB, 0)
    time.sleep(2)

    if not chrome_alive(proc):
        print(f"[{FAIL}] Chrome crashed on IDC_NEW_RSP_TAB")
        return False

    rsp_win = find_rsp_window(timeout=15)
    if not rsp_win:
        print(f"[{FAIL}] RSP window not found")
        kill_chrome()
        return False
    print(f"[{PASS}] RSP window: {rsp_win.window_text()}")

    rsp_btn = None
    win_with_btn = None
    all_wins = find_chrome_windows_uia(timeout=5, min_count=1)
    print(f"  Found {len(all_wins)} Chrome window(s): {[w.window_text() for w in all_wins]}")
    # Try all windows — the RSP tab might be in any window.
    for win in all_wins:
        try:
            buttons = [c.window_text() for c in win.descendants(control_type="Button")
                       if c.is_visible()]
            print(f"  Buttons in '{win.window_text()}': {buttons[:10]}")
        except Exception as e:
            print(f"  Could not enumerate buttons: {e}")
        rsp_btn = find_toolbar_rsp_button(win, timeout=3)
        if rsp_btn:
            win_with_btn = win
            print(f"[{PASS}] RSP toolbar button found in: {win.window_text()}")
            break
    if not rsp_btn:
        print(f"[{FAIL}] RSP toolbar button not found in any window")
        kill_chrome()
        return False

    # Stress test: open/close the bubble rapidly to trigger the race between
    # ShowWindow and the compositor's in_paint_layer_contents phase.
    # Get the RSP window HWND and button position for PostMessage clicks.
    # PostMessage (not SendMessage) puts clicks in the queue, processed
    # outside of any compositor paint phase — accurately reproducing crash4.
    chrome_hwnd = int(win_with_btn.handle)
    print(f"  Chrome RSP window HWND: {chrome_hwnd:#x}  valid={win32gui.IsWindow(chrome_hwnd)}")
    try:
        btn_rect = rsp_btn.rectangle()
        btn_cx = (btn_rect.left + btn_rect.right) // 2
        btn_cy = (btn_rect.top + btn_rect.bottom) // 2
        # Convert screen coords to client coords of Chrome window.
        pt = ctypes.wintypes.POINT(btn_cx, btn_cy)
        ctypes.windll.user32.ScreenToClient(chrome_hwnd, ctypes.byref(pt))
        lparam = (pt.y << 16) | (pt.x & 0xFFFF)
        print(f"  Button screen=({btn_cx},{btn_cy}) client=({pt.x},{pt.y}) lparam={lparam:#x}")
    except Exception as e:
        print(f"  Warning: could not get button position ({e}); using invoke()")
        lparam = None

    def click_button():
        if lparam is not None and win32gui.IsWindow(chrome_hwnd):
            win32api.PostMessage(chrome_hwnd, win32con.WM_LBUTTONDOWN,
                                 win32con.MK_LBUTTON, lparam)
            win32api.PostMessage(chrome_hwnd, win32con.WM_LBUTTONUP, 0, lparam)
        else:
            try:
                rsp_btn.invoke()
            except Exception:
                pass

    iterations  = 50
    crashes     = 0
    bubbles     = 0
    print(f"\nStress-testing bubble open/close ({iterations} iterations)...")

    for i in range(iterations):
        if not chrome_alive(proc):
            print(f"\n  Chrome CRASHED at iteration {i+1}!")
            crashes += 1
            break

        click_button()

        hwnd5 = find_bubble_hwnd(timeout=2)
        if hwnd5:
            bubbles += 1
            close_bubble(hwnd5)
            # Wait for bubble to fully close before next iteration.
            deadline = time.time() + 1.5
            while time.time() < deadline:
                if not find_bubble_hwnd(timeout=0.1):
                    break
                time.sleep(0.1)
            time.sleep(0.1)
        else:
            time.sleep(0.2)

        # Re-find button every 10 iters in case UIA element went stale.
        if (i + 1) % 10 == 0:
            print(f"  iter {i+1}/{iterations}: {bubbles} bubbles opened, "
                  f"{crashes} crashes")
            try:
                rsp_btn = find_toolbar_rsp_button(win_with_btn, timeout=3) or rsp_btn
            except Exception:
                pass

    print()
    if crashes == 0 and chrome_alive(proc):
        print(f"[{PASS}] No crash in {iterations} open/close cycles "
              f"({bubbles} bubbles)")
        kill_chrome()
        return True
    else:
        print(f"[{FAIL}] Chrome crashed ({crashes} crash(es) in "
              f"{iterations} iterations, {bubbles} bubbles opened)")
        return False


if __name__ == "__main__":
    ok = run_crash_test()
    sys.exit(0 if ok else 1)
