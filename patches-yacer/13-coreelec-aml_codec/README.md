# Video decoding

The Amlogic decoder is a black box that can wedge, drain or never open. Every
one of those should end in playback stopping cleanly or carrying on.

common_drivers 01 makes amlvideo take back only the frame QBUF names: it took
every older frame with it, and a release ahead of an older one sent the whole
queue to the screen early.
