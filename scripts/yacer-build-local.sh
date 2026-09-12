#!/usr/bin/env bash
#
# Build the tree the way CI does, on this machine.
#
#   yacer-build-local.sh              incremental
#   CLEAN=1 yacer-build-local.sh      make distclean first
#   TARGET=image yacer-build-local.sh add the fresh-flash .img.gz
#   JOBS=N THREADS=N ...              override the concurrency
#
# Run it after 'yacer-local.sh apply'. It builds whatever is in the tree and
# does not touch the branch.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="${TREE:-$(cd "${HERE}/../../CoreELEC" && pwd)}"
[ -f "${TREE}/Makefile" ] || { echo "not a build tree: ${TREE}" >&2; exit 1; }

export PROJECT="${PROJECT:-Amlogic-ce}"
export DEVICE="${DEVICE:-Amlogic-no}"
export ARCH="${ARCH:-aarch64}"

CORES="$(nproc)"
export THREADCOUNT="${THREADS:-${CORES}}"
export CONCURRENCY_MAKE_LEVEL="${JOBS:-${CORES}}"
export CONCURRENCY_LOAD="${CONCURRENCY_LOAD:-$(python3 -c "print(f'{${CORES} * 1.5:.2f}')")}"

# Keep the compiler cache out of the build tree. config/path defaults CCACHE_DIR
# to $BUILD/.ccache, and scripts/makefile_helper removes that directory on every
# distclean - so the cache a clean build most needs is the one a clean build
# throws away first. CI has no cache at all: a runner is new every time and
# carrying one costs more than it saves.
export CCACHE_DIR="${CCACHE_DIR:-${HOME}/.ccache-coreelec}"
export LOCAL_CCACHE_DIR="${LOCAL_CCACHE_DIR:-${HOME}/.ccache-coreelec-local}"
export CCACHE_CACHE_SIZE="${CCACHE_CACHE_SIZE:-75G}"
export CCACHE_COMPRESSLEVEL="${CCACHE_COMPRESSLEVEL:-0}"
if command -v ccache >/dev/null 2>&1; then
  for d in "${CCACHE_DIR}" "${LOCAL_CCACHE_DIR}"; do
    CCACHE_DIR="${d}" ccache -M "${CCACHE_CACHE_SIZE}" >/dev/null 2>&1 || true
  done
fi

cd "${TREE}"

./scripts/checkdeps

# checkdeps predates glibc moving crypt() out to libxcrypt: the runtime
# libcrypt.so.1 is there, but heimdal:host links against libcrypt.so and only
# libcrypt-dev ships that. Without it the build dies a quarter of an hour in,
# on a package whose name says nothing about the cause.
if [ ! -e /usr/include/crypt.h ]; then
  echo "libcrypt-dev is missing - heimdal:host will fail to link on -lcrypt." >&2
  echo "  sudo apt install -y libcrypt-dev" >&2
  exit 1
fi

if [ "${CLEAN:-0}" = "1" ]; then
  make distclean
  ./scripts/clean linux || true
fi

echo "building ${PROJECT}/${DEVICE}/${ARCH} in ${TREE}"
time make "${TARGET:-release}"

./scripts/ccache_stats 2>/dev/null || true
ls -lh target/*.tar 2>/dev/null || true
