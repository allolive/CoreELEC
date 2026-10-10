# Frame-sequential 3D

Show 3D films on a 2D display with active shutter glasses: Kodi's stereoscopic
mode "HFR VRR". The video layer alternates the eyes on every vsync, game VRR runs
the output at a whole multiple of the film rate, the GUI is drawn once, and an
external emitter follows the eye the box reports; the emitter is fed by the
`service.shutter3d.allolive` add-on.

The kernel patches:
- 01 reports which eye each vsync carries, and lets it be swapped.
- 02 makes game VRR settable under DRM (`vrr_rate`) and exact on any base mode.
- 03 scans out the GUI half that matches the eye the video shows on each frame
  (not used by HFR VRR, which draws the GUI once).
- 04 shows MVC one eye per vsync on linear-MIF chips (S6): one full picture, the
  eye of each vsync loaded into it, a frame kept by a pause included.
- 05 switches vd2 off when MVC is shown as one picture: the frames before the 3D
  mode was set had switched it on for the second eye.
- 06 adds 1080p165 (read from a DisplayID) as a game-VRR base, with exact
  1000/1001 rates on it: the TV draws each eye in 5.6 ms instead of 8.
- 07 refuses a vrr_rate the mode cannot take before touching VRR (-EINVAL),
  and leaving VRR gives back the mode's line count and clears the VRR packet.

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
  rate, the GUI drawn once, and the film shown once the output has switched.
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
