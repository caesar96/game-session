#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

echo "==> Configuring…"
cmake -B build

echo "==> Building…"
cmake --build build

echo "==> Installing binaries + sudoers…"
sudo cmake --install build

echo "==> Done! Run 'game-session echo ok' to verify."
