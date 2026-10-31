# Video decoding

The Amlogic decoder is a black box that can wedge, drain or never open. Every
one of those should end in playback stopping cleanly or carrying on.

common_drivers 01 makes amlvideo take back only the frame QBUF names: it took
every older frame with it, and a release ahead of an older one sent the whole
queue to the screen early.

kodi 17 sends MJPEG to software decoding: the hardware locked the S6 up on a
4K cover picture kept as a one-frame video track.

kodi 18 pairs only HEVC or AVC tracks as Dolby Vision layers: a cover picture
kept as a video track was the base layer the film was paired with, and the
player showed the cover.
