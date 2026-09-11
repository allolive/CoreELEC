# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2022-present Team CoreELEC (https://coreelec.org)

PKG_NAME="gcc7-linaro-aarch64-elf"
PKG_VERSION="7.5.0-2019.12"
PKG_SHA256="73689fb3e71beeecebd6434d60efad4cb926153d48399e4d16fb45395d9c81a0"
PKG_LICENSE="GPL"
PKG_SITE="https://www.linaro.org/"
# Upstream releases.linaro.org is offline (host decommissioned). Self-hosted mirror of the
# byte-identical tarball; sha256 still verified against PKG_SHA256 above.
# Original: https://releases.linaro.org/components/toolchain/binaries/7.5-2019.12/aarch64-elf/gcc-linaro-${PKG_VERSION}-x86_64_aarch64-elf.tar.xz
PKG_URL="https://github.com/allolive/CoreELEC/releases/download/mirror-${PKG_NAME}-${PKG_VERSION}/${PKG_NAME}-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_HOST="ccache:host"
PKG_LONGDESC="Linaro Aarch64 GNU Linux Binary Toolchain"
PKG_TOOLCHAIN="manual"

unpack() {
  mkdir -p ${PKG_BUILD}
  mkdir -p ${TOOLCHAIN}/lib/${PKG_NAME}

  tar --strip-components=1 -xf ${SOURCES}/${PKG_NAME}/${PKG_NAME}-${PKG_VERSION}.tar.xz -C ${TOOLCHAIN}/lib/${PKG_NAME}
}

makeinstall_host() {
  : # nothing, unpacked directly to toolchain
}
