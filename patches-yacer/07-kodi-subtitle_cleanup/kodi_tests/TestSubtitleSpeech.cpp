/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Realigning a subtitle on the video's speech: the change that lays its lines
 *  on where someone speaks, and nothing changed when the times are right or
 *  the speech does not tell.
 */

#include "cores/VideoPlayer/DVDSubtitles/SubtitleSpeech.h"

#include <algorithm>
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
  std::uniform_real_distribution<double> gap(0.3, 6.0);
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

//! What a detector hears of that film in some stretches of it: speech under most lines, a little
//! early and late, music now and then that it half takes for speech, and lines with no speech.
std::vector<Heard> Hear(const std::vector<Cue>& film,
                        unsigned seed,
                        int count = 8,
                        double length = 60,
                        double duration = 6000)
{
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> chance(0, 1);
  std::normal_distribution<double> noise(0, 0.2);
  const size_t frames = static_cast<size_t>(duration / SPEECH_FRAME);
  std::vector<float> speech(frames, 0.1f);
  for (const Cue& cue : film)
  {
    // a sign on screen: no one speaks
    if (chance(random) < 0.15)
      continue;
    const double from = cue.start + 0.2 + 0.2 * chance(random);
    const double to = cue.stop - 0.3 * chance(random);
    for (size_t f = static_cast<size_t>(from / SPEECH_FRAME);
         f < frames && f < static_cast<size_t>(to / SPEECH_FRAME); ++f)
      speech[f] = 0.85f;
  }
  // music: a minute in every ten
  for (double t = 300; t < duration; t += 600)
    for (size_t f = static_cast<size_t>(t / SPEECH_FRAME);
         f < frames && f < static_cast<size_t>((t + 60) / SPEECH_FRAME); ++f)
      speech[f] = std::max(speech[f], 0.5f);
  for (float& p : speech)
    p = static_cast<float>(std::clamp(p + noise(random), 0.0, 1.0));

  std::vector<Heard> heard;
  for (int i = 0; i < count; ++i)
  {
    const double start = (i + 0.5) / count * duration - length / 2;
    const size_t first = static_cast<size_t>(start / SPEECH_FRAME);
    const size_t last = std::min(frames, first + static_cast<size_t>(length / SPEECH_FRAME));
    heard.push_back({first * SPEECH_FRAME, {speech.begin() + first, speech.begin() + last}});
  }
  return heard;
}

//! The film's lines as a subtitle for another cut times them: the video has scenes the cut has
//! not, (where, how long), so their lines are missing and every later line is early.
std::vector<Cue> OtherCut(const std::vector<Cue>& film, const std::vector<std::pair<double, double>>& added)
{
  std::vector<Cue> cut;
  for (const Cue& cue : film)
  {
    double earlier = 0;
    bool inScene = false;
    for (const auto& [at, length] : added)
    {
      inScene |= cue.start >= at && cue.start < at + length;
      if (cue.start >= at + length)
        earlier += length;
    }
    if (!inScene)
      cut.push_back({cue.start - earlier, cue.stop - earlier});
  }
  return cut;
}

//! How many of a subtitle's lines a fit puts more than half a second from where the film has them.
size_t Misplaced(const Fit& fit, const std::vector<Cue>& subtitle, const std::vector<Cue>& film)
{
  size_t misplaced = 0;
  for (const Cue& cue : subtitle)
  {
    const double moved = Apply(fit, cue.start);
    const auto it = std::min_element(film.begin(), film.end(), [moved](const Cue& a, const Cue& b)
                                     { return std::abs(a.start - moved) < std::abs(b.start - moved); });
    misplaced += std::abs(it->start - moved) > 0.5;
  }
  return misplaced;
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

TEST(SubtitleSpeech, LeavesASubtitleThatIsRightAlone)
{
  // and is sure of it: nothing more need be heard
  const auto film = Film(1);
  const auto found = FitToSpeech(film, Hear(film, 2));
  ASSERT_TRUE(found);
  EXPECT_FALSE(found->fit.Moves());
}

TEST(SubtitleSpeech, FindsTheOffsetOfAnotherRelease)
{
  const auto film = Film(3);
  const auto found = FitToSpeech(Moved(film, 1, 12.3), Hear(film, 4));
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->fit.Moves());
  EXPECT_TRUE(found->fit.parts.empty());
  EXPECT_EQ(found->fit.scale, 1);
  // lines show a little before their speech starts: the change is that much short
  EXPECT_NEAR(found->fit.offset, 12.3, 0.3);
}

TEST(SubtitleSpeech, FindsAPalReleaseWithAnotherIntro)
{
  const auto film = Film(5);
  const auto found = FitToSpeech(Moved(film, FILM / 25, -41.5), Hear(film, 6));
  ASSERT_TRUE(found);
  EXPECT_NEAR(found->fit.scale, FILM / 25, 1e-9);
  EXPECT_NEAR(found->fit.offset, -41.5, 0.3);
}

TEST(SubtitleSpeech, LaysASubtitleForAnotherCutInParts)
{
  // the video has a scene of a minute and a half the subtitle's cut has not, and later three and
  // a half seconds more of a shot
  const auto film = Film(21);
  const auto cut = OtherCut(film, {{2000, 90}, {4000, 3.5}});
  const auto found = FitToSpeech(cut, Hear(film, 22, 100));
  ASSERT_TRUE(found);
  ASSERT_EQ(found->fit.parts.size(), 3u);
  EXPECT_EQ(found->fit.scale, 1);
  EXPECT_EQ(found->fit.parts[0].second, 0);
  EXPECT_NEAR(found->fit.parts[1].second, 90, 0.3);
  EXPECT_NEAR(found->fit.parts[2].second, 93.5, 0.3);
  EXPECT_LE(Misplaced(found->fit, cut, film), cut.size() / 100);
}

TEST(SubtitleSpeech, TellsAFewSecondsTrimmedFromADrift)
{
  // two steps of two seconds look much like a slow drift of a near-twin frame rate
  const auto film = Film(23);
  const auto cut = OtherCut(film, {{2000, 2.1}, {4000, 2.1}});
  const auto found = FitToSpeech(cut, Hear(film, 24, 100));
  ASSERT_TRUE(found);
  EXPECT_EQ(found->fit.scale, 1);
  ASSERT_EQ(found->fit.parts.size(), 3u);
  EXPECT_NEAR(found->fit.parts[1].second, 2.1, 0.3);
  EXPECT_NEAR(found->fit.parts[2].second, 4.2, 0.3);
  EXPECT_LE(Misplaced(found->fit, cut, film), cut.size() / 100);
}

TEST(SubtitleSpeech, LaysACutInPartsFromEightMinutesHeard)
{
  // a scene of a minute and a half more, heard in eight minutes spread over the film: the parts
  // are found, and only lines in the stretch not heard where they change can be misplaced -
  // playback hears it before they show
  const auto film = Film(31);
  const auto cut = OtherCut(film, {{2000, 90}});
  const auto found = FitToSpeech(cut, Hear(film, 32, 8));
  ASSERT_TRUE(found);
  ASSERT_EQ(found->fit.parts.size(), 2u);
  EXPECT_EQ(found->fit.parts[0].second, 0);
  EXPECT_NEAR(found->fit.parts[1].second, 90, 0.3);
  // the stretches heard either side: 1845-1905 and 2595-2655
  const size_t unheard = std::count_if(film.begin(), film.end(), [](const Cue& cue)
                                       { return cue.start > 1905 && cue.start < 2595; });
  EXPECT_LE(Misplaced(found->fit, cut, film), unheard);
  const auto whole = FitToSpeech(cut, Hear(film, 32, 100));
  ASSERT_TRUE(whole);
  EXPECT_EQ(whole->fit.parts.size(), 2u);
  EXPECT_LE(Misplaced(whole->fit, cut, film), cut.size() / 100);
}

TEST(SubtitleSpeech, MovesAForcedSubtitleRightOrNotAtAll)
{
  // forced lines: two scenes in another language, and signs no one speaks; under a near-twin
  // frame rate too, which over a few scenes moves them all but alike
  const auto film = Film(25);
  std::vector<Cue> forced(film.begin() + 300, film.begin() + 325);
  forced.insert(forced.end(), film.begin() + 1200, film.begin() + 1220);
  for (const double sign : {700.0, 2500.0, 3100.0, 5200.0})
    forced.push_back({sign, sign + 3});
  const auto heard = Hear(film, 26, 100);
  for (const auto& [scale, offset] : {std::pair{1.0, 0.0}, {1.0, 17.0}, {FILM / 24, 0.0}})
  {
    const auto moved = Moved(forced, scale, offset);
    const auto found = FitToSpeech(moved, heard);
    if (!found)
      continue;
    for (const Cue& cue : moved)
      EXPECT_NEAR(Apply(found->fit, cue.start), scale * cue.start + offset, 0.5);
  }
}

TEST(SubtitleSpeech, RefusesAnotherFilm)
{
  EXPECT_FALSE(FitToSpeech(Film(7), Hear(Film(8), 9)));
}

TEST(SubtitleSpeech, RefusesAnotherFilmHeardWhole)
{
  EXPECT_FALSE(FitToSpeech(Film(27), Hear(Film(28), 29, 100)));
}

TEST(SubtitleSpeech, RefusesWhenNothingIsHeard)
{
  // a sound the detector cannot tell anything in
  const auto film = Film(10);
  std::vector<Heard> heard = Hear(film, 11);
  std::mt19937 random(12);
  std::uniform_real_distribution<float> anything(0, 1);
  for (Heard& h : heard)
    for (float& p : h.speech)
      p = anything(random);
  EXPECT_FALSE(FitToSpeech(Moved(film, 1, 30), heard));
}

TEST(SubtitleSpeech, RefusesASubtitleWithTooFewLines)
{
  const auto film = Film(13);
  const std::vector<Cue> few(film.begin(), film.begin() + 10);
  EXPECT_FALSE(FitToSpeech(Moved(few, 1, 20), Hear(film, 14)));
}

TEST(SubtitleSpeech, DetectorTakesOnlyItsOwnWeights)
{
  CSpeechDetector detector;
  EXPECT_FALSE(detector.Load(std::vector<float>(1000)));
  EXPECT_FALSE(detector.Loaded());
  ASSERT_TRUE(detector.Load(std::vector<float>(CSpeechDetector::WeightCount())));
  // a network of zeros is undecided about anything
  const std::vector<float> chunk(SPEECH_CHUNK, 0.25f);
  EXPECT_FLOAT_EQ(detector.Process(chunk.data()), 0.5f);
}
