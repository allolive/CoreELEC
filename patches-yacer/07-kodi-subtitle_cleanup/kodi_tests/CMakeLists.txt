# The subtitle repairs. SubtitleCleanup.cpp holds every rule and reaches
# nothing of Kodi's, so the suite compiles it alone; the word list is the
# one the patch installs.
yacer_add_test(
  NAME SubtitleCleanup
  SOURCES TestSubtitleCleanup.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.cpp
  DEFINES SUBTITLE_WORDS="${KODI_SOURCE}/system/subtitlecleanup/en_words.txt"
)
