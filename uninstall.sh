#!/bin/bash
# Uninstall trackpad-filter service
# Run with sudo

set -e

systemctl stop trackpad-filter.service 2>/dev/null || true
systemctl disable trackpad-filter.service 2>/dev/null || true
rm -f /etc/systemd/system/trackpad-filter.service
rm -f /usr/local/bin/trackpad-filter
systemctl daemon-reload

echo "Uninstalled trackpad-filter."
