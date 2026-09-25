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

Holding the picture across a passthrough resume needs the render loop to keep
turning, so a frame the GUI skipped is presented rather than drawn and thrown
away; without that the player waits for a buffer that never comes back.
