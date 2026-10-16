/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Realigning a subtitle on another track of the same video: an offset and a
 *  frame-rate ratio found from a few windows of the reference, and nothing
 *  changed when the times are right or the evidence is not clear.
 */

#include "cores/VideoPlayer/DVDSubtitles/SubtitleTiming.h"

#include <cmath>
#include <random>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::SUBTITLES::TIMING;

namespace
{
constexpr double FILM = 24000.0 / 1001;

//! A film's lines, as the video times them: a hundred minutes of dialogue.
std::vector<Cue> Film(unsigned seed, double duration = 6000)
{
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> gap(0.3, 3.0);
  std::uniform_real_distribution<double> length(1.0, 3.5);
  std::vector<Cue> cues;
  for (double t = 30; t < duration;)
  {
    const double stop = t + length(random);
    cues.push_back({t, stop});
    t = stop + gap(random);
  }
  return cues;
}

//! The same film's lines in another language: starts a frame or two apart, some lines merged
//! away, and some signs of its own.
std::vector<Cue> OtherLanguage(const std::vector<Cue>& film, unsigned seed)
{
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> jitter(-0.08, 0.08);
  std::uniform_real_distribution<double> chance(0, 1);
  std::uniform_real_distribution<double> anywhere(0, film.back().stop);
  std::vector<Cue> cues;
  for (const Cue& cue : film)
    if (chance(random) > 0.3)
      cues.push_back({cue.start + jitter(random), cue.stop + jitter(random)});
  for (int i = 0; i < 150; ++i)
  {
    const double t = anywhere(random);
    cues.push_back({t, t + 2});
  }
  return cues;
}

std::vector<Window> Windows(double duration = 6000)
{
  std::vector<Window> windows;
  for (const double at : {0.1, 0.25, 0.4, 0.55, 0.7, 0.85})
    windows.push_back({at * duration, at * duration + 30});
  return windows;
}

std::vector<Cue> Moved(const std::vector<Cue>& cues, double scale, double offset)
{
  // what a subtitle shows as video = scale * subtitle + offset
  std::vector<Cue> moved;
  for (const Cue& cue : cues)
    moved.push_back({(cue.start - offset) / scale, (cue.stop - offset) / scale});
  return moved;
}
} // namespace

TEST(SubtitleTiming, LeavesASubtitleThatIsRightAlone)
{
  // and says so, rather than that it cannot tell
  const auto film = Film(1);
  const auto fit = FitToReference(film, OtherLanguage(film, 2), Windows());
  ASSERT_TRUE(fit);
  EXPECT_FALSE(fit->Moves());
}

TEST(SubtitleTiming, MovesEachLineByThePartItStartsIn)
{
  Fit fit;
  fit.offset = 3;
  fit.parts = {{100, 5}, {200, 12}};
  EXPECT_TRUE(fit.Moves());
  // before the first part, the fit's own offset
  EXPECT_DOUBLE_EQ(Apply(fit, 50), 53);
  EXPECT_DOUBLE_EQ(Apply(fit, 100), 105);
  EXPECT_DOUBLE_EQ(Apply(fit, 150), 155);
  EXPECT_DOUBLE_EQ(Apply(fit, 250), 262);
  // a line across the start of a part stays whole
  EXPECT_DOUBLE_EQ(Apply(fit, 101, 99), 104);
  fit.offset = 0;
  fit.parts = {{0, 0}, {100, 0}};
  EXPECT_FALSE(fit.Moves());
  fit.parts.clear();
  fit.offset = 7;
  EXPECT_TRUE(fit.Moves());
  EXPECT_DOUBLE_EQ(Apply(fit, 10), 17);
}

TEST(SubtitleTiming, DoesNotCallRightACutLateOnlyInTheLastStretch)
{
  // right as it is in five stretches of six: the last says otherwise, so it cannot tell
  const auto film = Film(15);
  std::vector<Cue> cut;
  for (const Cue& cue : film)
    cut.push_back(cue.start < 4800 ? cue : Cue{cue.start - 20, cue.stop - 20});
  EXPECT_FALSE(FitToReference(cut, OtherLanguage(film, 16), Windows()));
}

TEST(SubtitleTiming, CannotTellAForcedOnlySubtitle)
{
  // two scenes of lines are too few of the reference's to say anything
  const auto film = Film(17);
  std::vector<Cue> forced(film.begin() + 300, film.begin() + 325);
  forced.insert(forced.end(), film.begin() + 1200, film.begin() + 1220);
  EXPECT_FALSE(FitToReference(Moved(forced, 1, 17), OtherLanguage(film, 18), Windows()));
}

TEST(SubtitleTiming, FindsTheOffsetOfAnotherRelease)
{
  const auto film = Film(3);
  const auto fit = FitToReference(Moved(film, 1, 12.3), OtherLanguage(film, 4), Windows());
  ASSERT_TRUE(fit);
  EXPECT_EQ(fit->scale, 1);
  EXPECT_NEAR(fit->offset, 12.3, 0.1);
}

TEST(SubtitleTiming, FindsAnEarlyOneToo)
{
  const auto film = Film(5);
  const auto fit = FitToReference(Moved(film, 1, -95), OtherLanguage(film, 6), Windows());
  ASSERT_TRUE(fit);
  EXPECT_NEAR(fit->offset, -95, 0.1);
}

TEST(SubtitleTiming, FindsAPalRelease)
{
  const auto film = Film(7);
  const auto fit = FitToReference(Moved(film, 25 / FILM, 0), OtherLanguage(film, 8), Windows());
  ASSERT_TRUE(fit);
  EXPECT_NEAR(fit->scale, 25 / FILM, 1e-9);
  EXPECT_NEAR(fit->offset, 0, 0.15);
}

TEST(SubtitleTiming, FindsAPalReleaseWithAnotherIntro)
{
  const auto film = Film(9);
  const auto fit =
      FitToReference(Moved(film, FILM / 25, 41.5), OtherLanguage(film, 10), Windows());
  ASSERT_TRUE(fit);
  EXPECT_NEAR(fit->scale, FILM / 25, 1e-9);
  EXPECT_NEAR(fit->offset, 41.5, 0.15);
  // the line a minute in lands where the film has it
  const auto moved = Moved(film, FILM / 25, 41.5);
  EXPECT_NEAR(Apply(*fit, moved[20].start), film[20].start, 0.15);
}

TEST(SubtitleTiming, TellsAPalReleaseOfAShortFilmFromItsNearTwin)
{
  // over twenty minutes, 25/24 lines up much of what 25/23.976 does; it still loses
  const auto film = Film(19, 1300);
  const auto fit =
      FitToReference(Moved(film, 25 / FILM, 0), OtherLanguage(film, 20), Windows(1300));
  ASSERT_TRUE(fit);
  EXPECT_NEAR(fit->scale, 25 / FILM, 1e-9);
}

TEST(SubtitleTiming, RefusesAnotherFilm)
{
  EXPECT_FALSE(FitToReference(Film(11), OtherLanguage(Film(12), 13), Windows()));
}

TEST(SubtitleTiming, RefusesAReferenceWithTooFewLines)
{
  const auto film = Film(14);
  std::vector<Cue> few;
  for (const Cue& cue : film)
    if (cue.start >= 600 && cue.start <= 630 && few.size() < 5)
      few.push_back(cue);
  EXPECT_FALSE(FitToReference(Moved(film, 1, 20), few, Windows()));
}

TEST(SubtitleTiming, RefusesTwoCutsThatNoOneChangeFits)
{
  // a scene more in the middle: the first half is right, the second late by twenty seconds
  const auto film = Film(15);
  std::vector<Cue> cut;
  for (const Cue& cue : film)
    cut.push_back(cue.start < 3000 ? cue : Cue{cue.start - 20, cue.stop - 20});
  EXPECT_FALSE(FitToReference(cut, OtherLanguage(film, 16), Windows()));
}

TEST(SubtitleTiming, RefusesACutThatFitsOnlyThePartBeforeAScene)
{
  // a minute more after the first stretch read: one change fits the rest, and would put the
  // part before it a minute out
  const auto film = Film(21);
  std::vector<Cue> cut;
  for (const Cue& cue : film)
    cut.push_back(cue.start < 1000 ? cue : Cue{cue.start - 60, cue.stop - 60});
  EXPECT_FALSE(FitToReference(cut, OtherLanguage(film, 22), Windows()));
}

TEST(SubtitleTiming, MovesALineOfOnScreenTextWithTheRest)
{
  // a sign no reference line marks still moves by the change the dialogue gives
  const auto film = Film(17);
  auto subtitle = Moved(film, 1, 8);
  subtitle.push_back({1000, 1003});
  const auto fit = FitToReference(subtitle, OtherLanguage(film, 18), Windows());
  ASSERT_TRUE(fit);
  EXPECT_NEAR(Apply(*fit, 1000), 1008, 0.1);
}

TEST(SubtitleTiming, ReadsWhetherAPictureSubtitleShowsOrClears)
{
  // a presentation segment (0x16) of eleven bytes and more: its eleventh is how many objects
  const std::vector<uint8_t> shows{0x16, 0x00, 0x13, 7, 128, 4, 56, 0x10, 0, 1, 0x80, 0, 0, 1,
                                   0,    0,    0,    0, 0,   0, 0,  0x80, 0, 0};
  const std::vector<uint8_t> clears{0x16, 0x00, 0x0b, 7, 128, 4, 56, 0x10, 0, 2, 0, 0, 0, 0,
                                    0x80, 0x00, 0x00};
  const std::vector<uint8_t> none{0x14, 0x00, 0x02, 0, 0, 0x80, 0x00, 0x00};
  // a palette update (its ninth byte's top bit) only recolours what is there
  const std::vector<uint8_t> fade{0x16, 0x00, 0x0b, 7, 128, 4, 56, 0x10, 0, 3, 0, 0x80, 0, 1};
  const std::vector<uint8_t> cut{0x16, 0x00, 0x13, 7, 128};
  EXPECT_EQ(PgsShows(shows.data(), shows.size()), true);
  EXPECT_EQ(PgsShows(clears.data(), clears.size()), false);
  EXPECT_EQ(PgsShows(none.data(), none.size()), std::nullopt);
  EXPECT_EQ(PgsShows(fade.data(), fade.size()), std::nullopt);
  EXPECT_EQ(PgsShows(cut.data(), cut.size()), std::nullopt);
}
