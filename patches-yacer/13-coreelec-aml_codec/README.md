# Video decoding

The Amlogic decoder is a black box that can wedge, drain or never open. Every
one of those should end in playback stopping cleanly or carrying on.

Patch 16 hands frames back to amlvideo in the order they came out: one
released ahead of an older frame sent the whole queue to the screen early.
