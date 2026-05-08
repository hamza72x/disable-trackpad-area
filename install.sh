#!/bin/bash
# Install trackpad-filter as a system service
# Run with sudo

set -e

BIN="trackpad-filter"
SERVICE="trackpad-filter.service"
INSTALL_BIN="/usr/local/bin/$BIN"
INSTALL_SERVICE="/etc/systemd/system/$SERVICE"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/install.log"

echo "Installing trackpad-filter..." | tee "$LOG"

# Build
echo "Building..." | tee -a "$LOG"
gcc -O2 -Wall -o "$SCRIPT_DIR/$BIN" "$SCRIPT_DIR/trackpad-filter.c" 2>&1 | tee -a "$LOG"

# Install binary
cp "$SCRIPT_DIR/$BIN" "$INSTALL_BIN"
chmod 755 "$INSTALL_BIN"
echo "Installed binary: $INSTALL_BIN" | tee -a "$LOG"

# Install service
cp "$SCRIPT_DIR/$SERVICE" "$INSTALL_SERVICE"
echo "Installed service: $INSTALL_SERVICE" | tee -a "$LOG"

# Reload and enable
systemctl daemon-reload
systemctl enable --now trackpad-filter.service 2>&1 | tee -a "$LOG"

echo "Done. Service status:" | tee -a "$LOG"
systemctl status trackpad-filter.service --no-pager 2>&1 | tee -a "$LOG"
