# The subtitle repairs. SubtitleCleanup.cpp holds every rule and reaches
# nothing of Kodi's, so the suite compiles it alone, with Hunspell for the
# OCR letters.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HUNSPELL REQUIRED IMPORTED_TARGET hunspell)

# No language is built into Kodi; the suite's English pack is the one the
# English add-on ships: LibreOffice's dictionaries at the pinned revision and
# the profile tests/subtitle-repair/make_pack.py writes for them.
set(pack "${CMAKE_CURRENT_BINARY_DIR}/packs/en")
set(dictionaries "https://raw.githubusercontent.com/LibreOffice/dictionaries/32b006a2c22a4ac7e8ed3f03346f7b3d85a970a4/en")
foreach(file IN ITEMS
    "en_US.aff;e746c882dd6f303c2c46e7452804b9201115a6942cfeb15f18f8edf774d2e24e"
    "en_US.dic;f0b1a234bd178bdd01875b2a392a9647f888b8fe879f79c52aae62c2759b3647"
    "en_GB.aff;0fd6ed120ef28957847d98ba5149b117e27116cf81b5aa36208453f6755a36fd"
    "en_GB.dic;04e90f34f5263bf26780e9c4a442e9ad16584e227af49ddd1b3b21b01df5b29c")
  list(GET file 0 name)
  list(GET file 1 sha)
  # EXPECTED_HASH reuses a verified file without downloading. An existence
  # check alone also accepts the empty file left by a failed download.
  file(DOWNLOAD "${dictionaries}/${name}" "${pack}/${name}"
       EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON)
endforeach()
file(WRITE "${pack}/profile.txt"
  "codes = en eng english\n"
  "dictionaries = en_US en_GB\n"
  "one_letter_words = a A I\n"
  "lone_one = am was know think don't can't didn't won't have had will would want need mean love just guess thought said do did like can could should hope swear promise told saw heard wish believe remember\n"
  "spell_cues = spell spells spelled spelt spelling letter letters initials\n"
  "capitalised_nouns = no\n"
  "alphabet = ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyzÅàâäçèéêîïñóôöü\n"
  "accents = A:Å a:àâä c:ç e:èéê i:îï n:ñ o:óôö u:ü\n")

yacer_add_test(
  NAME SubtitleCleanup
  SOURCES TestSubtitleCleanup.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.cpp
  DEFINES HAS_HUNSPELL SUBTITLE_PACKS="${CMAKE_CURRENT_BINARY_DIR}/packs"
  LIBRARIES PkgConfig::HUNSPELL
)

# The subtitle translation, without a provider: which track is offered, what a
# model is asked and how its answer is read, and the thread that works on cues
# in order, with a stand-in model. None of the files reaches anything of Kodi's
# (SubtitleCleanup.cpp gives the letter case, built without a dictionary); the
# provider is SubtitleAI.cpp.
yacer_add_test(
  NAME SubtitleTranslation
  SOURCES TestSubtitleTranslation.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleModel.cpp
               xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleTranslation.cpp
               xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.cpp
)

# The AI provider's corrections, without a provider: how its answer is read,
# which changes the rules allow, and what is shown when it fails. The rules
# read words through SubtitleCleanup.cpp, built here without a dictionary: the
# suite gives its own.
yacer_add_test(
  NAME SubtitleRevision
  SOURCES TestSubtitleRevision.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleModel.cpp
               xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleRevision.cpp
               xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.cpp
)

# Realigning a subtitle on another track of the same video: the fit, and how a
# picture subtitle's packet is read. Nothing of Kodi's is reached; reading the
# video is SubtitleCueStage.cpp's.
yacer_add_test(
  NAME SubtitleTiming
  SOURCES TestSubtitleTiming.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleTiming.cpp
)

# The change a subtitle file's lines need to sit on the video's speech, and the
# voice activity model's arithmetic. Hearing the video - decoding its sound -
# is SubtitleListener.cpp's, and the weights come from an add-on.
yacer_add_test(
  NAME SubtitleSpeech
  SOURCES TestSubtitleSpeech.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleSpeech.cpp
               xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleTiming.cpp
)
