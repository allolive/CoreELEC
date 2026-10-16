# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="service.shutter3d.allolive"
PKG_VERSION="0.1.6"
PKG_SHA256="4845a23da890d36dd855ccfa0c6e4065cd9eab793ae5a766fe91f8029efdb0b9"
PKG_LICENSE="PolyForm-Noncommercial-1.0.0"
PKG_SITE="https://github.com/allolive/kodi_addons"
PKG_URL="https://github.com/allolive/kodi_addons/releases/download/ce22/${PKG_NAME}-${PKG_VERSION}/${PKG_NAME}-${PKG_VERSION}.zip"
PKG_SOURCE_DIR="${PKG_NAME}"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="Active 3D glasses for 120 Hz VRR displays."
PKG_TOOLCHAIN="manual"

# the add-on as the allolive repository publishes it; a newer one from there
# installs into the user's add-ons and takes over
makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/kodi/addons
  cp -a ${PKG_BUILD} ${INSTALL}/usr/share/kodi/addons/${PKG_NAME}
}
