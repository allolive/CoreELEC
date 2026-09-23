# Tests

Suites compile against a Kodi tree with every patch already applied, so
applying the stack is itself the first test: a patch that no longer applies, or
one of ours that only applies with fuzz, stops the run there.

    python3 tests/prepare-tree.py --out /tmp/kodi
    cmake -S tests -B /tmp/build -DKODI_SOURCE=/tmp/kodi
    cmake --build /tmp/build -j"$(nproc)"
    ctest --test-dir /tmp/build --output-on-failure

`-DYACER_SANITIZE=ON` adds the address and undefined-behaviour sanitizers.
`-DYACER_COVERAGE=ON`, with `tests/coverage.py`, reports gcov's figures for the
production code under test. GitHub Actions runs the same commands and publishes
the results.

## Where a suite lives

- `patches-yacer/<group>/<package>_tests/` - behaviour one of our patches adds.
- `tests/guards/<package>/` - behaviour required of the tree whoever provides
  it. When one of our patches is merged upstream its suite moves here: the
  patch goes, the guarantee does not, and a suite deleted with its patch cannot
  catch the change that later regresses it.

## Dependencies a suite may need

FFmpeg's and fmt's headers, and libavutil's `crc.c`, are staged from the
tarballs the build already pins and verified against the checksum each
declares. `tests/shims/` stands in for dependencies that are built rather than
carried in the source - the logger, and libdovi's declarations. A shim replaces
infrastructure, never the code under test.

Collaborators that are ordinary functions are defined by the test binary
itself. That needs no mocking framework and no production change.

## Every package, not just Kodi

`tests/check-patch-stack.py` applies each package's patch stack to its pinned
source - kodi, common_drivers, media_modules-aml, gpu-aml, bluez and
CoreELEC-settings - and reports which apply.
