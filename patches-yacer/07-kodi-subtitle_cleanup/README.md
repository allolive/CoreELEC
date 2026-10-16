# Subtitle repair and translation

A text subtitle should show what its author wrote, not the damage it picked up
on the way: text read in the wrong encoding, music notes an OCR turned into
other signs, letters it misread, advertising and uploader credits, and lines
timed so they cannot be seen or make the dialogue jump. Kodi repairs these as
it reads the subtitle - a file beside the video or a track inside it - and
changes nothing else: not the words, not the formatting, not the file.

A video with no subtitle in the language the viewer reads often has one in
another. With "Translate subtitles" on, Kodi offers that one translated, as one
more entry in the subtitle list ("Auto-translated to French (from English)"),
and plays it the way it would play a subtitle in the viewer's language. A
language model translates it, through the OpenAI chat API: OpenRouter, OpenAI,
or a server of one's own on the network, with the model the viewer picks. A
provider that does not answer offers no translation.

The same provider can correct what an OCR misread and the rules left, because
the subtitle never repeats it: the words the rules doubt, or every line. The
model answers with a list of changes, and the rules make only those that turn
what is no word into a word close to it.

A subtitle file made for another release is late, early, or drifting. Kodi
reads a few stretches of the video's own subtitles, in any language, and when
one change - an offset, and a frame-rate ratio - lines most of their starts up
with the file's, moves every line by it. A video with no subtitle of its own is
listened to instead: Silero's voice activity model, its weights shipped in an
add-on, hears where someone speaks, a minute at a time across the film until
the change that lays the lines on the speech stands clearly above any other.
A file made for another cut of the film - an extended edition, a scene trimmed -
is laid in parts, each run of lines at the offset its speech asks for, and read
on ahead of playback as the film plays.
