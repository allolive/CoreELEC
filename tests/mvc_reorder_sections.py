"""Compile the actual MVC timestamp reorder in a controlled picture harness.

The full ARM image separately compiles the real codec. This host suite extracts
the unchanged Reorder/DropLate bodies, and VideoPicture's reference handling,
at configure time; it does not run the decoder.
"""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]) / 'xbmc/cores/VideoPlayer/DVDCodecs/Video'
codec = (source / 'DVDVideoCodecAmlogic.cpp').read_text()
picture = (source / 'DVDVideoCodec.cpp').read_text()
header = (source / 'DVDVideoCodec.h').read_text()

def body(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

defines = [re.search(r'^#define %s\b.*$' % name, header, re.M).group(0)
           for name in ('DVP_FLAG_DROPPED', 'DVD_CODEC_CTRL_DRAIN')]
pieces = ['\n'.join(defines),
          body(picture, 'VideoPicture::~VideoPicture('),
          body(picture, 'VideoPicture& VideoPicture::CopyRef('),
          body(codec, 'CDVDVideoCodec::VCReturn CDVDVideoCodecAmlogic::Reorder('),
          body(codec, 'void CDVDVideoCodecAmlogic::DropLate(')]
Path(sys.argv[2]).write_text('\n\n'.join(pieces) + '\n')
