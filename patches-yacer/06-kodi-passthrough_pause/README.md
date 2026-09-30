# Pause and resume without losing the sound

Bitstreamed audio - TrueHD, Atmos, DTS-HD, DTS:X - is decoded by the receiver
rather than the box. Pausing stops the carrier, the receiver drops its lock,
and on resume the sound arrives late or not at all while the picture has
already started.

Keep the carrier alive across the pause where the sink allows it, prepare the
resume while the viewer is still paused so play has only to release what is
already held, and hold the first picture back until the audio is genuinely
running rather than merely submitted.

The audio handoff has two steps: a readiness report keeps submission and sync
correction held; only the player's commit releases them after the clock and
video hold have accepted that report. A callback seek keeps the picture hold
and replaces the preparation target until the callback barrier completes.

These are generic Kodi changes. CoreELEC's renderer adapter and its Dolby
Vision FEL restriction live in group 11. FEL continues using ordinary resume.
The device's receiver acquisition time is not measured by the host tests.

`kodi_tests/TestPlayerPrerollWiring.cpp` compiles the current player message
handlers and flush prologue extracted from the applied tree. Its collaborators
control queue delivery, clock and renderer acceptance; the full image build
checks the actual class contracts. It covers cancellation before commit,
rejected/stale readiness, commit failure or timeout after video submission,
and successive callback seeks. It does not replace hardware listening or
exercise the entire player thread.

The [September 30 validation](../../tests/validation/passthrough-resume-20260930.md)
records the installed-image results and the remaining buffering and resume-delay
issues; this handoff change does not claim instantaneous playback after every
control sequence.
