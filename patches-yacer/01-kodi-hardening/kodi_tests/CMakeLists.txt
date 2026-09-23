# Tests for the parser-hardening patches in this group.
#
# The third patch, which bounds the vvcC parser, has no suite here: reaching
# CBitstreamConverter::Open means linking FFmpeg's libraries and compiling
# Kodi's FFmpeg wrapper, which pulls in the settings and threading stacks.
# Extracting the vvcC parse into a translation unit of its own - the way
# 11-coreelec-av_sync/17 extracted CSyncError - would make it testable here.

yacer_add_test(
  NAME HevcSei
  SOURCES TestHevcSei.cpp
  KODI_SOURCES xbmc/utils/HevcSei.cpp xbmc/utils/BitstreamReader.cpp
)

yacer_add_test(
  NAME BitstreamReaderBounds
  SOURCES TestBitstreamReaderBounds.cpp
  KODI_SOURCES xbmc/utils/BitstreamReader.cpp
)
