# Working diary

Decisions and the state they left things in. Not a log of what was tried.

## Ground rules

- Nothing is pushed and no pull request is opened without asking first.
- The box is not touched during an incident: read-only until told otherwise.
- One subject per branch, so a regression points at one change.
- A review finding is verified before it is acted on, and a fix is checked by
  making the code wrong again and watching the right test fail.

## Rules learned the hard way

**Run an acceptance test from a clean boot, before anything else touches the
box.** Two titles appeared to lose their preroll entirely on a candidate build.
They had not: the baseline had been measured on a fresh boot and the candidate
after manual experiments and a failed capture-device read. Nothing could be
attributed because more than one thing had moved.

**When folding patch B into patch A, the "after" tree is the base plus A and B
only** - never the fully patched tree. Diffing against the latter made patch 09
absorb hunks belonging to later groups, and the stack stopped applying.

**Tooling that skips silently is worse than tooling that fails.** A patch
splitter that recognised only one diff shape dropped two patches without a
word, so the suites certified a tree missing both. It refuses now.

**Coverage counts execution, not verification.** Adding a nominal parser case
raised coverage by nothing: fuzz sweeps already executed those lines while
asserting nothing about them.

## Where things stand

`yacer` carries the product patches, the tests and the CI workflow. Nothing is
pushed; `origin/yacer` is still at `eb25938f59`.

| Branch | State |
|---|---|
| `yacer` | everything accepted, tested on the box |
| `genlock-v2` | machine checks pass, awaiting a visual sync check |
| `fix-flush-while-holding` | dropped, kept unmerged for the analysis |

### Builds kept for testing

| Build | What it is |
|---|---|
| `...20260923110449.tar` | `yacer` with both accepted fixes - the reference |
| `...20260923114830.tar` | above plus the genlock change |

## Decisions

### Patch 08 could not apply to a clean tree

It deleted a line no patch added - an incremental diff taken from a build tree
that already held its own earlier version. Our builds only survived because
they cleared the build stamp, not the unpack stamp. Rebuilt against a properly
staged base; the whole stack now applies with zero fuzz on ours.

### Tests live with the feature, and run against the shipped tree

Suites go in `patches-yacer/<group>/<package>_tests/` and compile against a
Kodi tree with every patch already applied, so applying the stack is itself the
first test. `tests/guards/<package>/` holds behaviour required of the tree
whoever provides it: when one of our patches is merged upstream, its suite
moves there, because the patch goes and the guarantee does not.

Rejected: building Kodi's own test target, which needs all of Kodi built and
cannot be a feedback loop.

### Seams, not mocks, are what open this codebase

A missing link-time symbol is a seam: the test binary defines it, with no
production change. Every C++ mocking framework needs a virtual method to hook,
and the blockers here - `av_crc`, `g_langInfo`, the `aml_*` functions,
libdovi's C API - are none of them virtual.

| Seam | For | Production change |
|---|---|---|
| Link | free functions, globals, C APIs | none |
| Include | built dependencies | none |
| Object (gmock) | virtual interfaces | none |
| Compile the real thing | FFmpeg, fmt | none |
| Parameter | a genuinely rich collaborator | one line |

### The three candidate fixes

Frame advance was dead for the whole of a pause: the held picture outranked it,
so the clock stepped and the display did not change. Kept.

The retained picture's re-check ran at a millisecond for the length of every
pause. Kept, but the framing was wrong - it cost seven processor ticks against
six over twenty seconds. The real cost was a tenfold difference in log volume.

The renderer refusing to flush while holding is real, but reachable only by
changing skin, language or profile while paused, and produced no visible defect
when driven deliberately. Dropped: no observed symptom, on the part of the
system with the worst record for unintended consequences.

### Coverage is measured and reported, never gated

A threshold buys assertion-free tests written to reach lines. The number went
from 70% to 46% while the code under test tripled, which is the right outcome
and the reason not to gate on it.
