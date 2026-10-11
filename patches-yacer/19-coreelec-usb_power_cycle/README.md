# coreelec: USB power cycle on restart

The patches:
- 01 makes a restart switch the board's USB supplies off for two seconds
  before the firmware resets the SoC, as a power cut would: on the Ugoos
  AM9 Pro a warm restart left the USB 2.5G network adapter silent about
  one boot in four. Set for the AM9 / AM9 Pro (HUB_5V, USB5V).
