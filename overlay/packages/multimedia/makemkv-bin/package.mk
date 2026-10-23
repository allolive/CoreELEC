# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="makemkv-bin"
PKG_VERSION="2.0.0"
PKG_SHA256="f1265e74875a186efdfbbbec7459a64e969033515e53cbc4d805f0a374f0a124"
PKG_LICENSE="MakeMKV EULA"
PKG_SITE="https://www.makemkv.com/"
PKG_URL="https://www.makemkv.com/download/${PKG_NAME}-${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="MakeMKV's program, which libmmbd asks to decrypt a Blu-ray disc."
PKG_TOOLCHAIN="manual"
# its licence lets a distribution repackage the Linux part as long as the
# program files stay unchanged: they are installed as they come
PKG_BUILD_FLAGS="-strip"
# repackaging is allowed, re-hosting the archive is not said to be: never in our mirror
YACER_MIRROR="no"
# MakeMKV moves a release to /download/old/ once the next one is out, and its builds stop
# decrypting after a while: bump this and makemkv's PKG_VERSION together to the newest

makeinstall_target() {
  local arch
  case "${TARGET_ARCH}" in
    aarch64) arch=arm64 ;;
    arm)     arch=armhf ;;
    x86_64)  arch=amd64 ;;
    *)       die "makemkv-bin: no MakeMKV program for ${TARGET_ARCH}" ;;
  esac
  mkdir -p ${INSTALL}/usr/bin ${INSTALL}/usr/share/MakeMKV
  # with the modes MakeMKV's own install gives them; the tarball keeps none
  install -m 755 bin/${arch}/makemkvcon ${INSTALL}/usr/bin
  install -m 644 src/share/appdata.tar src/share/blues.jar src/share/blues.policy \
                 src/eula_en_linux.txt ${INSTALL}/usr/share/MakeMKV
}
