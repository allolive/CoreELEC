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

The decoder patch (media_modules-aml):
- 01 pins both MVC views' buffers for a frame kept on screen (pause, seek), so
  the paused 3D picture is not overwritten.

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
- 10 does not prepare a resume behind a pause on MVC: the AML codec says a
  flush of it is costly (each one re-creates the MVC decoder), so the add-on's
  pause and resume are plain speed changes.
