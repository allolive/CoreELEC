# Blu-ray menus

The menus Blu-ray discs carry, Java ones included, playing as a standalone player
plays them.

The `libbluray` patch fixes the library Kodi plays them through.

Kodi patch 08 lets each disc play as the region chosen for it from its playback
choices, or as the only region its HDMV program asks for; it keeps the choice in
the film file of group 18 (patch 49).

Kodi patch 09 ends playback when a disc's HDMV program has ended, where Kodi read
again and again at once until stopped by hand.

Kodi patch 10 counts a Java disc as in a menu only while its graphics show, so
Select opens the player bar during the film instead of going to an empty menu.
