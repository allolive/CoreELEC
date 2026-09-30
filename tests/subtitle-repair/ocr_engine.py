"""OCR repair by the file's own error pattern - the reference the C++ in Kodi is checked against.

A subtitle made by OCR from disc images repeats the same misreadings hundreds of times - one
font, one engine - and a clean one has none. So a file (or a track, cue by cue) first learns
which misreadings it has, and only those are undone:

1. every word that is not a word of the subtitle's language is tried against each confusion an
   OCR makes ("l" for "I", "cl" for "d", "rn" for "m", "0" for "O", a dropped accent...); a
   confusion that alone turns it into one word, and only one, is evidence. Only lines in the
   subtitle's language count;
2. a confusion is the file's when it is frequent: a real share of how the file writes that
   letter, or across many different words. An exception among correct uses is left;
3. a word is then corrected when the file's confusions turn it into exactly one word, and its
   line is in the subtitle's language.

Left as they are: names the file capitalises mid-sentence, lines another language explains
better, an accent on a word another language has, numbers, Roman numerals.

Everything that depends on a character is decided by the explicit tables below, not by Python's
Unicode methods, so the C++ can reproduce it exactly. Hunspell is reached through ctypes; set
HUNSPELL_LIB if it is not found."""
import ctypes
import ctypes.util
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple

MIN_WORDS = 3           # different words a confusion must explain to be the file's
MIN_TIMES = 5           # and how often in all
MANY_TIMES = 20         # or one misreading this often ("l" for "I", 384 times in one film)
MIN_SHARE = 0.2         # of the file's uses of the right letter that come out wrong,
MANY_WORDS = 10         # unless the misreading recurs across this many different words
LANGUAGE_SHARE = 0.6    # of a line's other words, for the line to count as the language
OTHER_MARGIN = 0.2      # how much better another language must explain a line to claim it
MAX_CANDIDATES = 256

# (what the OCR wrote, what was there); a pack adds the accents its language drops
CONFUSIONS: List[Tuple[str, str]] = [
    ("l", "I"), ("I", "l"), ("1", "l"), ("1", "I"), ("|", "l"), ("|", "I"),
    ("0", "o"), ("0", "O"), ("cl", "d"), ("rn", "m"), ("vv", "w"), ("li", "h"),
    ("m", "rn"), ("d", "cl"),
]

# -- characters ---------------------------------------------------------------------------------

_SPACES = {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x85, 0xA0, 0x1680,
           0x2028, 0x2029, 0x202F, 0x205F, 0x3000} | set(range(0x2000, 0x200B))


def _case_pairs() -> Dict[str, str]:
    """Upper to lower, for ASCII, Latin-1, Latin Extended-A/B, Greek and Cyrillic: the letters a
    subtitle language uses. Written out as a table for the C++."""
    pairs = {}
    for lo, hi in ((0x41, 0x5A), (0xC0, 0x24F), (0x370, 0x3FF), (0x400, 0x4FF)):
        for cp in range(lo, hi + 1):
            u = chr(cp)
            low = u.lower()
            if len(low) == 1 and low != u and low.upper() == u:
                pairs[u] = low
    return pairs


UPPER_TO_LOWER = _case_pairs()
LOWERS = set(UPPER_TO_LOWER.values())


def is_space(c: str) -> bool:
    return ord(c) in _SPACES


def is_upper(c: str) -> bool:
    return c in UPPER_TO_LOWER


def is_lower(c: str) -> bool:
    return c in LOWERS


def lower(c: str) -> str:
    return UPPER_TO_LOWER.get(c, c)


def is_digit(c: str) -> bool:
    return "0" <= c <= "9"


def is_word_char(c: str) -> bool:
    """ASCII letters and digits, the "|" an OCR puts for "l" or "I", and any character beyond
    ASCII that is not white space, punctuation or a symbol ("là" is one word)."""
    cp = ord(c)
    if cp < 0x80:
        return c.isalnum() or c == "|"
    return not (is_space(c) or cp <= 0xBF or cp in (0xD7, 0xF7) or 0x2000 <= cp <= 0x2BFF
                or 0x3000 <= cp <= 0x303F or 0xFE10 <= cp <= 0xFE6F or 0xFF00 <= cp <= 0xFF0F
                or 0x1F000 <= cp <= 0x1FAFF or cp == 0xFEFF)


def is_letter(c: str) -> bool:
    return is_word_char(c) and not is_digit(c) and c != "|"


def all_upper(w: str) -> bool:
    """Every cased letter a capital, and at least one."""
    return any(is_upper(c) for c in w) and not any(is_lower(c) for c in w)


def all_lower(w: str) -> bool:
    return any(is_lower(c) for c in w) and not any(is_upper(c) for c in w)


def words_in(text: str) -> List[Tuple[int, int]]:
    """Words as (start, length): word characters, joined by an apostrophe followed by more."""
    words, i = [], 0
    while i < len(text):
        if not is_word_char(text[i]):
            i += 1
            continue
        start = i
        while i < len(text) and is_word_char(text[i]):
            i += 1
        while i + 1 < len(text) and text[i] in "'’" and is_word_char(text[i + 1]):
            i += 1
            while i < len(text) and is_word_char(text[i]):
                i += 1
        words.append((start, i - start))
    return words


def split_markup(line: str) -> List[str]:
    """Text and markup (<tag>, {label}) alternating, text first."""
    parts, text_start, i = [], 0, 0
    while i < len(line):
        close = {"<": ">", "{": "}"}.get(line[i])
        if close:
            end = line.find(close, i + 1)
            if end != -1:
                parts += [line[text_start:i], line[i:end + 1]]
                i = text_start = end + 1
                continue
        i += 1
    return parts + [line[text_start:]]


def visible(line: str) -> str:
    return "".join(split_markup(line)[::2])


def plausible(w: str) -> bool:
    """A capital after a small letter, or a digit among letters, is no word's spelling; and a
    Roman numeral is never what a misread word was ("Il Mondo" is not "II Mondo")."""
    if any(is_lower(a) and is_upper(b) for a, b in zip(w, w[1:], strict=False)):
        return False
    if len(w) > 1 and all(c in "IVXLCDM" for c in w):
        return False
    return not (any(is_digit(c) for c in w) and any(is_letter(c) for c in w))


def alone(text: str, start: int, length: int) -> bool:
    """A one-letter word stands on its own: joined by a hyphen to a letter it is a stutter or a
    word spelt out ("l-l-low", "l-T-H"); inside punctuation, a damaged word ("Sa?l"). A dash
    that ends there ("l-- I'm sorry") breaks off a word."""
    before = text[start - 1] if start else ""
    if before == "-":
        before_ok = start < 2 or is_space(text[start - 2])
    else:
        before_ok = before == "" or is_space(before) or before in "\"'([“‘¿¡*.…"
    rest = text[start + length:]
    if rest[:1] == "-":
        dashes = len(rest) - len(rest.lstrip("-"))
        return before_ok and (dashes == len(rest) or is_space(rest[dashes]))
    return before_ok and (rest == "" or is_space(rest[0]) or rest[0] in ".,!?;:)]\"”’…*")


# -- the language: a pack ------------------------------------------------------------------------

class Hunspell:
    _lib = None

    @classmethod
    def lib(cls):
        if cls._lib is None:
            path = os.environ.get("HUNSPELL_LIB") or ctypes.util.find_library("hunspell-1.7")
            cls._lib = ctypes.CDLL(path)
            cls._lib.Hunspell_create.restype = ctypes.c_void_p
            cls._lib.Hunspell_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            cls._lib.Hunspell_spell.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            cls._lib.Hunspell_get_dic_encoding.restype = ctypes.c_char_p
            cls._lib.Hunspell_get_dic_encoding.argtypes = [ctypes.c_void_p]
        return cls._lib

    def __init__(self, aff: Path, dic: Path):
        self.handle = self.lib().Hunspell_create(str(aff).encode(), str(dic).encode())
        self.encoding = self.lib().Hunspell_get_dic_encoding(self.handle).decode()
        self.cache: Dict[str, bool] = {}

    def spell(self, word: str) -> bool:
        if word not in self.cache:
            try:
                raw = word.replace("’", "'").encode(self.encoding)
            except (UnicodeEncodeError, LookupError):
                self.cache[word] = False
            else:
                self.cache[word] = bool(self.lib().Hunspell_spell(self.handle, raw))
        return self.cache[word]


class Language:
    """A pack: <dir>/profile.txt and the Hunspell dictionaries it names."""

    def __init__(self, pack: Path):
        pack = Path(pack)
        profile = {}
        for raw in (pack / "profile.txt").read_text(encoding="utf-8").splitlines():
            if "=" in raw and not raw.lstrip().startswith("#"):
                key, _, value = raw.partition("=")
                profile[key.strip()] = value.strip()
        self.code = pack.name
        self.codes = set(profile.get("codes", pack.name).lower().split())
        self.dicts = [Hunspell(pack / f"{d}.aff", pack / f"{d}.dic")
                      for d in profile.get("dictionaries", "").split()]
        self.one = set(profile.get("one_letter_words", "").split())
        self.lone = set(profile.get("lone_one", "").split())
        self.capitalised_nouns = profile.get("capitalised_nouns") == "yes"
        self.alphabet = set(profile.get("alphabet", ""))
        self.confusions = list(CONFUSIONS)
        for group in profile.get("accents", "").split():
            base, _, marked = group.partition(":")
            self.confusions += [(base, c) for c in marked]

    def word(self, w: str) -> bool:
        if len(w) == 1:
            return w in self.one
        return any(d.spell(w) for d in self.dicts)

    def readable(self, w: str) -> bool:
        """Written in this language's letters (or characters an OCR confuses them with)."""
        return all(c in self.alphabet or c in "0123456789|'’-" for c in w)


def load_packs(root: Path) -> Dict[str, Language]:
    return {p.name: Language(p) for p in sorted(Path(root).iterdir()) if (p / "profile.txt").is_file()}


def pack_for(code: Optional[str], packs: Dict[str, Language]) -> Optional[Language]:
    """The pack a subtitle's language code names ("fre", "French", "pt-BR"), if installed."""
    code = (code or "").lower()
    return next((p for p in packs.values() if code in p.codes), None)


# -- readings ------------------------------------------------------------------------------------

def _variants(w: str, confusion: Tuple[str, str]) -> List[str]:
    """w with the confusion undone at some of the places it could apply: every choice of them,
    fewest first, or all at once when there are more than four."""
    wrong, right = confusion
    spots, i = [], w.find(wrong)
    while i != -1:
        spots.append(i)
        i = w.find(wrong, i + len(wrong))
    if not spots:
        return []
    if len(spots) <= 4:
        choices = [[s for k, s in enumerate(spots) if mask >> k & 1] for mask in range(1, 1 << len(spots))]
        choices.sort(key=lambda c: (len(c), c))
    else:
        choices = [spots]
    out = []
    for chosen in choices:
        parts, last = [], 0
        for s in chosen:
            parts.append(w[last:s] + right)
            last = s + len(wrong)
        out.append("".join(parts) + w[last:])
    return out


def candidates(w: str, confusions: List[Tuple[str, str]]) -> List[str]:
    """What w could have been through the given confusions, in any mix, in a fixed order."""
    found, order = {w}, [w]
    for confusion in confusions:
        for base in list(order):
            for v in _variants(base, confusion):
                if v not in found and len(order) < MAX_CANDIDATES:
                    found.add(v)
                    order.append(v)
    return order[1:]


# -- the pattern ---------------------------------------------------------------------------------

@dataclass
class Pattern:
    """What a file's OCR confused, learnt from the file: for each confusion, the words it
    explains, and how often the file writes the right letter correctly."""
    evidence: Dict[Tuple[str, str], Dict[str, int]] = field(default_factory=dict)
    correct: Dict[Tuple[str, str], int] = field(default_factory=dict)

    def share(self, confusion: Tuple[str, str]) -> float:
        wrong = sum(self.evidence.get(confusion, {}).values())
        return wrong / (wrong + self.correct.get(confusion, 0)) if wrong else 0.0

    def active(self, confusions: List[Tuple[str, str]]) -> List[Tuple[str, str]]:
        """A confusion is the file's when it repeats and is how the file writes that letter, not
        an exception among correct ones. In the language's order."""
        out = []
        for c in confusions:
            words = self.evidence.get(c)
            if not words:
                continue
            times = sum(words.values())
            if (((len(words) >= MIN_WORDS and times >= MIN_TIMES) or times >= MANY_TIMES)
                    and (self.share(c) >= MIN_SHARE or len(words) >= MANY_WORDS)):
                out.append(c)
        return out


class Engine:
    def __init__(self, language: Language, others: Iterable[Language] = ()):
        self.lang = language
        self.others = [o for o in others if o.code != language.code]
        self.names: Set[str] = set()
        self.pattern = Pattern()

    # -- what the file says about itself

    def note_names(self, lines: Iterable[str]) -> None:
        """Words the file capitalises in the middle of a sentence - names ("Salut, Elise."),
        which keep their spelling wherever they appear. Not in a language that capitalises its
        nouns, and not a word starting with a letter an OCR confuses: "Ia" for "la" is
        capitalised by the misreading."""
        if self.lang.capitalised_nouns:
            return
        for line in lines:
            text = visible(line)
            for start, n in words_in(text):
                w = text[start:start + n]
                before = text[:start].rstrip()
                if (is_upper(w[0]) and w[0] not in "Il|1" and all_lower(w[1:]) and plausible(w)
                        and before and before[-1] not in ".!?…:-\"'¿¡“«([*"):
                    self.names.add(w)

    def claimed_by_other(self, words: List[str]) -> bool:
        """Another language explains these words clearly better ("de Big Brother & The
        Holding" is English, "de" or not)."""
        own = sum(self.lang.word(w) for w in words)
        return any(sum(o.word(w) for w in words) > own + OTHER_MARGIN * len(words)
                   for o in self.others)

    def learn(self, lines: Iterable[str]) -> None:
        for line in lines:
            text = visible(line)
            spans = words_in(text)
            if self.claimed_by_other([text[a:a + n] for a, n in spans if n >= 2 and not is_digit(text[a])]):
                continue    # another language's line: not evidence of this file's misreadings
            for start, n in spans:
                w = text[start:start + n]
                if self.lang.word(w):
                    for confusion in self.lang.confusions:
                        if confusion[1] in w:
                            self.pattern.correct[confusion] = self.pattern.correct.get(confusion, 0) + 1
                    continue
                if not self.lang.readable(w) or not any(is_letter(c) for c in w):
                    continue    # another script, or a number ("Episode 1" is not "I")
                if n == 1 and not alone(text, start, n):
                    continue
                readings = {}
                for confusion in self.lang.confusions:
                    for v in _variants(w, confusion):
                        if self.acceptable(w, v):
                            readings[v] = confusion
                if len(readings) == 1:
                    (confusion,) = readings.values()
                    words = self.pattern.evidence.setdefault(confusion, {})
                    words[w] = words.get(w, 0) + 1

    def active(self) -> List[Tuple[str, str]]:
        return self.pattern.active(self.lang.confusions)

    # -- correcting

    def acceptable(self, w: str, reading: str) -> bool:
        """A reading is a word of the language, spelt as words are, and not a name the file
        uses. An accent is not put on a word another language has ("THE" in a French subtitle's
        English sign is not "THÉ"). A capitalised word that shows no misreading may only become
        an ordinary word ("Ecoutez" is "Écoutez", "Moclan" is no "Modan"; "FeIipe" shows its
        misreading and is "Felipe") - unless every noun is capitalised."""
        if not plausible(reading) or not self.lang.word(reading) or w in self.names:
            return False
        if any(ord(c) >= 0x80 and c not in w for c in reading) and any(o.word(w) for o in self.others):
            return False
        if (not self.lang.capitalised_nouns and is_upper(w[0]) and plausible(w)
                and not all_upper(reading)):
            return self.lang.word(lower(reading[0]) + reading[1:])
        return True

    def fix_word(self, w: str, active: List[Tuple[str, str]]) -> Optional[str]:
        if self.lang.word(w) or not self.lang.readable(w) or not any(is_letter(c) or c == "|" for c in w):
            return None
        readings = [c for c in candidates(w, active) if self.acceptable(w, c)]
        return readings[0] if len(readings) == 1 else None

    def lone_one(self, text: str, start: int, length: int, active: List[Tuple[str, str]]) -> bool:
        """A lone "1" is "I" only in a file whose OCR writes "1" for "I", standing alone, before
        a word "I" goes with ("1 know"): "Episode 1", "1 day" stay numbers."""
        if text[start:start + length] != "1" or ("1", "I") not in active or not alone(text, start, length):
            return False
        rest = text[start + length:].lstrip()
        following = words_in(rest)
        return bool(following) and following[0][0] == 0 and rest[:following[0][1]] in self.lang.lone

    def reads_as_language(self, text: str, skip: Set[int]) -> bool:
        """Whether a line's other words are mostly the language's. Names are not weighed - a word
        starting with a capital A-Z - except in a line all in capitals ("NEMO, LIS THE NEWPORT
        NEWS"). And not a line another language explains clearly better."""
        shouting = not any(is_lower(c) for c in text)
        words = [text[a:a + n] for k, (a, n) in enumerate(words_in(text))
                 if k not in skip and n >= 2 and not is_digit(text[a])]
        others = [w for w in words if shouting or not "A" <= w[0] <= "Z"]
        if sum(self.lang.word(w) for w in others) < LANGUAGE_SHARE * len(others):
            return False
        return not self.claimed_by_other(words)

    def _fix(self, text: str, start: int, n: int, active) -> Optional[str]:
        w = text[start:start + n]
        if self.lone_one(text, start, n, active):
            return "I"
        fixed = self.fix_word(w, active)
        return fixed if fixed is not None and (n > 1 or alone(text, start, n)) else None

    def fix_line(self, line: str, fixes: Optional[list] = None) -> str:
        active = self.active()
        if not active:
            return line
        text = visible(line)
        pending = {k for k, (a, n) in enumerate(words_in(text)) if self._fix(text, a, n, active) is not None}
        if not pending or not self.reads_as_language(text, pending):
            return line
        parts = split_markup(line)
        for i in range(0, len(parts), 2):
            part, out, last = parts[i], [], 0
            for a, n in words_in(part):
                fixed = self._fix(part, a, n, active)
                out.append(part[last:a] + (fixed if fixed is not None else part[a:a + n]))
                if fixed is not None and fixes is not None:
                    fixes.append((part[a:a + n], fixed))
                last = a + n
            parts[i] = "".join(out) + part[last:]
        return "".join(parts)


def repair_file(lines: List[str], language: Language, others: Iterable[Language],
                fixes: Optional[list] = None) -> Tuple[List[str], List[Tuple[str, str]]]:
    """A whole file: learn from all of it, then correct it. Returns the lines and the pattern."""
    engine = Engine(language, others)
    engine.note_names(lines)
    engine.learn(lines)
    return [engine.fix_line(line_, fixes) for line_ in lines], engine.active()


class Stream:
    """A track inside the video, cue by cue: each cue first adds to what is known of the track -
    its names, its misreadings - and is then corrected with what is known so far."""

    def __init__(self, language: Language, others: Iterable[Language]):
        self.engine = Engine(language, others)

    def cue(self, lines: List[str], fixes: Optional[list] = None) -> List[str]:
        self.engine.note_names(lines)
        self.engine.learn(lines)
        return [self.engine.fix_line(line_, fixes) for line_ in lines]
