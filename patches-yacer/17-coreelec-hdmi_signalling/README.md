# Telling the sink what it is being sent

A display can only lock to a format it can identify. The 4k modes at 24, 25 and
30Hz are named to it by the HDMI 1.4b vendor infoframe rather than by the AVI,
and a packet that carries someone else's payload for a while has to give that
name back when it is done.
