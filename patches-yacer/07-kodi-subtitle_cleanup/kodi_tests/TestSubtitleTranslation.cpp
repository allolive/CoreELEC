/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The subtitle translation without a provider: which track is offered for
 *  translation, how a cue is read and laid out, what a model is asked and how
 *  its answer is read, and the thread that translates cues in order, with a
 *  stand-in model.
 */

#include "cores/VideoPlayer/DVDSubtitles/SubtitleTranslation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::SUBTITLES::TRANSLATION;
using KODI::SUBTITLES::MODEL::CCueWorker;
using KODI::SUBTITLES::MODEL::IChat;
using KODI::SUBTITLES::MODEL::Place;
using KODI::SUBTITLES::MODEL::AssText;
using KODI::SUBTITLES::MODEL::CAheadMatch;
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
//! remembers what it was asked and what it was told for the log.
class CUpper : public IChat
{
public:
  std::optional<std::string> Complete(const std::string&, const std::string& user) override
  {
    std::unique_lock lock(m_lock);
    m_requests.push_back(user);
    ++m_started;
    m_changed.notify_all();
    m_changed.wait(lock,
                   [this]
                   {
                     return !(m_hold || (m_holdFrom > 0 && m_started >= m_holdFrom)) ||
                            m_cancelled || m_aborted;
                   });
    if (m_aborted)
    {
      m_aborted = false;
      return std::nullopt;
    }
    if (m_cancelled || m_fail)
      return std::nullopt;
    if (m_failNext > 0)
    {
      --m_failNext;
      return std::nullopt;
    }
    // a request too long for it: a timeout, once
    if (m_failAbove > 0 && Asked(user).size() > m_failAbove)
    {
      m_failAbove = 0;
      return std::nullopt;
    }
    // a line it will not translate on its own: an answer with no numbered line
    if (!m_garbled.empty() && Asked(user) == std::vector<std::string>{m_garbled})
      return "I cannot translate that.";
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
  void Note(const std::string& what, bool warning) override
  {
    std::unique_lock lock(m_lock);
    m_notes.emplace_back(what, warning);
    m_changed.notify_all();
  }
  //! The next n requests fail, as a provider that is briefly unreachable.
  void FailNext(int n)
  {
    std::unique_lock lock(m_lock);
    m_failNext = n;
  }
  //! Whether the log was told this; the worker tells it after delivering, so wait for it.
  bool WaitNote(const std::string& what, bool warning)
  {
    std::unique_lock lock(m_lock);
    return m_changed.wait_for(lock, std::chrono::seconds(5),
                              [&]
                              {
                                return std::find(m_notes.begin(), m_notes.end(),
                                                 std::make_pair(what, warning)) != m_notes.end();
                              });
  }
  void Hold(bool hold)
  {
    std::unique_lock lock(m_lock);
    m_hold = hold;
    m_changed.notify_all();
  }
  //! Hold the requests from the nth on; 0, no more.
  void HoldFrom(int n)
  {
    std::unique_lock lock(m_lock);
    m_holdFrom = n;
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
  std::string m_garbled; //!< asked alone, this line gets no translation
  size_t m_failAbove{0}; //!< the first request for more lines than this fails

private:
  std::mutex m_lock;
  std::condition_variable m_changed;
  bool m_hold{false};
  int m_holdFrom{0};
  bool m_cancelled{false};
  bool m_aborted{false};
  int m_started{0};
  int m_failNext{0};
  std::vector<std::string> m_requests;
  std::vector<std::pair<std::string, bool>> m_notes;
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

TEST(SubtitleTranslation, LeavesOutWhatTheScreenHides)
{
  // a "<...>" or "{...}" closed on its line is hidden; anything else shows, and stays: some
  // tracks open a voice off screen with "< "
  EXPECT_EQ(Plain("< The audience at these shows"), "< The audience at these shows");
  EXPECT_EQ(Plain("< Who's there?\n-> Me"), "< Who's there? -> Me");
  EXPECT_EQ(Plain("< A voice off\n<i>Me</i>"), "< A voice off Me");
  EXPECT_EQ(Plain("<Bob> Who's {laughs} there {"), "Who's there {");
  EXPECT_EQ(Plain("<i>a</i> <b"), "a <b");
}

TEST(SubtitleTranslation, ReadsTextOffAPictureAsItIs)
{
  EXPECT_EQ(Plain("< Il pubblico applaude. >\n{ride}", false), "< Il pubblico applaude. > {ride}");
}

TEST(SubtitleTranslation, GivesLibassTextOffAPictureToShowAsItIs)
{
  EXPECT_EQ(AssText("< Il pubblico,\ndi tutte le età."), "< Il pubblico,\ndi tutte le età.");
  EXPECT_EQ(AssText("{ride}"), "\\{ride\\}");
  // "\N", "\h" and "\{" are libass's: a word joiner after the "\" keeps it a character
  EXPECT_EQ(AssText("a\\Nb \\h \\{"), "a\\\u2060Nb \\\u2060h \\\u2060\\{");
  EXPECT_EQ(AssText(""), "");
}

TEST(SubtitleTranslation, PutsTextOffAPictureWhereThePictureWas)
{
  // a Blu-ray's two lines at the foot of the picture, a DVD's sign at the top left
  EXPECT_EQ(Place(560, 930, 1360, 1040, 1920, 1080), 2);
  EXPECT_EQ(Place(40, 30, 300, 80, 720, 576), 7);
  EXPECT_EQ(Place(800, 60, 1100, 120, 1920, 1080), 8);
  EXPECT_EQ(Place(1500, 960, 1880, 1040, 1920, 1080), 3);
  EXPECT_EQ(Place(860, 500, 1060, 560, 1920, 1080), 5);
  // one eye of a 3D video: placed on that eye's half
  EXPECT_EQ(Place(280, 930, 680, 1040, 960, 1080), 2);
  EXPECT_EQ(Place(0, 0, 0, 0, 1920, 1080), 2);
  EXPECT_EQ(Place(560, 930, 1360, 1040, 0, 0), 2);
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

TEST(SubtitleTranslation, KeepsTheBracketsOfALineTheModelBrokeInTwo)
{
  EXPECT_EQ(ReadNumbered("1) Ciao.\n< Il pubblico applaude. >\n2) {ride}", 2),
            (std::vector<std::string>{"Ciao. < Il pubblico applaude. >", "{ride}"}));
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
  EXPECT_EQ(Plain("<i>" + std::string(200000, 'x') + "</i>"), std::string(200000, 'x'));
  EXPECT_EQ(Plain(std::string(200000, '<')), std::string(200000, '<'));
  EXPECT_EQ(Plain(std::string(200000, '{')), std::string(200000, '{'));
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

TEST(SubtitleTranslation, SendsTextOffAPictureWithItsBrackets)
{
  Context pictures = french;
  pictures.tags = false;
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  {
    CCueWorker translator(std::move(chat), Translation(pictures), out.Into());
    translator.Submit("< Il pubblico >", 1, 2);
    ASSERT_TRUE(out.WaitFor(1));
    const std::vector<std::string> requests = model->Requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(Asked(requests[0]), std::vector<std::string>{"< Il pubblico >"});
  }
  EXPECT_EQ(out.Text(0), "< IL PUBBLICO >");
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
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), [] { return 100.0; },
                        true);
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

TEST(SubtitleTranslation, ShowsCuesTheProviderFailedAsTheyCame)
{
  // a line is never lost: asked twice in vain, it shows in its own words, with its times
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  chat->m_fail = true;
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  translator.Submit("lost", 1, 2);
  ASSERT_TRUE(out.WaitFor(1));
  EXPECT_EQ(out.cues[0], std::make_tuple(std::string("lost"), 1.0, 2.0));
  EXPECT_EQ(model->Requests().size(), 2u);
  EXPECT_TRUE(model->WaitNote("1 lines shown as they came: the request failed twice", true));
}

TEST(SubtitleTranslation, AsksAgainAtOnceWhenARequestFails)
{
  // one failure costs one more request, not the wait, and its lines are no context for the next
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  model->FailNext(1);
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  const auto start = std::chrono::steady_clock::now();
  translator.Submit("one", 1, 2);
  ASSERT_TRUE(out.WaitFor(1));
  EXPECT_EQ(out.Text(0), "ONE");
  // and the next cue does not wait either
  translator.Submit("two", 3, 4);
  ASSERT_TRUE(out.WaitFor(2));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  EXPECT_EQ(out.Text(1), "TWO");
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(Asked(requests[0]), std::vector<std::string>{"one"});
  EXPECT_EQ(Asked(requests[1]), std::vector<std::string>{"one"});
  EXPECT_EQ(requests[1].find("for context only"), std::string::npos);
  EXPECT_NE(requests[2].find("for context only:\none\n"), std::string::npos);
}

TEST(SubtitleTranslation, WaitsBeforeAskingAgainOnlyAfterASecondFailure)
{
  // the provider is down: the batch is asked twice, then the next request waits
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  chat->m_fail = true;
  Delivered out;
  const auto start = std::chrono::steady_clock::now();
  {
    CCueWorker translator(std::move(chat), Translation(french), out.Into());
    translator.Submit("first", 1, 2);
    ASSERT_TRUE(out.WaitFor(1));
    translator.Submit("second", 3, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(model->Requests().size(), 2u);
    EXPECT_EQ(out.cues.size(), 1u);
  }
  // closing ends the wait
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(SubtitleTranslation, DoesNotAskAgainForCuesAlreadyOver)
{
  // playback is past every cue of the batch: they show as they came, without a second request
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  chat->m_fail = true;
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), [] { return 100.0; });
  translator.Submit("past", 1, 2);
  ASSERT_TRUE(out.WaitFor(1));
  EXPECT_EQ(out.Text(0), "past");
  EXPECT_EQ(model->Requests().size(), 1u);
}

TEST(SubtitleTranslation, ShowsALineTheModelLeftOutInItsOwnWords)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  chat->m_unnumbered = true;
  chat->m_garbled = "two";
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  model->Hold(true);
  translator.Submit("zero", 0, 1);
  model->WaitStarted(1);
  translator.Submit("one", 1, 2);
  translator.Submit("two", 2, 3);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(3));
  EXPECT_EQ(out.Text(0), "ZERO");
  EXPECT_EQ(out.Text(1), "ONE");
  EXPECT_EQ(out.Text(2), "two");
  EXPECT_TRUE(model->WaitNote("1 lines shown as they came: the model left them out", true));
}

namespace
{
//! Lets a worker carry more cues: a request of eight answered quickly.
void Quicken(CCueWorker& translator, CUpper& model, Delivered& out)
{
  model.Hold(true);
  translator.Submit("zero", 0, 1);
  model.WaitStarted(1);
  for (int i = 1; i <= 8; ++i)
    translator.Submit("warm " + std::to_string(i), i, i + 1);
  model.Hold(false);
  ASSERT_TRUE(out.WaitFor(9));
}
} // namespace

TEST(SubtitleTranslation, SendsUpToTwentyFourCuesOnceTheModelAnswersEightQuickly)
{
  // the model keeps up: one request for more of the cues waiting costs less than several
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), {}, false,
                        std::chrono::seconds(60));
  Quicken(translator, *model, out);
  ASSERT_EQ(Asked(model->Requests()[1]).size(), CCueWorker::BATCH);
  model->Hold(true);
  translator.Submit("held", 10, 11);
  model->WaitStarted(3);
  for (int i = 1; i <= 30; ++i)
    translator.Submit("cue " + std::to_string(i), 20 + i, 21 + i);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(40));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 5u);
  ASSERT_EQ(Asked(requests[3]).size(), CCueWorker::MORE);
  EXPECT_EQ(Asked(requests[3]).front(), "cue 1");
  EXPECT_EQ(Asked(requests[3]).back(), "cue 24");
  EXPECT_EQ(Asked(requests[4]).size(), 6u);
  for (int i = 1; i <= 30; ++i)
    EXPECT_EQ(out.Text(9 + i), "CUE " + std::to_string(i));
}

TEST(SubtitleTranslation, NeverSendsMoreThanEightToAModelNotKnownToBeQuick)
{
  // with no time given for a quick answer, a request carries eight however many wait
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  Quicken(translator, *model, out);
  model->Hold(true);
  translator.Submit("held", 10, 11);
  model->WaitStarted(3);
  for (int i = 1; i <= 20; ++i)
    translator.Submit("cue " + std::to_string(i), 20 + i, 21 + i);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(30));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 6u);
  EXPECT_EQ(Asked(requests[3]).size(), 8u);
  EXPECT_EQ(Asked(requests[4]).size(), 8u);
  EXPECT_EQ(Asked(requests[5]).size(), 4u);
}

TEST(SubtitleTranslation, GoesBackToEightWhenALargerRequestFails)
{
  // a request of 24 times out: its first eight are asked again at once, the rest wait their
  // turn, eight at a time until eight are answered quickly again, and none is lost or out of
  // order
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), {}, false,
                        std::chrono::seconds(60));
  Quicken(translator, *model, out);
  model->m_failAbove = CCueWorker::BATCH;
  model->Hold(true);
  translator.Submit("held", 10, 11);
  model->WaitStarted(3);
  for (int i = 1; i <= 30; ++i)
    translator.Submit("cue " + std::to_string(i), 20 + i, 21 + i);
  const auto start = std::chrono::steady_clock::now();
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(40));
  // asked again at once, not after the wait a provider that is down gets
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 8u);
  EXPECT_EQ(Asked(requests[3]).size(), CCueWorker::MORE);
  EXPECT_EQ(Asked(requests[4]),
            (std::vector<std::string>{"cue 1", "cue 2", "cue 3", "cue 4", "cue 5", "cue 6",
                                      "cue 7", "cue 8"}));
  // after the drop back, two quick answers in a row before it carries more again
  EXPECT_EQ(Asked(requests[5]).front(), "cue 9");
  EXPECT_EQ(Asked(requests[5]).size(), 8u);
  EXPECT_EQ(Asked(requests[6]).front(), "cue 17");
  EXPECT_EQ(Asked(requests[6]).size(), 8u);
  EXPECT_EQ(Asked(requests[7]).front(), "cue 25");
  EXPECT_EQ(Asked(requests[7]).size(), 6u);
  for (int i = 1; i <= 30; ++i)
    EXPECT_EQ(out.Text(9 + i), "CUE " + std::to_string(i));
}

TEST(SubtitleTranslation, ASeekDuringAFailingLargerRequestDropsAllItsCues)
{
  // the cues of a request of 24 that fails at a seek are dropped, none put back in the queue
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), {}, false,
                        std::chrono::seconds(60));
  Quicken(translator, *model, out);
  model->Hold(true);
  model->HoldFrom(4);
  translator.Submit("held", 10, 11);
  model->WaitStarted(3);
  for (int i = 1; i <= 30; ++i)
    translator.Submit("cue " + std::to_string(i), 20 + i, 21 + i);
  model->Hold(false);
  model->WaitStarted(4);
  ASSERT_EQ(Asked(model->Requests()[3]).size(), CCueWorker::MORE);
  translator.Flush();
  model->HoldFrom(0);
  translator.Submit("after", 100, 101);
  // delivered in order: whatever came back of the cues dropped would be before it
  translator.Pass("last", 102, 103);
  ASSERT_TRUE(out.WaitFor(12));
  ASSERT_EQ(out.cues.size(), 12u);
  EXPECT_EQ(out.Text(9), "HELD");
  EXPECT_EQ(out.Text(10), "AFTER");
  EXPECT_EQ(out.Text(11), "last");
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 5u);
  EXPECT_EQ(Asked(requests[4]), std::vector<std::string>{"after"});
}

TEST(SubtitleTranslation, AsksAgainForACueWithNoEndOfItsOwnNotYetStarted)
{
  // a stream's cue that the next one ends has no stop: it is over only once it has started
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  model->FailNext(1);
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), [] { return 100.0; });
  translator.Submit("open", 150, 0);
  ASSERT_TRUE(out.WaitFor(1));
  EXPECT_EQ(out.Text(0), "OPEN");
  EXPECT_EQ(model->Requests().size(), 2u);
}

TEST(SubtitleTranslation, SendsAStreamsCuesInTheOrderTheyCame)
{
  // a stream: a cue playback is past is not moved behind those still to come
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), [] { return 100.0; });
  model->Hold(true);
  translator.Submit("first", 200, 201);
  model->WaitStarted(1);
  translator.Submit("past", 10, 11);
  translator.Submit("open", 20, 0);
  translator.Submit("coming", 150, 151);
  translator.Pass("passed", 160, 161);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(5));
  EXPECT_EQ(out.Text(1), "PAST");
  EXPECT_EQ(out.Text(2), "OPEN");
  EXPECT_EQ(out.Text(3), "COMING");
  EXPECT_EQ(out.Text(4), "passed");
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(Asked(requests[1]), (std::vector<std::string>{"past", "open", "coming"}));
}

TEST(SubtitleTranslation, SendsEightCuesWhenNoMoreWait)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  model->Hold(true);
  translator.Submit("zero", 0, 1);
  model->WaitStarted(1);
  for (int i = 1; i <= 8; ++i)
    translator.Submit("cue " + std::to_string(i), i, i + 1);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(9));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(Asked(requests[1]).size(), CCueWorker::BATCH);
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

TEST(SubtitleTranslation, DeliversACueWorkedOnAlreadyInItsTurnUnasked)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  model->Hold(true);
  translator.Submit("first", 1, 2);
  model->WaitStarted(1);
  translator.Pass("Read and translated.", 3, 4);
  translator.Submit("third", 5, 6);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(3));
  EXPECT_EQ(out.cues[0], std::make_tuple(std::string("FIRST"), 1.0, 2.0));
  EXPECT_EQ(out.cues[1], std::make_tuple(std::string("Read and translated."), 3.0, 4.0));
  EXPECT_EQ(out.cues[2], std::make_tuple(std::string("THIRD"), 5.0, 6.0));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(Asked(requests[1]), std::vector<std::string>{"third"});
}

TEST(SubtitleTranslation, DropsACueWorkedOnAlreadyAtAFlush)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  model->Hold(true);
  translator.Submit("in flight", 1, 2);
  model->WaitStarted(1);
  translator.Pass("Before the seek.", 3, 4);
  translator.Flush();
  translator.Pass("After the seek.", 10, 11);
  translator.Submit("last", 12, 13);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(2));
  ASSERT_EQ(out.cues.size(), 2u);
  EXPECT_EQ(out.Text(0), "After the seek.");
  EXPECT_EQ(out.Text(1), "LAST");
}

TEST(SubtitleTranslation, TakesTheSourceOfACueWorkedOnAlreadyAsALineBefore)
{
  // a line read and translated at once is context for the lines translated after it
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  Heard heard;
  CCueWorker translator(std::move(chat), Translation(french, &heard), out.Into());
  translator.Heard(std::move(heard));
  translator.Pass("Good evening.", 1, 2, "Bonsoir.");
  translator.Submit("Il dort.", 3, 4);
  ASSERT_TRUE(out.WaitFor(2));
  const std::vector<std::string> requests = model->Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_NE(requests[0].find("for context only:\nBonsoir.\n"), std::string::npos);
}

// -- subtitle pictures read and translated at once ----------------------------------------------

TEST(SubtitleTranslation, AsksToReadAndTranslatePicturesWithTheVideoAndTheLinesBefore)
{
  const std::string prompt = PicturePrompt(french, {"Good evening."}, 3);
  EXPECT_EQ(prompt.rfind("Video: A Film (1999)\nCharacters: Marie, Paul\n"
                         "\nThe lines before, for context only:\nGood evening.\n",
                         0),
            0u);
  EXPECT_NE(prompt.find("holds 3 French film subtitles, one per numbered row"), std::string::npos);
  EXPECT_NE(prompt.find("Transcribe each exactly as written"), std::string::npos);
  EXPECT_NE(prompt.find("translate it into natural spoken English"), std::string::npos);
  EXPECT_NE(prompt.find("numbered 1) to 3), each the row as written, then \" => \", then its "
                        "English translation"),
            std::string::npos);
  // a language not declared is not named
  EXPECT_NE(PicturePrompt({"", "English", {}, {}}, {}, 1).find("holds 1 film subtitles"),
            std::string::npos);
}

TEST(SubtitleTranslation, ReadsWhatThePicturesSayAndTheirTranslation)
{
  const auto rows = ReadPictureAnswer("1) Où est Paul ? => Where is Paul?\n"
                                      "2) Il dort. => He is asleep.\n",
                                      2);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ((*rows)[0].source, "Où est Paul ?");
  EXPECT_EQ((*rows)[0].translation, "Where is Paul?");
  EXPECT_EQ((*rows)[1].source, "Il dort.");
  EXPECT_EQ((*rows)[1].translation, "He is asleep.");
}

TEST(SubtitleTranslation, KeepsTheReadingOfARowLeftInItsOwnLanguage)
{
  // the source language answered, or a translation running on: translated apart, as read
  const auto rows = ReadPictureAnswer("1) Où est Paul ? => où est  PAUL ?\n"
                                      "2) Oui. => " +
                                          std::string(200, 'x') + "\n3) Non. => No.\n",
                                      3);
  ASSERT_TRUE(rows);
  EXPECT_EQ((*rows)[0].source, "Où est Paul ?");
  EXPECT_EQ((*rows)[0].translation, "");
  EXPECT_EQ((*rows)[1].source, "Oui.");
  EXPECT_EQ((*rows)[1].translation, "");
  EXPECT_EQ((*rows)[2].translation, "No.");
}

TEST(SubtitleTranslation, ReadsNoRowsOfAnAnswerOfNoUse)
{
  // a row left out, a refusal, a row without its reading: read alone, then translated
  EXPECT_FALSE(ReadPictureAnswer("1) Où est Paul ? => Where is Paul?\n", 2));
  EXPECT_FALSE(ReadPictureAnswer("I cannot read this image.", 1));
  EXPECT_FALSE(ReadPictureAnswer("1) Where is Paul?\n", 1));
  EXPECT_FALSE(ReadPictureAnswer("1) => Where is Paul?\n", 1));
}

// -- a track read ahead of playback, and by the player ------------------------------------------

namespace
{
constexpr double SECOND = 1000000;
} // namespace

TEST(SubtitleTranslation, TakesACueReadAheadOnceWhenThePlayerReadsItToo)
{
  CAheadMatch match;
  const unsigned int ahead = match.Generation();
  EXPECT_EQ(match.Hand(10 * SECOND, "Bonjour.", ahead), CAheadMatch::Take::FIRST);
  // the player's own packet, its time rounded a little differently
  EXPECT_EQ(match.Hand(10 * SECOND + 3000, "Bonjour.", 0), CAheadMatch::Take::AGAIN);
  // and the other way round: the player first
  EXPECT_EQ(match.Hand(20 * SECOND, "Salut.", 0), CAheadMatch::Take::FIRST);
  EXPECT_EQ(match.Hand(20 * SECOND, "Salut.", ahead), CAheadMatch::Take::AGAIN);
}

TEST(SubtitleTranslation, TellsCuesStartingTogetherApartByTheirText)
{
  CAheadMatch match;
  const unsigned int ahead = match.Generation();
  EXPECT_EQ(match.Hand(10 * SECOND, "{\\an8}Sign", ahead), CAheadMatch::Take::FIRST);
  EXPECT_EQ(match.Hand(10 * SECOND, "Dialogue", ahead), CAheadMatch::Take::FIRST);
  EXPECT_EQ(match.Hand(10 * SECOND, "Dialogue", 0), CAheadMatch::Take::AGAIN);
}

TEST(SubtitleTranslation, DropsWhatWasReadAheadBeforeASeek)
{
  CAheadMatch match;
  const unsigned int before = match.Generation();
  EXPECT_EQ(match.Hand(10 * SECOND, "Avant.", before), CAheadMatch::Take::FIRST);
  match.Seek();
  EXPECT_NE(match.Generation(), before);
  EXPECT_EQ(match.Hand(30 * SECOND, "Trop tard.", before), CAheadMatch::Take::STALE);
  // after a seek back, a cue already read is new again
  EXPECT_EQ(match.Hand(10 * SECOND, "Avant.", match.Generation()), CAheadMatch::Take::FIRST);
}

TEST(SubtitleTranslation, StopsAReadAheadThatMissedWhatThePlayerRead)
{
  CAheadMatch match;
  const unsigned int ahead = match.Generation();
  EXPECT_EQ(match.Hand(60 * SECOND, "Loin.", ahead), CAheadMatch::Take::FIRST);
  // just behind where the read-ahead is: it may not have reached it yet
  EXPECT_EQ(match.Hand(59 * SECOND, "Presque.", 0), CAheadMatch::Take::FIRST);
  EXPECT_FALSE(match.Missed());
  EXPECT_EQ(match.Hand(40 * SECOND, "Manqué.", 0), CAheadMatch::Take::MISSED);
  EXPECT_TRUE(match.Missed());
  EXPECT_EQ(match.Hand(41 * SECOND, "Encore.", 0), CAheadMatch::Take::FIRST);
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

// -- how far ahead the translation is -----------------------------------------------------------

namespace
{
using KODI::SUBTITLES::MODEL::CLead;

//! Whether playback may go on, waiting a little for the worker's thread.
bool ReadySoon(double time = 0)
{
  for (int i = 0; i < 500; ++i)
  {
    if (CLead::Check(time, 0).ready)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
} // namespace

TEST(SubtitleTranslation, NothingTranslatedNothingWaitedFor)
{
  EXPECT_TRUE(CLead::Check(0, 0).ready);
  CLead repairOnly(CLead::Known::AS_READ);
  repairOnly.Close();
  EXPECT_EQ(repairOnly.Add(SECOND, 2 * SECOND), 0u);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, AFileIsWaitedForUntilTranslatedFifteenSecondsAhead)
{
  CLead lead(CLead::Known::WHOLE);
  const uint64_t near = lead.Add(10 * SECOND, 12 * SECOND);
  lead.Add(50 * SECOND, 52 * SECOND);
  const CLead::State state = CLead::Check(0, 0);
  EXPECT_FALSE(state.ready);
  EXPECT_DOUBLE_EQ(state.lead, 10 * SECOND);
  lead.Done(near);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, ALineOverAlreadyIsNotWaitedFor)
{
  // a file's lines behind playback are worked on last
  CLead lead(CLead::Known::WHOLE);
  lead.Add(5 * SECOND, 6 * SECOND);
  EXPECT_TRUE(CLead::Check(0, 10 * SECOND).ready);
}

TEST(SubtitleTranslation, AReadAheadIsWaitedForUntilItHasReadFifteenSecondsAhead)
{
  CLead lead(CLead::Known::AS_READ);
  lead.ReadAhead();
  const unsigned int generation = lead.Generation();
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  lead.Reached(10 * SECOND, generation);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  const uint64_t line = lead.Add(12 * SECOND, 13 * SECOND);
  lead.Reached(20 * SECOND, generation);
  const CLead::State state = CLead::Check(0, 0);
  EXPECT_FALSE(state.ready);
  EXPECT_DOUBLE_EQ(state.lead, 12 * SECOND);
  lead.Done(line);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, AReadAheadReadToTheEndIsWaitedForUntilAllIsTranslated)
{
  CLead lead(CLead::Known::AS_READ);
  lead.ReadAhead();
  const uint64_t line = lead.Add(5 * SECOND, 6 * SECOND);
  lead.Ended(lead.Generation());
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  lead.Done(line);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, WhatAReadAheadSaysBeforeASeekIsNotTaken)
{
  CLead lead(CLead::Known::AS_READ);
  lead.ReadAhead();
  const unsigned int before = lead.Generation();
  lead.Seek();
  lead.Reached(100 * SECOND, before);
  lead.Ended(before);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  lead.Reached(100 * SECOND, lead.Generation());
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, AStreamReadAsItPlaysIsWaitedForUntilWhatCameIsTranslated)
{
  CLead lead(CLead::Known::AS_READ);
  const uint64_t line = lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  lead.Done(line);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, AReadAheadThatStopsIsWaitedForAsTheStreamIsRead)
{
  CLead lead(CLead::Known::AS_READ);
  lead.ReadAhead();
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  lead.Unread();
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, AFilesLinesAreWaitedForByTheFilesClock)
{
  // a file's times are the player's moved by the title's start; a stream's are its own
  CLead file(CLead::Known::WHOLE);
  file.Add(25 * SECOND, 26 * SECOND);
  EXPECT_FALSE(CLead::Check(0, 15 * SECOND).ready);
  EXPECT_TRUE(CLead::Check(15 * SECOND, 0).ready);
}

TEST(SubtitleTranslation, AStreamsLinesAreWaitedForByTheStreamsClock)
{
  CLead stream(CLead::Known::AS_READ);
  stream.Add(25 * SECOND, 26 * SECOND);
  EXPECT_FALSE(CLead::Check(15 * SECOND, 0).ready);
  EXPECT_TRUE(CLead::Check(0, 15 * SECOND).ready);
}

TEST(SubtitleTranslation, AReadAheadThatReadsOnAfterAFailedReadIsNotAtItsEnd)
{
  CLead lead(CLead::Known::AS_READ);
  lead.ReadAhead();
  const unsigned int generation = lead.Generation();
  lead.Ended(generation);
  lead.Reached(10 * SECOND, generation);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  lead.Reached(40 * SECOND, generation);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, SaysWhenTheLeadWaitedForLastMoved)
{
  const auto opened = CLead::Clock::now();
  CLead first(CLead::Known::AS_READ, opened);
  CLead second(CLead::Known::AS_READ, opened);
  first.Add(SECOND, 2 * SECOND);
  second.Add(SECOND, 2 * SECOND);
  second.Seek(opened + std::chrono::seconds(3));
  second.Add(SECOND, 2 * SECOND);
  EXPECT_TRUE(CLead::Check(0, 0, opened + std::chrono::seconds(4)).moved ==
              opened + std::chrono::seconds(3));
}

TEST(SubtitleTranslation, ALeadFoundReadyIsNotWaitedForAgainUntilASeek)
{
  // a stall later in the film does not wait for the translation
  CLead lead(CLead::Known::AS_READ);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
  lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_TRUE(CLead::Check(0, 0).ready);
  lead.Seek();
  lead.Add(10 * SECOND, 11 * SECOND);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, AFilesLinesStayWaitedForAfterASeek)
{
  CLead lead(CLead::Known::WHOLE);
  lead.Add(10 * SECOND, 12 * SECOND);
  lead.Seek();
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  EXPECT_TRUE(CLead::Check(0, 20 * SECOND).ready);
}

TEST(SubtitleTranslation, ALeadIsWaitedForOnlySoonAfterItOpensOrMoves)
{
  const auto opened = CLead::Clock::now();
  CLead lead(CLead::Known::AS_READ, opened);
  lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_TRUE(CLead::Check(0, 0, opened + CLead::FRESH + std::chrono::seconds(1)).ready);

  const auto moved = opened + std::chrono::minutes(5);
  lead.Seek(moved);
  lead.Add(2 * SECOND, 3 * SECOND);
  // asked about, it is waited for past the time it was fresh for, until no longer asked about
  using std::chrono::milliseconds;
  EXPECT_FALSE(CLead::Check(0, 0, moved + CLead::FRESH - milliseconds(1000)).ready);
  EXPECT_FALSE(CLead::Check(0, 0, moved + CLead::FRESH + milliseconds(500)).ready);
  EXPECT_TRUE(CLead::Check(0, 0, moved + CLead::FRESH + milliseconds(500) + CLead::KEEP +
                                     milliseconds(1))
                  .ready);
}

TEST(SubtitleTranslation, AWaitGivenUpIsNotWaitedForAgainUntilASeek)
{
  CLead lead(CLead::Known::AS_READ);
  lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  CLead::GiveUp();
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, TheWorkerCountsALineUntilItIsShown)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  auto lead = std::make_shared<CLead>(CLead::Known::WHOLE);
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  translator.Track(lead);
  model->Hold(true);
  translator.Submit("one", 10 * SECOND, 11 * SECOND);
  translator.Pass("Two.", 12 * SECOND, 13 * SECOND);
  model->WaitStarted(1);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(2));
  EXPECT_TRUE(ReadySoon());
}

TEST(SubtitleTranslation, TheWorkerLetsGoOfTheLinesASeekDrops)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  auto lead = std::make_shared<CLead>(CLead::Known::WHOLE);
  CCueWorker translator(std::move(chat), Translation(french), out.Into());
  translator.Track(lead);
  model->Hold(true);
  translator.Submit("in flight", 10 * SECOND, 11 * SECOND);
  model->WaitStarted(1);
  translator.Submit("waiting", 12 * SECOND, 13 * SECOND);
  translator.Pass("Passed.", 14 * SECOND, 15 * SECOND);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  translator.Flush();
  EXPECT_TRUE(ReadySoon());
  model->Hold(false);
}

TEST(SubtitleTranslation, TheWorkerLetsGoOfItsLinesWhenItCloses)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  auto lead = std::make_shared<CLead>(CLead::Known::WHOLE);
  {
    CCueWorker translator(std::move(chat), Translation(french), out.Into());
    translator.Track(lead);
    model->Hold(true);
    translator.Submit("in flight", 10 * SECOND, 11 * SECOND);
    model->WaitStarted(1);
    translator.Submit("waiting", 12 * SECOND, 13 * SECOND);
    EXPECT_FALSE(CLead::Check(0, 0).ready);
  }
  EXPECT_TRUE(CLead::Check(0, 0).ready);
}

TEST(SubtitleTranslation, TheWorkerLetsGoOfLinesPutBackAfterALargerRequestFailed)
{
  // the cues past the first eight wait in the queue again, still counted, until shown
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), {}, false,
                        std::chrono::seconds(60));
  Quicken(translator, *model, out);
  auto lead = std::make_shared<CLead>(CLead::Known::WHOLE);
  translator.Track(lead);
  model->m_failAbove = CCueWorker::BATCH;
  model->Hold(true);
  translator.Submit("held", 10, 11);
  model->WaitStarted(3);
  for (int i = 1; i <= 30; ++i)
    translator.Submit("cue " + std::to_string(i), 20 + i, 21 + i);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  model->Hold(false);
  ASSERT_TRUE(out.WaitFor(40));
  EXPECT_TRUE(ReadySoon());
}

TEST(SubtitleTranslation, TheWorkerLetsGoOfALargerRequestASeekDrops)
{
  auto chat = std::make_unique<CUpper>();
  CUpper* model = chat.get();
  Delivered out;
  CCueWorker translator(std::move(chat), Translation(french), out.Into(), {}, false,
                        std::chrono::seconds(60));
  Quicken(translator, *model, out);
  auto lead = std::make_shared<CLead>(CLead::Known::WHOLE);
  translator.Track(lead);
  model->Hold(true);
  model->HoldFrom(4);
  translator.Submit("held", 10, 11);
  model->WaitStarted(3);
  for (int i = 1; i <= 30; ++i)
    translator.Submit("cue " + std::to_string(i), 20 + i, 21 + i);
  model->Hold(false);
  model->WaitStarted(4);
  EXPECT_FALSE(CLead::Check(0, 0).ready);
  translator.Flush();
  model->HoldFrom(0);
  EXPECT_TRUE(ReadySoon());
}

// -- playback waiting for the translation -------------------------------------------------------

namespace
{
using KODI::SUBTITLES::MODEL::CTranslationWait;
using Turn = CTranslationWait::Turn;

std::chrono::seconds At(int s)
{
  return std::chrono::seconds(s);
}
} // namespace

TEST(SubtitleTranslation, PlaybackWaitsUntilTheTranslationIsAhead)
{
  const auto opened = CLead::Clock::now();
  CLead lead(CLead::Known::AS_READ, opened);
  const uint64_t line = lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, opened + At(1)).turn, Turn::STARTS);
  EXPECT_TRUE(CTranslationWait::Waiting(opened + At(2)));
  const CTranslationWait::Answer waits = CTranslationWait::Ask(0, 0, opened + At(2));
  EXPECT_EQ(waits.turn, Turn::WAITS);
  EXPECT_DOUBLE_EQ(waits.lead, 2 * SECOND);
  lead.Done(line);
  const CTranslationWait::Answer ends = CTranslationWait::Ask(0, 0, opened + At(3));
  EXPECT_EQ(ends.turn, Turn::ENDS);
  EXPECT_DOUBLE_EQ(ends.waited, 2.0);
  EXPECT_FALSE(CTranslationWait::Waiting(opened + At(3)));
  EXPECT_EQ(CTranslationWait::Ask(0, 0, opened + At(4)).turn, Turn::GO);
}

TEST(SubtitleTranslation, PlaybackGoesOnThirtySecondsAfterTheLastSeek)
{
  const auto opened = CLead::Clock::now();
  CLead lead(CLead::Known::AS_READ, opened);
  lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, opened + At(1)).turn, Turn::STARTS);
  for (int s = 2; s < 25; ++s)
    ASSERT_EQ(CTranslationWait::Ask(0, 0, opened + At(s)).turn, Turn::WAITS) << s;
  // a seek during the wait: the thirty seconds count from it
  lead.Seek(opened + At(25));
  lead.Add(2 * SECOND, 3 * SECOND);
  for (int s = 25; s < 55; ++s)
    ASSERT_EQ(CTranslationWait::Ask(0, 0, opened + At(s)).turn, Turn::WAITS) << s;
  const CTranslationWait::Answer cap = CTranslationWait::Ask(0, 0, opened + At(55));
  EXPECT_EQ(cap.turn, Turn::GIVES_UP);
  EXPECT_DOUBLE_EQ(cap.waited, 54.0);
  EXPECT_FALSE(CTranslationWait::Waiting(opened + At(55)));
  // given up: not waited for again until it moves
  EXPECT_EQ(CTranslationWait::Ask(0, 0, opened + At(56)).turn, Turn::GO);
}

TEST(SubtitleTranslation, AWaitAskedAboutSeldomIsToldOnce)
{
  // before the start sync places the clock the player does not ask
  const auto opened = CLead::Clock::now();
  CLead lead(CLead::Known::AS_READ, opened);
  const uint64_t line = lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, opened + At(1)).turn, Turn::STARTS);
  const auto later = opened + At(1) + CTranslationWait::GONE + std::chrono::milliseconds(1);
  EXPECT_FALSE(CTranslationWait::Waiting(later));
  EXPECT_EQ(CTranslationWait::Ask(0, 0, later).turn, Turn::WAITS);
  EXPECT_TRUE(CTranslationWait::Waiting(later));
  const auto again = later + CTranslationWait::GONE + std::chrono::milliseconds(1);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, again).turn, Turn::WAITS);
  lead.Done(line);
  const CTranslationWait::Answer ends = CTranslationWait::Ask(0, 0, again);
  EXPECT_EQ(ends.turn, Turn::ENDS);
  EXPECT_NEAR(ends.waited, 2.0 * std::chrono::duration<double>(CTranslationWait::GONE).count(),
              0.01);
}

TEST(SubtitleTranslation, AFilmStartWaitsOnceForATrackReadAMinuteAhead)
{
  const auto opened = CLead::Clock::now();
  CLead lead(CLead::Known::AS_READ, opened);
  lead.ReadAhead();
  const unsigned int generation = lead.Generation();
  const uint64_t line = lead.Add(4 * SECOND, 6 * SECOND);
  lead.Reached(60 * SECOND, generation);
  // the held start asks at the position the film starts from
  const CTranslationWait::Answer starts = CTranslationWait::Ask(0, 0, opened + At(2));
  EXPECT_EQ(starts.turn, Turn::STARTS);
  EXPECT_DOUBLE_EQ(starts.lead, 4 * SECOND);
  // the start sync places the clock later and asks on: the same wait
  const auto synced = opened + At(2) + CTranslationWait::GONE + At(1);
  for (auto at = synced; at < synced + At(5); at += std::chrono::milliseconds(500))
    ASSERT_EQ(CTranslationWait::Ask(0, 0, at).turn, Turn::WAITS);
  lead.Done(line);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, synced + At(5)).turn, Turn::ENDS);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, synced + At(6)).turn, Turn::GO);
}

TEST(SubtitleTranslation, AWaitNotAskedAboutForLongEndedSomeOtherWay)
{
  const auto opened = CLead::Clock::now();
  CLead lead(CLead::Known::AS_READ, opened);
  lead.Add(2 * SECOND, 3 * SECOND);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, opened + At(1)).turn, Turn::STARTS);
  // the subtitle switched off, say: the next wait, after a seek, is a new one, told again
  const auto later = opened + At(1) + CTranslationWait::CAP + std::chrono::milliseconds(1);
  lead.Seek(later);
  const uint64_t line = lead.Add(2 * SECOND, 3 * SECOND);
  const CTranslationWait::Answer starts = CTranslationWait::Ask(0, 0, later);
  EXPECT_EQ(starts.turn, Turn::STARTS);
  EXPECT_DOUBLE_EQ(starts.waited, 0.0);
  lead.Done(line);
  EXPECT_EQ(CTranslationWait::Ask(0, 0, later).turn, Turn::ENDS);
}

TEST(SubtitleTranslation, SaysWhetherALeadMayYetBeWaitedFor)
{
  const auto opened = CLead::Clock::now();
  EXPECT_FALSE(CLead::Awaited(opened));
  CLead lead(CLead::Known::AS_READ, opened);
  EXPECT_TRUE(CLead::Awaited(opened + At(1)));
  EXPECT_FALSE(CLead::Awaited(opened + CLead::FRESH + At(1)));
  // asking does not settle it
  EXPECT_TRUE(CLead::Awaited(opened + At(2)));
  EXPECT_TRUE(CLead::Check(0, 0, opened + At(2)).ready);
  EXPECT_FALSE(CLead::Awaited(opened + At(2)));
}
