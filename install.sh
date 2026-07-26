#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

echo "==> Configuring…"
cmake -B build

echo "==> Building…"
cmake --build build

echo "==> Installing binaries…"
sudo cmake --install build

echo "==> Installing sudoers…"
sudo cmake --install build --component system

echo "==> Done! Run 'game-session echo ok' to verify."
