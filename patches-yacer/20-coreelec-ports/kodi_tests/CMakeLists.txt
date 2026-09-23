# Tests for the ported work in this group.
#
# The HDR10+ parser reads counts out of the payload and CBitstreamReader does
# not bound itself, so the gates in the parser are the only thing standing
# between a malformed stream and a read past the end. These pin them.

yacer_add_test(
  NAME Hdr10PlusParser
  STANDARD 20
  SOURCES TestHdr10PlusParser.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLHdr10Plus.cpp
               xbmc/utils/BitstreamReader.cpp xbmc/utils/HevcSei.cpp
  INCLUDES ${YACER_SHIMS}
)

# The converter itself. libdovi is a built dependency, so the test binary
# defines its six entry points - a link seam, not a mock: no framework can
# hook a C function, and none is needed.
yacer_add_test(
  NAME Hdr10PlusToDv
  STANDARD 20
  SOURCES TestHdr10PlusToDv.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLHdr10PlusToDv.cpp
               xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLHdr10Plus.cpp
               xbmc/utils/BitstreamReader.cpp xbmc/utils/HevcSei.cpp
  INCLUDES ${YACER_SHIMS}
  DEFINES HAVE_LIBDOVI
)

# Dialogue-normalisation defeat. libavutil's crc.c is compiled in from the
# pinned tarball rather than stood in for: a checksum test that computes the
# checksum with an imitation proves nothing.
yacer_add_test(
  NAME DialNorm
  STANDARD 20
  SOURCES TestDialNorm.cpp
  KODI_SOURCES xbmc/cores/AudioEngine/Utils/AEStreamInfo.cpp
               .yacer-src/libavutil/crc.c
  INCLUDES ${YACER_SHIMS}
)
