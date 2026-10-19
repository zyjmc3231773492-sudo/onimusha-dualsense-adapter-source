#pragma once
#include "product_runtime.hpp"
#include "speaker_preview_pipe.hpp"

namespace speaker_preview {
inline product_runtime::Settings WithVolume(product_runtime::Settings settings, const Request& request) {
    const float volume = static_cast<float>(request.volume) / 100.0f;
    switch (request.cue) {
    case 2: settings.soulSpeakerVolume = volume; break;
    case 4: settings.clashSpeakerVolume = volume; break;
    case 5: settings.guardSpeakerVolume = volume; break;
    case 6: settings.deflectSpeakerVolume = volume; break;
    case 7: settings.executionSpeakerVolume = volume; break;
    case 11: settings.issenSpeakerVolume = volume; break;
    case 12: case 13: settings.parrySpeakerVolume = volume; break;
    case 14: settings.gauntletVoiceVolume = volume; break;
    default: break;
    }
    return settings;
}
} // namespace speaker_preview
