# Frame-sequential 3D

Show 3D films on a 2D display with active shutter glasses: Kodi's stereoscopic
mode "HFR VRR". The video layer alternates the eyes on every vsync, game VRR runs
the output at a whole multiple of the film rate, each eye gets its own GUI at full
1080p, and an external emitter follows the eye the box reports; the emitter is fed by the
`service.shutter3d.allolive` add-on.

The kernel patches:
- 01 reports which eye each vsync carries, and lets it be swapped.
- 02 makes game VRR settable under DRM (`vrr_rate`) and exact on any base mode.
- 03 scans out the GUI half that matches the eye the video shows on each frame.
- 04 shows MVC one eye per vsync on linear-MIF chips (S6): one full picture, the
  eye of each vsync loaded into it, a frame kept by a pause included.
- 05 switches vd2 off when MVC is shown as one picture: the frames before the 3D
  mode was set had switched it on for the second eye.
- 06 adds 1080p165 (read from a DisplayID) as a game-VRR base, with exact
  1000/1001 rates on it: the TV draws each eye in 5.6 ms instead of 8.
- 07 refuses a vrr_rate the mode cannot take before touching VRR (-EINVAL),
  and leaving VRR gives back the mode's line count and clears the VRR packet,
  also when the output goes away (plugout, suspend, reboot).
- 08 keeps a 3D type the player sets from being lost to the vsync's MVC bit.
- 09 publishes VRR as enabled only after the configuration it enables.
- 10 places one eye that many pixels right for 3D depth (`fa_depth`), on its
  vsyncs only, where the video window leaves the room on screen.
- 11 sends a VESA timing (1080p165) as YCbCr 4:4:4, 10-bit where it fits, when
  the sink takes 4:4:4: the RGB the S6 made for it was full range in a frame
  stating limited, and the picture came out dark.
- 12 makes the fast game-VRR base the sink's own fastest 1080p above 120 Hz
  (144, 165, 240 Hz), read from its EDID (DisplayID Type I or VII, detailed
  timings) within the link's 600 MHz, in place of one fixed 1080p165 timing.
- 13 makes fa_sync give the eye on screen (the one RDMA put out at this vsync,
  not the one chosen for the next) and the time its first active line leaves
  the encoder: a glasses delay of 0 is the start of the eye's picture.

The decoder patch (media_modules-aml):
- 01 pins both MVC views' buffers for a frame kept on screen (pause, seek), so
  the paused 3D picture is not overwritten.
- 02 hands MVC frame slots out and takes them back in any order, with a slot
  pool, a ready queue and buffers returned by reference, so a picture held by
  the display no longer blocks the next one or leaks a buffer after a restart.
- 03 reports a 720p picture as 720 lines high, not 736.
- 04 publishes a picture only once both its views are decoded.
- 05 looks a picture's pts up a little inside its own packet: the firmware
  now and then reports a position just before it, and two neighbouring
  pictures then came out with each other's timestamps.

The Kodi patches:
- Adds the stereoscopic mode HFR VRR, offered where the platform supports it
  (on Amlogic: game VRR and no HDMI 3D); the other modes are plain Kodi.
- MVC is then decoded with both views. Without HDMI 3D, that and the rest of
  Kodi's 3D handling apply only for a user who has chosen HFR VRR: on a 2D TV
  with game VRR everyone else plays 3D files as 2D, as before.
- In HFR VRR the output stays 2D 1080p 120 Hz, with ALLM and VRR at the film's
  rate, and the film shown once the output has switched.
- 02 adds the 3D glasses delay (videoscreen.fs3ddelay) for the emitter, set in
  the player's video settings with the audio offset's slider; the add-on reads it.
- 03 adds the actions fs3ddelay and stereoinvert and the infolabels
  Player.Fs3dDelay and Player.StereoInvert, so a skin can offer both next to
  the audio and subtitle offsets.
- 04 runs HFR VRR on 1080p165 where the sink lists it, 1080p120 otherwise.
- 05 times the picture by the output's real refresh rate in HFR VRR (an even
  multiple of the film rate, not Kodi's resolution's), and looks the
  advancedsettings latency up by it, so 3D can have a latency of its own.
- 06 starts MVC, after an open or a seek, at a picture that carries its own
  SPS and PPS: a remux whose header holds stale ones never showed a picture
  when it was resumed at an IDR without them. A seek waits only for a stream
  whose own sets differ from the header's.
- 07 lets Kodi start on a 1000/1001 mode its list has no entry for: one that
  died in 3D at 23.976 fps left the output at 119.88 Hz, and every restart
  failed to create its window until a reboot.
- 08 draws each eye's GUI at full 1080p while frame-sequential 3D is on, the
  left above the right in a surface of twice the height (the right above with
  Invert), and kernel patch 03 shows the eye's half: the subtitle depth setting
  and the skin's 3D effect strength work in HFR VRR. Otherwise the GUI is drawn
  once, as before.
- 09 gives Blu-ray PG subtitles the disc's own depth, frame by frame (MVC
  "1 plane + offset"), each picture by its own pts; the disc's depth replaces
  the subtitle depth setting, which covers subtitles without it.
- 11 consumes an empty packet instead of handing it back forever.
- 12 starts the Blu-ray second view where the first starts, after a seek too.
- 13-17 make the AML decoder recover on its own: one NAL walker for the
  packet scanners (in its own header, so the depth parser stays testable); the
  hvcC length size read as the converter reads it; a decoder timed out on no
  progress or no picture rather than on the last picture; each packet's header
  and pts written once, with its outcome reported (added, dropped, retry,
  reset), a refused packet fed again unchanged; and after a reset, decoding
  resumes at a keyframe instead of on macroblocks.
- 18-20 are plain Kodi fixes the Blu-ray 3D work needs: a Blu-ray stream set
  up again is not its own duplicate, a menu-selected stream takes its new
  hints, and the play item's clip is found again after an angle change.
- 21-31 make the Blu-ray 3D second view follow the first: restarted at a clip
  join or seek, kept across a stream change, read on through a failed read or
  a stalled source, the base view's wait for it bounded (resynced by the play
  item each picture came from), and, when it cannot be read, the base view
  played alone as 2D for the rest of the playlist.
- 32 picks the output mode again when the HDMI 3D signal changes in playback,
  so a 2D fallback leaves frame packing.
- 35 adds a 3D depth for each film (-20 to 20 px, 0 unless set), kept in
  `stereodepth.json` in the profile rather than in the shared video database,
  with the action stereodepth (its slider) and Player.StereoDepth: the window is
  narrowed by the depth and kernel patch 10 moves one eye by as much.
- 36 notifies the rate Active 3D plays at once the output runs ("23.976 fps
  at 95.904 Hz, 1080p 165 Hz base") in place of the stereo mode's name.
- 37 keeps the window when Kodi picks the film's resolution after entering
  HFR VRR: only the film's rate decides the output there, and the rebuild
  cost a second audio sink reopen a second into every 3D film.
- 38 gives every subtitle on a 3D film the disc's depth, text and translated
  ones included, not only PG ones; and a sequence that has carried only
  zeros counts as no depth, so the configured one stands in until the disc
  gives a value.
- 39 tells the player HFR VRR is coming while it is wanted and not yet on,
  so a 3D film's sound starts after the switch (11_25 waits for it).
- 40 runs HFR VRR on that fast 1080p, whatever its rate, where the sink's
  VRR range reaches it; the film still plays at 96 Hz (95.904 for 23.976).
- 41 names the 3D mode Active 3D for VRR displays: it no longer runs on 120 Hz only.
- 42 lets each film move its subtitles closer or further in 3D, on top of the
  disc's or the configured depth, from the subtitle settings; kept with its 3D depth.

- 43 puts back the timestamps of two pictures the MVC decoder hands out
  swapped after a restart, holding one picture back; one older than a picture
  already out goes as a drop rather than stepping the film back.
- 44 takes a whole-refresh 3D picture offset out of the frame latency at each
  start and resume, once settled, clock and audio untouched.
