# Tests for the frame-sequential 3D group.
#
# The Blu-ray subtitle depth parser and its tables are header-only and depend
# on nothing.

yacer_add_test(
  NAME MvcSubtitleDepth
  STANDARD 20
  SOURCES TestMvcSubtitleDepth.cpp
)
