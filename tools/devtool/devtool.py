#!/usr/bin/env python3
"""
WetGreg DevTool — a focused build/flash/debug companion for the wetgreg-hub-rtos
firmware running on the WetGreg PCB (soldered Pico 2 W / RP2350) with a WeAct
2.13" V4 e-ink panel.

This is the SLIM descendant of the original multi-board WetGreg DevTool. It does
exactly three things, one per tab:

  1. Flash  — picotool setup, device info, and deployment:
              • Clean Build & Flash   (Docker build from scratch, then flash)
              • Flash (existing build) (flash the last .uf2 without rebuilding)
              • Device Info / Reboot to BOOTSEL / Full Erase (recovery)
  2. Debug  — a serial monitor (live printf output) plus a one-click
              environment diagnostics panel (deps, USB, serial port, groups).
  3. Docs   — renders DEVTOOL_GUIDE.md (the full user guide) with a clickable
              table of contents and search.

Everything is hardcoded to the single supported target:
    firmware : wetgreg-hub-rtos
    board    : pico2_w        (RP2350)
    display  : V4             (WeAct 2.13" B&W, SSD1680)
    wiring   : WetGreg PCB     (e-ink on SPI0, GP17-22)

It also has a CLI for headless use (build/flash from a script or over SSH):
    python3 devtool.py info          # picotool device info
    python3 devtool.py flash         # flash existing build/wetgreg_hub_rtos.uf2
    python3 devtool.py build-flash   # clean Docker build, then flash
    python3 devtool.py reboot        # reboot the Pico into BOOTSEL
    python3 devtool.py erase         # full chip erase (recovery)
    python3 devtool.py deps          # check the toolchain dependencies
    python3 devtool.py gui           # launch the GUI (default with no args)
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

# ─────────────────────────────────────────────────────────────────────────────
# Project layout & target constants
# ─────────────────────────────────────────────────────────────────────────────

# tools/devtool/devtool.py → tools/devtool → tools → <repo root>
PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEV_SETUP    = PROJECT_ROOT / "dev-setup"
RTOS_DIR     = DEV_SETUP / "wetgreg-hub-rtos"
BUILD_DIR    = RTOS_DIR / "build"
UF2_NAME     = "wetgreg_hub_rtos.uf2"
UF2_PATH     = BUILD_DIR / UF2_NAME
GUIDE_MD     = Path(__file__).resolve().parent / "DEVTOOL_GUIDE.md"

DOCKER_SERVICE = "build-wetgreg-hub-rtos"
PICO_BOARD     = "pico2_w"     # RP2350 (the WetGreg PCB carries a soldered Pico 2 W)
DISPLAY_VARIANT = "V4"         # WeAct 2.13" B&W panel (SSD1680)

# WetGreg PCB Rev 1 routes the WeAct e-ink header to GP17-22 on SPI0.
EINK_PINS_WETGREG_PCB = {"RST": 21, "DC": 20, "CS": 17, "BUSY": 22, "CLK": 18, "MOSI": 19}
EINK_SPI_PORT = "spi0"

# BOOTSEL drive labels: RP2040 → RPI-RP2, RP2350 (our board) → RP2350.
BOOTSEL_LABELS = ["RP2350", "RPI-RP2"]

# Raspberry Pi USB vendor id (Pico / Pico 2).
RPI_VID = 0x2E8A


# ─────────────────────────────────────────────────────────────────────────────
# Core logic — pure functions usable by both the CLI and the GUI.
# `log` is a callback taking a single string; defaults to printing.
# ─────────────────────────────────────────────────────────────────────────────

def _default_log(msg):
    print(msg, flush=True)


def find_picotool():
    """Locate the picotool binary. Returns a path string or None."""
    found = shutil.which("picotool")
    if found:
        return found
    sdk = os.environ.get("PICO_SDK_PATH", "")
    candidates = []
    if sdk:
        candidates.append(Path(sdk).parent / "picotool" / "build" / "picotool")
    candidates += [
        Path.home() / "pico" / "picotool" / "build" / "picotool",
        Path.home() / "picotool" / "build" / "picotool",
        Path("/usr/local/bin/picotool"),
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    return None


def run_picotool(args, timeout=15, log=_default_log):
    """Run picotool with `args`. Returns (returncode, stdout, stderr)."""
    pt = find_picotool()
    if not pt:
        return -1, "", "picotool not found (install it: see the Flash tab / install-deps.sh)"
    cmd = [pt] + list(args)
    log(f"[picotool] $ {' '.join(cmd)}")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "Timeout"
    except Exception as e:           # noqa: BLE001
        return -1, "", str(e)


def find_pico_serial():
    """Find the Pico serial port (/dev/ttyACM* on Linux). Returns path or None."""
    try:
        import serial.tools.list_ports as lp
    except Exception:                # noqa: BLE001
        # Fall back to globbing the common device path.
        import glob
        ports = sorted(glob.glob("/dev/ttyACM*"))
        return ports[0] if ports else None
    for p in lp.comports():
        if p.vid == RPI_VID:
            return p.device
    for p in lp.comports():
        if "ttyACM" in p.device or "usbmodem" in p.device:
            return p.device
    return None


def find_rpi_rp2_mount():
    """Find the BOOTSEL USB drive mountpoint, or None."""
    user = os.environ.get("USER", "")
    for label in BOOTSEL_LABELS:
        for p in (Path(f"/run/media/{user}/{label}"),
                  Path(f"/media/{user}/{label}"),
                  Path(f"/mnt/{label}")):
            if p.exists() and p.is_dir():
                return p
    for label in BOOTSEL_LABELS:
        try:
            r = subprocess.run(["findmnt", "-rno", "TARGET", "-S", f"LABEL={label}"],
                               capture_output=True, text=True, timeout=3)
            if r.returncode == 0 and r.stdout.strip():
                p = Path(r.stdout.strip().splitlines()[0])
                if p.exists():
                    return p
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass
    return None


def wait_for_bootsel_mount(timeout=10):
    """Poll for the BOOTSEL drive to appear. Returns mount path or None."""
    for _ in range(timeout * 4):
        m = find_rpi_rp2_mount()
        if m:
            return m
        time.sleep(0.25)
    return None


def eject_bootsel(mount_path, log=_default_log):
    """Flush + logically unmount the BOOTSEL drive (never a SCSI power-off).

    The RP2 bootrom auto-reboots the instant it receives a complete .uf2, so all
    we need is to flush the write and clean up the mountpoint. Best-effort."""
    try:
        subprocess.run(["sync"], timeout=5)
    except Exception:                # noqa: BLE001
        pass
    try:
        src = subprocess.run(["findmnt", "-n", "-o", "SOURCE", str(mount_path)],
                             capture_output=True, text=True, timeout=5).stdout.strip()
        if src:
            r = subprocess.run(["udisksctl", "unmount", "-b", src],
                               capture_output=True, text=True, timeout=10)
            if r.returncode == 0:
                return True
    except Exception:                # noqa: BLE001
        pass
    try:
        subprocess.run(["umount", str(mount_path)], capture_output=True, timeout=10)
        return True
    except Exception:                # noqa: BLE001
        pass
    return False


def apply_eink_wiring(log=_default_log):
    """Rewrite the vendored DEV_Config.c onto the WetGreg PCB e-ink wiring (SPI0,
    GP17-22). Idempotent; returns a summary string if it changed anything."""
    cfg = RTOS_DIR / "lib" / "Config" / "DEV_Config.c"
    if not cfg.exists():
        return None
    try:
        text = cfg.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    orig = text
    for name, num in EINK_PINS_WETGREG_PCB.items():
        text = re.sub(rf"(EPD_{name}_PIN\s*=\s*)\d+(\s*;)", rf"\g<1>{num}\g<2>", text)
    text = re.sub(r"(#define\s+SPI_PORT\s+)spi[01]\b", rf"\g<1>{EINK_SPI_PORT}", text)
    if text != orig:
        try:
            cfg.write_text(text, encoding="utf-8")
        except OSError:
            return None
        return "WetGreg PCB e-ink wiring (SPI0, GP17-22)"
    return None


def check_deps():
    """Return a list of (name, ok, detail) describing the toolchain state."""
    rows = []
    for name, path in (("cmake", shutil.which("cmake")),
                       ("ninja", shutil.which("ninja")),
                       ("arm-none-eabi-gcc", shutil.which("arm-none-eabi-gcc")),
                       ("docker", shutil.which("docker")),
                       ("git", shutil.which("git")),
                       ("picotool", find_picotool())):
        rows.append((name, bool(path), path or "not found"))

    # Docker daemon running?
    daemon = False
    if shutil.which("docker"):
        try:
            daemon = subprocess.run(["docker", "info"], capture_output=True,
                                    timeout=10).returncode == 0
        except Exception:            # noqa: BLE001
            daemon = False
    rows.append(("docker daemon", daemon, "running" if daemon else "not running"))

    # Submodules present?
    frtos = (PROJECT_ROOT / "FreeRTOS-Kernel" / "tasks.c").exists()
    rows.append(("FreeRTOS-Kernel submodule", frtos,
                 "present" if frtos else "run: git submodule update --init --recursive"))

    # Serial port / device
    port = find_pico_serial()
    rows.append(("Pico serial port", bool(port), port or "no /dev/ttyACM* (BOOTSEL mode or unplugged)"))

    return rows


def docker_build(log=_default_log, progress=None):
    """Clean Docker build of wetgreg-hub-rtos. Returns the .uf2 path or raises."""
    def _p(v):
        if progress:
            progress(v)

    # 1. Nuke the build dir (may be root-owned from a prior Docker run).
    if BUILD_DIR.exists():
        try:
            shutil.rmtree(BUILD_DIR)
        except PermissionError:
            log("[build] build/ is root-owned — cleaning via docker...")
            subprocess.run(["docker", "run", "--rm", "-v", f"{BUILD_DIR}:/clean",
                            "alpine", "rm", "-rf", "/clean"],
                           capture_output=True, timeout=30)
            shutil.rmtree(BUILD_DIR, ignore_errors=True)
    _p(5)

    # 2. Docker daemon check.
    if subprocess.run(["docker", "info"], capture_output=True, timeout=15).returncode != 0:
        raise RuntimeError("Docker is not running — start the docker service first")

    # 3. Retarget the vendored e-ink driver onto the WetGreg PCB wiring.
    w = apply_eink_wiring(log)
    if w:
        log(f"[build] applied {w}")
    _p(10)

    # 4. Build the image (cached after the first run).
    log(f"[build] docker compose build {DOCKER_SERVICE} ...")
    proc = subprocess.Popen(["docker", "compose", "build", "--progress=plain", DOCKER_SERVICE],
                            cwd=str(DEV_SETUP), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            log(f"[docker] {line}")
    proc.wait(timeout=900)
    if proc.returncode != 0:
        raise RuntimeError("docker image build failed — see log")
    _p(25)

    # 5. Compile (cmake + ninja inside the container).
    log(f"[build] compiling ({PICO_BOARD}, display {DISPLAY_VARIANT}) ...")
    proc = subprocess.Popen(["docker", "compose", "run", "--rm",
                             "-e", f"DISPLAY_VARIANT={DISPLAY_VARIANT}",
                             "-e", f"PICO_BOARD={PICO_BOARD}", DOCKER_SERVICE],
                            cwd=str(DEV_SETUP), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            log(f"[ninja] {line}")
    proc.wait(timeout=600)
    if proc.returncode != 0:
        raise RuntimeError("firmware compile failed — see log")
    _p(45)

    if not UF2_PATH.exists():
        raise RuntimeError(f"build finished but {UF2_NAME} not found")
    size_kb = UF2_PATH.stat().st_size // 1024
    log(f"[build] OK — {UF2_NAME} ({size_kb} KB)")
    return UF2_PATH


def flash_uf2(uf2_path, log=_default_log, progress=None):
    """Flash a .uf2: reboot to BOOTSEL → wait for drive → copy → eject."""
    def _p(v):
        if progress:
            progress(v)

    uf2_path = Path(uf2_path)
    if not uf2_path.exists():
        raise RuntimeError(f"firmware not found: {uf2_path}")
    name = uf2_path.name

    log(f"[flash] rebooting Pico to BOOTSEL ...")
    _p(10)
    rc, _out, _err = run_picotool(["reboot", "-f", "-u"], 8, log)
    if rc != 0 and not find_rpi_rp2_mount():
        raise RuntimeError("reboot failed and no BOOTSEL drive — hold BOOTSEL, "
                           "plug in, and retry")

    log("[flash] waiting for BOOTSEL drive ...")
    _p(30)
    mount = wait_for_bootsel_mount(timeout=12)
    if not mount:
        raise RuntimeError("BOOTSEL drive didn't appear")
    log(f"[flash] BOOTSEL mounted at {mount}")

    log(f"[flash] copying {name} ...")
    _p(60)
    shutil.copy2(str(uf2_path), str(Path(mount) / name))
    _p(80)

    log("[flash] flushing + unmounting (Pico reboots itself) ...")
    eject_bootsel(mount, log)
    _p(90)
    time.sleep(3)
    _p(100)
    size_kb = uf2_path.stat().st_size // 1024
    log(f"[flash] done — {name} ({size_kb} KB) flashed")


def clean_build_and_flash(log=_default_log, progress=None):
    """Clean Docker build, then flash the produced .uf2."""
    uf2 = docker_build(log, progress)
    log("[flash] build complete — flashing ...")
    # Re-map the 0-45 build progress onto 50-100 for the flash phase.
    flash_uf2(uf2, log, (lambda v: progress(50 + v // 2)) if progress else None)


def device_info(log=_default_log):
    """Return picotool's device info string (or an error message)."""
    rc, out, err = run_picotool(["info", "-a"], 8, log)
    if rc == 0 and out.strip():
        return out.strip()
    return (err.strip() or out.strip() or "No device found — plug in the Pico via USB")


def reboot_bootsel(log=_default_log):
    rc, _out, err = run_picotool(["reboot", "-f", "-u"], 8, log)
    if rc == 0:
        return True, "Pico is in BOOTSEL mode"
    return False, (err.strip() or "reboot failed — is the Pico running firmware?")


def full_erase(log=_default_log):
    """Erase ALL flash (recovery for a brownout-corrupted board). Leaves it in
    BOOTSEL, ready to re-flash."""
    rc, _out, err = run_picotool(["erase", "-a", "-F"], 90, log)
    if rc != 0:                       # maybe already in BOOTSEL
        rc, _out, err = run_picotool(["erase", "-a"], 90, log)
    if rc == 0:
        return True, "Flash erased — now Flash Firmware to restore"
    return False, (err.strip() or "erase failed — hold BOOTSEL, plug in, retry")


def _find_terminal():
    """Return an argv prefix that runs a command in a visible terminal, or None.
    The command runs so the user can enter their sudo password interactively."""
    for term, prefix in (("kitty", ["kitty", "--hold", "bash", "-lc"]),
                         ("alacritty", ["alacritty", "-e", "bash", "-lc"]),
                         ("konsole", ["konsole", "-e", "bash", "-lc"]),
                         ("gnome-terminal", ["gnome-terminal", "--", "bash", "-lc"]),
                         ("xfce4-terminal", ["xfce4-terminal", "-e", "bash -lc"]),
                         ("xterm", ["xterm", "-e", "bash", "-lc"])):
        if shutil.which(term):
            return prefix
    return None


def _pkg_install_picotool_cmd():
    """Return the best shell command to install picotool via the system package
    manager, or None if we don't know how. On Arch we prefer the AUR package
    (paru/yay) because building picotool from source fails with GCC 15; the AUR
    package carries the fix."""
    if shutil.which("pacman"):
        # In the official repos? (Some Arch derivatives ship it.)
        if subprocess.run(["pacman", "-Si", "picotool"],
                          capture_output=True).returncode == 0:
            return "sudo pacman -S --needed picotool"
        for helper in ("paru", "yay"):
            if shutil.which(helper):
                return f"{helper} -S --needed picotool"
        return None
    if shutil.which("apt"):
        return "sudo apt update && sudo apt install -y picotool"
    if shutil.which("dnf"):
        return "sudo dnf install -y picotool"
    return None


def install_picotool(log=_default_log):
    """Install picotool. Prefers the system package manager (handles the GCC 15
    build break on Arch); only falls back to building from the Pico SDK source
    on systems where that is known to work. Returns (ok, message)."""
    if find_picotool():
        return True, f"picotool already installed: {find_picotool()}"

    # 1. System package manager (the reliable path, esp. on Arch + GCC 15).
    cmd = _pkg_install_picotool_cmd()
    if cmd:
        # The install needs sudo (a password), which we can't supply from a GUI
        # subprocess. Launch it in a visible terminal so the user can authorise.
        term = _find_terminal()
        if term:
            log(f"[picotool] launching installer in a terminal: {cmd}")
            try:
                subprocess.Popen(term + [f'echo "$ {cmd}"; {cmd}; '
                                         'echo; echo "Done — close this window and '
                                         'click Refresh in the DevTool."'])
                return True, ("installing in a terminal window — enter your "
                              "password there, then click Refresh")
            except Exception as e:            # noqa: BLE001
                log(f"[picotool] could not launch terminal: {e}")
        # No terminal (headless / CLI): hand the user the exact command.
        return False, (f"Run this to install picotool (needs sudo): {cmd}\n"
                       "Then re-check. (Building from the SDK is skipped here "
                       "because it fails with GCC 15 on Arch.)")

    # 2. Fall back to the from-source SDK build (Debian/Fedora without a pkg).
    sdk = os.environ.get("PICO_SDK_PATH", "")
    if not sdk:
        for c in (Path.home() / "pico" / "pico-sdk", Path.home() / "pico-sdk",
                  Path("/opt/pico-sdk")):
            if (c / "pico_sdk_init.cmake").exists():
                sdk = str(c)
                break
    if not sdk:
        return False, ("Pico SDK not found and no package manager picotool. "
                       "Install picotool manually, then click Refresh.")
    src = Path(sdk).parent / "picotool"
    if not src.exists():
        log("[picotool] cloning picotool ...")
        subprocess.run(["git", "clone", "https://github.com/raspberrypi/picotool.git",
                        str(src)], capture_output=True, timeout=120)
    bdir = src / "build"
    bdir.mkdir(exist_ok=True)
    log("[picotool] cmake configure ...")
    r = subprocess.run(["cmake", "-G", "Ninja", f"-DPICO_SDK_PATH={sdk}", ".."],
                       cwd=str(bdir), capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return False, "cmake failed: " + (r.stderr or r.stdout)[-200:]
    log("[picotool] ninja ...")
    r = subprocess.run(["ninja"], cwd=str(bdir), capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        return False, "build failed: " + (r.stderr or r.stdout)[-200:]
    if (bdir / "picotool").exists():
        return True, f"picotool built at {bdir / 'picotool'}"
    return False, "build finished but binary not found"


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────

def _cli(argv):
    p = argparse.ArgumentParser(
        prog="devtool",
        description="WetGreg DevTool — build/flash/debug for wetgreg-hub-rtos "
                    "(WetGreg PCB, Pico 2 W, WeAct 2.13\" V4).")
    sub = p.add_subparsers(dest="cmd")
    sub.add_parser("gui", help="launch the GUI (default)")
    sub.add_parser("info", help="picotool device info")
    f = sub.add_parser("flash", help="flash the existing build/wetgreg_hub_rtos.uf2")
    f.add_argument("uf2", nargs="?", help="path to a .uf2 (defaults to the RTOS build)")
    sub.add_parser("build-flash", help="clean Docker build, then flash")
    sub.add_parser("build", help="clean Docker build only (no flash)")
    sub.add_parser("reboot", help="reboot the Pico into BOOTSEL")
    sub.add_parser("erase", help="full chip erase (recovery)")
    sub.add_parser("deps", help="check toolchain dependencies")
    args = p.parse_args(argv)

    cmd = args.cmd or "gui"
    if cmd == "gui":
        return _launch_gui()
    if cmd == "info":
        print(device_info())
        return 0
    if cmd == "deps":
        ok_all = True
        for name, ok, detail in check_deps():
            mark = "OK " if ok else "!! "
            ok_all = ok_all and ok
            print(f"  [{mark}] {name:<28} {detail}")
        return 0 if ok_all else 1
    if cmd == "reboot":
        ok, msg = reboot_bootsel(); print(msg); return 0 if ok else 1
    if cmd == "erase":
        ok, msg = full_erase(); print(msg); return 0 if ok else 1
    if cmd == "build":
        try:
            docker_build(); return 0
        except Exception as e:        # noqa: BLE001
            print(f"build failed: {e}", file=sys.stderr); return 1
    if cmd == "flash":
        try:
            flash_uf2(args.uf2 or UF2_PATH); return 0
        except Exception as e:        # noqa: BLE001
            print(f"flash failed: {e}", file=sys.stderr); return 1
    if cmd == "build-flash":
        try:
            clean_build_and_flash(); return 0
        except Exception as e:        # noqa: BLE001
            print(f"build+flash failed: {e}", file=sys.stderr); return 1
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# GUI
# ─────────────────────────────────────────────────────────────────────────────

def _launch_gui():
    try:
        import tkinter  # noqa: F401
    except Exception:                # noqa: BLE001
        print("Tkinter is not installed. Install it (Arch: sudo pacman -S tk, "
              "Debian: sudo apt install python3-tk) or use the CLI:\n"
              "  python3 devtool.py build-flash", file=sys.stderr)
        return 1
    app = DevToolApp()
    app.mainloop()
    return 0


# Catppuccin-ish palette (matches the original tool's look).
BG_DARK   = "#1e1e2e"
BG_PANEL  = "#282840"
FG_TEXT   = "#cdd6f4"
FG_DIM    = "#6c7086"
FG_ACCENT = "#89b4fa"
FG_GREEN  = "#a6e3a1"
FG_RED    = "#f38ba8"
FG_YELLOW = "#f9e2af"
MONO = ("JetBrains Mono", 9)


def _gui_imports():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    return tk, ttk, filedialog, messagebox


class DevToolApp:
    """Thin wrapper that builds the Tk root, the notebook and the shared log."""

    def __new__(cls):
        tk, ttk, _fd, _mb = _gui_imports()

        class _App(tk.Tk):
            def __init__(self):
                super().__init__()
                self.title("WetGreg DevTool — wetgreg-hub-rtos")
                self.geometry("1080x720")
                self.configure(bg=BG_DARK)
                self._init_style()

                outer = ttk.PanedWindow(self, orient=tk.VERTICAL)
                outer.pack(fill=tk.BOTH, expand=True)

                nb = ttk.Notebook(outer)
                self.flash_tab = FlashTab(nb, self)
                self.debug_tab = DebugTab(nb, self)
                self.docs_tab  = DocsTab(nb, self)
                nb.add(self.flash_tab, text="  Flash  ")
                nb.add(self.debug_tab, text="  Debug  ")
                nb.add(self.docs_tab,  text="  Docs  ")
                outer.add(nb, weight=4)

                logf = ttk.LabelFrame(outer, text="  Log  ")
                self.log_text = tk.Text(logf, height=8, bg="#11111b", fg=FG_TEXT,
                                        font=MONO, relief=tk.FLAT, wrap=tk.WORD)
                lsb = ttk.Scrollbar(logf, command=self.log_text.yview)
                self.log_text.configure(yscrollcommand=lsb.set, state=tk.DISABLED)
                lsb.pack(side=tk.RIGHT, fill=tk.Y)
                self.log_text.pack(fill=tk.BOTH, expand=True)
                outer.add(logf, weight=1)

                self.log(f"WetGreg DevTool ready. Target: wetgreg-hub-rtos / "
                         f"{PICO_BOARD} / display {DISPLAY_VARIANT} / WetGreg PCB.")

            def _init_style(self):
                st = ttk.Style(self)
                try:
                    st.theme_use("clam")
                except Exception:    # noqa: BLE001
                    pass
                st.configure(".", background=BG_DARK, foreground=FG_TEXT, font=MONO)
                st.configure("TFrame", background=BG_DARK)
                st.configure("TLabelframe", background=BG_DARK, foreground=FG_ACCENT)
                st.configure("TLabelframe.Label", background=BG_DARK, foreground=FG_ACCENT)
                st.configure("TLabel", background=BG_DARK, foreground=FG_TEXT)
                st.configure("TButton", background=BG_PANEL, foreground=FG_TEXT)
                st.map("TButton", background=[("active", FG_ACCENT)],
                       foreground=[("active", BG_DARK)])
                st.configure("TNotebook", background=BG_DARK)
                st.configure("TNotebook.Tab", background=BG_PANEL, foreground=FG_DIM,
                             padding=(14, 6))
                st.map("TNotebook.Tab", background=[("selected", FG_ACCENT)],
                       foreground=[("selected", BG_DARK)])

            def log(self, msg):
                """Thread-safe append to the shared log."""
                def _do():
                    self.log_text.configure(state=tk.NORMAL)
                    ts = time.strftime("%H:%M:%S")
                    self.log_text.insert(tk.END, f"[{ts}] {msg}\n")
                    self.log_text.see(tk.END)
                    self.log_text.configure(state=tk.DISABLED)
                try:
                    self.after(0, _do)
                except Exception:    # noqa: BLE001
                    pass

        return _App()


# The tab classes are defined as factory functions because they need the tk/ttk
# modules (only importable when a display is available). Each returns a ttk.Frame
# subclass instance.

# Right-hand "how to" panels shown beside the Flash and Debug controls.
FLASH_GUIDE_TEXT = """HOW TO FLASH — step by step
═══════════════════════════

THE 10-SECOND VERSION
  1. Plug the WetGreg (Pico 2 W) into USB.
  2. Click "Clean Build & Flash".
  3. Watch the Log panel at the bottom — done.

You never press the BOOTSEL button. picotool
reboots the board into BOOTSEL over USB, copies
the firmware, and the board reboots itself.


1 · picotool (do this once)
───────────────────────────
picotool is what lets us flash without touching
the BOOTSEL button. If the status reads
"not found", click "Install picotool".

  • On Arch/CachyOS it installs the AUR package
    in a terminal — enter your password there,
    then click Refresh.
  • Building picotool from source fails with
    GCC 15, so we use the package manager.


2 · Device (is the board there?)
────────────────────────────────
  • Device Info  — prints what is *actually*
    running on the board (program name, version,
    flash id). Your first sanity check.
  • Reboot to BOOTSEL — drops the board into USB
    drive mode and leaves it there.
  • Full Erase — wipes ALL flash. Only for
    rescuing a board that won't boot after a
    brownout. Re-flash firmware afterward.


3 · Deploy (get firmware on)
────────────────────────────
  • Clean Build & Flash
      Changed the firmware source? Use this.
      Deletes build/, recompiles in Docker from
      scratch, then flashes. ~1 min after the
      first (cached) build.

  • Flash (existing build)
      Source unchanged? Skip the compile and
      flash the last build/wetgreg_hub_rtos.uf2.
      Instant.

  • Build Only
      Compile but don't flash — catch compile
      errors with no board attached.

  • Browse .uf2…
      Flash any file you pick instead of the
      default build.


IF SOMETHING GOES WRONG
───────────────────────
  • "BOOTSEL drive didn't appear" — the reboot
    didn't take. Hold BOOTSEL, plug in, release,
    then Flash once. picotool works after that.
  • "reboot failed — is the Pico running?" — the
    firmware is hung. Same manual-BOOTSEL fix.
  • Board shows snow / won't boot — Full Erase,
    then Clean Build & Flash.

Full reference lives in the Docs tab.
"""

DEBUG_GUIDE_TEXT = """HOW TO DEBUG
════════════

Two tools live here: a one-click environment
check, and a live serial monitor.


ENVIRONMENT DIAGNOSTICS
───────────────────────
Click "Run diagnostics" whenever a build or
flash misbehaves. Each row is green OK or red
FAIL:

  • cmake / ninja / arm-gcc — the toolchain
    (also baked into the Docker image).
  • docker + docker daemon — the build runs in a
    container; the daemon must be up. Start it
    with: sudo systemctl start docker
  • git — needed for the SDK + submodules.
  • picotool — needed to flash.
  • FreeRTOS-Kernel submodule — the firmware
    won't link without it. Fix:
      git submodule update --init --recursive
  • Pico serial port — confirms the board shows
    up as /dev/ttyACM*.

This usually points straight at the problem.


SERIAL MONITOR (live printf)
────────────────────────────
See what the firmware prints at runtime.

  1. Plug in the board (running firmware, NOT in
     BOOTSEL mode).
  2. Pick the Port — it auto-detects the Pico
     (USB VID 2E8A). Hit Refresh if you plugged
     in after opening the tab.
  3. Click Connect. The label turns green.
  4. printf output streams into the black panel.

  • The input box sends a line (Enter or Send).
  • Ctrl+C sends an interrupt byte.
  • Reset (Ctrl+D) sends a soft-reset byte.
  • Clear empties the view; Save Log… writes it
    to a timestamped file.


A GOOD DEBUG LOOP
─────────────────
  1. Add printf() lines in the firmware.
  2. Flash tab → Clean Build & Flash.
  3. Come back here → Connect → watch the output.
  4. Repeat. Disconnect before re-flashing (the
     serial port frees up automatically on
     reboot, but disconnecting is tidier).


COMMON SNAGS
────────────
  • No ports listed — board is in BOOTSEL mode,
    not running firmware, or the cable is
    charge-only. Click Refresh.
  • "permission denied" — add yourself to the
    uucp (Arch) / dialout (Debian) group, then
    log out and back in.
  • "pyserial is not installed" — pip install
    pyserial. Only the monitor needs it; the
    rest of the tool is unaffected.
"""


def _guide_panel(parent, title, body):
    """A read-only scrollable 'how to' text panel for the right of a tab."""
    tk, ttk, _fd, _mb = _gui_imports()
    frame = ttk.LabelFrame(parent, text=title)
    txt = tk.Text(frame, wrap=tk.WORD, bg="#11111b", fg=FG_TEXT,
                  font=("JetBrains Mono", 9), relief=tk.FLAT, padx=10, pady=8)
    sb = ttk.Scrollbar(frame, command=txt.yview)
    txt.configure(yscrollcommand=sb.set)
    sb.pack(side=tk.RIGHT, fill=tk.Y)
    txt.pack(fill=tk.BOTH, expand=True)
    txt.tag_configure("head", foreground=FG_ACCENT,
                      font=("JetBrains Mono", 9, "bold"))
    txt.insert("1.0", body)
    # Highlight section headers (lines immediately above a ─── or ═══ rule).
    lines = body.split("\n")
    for i, ln in enumerate(lines):
        if set(ln.strip()) <= {"─", "═"} and ln.strip() and i > 0:
            txt.tag_add("head", f"{i}.0", f"{i}.end")
    txt.configure(state=tk.DISABLED)
    return frame


def _init_sashes(pw, fractions):
    """Place a PanedWindow's sashes at the given fractions of its size, once,
    when it is first realized. Keeps panes proportional at startup while still
    leaving them user-resizable. `fractions` are cumulative (e.g. [1/3, 2/3] for
    three equal panes, [0.55] for a 55/45 two-pane split)."""
    state = {"done": False}

    def _apply(_evt=None):
        if state["done"]:
            return
        pw.update_idletasks()
        horiz = "horizontal" in str(pw.cget("orient"))
        total = pw.winfo_width() if horiz else pw.winfo_height()
        if total <= 1:                      # not sized yet — try again shortly
            pw.after(60, _apply)
            return
        for i, f in enumerate(fractions):
            try:
                pw.sashpos(i, int(total * f))
            except Exception:               # noqa: BLE001
                pass
        state["done"] = True

    pw.bind("<Map>", lambda e: pw.after(80, _apply), add="+")


def FlashTab(parent, app):
    tk, ttk, filedialog, messagebox = _gui_imports()

    class _Flash(ttk.Frame):
        def __init__(self):
            super().__init__(parent)
            self.app = app
            self._busy = False
            self._build()
            self.after(400, self._refresh_status)

        def _build(self):
            pad = dict(padx=8, pady=4)

            # Split: controls on the left, a "how to" guide on the right.
            split = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
            split.pack(fill=tk.BOTH, expand=True)
            left = ttk.Frame(split)
            right = ttk.Frame(split)
            split.add(left, weight=3)
            split.add(right, weight=2)

            # Left side: the three sections in an equal, resizable vertical split.
            lpw = ttk.PanedWindow(left, orient=tk.VERTICAL)
            lpw.pack(fill=tk.BOTH, expand=True, **pad)

            # 1. picotool setup
            sf = ttk.LabelFrame(lpw, text="  1. picotool  ")
            row = ttk.Frame(sf); row.pack(fill=tk.X, padx=8, pady=6)
            ttk.Button(row, text="Install picotool",
                       command=self._install).pack(side=tk.LEFT, padx=(0, 4))
            ttk.Button(row, text="Refresh", command=self._refresh_status).pack(side=tk.LEFT)
            self._tool_lbl = ttk.Label(sf, text="checking…", foreground=FG_DIM,
                                       wraplength=380, justify=tk.LEFT)
            self._tool_lbl.pack(anchor=tk.W, padx=8, pady=(0, 6))

            # 2. device
            df = ttk.LabelFrame(lpw, text="  2. Device (Pico 2 W over USB)  ")
            row = ttk.Frame(df); row.pack(fill=tk.X, padx=8, pady=6)
            ttk.Button(row, text="Device Info", command=self._device_info).pack(side=tk.LEFT, padx=(0, 4))
            ttk.Button(row, text="Reboot to BOOTSEL", command=self._reboot).pack(side=tk.LEFT, padx=(0, 4))
            ttk.Button(row, text="Full Erase (recovery)", command=self._erase).pack(side=tk.LEFT)
            self._dev_lbl = ttk.Label(df, text="plug in the Pico via USB", foreground=FG_DIM)
            self._dev_lbl.pack(anchor=tk.W, padx=8)
            self._dev_txt = tk.Text(df, height=4, width=40, bg="#11111b", fg=FG_TEXT,
                                    font=MONO, relief=tk.FLAT, wrap=tk.WORD)
            self._dev_txt.pack(fill=tk.BOTH, expand=True, padx=8, pady=(4, 8))
            self._dev_txt.configure(state=tk.DISABLED)

            # 3. deploy
            ff = ttk.LabelFrame(lpw, text="  3. Deploy firmware (no BOOTSEL button needed)  ")

            info = ttk.Label(ff, foreground=FG_DIM, justify=tk.LEFT,
                             text="Clean Build & Flash → Docker compiles wetgreg-hub-rtos from\n"
                                  "scratch, then picotool reboots the Pico and flashes it.\n"
                                  "Flash (existing) → flash the last build without recompiling.")
            info.pack(anchor=tk.W, padx=8, pady=(6, 4))

            self._uf2_lbl = ttk.Label(ff, text="", foreground=FG_DIM)
            self._uf2_lbl.pack(anchor=tk.W, padx=8)

            row = ttk.Frame(ff); row.pack(fill=tk.X, padx=8, pady=8)
            self._build_btn = ttk.Button(row, text="Clean Build & Flash",
                                         command=self._clean_build_flash)
            self._build_btn.pack(side=tk.LEFT, padx=(0, 4))
            self._flash_btn = ttk.Button(row, text="Flash (existing build)",
                                         command=self._flash_existing)
            self._flash_btn.pack(side=tk.LEFT, padx=(0, 4))
            self._buildonly_btn = ttk.Button(row, text="Build Only",
                                             command=self._build_only)
            self._buildonly_btn.pack(side=tk.LEFT, padx=(0, 4))
            ttk.Button(row, text="Browse .uf2…", command=self._browse).pack(side=tk.LEFT)

            self._pbar = ttk.Progressbar(ff, maximum=100)
            self._pbar.pack(fill=tk.X, padx=8, pady=(2, 4))
            self._status = ttk.Label(ff, text="Ready", foreground=FG_DIM)
            self._status.pack(anchor=tk.W, padx=8, pady=(0, 8))
            self._custom_uf2 = None

            # Add the three sections as resizable panes. Section 1 (picotool) is
            # half the height of sections 2 & 3 — it only holds two buttons and a
            # status line. Weights keep that ratio (1:2:2) as the window resizes.
            lpw.add(sf, weight=1)
            lpw.add(df, weight=2)
            lpw.add(ff, weight=2)
            _init_sashes(lpw, [0.2, 0.6])          # 1/5, 2/5, 2/5 at startup

            # Right: how-to guide
            _guide_panel(right, "  How to flash  ", FLASH_GUIDE_TEXT).pack(
                fill=tk.BOTH, expand=True, **pad)
            _init_sashes(split, [0.56])            # ~56/44 controls/guide

        # ---- helpers ----
        def _set(self, lbl, text, colour=FG_DIM):
            lbl.config(text=text, foreground=colour)

        def _progress(self, v):
            self.after(0, lambda: self._pbar.configure(value=v))

        def _set_busy(self, busy):
            self._busy = busy
            state = tk.DISABLED if busy else tk.NORMAL
            for b in (self._build_btn, self._flash_btn, self._buildonly_btn):
                self.after(0, lambda b=b, s=state: b.config(state=s))

        def _refresh_status(self):
            pt = find_picotool()
            if pt:
                self._set(self._tool_lbl, f"found: {pt}", FG_GREEN)
            else:
                self._set(self._tool_lbl, "not found — click Install, or "
                          "install-deps.sh / your package manager", FG_YELLOW)
            if UF2_PATH.exists():
                kb = UF2_PATH.stat().st_size // 1024
                self._set(self._uf2_lbl, f"existing build: {UF2_PATH}  ({kb} KB)", FG_GREEN)
            else:
                self._set(self._uf2_lbl, "no existing build yet — use Clean Build & Flash", FG_YELLOW)

        def _install(self):
            self.app.log("[picotool] installing from SDK…")
            def _run():
                ok, msg = install_picotool(self.app.log)
                self.app.log(f"[picotool] {msg}")
                self.after(0, self._refresh_status)
            threading.Thread(target=_run, daemon=True).start()

        def _device_info(self):
            self._set(self._dev_lbl, "querying…", FG_YELLOW)
            def _run():
                txt = device_info(self.app.log)
                def _upd():
                    self._dev_txt.configure(state=tk.NORMAL)
                    self._dev_txt.delete("1.0", tk.END)
                    self._dev_txt.insert("1.0", txt)
                    self._dev_txt.configure(state=tk.DISABLED)
                    connected = "No device" not in txt and "not found" not in txt
                    self._set(self._dev_lbl, "device connected" if connected else
                              "no device — plug in via USB",
                              FG_GREEN if connected else FG_YELLOW)
                self.after(0, _upd)
            threading.Thread(target=_run, daemon=True).start()

        def _reboot(self):
            def _run():
                ok, msg = reboot_bootsel(self.app.log)
                self.app.log(f"[picotool] {msg}")
                self.after(0, lambda: self._set(self._dev_lbl, msg,
                                                FG_GREEN if ok else FG_RED))
            threading.Thread(target=_run, daemon=True).start()

        def _erase(self):
            if not messagebox.askyesno("Full Erase",
                "This ERASES ALL FLASH on the connected Pico (firmware + settings).\n"
                "Use it to recover a board that won't boot after a brownout.\n"
                "You must re-flash firmware afterward.\n\nProceed?"):
                return
            self._set(self._dev_lbl, "erasing all flash…", FG_YELLOW)
            def _run():
                ok, msg = full_erase(self.app.log)
                self.app.log(f"[picotool] {msg}")
                self.after(0, lambda: self._set(self._dev_lbl, msg,
                                                FG_GREEN if ok else FG_RED))
            threading.Thread(target=_run, daemon=True).start()

        def _browse(self):
            path = filedialog.askopenfilename(
                title="Select a .uf2", initialdir=str(BUILD_DIR if BUILD_DIR.exists() else RTOS_DIR),
                filetypes=[("UF2 files", "*.uf2"), ("All files", "*.*")])
            if path:
                self._custom_uf2 = path
                self._set(self._uf2_lbl, f"selected: {path}", FG_ACCENT)

        def _flash_existing(self):
            if self._busy:
                return
            uf2 = self._custom_uf2 or UF2_PATH
            if not Path(uf2).exists():
                messagebox.showwarning("Flash", "No build found. Use Clean Build & Flash first.")
                return
            self._set_busy(True)
            self._progress(0)
            self._set(self._status, "flashing…", FG_YELLOW)
            def _run():
                try:
                    flash_uf2(uf2, self.app.log, self._progress)
                    self.after(0, lambda: self._set(self._status, "Done — flashed", FG_GREEN))
                except Exception as e:    # noqa: BLE001
                    self.app.log(f"[flash] error: {e}")
                    self.after(0, lambda e=e: self._set(self._status, f"Error: {e}", FG_RED))
                finally:
                    self._set_busy(False)
            threading.Thread(target=_run, daemon=True).start()

        def _clean_build_flash(self):
            if self._busy:
                return
            self._set_busy(True); self._progress(0)
            self._set(self._status, "building (Docker)…", FG_YELLOW)
            def _run():
                try:
                    clean_build_and_flash(self.app.log, self._progress)
                    self.after(0, lambda: self._set(self._status, "Done — built and flashed", FG_GREEN))
                    self.after(0, self._refresh_status)
                except Exception as e:    # noqa: BLE001
                    self.app.log(f"[build] error: {e}")
                    self.after(0, lambda e=e: self._set(self._status, f"Error: {e}", FG_RED))
                finally:
                    self._set_busy(False)
            threading.Thread(target=_run, daemon=True).start()

        def _build_only(self):
            if self._busy:
                return
            self._set_busy(True); self._progress(0)
            self._set(self._status, "building (Docker)…", FG_YELLOW)
            def _run():
                try:
                    docker_build(self.app.log, self._progress)
                    self.after(0, lambda: self._set(self._status, "Build complete (not flashed)", FG_GREEN))
                    self.after(0, self._refresh_status)
                except Exception as e:    # noqa: BLE001
                    self.app.log(f"[build] error: {e}")
                    self.after(0, lambda e=e: self._set(self._status, f"Error: {e}", FG_RED))
                finally:
                    self._set_busy(False)
            threading.Thread(target=_run, daemon=True).start()

    return _Flash()


def DebugTab(parent, app):
    tk, ttk, filedialog, messagebox = _gui_imports()

    class _Debug(ttk.Frame):
        def __init__(self):
            super().__init__(parent)
            self.app = app
            self._ser = None
            self._reader = None
            self._running = False
            self._build()
            self.after(400, self._run_diagnostics)

        def _build(self):
            pad = dict(padx=8, pady=4)

            # Split: tools on the left, a "how to debug" guide on the right.
            split = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
            split.pack(fill=tk.BOTH, expand=True)
            left = ttk.Frame(split)
            right = ttk.Frame(split)
            split.add(left, weight=3)
            split.add(right, weight=2)

            # Left side: diagnostics + serial monitor in a resizable vertical split.
            lpw = ttk.PanedWindow(left, orient=tk.VERTICAL)
            lpw.pack(fill=tk.BOTH, expand=True, **pad)

            # Diagnostics
            diag = ttk.LabelFrame(lpw, text="  Environment diagnostics  ")
            row = ttk.Frame(diag); row.pack(fill=tk.X, padx=8, pady=6)
            ttk.Button(row, text="Run diagnostics", command=self._run_diagnostics).pack(side=tk.LEFT)
            ttk.Label(row, text="  cmake / ninja / arm-gcc / docker / picotool / "
                                "submodule / serial", foreground=FG_DIM).pack(side=tk.LEFT)
            self._diag_txt = tk.Text(diag, height=8, width=40, bg="#11111b", fg=FG_TEXT,
                                     font=MONO, relief=tk.FLAT, wrap=tk.NONE)
            self._diag_txt.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))
            self._diag_txt.configure(state=tk.DISABLED)
            for tagname, col in (("ok", FG_GREEN), ("bad", FG_RED)):
                self._diag_txt.tag_configure(tagname, foreground=col)

            # Serial monitor
            mon = ttk.LabelFrame(lpw, text="  Serial monitor (live printf output, 115200 baud)  ")
            ctl = ttk.Frame(mon); ctl.pack(fill=tk.X, padx=8, pady=6)
            ttk.Label(ctl, text="Port:").pack(side=tk.LEFT)
            self._port_var = tk.StringVar()
            self._port_cb = ttk.Combobox(ctl, textvariable=self._port_var, width=18)
            self._port_cb.pack(side=tk.LEFT, padx=4)
            ttk.Button(ctl, text="Refresh", command=self._refresh_ports).pack(side=tk.LEFT, padx=2)
            self._conn_btn = ttk.Button(ctl, text="Connect", command=self._toggle_conn)
            self._conn_btn.pack(side=tk.LEFT, padx=2)
            ttk.Button(ctl, text="Clear", command=self._clear_mon).pack(side=tk.LEFT, padx=2)
            ttk.Button(ctl, text="Save Log…", command=self._save_log).pack(side=tk.LEFT, padx=2)
            self._conn_lbl = ttk.Label(ctl, text="disconnected", foreground=FG_DIM)
            self._conn_lbl.pack(side=tk.LEFT, padx=8)

            self._mon_txt = tk.Text(mon, width=40, bg="#11111b", fg=FG_GREEN, font=MONO,
                                    relief=tk.FLAT, wrap=tk.CHAR)
            msb = ttk.Scrollbar(mon, command=self._mon_txt.yview)
            self._mon_txt.configure(yscrollcommand=msb.set)
            msb.pack(side=tk.RIGHT, fill=tk.Y)
            self._mon_txt.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 4))

            send = ttk.Frame(mon); send.pack(fill=tk.X, padx=8, pady=(0, 8))
            self._send_var = tk.StringVar()
            ent = ttk.Entry(send, textvariable=self._send_var)
            ent.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 4))
            ent.bind("<Return>", lambda _e: self._send())
            ttk.Button(send, text="Send", command=self._send).pack(side=tk.LEFT, padx=2)
            ttk.Button(send, text="Ctrl+C", command=lambda: self._send_raw(b"\x03")).pack(side=tk.LEFT, padx=2)
            ttk.Button(send, text="Reset (Ctrl+D)", command=lambda: self._send_raw(b"\x04")).pack(side=tk.LEFT, padx=2)

            # Add diagnostics + monitor as resizable panes (monitor gets more room).
            lpw.add(diag, weight=2)
            lpw.add(mon, weight=3)
            _init_sashes(lpw, [0.42])

            # Right: how-to guide
            _guide_panel(right, "  How to debug  ", DEBUG_GUIDE_TEXT).pack(
                fill=tk.BOTH, expand=True, **pad)
            _init_sashes(split, [0.56])            # ~56/44 tools/guide

            self._refresh_ports()

        # ---- diagnostics ----
        def _run_diagnostics(self):
            def _run():
                rows = check_deps()
                def _upd():
                    self._diag_txt.configure(state=tk.NORMAL)
                    self._diag_txt.delete("1.0", tk.END)
                    for name, ok, detail in rows:
                        mark = "  OK  " if ok else " FAIL "
                        self._diag_txt.insert(tk.END, f"[{mark}] ", "ok" if ok else "bad")
                        self._diag_txt.insert(tk.END, f"{name:<28} {detail}\n")
                    self._diag_txt.configure(state=tk.DISABLED)
                self.after(0, _upd)
            threading.Thread(target=_run, daemon=True).start()

        # ---- serial ----
        def _refresh_ports(self):
            ports = []
            try:
                import serial.tools.list_ports as lp
                ports = [p.device for p in lp.comports()]
            except Exception:        # noqa: BLE001
                import glob
                ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
            self._port_cb["values"] = ports
            cur = self._port_var.get()
            if not cur:
                pico = find_pico_serial()
                self._port_var.set(pico or (ports[0] if ports else ""))

        def _toggle_conn(self):
            if self._running:
                self._disconnect()
            else:
                self._connect()

        def _connect(self):
            try:
                import serial
            except Exception:        # noqa: BLE001
                messagebox.showerror("pyserial", "pyserial is not installed.\n"
                                     "Install it: pip install pyserial")
                return
            port = self._port_var.get().strip()
            if not port:
                messagebox.showwarning("Serial", "No port selected.")
                return
            try:
                self._ser = serial.Serial(port, 115200, timeout=0.1)
            except Exception as e:   # noqa: BLE001
                messagebox.showerror("Serial", f"Could not open {port}:\n{e}")
                self.app.log(f"[serial] open failed: {e}")
                return
            self._running = True
            self._conn_btn.config(text="Disconnect")
            self._conn_lbl.config(text=f"connected: {port}", foreground=FG_GREEN)
            self.app.log(f"[serial] connected {port} @ 115200")
            self._reader = threading.Thread(target=self._read_loop, daemon=True)
            self._reader.start()

        def _disconnect(self):
            self._running = False
            try:
                if self._ser:
                    self._ser.close()
            except Exception:        # noqa: BLE001
                pass
            self._ser = None
            self._conn_btn.config(text="Connect")
            self._conn_lbl.config(text="disconnected", foreground=FG_DIM)
            self.app.log("[serial] disconnected")

        def _read_loop(self):
            buf = b""
            while self._running and self._ser:
                try:
                    data = self._ser.read(256)
                except Exception:    # noqa: BLE001
                    break
                if data:
                    buf += data
                    try:
                        text = buf.decode("utf-8", errors="replace")
                        buf = b""
                    except Exception:    # noqa: BLE001
                        continue
                    self.after(0, lambda t=text: self._append_mon(t))
            # loop ended
            if self._running:
                self.after(0, self._disconnect)

        def _append_mon(self, text):
            self._mon_txt.insert(tk.END, text)
            self._mon_txt.see(tk.END)

        def _clear_mon(self):
            self._mon_txt.delete("1.0", tk.END)

        def _send(self):
            msg = self._send_var.get()
            self._send_raw((msg + "\r\n").encode("utf-8"))
            self._send_var.set("")

        def _send_raw(self, data):
            if not (self._ser and self._running):
                self.app.log("[serial] not connected")
                return
            try:
                self._ser.write(data)
            except Exception as e:   # noqa: BLE001
                self.app.log(f"[serial] write failed: {e}")

        def _save_log(self):
            path = filedialog.asksaveasfilename(
                title="Save serial log", defaultextension=".txt",
                initialfile=time.strftime("serial-%Y%m%d-%H%M%S.txt"))
            if path:
                Path(path).write_text(self._mon_txt.get("1.0", tk.END))
                self.app.log(f"[serial] log saved to {path}")

    return _Debug()


def DocsTab(parent, app):
    tk, ttk, _fd, _mb = _gui_imports()

    class _Docs(ttk.Frame):
        def __init__(self):
            super().__init__(parent)
            self.app = app
            self._headings = []   # (line_index_tag, title)
            self._build()
            self._load()

        def _build(self):
            top = ttk.Frame(self); top.pack(fill=tk.X, padx=8, pady=6)
            ttk.Label(top, text="Search:").pack(side=tk.LEFT)
            self._search_var = tk.StringVar()
            ent = ttk.Entry(top, textvariable=self._search_var, width=30)
            ent.pack(side=tk.LEFT, padx=4)
            ent.bind("<Return>", lambda _e: self._search())
            ttk.Button(top, text="Find", command=self._search).pack(side=tk.LEFT, padx=2)
            ttk.Button(top, text="Clear", command=self._clear_search).pack(side=tk.LEFT, padx=2)
            ttk.Button(top, text="Reload", command=self._load).pack(side=tk.LEFT, padx=2)
            ttk.Label(top, text=f"  source: {GUIDE_MD.name}", foreground=FG_DIM).pack(side=tk.LEFT)

            pw = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
            pw.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 8))

            tocf = ttk.LabelFrame(pw, text="  Contents  ")
            self._toc = tk.Listbox(tocf, bg="#11111b", fg=FG_TEXT, font=MONO,
                                   relief=tk.FLAT, activestyle="none",
                                   selectbackground=FG_ACCENT, width=34)
            self._toc.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
            self._toc.bind("<<ListboxSelect>>", self._on_toc)
            pw.add(tocf, weight=1)

            bodyf = ttk.LabelFrame(pw, text="  Guide  ")
            self._txt = tk.Text(bodyf, bg="#11111b", fg=FG_TEXT, font=("JetBrains Mono", 10),
                                relief=tk.FLAT, wrap=tk.WORD, padx=12, pady=10, spacing3=2)
            sb = ttk.Scrollbar(bodyf, command=self._txt.yview)
            self._txt.configure(yscrollcommand=sb.set)
            sb.pack(side=tk.RIGHT, fill=tk.Y)
            self._txt.pack(fill=tk.BOTH, expand=True)
            pw.add(bodyf, weight=4)

            # markdown render tags
            self._txt.tag_configure("h1", font=("JetBrains Mono", 16, "bold"), foreground=FG_ACCENT, spacing1=12, spacing3=6)
            self._txt.tag_configure("h2", font=("JetBrains Mono", 13, "bold"), foreground=FG_GREEN, spacing1=10, spacing3=4)
            self._txt.tag_configure("h3", font=("JetBrains Mono", 11, "bold"), foreground=FG_YELLOW, spacing1=8, spacing3=2)
            self._txt.tag_configure("code", font=("JetBrains Mono", 9), background="#181825", foreground="#f5e0dc")
            self._txt.tag_configure("bullet", lmargin1=24, lmargin2=40)
            self._txt.tag_configure("hr", foreground=FG_DIM)
            self._txt.tag_configure("search", background=FG_YELLOW, foreground=BG_DARK)

        def _load(self):
            self._toc.delete(0, tk.END)
            self._headings = []
            self._txt.configure(state=tk.NORMAL)
            self._txt.delete("1.0", tk.END)
            if GUIDE_MD.exists():
                md = GUIDE_MD.read_text(encoding="utf-8", errors="replace")
            else:
                md = ("# DevTool Guide not found\n\n"
                      f"Expected at: {GUIDE_MD}\n\nRegenerate it or check the repo.")
            self._render(md)
            self._txt.configure(state=tk.DISABLED)

        def _render(self, md):
            in_code = False
            hcount = 0
            for raw in md.splitlines():
                line = raw.rstrip("\n")
                if line.strip().startswith("```"):
                    in_code = not in_code
                    continue
                if in_code:
                    self._txt.insert(tk.END, line + "\n", "code")
                    continue
                if line.startswith("### "):
                    self._add_heading(line[4:], "h3", hcount); hcount += 1
                elif line.startswith("## "):
                    self._add_heading(line[3:], "h2", hcount); hcount += 1
                elif line.startswith("# "):
                    self._add_heading(line[2:], "h1", hcount); hcount += 1
                elif line.strip() in ("---", "***", "___"):
                    self._txt.insert(tk.END, "─" * 60 + "\n", "hr")
                elif line.lstrip().startswith(("- ", "* ")):
                    txt = line.lstrip()[2:]
                    self._txt.insert(tk.END, "• " + self._strip_inline(txt) + "\n", "bullet")
                else:
                    self._txt.insert(tk.END, self._strip_inline(line) + "\n")

        def _add_heading(self, title, tag, idx):
            mark = f"h{idx}"
            self._txt.insert(tk.END, title + "\n", (tag,))
            self._txt.mark_set(mark, f"{self._txt.index(tk.END)} -2l linestart")
            self._headings.append((mark, title))
            indent = {"h1": "", "h2": "  ", "h3": "    "}[tag]
            self._toc.insert(tk.END, indent + title)

        @staticmethod
        def _strip_inline(text):
            text = re.sub(r"\*\*(.+?)\*\*", r"\1", text)
            text = re.sub(r"`(.+?)`", r"\1", text)
            return text

        def _on_toc(self, _evt):
            sel = self._toc.curselection()
            if not sel or sel[0] >= len(self._headings):
                return
            mark, _title = self._headings[sel[0]]
            self._txt.see(mark)

        def _search(self):
            self._txt.tag_remove("search", "1.0", tk.END)
            term = self._search_var.get().strip()
            if not term:
                return
            idx = "1.0"
            count = 0
            while True:
                idx = self._txt.search(term, idx, nocase=True, stopindex=tk.END)
                if not idx:
                    break
                end = f"{idx}+{len(term)}c"
                self._txt.tag_add("search", idx, end)
                idx = end
                count += 1
            if count:
                first = self._txt.search(term, "1.0", nocase=True, stopindex=tk.END)
                self._txt.see(first)
            self.app.log(f"[docs] '{term}': {count} match(es)")

        def _clear_search(self):
            self._txt.tag_remove("search", "1.0", tk.END)
            self._search_var.set("")

    return _Docs()


# ─────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
