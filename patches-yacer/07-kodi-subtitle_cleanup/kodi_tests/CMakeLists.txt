# The subtitle repairs. SubtitleCleanup.cpp holds every rule and reaches
# nothing of Kodi's, so the suite compiles it alone, with Hunspell for the
# OCR letters and the English pack the patch installs.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HUNSPELL REQUIRED IMPORTED_TARGET hunspell)

yacer_add_test(
  NAME SubtitleCleanup
  SOURCES TestSubtitleCleanup.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.cpp
  DEFINES HAS_HUNSPELL SUBTITLE_PACKS="${KODI_SOURCE}/system/subtitlecleanup"
  LIBRARIES PkgConfig::HUNSPELL
)
