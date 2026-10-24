# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="binaural"
PKG_VERSION="0.1.0"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://github.com/allolive/binaural"
PKG_DEPENDS_TARGET="toolchain camilladsp alsa-utils"
PKG_LONGDESC="Film sound to binaural headphones: Kodi plays 7.1 into an ALSA loopback, CamillaDSP convolves each speaker with the listener's measured responses."
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/binaural
  cp ${PKG_DIR}/config/camilladsp-7.1.yml ${INSTALL}/usr/share/binaural
  mkdir -p ${INSTALL}/usr/lib/modprobe.d
  cp ${PKG_DIR}/modprobe.d/binaural.conf ${INSTALL}/usr/lib/modprobe.d
}

post_install() {
  enable_service binaural.service
}
