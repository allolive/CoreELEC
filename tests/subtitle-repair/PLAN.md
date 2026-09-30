# OCR repair: the port

The spec, agreed 2026-09-29:

- The subtitle's language comes from its language code; with none, CLD2 detects it. No
  language, no OCR repair.
- Each file (or track) learns its own misreadings: every non-word is tried against the OCR
  confusions (l/I/1/|, 0/O, rn/m, cl/d, li/h, dropped accents). Only lines in the subtitle's
  language count as evidence.
- A confusion is the file's when it is frequent: a real share of how the file writes that
  letter (MIN_SHARE), or across many different words (MANY_WORDS). Exceptions are left alone.
- A word is corrected only through the file's confusions, only into exactly one word, only in
  a line in the subtitle's language. Names the file uses mid-sentence, lines another language
  explains better, accents on words another language has, numbers and Roman numerals are left.
- Hunspell is the dictionary; CLD2 the language detector. A language is a pack: its Hunspell
  dictionary and a profile (one-letter words, capitalised nouns, the lone "1"/"l" for "I",
  alphabet, accents). English is built in; the others are add-ons.

## Milestones, in order

1. The Python reference, final and portable: explicit character classes and case tables (no
   Python Unicode methods the C++ cannot reproduce), packs read from files, learning only from
   the subtitle's language, per-language facts from the profile, a cue-by-cue mode for tracks
   inside the video. Re-run on every corpus; every change against the current engine reviewed.
2. CoreELEC packages: hunspell and cld2; Kodi finds them (optional dependencies). Done
   2026-09-30: both cross-compile static, Kodi's configure reports them enabled.
3. The C++ engine in the patch, replacing the English-only rules; gtests; identical output to
   the reference on every corpus. Done 2026-09-30: 899 files, whole and cue by cue (tagged and
   detected), identical; 48 gtests, clean under ASan/UBSan.
4. Kodi integration: the English pack built in, pack discovery, settings; full build; the box.
   Built 2026-09-30 (20260930002101): English in system/subtitlecleanup/en, other packs from
   enabled library add-ons carrying resources/subtitlerepair/<language>/, the track's language
   through CDVDStreamInfo. Not yet on the box: timing and memory there still to measure.
5. Packs for fr, es, de, it, nl, pt as add-ons in kodi_addons, generated from LibreOffice's
   dictionaries at a pinned revision. Done 2026-09-30 (script.module.subtitlerepair.<language>
   .allolive; the dictionaries are fetched at build time by revision and sha256).
