#!/usr/bin/env python3
"""
Automated UI test for the RSP proxy Chrome integration.

Uses WM_COMMAND to trigger Chrome commands directly (no menu navigation needed
since Chrome's app menu items aren't exposed via UIA). UIA is used only for
observing results (window presence, button visibility, bubble content).

Tests:
  1. IDC_NEW_RSP_TAB via WM_COMMAND — must not crash Chrome
  2. RSP window appears (second chrome.exe window)
  3. RSP toolbar button visible in the RSP window
  4. RSP config bubble opens without crashing
  5. Refresh discovers bsd_sockets nodes (requires RM + bsd_sockets running)

Usage (from repo root):
  node scripts/test_multi_bsd_sockets.js   # start RM + 2 bsd_sockets first
  python scripts/test_rsp_ui.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

# Set DPI awareness so UIA coordinates match physical pixel coordinates used
# by SendInput/click_input.  Must be called before any UIA/win32gui calls.
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
except Exception:
    pass  # older Windows — ignore

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

CHROME = r"C:\chromium\src\out\Default\chrome.exe"
CHROME_ARGS = [
    "--no-sandbox",
    "--no-first-run",
    "--disable-session-crashed-bubble",
    r"--user-data-dir=C:\Temp\rsp_ui_test_profile",
]

# From chrome/app/chrome_command_ids.h
IDC_NEW_RSP_TAB    = 34065
IDC_NEW_RSP_WINDOW = 34068

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

_results: list = []
_chrome_pids: set = set()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def report(name, ok, detail=""):
    status = PASS if ok else FAIL
    line = f"  [{status}] {name}"
    if detail:
        line += f": {detail}"
    print(line)
    _results.append((name, ok, detail))


def chrome_alive(proc):
    return proc.poll() is None


def refresh_chrome_pids():
    _chrome_pids.update(
        p.pid for p in psutil.process_iter(["name"])
        if p.name().lower() == "chrome.exe"
    )


def find_chrome_windows_uia(timeout=60, min_count=1):
    """Wait until at least min_count chrome.exe windows are visible.
    
    Tries UIA first; falls back to win32gui enumeration since Chrome's main
    window can be visible in win32 but not yet registered with UIA.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        refresh_chrome_pids()
        found = []
        # UIA path
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
        # win32 fallback — wrap HWND in a UIA wrapper if UIA missed it
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


def find_window_uia(title_re, timeout=10):
    """Wait for any top-level window whose title matches title_re."""
    pattern = re.compile(title_re, re.IGNORECASE)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for w in Desktop(backend="uia").windows(visible_only=True):
                if pattern.search(w.window_text() or ""):
                    return w
        except Exception:
            pass
        time.sleep(0.4)
    return None


def find_control(window, ctrl_type, title_re=None, timeout=5):
    """Find a visible child control by UIA control type + optional title regex."""
    pattern = re.compile(title_re, re.IGNORECASE) if title_re else None
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for ctrl in window.descendants(control_type=ctrl_type):
                try:
                    t = ctrl.window_text() or ""
                    if (pattern is None or pattern.search(t)) and ctrl.is_visible():
                        return ctrl
                except Exception:
                    pass
        except Exception:
            pass
        time.sleep(0.3)
    return None


def find_toolbar_rsp_button(window, timeout=8):
    """Find the RSP toolbar button specifically (sibling of 'Chromium' app menu).

    There are two 'RSP proxy settings' buttons: one in the toolbar and one in
    the tab strip chip.  We want the toolbar button because clicking the tab
    chip button requires additional hover/focus state the automation doesn't
    provide reliably.  Strategy: find the 'Chromium' app-menu button then walk
    its siblings looking for the RSP button with the same parent.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            # Find the app-menu "Chromium" button which is always in the toolbar.
            app_menu_btn = None
            for ctrl in window.descendants(control_type="Button"):
                try:
                    if ctrl.window_text() == "Chromium" and ctrl.is_visible():
                        app_menu_btn = ctrl
                        break
                except Exception:
                    pass
            if app_menu_btn:
                try:
                    parent = app_menu_btn.parent()
                    for sibling in parent.children():
                        try:
                            t = sibling.window_text() or ""
                            if "RSP proxy settings" in t and sibling.is_visible():
                                return sibling
                        except Exception:
                            pass
                except Exception:
                    pass
        except Exception:
            pass
        time.sleep(0.3)
    return None


def find_bubble_hwnd(timeout=5):
    """Find the RSP Proxy Settings bubble HWND fresh via win32gui."""
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
        time.sleep(0.3)
    return None


def count_chrome_hwnds():
    """Return the set of visible Chrome_WidgetWin HWNDs across all chrome pids."""
    hwnds = set()
    def cb(hwnd, _):
        if not win32gui.IsWindowVisible(hwnd):
            return
        _, pid = win32process.GetWindowThreadProcessId(hwnd)
        if pid in _chrome_pids:
            cls = win32gui.GetClassName(hwnd)
            if "Chrome_WidgetWin" in cls:
                hwnds.add(hwnd)
    win32gui.EnumWindows(cb, None)
    return hwnds


def chrome_hwnd_for_window(uia_window):
    """Return the Win32 HWND for a UIA window wrapper."""
    return uia_window.handle


def post_chrome_command(hwnd, command_id):
    """Send WM_COMMAND to a Chrome window to trigger a browser command."""
    win32api.PostMessage(hwnd, win32con.WM_COMMAND, command_id, 0)


def kill_chrome():
    os.system("taskkill /F /IM chrome.exe >nul 2>&1")
    time.sleep(1.5)


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------

def run_tests():
    kill_chrome()
    print("\n=== RSP UI Automated Test ===\n")

    proc = subprocess.Popen([CHROME] + CHROME_ARGS)
    print(f"  Chrome launched (PID {proc.pid})")
    refresh_chrome_pids()

    chrome_wins = find_chrome_windows_uia(timeout=60, min_count=1)
    if not chrome_wins:
        report("Chrome starts", False, "No chrome.exe window within 60s")
        kill_chrome()
        return False

    main_win = chrome_wins[0]
    report("Chrome starts", True, main_win.window_text())
    time.sleep(2)

    # ------------------------------------------------------------------
    # Test 1: IDC_NEW_RSP_TAB via WM_COMMAND — must not crash Chrome
    # ------------------------------------------------------------------
    print("\n--- Test 1: New RSP Tab (WM_COMMAND) ---")
    try:
        hwnd = chrome_hwnd_for_window(main_win)
        post_chrome_command(hwnd, IDC_NEW_RSP_TAB)
        # The Navigate() is posted async so give it a moment to execute.
        time.sleep(3)

        if not chrome_alive(proc):
            report("IDC_NEW_RSP_TAB does not crash Chrome", False, "Chrome exited")
            return False
        report("IDC_NEW_RSP_TAB does not crash Chrome", True)
    except Exception as e:
        report("Test 1 (WM_COMMAND)", False, str(e))
        if not chrome_alive(proc):
            kill_chrome()
            return False

    # ------------------------------------------------------------------
    # Test 2: RSP window appears (second chrome.exe UIA window)
    # ------------------------------------------------------------------
    print("\n--- Test 2: RSP window appears ---")
    rsp_win = None
    try:
        refresh_chrome_pids()
        all_chrome = find_chrome_windows_uia(timeout=8, min_count=2)
        report("RSP window opened", len(all_chrome) >= 2,
               f"Found {len(all_chrome)} chrome.exe window(s)")

        for w in all_chrome:
            if w.handle != main_win.handle:
                rsp_win = w
                break

        if rsp_win:
            report("Identify RSP window", True, rsp_win.window_text())
        else:
            # Only one window — maybe RSP tab opened in same window (wrong profile)
            all_titles = [w.window_text() for w in all_chrome]
            report("Identify RSP window", False, f"Windows: {all_titles}")
    except Exception as e:
        report("Test 2 (RSP window)", False, str(e))

    # ------------------------------------------------------------------
    # Test 3: RSP toolbar button visible in RSP window
    # ------------------------------------------------------------------
    print("\n--- Test 3: RSP toolbar button visible ---")
    rsp_btn = None
    if rsp_win:
        try:
            rsp_win.set_focus()
            time.sleep(0.5)
            rsp_btn = find_toolbar_rsp_button(rsp_win, timeout=8)
            if rsp_btn:
                report("RSP toolbar button visible", True)
            else:
                btns = []
                try:
                    btns = [c.window_text() for c in
                            rsp_win.descendants(control_type="Button")
                            if c.window_text()]
                except Exception:
                    pass
                report("RSP toolbar button visible", False,
                       f"Buttons present: {btns}")
        except Exception as e:
            report("RSP toolbar button visible", False, str(e))
    else:
        report("RSP toolbar button visible", False, "No RSP window")

    # ------------------------------------------------------------------
    # Test 4: Config bubble opens without crashing
    # ------------------------------------------------------------------
    print("\n--- Test 4: Config bubble opens ---")
    bubble = None
    if rsp_btn:
        try:
            rsp_win.set_focus()
            time.sleep(0.3)
            hwnds_before = count_chrome_hwnds()
            try:
                rsp_btn.invoke()
            except Exception:
                rsp_btn.click_input()
            # Poll for a new visible Chrome window (the bubble widget).
            bubble_hwnd = None
            deadline = time.time() + 5
            while time.time() < deadline:
                time.sleep(0.3)
                refresh_chrome_pids()
                hwnds_after = count_chrome_hwnds()
                new_hwnds = hwnds_after - hwnds_before
                if new_hwnds:
                    bubble_hwnd = next(iter(new_hwnds))
                    break

            if not chrome_alive(proc):
                report("Config bubble opens (no crash)", False, "Chrome exited")
                kill_chrome()
                return False
            report("Config bubble opens (no crash)", True)

            if bubble_hwnd:
                # Try to find by HWND handle first, then by title.
                try:
                    bubble = Desktop(backend="uia").window(handle=bubble_hwnd)
                    if bubble.window_text():
                        report("Config bubble visible", True, bubble.window_text())
                    else:
                        bubble = None
                except Exception:
                    bubble = None
                if not bubble:
                    bubble = find_window_uia(r"RSP Proxy Settings", timeout=5)
                    report("Config bubble visible", bubble is not None,
                           bubble.window_text() if bubble else
                           f"Win32 hwnd={bubble_hwnd} but not in UIA")
            else:
                # Last resort: try UIA Desktop scan
                bubble = find_window_uia(r"RSP Proxy Settings", timeout=3)
                report("Config bubble visible", bubble is not None,
                       bubble.window_text() if bubble else
                       "No new Chrome hwnd and not in UIA — button click may have missed")
        except Exception as e:
            report("Test 4 (config bubble)", False, str(e))
            if not chrome_alive(proc):
                kill_chrome()
                return False
    else:
        report("Config bubble opens", False, "No RSP toolbar button to click")

    # ------------------------------------------------------------------
    # Test 5: Nodes auto-discovered when bubble opens
    # The bubble auto-refreshes 500ms after opening (via PostDelayedTask in
    # Show()), so the test just needs to wait and check the hint text.
    # ------------------------------------------------------------------
    print("\n--- Test 5: Auto-refresh discovers nodes ---")
    try:
        # Re-find the bubble fresh to avoid stale UIA element references.
        bubble_hwnd5 = find_bubble_hwnd(timeout=5)
        if not bubble_hwnd5:
            print("    Bubble not found, re-opening...")
            if rsp_btn is not None:
                try:
                    rsp_btn.invoke()
                except Exception as e2:
                    print(f"    T5: rsp_btn.invoke() failed: {e2}")
                time.sleep(2)
                bubble_hwnd5 = find_bubble_hwnd(timeout=5)

        if not bubble_hwnd5:
            report("Find config bubble for auto-refresh", False)
        else:
            bubble_win5 = Desktop(backend="uia").window(handle=bubble_hwnd5)

            if not chrome_alive(proc):
                report("Auto-refresh does not crash", False, "Chrome exited")
                kill_chrome()
                return False
            report("Auto-refresh does not crash", True)

            # Wait up to 6s for the hint label to change from the default.
            hint = find_control(bubble_win5, "Text",
                                title_re=r"Found \d+ node", timeout=6)

            # Dump all text controls for diagnostics.
            texts = []
            try:
                texts = [c.window_text() for c in
                         bubble_win5.descendants(control_type="Text")
                         if c.window_text()]
            except Exception:
                pass
            print(f"    Bubble texts: {texts}")

            report("Nodes discovered", hint is not None,
                   hint.window_text() if hint else
                   f"No match in: {texts}")
    except Exception as e:
        report("Test 5 (auto-refresh)", False, str(e))

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print("\n=== Results ===")
    passed = sum(1 for _, ok, _ in _results if ok)
    total = len(_results)
    for name, ok, detail in _results:
        status = PASS if ok else FAIL
        line = f"  [{status}] {name}"
        if detail:
            line += f" ({detail})"
        print(line)
    print(f"\n  {passed}/{total} passed\n")

    kill_chrome()
    return passed == total


if __name__ == "__main__":
    ok = run_tests()
    sys.exit(0 if ok else 1)
