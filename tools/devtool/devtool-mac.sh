#!/usr/bin/env bash
#
# devtool-mac.sh — launch the WetGreg DevTool GUI on macOS.
#
# macOS ships a system Python (/usr/bin/python3) linked against the deprecated
# Tcl/Tk 8.5, which renders the DevTool as a blank/black window. This script
# finds a Python with a working Tk 8.6 (Homebrew's python3.11 / python3.12 /
# python3.13), makes sure pyserial is present, and launches the tool.
#
#   ./devtool-mac.sh            # launch the GUI
#   ./devtool-mac.sh deps       # any extra args are passed through to devtool.py
#
set -euo pipefail

# Resolve the directory this script lives in (so it works from anywhere).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVTOOL="$SCRIPT_DIR/devtool.py"

# Find a Python whose tkinter reports Tk >= 8.6 (8.5 = the black-window bug).
pick_python() {
  for py in \
    /opt/homebrew/bin/python3.13 \
    /opt/homebrew/bin/python3.12 \
    /opt/homebrew/bin/python3.11 \
    "$(command -v python3 || true)"; do
    [ -x "$py" ] || continue
    if "$py" -c 'import sys,tkinter; sys.exit(0 if tkinter.TkVersion>=8.6 else 1)' 2>/dev/null; then
      echo "$py"
      return 0
    fi
  done
  return 1
}

PY="$(pick_python || true)"
if [ -z "${PY:-}" ]; then
  cat >&2 <<'EOF'
No Python with a working Tk 8.6 was found.

Install one with Homebrew, then re-run this script:
    brew install python-tk@3.13      # Python 3.13 + Tk 8.6
EOF
  exit 1
fi

# Ensure pyserial (Debug-tab serial monitor). Harmless if already installed.
if ! "$PY" -c 'import serial' 2>/dev/null; then
  echo "Installing pyserial into $PY ..." >&2
  "$PY" -m pip install --quiet pyserial
fi

echo "Launching DevTool with $PY (Tk $("$PY" -c 'import tkinter;print(tkinter.TkVersion)'))" >&2
export TK_SILENCE_DEPRECATION=1
exec "$PY" "$DEVTOOL" "$@"
