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
