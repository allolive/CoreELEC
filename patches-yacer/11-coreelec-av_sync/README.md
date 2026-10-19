# Audio and video in step

The display and the receiver each run their own clock, so picture and sound
drift apart and the player can only correct in steps big enough to notice.
Measure where each of them really is, and hold them together continuously.

Passthrough window diagnostics and native tests preserve the current policy
while making its measurements and corrections reviewable. See the
[test runner and coverage](../../tests/README.md). These additions integrate
with this downstream series. The two development tracks are a generic Kodi
series and a CoreELEC patch connecting the Amlogic timing adapter by default.

Patch 18 is the first platform opt-in to the generic passthrough pause policy
in `06-kodi-passthrough_pause`: opened Auge HDMI TrueHD/DTS-HD sinks retain IEC
pause packets during the existing keepalive budget. It is a carrier-policy
candidate, not a receiver-readiness guarantee or the complete restart fix.

Patch 19 connects Kodi's generic resume preparation to the built-in AML
renderer. It preserves the displayed picture across decoder reset, rejects
old buffer generations, and acknowledges successful submission of the first
scheduled target frame. It also resets the extra CoreELEC audio-acquisition
observations at preparation boundaries. No separate addon is required.
Speaker and panel alignment still need matched hardware tests.

Frames reach the decoder from CoreELEC's vsync presenter, which keeps turning
whatever the GUI does, so the picture hold needs nothing from the render loop.
The timing patches observe each refresh in the presenter's vsync wait and act
where it hands a frame over; the render loop does both only when the presenter
is not running.

Patch 22 keeps the Dolby Vision FEL exclusion in the CoreELEC adapter series.
The condition is unchanged: decoder reset cannot retain FEL composition while
preparation withholds new frames, so those streams use ordinary resume. The
Kodi-only series no longer refers to CoreELEC's `GetDoviIsFEL()` API.

Patch 25 starts playback only once a queued refresh rate change, or one the
platform says is coming, has been made, and after every display mode change
keeps the player paused for "Wait after a display change" (1.5 s by
default) while the TV and a receiver behind it find the sound again.
Patch 26 holds the start sync too, so audio and video are put on the clock
after the mode change, against the sound output the film plays on, not the
one the change closes (that left the sound ~100 ms off and cost a gap).
Patch 27 starts a film (TrueHD, DTS, AC-3 passthrough or decoded PCM) through the prepared resume
once the display has settled: its audio held from the open and taken from the
first packet (no seek, no flush, so MVC too), the first picture held until the
audio runs in sync, then both released, so no correction follows.

Patch 28 logs the lead (audio against picture at the box output) every 2 s for
30 s after a start or resume, which the minute report averages away.

Patch 29 has the genlock place the picture through the windowing system's frame
latency adjustment instead of stepping the clock. Patch 30 keeps the player from
stepping the clock while the audio rate trim holds audio to the picture: after
the start, sync is held by the audio rate alone.

Patch 31 hands the trim no lead until the pipeline has settled, so a picture
timestamp the kernel and the player disagree on cannot steer the audio.

Patch 32 takes the display's phase out of the clock before a prepared start or
resume puts the audio on it, so the first frame lands centred and no placement
is owed afterwards.

Patch 33 outputs the picture a prepared start or resume retained before the
decoder is asked for the next, so that picture is the first shown and no frame
repeats behind it.

Patch 34 presents that prepared picture only once it is due, never a refresh
early, so it is not shown twice.

Patch 35 lets a pause that has no prepared resume play out the audio already
queued in the engine, which pausing the stream would flush unheard, and stops
the clock and the picture where it ends, so the resume has nothing to fill with
silence.

Patch 36 schedules a start with the pipeline count measured before, rather
than CGraphicContext's buffer-count guess, so the measurement landing does not
repeat two frames.

Patch 37 logs where the audio is against the clock, by the player and by the
engine, when the prepared picture is handed over.

Patch 39 aims the engine's first correction after a resync at zero, from a
sample taken with its queue full, so a start or resume hands over its first
picture in sync rather than anywhere inside the 30 ms band.

Patch 40 sets aside the first audio window after a re-arm, which spans that
correction while it is still queued, so the audio trim does not chase it.

Patch 41 does the display alignment of patch 32 for a seek and an ordinary
start too.

Patch 44 lets a preparation keep taking pictures from a decoder whose input is
full, which otherwise stops handing them out while the player is paused.

Patch 45 moves the audio clock slowly: the receiver dropped the sound when the
trim swung it hundreds of ppm within seconds.
