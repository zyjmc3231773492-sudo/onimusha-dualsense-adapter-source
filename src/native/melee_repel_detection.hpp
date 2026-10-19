#pragma once

namespace melee_repel_detection {

struct Evidence {
    bool playerSupporter = false;
    bool succeeded = false;
    bool attackCharacterPresent = false;
    bool recentRepelInput = false;
    bool gameplayPaused = false;
};

// Input alone, a failed defensive attempt, enemy-side checks, and shell checks
// must never be enough to emit the player's melee Repel speaker cue.
constexpr bool ShouldEmit(const Evidence& evidence) {
    return evidence.playerSupporter && evidence.succeeded &&
        evidence.attackCharacterPresent && evidence.recentRepelInput &&
        !evidence.gameplayPaused;
}

}  // namespace melee_repel_detection
