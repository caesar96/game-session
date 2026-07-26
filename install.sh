#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

echo "==> Building package…"
makepkg -cf --noconfirm

pkgfile=$(ls -t *.pkg.tar.zst 2>/dev/null | head -1)
if [[ -z "$pkgfile" ]]; then
    echo "error: no package file found"
    exit 1
fi

echo "==> Installing $pkgfile…"
sudo pacman -U "$pkgfile"

echo "==> Done! Run 'game-session echo ok' to verify."
