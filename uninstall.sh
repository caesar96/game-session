#!/usr/bin/env bash
set -euo pipefail

echo "==> Uninstalling game-session…"
sudo pacman -R game-session

echo "==> Done."
