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
  if(NOT EXISTS "${pack}/${name}")
    file(DOWNLOAD "${dictionaries}/${name}" "${pack}/${name}"
         EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON)
  endif()
endforeach()
file(WRITE "${pack}/profile.txt"
  "codes = en eng english\n"
  "dictionaries = en_US en_GB\n"
  "one_letter_words = a A I\n"
  "lone_one = am was know think don't can't didn't won't have had will would want need mean love just guess thought said do did like can could should hope swear promise told saw heard wish believe remember\n"
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
