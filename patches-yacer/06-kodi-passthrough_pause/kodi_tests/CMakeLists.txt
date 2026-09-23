# Tests for the passthrough pause/resume patches in this group.

yacer_add_test(
  NAME PlaybackPreroll
  SOURCES TestPlaybackPreroll.cpp
)

# The fence is header-only, so this needs no production translation units and
# no stand-ins: gmock supplies the work a ticket runs.
yacer_add_test(
  NAME PlaybackCallbackFence
  SOURCES TestPlaybackCallbackFence.cpp
)

# The ordering this pins is the message queue's own - a barrier must not
# overtake a seek the callback already queued - so the real queue and the real
# event and mutex primitives are compiled in rather than stood in for.
yacer_add_test(
  NAME PlaybackResumeGate
  STANDARD 20
  SOURCES TestPlaybackResumeGate.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDMessage.cpp
               xbmc/cores/VideoPlayer/DVDMessageQueue.cpp
               xbmc/threads/Event.cpp
               xbmc/platform/posix/threads/RecursiveMutex.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/platform/posix
           ${KODI_SOURCE}/xbmc/cores/VideoPlayer
)
