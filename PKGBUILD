# Maintainer: devd <devd@archlinux>
pkgname=game-session
pkgver=1.0
pkgrel=1
pkgdesc="AMD GPU overclock, fan curve, monitor HDR/presets and CPU optimizations for gaming"
arch=('x86_64')
url="https://github.com/caesar96/game-session"
license=('MIT')
depends=('glibc' 'gcc-libs' 'libkscreen')
makedepends=('cmake' 'gcc')
optdepends=(
    'game-performance: CPU governor and sleep inhibit (CachyOS)'
    'ddcutil: DDC/CI monitor preset switching'
)
provides=('game-session')
conflicts=('game-session')
source=()
sha256sums=()

prepare() {
    rm -rf "${srcdir}/${pkgname}"
    mkdir -p "${srcdir}/${pkgname}"
    # copy everything except makepkg working dirs and build artifacts
    for f in "${PWD}"/*; do
        case "${f##*/}" in
            src|pkg|build) ;;
            *) cp -r "$f" "${srcdir}/${pkgname}/" ;;
        esac
    done
}

build() {
    cd "${srcdir}/${pkgname}"
    cmake -B build \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build build
}

package() {
    cd "${srcdir}/${pkgname}"
    DESTDIR="${pkgdir}" cmake --install build

    # Generate sudoers with wheel group so any admin user can use it
    install -Dm440 /dev/stdin "${pkgdir}/etc/sudoers.d/game-session" <<EOF
%wheel ALL=(ALL) NOPASSWD: /usr/bin/game-session-helper
EOF
}
