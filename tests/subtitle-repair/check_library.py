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
import ocr_engine as E  # noqa: E402
import reference as R  # noqa: E402

NOTE_SOURCES = set("#\u00b6Jj\u00cc\u00ec") | {"\U0001F3B5", "\U0001F3B6", "\U0001F3BC", "\uFE0F"}
CONFUSIONS: set = set()     # every (OCR wrote, was there) the installed packs know, set in main()
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
static std::vector<std::string> Fields(const std::string& row)
{
  std::vector<std::string> f(1);
  for (const char c : row)
    if (c == '\t') f.emplace_back(); else f.back().push_back(c);
  return f;
}
// argv[1]: the packs; argv[2]: rows of "file<TAB>path<TAB>out" or "cues<TAB>path<TAB>out<TAB>language"
int main(int, char** argv)
{
  const Packs packs = LoadPacks(argv[1]);
  std::ifstream list(argv[2]); std::string row;
  while (std::getline(list, row))
  {
    const std::vector<std::string> f = Fields(row);
    if (f[0] == "file")
    {
      std::string text; Report report;
      const bool cleaned = CleanSubRip(Slurp(f[1]), LanguageOfPath(f[1]), Options(), &packs,
                                       DetectLanguage, text, report);
      std::ofstream(f[2], std::ios::binary) << (cleaned ? text : "");
      std::cout << report.readAs << '\t' << report.Changed() << '\t' << LanguageOfPath(f[1])
                << '\t' << report.ocrFixes << '\t' << report.ocrLanguage << '\t'
                << report.ocrPattern << '\n';
      continue;
    }
    // cues, one after another, each ended by a NUL; a dropped cue comes out as "\x01"
    CCueCleaner cleaner(Options(), &packs, f.size() > 3 ? f[3] : "", DetectLanguage);
    const std::string all = Slurp(f[1]);
    std::ofstream out(f[2], std::ios::binary);
    for (size_t at = 0; at < all.size();)
    {
      const size_t end = all.find('\0', at);
      std::string text = all.substr(at, end - at);
      out << (cleaner.Clean(text) ? text : "\x01") << '\0';
      at = end + 1;
    }
    std::cout << "cues\n";
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


def ocr_edit(old, new):
    """Whether new undoes any OCR misreading of old (not only music notes)."""
    return any(op == "replace" and (old[a1:a2], new[b1:b2]) in CONFUSIONS
               or (op == "replace" and a2 - a1 == b2 - b1 and old[a1:a2] != new[b1:b2]
                   and not set(new[b1:b2]) <= {R.NOTE})
               for op, a1, a2, b1, b2 in
               difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes())


def _word_level(old, new):
    """Word by word: the same words and separators, each changed word one the confusions reach."""
    a, b = E.words_in(old), E.words_in(new)
    if len(a) != len(b):
        return False
    gaps_old = [old[x + n:y] for (x, n), (y, _) in zip(a, a[1:] + [(len(old), 0)], strict=False)]
    gaps_new = [new[x + n:y] for (x, n), (y, _) in zip(b, b[1:] + [(len(new), 0)], strict=False)]
    if old[:a[0][0] if a else len(old)] != new[:b[0][0] if b else len(new)] or gaps_old != gaps_new:
        return False
    for (x, n), (y, m) in zip(a, b, strict=True):
        w, v = old[x:x + n], new[y:y + m]
        if w != v and v not in E.candidates(w, sorted(CONFUSIONS)) and not (w == "1" and v == "I"):
            return False
    return True


def allowed_edit(old, new):
    """Whether new is old with only music-note glyphs changed and OCR misreadings undone."""
    if _word_level(old, new):
        return None
    for op, a1, a2, b1, b2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if op == "equal":
            continue
        src, dst = old[a1:a2], new[b1:b2]
        if dst and set(dst) == {R.NOTE} and src and set(src) <= NOTE_SOURCES:
            continue
        if op == "replace" and (src, dst) in CONFUSIONS:
            continue
        if op == "replace" and len(src) == len(dst) and all(x == y or (x, y) in CONFUSIONS for x, y in zip(src, dst, strict=True)):
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


def check(path, packs, detect):
    """(report, output, problems) for one file, from the reference."""
    data = Path(path).read_bytes()
    lang = R.language_of_path(path)
    out, report = R.clean(data, lang, True, packs, True, detect)
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
                if why is None and not report.ocr_pattern and ocr_edit(base, kept.lines[k]):
                    why = "a misreading undone in a file with no pattern of them"
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
    again, second = R.clean(out.encode("utf-8"), lang, True, packs, True, detect)
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


def compare_with_cpp(kodi, native, packs_dir, rows, work, streams, packs, detect):
    """Every file through SubtitleCleanup.cpp, whole and (with streams) cue by cue as a track in
    the video with and without a language; the paths where it differs from the reference."""
    source = work / "harness.cpp"
    source.write_text(HARNESS)
    binary = work / "harness"
    subprocess.run(["g++", "-std=c++17", "-O2", "-DHAS_HUNSPELL", "-DHAS_CLD2",
                    f"-I{kodi}/xbmc", f"-I{native}/include/hunspell", f"-I{native}/include/cld2",
                    "-o", str(binary), str(source),
                    f"{kodi}/xbmc/cores/VideoPlayer/DVDSubtitles/SubtitleCleanup.cpp",
                    f"-L{native}/lib", f"-Wl,-rpath,{native}/lib", "-lhunspell-1.7", "-lcld2_full"],
                   check=True)
    listing, expected_cues = [], {}
    for i, (path, _, _) in enumerate(rows):
        listing.append(f"file\t{path}\t{work}/{i}.out\n")
        if not streams:
            continue
        data = Path(path).read_bytes()
        text, read_as = R.decode(data, R.language_of_path(path))
        if read_as == "unsure":
            continue
        cues = ["\n".join(b.cue.lines) for b in R.parse(text) if b.cue]
        (work / f"{i}.cues").write_bytes(b"".join(c.encode("utf-8") + b"\0" for c in cues))
        for tag, language in (("tagged", R.language_of_path(path) or ""), ("untagged", "")):
            cleaner = R.CueCleaner(True, packs, language or None, detect)
            expected_cues[(i, tag)] = [cleaner.clean(c) for c in cues]
            listing.append(f"cues\t{work}/{i}.cues\t{work}/{i}.{tag}\t{language}\n")
    (work / "list.tsv").write_text("".join(listing))
    verdicts = [v for v in subprocess.run([str(binary), str(packs_dir), str(work / "list.tsv")],
                                          check=True, capture_output=True,
                                          text=True).stdout.splitlines() if v != "cues"]
    differ = []
    for i, ((path, report, out), verdict) in enumerate(zip(rows, verdicts, strict=True)):
        read_as, changed, lang, fixes, ocr_lang, pattern = (verdict.split("\t") + [""] * 6)[:6]
        cpp = (work / f"{i}.out").read_bytes().decode("utf-8")
        expected = "" if report.read_as == "unsure" else out
        if (cpp != expected or read_as != report.read_as or changed != str(int(report.changed))
                or lang != (R.language_of_path(path) or "") or fixes != str(report.ocr_fixes)
                or ocr_lang != report.ocr_language or pattern != " ".join(report.ocr_pattern)):
            differ.append(path)
            continue
        for tag in ("tagged", "untagged"):
            if (i, tag) not in expected_cues:
                continue
            got = (work / f"{i}.{tag}").read_bytes().split(b"\0")[:-1]
            got = [None if g == b"\x01" else g.decode("utf-8") for g in got]
            if got != expected_cues[(i, tag)]:
                differ.append(f"{path} (cue by cue, {tag})")
                break
    return differ


def cld2_detector(path):
    """CLD2 through a C shim exporting cld2_detect(text, length, code, &percent, &reliable)."""
    import ctypes
    lib = ctypes.CDLL(path)

    def detect(text):
        raw = text.encode("utf-8")
        code, percent, reliable = ctypes.create_string_buffer(16), ctypes.c_int(), ctypes.c_int()
        lib.cld2_detect(raw, len(raw), code, ctypes.byref(percent), ctypes.byref(reliable))
        return code.value.decode(), percent.value, bool(reliable.value)
    return detect


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", help="a file listing the .srt files, one per line")
    ap.add_argument("--packs", required=True, type=Path, help="a directory of language packs")
    ap.add_argument("--cld2", help="the CLD2 shim library, to detect untagged files' language")
    ap.add_argument("--kodi", type=Path, help="a tree prepare-tree.py made, to compare the C++")
    ap.add_argument("--native", type=Path, help="with --kodi: a prefix holding include/hunspell, "
                    "include/cld2 and lib/ with libhunspell-1.7 and libcld2_full")
    ap.add_argument("--streams", action="store_true",
                    help="with --kodi: also compare the cue-by-cue repair of video tracks")
    ap.add_argument("--residue", default="residue.json", help="where to write what still looks odd")
    args = ap.parse_args()
    packs = E.load_packs(args.packs)
    detect = cld2_detector(args.cld2) if args.cld2 else None
    CONFUSIONS.update(c for p in packs.values() for c in p.confusions)
    paths = [p for p in Path(args.paths).read_text().splitlines() if p.lower().endswith(".srt")]
    rows, failed, totals, residues = [], 0, collections.Counter(), {}
    for path in paths:
        report, out, problems = check(path, packs, detect)
        rows.append((path, report, out))
        totals["changed"] += report.changed
        totals["left alone"] += report.read_as == "unsure"
        totals["ocr files"] += bool(report.ocr_fixes)
        totals["ocr fixes"] += report.ocr_fixes
        if problems:
            failed += 1
            print(f"FAIL {path}")
            for problem in problems[:10]:
                print(f"    {problem}")
        found = residue(out)
        if found:
            residues[path] = found
    Path(args.residue).write_text(json.dumps(residues, ensure_ascii=False, indent=1))
    differ = []
    if args.kodi:
        with tempfile.TemporaryDirectory() as work:
            differ = compare_with_cpp(args.kodi, args.native, args.packs, rows, Path(work),
                                      args.streams, packs, detect)
        for path in differ[:20]:
            print(f"C++ DIFFERS {path}")
    print(f"{len(paths)} files, {totals['changed']} repaired, {totals['left alone']} left alone, "
          f"{totals['ocr fixes']} OCR fixes in {totals['ocr files']} files; "
          f"{failed} with an edit no rule explains"
          + (f", {len(differ)} where the C++ differs" if args.kodi else "")
          + f"; what still looks odd is in {args.residue}")
    return 1 if failed or differ else 0


if __name__ == "__main__":
    sys.exit(main())
