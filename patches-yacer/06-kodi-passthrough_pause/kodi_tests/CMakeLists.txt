# Tests for the passthrough pause/resume patches in this group.

yacer_add_test(
  NAME PlaybackPreroll
  SOURCES TestPlaybackPreroll.cpp
)

# The fence is header-only, so this needs no production translation units and
# no stand-ins: gmock supplies the work a ticket runs.
yacer_add_test(
  NAME PlaybackCallbackFence
  SOURCES TestPlaybackCallbackFence.cpp
)

# The ordering this pins is the message queue's own - a barrier must not
# overtake a seek the callback already queued - so the real queue and the real
# event and mutex primitives are compiled in rather than stood in for.
yacer_add_test(
  NAME PlaybackResumeGate
  STANDARD 20
  SOURCES TestPlaybackResumeGate.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDMessage.cpp
               xbmc/cores/VideoPlayer/DVDMessageQueue.cpp
               xbmc/threads/Event.cpp
               xbmc/platform/posix/threads/RecursiveMutex.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/platform/posix
           ${KODI_SOURCE}/xbmc/cores/VideoPlayer
)

# The real packer: a reset test that compared against an imitation of MAT
# framing would prove nothing about the framing the box emits.
yacer_add_test(
  NAME MATReset
  SOURCES TestMATReset.cpp
  KODI_SOURCES xbmc/cores/AudioEngine/Utils/PackerMAT.cpp
               xbmc/utils/BitstreamReader.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/platform/posix
)

# Both production packers, so the bytes checked here are the bytes the sink
# would hand the HDMI carrier.
yacer_add_test(
  NAME IECPause
  SOURCES TestIECPause.cpp
  KODI_SOURCES xbmc/cores/AudioEngine/Utils/AEBitstreamPacker.cpp
               xbmc/cores/AudioEngine/Utils/AEPackIEC61937.cpp
               xbmc/cores/AudioEngine/Utils/AEChannelInfo.cpp
               xbmc/cores/AudioEngine/Utils/PackerMAT.cpp
               xbmc/utils/BitstreamReader.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/platform/posix
)

# The decorator and Kodi's real callback queue, so the paths that drop a call
# are the paths the box has rather than an imitation of them.
yacer_add_test(
  NAME PlaybackCallbackDecorator
  STANDARD 20
  SOURCES TestPlaybackCallbackDecorator.cpp
  KODI_SOURCES xbmc/interfaces/legacy/CallbackFunction.cpp
               xbmc/interfaces/legacy/CallbackHandler.cpp
               xbmc/interfaces/legacy/AddonClass.cpp
               xbmc/interfaces/legacy/LanguageHook.cpp
               xbmc/interfaces/legacy/String.cpp
               xbmc/platform/posix/threads/RecursiveMutex.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/platform/posix
)

# Preroll state is header-only; the sink and player sides are covered by their
# own suites.
yacer_add_test(
  NAME AudioPreroll
  SOURCES TestAudioPreroll.cpp
  INCLUDES ${YACER_SHIMS}
)



# The real sink. gmock supplies the audio engine interfaces, which are pure
# virtual; the registry comes from the opt-in shim and the clock's symbols from
# the test binary, because both are singletons reached through a header.
yacer_add_test(
  NAME AudioPrerollSink
  STANDARD 20
  SOURCES TestAudioPrerollSink.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/AudioSinkAE.cpp
               xbmc/cores/AudioEngine/Utils/AEChannelInfo.cpp
               xbmc/platform/posix/threads/RecursiveMutex.cpp
  INCLUDES ${YACER_SHIMS}/registry ${YACER_SHIMS}
           ${KODI_SOURCE}/xbmc/platform/posix ${KODI_SOURCE}/xbmc/cores/VideoPlayer
)

# The gate is header-only. The AML side of this - the QBUF generation check in
# the codec and the renderer's handoff - is not here: AMLCodec.cpp needs GBM
# headers and a cmake-generated one, so it wants a build dependency rather than
# a seam.
yacer_add_test(
  NAME VideoPrerollGate
  SOURCES TestVideoPrerollGate.cpp
  INCLUDES ${YACER_SHIMS} ${KODI_SOURCE}/xbmc/cores/VideoPlayer
)

# Exact player message handlers/prologue, with controllable collaborators. The
# full image compiles the actual class contracts; this suite tests their ordering.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${KODI_SOURCE}/xbmc/cores/VideoPlayer/VideoPlayer.cpp"
  "${KODI_SOURCE}/xbmc/cores/VideoPlayer/VideoPlayerAudio.cpp"
  "${CMAKE_SOURCE_DIR}/player_preroll_sections.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/player_preroll_sections.py"
  "${KODI_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/PrerollPlayerSections.inc"
  COMMAND_ERROR_IS_FATAL ANY)
yacer_add_test(
  NAME PlayerPrerollWiring
  SOURCES TestPlayerPrerollWiring.cpp
  INCLUDES ${CMAKE_CURRENT_BINARY_DIR}
)

# Stand-ins for the codec's collaborators that say nothing about how it stamps
# TrueHD, written here because a patch group holds no subdirectories: process
# information that asks for IEC (the MAT path) unless a test sets raw, empty
# stream hints and codec options, and no settings, so dialogue-normalisation
# defeat is off. The codec, parser and MAT packer are the real ones.
set(truehd_shims "${CMAKE_CURRENT_BINARY_DIR}/truehd_shims")
file(WRITE "${truehd_shims}/cores/VideoPlayer/Process/ProcessInfo.h" "#pragma once
// IEC (the MAT path) unless a test asks for raw passthrough.
class CProcessInfo
{
public:
  bool raw = false;
  bool WantsRawPassthrough() const { return raw; }
};
")
file(WRITE "${truehd_shims}/DVDStreamInfo.h" "#pragma once
struct CDVDStreamInfo
{
  int profile = 0;
  int bitspersample = 24;
};
")
file(WRITE "${truehd_shims}/DVDCodecs/DVDCodecs.h" "#pragma once
class CDVDCodecOptions
{
};
")
file(WRITE "${truehd_shims}/utils/Variant.h" "#pragma once
class CVariant
{
public:
  int asInteger() const { return 0; }
};
")
file(WRITE "${truehd_shims}/settings/Settings.h" "#pragma once
#include \"utils/Variant.h\"
#include <vector>
class CSettings
{
public:
  static constexpr const char* SETTING_COREELEC_AUDIO_DIALNORMDEFEAT = \"unused\";
  std::vector<CVariant> GetList(const char*) const { return {}; }
};
")
file(WRITE "${truehd_shims}/settings/SettingsComponent.h" "#pragma once
#include \"settings/Settings.h\"
class CSettingsComponent
{
public:
  CSettings* GetSettings() { return nullptr; }
};
")
file(WRITE "${truehd_shims}/ServiceBroker.h" "#pragma once
#include \"settings/SettingsComponent.h\"
class CServiceBroker
{
public:
  static CSettingsComponent* GetSettingsComponent() { return nullptr; }
};
")

# The real codec, parser and MAT packer, with libavutil's crc.c for the major
# header's checksum: a timestamp test against an imitation of any of them
# would prove nothing about what reaches the receiver.
yacer_add_test(
  NAME TrueHDTimeline
  STANDARD 20
  SOURCES TestTrueHDTimeline.cpp
  KODI_SOURCES xbmc/cores/VideoPlayer/DVDCodecs/Audio/DVDAudioCodecPassthrough.cpp
               xbmc/cores/AudioEngine/Utils/AEStreamInfo.cpp
               xbmc/cores/AudioEngine/Utils/PackerMAT.cpp
               xbmc/cores/AudioEngine/Utils/AEChannelInfo.cpp
               xbmc/utils/BitstreamReader.cpp
               .yacer-src/libavutil/crc.c
  INCLUDES ${truehd_shims} ${YACER_SHIMS}
           ${KODI_SOURCE}/xbmc/platform/posix
)
