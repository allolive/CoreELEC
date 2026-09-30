#!/usr/bin/env python3
"""Run the subtitle repairs over a real library and prove, file by file, that they change only
what their rules allow - and that the C++ Kodi runs does exactly what the Python reference does.

    python3 tests/prepare-tree.py --out /tmp/kodi
    find /mnt/nas -iname '*.srt' > /tmp/srt.txt
    python3 tests/subtitle-repair/check_library.py \\
        --kodi /tmp/kodi /tmp/srt.txt

The library is not in the repository, so this is not a ctest suite. For every file it checks:
- the timing lines and their order are untouched, except an end time a timing rule moved, and
  only advertising cues were dropped;
- every kept line differs from the source only by an allowed edit: a music-note glyph or an
  OCR letter (I/l, 0/o) - damaged characters are repaired before the comparison;
- formatting tags are the same, in the same order;
- repairing the output again changes nothing;
- SubtitleCleanup.cpp, compiled from the tree, produces the same bytes.
It then writes what still looks odd after the repairs, for a person to read."""
import argparse
import collections
import difflib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import reference as R  # noqa: E402

NOTE_SOURCES = set("#\u00b6Jj\u00cc\u00ec") | {"\U0001F3B5", "\U0001F3B6", "\U0001F3BC", "\uFE0F"}
OCR_PAIRS = {("I", "l"), ("l", "I"), ("0", "o"), ("0", "O")}
TAG = re.compile(r"<[^>]*>|\{[^}]*\}")
TIME = re.compile(r"(\d+):(\d+):(\d+)[,.](\d+)\s*\S*?-->\s*(\d+):(\d+):(\d+)[,.](\d+)")
POSITION = re.compile(r"\\an(\d)")

HARNESS = r'''
#include "cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.h"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace KODI::SUBTITLES::CLEANUP;
static std::string Slurp(const std::string& p)
{ std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
int main(int, char** argv)
{
  CWordList words; words.Load(Slurp(argv[1]));
  std::ifstream list(argv[2]); std::string row;
  while (std::getline(list, row))
  {
    const size_t tab = row.find('\t');
    const std::string path = row.substr(0, tab), out = row.substr(tab + 1);
    std::string text; Report report;
    const bool cleaned = CleanSubRip(Slurp(path), LanguageOfPath(path), Options(), &words, text, report);
    std::ofstream(out, std::ios::binary) << (cleaned ? text : "") ;
    std::cout << report.readAs << '\t' << report.Changed() << '\t' << LanguageOfPath(path) << '\n';
  }
}
'''


def times(line):
    m = TIME.search(line)
    if not m:
        return None
    g = [int(x) for x in m.groups()]
    return (((g[0] * 60 + g[1]) * 60 + g[2]) * 1000 + int(m.group(4).ljust(3, "0")[:3]),
            ((g[4] * 60 + g[5]) * 60 + g[6]) * 1000 + int(m.group(8).ljust(3, "0")[:3]))


def _word_around(text, start, end):
    """The word the characters [start, end) of a text sit in."""
    while start > 0 and R.is_word_char(text[start - 1]):
        start -= 1
    while end < len(text) and R.is_word_char(text[end]):
        end += 1
    return text[start:end]


def allowed_edit(old, new):
    for op, a1, a2, b1, b2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if op == "equal":
            continue
        src, dst = old[a1:a2], new[b1:b2]
        # an OCR letter is only ever corrected in a plain English word: one changed inside a
        # word with an accented or non-Latin letter ("là" to "Ià") is an over-correction
        if set(src) | set(dst) <= {"I", "l", "0", "o", "O"} and not all(
                ord(c) < 0x80 or c == "\u2019" for c in _word_around(old, a1, a2)):
            return f"over-correction in {_word_around(old, a1, a2)!r}: {src!r} -> {dst!r}"
        if dst and set(dst) == {R.NOTE} and src and set(src) <= NOTE_SOURCES:
            continue
        if op == "replace" and len(src) == len(dst) and all(p in OCR_PAIRS for p in zip(src, dst, strict=True)):
            continue
        return f"{op} {src!r} -> {dst!r}"
    return None


def timing_problems(src, dst):
    problems = []
    pos = [(POSITION.search(" ".join(c.lines)) or [None, "2"])[1] for c in dst]
    for i, (a, b) in enumerate(zip(src, dst, strict=True)):
        if a.timing == b.timing:
            continue
        ta, tb = times(a.timing), times(b.timing)
        if ta is None or tb is None or ta[0] != tb[0]:
            problems.append(f"timing rewritten: {a.timing!r} -> {b.timing!r}")
            continue
        if ta == tb:
            continue                                # only the arrow was cleaned
        nxt = next((times(c.timing) for c in dst[i + 1:] if times(c.timing)), None)
        next_start = nxt[0] if nxt and nxt[0] > ta[0] else None
        start, old_end, new_end = ta[0], ta[1], tb[1]
        short = (old_end - start < R.MIN_SHOWN and old_end < new_end <= start + R.READABLE
                 and (next_start is None or new_end <= next_start))
        overlap = (next_start is not None and new_end == next_start and pos[i] == pos[i + 1]
                   and R.MIN_OVERLAP <= old_end - next_start <= R.MAX_SLOP)
        if not (short or overlap):
            problems.append(f"end moved without a rule: {a.timing!r} -> {b.timing!r}")
    return problems


def check(path, fixer):
    """(report, output, problems) for one file, from the reference."""
    data = Path(path).read_bytes()
    lang = R.language_of_path(path)
    out, report = R.clean(data, lang, True, fixer)
    if report.read_as == "unsure":
        return report, out, []
    utf = report.read_as.startswith(("utf-", "mixed:"))
    text, _ = R.decode(data, lang)
    src = [b.cue for b in R.parse(text) if b.cue]
    dst = [b.cue for b in R.parse(out) if b.cue]
    problems = []
    removed = collections.Counter(report.removed)
    kept_src, j = [], 0
    for cue in src:
        same_start = j < len(dst) and (times(dst[j].timing) or (None,))[0] == (times(cue.timing) or (None,))[0]
        if not same_start:                         # dropped: every line of it must be an ad
            for line in cue.lines:
                visible = TAG.sub("", R.repair_chars(line, utf)).strip()
                if visible and removed[visible]:
                    removed[visible] -= 1
                elif visible:
                    problems.append(f"{cue.timing}: dropped a line that is not an ad: {line!r}")
            continue
        kept, k = dst[j], 0
        kept_src.append(cue)
        j += 1
        for line in cue.lines:
            base = R.repair_chars(line, utf)
            if k < len(kept.lines):
                why = allowed_edit(base, kept.lines[k])
                if why is None and TAG.findall(base) == TAG.findall(kept.lines[k]):
                    k += 1
                    continue
            visible = TAG.sub("", base).strip()
            if removed[visible]:
                removed[visible] -= 1
                continue
            problems.append(f"{cue.timing}: {line!r} -> "
                            f"{kept.lines[k] if k < len(kept.lines) else None!r}")
            k += 1
    if len(kept_src) != len(dst):
        problems.append(f"{len(kept_src)} source cues matched against {len(dst)}")
    else:
        problems += timing_problems(kept_src, dst)
    again, second = R.clean(out.encode("utf-8"), lang, True, fixer)
    if again != out:
        problems.append(f"repairing again changes more: {second}")
    return report, out, problems


RESIDUE = {
    "hash_or_pilcrow": re.compile(r"[#\u00b6]"),
    "address_or_credit": re.compile(r"www\.|https?://|@[\w-]+\.\w|\.(?:com|org|net)\b|"
                                    r"\b(?:sync|subtitle|caption|rip|encod|translat)\w*\b.{0,20}\bby\b",
                                    re.I),
    "mojibake_looking": re.compile("[\u00c2\u00c3\u00e2][\u0080-\u00bf\u2018-\u203a\u20ac\u0152-\u0178]"),
    "invisible_or_control": re.compile("[\u0000-\u0008\u000b-\u001f\u007f-\u009f\u200b\u2060\ufeff]"),
    "replacement_char": re.compile("\ufffd"),
    "html_entity": re.compile(r"&(?:#\d+|#x[0-9a-f]+|[a-z]+);", re.I),
}


def residue(out):
    found = collections.defaultdict(list)
    prev_end = -1
    for block in R.parse(out):
        cue = block.cue
        if cue is None:
            found["not_a_cue"].append(block.raw[:80])
            continue
        t = times(cue.timing)
        if t is None:
            found["bad_timing"].append(cue.timing)
        else:
            if t[1] <= t[0]:
                found["end_before_start"].append(cue.timing)
            if t[0] < prev_end - 1000:
                found["starts_a_second_before_the_previous_ends"].append(cue.timing)
            prev_end = t[1]
        for line in cue.lines:
            for name, rx in RESIDUE.items():
                if rx.search(TAG.sub("", line) if name == "hash_or_pilcrow" else line):
                    found[name].append(line[:100])
    return found


def compare_with_cpp(kodi, words, rows, work):
    """Every file through SubtitleCleanup.cpp; the paths whose output or verdict differs."""
    source = work / "harness.cpp"
    source.write_text(HARNESS)
    binary = work / "harness"
    subprocess.run(["g++", "-std=c++17", "-O2", f"-I{kodi}/xbmc", "-o", str(binary), str(source),
                    f"{kodi}/xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.cpp"], check=True)
    listing = work / "list.tsv"
    listing.write_text("".join(f"{path}\t{work}/{i}.out\n" for i, (path, _, _) in enumerate(rows)))
    verdicts = subprocess.run([str(binary), str(words), str(listing)], check=True,
                              capture_output=True, text=True).stdout.splitlines()
    differ = []
    for i, ((path, report, out), verdict) in enumerate(zip(rows, verdicts, strict=True)):
        read_as, changed, lang = (verdict.split("\t") + [""])[:3]
        cpp = (work / f"{i}.out").read_bytes().decode("utf-8")
        expected = "" if report.read_as == "unsure" else out
        if (cpp != expected or read_as != report.read_as or changed != str(int(report.changed))
                or lang != (R.language_of_path(path) or "")):
            differ.append(path)
    return differ


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", help="a file listing the .srt files, one per line")
    ap.add_argument("--kodi", required=True, type=Path, help="a tree prepare-tree.py made")
    ap.add_argument("--residue", default="residue.json", help="where to write what still looks odd")
    args = ap.parse_args()
    words = args.kodi / "system/subtitlecleanup/en_words.txt"
    fixer = R.Fixer(R.load_words(str(words)))
    paths = [p for p in Path(args.paths).read_text().splitlines() if p.lower().endswith(".srt")]
    rows, failed, totals, residues = [], 0, collections.Counter(), {}
    for path in paths:
        report, out, problems = check(path, fixer)
        rows.append((path, report, out))
        totals["changed"] += report.changed
        totals["left alone"] += report.read_as == "unsure"
        if problems:
            failed += 1
            print(f"FAIL {path}")
            for problem in problems[:10]:
                print(f"    {problem}")
        found = residue(out)
        if found:
            residues[path] = found
    Path(args.residue).write_text(json.dumps(residues, ensure_ascii=False, indent=1))
    with tempfile.TemporaryDirectory() as work:
        differ = compare_with_cpp(args.kodi, words, rows, Path(work))
    for path in differ[:20]:
        print(f"C++ DIFFERS {path}")
    print(f"{len(paths)} files, {totals['changed']} repaired, {totals['left alone']} left alone; "
          f"{failed} with an edit no rule explains, {len(differ)} where the C++ differs; "
          f"what still looks odd is in {args.residue}")
    return 1 if failed or differ else 0


if __name__ == "__main__":
    sys.exit(main())
