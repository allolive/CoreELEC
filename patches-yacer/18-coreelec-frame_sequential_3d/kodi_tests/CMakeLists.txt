# Tests for the frame-sequential 3D group.
#
# The Blu-ray subtitle depth parser and its tables are header-only and depend
# on nothing.

yacer_add_test(
  NAME MvcSubtitleDepth
  STANDARD 20
  SOURCES TestMvcSubtitleDepth.cpp
)

# The real Reorder/DropLate bodies and VideoPicture's reference handling,
# extracted at configure time: the codec itself needs the AML decoder stack.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${KODI_SOURCE}/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp"
  "${KODI_SOURCE}/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.cpp"
  "${KODI_SOURCE}/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
  "${CMAKE_SOURCE_DIR}/mvc_reorder_sections.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/mvc_reorder_sections.py"
  "${KODI_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/MvcReorderSections.inc"
  COMMAND_ERROR_IS_FATAL ANY)
yacer_add_test(
  NAME MvcReorder
  STANDARD 20
  SOURCES TestMvcReorder.cpp
  INCLUDES ${YACER_SHIMS} ${CMAKE_CURRENT_BINARY_DIR}
)
