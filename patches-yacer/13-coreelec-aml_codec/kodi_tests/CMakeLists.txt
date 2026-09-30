# Tests for the AML codec group.
#
# CAMLSeekPreroll is header-only and depends on nothing, so this needs no
# production translation units, no stand-ins and no seams at all.

yacer_add_test(
  NAME AMLSeekPreroll
  SOURCES TestAMLSeekPreroll.cpp
)

yacer_add_test(
  NAME DolbyVisionTiming
  SOURCES TestDolbyVisionTiming.cpp
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${KODI_SOURCE}/xbmc/cores/VideoPlayer/DVDDemuxers/DVDDemuxFFmpeg.cpp"
  "${CMAKE_SOURCE_DIR}/dovi_demux_sections.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/dovi_demux_sections.py"
  "${KODI_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/DoviDemuxSections.inc"
  COMMAND_ERROR_IS_FATAL ANY)
yacer_add_test(
  NAME DolbyVisionDemux
  SOURCES TestDolbyVisionDemux.cpp
  INCLUDES ${CMAKE_CURRENT_BINARY_DIR}
)

yacer_add_test(
  NAME DolbyVisionBitstream
  STANDARD 20
  SOURCES TestDolbyVisionBitstream.cpp BitstreamDependencies.cpp
  KODI_SOURCES xbmc/utils/BitstreamConverter.cpp xbmc/utils/HevcSei.cpp
               xbmc/utils/BitstreamReader.cpp
  INCLUDES ${YACER_SHIMS}
  DEFINES HAVE_LIBDOVI
)
