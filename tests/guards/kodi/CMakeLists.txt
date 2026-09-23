# Upstream behaviour our patches rely on, and behaviour of ours that upstream
# has taken over. Nothing here is tied to a patch still being present.

yacer_add_test(
  NAME BitstreamReaderContract
  SOURCES TestBitstreamReaderContract.cpp
  KODI_SOURCES xbmc/utils/BitstreamReader.cpp
)
