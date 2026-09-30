"""Compile the actual player handoff/flush wiring in a controlled collaborator harness.

The full ARM image separately compiles the real class declarations. This host
suite extracts unchanged bodies at configure time, never a hand-maintained copy
of the transition logic. It does not execute demux, renderer or audio device I/O.
"""
from pathlib import Path
import sys

source = Path(sys.argv[1]) / 'xbmc/cores/VideoPlayer'
player = (source / 'VideoPlayer.cpp').read_text()
audio = (source / 'VideoPlayerAudio.cpp').read_text()

def body(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

pieces = [body(player, 'void CVideoPlayer::CancelResumePreroll('),
          body(player, 'void CVideoPlayer::HandleAudioPrerollStatus('),
          body(player, 'bool CVideoPlayer::PollResumePreroll(')]
flush = body(player, 'void CVideoPlayer::FlushBuffers(')
pieces.append(flush[:flush.index('  double startpts;')] + '  (void)sync;\n}')
commands = body(audio, '    else if (pMsg->IsType(CDVDMsg::AUDIO_PREROLL))')
commands = commands[commands.index('      if (command.action'):commands.rfind('}')]
pieces.append('void AudioHarness::Handle(const AudioPrerollCommand& command)\n{\n' + commands + '}')
output = body(audio, 'bool CVideoPlayerAudio::ProcessDecoderOutput(')
guard = output[output.index('  if (m_preroll.Active()'):output.index('return false;')+len('return false;')]
pieces.append('bool AudioHarness::CanProcessOutput()\n{\n' + guard + '\n  return true;\n}')
Path(sys.argv[2]).write_text('\n\n'.join(pieces)+'\n')
