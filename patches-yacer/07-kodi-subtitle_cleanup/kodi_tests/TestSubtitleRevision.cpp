/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The AI provider's corrections without a provider: how its answer is read,
 *  which changes the rules allow, and what reaches the screen when the
 *  provider answers, refuses or is not asked.
 */

#include "cores/VideoPlayer/DVDSubtitles/SubtitleRevision.h"

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::SUBTITLES::REVISION;
using KODI::SUBTITLES::MODEL::IChat;
using KODI::SUBTITLES::MODEL::Item;

namespace
{
//! A dictionary of a few words, and a cast of one.
Context English()
{
  static const std::set<std::wstring> words{L"I",   L"would", L"go", L"went", L"left", L"fire",
                                            L"the", L"he",    L"is", L"here", L"a",    L"man"};
  Context context;
  context.language = "English";
  context.cast = {"Martin Hale Rozer"};
  context.isWord = [](const std::wstring& w) { return words.count(w) > 0; };
  return context;
}

Context NoDictionary()
{
  Context context = English();
  context.isWord = nullptr;
  return context;
}

bool Allows(const Context& context,
            const std::string& line,
            const std::string& wrong,
            const std::string& right,
            Mode mode = Mode::EVERY_LINE,
            const std::vector<std::string>& doubts = {})
{
  return Allowed({0, wrong, right}, line, doubts, context, mode);
}

//! Answers what it is given, and remembers what it was asked.
class CAnswer : public IChat
{
public:
  explicit CAnswer(std::optional<std::string> answer) : m_answer(std::move(answer)) {}
  std::optional<std::string> Complete(const std::string&, const std::string& user) override
  {
    asked.push_back(user);
    return m_answer;
  }
  bool Aborted() const override { return aborted; }
  std::vector<std::string> asked;
  bool aborted{false};

private:
  std::optional<std::string> m_answer;
};
} // namespace

// -- reading the answer -------------------------------------------------------------------------

TEST(SubtitleRevision, ReadsTheChangesAnAnswerAsksFor)
{
  const std::vector<Change> changes =
      ReadChanges("<think>1: no -> yes</think>\nHere:\n2: wouId -> would\n1) 'Ieft' → \"left\"\n"
                  "7: out -> of range\n3: -> nothing\nnone",
                  3);
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].line, 1u);
  EXPECT_EQ(changes[0].wrong, "wouId");
  EXPECT_EQ(changes[0].right, "would");
  EXPECT_EQ(changes[1].line, 0u);
  EXPECT_EQ(changes[1].wrong, "Ieft");
  EXPECT_EQ(changes[1].right, "left");
  EXPECT_TRUE(ReadChanges("none", 3).empty());
}

TEST(SubtitleRevision, AsksWithTheDoubtedWords)
{
  const std::string user =
      UserPrompt(English(), {"Before."}, {"I wouId go.", "He is here."}, {{"wouId"}, {}});
  EXPECT_NE(user.find("1. I wouId go.\n   doubted: wouId\n2. He is here.\n"), std::string::npos);
  EXPECT_NE(user.find("for context only:\nBefore.\n"), std::string::npos);
  EXPECT_NE(SystemPrompt(English(), Mode::DOUBTS).find("doubts"), std::string::npos);
  EXPECT_EQ(SystemPrompt(English(), Mode::EVERY_LINE).find("doubts"), std::string::npos);
}

// -- what may change ----------------------------------------------------------------------------

TEST(SubtitleRevision, AllowsANonWordToBecomeTheWordItWasMisreadFor)
{
  EXPECT_TRUE(Allows(English(), "I wouId go.", "wouId", "would"));
  EXPECT_TRUE(Allows(English(), "He Ieft.", "Ieft", "left"));
  EXPECT_EQ(Apply({0, "wouId", "would"}, "<i>I wouId go.</i>"), "<i>I would go.</i>");
}

TEST(SubtitleRevision, NeverChangesAWordOfTheLanguage)
{
  // the author's choice of word stays, however the model would rather put it
  EXPECT_FALSE(Allows(English(), "I go.", "go", "went"));
  EXPECT_FALSE(Allows(English(), "I wouId go.", "wouId go", "would went"));
}

TEST(SubtitleRevision, MakesWordsNamesOrWhatTheOcrsConfusionsExplain)
{
  EXPECT_FALSE(Allows(English(), "I wouId go.", "wouId", "woud"));
  EXPECT_TRUE(Allows(English(), "Hello, R0zer.", "R0zer", "Rozer"));
  EXPECT_FALSE(Allows(English(), "Hello, Rozre.", "Rozre", "Rozer"));
  // slang no dictionary has, misread the way the OCR misreads
  EXPECT_TRUE(Allows(English(), "That couIda happened.", "couIda", "coulda"));
  EXPECT_FALSE(Allows(English(), "That couIda happened.", "couIda", "could've"));
}

TEST(SubtitleRevision, RefusesWhatIsNotThereAsWholeWords)
{
  EXPECT_FALSE(Allows(English(), "I go.", "wouId", "would"));
  EXPECT_FALSE(Allows(English(), "I wouIdn't go.", "wouId", "would"));
}

TEST(SubtitleRevision, PrefersTheReadingTheOcrsConfusionsExplain)
{
  // a model that would rather read "Hip" than "Help" changes nothing
  Context context = English();
  context.isWord = [](const std::wstring& w) { return w == L"Help" || w == L"Hip"; };
  EXPECT_FALSE(Allows(context, "HeIp! HeIp!", "HeIp", "Hip"));
  EXPECT_TRUE(Allows(context, "HeIp! HeIp!", "HeIp", "Help"));
  // where no reading is a word, a word one letter away: an accent the OCR lost
  context.isWord = [](const std::wstring& w) { return w == L"Écoutez"; };
  EXPECT_TRUE(Allows(context, "Ecoutez !", "Ecoutez", "Écoutez"));
  EXPECT_FALSE(Allows(context, "Ecoutez !", "Ecoutez", "Écouter"));
}

TEST(SubtitleRevision, RestoresAnAccentButMakesNoOtherWordOfAName)
{
  // a letter more or less is another word: a town, a name, a word the OCR broke
  Context context = English();
  context.isWord = [](const std::wstring& w)
  { return w == L"Berk" || w == L"Bernard" || w == L"word" || w == L"Écoutez"; };
  EXPECT_FALSE(Allows(context, "Are we in Berck?", "Berck", "Berk"));
  EXPECT_FALSE(Allows(context, "For Mr Benard Shah.", "Benard", "Bernard"));
  EXPECT_FALSE(Allows(context, "The wor/dt most awesome.", "wor/dt", "word"));
  EXPECT_TRUE(Allows(context, "Ecoutez !", "Ecoutez", "Écoutez"));
}

TEST(SubtitleRevision, CorrectsAMisreadingWhereverTheLineRepeatsIt)
{
  EXPECT_TRUE(Allows(English(), "HeIp! HeIp!", "HeIp", "Help"));
  EXPECT_EQ(Apply({0, "HeIp", "Help"}, "HeIp! HeIp!"), "Help! Help!");
}

TEST(SubtitleRevision, RefusesAChangeFarFromWhatIsWritten)
{
  EXPECT_FALSE(Allows(English(), "I wouId go.", "wouId", "the"));
  EXPECT_FALSE(Allows(English(), "Tlie man.", "Tlie", "<i>the</i>"));
}

TEST(SubtitleRevision, NeverChangesAName)
{
  // a name of the cast that is no word of the dictionary
  Context context = English();
  context.cast = {"Bjorn Ward"};
  EXPECT_FALSE(Allows(context, "Bjorn, wait.", "Bjorn", "Bjom"));
}

TEST(SubtitleRevision, LeavesTheFormattingTagsAlone)
{
  EXPECT_FALSE(Allows(English(), "<font color=\"red\">I go.</font>", "font", "fond"));
  EXPECT_EQ(Apply({0, "wouId", "would"}, "{\\c&HwouId&}I wouId go."), "{\\c&HwouId&}I would go.");
}

TEST(SubtitleRevision, RejoinsAWordBrokenIntoPieces)
{
  EXPECT_TRUE(Allows(English(), "The fi re is here.", "fi re", "fire"));
  // words that stand apart stay apart, and pieces become a word or nothing
  EXPECT_FALSE(Allows(English(), "He is a man.", "a man", "aman"));
  EXPECT_FALSE(Allows(English(), "The fi rex is here.", "fi rex", "firex"));
  EXPECT_EQ(Apply({0, "fi re", "fire"}, "The fi re is here."), "The fire is here.");
}

TEST(SubtitleRevision, InDoubtsModeChangesOnlyADoubtedWord)
{
  EXPECT_FALSE(Allows(English(), "He Ieft.", "Ieft", "left", Mode::DOUBTS, {"wouId"}));
  EXPECT_TRUE(Allows(English(), "He Ieft.", "Ieft", "left", Mode::DOUBTS, {"Ieft"}));
}

TEST(SubtitleRevision, WithoutADictionaryOnlyWhatTheOcrsConfusionsExplain)
{
  EXPECT_TRUE(Allows(NoDictionary(), "I wouId go.", "wouId", "would"));
  EXPECT_TRUE(Allows(NoDictionary(), "Hello, R0zer.", "R0zer", "Rozer"));
  // and only for what can be no word's spelling: "modern" might as well be "modem", for all
  // the confusions say, and "rnan" a word of a language no dictionary is here for
  EXPECT_FALSE(Allows(NoDictionary(), "It's a modern house.", "modern", "modem"));
  EXPECT_FALSE(Allows(NoDictionary(), "Take a dose.", "dose", "close"));
  EXPECT_FALSE(Allows(NoDictionary(), "I know.", "I", "l"));
  EXPECT_FALSE(Allows(NoDictionary(), "Hey, rnan.", "rnan", "man"));
  // nothing says the pieces are no words
  EXPECT_FALSE(Allows(NoDictionary(), "The fi re.", "fi re", "fire"));
  // a change no misreading explains is the model's taste
  EXPECT_FALSE(Allows(NoDictionary(), "I wanna go.", "wanna", "want"));
  EXPECT_FALSE(Allows(NoDictionary(), "Colour.", "Colour", "Color"));
}

// -- the work -----------------------------------------------------------------------------------

TEST(SubtitleRevision, CorrectsWhatTheRulesAllowAndKeepsTheRest)
{
  CAnswer chat("1: wouId -> would\n1: go -> went\n2: Ieft -> left");
  auto work = Revision(English(), Mode::EVERY_LINE);
  const auto outcome = work(chat, {Item{"I wouId go.", {}}, Item{"He Ieft.", {}}, Item{"", {}}});
  ASSERT_EQ(outcome.cues.size(), 3u);
  EXPECT_FALSE(outcome.failed);
  EXPECT_EQ(outcome.cues[0], "I would go.");
  EXPECT_EQ(outcome.cues[1], "He left.");
  EXPECT_EQ(outcome.cues[2], "");
}

TEST(SubtitleRevision, AsksNothingInDoubtsModeWhenNothingIsDoubted)
{
  CAnswer chat("1: go -> went");
  auto work = Revision(English(), Mode::DOUBTS);
  const auto outcome = work(chat, {Item{"I go.", {}}, Item{"He is here.", {}}});
  EXPECT_TRUE(chat.asked.empty());
  EXPECT_EQ(outcome.cues[0], "I go.");
  EXPECT_EQ(outcome.cues[1], "He is here.");
}

TEST(SubtitleRevision, AsksOnlyAboutTheCuesWithDoubts)
{
  CAnswer chat("1: Ieft -> left");
  auto work = Revision(English(), Mode::DOUBTS);
  const auto outcome = work(chat, {Item{"I go.", {}}, Item{"He Ieft.", {"Ieft"}}});
  ASSERT_EQ(chat.asked.size(), 1u);
  EXPECT_NE(chat.asked[0].find("1. He Ieft.\n   doubted: Ieft\n"), std::string::npos);
  EXPECT_EQ(chat.asked[0].find("2. "), std::string::npos);
  EXPECT_EQ(outcome.cues[0], "I go.");
  EXPECT_EQ(outcome.cues[1], "He left.");
}

TEST(SubtitleRevision, ShowsTheCuesAsTheyWereWhenTheProviderFails)
{
  // and lets the next ones by without asking it, rather than hold them
  CAnswer chat(std::nullopt);
  auto work = Revision(English(), Mode::EVERY_LINE);
  const auto outcome = work(chat, {Item{"I wouId go.", {}}});
  EXPECT_EQ(outcome.cues[0], "I wouId go.");
  const auto next = work(chat, {Item{"He Ieft.", {}}});
  EXPECT_EQ(chat.asked.size(), 1u);
  EXPECT_EQ(next.cues[0], "He Ieft.");
}

TEST(SubtitleRevision, ASeekIsNoFailure)
{
  // a request a seek ended says nothing of the provider: the next cues are asked about
  CAnswer chat(std::nullopt);
  chat.aborted = true;
  auto work = Revision(English(), Mode::EVERY_LINE);
  work(chat, {Item{"I wouId go.", {}}});
  chat.aborted = false;
  work(chat, {Item{"He Ieft.", {}}});
  EXPECT_EQ(chat.asked.size(), 2u);
  // and the lines before the seek are no context for those after it
  EXPECT_EQ(chat.asked[1].find("wouId"), std::string::npos);
}
