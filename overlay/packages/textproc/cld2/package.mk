# SPDX-License-Identifier: GPL-2.0-or-later

PKG_NAME="cld2"
PKG_VERSION="b56fa78a2fe44ac2851bae5bf4f4693a0644da7b"
PKG_SHA256="2bd0f8aa344698c0ce6b2c89f5540af10e69e92d4c74f9fe66ffe25281be1111"
PKG_LICENSE="Apache-2.0"
PKG_SITE="https://github.com/CLD2Owners/cld2"
PKG_URL="https://github.com/CLD2Owners/cld2/archive/${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="Compact Language Detector 2; Kodi's subtitle repair uses it to tell an untagged subtitle's language."
PKG_TOOLCHAIN="manual"
PKG_BUILD_FLAGS="+pic"

# The source list and tables of upstream's compile_full.sh: the full 0122
# tables, which tell apart the closer languages (Portuguese and Spanish, the
# Scandinavian ones) better than the small default set.
CLD2_SOURCES="cldutil.cc cldutil_shared.cc compact_lang_det.cc compact_lang_det_hint_code.cc
              compact_lang_det_impl.cc debug.cc fixunicodevalue.cc generated_entities.cc
              generated_language.cc generated_ulscript.cc getonescriptspan.cc lang_script.cc
              offsetmap.cc scoreonescriptspan.cc tote.cc utf8statetable.cc
              cld_generated_cjk_uni_prop_80.cc cld2_generated_cjk_compatible.cc
              cld_generated_cjk_delta_bi_32.cc generated_distinct_bi_0.cc
              cld2_generated_quad0122.cc cld2_generated_deltaocta0122.cc
              cld2_generated_distinctocta0122.cc cld_generated_score_quad_octa_0122.cc"

make_target() {
  cd "${PKG_BUILD}/internal"
  local src
  for src in ${CLD2_SOURCES}; do
    ${CXX} ${CXXFLAGS} -Wno-narrowing -c "${src}" -o "${src%.cc}.o" || return 1
  done
  ${AR} rcs libcld2.a ${CLD2_SOURCES//.cc/.o}
}

makeinstall_target() {
  mkdir -p "${SYSROOT_PREFIX}/usr/lib/pkgconfig" \
           "${SYSROOT_PREFIX}/usr/include/cld2/public" \
           "${SYSROOT_PREFIX}/usr/include/cld2/internal"
  cd "${PKG_BUILD}"
  cp internal/libcld2.a "${SYSROOT_PREFIX}/usr/lib"
  cp public/*.h "${SYSROOT_PREFIX}/usr/include/cld2/public"
  cp internal/*.h "${SYSROOT_PREFIX}/usr/include/cld2/internal"
  cat > "${SYSROOT_PREFIX}/usr/lib/pkgconfig/cld2.pc" << EOF_PC
prefix=/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib

Name: cld2
Description: Compact Language Detector 2
Version: 0.0.0
Cflags: -I\${includedir}/cld2
Libs: -L\${libdir} -lcld2
EOF_PC
}
