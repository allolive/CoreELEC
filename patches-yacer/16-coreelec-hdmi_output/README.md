# What the sink is told over HDMI

Let a calibration pattern add-on choose the HDMI colour format, bit depth and
quantization range through `/sys/class/amhdmitx/amhdmitx0/user_attr`. Until
something writes to it, the box behaves as stock.

The 4k modes that carry no VIC in the AVI are named to the sink by the HDMI 1.4b
vendor infoframe instead, and that slot is shared with the Dolby Vision and
HDR10+ payloads. Releasing it puts the name back in the same operation, so the
sink is never left without one.
