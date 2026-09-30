# Passthrough resume handoff validation

The handoff now keeps audio submission and correction held until the player has
accepted readiness, anchored the clock and released the video hold. Recovery
owns the generation until both the audio commit and first picture submission
acknowledge. Callback seeks replace preparation while retaining the picture hold
and final seek target. The unchanged FEL exclusion belongs to the CoreELEC
adapter series, rather than the generic Kodi series.

## Build and host checks

The complete Kodi stack applied to pin
`0661a5d74edc384f443b17c9467c1f5efa9ec337`: 80 patches, 78 ours, with zero fuzz
for ours. One upstream warning-removal patch applied with fuzz 1. The complete
AArch64 CoreELEC image built and its embedded Kodi binary was verified after
installation. The source for the four changed production files matched the
native test tree and the image build byte for byte.

Push preparation repeated application from the pinned archive and expanded the
host check to the complete repository suite: **305 tests passed**, including a
final run with ASan, LeakSanitizer and UBSan. It also corrected two existing test
fixture issues found by that wider run: retrying an empty dictionary download
must verify its checksum, and the dialnorm fixture must free its caller-owned
parser output. Neither changes the installed production code.

The focused host run passed 216 cases, including 14 new regressions. It used
ASan, LeakSanitizer and UBSan; the existing genlock clock fixture requires vptr
instrumentation to be disabled for that target only. Its clock's reference-clock
pointer is empty and its thread implementation/RTTI is not linked. This exception
is now recorded in the target's CMake definition, so no private harness edit is
needed. The real class declarations are separately checked by the image build.

Use the repository's normal test entry points:

```sh
python3 tests/prepare-tree.py --out /tmp/kodi
cmake -S tests -B /tmp/build -DKODI_SOURCE=/tmp/kodi
cmake --build /tmp/build --parallel
ctest --test-dir /tmp/build --output-on-failure --no-tests=error
```

`-DYACER_SANITIZE=ON` enables the sanitizer build. Host tests control queue,
renderer and clock collaborators; they do not reproduce the complete player
thread, HDMI receiver or physical presentation timing.

## Installed-image checks

Device: Ugoos AM9 Pro. Image: `20260930171141`, built from commit
`903883480e90c3c80575320c67d72d4abaebdaf3` before subsequent test/documentation
cleanup. Installed Kodi SHA256:
`58da7714463973bc368091b2ca12cb2b4e6f86b3b5e87b12d2f303681505c6f6`.

HDMI passthrough settings were preserved, including 600 ms audio lead. Ordinary
tests used pauses of 4, 4 and 12 seconds. Callback tests temporarily enabled the
existing Unpause Jumpback addon with its four-second rewind after a pause longer
than 10 seconds. Rapid tests used a 200 ms pause and a separate resume interrupted
by another pause. Saved movie positions and addon state were restored afterward.

| Cases | Result |
| --- | --- |
| 3 TrueHD and 3 DTS-HD ordinary resumes | All handoffs completed without fallback/rejection; listener reported aligned sound and no stutter. |
| 2 TrueHD and 2 DTS-HD callback resumes | One addon seek per case; all replacement targets submitted without fallback/rejection. DTS sounded clean. Birds of Prey had early buffering followed by good playback. |
| 2 TrueHD and 2 DTS-HD rapid-control cases | All recovered and progressed normally. Each cancelled one superseded preparation through the ordinary-seek fallback. |
| 2 Dolby Vision FEL resumes | Ordinary resume, no preroll preparation; listener confirmed picture stayed visible and playback resumed normally. |

All 16 cases finished without a Kodi restart. There were 14 coordinated picture
submissions, no rejected handoffs, and four cancellation fallbacks in the rapid
cases. Those cancellations are real and must not be included in a claim of
"zero fallbacks" across all testing.

The original device runner's net-progress assertion incorrectly counted the
intentional rewind as stalled playback in one TrueHD callback case. Retained
samples show normal progression after release. The corrected assertion checks
the final three seconds of playback; the original error remains in the local
evidence rather than being erased.

## Remaining issues and limits

- Birds of Prey's initial seek produced harsh audio and repeated source-cache
  buffering holds. Four post-seek holds each lasted about 616 ms. The same
  source-cache patch is present unchanged in the baseline. No prepared resume
  ran during that setup interval; the cause and any baseline/candidate difference
  still need a matched comparison.
- Resume is not instant after every command. Ordinary request-to-submission
  times were about 1.13–1.19 seconds for TrueHD and 0.75–0.81 seconds for DTS-HD.
  Callback cases reached 3.14 seconds; the short TrueHD pause reached 3.64 seconds.
- Ordinary DTS cases still logged a 54–72 ms clock correction about half a second
  after first submission, despite clean listening results. One ordinary TrueHD
  case logged a missing-frame warning several seconds later; one callback case
  logged a buffer timeout before submission.
- Receiver control polling was unavailable. Listening confirmed audible playback,
  but software submission times do not measure speaker onset or panel presentation.
- This session did not run a matched old/new device comparison and does not prove
  a numerical latency improvement over the prior build.

The next focused check is the repeated initial-seek source-cache holds, followed
by rapid-control cancellation cost and the post-submission DTS correction.
