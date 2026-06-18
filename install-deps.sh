#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# WetGregFirmware — Development Dependencies Installer
#
# Installs everything needed to build the wetgreg-hub-rtos firmware, run
# the DevTool, and flash the WetGreg PCB (soldered Pico 2 W / RP2350).
# Supports Arch (pacman/yay), Debian/Ubuntu (apt), and Fedora (dnf).
#
# Usage:
#   chmod +x install-deps.sh
#   ./install-deps.sh
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

B="\033[1m"; G="\033[32m"; Y="\033[33m"; R="\033[31m"; C="\033[36m"; X="\033[0m"
info()  { echo -e "${C}[info]${X} $1"; }
ok()    { echo -e "${G}[ok]${X}   $1"; }
warn()  { echo -e "${Y}[warn]${X} $1"; }
fail()  { echo -e "${R}[fail]${X} $1"; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Detect package manager ──
if command -v pacman &>/dev/null; then
    PM="pacman"; INSTALL="sudo pacman -S --needed --noconfirm"
elif command -v apt &>/dev/null; then
    PM="apt"; INSTALL="sudo apt install -y"
elif command -v dnf &>/dev/null; then
    PM="dnf"; INSTALL="sudo dnf install -y"
else
    fail "Unsupported package manager. Install dependencies manually."; exit 1
fi
info "Package manager: $PM"

# ── System packages ──
info "Installing system packages..."
case $PM in
    pacman)
        $INSTALL base-devel cmake ninja git python python-pip \
                 arm-none-eabi-gcc arm-none-eabi-newlib \
                 docker docker-compose tk python-pyserial libusb hidapi ;;
    apt)
        $INSTALL build-essential cmake ninja-build git python3 python3-pip \
                 gcc-arm-none-eabi libnewlib-arm-none-eabi \
                 docker.io docker-compose python3-tk python3-serial \
                 libusb-1.0-0-dev libhidapi-dev ;;
    dnf)
        $INSTALL cmake ninja-build git python3 python3-pip \
                 arm-none-eabi-gcc-cs arm-none-eabi-newlib \
                 docker docker-compose python3-tkinter python3-pyserial \
                 libusb1-devel hidapi-devel ;;
esac
ok "System packages installed"

# ── Docker group ──
if ! groups | grep -q docker; then
    info "Adding $USER to docker group..."
    sudo usermod -aG docker "$USER"
    warn "Log out and back in for Docker permissions to take effect"
else
    ok "User already in docker group"
fi

# ── Serial group (so the DevTool serial monitor works without sudo) ──
SERIAL_GROUP="uucp"; [ "$PM" = "apt" ] && SERIAL_GROUP="dialout"
if ! groups | grep -q "$SERIAL_GROUP"; then
    info "Adding $USER to $SERIAL_GROUP group (serial access)..."
    sudo usermod -aG "$SERIAL_GROUP" "$USER"
    warn "Log out and back in for serial permissions to take effect"
else
    ok "User already in $SERIAL_GROUP group"
fi

# ── Submodules (FreeRTOS-Kernel + picowota) ──
info "Initialising git submodules (FreeRTOS-Kernel, picowota)..."
git -C "$REPO_ROOT" submodule update --init --recursive
ok "Submodules initialised"

# ── Pico SDK (host copy — optional; the Docker build clones its own) ──
PICO_DIR="$HOME/pico"; SDK_DIR="$PICO_DIR/pico-sdk"
if [ -f "$SDK_DIR/pico_sdk_init.cmake" ]; then
    ok "Pico SDK already installed at $SDK_DIR"
else
    info "Installing Pico SDK (for host-side picotool builds)..."
    mkdir -p "$PICO_DIR"
    git clone https://github.com/raspberrypi/pico-sdk.git "$SDK_DIR"
    git -C "$SDK_DIR" submodule update --init
    ok "Pico SDK installed at $SDK_DIR"
fi
export PICO_SDK_PATH="$SDK_DIR"
if ! grep -q "PICO_SDK_PATH" ~/.bashrc 2>/dev/null && \
   ! grep -q "PICO_SDK_PATH" ~/.zshrc 2>/dev/null; then
    SHELL_RC="$HOME/.bashrc"; [ -f "$HOME/.zshrc" ] && SHELL_RC="$HOME/.zshrc"
    { echo ""; echo "# Pico SDK"; echo "export PICO_SDK_PATH=\"$SDK_DIR\""; } >> "$SHELL_RC"
    ok "Added PICO_SDK_PATH to $SHELL_RC"
fi

# ── picotool ──
if command -v picotool &>/dev/null; then
    ok "picotool already installed: $(which picotool)"
else
    info "Installing picotool..."
    case $PM in
        pacman)
            if command -v yay &>/dev/null; then yay -S --needed --noconfirm picotool
            elif command -v paru &>/dev/null; then paru -S --needed --noconfirm picotool
            else warn "No AUR helper found — install picotool manually (sudo pacman -S picotool or via yay)"; fi ;;
        apt|dnf)
            PICOTOOL_DIR="$PICO_DIR/picotool"
            [ -d "$PICOTOOL_DIR" ] || git clone https://github.com/raspberrypi/picotool.git "$PICOTOOL_DIR"
            cmake -G Ninja -DPICO_SDK_PATH="$SDK_DIR" -S "$PICOTOOL_DIR" -B "$PICOTOOL_DIR/build"
            ninja -C "$PICOTOOL_DIR/build"
            if [ -f "$PICOTOOL_DIR/build/picotool" ]; then
                sudo cp "$PICOTOOL_DIR/build/picotool" /usr/local/bin/; ok "picotool installed to /usr/local/bin/"
            else fail "picotool build failed"; fi ;;
    esac
    command -v picotool &>/dev/null && ok "picotool: $(picotool version 2>&1 | head -1)" || warn "picotool not on PATH yet"
fi

# ── udev rules (Pico USB access without sudo) ──
UDEV_RULE="/etc/udev/rules.d/99-pico.rules"
if [ ! -f "$UDEV_RULE" ]; then
    info "Installing udev rules for Pico USB access..."
    sudo tee "$UDEV_RULE" > /dev/null << 'UDEV'
# Raspberry Pi Pico / Pico 2 — BOOTSEL mode
SUBSYSTEM=="usb", ATTR{idVendor}=="2e8a", MODE="0666"
# Raspberry Pi Pico / Pico 2 — serial (CDC)
SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", MODE="0666"
UDEV
    sudo udevadm control --reload-rules; sudo udevadm trigger
    ok "udev rules installed"
else
    ok "udev rules already installed"
fi

# ── Python deps for the DevTool ──
pip install --user -r "$REPO_ROOT/tools/devtool/requirements.txt" 2>/dev/null \
    && ok "Python deps installed" || warn "pip install skipped (pyserial may already be a system package)"

echo ""
echo -e "${B}${G}════════════════════════════════════════${X}"
echo -e "${B}${G}  WetGregFirmware dependencies installed!${X}"
echo -e "${B}${G}════════════════════════════════════════${X}"
echo -e "Next:  ${C}python3 tools/devtool/devtool.py${X}  → Flash tab → Clean Build & Flash"
