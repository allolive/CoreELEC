# Pause and resume without losing the sound

Bitstreamed audio - TrueHD, Atmos, DTS-HD, DTS:X - is decoded by the receiver
rather than the box. Pausing stops the carrier, the receiver drops its lock,
and on resume the sound arrives late or not at all while the picture has
already started.

Keep the carrier alive across the pause where the sink allows it, prepare the
resume while the viewer is still paused so play has only to release what is
already held, and hold the first picture back until the audio is genuinely
running rather than merely submitted.
