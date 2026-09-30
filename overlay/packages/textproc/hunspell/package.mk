# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="hunspell"
PKG_VERSION="1.7.2"
PKG_SHA256="11ddfa39afe28c28539fe65fc4f1592d410c1e9b6dd7d8a91ca25d85e9ec65b8"
PKG_LICENSE="MPL-1.1 OR GPL-2.0-or-later OR LGPL-2.1-or-later"
PKG_SITE="https://hunspell.github.io"
PKG_URL="https://github.com/hunspell/hunspell/releases/download/v${PKG_VERSION}/hunspell-${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="The spell checker of LibreOffice; Kodi's subtitle repair uses its dictionaries."
PKG_TOOLCHAIN="autotools"
PKG_BUILD_FLAGS="+pic"

PKG_CONFIGURE_OPTS_TARGET="--enable-static \
                           --disable-shared \
                           --disable-nls \
                           --without-ui \
                           --without-readline"

makeinstall_target() {
  make -C src/hunspell DESTDIR="${SYSROOT_PREFIX}" install
  mkdir -p "${SYSROOT_PREFIX}/usr/lib/pkgconfig"
  cp hunspell.pc "${SYSROOT_PREFIX}/usr/lib/pkgconfig"
}
