"""The subtitle repairs in Python: the reference SubtitleCleanup.cpp (group 07) was ported from
and is checked against, byte for byte, by check_library.py beside it. A rule changes here first,
is proven on a real library there, and is then carried to the C++.

Regular expressions use ASCII classes (re.ASCII) because std::wregex in the classic locale
does: the two must mean the same thing by \\w, \\s, \\d, \\b and case folding."""
import re
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterable, List, Optional, Set, Tuple



# -- SubRip ----------------------------------------------------------------------------------
#
# SubRip, read and written back without touching what we do not change: a cue keeps its
# number, its timing line and every text line exactly as they were.

_BLOCK_SPLIT = re.compile(r"\n[ \t]*\n")


@dataclass
class Cue:
    number: str                 # as written; renumbered only when a cue is dropped
    timing: str                 # the "-->" line, verbatim
    lines: List[str] = field(default_factory=list)


@dataclass
class Block:
    """One blank-line separated block: a cue, or text that is not one (kept verbatim)."""
    cue: Optional[Cue]
    raw: str


def parse(text: str) -> List[Block]:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    blocks = []
    for raw in _BLOCK_SPLIT.split(text.strip("\n")):
        if not raw.strip():
            continue
        lines = raw.split("\n")
        while lines and not lines[0].strip():            # a block after a whitespace-only line
            lines.pop(0)
        timing_at = next((i for i, line in enumerate(lines[:3]) if "-->" in line), None)
        if timing_at is None:
            blocks.append(Block(None, raw))
            continue
        # the number is the line before the timing; one before it ("776" in "776/799/...") is
        # a stray the source left, kept with the number so nothing is lost
        number = "\n".join(lines[:timing_at])
        blocks.append(Block(Cue(number, lines[timing_at], lines[timing_at + 1:]), raw))
    return blocks


def serialize(blocks: List[Block], renumber: bool) -> str:
    out = []
    n = 0
    for block in blocks:
        if block.cue is None:
            out.append(block.raw)
            continue
        n += 1
        cue = block.cue
        number = str(n) if renumber else cue.number
        head = [number] if number else []
        out.append("\n".join(head + [cue.timing] + cue.lines))
    return "\n\n".join(out) + "\n"


# -- timing ----------------------------------------------------------------------------------
#
# Timing that is plainly broken - a line that cannot be seen, or that makes the dialogue jump.
# Timing that works is trusted as it is, however it was authored: gaps, short lines that can be
# seen, long lines and deliberate overlaps are left alone. A start time is never moved; an end
# time is moved only to meet the next line or to let a line be seen at all:
#
# - a line that ends before it starts, or is up for less than MIN_SHOWN (two frames at 24 fps),
#   stays until READABLE or until the next line starts, whichever comes first - Kodi never
#   shows the first kind and at most flashes the second;
# - a line that runs MIN_OVERLAP (a frame) to MAX_SLOP into the next one at the same place on
#   screen ends where that one starts. Kodi (libass) stacks lines that overlap, so the dialogue
#   jumps up and back down for those frames; half a second of two lines cannot be read, so
#   such an overlap is not two lines meant to be read together. An overlap shorter than a
#   frame rarely lands on one and is left alone;
# - a timing line with junk around its arrow ("01:06:49,130 ??--> ...") is written plainly, or
#   Kodi cannot read it and drops the line.

MIN_SHOWN = 84          # ms: two frames at 23.976 fps
READABLE = 1000
MIN_OVERLAP = 42        # ms: one frame
MAX_SLOP = 500

_STAMP = r"(\d{1,2}):(\d{2}):(\d{2})([,.])(\d{1,3})"
_TIMING = re.compile(rf"^(\s*){_STAMP}(\s*\S*?-->\S*?\s*){_STAMP}(.*)$", re.ASCII)
_POSITION = re.compile(r"\{\\an(\d)\}", re.ASCII)


def _ms(h: str, m: str, s: str, frac: str) -> int:
    return ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(frac.ljust(3, "0")[:3])


def _stamp(ms: int, sep: str) -> str:
    h, rest = divmod(ms, 3600000)
    m, rest = divmod(rest, 60000)
    s, f = divmod(rest, 1000)
    return f"{h:02d}:{m:02d}:{s:02d}{sep}{f:03d}"


class _Timed:
    def __init__(self, cue: Cue):
        self.cue = cue
        m = _TIMING.match(cue.timing)
        self.ok = m is not None
        if m is None:
            return
        g = m.groups()
        self.lead, self.arrow, self.tail = g[0], g[6], g[12]
        self.start, self.end = _ms(g[1], g[2], g[3], g[5]), _ms(g[7], g[8], g[9], g[11])
        self.start_text = cue.timing[m.start(2):m.end(6)]
        self.end_text = cue.timing[m.start(8):m.end(12)]
        self.sep = g[10]
        self.orig_end = self.end
        text = " ".join(cue.lines)
        pos = _POSITION.search(text)
        self.position = pos.group(1) if pos else "2"

    def write(self) -> Optional[str]:
        """The rewritten timing line, or None if it did not change."""
        clean_arrow = self.arrow.strip() != "-->"
        if self.end == self.orig_end and not clean_arrow:
            return None
        end = _stamp(self.end, self.sep) if self.end != self.orig_end else self.end_text
        return f"{self.lead}{self.start_text} --> {end}{self.tail}"


def fix_timing_of(cues: List[Cue]) -> List[Tuple[str, str, str]]:
    """Fix the cues in place; returns (what, old timing, new timing) for each change."""
    timed = [_Timed(c) for c in cues]
    following: List[Optional[_Timed]] = [None] * len(timed)      # the next readable cue
    upcoming: Optional[_Timed] = None
    for i in range(len(timed) - 1, -1, -1):
        following[i] = upcoming
        if timed[i].ok:
            upcoming = timed[i]
    for i, t in enumerate(timed):
        if not t.ok:
            continue
        nxt = following[i]
        next_start = nxt.start if nxt is not None and nxt.start > t.start else None
        if t.end - t.start < MIN_SHOWN:
            target = t.start + READABLE
            if next_start is not None:
                target = min(target, next_start)
            if target > t.end:
                t.end = target
        elif (next_start is not None and nxt is not None and nxt.position == t.position
              and MIN_OVERLAP <= t.end - next_start <= MAX_SLOP):
            t.end = next_start
    changes = []
    for t in timed:
        if not t.ok:
            continue
        new = t.write()
        if new is None:
            continue
        what = ("too short" if t.orig_end - t.start < MIN_SHOWN else "overlap") \
            if t.end != t.orig_end else "arrow"
        changes.append((what, t.cue.timing, new))
        t.cue.timing = new
    return changes


# -- OCR letters -----------------------------------------------------------------------------
#
# Letters a subtitle OCR confused, put back in English text. A word is changed only when it
# is not a word, one of these confusions explains it, and undoing that gives a word:
#
# - a capital I among lower-case letters stands for l ("wouId", "IittIe", "Iike");
# - a word starting with l and a consonant l can't precede starts with I ("lnspector", "lts");
# - l on its own, or "lf", is I or If;
# - a zero among letters is an o ("N0", "g0t");
# - a final l after capitals is an I ("FBl").
#
# Nothing else is touched: a name or a word the list lacks stays as it is.

_STANDALONE = {"l": "I", "lf": "If", "l'm": "I'm", "l'll": "I'll", "l've": "I've", "l'd": "I'd"}
_SUFFIXES = ("'s", "'re", "'ve", "'ll", "'d", "n't", "'m")
_APOSTROPHES = "'\u2019"
_MARKUP = re.compile(r"(<[^>]*>|\{[^}]*\})")
_EN_STOP = set("the and you to of it is that what in me this we he for have not be your do are "
               "was on".split())
_FR_STOP = set("le la les et vous tu de des est que qui pas je une un il ne dans pour ce".split())
# around a lone "l" read as "I": what may come before it, and after it
_BEFORE_LONE = "\"'([\u201c\u2018\u00bf\u00a1*.\u2026"
_AFTER_LONE = ".,!?;:)]\"\u201d\u2019\u2026*"
ENGLISH_SHARE = 0.6     # of a line's other words, for its letters to be corrected


def is_word_char(c: str) -> bool:
    """ASCII letters and digits, and any character beyond ASCII that is not white space,
    punctuation or a symbol: an accented or non-Latin letter belongs to its word ("là",
    "Hélène", "lähden"), so the "l" in it is not a word of its own."""
    cp = ord(c)
    if cp < 0x80:
        return c.isalnum()
    return not (c.isspace() or cp <= 0xBF or cp in (0xD7, 0xF7) or 0x2000 <= cp <= 0x2BFF
                or 0x3000 <= cp <= 0x303F or 0xFE10 <= cp <= 0xFE6F or 0xFF00 <= cp <= 0xFF0F
                or 0x1F000 <= cp <= 0x1FAFF or cp == 0xFEFF)


def words_of(text: str) -> List[Tuple[int, int]]:
    """The words of a text as (start, length): word characters, joined by an apostrophe
    followed by more of them ("l'm", "couldn’t")."""
    words = []
    i = 0
    while i < len(text):
        if not is_word_char(text[i]):
            i += 1
            continue
        start = i
        while i < len(text) and is_word_char(text[i]):
            i += 1
        while i + 1 < len(text) and text[i] in _APOSTROPHES and is_word_char(text[i + 1]):
            i += 1
            while i < len(text) and is_word_char(text[i]):
                i += 1
        words.append((start, i - start))
    return words


def load_words(path: str) -> Set[str]:
    with open(path, encoding="utf-8") as f:
        return set(f.read().split())


def looks_english(lines: Iterable[str]) -> bool:
    words = [line[a:a + n].lower() for line in lines for a, n in words_of(line)][:4000]
    en = sum(w in _EN_STOP for w in words)
    fr = sum(w in _FR_STOP for w in words)
    return en > 50 and en > 2 * fr


def _plain(word: str) -> bool:
    return all(c.isascii() or c == "\u2019" for c in word)


class Fixer:
    def __init__(self, words: Set[str]):
        self.words = words

    def known(self, word: str) -> bool:
        w = word.lower().replace("\u2019", "'")
        if w in self.words:
            return True
        return any(w.endswith(s) and w[:-len(s)] in self.words for s in _SUFFIXES)

    def fix_word(self, w: str) -> Optional[str]:
        """The word as it was before an OCR misread it, or None. Only plain English letters:
        a word with an accent is another language's."""
        if not _plain(w):
            return None
        straight = w.replace("\u2019", "'")
        if straight in _STANDALONE:
            return _STANDALONE[straight].replace("'", "\u2019") if straight != w else _STANDALONE[w]
        if len(w) < 2 or self.known(w):
            return None
        candidates = []
        if re.search(r"[a-z]I", w) or (len(w) >= 3 and re.match(r"I[a-z]", w)):
            inner = re.sub(r"(?<=[a-z])I", "l", w)
            candidates += [inner, "l" + inner[1:] if inner[0] == "I" else inner,
                           w.replace("I", "l")]
        if len(w) >= 3 and re.match(r"l[bcdfghjkmnpqrstvwxz]", w):
            candidates.append("I" + w[1:])
        letters = re.sub(r"[^A-Za-z]", "", w)
        if re.search(r"[A-Za-z]0|0[A-Za-z]", w) and not re.search(r"[1-9]", w):
            if letters.islower():
                candidates.append(w.replace("0", "o"))
            elif letters.isupper():
                candidates.append(w.replace("0", "O"))
        if re.fullmatch(r"[A-Z]{2,}l", w):
            candidates.append(w[:-1] + "I")
        # a capital after a small letter is no word's spelling ("alI" for "aII")
        return next((c for c in candidates
                     if c != w and not re.search(r"[a-z][A-Z]", c) and self.known(c)), None)

    @staticmethod
    def _alone(text: str, start: int, length: int) -> bool:
        """A lone "l" stands for "I" only between spaces and sentence punctuation: joined by
        a hyphen to a letter it is a stutter or a word spelt out ("l-l-low", "l-T-H"), inside
        punctuation a damaged word ("Sa?l")."""
        before = text[start - 1] if start else ""
        if before == "-":
            ahead = text[start - 2] if start >= 2 else ""
            before_ok = ahead == "" or ahead.isspace()
        else:
            before_ok = before == "" or before.isspace() or before in _BEFORE_LONE
        rest = text[start + length:]
        after = rest[:1]
        if after == "-":
            # a dash that ends there ("l-- I'm sorry", "l- I hope") breaks off a word; one
            # followed by a letter joins a stutter or a word spelt out
            dashes = len(rest) - len(rest.lstrip("-"))
            return before_ok and (dashes == len(rest) or rest[dashes].isspace())
        return before_ok and (after == "" or after.isspace() or after in _AFTER_LONE)

    def reads_as_english(self, line: str, skip: Set[int]) -> bool:
        """Whether a line's other words are mostly English: a French line in an English
        subtitle, or an English one in another, keeps its words as they are. Names are not
        words of any language, so a word starting with a capital A-Z is not weighed, nor a
        number; a line with nothing else ("lnside.", "Mr. Yadav? l'm Shahid Khan.") has
        nothing against it."""
        visible = "".join(p for i, p in enumerate(_MARKUP.split(line)) if not i % 2)
        others = [visible[a:a + n] for k, (a, n) in enumerate(words_of(visible))
                  if k not in skip and n >= 2 and not ("A" <= visible[a] <= "Z" or "0" <= visible[a] <= "9")]
        return sum(self.known(w) for w in others) >= ENGLISH_SHARE * len(others)

    def fix_line(self, line: str, on_fix: Callable[[str, str], None]) -> str:
        visible = "".join(p for i, p in enumerate(_MARKUP.split(line)) if not i % 2)
        pending = set()
        for k, (a, n) in enumerate(words_of(visible)):
            word = visible[a:a + n]
            if self.fix_word(word) is not None and (
                    word.replace("\u2019", "'") not in _STANDALONE or self._alone(visible, a, n)):
                pending.add(k)
        if not pending or not self.reads_as_english(line, pending):
            return line

        def fix_text(text: str) -> str:
            out, last = [], 0
            for a, n in words_of(text):
                word = text[a:a + n]
                fixed = self.fix_word(word)
                if fixed is not None and (word.replace("\u2019", "'") not in _STANDALONE
                                          or self._alone(text, a, n)):
                    out.append(text[last:a] + fixed)
                    on_fix(word, fixed)
                else:
                    out.append(text[last:a + n])
                last = a + n
            return "".join(out) + text[last:]

        parts = _MARKUP.split(line)     # tags stay as they are; only the text between is read
        return "".join(p if i % 2 else fix_text(p) for i, p in enumerate(parts))


# -- the repairs -----------------------------------------------------------------------------
#
# The cleanup rules. Each one fixes a known kind of damage and nothing else: the words of the
# dialogue, the timings and the formatting tags are never changed.
#
# - encoding: a file that is not UTF-8 is read in the code page of its language, and text that
#   was UTF-8 once read as Windows-1252 ("â™ª", "Ã©") is put back;
# - music notes: a note that came out of OCR as "¶", as a pair of "J"s, or as "#" in a file that
#   writes its notes that way, or as an emoji the subtitle font has no glyph for, becomes "♪";
# - OCR letters (English only): "wouId", "lnspector", "N0" become "would", "Inspector", "NO"
#   when the word list says so - see ocr.py;
# - timing: a line too short to be seen, or running into the next one - see timing.py;
# - advertising: lines that sell something or credit the uploader are removed, and a cue left
#   with nothing is dropped.

VERSION = "2"       # part of the cache key: bump when a rule changes what it produces

NOTE = "\u266a"     # ♪

# -- encoding -------------------------------------------------------------------------------

# the code page a subtitle in each language was most likely saved in, by ISO 639-1/-2 code or
# by the English name a file is often labelled with ("Arabic.srt")
_CODEPAGES = {
    "cp1252": "en eng english fr fre fra french de ger deu german es spa spanish it ita italian "
              "pt por portuguese nl dut nld dutch da dan danish sv swe swedish no nor nb nob "
              "norwegian fi fin finnish is ice isl icelandic ca cat catalan",
    "cp1250": "pl cs cz sk hu hr sl ro bs pol cze ces slo slk hun hrv scr slv rum ron bos polish "
              "czech slovak hungarian croatian slovenian romanian bosnian",
    "cp1251": "ru uk bg be mk rus ukr bul bel mac mkd russian ukrainian bulgarian macedonian",
    "cp1253": "el gre ell greek",
    "cp1254": "tr tur turkish",
    "cp1255": "he iw heb hebrew",
    "cp1256": "ar fa ara per fas arabic persian farsi",
    "cp1257": "lt lv et lit lav est lithuanian latvian estonian",
    "cp1258": "vi vie vietnamese",
}
_CODEPAGE_OF = {code: cp for cp, codes in _CODEPAGES.items() for code in codes.split()}


def language_of(tags: List[str]) -> Optional[str]:
    """The language a subtitle's file name gives, as written ("en", "fre", "Arabic")."""
    return next((t for t in tags if t.lower() in _CODEPAGE_OF), None)


def language_of_path(path: str) -> Optional[str]:
    """The language a subtitle file is labelled with: one of the last three words of its name
    ("Film.en.forced.srt", "Film_English.srt", "Arabic.srt")."""
    name = path.replace("\\", "/").rsplit("/", 1)[-1]
    stem = name.rsplit(".", 1)[0] if "." in name else name
    # the code nearest the extension is the subtitle's: "...-bg.REMUX-Danishbits.da.srt" is Danish
    return language_of(list(reversed(re.split(r"[._ ]", stem)[-3:])))


def codepage_for(language: Optional[str]) -> Optional[str]:
    return _CODEPAGE_OF.get((language or "").lower())


def _looks_misread(text: str) -> bool:
    """Text read as Windows-1252 that is mostly accented capitals and the like is some other
    code page: Arabic, Cyrillic, Greek and Hebrew all land in 0xC0-0xFF."""
    letters = [c for c in text if c.isalpha()]
    high = sum(1 for c in letters if "\u00c0" <= c <= "\u00ff")
    return len(letters) > 50 and high > 0.3 * len(letters)


def decode(data: bytes, language: Optional[str]) -> Tuple[str, str]:
    """(text, how it was read). A file that is not UTF-8 is read line by line: files stitched
    together from two sources carry UTF-8 lines among code-page ones, and each is read in its
    own encoding ("mixed"). "unsure" when the language is not known and a Western reading makes
    no sense: such a file is left for Kodi to read."""
    for bom, codec in ((b"\xef\xbb\xbf", "utf-8"), (b"\xff\xfe", "utf-16-le"),
                       (b"\xfe\xff", "utf-16-be")):
        if data.startswith(bom):
            return data[len(bom):].decode(codec, errors="replace"), codec
    try:
        return data.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        pass
    codepage = codepage_for(language)
    if codepage is None and _looks_misread(data.decode("cp1252", errors="replace")):
        return data.decode("cp1252", errors="replace"), "unsure"
    codepage = codepage or "cp1252"
    lines, utf_lines = [], 0
    for raw in data.split(b"\n"):
        try:
            line = raw.decode("utf-8")
            utf_lines += any(b >= 0x80 for b in raw)
        except UnicodeDecodeError:
            line = raw.decode(codepage, errors="replace")
        lines.append(line)
    return "\n".join(lines), (f"mixed:{codepage}" if utf_lines else codepage)


def _cp1252_byte(ch: str) -> Optional[int]:
    return _BYTE_OF.get(ch)


# every character Windows-1252 (or Latin-1, for its five holes) shows for a byte 0x80-0xFF
_BYTE_OF: Dict[str, int] = {}
for _b in range(0x80, 0x100):
    try:
        _BYTE_OF[bytes([_b]).decode("cp1252")] = _b
    except UnicodeDecodeError:
        _BYTE_OF[chr(_b)] = _b


def _plausible(ch: str) -> bool:
    """What UTF-8 read as Windows-1252 turns back into in a subtitle: Latin letters, general
    punctuation (’ “ ” … –), currency and letterlike signs (€ ™), symbols (♪) and emoji. Nothing
    else: in Czech, "Úž" also happens to read as a UTF-8 pair - of an Arabic letter."""
    cp = ord(ch)
    return (0xA0 <= cp <= 0x17F or 0x2000 <= cp <= 0x206F or 0x20A0 <= cp <= 0x214F
            or 0x2190 <= cp <= 0x27BF or 0x1F300 <= cp <= 0x1FAFF)


_LEADS = (0xC2, 0xC3, 0xE2, 0xF0)


def _utf8_length(lead: Optional[int]) -> int:
    """How many continuation bytes a UTF-8 sequence starting with this byte has (0: none)."""
    if lead is None:
        return 0
    return 1 if 0xC2 <= lead <= 0xDF else 2 if 0xE0 <= lead <= 0xEF else 3 if 0xF0 <= lead <= 0xF4 else 0


def _unmojibake_once(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        lead = _cp1252_byte(text[i])
        need = _utf8_length(lead)
        # "Ã"/"Â" start an accented letter, "â" punctuation and symbols, "ð" an emoji; any
        # other lead is as likely to be Central European text ("Äš") as damage, so it stays
        if lead in _LEADS and need:
            tail = [_cp1252_byte(c) for c in text[i + 1:i + 1 + need]]
            seq = [b for b in tail if b is not None and 0x80 <= b <= 0xBF]
            if len(seq) == need:
                try:
                    ch = bytes([lead] + seq).decode("utf-8")
                except UnicodeDecodeError:
                    ch = ""
                if len(ch) == 1 and _plausible(ch):
                    out.append(ch)
                    i += 1 + need
                    continue
        out.append(text[i])
        i += 1
    return "".join(out)


def unmojibake(text: str) -> str:
    """Undo UTF-8 read as Windows-1252, twice over if it happened twice."""
    for _ in range(3):
        fixed = _unmojibake_once(text)
        if fixed == text:
            break
        text = fixed
    return text


_INVISIBLE = re.compile("[\u007f\u200b\u2060\ufeff]")      # zero-width, word joiner, DEL
_SEPARATORS = re.compile("[\u2028\u2029]")                  # Unicode line breaks: not SubRip's
_C1 = re.compile("[\u0080-\u009f]")
_LIGATURES = {"\ufb00": "ff", "\ufb01": "fi", "\ufb02": "fl", "\ufb03": "ffi", "\ufb04": "ffl",
              "\ufb05": "st", "\ufb06": "st"}
_ENTITIES = {"&amp;": "&", "&quot;": '"', "&apos;": "'", "&#39;": "'", "&#039;": "'",
             "&nbsp;": " "}
_ENTITY = re.compile("|".join(re.escape(e) for e in _ENTITIES), re.IGNORECASE | re.ASCII)


def _c1_as_cp1252(m: "re.Match[str]") -> str:
    """A C1 control is a Windows-1252 byte read as Latin-1: 0x9C is "œ", 0x92 is "’"."""
    try:
        return bytes([ord(m.group(0))]).decode("cp1252")
    except UnicodeDecodeError:
        return m.group(0)


def repair_chars(line: str, utf: bool) -> str:
    """Characters that were damaged on the way, put back; the words are not touched."""
    if utf:
        line = unmojibake(line)
    line = _C1.sub(_c1_as_cp1252, line)
    line = _INVISIBLE.sub("", line)
    line = _SEPARATORS.sub(" ", line)
    line = "".join(_LIGATURES.get(c, c) for c in line)
    return _ENTITY.sub(lambda m: _ENTITIES[m.group(0).lower()], line)


# -- music notes ----------------------------------------------------------------------------

_EMOJI_NOTES = re.compile("[\U0001F3B5\U0001F3B6\U0001F3BC]\uFE0F?")
_PILCROWS = re.compile("\u00b6+")                 # "¶" is never text in a subtitle
_SKIP = r"(?:<[^>]*>|\{[^}]*\}|\s)*"               # tags and spaces around the visible text
_LEAD_HASH = re.compile(rf"^({_SKIP}(?:-{_SKIP})?)#+(?!\d)", re.ASCII)   # "#260" is a number
_TRAIL_HASH = re.compile(rf"#+({_SKIP})$", re.ASCII)
_PAIRED = re.compile(rf"^({_SKIP})([Jj\u00cc\u00ec]) (.+) \2({_SKIP})$", re.ASCII)   # "J Happy birthday J"


def _visible(line: str) -> str:
    return re.sub(r"<[^>]*>|\{[^}]*\}", "", line)


def hash_is_note(lines: List[str]) -> bool:
    """Whether this file writes its music notes as "#". A hashtag ("#260", "#random") is text,
    so "#" is read as a note only in a file with lines wrapped in it ("#ah-ah-ah#") or opening
    with it and a space ("# Love is in the air") - a hashtag never has a space after it."""
    wrapped = opened = 0
    for line in lines:
        v = _visible(line).strip().lstrip("- ")
        if len(v) >= 2 and v[0] == "#" and v[-1] == "#":
            wrapped += 1
        elif v.startswith("# ") and len(v) > 2:
            opened += 1
        if wrapped >= 2 or opened >= 3:
            return True
    return False


_MARKUP = re.compile(r"(<[^>]*>|\{[^}]*\})")


def fix_notes(line: str, hash_notes: bool) -> str:
    """Inside a tag or a {label} a mark is left alone: "{Man ¶¶ 2}" is an OCR'd "#2"."""
    parts = _MARKUP.split(line)
    line = "".join(p if i % 2 else _PILCROWS.sub(NOTE, _EMOJI_NOTES.sub(NOTE, p))
                   for i, p in enumerate(parts))
    if hash_notes:
        line = _LEAD_HASH.sub(lambda m: m.group(1) + NOTE, line)
        line = _TRAIL_HASH.sub(lambda m: NOTE + m.group(1), line)
    return _PAIRED.sub(lambda m: f"{m.group(1)}{NOTE} {m.group(3)} {NOTE}{m.group(4)}", line)



# -- advertising ----------------------------------------------------------------------------

# anywhere in the file: nobody says these in a film
_AD_ANYWHERE = re.compile(
    r"opensubtitles|osdb\.link|addic7ed|subscene|podnapisi|titlovi|\byify\b|\byts\b|"
    r"explosiveskull|advertise your (?:product|brand)|become (?:a )?vip member|remove all ads|"
    r"please rate this subtitle|help other users to choose|danishbits|admit1\.app|admitme\.app|"
    r"4kvod\.tv|playships|iptv\.cat|released on www\.|kaboomskull|saveanilluminati|"
    r"^\W*subtitles?\s*:|"
    r"tv series and live sports|live tv, ?movies|la tv live|browser of future|"
    r"(?:use|free) (?:the )?(?:free )?code\b|code gratuit|\buse code:|discount on your next|"
    r"do you want subtitles for any video|watch any video online|watch more movies for free|"
    r"aidez les autres utilisateurs|professional translation services|"
    r"^\W*(?:translation|translated|vertaling|vertaald door|[öÖ]versatt av|traducere realizata|"
    r"improved by|ripped en bewerkt|sync(?:ed)?(?: ?& ?(?:corrections|edited))?|web[- ]?dl sync|"
    r"(?:bluray |dvd )?resync)\b\s*(?::|by\b|door\b|de\b)|"
    r"^\W*(?:subtitles?|subtitled|subs|subedit|captions?|captioning|transcript|dvd subtitles|"
    r"a subtitle)\b.*\bby\b|"
    r"^\W*(?:re-?sync(?:ed|hronized)?|sync(?:ed|hronized)?|ripped|encoded|corrected|fixed|"
    r"corrections|edited|translated|translation|timing|web[- ]?dl sync)\b.{0,30}\bby\b|"
    r"^\W*sous-titres? (?:par|by)\b",
    re.IGNORECASE | re.ASCII)
# only in the first or last few cues: a character may read out an address mid-film. A line is
# removed only for what it says itself, never for the line it shares a cue with: "Woohoo" and
# "WWW.CRAZY-TORRENT.COM" can be one cue, and "Woohoo" is the film
_AD_AT_EDGES = re.compile(r"https?://|www\.|[\w.+-]+@[\w-]+\.[\w.]+|^\W*downloaded from\b|"
                          r"find us on the web", re.IGNORECASE | re.ASCII)
EDGE_CUES = 3
_ONLY_TAGS = re.compile(r"^(?:\s|<[^>]*>|\{[^}]*\})*$", re.ASCII)


def _is_ad(line: str, at_edge: bool) -> bool:
    text = _visible(line)
    return bool(_AD_ANYWHERE.search(text) or (at_edge and _AD_AT_EDGES.search(text)))


# -- the pass -------------------------------------------------------------------------------

@dataclass
class Report:
    read_as: str = ""
    repaired_lines: int = 0
    notes_fixed: int = 0
    ad_lines: int = 0
    cues_dropped: int = 0
    ocr_fixes: int = 0
    timing_fixes: List[str] = field(default_factory=list)   # "overlap: old -> new", for the log
    removed: List[str] = field(default_factory=list)    # the ad lines, for the log
    corrected: List[str] = field(default_factory=list)  # "wouId -> would", for the log

    @property
    def changed(self) -> bool:
        if self.read_as == "unsure":
            return False
        return bool(not self.read_as.startswith("utf-") or self.repaired_lines
                    or self.notes_fixed or self.ad_lines or self.ocr_fixes
                    or self.timing_fixes)


ENGLISH = {"en", "eng", "english"}


def clean(data: bytes, language: Optional[str] = None, remove_ads: bool = True,
          ocr: Optional[Fixer] = None, fix_timing: bool = True) -> Tuple[str, Report]:
    """The repaired subtitle and what was done. OCR letters are put back only with a Fixer,
    and only in English: named so, or unnamed and reading like it."""
    report = Report()
    text, report.read_as = decode(data, language)
    if report.read_as == "unsure":
        return text, report
    # only text that was UTF-8 can carry UTF-8 mojibake; in a mixed file, its UTF-8 lines
    utf = report.read_as.startswith(("utf-", "mixed:"))
    blocks = parse(text)
    cue_indexes = [i for i, b in enumerate(blocks) if b.cue is not None]
    hash_notes = hash_is_note([line for b in blocks if b.cue for line in b.cue.lines])
    edges = set(cue_indexes[:EDGE_CUES] + cue_indexes[-EDGE_CUES:])
    if ocr is not None:
        english = (language.lower() in ENGLISH if language
                   else looks_english(line for b in blocks if b.cue for line in b.cue.lines))
        if not english:
            ocr = None

    def corrected(wrong: str, right: str) -> None:
        report.ocr_fixes += 1
        report.corrected.append(f"{wrong} -> {right}")

    kept = []
    for i, block in enumerate(blocks):
        cue = block.cue
        if cue is None:
            kept.append(block)
            continue
        repaired = [repair_chars(line, utf) for line in cue.lines]
        report.repaired_lines += sum(a != b for a, b in zip(cue.lines, repaired, strict=True))
        ads = [remove_ads and _is_ad(line, i in edges) for line in repaired]
        lines = []
        for fixed, ad in zip(repaired, ads, strict=True):
            if ad:
                report.ad_lines += 1
                report.removed.append(_visible(fixed).strip())
                continue
            noted = fix_notes(fixed, hash_notes)
            if noted != fixed:
                report.notes_fixed += 1
            if ocr is not None:
                noted = ocr.fix_line(noted, corrected)
            lines.append(noted)
        if cue.lines and all(_ONLY_TAGS.match(line) for line in lines):
            report.cues_dropped += 1
            continue
        cue.lines = lines
        kept.append(block)
    if fix_timing:
        report.timing_fixes = [f"{what}: {old} -> {new}"
                               for what, old, new in fix_timing_of([b.cue for b in kept if b.cue])]
    return serialize(kept, renumber=report.cues_dropped > 0), report
