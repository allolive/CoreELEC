/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The subtitle translation without a provider: which track is offered for
 *  translation, how a cue is read and laid out, what a model is asked and how
 *  its answer is read, and the thread that translates cues in order, with a
 *  stand-in model.
 */

#include "cores/VideoPlayer/DVDSubtitles/SubtitleTranslation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::SUBTITLES::TRANSLATION;
using KODI::SUBTITLES::MODEL::CCueWorker;
using KODI::SUBTITLES::MODEL::IChat;
using KODI::SUBTITLES::MODEL::Plain;
using KODI::SUBTITLES::MODEL::ReadNumbered;

namespace
{
Track In(const std::string& code, bool isTarget = false)
{
  return {code, isTarget, false, true};
}

//! The numbered lines a request asks to translate.
std::vector<std::string> Asked(const std::string& user)
{
  std::vector<std::string> lines;
  const size_t from = user.find("The lines to translate:\n");
  size_t start = from + 24;
  while (from != std::string::npos && start < user.size())
  {
    const size_t end = user.find('\n', start);
    const std::string line = user.substr(start, end - start);
    lines.push_back(line.substr(line.find(". ") + 2));
    start = end + 1;
  }
  return lines;
}

//! Answers in capitals, keeping the numbering; can be held, broken, or made to fail, and
//! remembers what it was asked.
class CUpper : public IChat
{
public:
  std::optional<std::string> Complete(const std::string&, const std::string& user) override
  {
    std::unique_lock lock(m_lock);
    m_requests.push_back(user);
    ++m_started;
    m_changed.notify_all();
    m_changed.wait(lock, [this] { return !m_hold || m_cancelled || m_aborted; });
    if (m_aborted)
    {
      m_aborted = false;
      return std::nullopt;
    }
    if (m_cancelled || m_fail)
      return std::nullopt;
    std::string out = m_unnumbered && Asked(user).size() > 1 ? "Here you go:\n" : "";
    int n = 0;
    for (const std::string& line : Asked(user))
    {
      std::string upper = line;
      for (char& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      out +=
          (m_unnumbered && Asked(user).size() > 1 ? "" : std::to_string(++n) + ". ") + upper + "\n";
    }
    return out;
  }
  void Cancel() override
  {
    std::unique_lock lock(m_lock);
    m_cancelled = true;
    m_changed.notify_all();
  }
  void Abort() override
  {
    std::unique_lock lock(m_lock);
    m_aborted = true;
    m_changed.notify_all();
  }
  void Hold(bool hold)
  {
    std::unique_lock lock(m_lock);
    m_hold = hold;
    m_changed.notify_all();
  }
  void WaitStarted(int n)
  {
    std::unique_lock lock(m_lock);
    m_changed.wait(lock, [&] { return m_started >= n; });
  }
  std::vector<std::string> Requests()
  {
    std::unique_lock lock(m_lock);
    return m_requests;
  }

  bool m_fail{false};
  bool m_unnumbered{false}; //!< a batch comes back without its numbers

private:
  std::mutex m_lock;
  std::condition_variable m_changed;
  bool m_hold{false};
  bool m_cancelled{false};
  bool m_aborted{false};
  int m_started{0};
  std::vector<std::string> m_requests;
};

const Context french{"French", "English", "A Film (1999)", {"Marie", "Paul"}};

struct Delivered
{
  std::mutex lock;
  std::condition_variable arrived;
  std::vector<std::tuple<std::string, double, double>> cues;

  CCueWorker::Deliver Into()
  {
    return [this](const std::string& text, double start, double stop) { Add(text, start, stop); };
  }
  void Add(const std::string& text, double start, double stop)
  {
    std::unique_lock guard(lock);
    cues.emplace_back(text, start, stop);
    arrived.notify_all();
  }
  bool WaitFor(size_t n)
  {
    std::unique_lock guard(lock);
    return arrived.wait_for(guard, std::chrono::seconds(5), [&] { return cues.size() >= n; });
  }
  std::string Text(size_t i)
  {
    std::unique_lock guard(lock);
    return std::get<0>(cues.at(i));
  }
};

//! One cue through a translator; what the model was asked and what came back.
std::string TranslatedWith(std::vector<std::string>& asked, const std::string& cue)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  {
    CCueWorker translator(std::move(chat), Translation(french), out.Into());
    translator.Submit(cue, 0, 1);
    out.WaitFor(1);
    for (const std::string& request : model->Requests())
      for (const std::string& line : Asked(request))
        asked.push_back(line);
  }
  return out.cues.empty() ? std::string() : out.Text(0);
}
} // namespace

// -- which track is offered ---------------------------------------------------------------------

TEST(SubtitleTranslation, OffersTheFirstTrackKodiWouldChoose)
{
  EXPECT_EQ(ChooseSource({In("en"), In("de")}), 0u);
  EXPECT_EQ(ChooseSource({In("de"), In("en")}), 0u);
}

TEST(SubtitleTranslation, OffersNothingWhenAFullTrackInTheLanguageExists)
{
  EXPECT_EQ(ChooseSource({In("en"), In("fr", true)}), std::nullopt);
}

TEST(SubtitleTranslation, AForcedTrackInTheLanguageIsNoSubtitleInIt)
{
  // an English film with French forced subtitles for its foreign lines: the English is translated
  Track forcedFrench = In("fr", true);
  forcedFrench.forced = true;
  EXPECT_EQ(ChooseSource({forcedFrench, In("en")}), 1u);
}

TEST(SubtitleTranslation, NeverTranslatesAForcedTrack)
{
  Track forcedEnglish = In("en");
  forcedEnglish.forced = true;
  EXPECT_EQ(ChooseSource({forcedEnglish}), std::nullopt);
  EXPECT_EQ(ChooseSource({forcedEnglish, In("de")}), 1u);
}

TEST(SubtitleTranslation, SkipsImagesAndUndeclaredLanguages)
{
  Track image = In("en");
  image.text = false;
  EXPECT_EQ(ChooseSource({image, In(""), In("de")}), 2u);
  EXPECT_EQ(ChooseSource({image, In("")}), std::nullopt);
}

// -- reading and laying out a cue ---------------------------------------------------------------

TEST(SubtitleTranslation, ReadsACueWithoutItsTagsOnOneLine)
{
  EXPECT_EQ(Plain("<i>I know what\nyou mean.</i>"), "I know what you mean.");
  EXPECT_EQ(Plain("{\\an8}Previously  on\r\nthe show"), "Previously on the show");
  EXPECT_EQ(Plain("<font color=\"#ffff00\"></font>"), "");
}

TEST(SubtitleTranslation, LaysALongTranslationOnTwoLines)
{
  EXPECT_EQ(Wrap("Coucou !"), "Coucou !");
  EXPECT_EQ(Wrap("Oh, désolé, je pensais que tu étais, euh, suédoise."),
            "Oh, désolé, je pensais que\ntu étais, euh, suédoise.");
  const std::string unbroken(60, 'x');
  EXPECT_EQ(Wrap(unbroken), unbroken);
}

// -- what the model is asked, and how its answer is read ----------------------------------------

TEST(SubtitleTranslation, AsksWithTheVideoTheCastAndTheLinesBefore)
{
  const std::string user = UserPrompt(french, {"Bonsoir."}, {"Où est Paul ?", "Il dort."});
  EXPECT_EQ(user, "Video: A Film (1999)\n"
                  "Characters: Marie, Paul\n"
                  "\nThe lines before, for context only:\nBonsoir.\n"
                  "\nThe lines to translate:\n1. Où est Paul ?\n2. Il dort.\n");
  EXPECT_NE(SystemPrompt(french).find("from French into English"), std::string::npos);
  // nothing known, nothing said
  EXPECT_EQ(UserPrompt({"French", "English", {}, {}}, {}, {"Oui."}),
            "\nThe lines to translate:\n1. Oui.\n");
}

TEST(SubtitleTranslation, ReadsTheNumberedLinesOfAnAnswer)
{
  const std::vector<std::string> sources{"Bonjour.", "Je suis là, je t'attends."};
  EXPECT_EQ(ReadNumbered("1. Hello\nthere.\n2) I'm here, waiting for you.\n", sources.size()),
            (std::vector<std::string>{"Hello there.", "I'm here, waiting for you."}));
  // a reasoning model's thoughts, and a word before the lines, are not lines
  EXPECT_EQ(ReadNumbered("<think>1. no</think>\nSure:\n1: Hello.\n2. I'm here.", sources.size()),
            (std::vector<std::string>{"Hello.", "I'm here."}));
}

TEST(SubtitleTranslation, LeavesWhatFollowsTheLinesOutOfThem)
{
  EXPECT_EQ(ReadNumbered("1. Hello.\n\n(Translated from French.)", 1),
            (std::vector<std::string>{"Hello."}));
}

TEST(SubtitleTranslation, RefusesAnAnswerWithARemarkAfterItsLines)
{
  // a remark run into the last line would be shown as part of it
  EXPECT_TRUE(ReadNumbered("1. Bonjour.\n2. Merci.\nNote: names kept.", 2).empty());
  // a code fence is no line
  EXPECT_EQ(ReadNumbered("```\n1. Bonjour.\n2. Merci.\n```", 2),
            (std::vector<std::string>{"Bonjour.", "Merci."}));
}

TEST(SubtitleTranslation, RefusesAnAnswerFarTooLongToBeOne)
{
  // and reads it without recursing through it: a model that loops can write megabytes
  EXPECT_TRUE(ReadNumbered("1. " + std::string(200000, 'x'), 1).empty());
  EXPECT_EQ(Plain("<" + std::string(200000, 'x') + ">y"), "y");
}

TEST(SubtitleTranslation, RefusesAnAnswerThatLosesTheNumbering)
{
  EXPECT_TRUE(ReadNumbered("Hello.\nThank you.", 2).empty());
  EXPECT_TRUE(ReadNumbered("1. Hello.", 2).empty());
  EXPECT_TRUE(ReadNumbered("1. Hello.\n1. Hi.\n2. Thanks.", 2).empty());
  EXPECT_TRUE(ReadNumbered("1. Hello.\n2.\n", 2).empty());
  EXPECT_TRUE(ReadNumbered("1. Hello.\n2. Thanks.\n3. And more.", 2).empty());
}

TEST(SubtitleTranslation, RefusesALineFarLongerThanItsSource)
{
  // a model that runs on
  const std::string rambling(80, 'x');
  EXPECT_TRUE(ReadTranslation("1. " + rambling, {"Oui."}).empty());
  EXPECT_EQ(ReadTranslation("1. Yes, of course.", {"Oui."}),
            (std::vector<std::string>{"Yes, of course."}));
}

TEST(SubtitleTranslation, AsksLineByLineWhenABatchLosesItsNumbering)
{
  CUpper model;
  model.m_unnumbered = true;
  const auto out = TranslateLines(model, french, {}, {"un", "deux"});
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(*out, (std::vector<std::string>{"UN", "DEUX"}));
  const std::vector<std::string> requests = model.Requests();
  ASSERT_EQ(requests.size(), 3u);
  // the second line is asked with the first as context
  EXPECT_NE(requests[2].find("for context only:\nun\n"), std::string::npos);
}

TEST(SubtitleTranslation, GivesNothingWhenTheProviderFails)
{
  CUpper model;
  model.m_fail = true;
  EXPECT_EQ(TranslateLines(model, french, {}, {"un", "deux"}), std::nullopt);
}

// -- the thread ---------------------------------------------------------------------------------

TEST(SubtitleTranslation, TranslatesCuesInOrderWithTheirTimes)
{
  Delivered out;
  {
    CCueWorker translator(std::make_unique<CUpper>(), Translation(french), out.Into());
    translator.Submit("<i>one</i>", 1, 2);
    translator.Submit("two\nlines", 3, 4);
    ASSERT_TRUE(out.WaitFor(2));
  }
  EXPECT_EQ(out.cues[0], std::make_tuple(std::string("ONE"), 1.0, 2.0));
  EXPECT_EQ(out.cues[1], std::make_tuple(std::string("TWO LINES"), 3.0, 4.0));
}

TEST(SubtitleTranslation, SendsWaitingCuesTogetherWithTheLinesBefore)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  model->Hold(true);
  translator.Submit("zero", 0, 1);
  model->WaitStarted(1);
  for (int i = 1; i <= 3; ++i)
    translator.Submit("cue " + std::to_string(i), i, i + 1);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(4));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(Asked(requests[1]), (std::vector<std::string>{"cue 1", "cue 2", "cue 3"}));
  EXPECT_NE(requests[1].find("for context only:\nzero\n"), std::string::npos);
}

TEST(SubtitleTranslation, KeepsADialogueOneSpeakerALine)
{
  Delivered out;
  CCueWorker translator(std::make_unique<CUpper>(), Translation(french), out.Into());
  translator.Submit("- Hi.\n-<i>Hello.</i>", 1, 2);
  ASSERT_TRUE(out.WaitFor(1));
  EXPECT_EQ(out.Text(0), "- HI.\n- HELLO.");
}

TEST(SubtitleTranslation, DeliversNothingFromBeforeAFlush)
{
  // a seek: the cue being translated and those waiting are dropped, the next ones are not
  Delivered out;
  auto upper = std::make_unique<CUpper>();
  CUpper* model = upper.get();
  CCueWorker translator(std::move(upper), Translation(french), out.Into());
  model->Hold(true);
  translator.Submit("in flight", 1, 2);
  model->WaitStarted(1);
  translator.Submit("waiting", 3, 4);
  translator.Flush();
  translator.Submit("after", 10, 11);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_EQ(out.cues.size(), 1u);
  EXPECT_EQ(out.Text(0), "AFTER");
}

TEST(SubtitleTranslation, ASeekEndsTheRequestForTheCuesItDrops)
{
  // the cues after a seek do not wait for an answer no one will show
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  model->Hold(true);
  translator.Submit("before", 1, 2);
  model->WaitStarted(1);
  const auto start = std::chrono::steady_clock::now();
  translator.Flush();
  model->Hold(false);
  translator.Submit("after", 10, 11);
  ASSERT_TRUE(out.WaitFor(1));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  EXPECT_EQ(out.Text(0), "AFTER");
}

TEST(SubtitleTranslation, AsksNothingAboutACueWithNothingToTranslate)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  translator.Submit("{\\an8}<i></i>", 1, 2);
  translator.Submit("- Oui.\n-", 3, 4);
  // the empty one still comes, to end a cue left open before it
  ASSERT_TRUE(out.WaitFor(2));
  EXPECT_EQ(out.Text(0), "");
  EXPECT_EQ(out.Text(1), "- OUI. -");
  for (const std::string& request : model->Requests())
    EXPECT_EQ(Asked(request).size(), 1u);
}

TEST(SubtitleTranslation, TranslatesAFileFromWherePlaybackIs)
{
  // a file known whole, opened at 100 s: what comes after goes first, what is behind last
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  model->Hold(true);
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), [] { return 100.0; });
  translator.Submit("early", 10, 11);
  model->WaitStarted(1);
  for (int t = 20; t <= 200; t += 20)
    translator.Submit("at " + std::to_string(t), t, t + 1);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(11));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_GE(requests.size(), 3u);
  EXPECT_EQ(Asked(requests[1]).front(), "at 100");
  EXPECT_EQ(Asked(requests[2]).front(), "at 20");
}

TEST(SubtitleTranslation, ShowsNothingForCuesTheProviderFailed)
{
  // but still delivers them, to end a cue left open before them
  auto chat = std::make_unique<CUpper>();
  chat->m_fail = true;
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  translator.Submit("lost", 1, 2);
  ASSERT_TRUE(out.WaitFor(1));
  EXPECT_EQ(out.Text(0), "");
}

TEST(SubtitleTranslation, ClosingEndsTheRequestInFlight)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  model->Hold(true);
  Delivered out;
  const auto start = std::chrono::steady_clock::now();
  {
    CCueWorker translator(std::move(chat), Translation(french), out.Into());
    translator.Submit("slow", 1, 2);
    model->WaitStarted(1);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  EXPECT_TRUE(out.cues.empty());
}

// -- capitals -----------------------------------------------------------------------------------

TEST(SubtitleTranslation, ReadsASongInCapitalsInOrdinaryCase)
{
  // a model reads capitals as names and keeps them in French: it gets ordinary case, and the
  // translation is set back in capitals
  std::vector<std::string> asked;
  // (a long cue is laid out on two lines, so it is compared joined)
  EXPECT_EQ(
      Plain(TranslatedWith(asked, "♪ QUAND IL ME PREND DANS SES BRAS. ELLE ÉTAIT SI JOLIE ♪")),
      "♪ QUAND IL ME PREND DANS SES BRAS. ELLE ÉTAIT SI JOLIE ♪");
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], "♪ Quand il me prend dans ses bras. Elle était si jolie ♪");
}

TEST(SubtitleTranslation, LeavesOrdinaryTextAndShortCapitalsAsTheyAre)
{
  std::vector<std::string> asked;
  TranslatedWith(asked, "Il travaille pour le FBI.");
  TranslatedWith(asked, "OK!");
  ASSERT_EQ(asked.size(), 2u);
  EXPECT_EQ(asked[0], "Il travaille pour le FBI.");
  EXPECT_EQ(asked[1], "OK!");
}

TEST(SubtitleTranslation, ReadsCapitalsInGreekAndCyrillicButNotInAScriptWithNoCase)
{
  std::vector<std::string> asked;
  TranslatedWith(asked, "СТОЙ, ПОЛИЦИЯ!");
  // an acronym in a line of a script with no capitals is no shouting
  TranslatedWith(asked, "これはNASAのデータだ");
  TranslatedWith(asked, "Звоните в FBI и CIA.");
  ASSERT_EQ(asked.size(), 3u);
  EXPECT_EQ(asked[0], "Стой, полиция!");
  EXPECT_EQ(asked[1], "これはNASAのデータだ");
  EXPECT_EQ(asked[2], "Звоните в FBI и CIA.");
}

TEST(SubtitleTranslation, ChangesTheCaseOfAccentedLettersInPairs)
{
  // Latin-1 (French, German) and Latin Extended-A, whose pairs start one later in two runs
  // (Polish Ł Ń, Czech Ž)
  std::vector<std::string> asked;
  TranslatedWith(asked, "ÇA ÜBER ŁÓDŹ ŽIVOT ŒUVRE");
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], "Ça über łódź život œuvre");
}
