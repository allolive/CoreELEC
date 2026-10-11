# Kodi robustness

Kodi's own parsers and helpers should survive malformed, truncated or
unreadable input rather than trusting it, and its threads should not outlive
what they use. No Amlogic in any of it, so it is all upstreamable.
