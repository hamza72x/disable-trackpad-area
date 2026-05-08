#!/bin/bash
# Install/update trackpad-filter as a system service
# Run with sudo
# Always rebuilds, overwrites binary+service, restarts.

set -e

BIN="trackpad-filter"
SERVICE="trackpad-filter.service"
INSTALL_BIN="/usr/local/bin/$BIN"
INSTALL_SERVICE="/etc/systemd/system/$SERVICE"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/install.log"

echo "Installing trackpad-filter..." | tee "$LOG"

# Stop running service first (ignore if not running)
systemctl stop trackpad-filter.service 2>/dev/null || true

# Build
echo "Building..." | tee -a "$LOG"
gcc -O2 -Wall -Wextra -o "$SCRIPT_DIR/$BIN" "$SCRIPT_DIR/trackpad-filter.c" 2>&1 | tee -a "$LOG"

# Install binary (always overwrite)
cp -f "$SCRIPT_DIR/$BIN" "$INSTALL_BIN"
chmod 755 "$INSTALL_BIN"
echo "Installed binary: $INSTALL_BIN" | tee -a "$LOG"

# Install service file (always overwrite)
cp -f "$SCRIPT_DIR/$SERVICE" "$INSTALL_SERVICE"
echo "Installed service: $INSTALL_SERVICE" | tee -a "$LOG"

# Reload systemd, enable and start
systemctl daemon-reload
systemctl enable trackpad-filter.service 2>&1 | tee -a "$LOG"
systemctl restart trackpad-filter.service 2>&1 | tee -a "$LOG"

echo "Done. Service status:" | tee -a "$LOG"
systemctl status trackpad-filter.service --no-pager 2>&1 | tee -a "$LOG"
