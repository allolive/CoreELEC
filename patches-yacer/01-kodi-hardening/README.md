# Kodi robustness

Kodi's own parsers and helpers should survive malformed, truncated or
unreadable input rather than trusting it, and its threads should not outlive
what they use. A disc's title change should not hold the player up either:
stop, pause and seek keep working while the old title plays out, and the
time shown is the picture's. No Amlogic in any of it, so it is all
upstreamable.
