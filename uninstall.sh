#!/usr/bin/env bash
set -euo pipefail

echo "==> Removing binaries…"
sudo rm -fv /usr/local/bin/game-session /usr/local/bin/game-session-helper

echo "==> Removing sudoers…"
sudo rm -fv /etc/sudoers.d/game-session

echo "==> Removing build directory…"
rm -rfv build

echo "==> Done."
