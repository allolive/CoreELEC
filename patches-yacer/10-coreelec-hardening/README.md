# Driver robustness

The Amlogic drivers and the video decoders should meet malformed input,
contention and their own failure paths with an error rather than a corrupted
buffer, a leak or a crash. Nothing here changes what the box does when nothing
goes wrong.
