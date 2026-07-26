#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

PREFIX="${1:-/usr/local}"

echo "==> Removing binaries from ${PREFIX}/bin…"
sudo rm -fv "${PREFIX}/bin/game-session" "${PREFIX}/bin/game-session-helper"

echo "==> Removing sudoers…"
sudo rm -fv /etc/sudoers.d/game-session

echo "==> Removing build directory…"
rm -rfv build

echo "==> Done."
