"""Compile the actual legacy adapter, HDR detection and AddStream pairing branch.

The host harness controls stream descriptions and inspects mutated demux state.
It does not execute FFmpeg I/O. Target compilation checks the real declarations.
"""
from pathlib import Path
import sys

source = (Path(sys.argv[1]) / 'xbmc/cores/VideoPlayer/DVDDemuxers/DVDDemuxFFmpeg.cpp').read_text()


def body(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


# Also accepts the previous adapter so the regression can run against that tree.
adapter = source.index(' LegacyDovi')
adapter = source.rfind('\n', 0, adapter) + 1
pieces = [body(source[adapter:source.index('\n', adapter)]),
          body('StreamHdrType CDVDDemuxFFmpeg::DetermineHdrType(')]
start = source.index('        st->hdr_type = DetermineHdrType(pStream);')
end = source.index('        sideData = av_packet_side_data_get(pStream->codecpar->coded_side_data,\n'
                   '                                           pStream->codecpar->nb_coded_side_data,\n'
                   '                                           AV_PKT_DATA_MASTERING_DISPLAY_METADATA);', start)
pieces.append('''CDemuxStream* CDVDDemuxFFmpeg::AddPairingStream(int streamIdx)
{
  auto* pStream = m_pFormatContext->streams[streamIdx];
  auto* st = new CDemuxStreamVideo;
  st->uniqueId = streamIdx;
  CDemuxStream* stream = st;
''' + source[start:end] + '''
  m_streams.emplace(streamIdx, stream);
  return stream;
}
''')
Path(sys.argv[2]).write_text('\n\n'.join(pieces))
