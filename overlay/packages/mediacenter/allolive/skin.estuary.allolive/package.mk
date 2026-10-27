# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="skin.estuary.allolive"
PKG_VERSION="4.1.0_211"
PKG_SHA256="d9c68f1e21c7b75dd50e9668779f19ce9a8a944a7857fe01e63846be31b0f535"
PKG_LICENSE="GPL-2.0-only AND CC-BY-SA-4.0"
PKG_SITE="https://github.com/allolive/kodi_addons"
PKG_URL="https://github.com/allolive/kodi_addons/releases/download/ce22/${PKG_NAME}-${PKG_VERSION}/${PKG_NAME}-${PKG_VERSION}.zip"
PKG_SOURCE_DIR="${PKG_NAME}"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="Estuary as these builds ship it, the skin a new profile starts on."
PKG_TOOLCHAIN="manual"

# the add-on as the allolive repository publishes it; a newer one from there
# installs into the user's add-ons and takes over
makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/kodi/addons
  cp -a ${PKG_BUILD} ${INSTALL}/usr/share/kodi/addons/${PKG_NAME}
}
