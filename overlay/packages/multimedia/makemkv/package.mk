# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="makemkv"
PKG_VERSION="2.0.0"
PKG_SHA256="435316b2d219eb48c880526557addd076b5f5e6de5171424c7651f9cac95b161"
PKG_LICENSE="MakeMKV EULA, LGPL-2.1"
PKG_SITE="https://www.makemkv.com/"
PKG_URL="https://www.makemkv.com/download/${PKG_NAME}-oss-${PKG_VERSION}.tar.gz"
# keep PKG_VERSION the same as makemkv-bin's: libmmbd and makemkvcon come as a pair
PKG_DEPENDS_TARGET="toolchain ffmpeg openssl zlib expat makemkv-bin"
PKG_LONGDESC="libmmbd, which plays an encrypted Blu-ray through MakeMKV in place of libaacs and libbdplus."
PKG_TOOLCHAIN="configure"

PKG_CONFIGURE_OPTS_TARGET="--disable-gui"

# its Makefile reads its sources from the directory it runs in
pre_configure_target() {
  cd ${PKG_BUILD}
  rm -rf .${TARGET_NAME}
  export OBJCOPY LD
}

makeinstall_target() {
  make install DESTDIR=${INSTALL} prefix=/usr libdir=/usr/lib bindir=/usr/bin
}

post_makeinstall_target() {
  # libbluray takes libaacs and libbdplus first; Kodi asks it for libmmbd
  mkdir -p ${INSTALL}/usr/lib/systemd/system/kodi.service.d
  cp ${PKG_DIR}/system.d/kodi.service.d/makemkv.conf ${INSTALL}/usr/lib/systemd/system/kodi.service.d
}
