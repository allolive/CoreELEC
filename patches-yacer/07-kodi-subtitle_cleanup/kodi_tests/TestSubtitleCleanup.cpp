/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The subtitle repairs, on lines taken from the subtitles of a real library.
 *  What a rule does not fix must come out exactly as it went in: every case
 *  that repairs nothing compares the whole file, byte for byte.
 */

#include "cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::SUBTITLES::CLEANUP;

namespace
{
std::string Stamp(int ms)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", ms / 3600000, ms / 60000 % 60,
                ms / 1000 % 60, ms % 1000);
  return buf;
}

//! Cue i shown from i s to i.9 s.
std::string Srt(std::initializer_list<std::string> texts)
{
  std::string out;
  int i = 0;
  for (const std::string& text : texts)
  {
    ++i;
    if (i > 1)
      out += "\n\n";
    out += std::to_string(i) + "\n" + Stamp(i * 1000) + " --> " + Stamp(i * 1000 + 900) + "\n" +
           text;
  }
  return out + "\n";
}

struct Timed
{
  int start;
  int end;
  std::string text;
};

std::string Srt(std::initializer_list<Timed> cues)
{
  std::string out;
  int i = 0;
  for (const Timed& cue : cues)
  {
    ++i;
    if (i > 1)
      out += "\n\n";
    out += std::to_string(i) + "\n" + Stamp(cue.start) + " --> " + Stamp(cue.end) + "\n" +
           cue.text;
  }
  return out + "\n";
}

struct Result
{
  bool cleaned;
  std::string out;
  Report report;
};

Result Clean(const std::string& data,
           const std::string& language = "",
           const Packs* packs = nullptr,
           Options options = Options(),
           const Detector& detect = {})
{
  Result r;
  r.cleaned = CleanSubRip(data, language, options, packs, detect, r.out, r.report);
  return r;
}

//! The text of each cue.
std::vector<std::string> Cues(const std::string& out)
{
  std::vector<std::string> texts;
  size_t start = 0;
  while (start < out.size())
  {
    size_t end = out.find("\n\n", start);
    if (end == std::string::npos)
      end = out.size();
    std::string block = out.substr(start, end - start);
    if (!block.empty() && block.back() == '\n')
      block.pop_back();
    const size_t first = block.find('\n');
    const size_t second = block.find('\n', first + 1);
    texts.push_back(second == std::string::npos ? "" : block.substr(second + 1));
    start = end + 2;
  }
  return texts;
}

std::vector<std::string> Timings(const std::string& out)
{
  std::vector<std::string> lines;
  std::istringstream in(out);
  std::string line;
  while (std::getline(in, line))
    if (line.find("-->") != std::string::npos)
      lines.push_back(line);
  return lines;
}

//! Whether a line in the middle of a film is taken for an advertisement.
bool IsAdMidFilm(const std::string& line)
{
  return Clean(Srt({"a", "b", "c", line, "d", "e", "f"})).report.adLines == 1;
}

//! The English pack the patch installs.
const Packs& English()
{
  static const Packs packs = LoadPacks(SUBTITLE_PACKS);
  return packs;
}

void Write(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream(path, std::ios::binary) << text;
}

//! English and a small French pack: enough words for a French subtitle, and the French for a
//! few an English one has too.
const Packs& EnglishAndFrench()
{
  static const Packs packs = [] {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("subtitle-packs-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root / "fr");
    std::filesystem::copy(SUBTITLE_PACKS "/en", root / "en",
                          std::filesystem::copy_options::recursive);
    Write(root / "fr" / "profile.txt",
          "codes = fr fre fra french\n"
          "dictionaries = fr\n"
          "one_letter_words = a A à À y Y ô Ô\n"
          "capitalised_nouns = no\n"
          "alphabet = ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyzÀÉàâçèéêëîïôùûü\n"
          "accents = A:À E:É a:àâ c:ç e:èéêë i:îï o:ô u:ùûü\n");
    Write(root / "fr" / "fr.aff", "SET UTF-8\n");
    const char* words[] = {"la",     "le",   "les",   "il",    "elle",   "est",   "là",
                           "ici",    "je",   "ne",    "sais",  "pas",    "vous",  "voulez",
                           "maison", "dans", "que",   "qui",   "un",     "une",   "et",
                           "de",     "ce",   "merci", "oui",   "non",    "écoutez", "été",
                           "très",   "café", "thé",   "mère",  "père",   "déjà",  "où",
                           "a",      "bu",   "du",    "tu",    "viens",  "mon",   "ami",
                           "vérité", "désolé", "suis"};
    std::string dic = std::to_string(std::size(words)) + "\n";
    for (const char* w : words)
      dic += std::string(w) + "\n";
    Write(root / "fr" / "fr.dic", dic);
    Packs loaded = LoadPacks(root.string());
    std::filesystem::remove_all(root);
    return loaded;
  }();
  return packs;
}

//! An English file whose OCR reads "I" as "l" all through: in several words, many times.
std::vector<std::string> MisreadsI()
{
  return {"l know what you mean.", "l'm sure of it.", "l'll be there.", "Can l come?",
          "l think so.",           "l'm not going.",  "What do l do?",  "l'll call you."};
}

std::string SrtOf(const std::vector<std::string>& texts)
{
  std::string out;
  for (size_t i = 0; i < texts.size(); ++i)
  {
    if (i)
      out += "\n\n";
    out += std::to_string(i + 1) + "\n" + Stamp(static_cast<int>(i + 1) * 1000) + " --> " +
           Stamp(static_cast<int>(i + 1) * 1000 + 900) + "\n" + texts[i];
  }
  return out + "\n";
}

std::vector<std::string> Plus(std::vector<std::string> a, const std::vector<std::string>& b)
{
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
} // namespace

// -- whole files ----------------------------------------------------------------------------

TEST(SubtitleCleanup, LeavesACleanFileAlone)
{
  const std::string text = Srt({"<i>Hello there.</i>", "- Who's that?\n- Nobody.", "#260 is the room."});
  const Result r = Clean(text);
  EXPECT_EQ(r.out, text);
  EXPECT_FALSE(r.report.Changed());
}

TEST(SubtitleCleanup, PutsBackMusicNotesReadAsWindows1252)
{
  const Result r = Clean(Srt({">> â™ª THERE'S A HOLE IN THE", "â™ª AND A CONE ON THE HOLE â™ª"}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{">> ♪ THERE'S A HOLE IN THE",
                                                   "♪ AND A CONE ON THE HOLE ♪"}));
  EXPECT_EQ(r.report.repairedLines, 2);
}

TEST(SubtitleCleanup, UndoesMojibakeTwiceOver)
{
  const Result r = Clean(Srt({"Caf\xc3\x83\xc6\x92\xc3\x82\xc2\xa9 \xc3\x83\xc2\xa2\xc3\xa2\xe2\x80\x9e"
                            "\xc2\xa2\xc3\x82\xc2\xaa"}));
  EXPECT_EQ(Cues(r.out), std::vector<std::string>{"Café ♪"});
}

TEST(SubtitleCleanup, TakesRealAccentsForWhatTheyAre)
{
  const std::string text = Srt({"Não é possível.", "À bientôt, ça va?"});
  const Result r = Clean(text);
  EXPECT_EQ(r.out, text);
  EXPECT_EQ(r.report.repairedLines, 0);
}

TEST(SubtitleCleanup, ReadsACodePageFileInTheLanguageItsNameGives)
{
  const Result r = Clean(Srt({"\xE3\xC7\xD0\xC7 \xCD\xCF\xCB\xBF"}), "Arabic");
  EXPECT_EQ(r.report.readAs, "cp1256");
  EXPECT_EQ(Cues(r.out), std::vector<std::string>{"ماذا حدث؟"});
}

TEST(SubtitleCleanup, LeavesAnUnlabelledForeignCodePageAlone)
{
  std::string hebrew;
  for (int i = 0; i < 10; ++i)
    hebrew += "\xEE\xE4 \xF7\xF8\xE4 \xF4\xE4? ";
  const Result r = Clean(Srt({hebrew, hebrew}));
  EXPECT_FALSE(r.cleaned);
  EXPECT_EQ(r.report.readAs, "unsure");
  EXPECT_FALSE(r.report.Changed());
}

TEST(SubtitleCleanup, TurnsPilcrowsIntoNotesEvenAgainstATag)
{
  const Result r = Clean(Srt({"¶<i> in all the wires</i>", "<i>¶¶ Happy day ¶¶</i>", ">> MAN: ¶ la, la"}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"♪<i> in all the wires</i>",
                                                   "<i>♪ Happy day ♪</i>", ">> MAN: ♪ la, la"}));
}

TEST(SubtitleCleanup, ReadsHashAsANoteOnlyWhereTheFileWritesNotesThatWay)
{
  const std::string tags = Srt({"#random #doable.", "#260", "#Zing, zing"});
  EXPECT_EQ(Clean(tags).out, tags);
  const Result r = Clean(Srt({"#ah-ah-ah-ah#", "#Zing, zing, zing", "- # Take me home #", "#---#"}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"♪ah-ah-ah-ah♪", "♪Zing, zing, zing",
                                                   "- ♪ Take me home ♪", "♪---♪"}));
}

TEST(SubtitleCleanup, ReadsSongsOpeningWithHashAndASpace)
{
  const Result r = Clean(Srt({"# Love is in the air", "# She walks softly", "# Yeah yeah", "#260 is free."}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"♪ Love is in the air", "♪ She walks softly",
                                                   "♪ Yeah yeah", "#260 is free."}));
  const std::string two = Srt({"# Love is in the air", "# Yeah yeah", "#random"});
  EXPECT_EQ(Clean(two).out, two);
}

TEST(SubtitleCleanup, LeavesMarksInsideALabelOrATag)
{
  const std::string text = Srt({"<i>{Man ¶¶ 2} And we're clear.</i>", "<font color=\"#ffffff\">DOOR OPENS</font>"});
  EXPECT_EQ(Clean(text).out, text);
}

TEST(SubtitleCleanup, PutsBackDamagedCharacters)
{
  const Result r = Clean(Srt({"tout près de mon c\xc2\x9cur.", "He \xE2\x80\x8B" "\xE2\x80\x8B" "is coming soon",
                            "I can ﬂy, ﬁnally.", "de Big Brother &amp; The Holding",
                            "Merci d&#039;avoir", "wait\x7f"}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"tout près de mon cœur.", "He is coming soon",
                                                   "I can fly, finally.",
                                                   "de Big Brother & The Holding",
                                                   "Merci d'avoir", "wait"}));
}

TEST(SubtitleCleanup, LeavesDirectionMarksAndOtherScriptsAlone)
{
  const std::string text = Srt({"\xE2\x80\x8E" "NETFLIX シリーズ", "\xE2\x80\xAB" "360.\xE2\x80\xAC" "", "- \xE2\x80\xAD" "Oh, God.",
                                "שלום\xE2\x80\x8F" "!", "- Úžasný. Jste umělec.", "Éš Íž Äš"});
  const Result r = Clean(text);
  EXPECT_EQ(r.out, text);
  EXPECT_FALSE(r.report.Changed());
}

TEST(SubtitleCleanup, ReadsAMixedEncodingFileLineByLine)
{
  const std::string data = "1\n00:00:01,000 --> 00:00:02,000\nHere is my résumé.\n\n"
                           "2\n00:00:03,000 --> 00:00:04,000\nCaf\xE9 cr\xE8me\n";
  const Result r = Clean(data, "en");
  EXPECT_EQ(r.report.readAs, "mixed:cp1252");
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"Here is my résumé.", "Café crème"}));
}

TEST(SubtitleCleanup, ReadsCuesAfterABlankLineOrAStrayNumber)
{
  const Result r = Clean("1\n00:00:01,000 --> 00:00:02,000\n¶ One ¶\n \n\n"
                       "776\n799\n00:00:03,000 --> 00:00:04,000\n¶ Two ¶\n");
  EXPECT_NE(r.out.find("♪ One ♪"), std::string::npos);
  EXPECT_NE(r.out.find("776\n799\n00:00:03,000"), std::string::npos);
  EXPECT_NE(r.out.find("♪ Two ♪"), std::string::npos);
}

TEST(SubtitleCleanup, TurnsPairedOcrMarksAndEmojiIntoNotes)
{
  const Result r = Clean(Srt({"J Happy birthday to you J", "Just me J", "I love \U0001F3B5 music"}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"♪ Happy birthday to you ♪", "Just me J",
                                                   "I love ♪ music"}));
}

TEST(SubtitleCleanup, RemovesAdsAndRenumbers)
{
  const Result r = Clean(Srt({"Support us and become VIP member\nto remove all ads from www.OpenSubtitles.org",
                            "Good morning.", "Sync & corrections by honeybunny", "Goodbye.", "Good.",
                            "Bye."}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"Good morning.", "Goodbye.", "Good.", "Bye."}));
  EXPECT_EQ(r.out.compare(0, 2, "1\n"), 0);
  EXPECT_NE(r.out.find("\n2\n"), std::string::npos);
  EXPECT_EQ(r.report.adLines, 3);
  EXPECT_EQ(r.report.cuesDropped, 2);
}

TEST(SubtitleCleanup, RemovesAnAdLineButKeepsTheDialogueBesideIt)
{
  const Result r = Clean(Srt({"a", "b", "c", "<font color=#ffff00>www.addic7ed.com</font>\nWhere were we?",
                            "d", "e", "f"}));
  EXPECT_EQ(Cues(r.out)[3], "Where were we?");
}

TEST(SubtitleCleanup, RemovesATwoLineAdButNotTheDialogueSharingACueWithOne)
{
  const Result r = Clean(Srt({"Use the free code JOINNOW at\n\xE2\x80\xA8" "www.playships.eu", "Hello.", "a",
                            "Woohoo\nWWW.CRAZY-TORRENT.COM 2010."}));
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{"Hello.", "a", "Woohoo"}));
  EXPECT_EQ(r.report.cuesDropped, 1);
}

TEST(SubtitleCleanup, KnowsCreditsAndOffersFromDialogue)
{
  for (const char* line :
       {"Released on www.Danishbits.org", "Fixed & Synced by bozxphd. Enjoy The Flick",
        "-Subtitled by limos88-", "subedit By: SharePirate.Com", "WEB DL sync by VeRdiKT",
        "A subtitle made possible by R3V0LV3R.", "Sous-titres par (Subtitles by)",
        "Signup Here -> WWW.ADMIT1.APP", "REGARDEZ TOUTE LA VOD EN HQ @4KVOD.TV",
        "Subtitles: WayFilmTranslation.com,", "Find out @ saveanilluminati.com",
        "https://twitter.com/kaboomskull", "Watch Movies, TV Series and Live Sports",
        "Surf the internet with browser of future", "Save 25%, use code: OPEN25",
        "Vertaling: r3p0", "Översatt av:", "Improved by: @Ivandrofly", "Sync & corrections: Reef",
        "BluRay resync by GoldenBeard", "Translation: foxhidden",
        "Claim a $75 discount on your next hotel bookings."})
    EXPECT_TRUE(IsAdMidFilm(line)) << line;
  for (const char* line :
       {"Captioned by", "Media Access Group at WGBH", "PartyPoker.net.",
        "It's dawnoftime.com. Like, Dawn.", "Woohoo", "Enjoy!", "Translation is hard.",
        "I downloaded from the server.", "Here's your code: 1234."})
    EXPECT_FALSE(IsAdMidFilm(line)) << line;
}

TEST(SubtitleCleanup, KeepsDialogueThatMentionsAnAddressOrATranslator)
{
  const std::string text = Srt({"a", "b", "c", "It was translated by my grandmother.",
                                "Go to www.example.com now.", "d", "e", "Wuphf.com."});
  const Result r = Clean(text);
  EXPECT_EQ(r.out, text);
  EXPECT_EQ(r.report.adLines, 0);
}

TEST(SubtitleCleanup, KeepsAdsWhenAskedTo)
{
  const std::string text = Srt({"Subtitles by explosiveskull", "Hello."});
  Options options;
  options.removeAds = false;
  EXPECT_EQ(Clean(text, "", nullptr, options).out, text);
}

TEST(SubtitleCleanup, ReadsWindowsLineEndingsAndAByteOrderMark)
{
  const Result r = Clean("\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,000\r\n¶ Hey ¶\r\n");
  EXPECT_EQ(Cues(r.out), std::vector<std::string>{"♪ Hey ♪"});
}

TEST(SubtitleCleanup, FindsTheLanguageInTheFileName)
{
  EXPECT_EQ(LanguageOfPath("/m/Film (2001)/Film.en.forced.srt"), "en");
  EXPECT_EQ(LanguageOfPath("/m/Film/Subs/2_English.srt"), "English");
  EXPECT_EQ(LanguageOfPath("smb://nas/Film/Arabic.srt"), "Arabic");
  EXPECT_EQ(LanguageOfPath("/m/Film/Film.forced.srt"), "");
  EXPECT_EQ(LanguageOfPath("/m/Film/Film-bg.REMUX-Danishbits.da.srt"), "da");
  EXPECT_EQ(LanguageOfPath("/m/Film/Film HQ NL Subs.en.srt"), "en");
}

// -- OCR letters ----------------------------------------------------------------------------

TEST(SubtitleCleanup, LoadsTheEnglishPack)
{
  ASSERT_EQ(English().size(), 1u);
  EXPECT_EQ(English()[0]->Name(), "en");
  EXPECT_TRUE(English()[0]->HasCode("ENG"));
  EXPECT_TRUE(English()[0]->Word(L"colour"));
  EXPECT_TRUE(English()[0]->Word(L"color"));
  EXPECT_FALSE(English()[0]->Word(L"wouId"));
}

TEST(SubtitleCleanup, UndoesAMisreadingTheFileRepeats)
{
  const Result r = Clean(SrtOf(Plus(MisreadsI(), {"<i>l was there.</i>"})), "en", &English());
  const std::vector<std::string> cues = Cues(r.out);
  EXPECT_EQ(cues[0], "I know what you mean.");
  EXPECT_EQ(cues[1], "I'm sure of it.");
  EXPECT_EQ(cues[3], "Can I come?");
  EXPECT_EQ(cues[8], "<i>I was there.</i>");
  EXPECT_EQ(r.report.ocrFixes, 9);
  EXPECT_EQ(r.report.ocrLanguage, "en");
  EXPECT_EQ(r.report.ocrPattern, "l->I");
}

TEST(SubtitleCleanup, LeavesAMisreadingThatIsAnException)
{
  // one "l" for "I" among many written right is an exception, not the file's pattern
  std::vector<std::string> texts;
  for (int i = 0; i < 12; ++i)
    texts.push_back("I know. I'm sure. I'll go.");
  texts.push_back("l know.");
  const std::string text = SrtOf(texts);
  const Result r = Clean(text, "en", &English());
  EXPECT_EQ(r.out, text);
  EXPECT_EQ(r.report.ocrPattern, "");
}

TEST(SubtitleCleanup, LeavesACleanFileWithOddWordsAlone)
{
  const std::string text = Srt({"Gary, Isabelle and Ig went to lx.",
                                "<font color=\"#llllll\">Hello.</font>", "Nest0r waited.",
                                "Episode 1: the beginning.", "Louis II was king."});
  const Result r = Clean(text, "en", &English());
  EXPECT_EQ(r.out, text);
  EXPECT_EQ(r.report.ocrFixes, 0);
}

TEST(SubtitleCleanup, LeavesOcrLettersAloneWithoutAPackForTheLanguage)
{
  const std::string text = SrtOf(MisreadsI());
  EXPECT_EQ(Clean(text, "da", &English()).out, text);
  EXPECT_EQ(Clean(text, "fr", &English()).out, text);
  EXPECT_EQ(Clean(text, "en", nullptr).out, text);
  Options off;
  off.fixOcr = false;
  EXPECT_EQ(Clean(text, "en", &English(), off).out, text);
}

TEST(SubtitleCleanup, AsksTheDetectorOnlyForAFileThatNamesNoLanguage)
{
  int asked = 0;
  const Detector english = [&](const std::string& text) {
    ++asked;
    EXPECT_NE(text.find("l know what you mean."), std::string::npos);
    return Detection{"en", 97, true};
  };
  const std::string text = SrtOf(MisreadsI());
  EXPECT_EQ(Cues(Clean(text, "", &English(), Options(), english).out)[0],
            "I know what you mean.");
  EXPECT_EQ(asked, 1);
  Clean(text, "en", &English(), Options(), english);
  EXPECT_EQ(asked, 1);

  const Detector unsure = [](const std::string&) { return Detection{"en", 97, false}; };
  const Detector mixed = [](const std::string&) { return Detection{"en", 70, true}; };
  EXPECT_EQ(Clean(text, "", &English(), Options(), unsure).out, text);
  EXPECT_EQ(Clean(text, "", &English(), Options(), mixed).out, text);
  EXPECT_EQ(Clean(text, "", &English()).out, text);
}

TEST(SubtitleCleanup, NeverTouchesAnLInsideAnAccentedWord)
{
  const Result r = Clean(SrtOf(Plus(MisreadsI(), {"Oh là là!", "Miss Hélène, may l drink",
                                                  "Vielleicht läuft es super,"})),
                         "en", &English());
  const std::vector<std::string> cues = Cues(r.out);
  EXPECT_EQ(cues[8], "Oh là là!");
  EXPECT_EQ(cues[9], "Miss Hélène, may I drink");
  EXPECT_EQ(cues[10], "Vielleicht läuft es super,");
}

TEST(SubtitleCleanup, LeavesStuttersSpellingAndDamagedWordsAlone)
{
  const std::vector<std::string> odd = {"Ba-ba-ba-batter-r-ry l-l-l-low.", "and lithic... l-T-H...",
                                        "Sa?l! Sa?l!", "just north of the l-20 junction."};
  const std::vector<std::string> cues =
      Cues(Clean(SrtOf(Plus(MisreadsI(), odd)), "en", &English()).out);
  EXPECT_EQ(std::vector<std::string>(cues.begin() + 8, cues.end()), odd);
}

TEST(SubtitleCleanup, LeavesALineAnotherLanguageExplainsBetter)
{
  const Result r = Clean(SrtOf(Plus(MisreadsI(), {"Je ne sais pas, il est là.",
                                                  "- Can l have a café,"})),
                         "en", &EnglishAndFrench());
  const std::vector<std::string> cues = Cues(r.out);
  EXPECT_EQ(cues[8], "Je ne sais pas, il est là.");
  EXPECT_EQ(cues[9], "- Can I have a café,");
}

TEST(SubtitleCleanup, LeavesNamesTheFileUsesMidSentence)
{
  // the file drops "d" for "cl" all through, but "Moclan" is a name it uses mid-sentence
  const Result r = Clean(SrtOf({"l coulcl go.", "She woulcl know.", "We shoulcl stay.",
                                "They coulcl try.", "You woulcl see.", "Ask Moclan.",
                                "Where is Moclan?"}),
                         "en", &English());
  const std::vector<std::string> cues = Cues(r.out);
  EXPECT_EQ(cues[1], "She would know.");
  EXPECT_EQ(cues[5], "Ask Moclan.");
  EXPECT_EQ(cues[6], "Where is Moclan?");
}

TEST(SubtitleCleanup, PutsBackOnlyTheAccentsTheFileDropsAllThrough)
{
  // "é" is dropped all through; "è" once and "É" once are exceptions
  const Result r = Clean(SrtOf({"Il a ete ici.", "La verite est la.", "Je suis desole.",
                                "Il a ete la.", "La verite, merci.", "Ecoutez, je suis la.",
                                "La mere est la."}),
                         "fr", &EnglishAndFrench());
  EXPECT_EQ(Cues(r.out), (std::vector<std::string>{
                             "Il a été ici.", "La vérité est la.", "Je suis désolé.",
                             "Il a été la.", "La vérité, merci.", "Ecoutez, je suis la.",
                             "La mere est la."}));
  EXPECT_EQ(r.report.ocrPattern, "e->é");
}

TEST(SubtitleCleanup, LeavesAnAccentOffAWordAnotherLanguageHas)
{
  const std::vector<std::string> caps = {"IL A ETE ICI.",  "LA VERITE EST LA.",
                                         "JE SUIS DESOLE.", "IL A ETE LA.",
                                         "LA VERITE, MERCI.", "LE THE EST LA."};
  const Result r = Clean(SrtOf(caps), "fr", &EnglishAndFrench());
  EXPECT_EQ(Cues(r.out)[0], "IL A ÉTÉ ICI.");
  EXPECT_EQ(Cues(r.out)[5], "LE THE EST LA.");
  // with no English installed, nothing says "THE" is a word
  const Packs french = {EnglishAndFrench()[1]};
  ASSERT_EQ(french[0]->Name(), "fr");
  EXPECT_EQ(Cues(Clean(SrtOf(caps), "fr", &french).out)[5], "LE THÉ EST LA.");
}

TEST(SubtitleCleanup, ReadsALoneOneAsIOnlyBeforeAWordIGoesWith)
{
  // the file writes "1" for "I" in words ("1'm"), so a lone "1" before "know" is "I" too
  const Result r = Clean(SrtOf({"1'm here.", "1'll go.", "1've seen it.", "1'm sure.", "1'll try.",
                                "1 know.", "1 day left.", "Chapter 1."}),
                         "en", &English());
  const std::vector<std::string> cues = Cues(r.out);
  EXPECT_EQ(cues[0], "I'm here.");
  EXPECT_EQ(cues[5], "I know.");
  EXPECT_EQ(cues[6], "1 day left.");
  EXPECT_EQ(cues[7], "Chapter 1.");
}

// -- timing ---------------------------------------------------------------------------------

TEST(SubtitleCleanup, TrustsTimingThatWorks)
{
  const std::string text =
      Srt({Timed{1000, 1200, "Short but seen."}, Timed{1240, 3000, "Gap under a frame."},
           Timed{3083, 5000, "Two-frame gap."}, Timed{4990, 7000, "Overlap under a frame."},
           Timed{7000, 30000, "[♪...]"}, Timed{8000, 12000, "{\\an8}A sign over the dialogue."}});
  const Result r = Clean(text);
  EXPECT_EQ(r.out, text);
  EXPECT_EQ(r.report.timingFixes, 0);
}

TEST(SubtitleCleanup, GivesALineThatCannotBeSeenASecondOrUntilTheNext)
{
  const Result r = Clean(Srt({Timed{2000, 2000, "Hello?"}, Timed{2700, 3500, "Katie!"},
                            Timed{9000, 9050, "Oh."}, Timed{20000, 19998, "Bye."}}));
  EXPECT_EQ(Timings(r.out), (std::vector<std::string>{
                                "00:00:02,000 --> 00:00:02,700", "00:00:02,700 --> 00:00:03,500",
                                "00:00:09,000 --> 00:00:10,000", "00:00:20,000 --> 00:00:21,000"}));
  EXPECT_EQ(r.report.timingFixes, 3);
}

TEST(SubtitleCleanup, EndsAnOverlapAtTheSamePlaceWhereTheNextStarts)
{
  const Result r = Clean(Srt({Timed{1000, 3100, "Tu as beaucoup changé."}, Timed{3000, 5000, "Il faut garder."},
                            Timed{6000, 9000, "Long overlap, meant."}, Timed{7000, 10000, "Both shown."}}));
  EXPECT_EQ(Timings(r.out), (std::vector<std::string>{
                                "00:00:01,000 --> 00:00:03,000", "00:00:03,000 --> 00:00:05,000",
                                "00:00:06,000 --> 00:00:09,000", "00:00:07,000 --> 00:00:10,000"}));
}

TEST(SubtitleCleanup, WritesADamagedArrowPlainly)
{
  const Result r = Clean("1\n01:06:49,130 ?\?--> 01:06:50,086\nVi ?\n");
  EXPECT_EQ(Timings(r.out), std::vector<std::string>{"01:06:49,130 --> 01:06:50,086"});
}

TEST(SubtitleCleanup, LeavesTimingAloneWhenAskedTo)
{
  const std::string text = Srt({Timed{2000, 2000, "Hello?"}, Timed{2700, 3500, "Katie!"}});
  Options options;
  options.fixTiming = false;
  EXPECT_EQ(Clean(text, "", nullptr, options).out, text);
}

// -- cues of a track inside the video -------------------------------------------------------

TEST(CueCleaner, RepairsACueAndDropsOneThatWasAllAdvertising)
{
  CCueCleaner cleaner(Options(), nullptr, "", {});
  std::string text = "â™ª Hey ¶\r\nwww.addic7ed.com";
  EXPECT_TRUE(cleaner.Clean(text));
  EXPECT_EQ(text, "♪ Hey ♪");
  std::string ad = "Support us and become VIP member\nto remove all ads from www.OpenSubtitles.org";
  EXPECT_FALSE(cleaner.Clean(ad));
}

TEST(CueCleaner, LeavesAnEmptyCueToKodi)
{
  CCueCleaner cleaner(Options(), nullptr, "", {});
  std::string text;
  EXPECT_TRUE(cleaner.Clean(text));
}

TEST(CueCleaner, LearnsThatHashIsTheNoteFromTheCuesSoFar)
{
  CCueCleaner cleaner(Options(), nullptr, "", {});
  std::string first = "# Love is in the air";
  EXPECT_TRUE(cleaner.Clean(first));
  EXPECT_EQ(first, "# Love is in the air");
  std::string second = "# She walks softly";
  cleaner.Clean(second);
  std::string third = "# Yeah yeah";
  cleaner.Clean(third);
  EXPECT_EQ(third, "♪ Yeah yeah");
}

TEST(CueCleaner, CorrectsLettersOnceTheTrackShowsItsMisreadings)
{
  CCueCleaner cleaner(Options(), &English(), "en", {});
  std::string early = "l was there.";
  cleaner.Clean(early);
  EXPECT_EQ(early, "l was there.");
  for (std::string text : MisreadsI())
    cleaner.Clean(text);
  std::string later = "l was there.";
  cleaner.Clean(later);
  EXPECT_EQ(later, "I was there.");
}

TEST(CueCleaner, ReadsTheLanguageOfATrackThatNamesNone)
{
  int asked = 0;
  CCueCleaner cleaner(Options(), &English(), "", [&](const std::string&) {
    ++asked;
    return Detection{"en", 95, true};
  });
  std::string text;
  for (int i = 0; i < DETECT_CUES - 1; ++i)
  {
    text = MisreadsI()[static_cast<size_t>(i) % MisreadsI().size()];
    cleaner.Clean(text);
  }
  EXPECT_EQ(asked, 0);
  EXPECT_EQ(text, MisreadsI()[static_cast<size_t>(DETECT_CUES - 2) % MisreadsI().size()]);
  text = "l was there.";
  cleaner.Clean(text);
  EXPECT_EQ(asked, 1);
  EXPECT_EQ(text, "I was there.");
  for (int i = 0; i < DETECT_CUES; ++i)
  {
    text = "l know.";
    cleaner.Clean(text);
  }
  EXPECT_EQ(asked, 1);
  EXPECT_EQ(text, "I know.");
}

TEST(CueCleaner, LeavesATrackSurelyInALanguageWithNoPack)
{
  int asked = 0;
  CCueCleaner cleaner(Options(), &English(), "", [&](const std::string&) {
    ++asked;
    return Detection{"da", 99, true};
  });
  std::string text;
  for (int i = 0; i < 3 * DETECT_CUES; ++i)
  {
    text = "l know.";
    cleaner.Clean(text);
  }
  EXPECT_EQ(asked, 1);
  EXPECT_EQ(text, "l know.");
}

TEST(CueCleaner, ReadsTheScreenPosition)
{
  EXPECT_EQ(CCueCleaner::Position("{\\an8}Sign"), 8);
  EXPECT_EQ(CCueCleaner::Position("Dialogue"), 2);
}
