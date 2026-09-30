#!/usr/bin/env python3
"""Build a subtitle-repair language pack: a language's Hunspell dictionaries and the profile the
OCR repair reads beside them.

    python3 make_pack.py --dicts DIR --out PACKDIR fr

DIR holds LibreOffice's <name>.aff/.dic files. The profile is plain text, one "key = value" a
line, so Kodi reads it without a parser:

    dictionaries      the Hunspell dictionaries, by file name
    one_letter_words  the words of one letter the language has (Hunspell takes any letter)
    lone_one          words a lone "1" may stand before when it is the word "I" (English)
    codes             how subtitle files and tracks name the language: ISO 639-1 and -2 codes
                      and the English name ("fr fre fra french")
    capitalised_nouns yes when every noun takes a capital (German): a capital is then no sign
                      of a name
    alphabet          the language's letters: ASCII and those its dictionary's TRY line lists
    accents           the accented letters an OCR may drop, by the letter it leaves: "e:éèê"
"""
import argparse
import re
import shutil
import unicodedata
from pathlib import Path

LANGUAGES = {
    "en": {"codes": "en eng english", "dics": ["en_US", "en_GB"], "one": "a A I",
           "lone": "am was know think don't can't didn't won't have had will would want need "
                   "mean love just guess thought said do did like can could should hope swear "
                   "promise told saw heard wish believe remember"},
    "fr": {"codes": "fr fre fra french", "dics": ["fr"], "one": "a A à À y Y ô Ô"},
    "es": {"codes": "es spa spanish", "dics": ["es_ES"], "one": "a A e E o O u U y Y"},
    "pt": {"codes": "pt por portuguese pob pt-br pt_br", "dics": ["pt_PT", "pt_BR"], "one": "a A à À e E é É o O"},
    "de": {"codes": "de ger deu german", "dics": ["de_DE_frami"], "one": "", "capitalised": True},
    "nl": {"codes": "nl dut nld dutch", "dics": ["nl_NL"], "one": "u U"},
    "it": {"codes": "it ita italian", "dics": ["it_IT"], "one": "a A e E è È i I o O"},
}


def alphabet(aff: Path) -> str:
    raw = aff.read_bytes()
    enc = re.search(rb"^SET\s+(\S+)", raw, re.M)
    text = raw.decode(enc.group(1).decode() if enc else "utf-8", errors="replace")
    letters = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
    m = re.search(r"^TRY\s+(\S+)", text, re.M)
    if m:
        letters |= {c for c in m.group(1) if c.isalpha()}
    return "".join(sorted(letters))


def accents(letters: str) -> str:
    by_base = {}
    for c in letters:
        base = unicodedata.normalize("NFD", c)[0]
        if base != c and base.isascii():
            by_base.setdefault(base, []).append(c)
    return " ".join(f"{b}:{''.join(sorted(v))}" for b, v in sorted(by_base.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("language", choices=sorted(LANGUAGES))
    ap.add_argument("--dicts", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()
    spec = LANGUAGES[args.language]
    args.out.mkdir(parents=True, exist_ok=True)
    letters = set()
    for name in spec["dics"]:
        for ext in ("aff", "dic"):
            shutil.copy(args.dicts / f"{name}.{ext}", args.out / f"{name}.{ext}")
        letters |= set(alphabet(args.dicts / f"{name}.aff"))
    letters_text = "".join(sorted(letters))
    lines = [
        f"# Subtitle repair: {args.language}",
        f"codes = {spec['codes']}",
        f"dictionaries = {' '.join(spec['dics'])}",
        f"one_letter_words = {spec['one']}",
        f"lone_one = {spec.get('lone', '')}",
        f"capitalised_nouns = {'yes' if spec.get('capitalised') else 'no'}",
        f"alphabet = {letters_text}",
        f"accents = {accents(letters_text)}",
    ]
    (args.out / "profile.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
