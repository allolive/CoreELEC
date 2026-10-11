# Tests for the patches in this group.
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

# The real queue and synchronise message, so the drain a stop has to end is
# the one the stream players wait in.
yacer_add_test(
  NAME MessageQueueDrain
  STANDARD 20
  SOURCES TestMessageQueueDrain.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDMessage.cpp
               xbmc/cores/VideoPlayer/DVDMessageQueue.cpp
               xbmc/threads/Event.cpp
               xbmc/platform/posix/threads/RecursiveMutex.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/platform/posix
           ${KODI_SOURCE}/xbmc/cores/VideoPlayer
)

# Header-only: the player's decision each pass while an old title plays out.
yacer_add_test(
  NAME NextStreamDrain
  SOURCES TestNextStreamDrain.cpp
)
