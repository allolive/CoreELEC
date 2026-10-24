# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="camilladsp"
PKG_VERSION="4.1.3"
PKG_SHA256="7e2fdca1e9c73885426ac889d27682c76d49ea1ea52c5f5bfe5356d7fdfa52df"
PKG_LICENSE="GPL-3.0-only OR MPL-2.0"
PKG_SITE="https://github.com/HEnquist/camilladsp"
PKG_URL="https://github.com/HEnquist/camilladsp/archive/v${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain alsa-lib cargo:host"
PKG_LONGDESC="CamillaDSP, a convolution and filter engine; binaural uses it to turn Kodi's multichannel PCM into headphone sound."
PKG_TOOLCHAIN="manual"

make_target() {
  export RUSTC_LINKER=${CC}
  cargo build --target ${TARGET_NAME} --release
  ${STRIP} ${PKG_BUILD}/.${TARGET_NAME}/target/${TARGET_NAME}/release/camilladsp
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_BUILD}/.${TARGET_NAME}/target/${TARGET_NAME}/release/camilladsp ${INSTALL}/usr/bin
}
