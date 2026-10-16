# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="script.module.subtitlespeech.allolive"
PKG_VERSION="1.0.0"
PKG_SHA256="c9160985547695b581d50259a8cea290309337a4715db7ac6b4c03df8173d014"
PKG_LICENSE="MIT"
PKG_SITE="https://github.com/allolive/kodi_addons"
PKG_URL="https://github.com/allolive/kodi_addons/releases/download/ce22/${PKG_NAME}-${PKG_VERSION}/${PKG_NAME}-${PKG_VERSION}.zip"
PKG_SOURCE_DIR="${PKG_NAME}"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="Silero's voice activity model, which Kodi's subtitle realignment listens to the film with."
PKG_TOOLCHAIN="manual"

# the add-on as the allolive repository publishes it; a newer one from there
# installs into the user's add-ons and takes over
makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/kodi/addons
  cp -a ${PKG_BUILD} ${INSTALL}/usr/share/kodi/addons/${PKG_NAME}
}
