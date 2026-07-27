# Maintainer: devd <devd@archlinux>
pkgname=game-session
pkgver=1.0
pkgrel=1
pkgdesc="AMD GPU overclock, fan curve, monitor HDR/presets and CPU optimizations for gaming"
arch=('x86_64')
url="https://github.com/caesar96/game-session"
license=('MIT')
depends=('glibc' 'gcc-libs' 'libkscreen' 'game-performance')
makedepends=('cmake' 'gcc')
optdepends=(
    'ddcutil: DDC/CI monitor preset switching'
)
provides=('game-session')
conflicts=('game-session')
source=()
sha256sums=()

build() {
    cmake -S "${startdir}" -B "${startdir}/build" \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${startdir}/build"
}

package() {
    DESTDIR="${pkgdir}" cmake --install "${startdir}/build"

    # Generate sudoers with wheel group so any admin user can use it
    install -Dm440 /dev/stdin "${pkgdir}/etc/sudoers.d/game-session" <<EOF
%wheel ALL=(ALL) NOPASSWD: /usr/bin/game-session-helper
EOF
}
