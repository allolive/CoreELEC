# Tests for the A/V sync work in this group.

# The observation window is header-only, so this needs no production
# translation units: the header under test is the code under test.
yacer_add_test(
  NAME AESyncObservation
  SOURCES TestAESyncObservation.cpp
  INCLUDES ${YACER_SHIMS}
)

# Also header-only. CSyncError takes its timer as a template parameter, so the
# clock is supplied by the test rather than waited for.
yacer_add_test(
  NAME AudioSyncReplay
  SOURCES TestAudioSyncReplay.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/cores/VideoPlayer
)

# The genlock's collaborators are all ordinary non-virtual functions, so the
# test binary defines them rather than mocking them.
yacer_add_test(
  NAME AMLGenlock
  STANDARD 20
  SOURCES TestAMLGenlock.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLGenlock.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/cores/VideoPlayer
)
