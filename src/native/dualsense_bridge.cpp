// Onimusha: Way of the Sword PC DualSense adaptive-trigger bridge.
//
// Trigger packet encoding follows the MIT-licensed effect factories by
// John "Nielk1" Klein and the public USB report layout used by SDL/Microsoft.
// The game-event hooks and normalized effect parameters are specific to the
// 1.0.1.0 / Steam build 24769601 executable and are resolved through
// REFramework type metadata rather than fixed executable offsets.

#define NOMINMAX
#include <Windows.h>
#include <Xinput.h>

#pragma comment(lib, "user32.lib")

#include <reframework/API.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adaptive_trigger_effects.hpp"
#include "black_soul_haptics.hpp"
#include "ladder_haptics.hpp"
#include "wall_run_haptics.hpp"
#include "normal_attack_haptics.hpp"
#include "execution_haptics.hpp"
#include "captured_haptic_presets.hpp"
#include "melee_repel_detection.hpp"
#include "combat_defense_feedback.hpp"
#include "live_motor_state.hpp"
#include "treasure_chest_haptics.hpp"
#include "oni_transformation_haptics.hpp"
#include "oni_charge_release_haptics.hpp"
#include "bow_shot_haptics.hpp"
#include "cutscene_authored_haptics.h"
#include "dualsense_touch_input.hpp"

namespace {
using reframework::API;

constexpr size_t OutputReportLength = 48;
constexpr size_t RightTriggerOffset = 0x0B;
constexpr size_t LeftTriggerOffset = 0x16;
constexpr uint8_t RightTriggerFlag = 0x04;
constexpr uint8_t LeftTriggerFlag = 0x08;
constexpr uint8_t TriggerEffectOff = 0x05;
constexpr uint8_t TriggerEffectFeedback = 0x21;
constexpr uint8_t TriggerEffectBow = 0x22;
constexpr uint8_t TriggerEffectVibration = 0x26;
constexpr uint8_t HapticPlayMessage = 0xF1;
constexpr uint8_t HapticStopAllMessage = 0xF2;
constexpr uint8_t HapticMotorPowerMessage = 0xF3;
constexpr uint8_t HapticMotorPulseMessage = 0xF4;
constexpr uint8_t HapticStopMotorMessage = 0xF5;
constexpr uint8_t HapticStopWaveformsMessage = 0xF6;
constexpr uint8_t HapticEnvironmentMessage = 0xF7;
constexpr uint8_t HapticFootstepMessage = 0xF8;
constexpr uint8_t HapticMapZoomMessage = 0xF9;
constexpr uint8_t HapticPlayerReactionMessage = 0xFA;
constexpr uint8_t ControllerSpeakerMessage = 0xFB;
constexpr uint8_t GauntletVoiceMessage = 0xFC;
constexpr uint8_t TitleHapticMessage = 0xFD;
constexpr uint8_t HapticEnvironmentInteractionMessage = 0xED;
constexpr uint8_t HapticLongPressMessage = 0xEE;
constexpr uint8_t HapticOniStrengthMessage = 0xEF;
constexpr uint8_t HapticCapturedPresetMessage = 0xEC;
constexpr uint8_t HapticCombatActionMessage = 0xE9;
constexpr uint8_t HapticContinuousPlayerMessage = 0xEA;
constexpr uint8_t HapticOniEyeMessage = 0xEB;
static_assert(oni_transformation_haptics::Message != wall_run_haptics::Message,
    "Oni transformation and wall-run IPC message IDs must be unique");
static_assert(oni_transformation_haptics::Message != normal_attack_haptics::Message,
    "Oni transformation and normal-attack IPC message IDs must be unique");
static_assert(oni_charge_release_haptics::Message != oni_transformation_haptics::Message &&
    oni_charge_release_haptics::Message != normal_attack_haptics::Message &&
    oni_charge_release_haptics::Message != wall_run_haptics::Message,
    "Oni charge release IPC message ID must be unique");
static_assert(bow_shot_haptics::Message != oni_charge_release_haptics::Message &&
    bow_shot_haptics::Message != oni_transformation_haptics::Message &&
    bow_shot_haptics::Message != normal_attack_haptics::Message &&
    bow_shot_haptics::Message != wall_run_haptics::Message,
    "Bow shot IPC message ID must be unique");
constexpr uint16_t ParryInputChord =
    XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_A;
std::atomic<bool> g_titleActive{false};
std::atomic<ULONGLONG> g_lastTitleAccent{0};
std::atomic<ULONGLONG> g_lastTitleVisible{0};
std::atomic<ULONGLONG> g_lastTitleDecision{0};

enum class PlayerHapticProfile : uint32_t {
    Hit = 1,
    Stab = 2,
    Collision = 3,
    Knockdown = 4,
    Grab = 5,
    Dodge = 6,
    JustDodge = 7,
    CinematicImpact = 8
};
enum class EnvironmentInteractionProfile : uint8_t {
    GenericSuccess = 0,
    Door = 1,
    Lever = 2,
    ElevatorStart = 3,
    ElevatorMove = 4,
    ElevatorStop = 5,
    TreasureChestOpen = 6,
    TreasureChestPush = 7,
    TreasureChestLidLand = 8
};
// Bluetooth DualSense audio haptics and compatible rumble share a controller
// selection bit. The latter explicitly disables the former while active.
// Keep the chest on the authored PCM path only; otherwise the fallback routes
// can mask the push/landing voices even when their HID writes succeed.
constexpr bool UseTreasureChestLegacyMotorFallback = false;
enum class ControllerSpeakerCue : uint32_t {
    None = 0,
    SoulStart = 1,
    SoulCollect = 2,
    SoulStop = 3,
    CombatClash = 4,
    CombatGuard = 5,
    CombatDeflect = 6,
    CombatExecution = 7,
    CombatClashHit = 8,
    CombatClashSuccess = 9,
    CombatGuardDirectional = 10,
    CombatIssen = 11,
    CombatParryBlade = 12,
    CombatParryProjectile = 13
};
enum class CombatHapticProfile : uint8_t {
    Guard = 1,
    Parry = 2,
    Issen = 3,
    Deflect = 4,
    ProjectileParry = 5,
    ParryActivation = 6,
    ExecutionThrust = 7,
    ExecutionSlash = 8,
    ExecutionArmCut = 9,
    ExecutionDowned = 10,
    ExecutionConst = 11,
    ExecutionBreakIssen = 12,
    ExecutionBlockIssen = 13
};
enum class ContinuousPlayerHaptic : uint8_t {
    SoulAbsorption = 1,
    Sprint = 2,
    SoulSearch = 3,
    BlackSoulAbsorption = black_soul_haptics::Profile,
    LadderClimb = ladder_haptics::ClimbProfile,
    LadderFastSlide = ladder_haptics::FastSlideProfile
};
enum class ContinuousHapticOperation : uint8_t {
    Stop = 0,
    Start = 1,
    Refresh = 2,
    Accent = 3,
    Decelerate = 4
};
constexpr uint32_t InvalidVibrationId = 0xFFFFFFFFu;
// The native preset observed immediately after a successful L1 + Cross Repel.
// It is also used elsewhere, so it is only a Repel confirmation while the
// physical input chord is still inside the short correlation window.
constexpr uint32_t BowEventEffect = 0x01u;
constexpr uint32_t AbsorbEffect = 0x02u;
constexpr uint32_t BowMainActionEffect = 0x04u;
constexpr uint32_t BowSubActionEffect = 0x08u;
constexpr uint32_t BowFullChargeEffect = 0x10u;
constexpr uint32_t BowEquippedEffect = 0x20u;
constexpr uint32_t BowEffectMask =
    BowEventEffect | BowMainActionEffect | BowSubActionEffect |
    BowFullChargeEffect | BowEquippedEffect;
constexpr uint32_t BowActionEffectMask =
    BowEventEffect | BowMainActionEffect | BowSubActionEffect |
    BowFullChargeEffect;
constexpr ULONGLONG BowFullDrawVisualDelayMs = 1120;
// Runtime VOICE_REQUEST_PARAM value captured from the gauntlet test scene.
// This is not the same namespace as SubtitleSpeakerInfo._Index.
constexpr int32_t GauntletSpeakerIndex = 200;
struct GauntletVoiceMapping {
    GUID messageGuid;
    uint32_t triggerId;
};
constexpr GauntletVoiceMapping GauntletVoiceMappings[] = {
#include "gauntlet_voice_catalog.inc"
};

std::mutex g_mutex;
int g_moduleAnchor = 0;
HANDLE g_pipe = INVALID_HANDLE_VALUE;
HANDLE g_helperProcess = nullptr;
std::wstring g_helperPath;
std::wstring g_gameDirectory;
std::wstring g_pipeName;
HANDLE g_touchMapping = nullptr;
dualsense_touch_input::SharedState* g_touchState = nullptr;
uint32_t g_activeEffects = 0;
uint32_t g_lastHapticTrigger = 0;
float g_lastHapticLeftGain = 0.0f;
float g_lastHapticRightGain = 0.0f;
ULONGLONG g_lastHapticTick = 0;
uint32_t g_pendingSpatialTrigger = 0;
ULONGLONG g_pendingSpatialTick = 0;
live_motor_state::Sources g_liveMotorSources;
ULONGLONG g_lastMotorHeartbeatTick = 0;
ULONGLONG g_lastMotorConnectionPollTick = 0;
std::array<ULONGLONG, 8> g_lastMotorSourceLogTick{};
uint32_t g_motorSourceLogCount = 0;
std::unordered_map<uintptr_t, int32_t> g_shortcutWheelSelections;
struct MapHapticState {
    bool scaleInitialized = false;
    float scaleProgress = 0.0f;
    bool cursorInitialized = false;
    bool absorbed = false;
    int32_t absorbedIndex = -1;
    ULONGLONG lastZoomSendTick = 0;
    ULONGLONG lastZoomMotionTick = 0;
    int32_t lastZoomDirection = 0;
};
std::mutex g_mapMutex;
std::unordered_map<uintptr_t, MapHapticState> g_mapHapticStates;
std::array<float, 4> g_sentMotorPower{-1.0f, -1.0f, -1.0f, -1.0f};
std::array<ULONGLONG, 4> g_lastMotorLogTick{};
std::array<ULONGLONG, 4> g_lastPulseTick{};
std::array<float, 4> g_lastPulsePower{};
std::array<float, 4> g_lastPulseDuration{};
std::atomic<uintptr_t> g_nativeGamePadDevice{0};
API::Method* g_nativeSetMotorPowerMethod = nullptr;
thread_local bool g_nativeMotorInvocation = false;
std::array<float, 2> g_nativeTreasureChestMotorPower{};
std::array<ULONGLONG, 2> g_nativeTreasureChestMotorUntil{};
std::atomic<ULONGLONG> g_lastTreasureChestXInputOverrideLogTick{0};
using XInputSetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*);
using XInputGetStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
DWORD WINAPI HookedXInputSetState(DWORD userIndex, XINPUT_VIBRATION* vibration);
XInputSetStateFn g_originalXInputSetState = nullptr;
XInputGetStateFn g_originalXInputGetState = nullptr;
std::atomic<DWORD> g_lastXInputUser{0};
ULONG_PTR* g_xinputSetStateIatSlot = nullptr;
ULONG_PTR* g_xinputGetStateIatSlot = nullptr;
std::atomic<uint32_t> g_xinputRehookCount{0};
std::atomic<uint32_t> g_xinputGetRehookCount{0};
std::array<std::atomic<uint16_t>, XUSER_MAX_COUNT> g_lastXInputButtons{};
std::array<std::atomic<bool>, XUSER_MAX_COUNT> g_soulActionInputHeld{};
std::array<std::atomic<ULONGLONG>, XUSER_MAX_COUNT> g_oniChargeButtonDownTick{};
std::array<std::atomic<ULONGLONG>, XUSER_MAX_COUNT> g_oniChargeRefreshTick{};
std::array<std::atomic<uint8_t>, XUSER_MAX_COUNT> g_oniChargeInputKind{};
std::array<std::atomic<bool>, XUSER_MAX_COUNT> g_oniChargeFeedbackActive{};
std::atomic<bool> g_ordinaryFormActive{false};
std::array<std::atomic<uint8_t>, XUSER_MAX_COUNT> g_oniChargeFallbackKind{};
std::array<std::atomic<ULONGLONG>, XUSER_MAX_COUNT> g_oniChargeFallbackTick{};
constexpr uint8_t OniFinisherInputCode = 0xFEu;
constexpr uint8_t OniFinisherPreparingCode = 0xFDu;
constexpr uint8_t OniFinisherSlashSentCode = 0xFCu;
// The finisher cuts to black before the blade starts to leave the sheath.
// Silence the held-charge bed at that cut instead of letting it leak into the
// black frame; the short draw cue is deliberately scheduled later by helper.
constexpr ULONGLONG OniFinisherPrepareMs = 3200u;
std::atomic<bool> g_oniTransformationActive{false};
std::atomic<uint8_t> g_pendingOniChargeReleaseKind{0};
std::atomic<ULONGLONG> g_pendingOniChargeReleaseTick{0};
std::atomic<uintptr_t> g_pendingOniChargeOwner{0};
std::atomic<ULONGLONG> g_lastOniChargeHitTick{0};
std::atomic<ULONGLONG> g_lastOniChargeReleaseTick{0};
std::atomic<uint8_t> g_lastOniChargeReleaseKind{0};
std::atomic<uint32_t> g_soulSearchSuppressedUserMask{0};
std::atomic<ULONGLONG> g_lastParryInputTick{0};
std::mutex g_deflectEpisodeMutex;
combat_defense_feedback::DeflectEpisodeGate g_deflectEpisodeGate;
std::atomic<uint32_t> g_deflectRepeatedCallbackCount{0};
std::atomic<uint64_t> g_defenseResultSequence{0};
bool g_cinematicParrySchemaReady = false;
bool g_cinematicParryFieldsReady = false;
API::Field* g_parryArgumentField = nullptr;
API::Field* g_parryRequestActionField = nullptr;
API::Field* g_parryCurrentActionField = nullptr;
API::Field* g_parryCategoryField = nullptr;
API::Field* g_parryEnemyActionField = nullptr;
API::Field* g_parryLargeField = nullptr;
std::atomic<ULONGLONG> g_lastParryActivationTick{0};
std::atomic<ULONGLONG> g_repelStandardRumbleSuppressionUntil{0};
std::atomic<uint32_t> g_repelStandardRumbleSuppressedCount{0};
std::atomic<ULONGLONG> g_executionStandardRumbleSuppressionUntil{0};
std::atomic<uint32_t> g_executionStandardRumbleSuppressedCount{0};
std::atomic<ULONGLONG> g_lastExecutionHapticTick{0};
std::atomic<bool> g_gameplayHapticsPaused{false};
// A playback heartbeat is safer than retaining a managed cutscene object
// across scene teardown.  Camera shake callbacks can arrive on either side
// of an update, so allow a short grace window around the last PLAY update.
std::atomic<ULONGLONG> g_cutscenePlayHeartbeatTick{0};
std::atomic<ULONGLONG> g_lastCutsceneCameraShakeTick{0};
std::atomic<uintptr_t> g_lastCutsceneCameraShakeParam{0};
std::atomic<ULONGLONG> g_lastCutsceneCameraShakeParamTick{0};
std::atomic<ULONGLONG> g_lastCutsceneAuthoredCueTick{0};
API::Method* g_cutsceneResourceGetter = nullptr;
API::Method* g_cutsceneFlowFrameGetter = nullptr;
std::mutex g_cutsceneCueStateMutex;
uintptr_t g_cutsceneCuePlayer = 0;
int32_t g_cutsceneCueEventId = -1;
int32_t g_cutsceneCueFlowId = -1;
double g_cutsceneCueLastFrame = -1.0;
ULONGLONG g_cutsceneCueLastTick = 0;
API::Method* g_hapticPausingGetter = nullptr;
API::Method* g_menuPauseGetter = nullptr;
API::Method* g_anyPauseGetter = nullptr;
API::Method* g_subWeaponIdGetter = nullptr;
API::Method* g_subWeaponEnableGetter = nullptr;
API::Method* g_bowDisableTimerEnabledGetter = nullptr;
API::Method* g_bowDisableTimerIsTimeOutGetter = nullptr;
API::Method* g_currentZoneSafeAreaGetter = nullptr;
API::Method* g_isRainyWeatherGetter = nullptr;
std::atomic<float> g_rainVisualAlpha{-1.0f};
std::atomic<float> g_lastLoggedRainVisualAlpha{-2.0f};
std::atomic<uintptr_t> g_playerCharacterEntity{0};
std::atomic<uintptr_t> g_playerActionSelector{0};
ULONGLONG g_lastRainRoofPollTick = 0;
bool g_rainRoofCovered = false;
int g_lastLoggedRainRoofCovered = -1;
std::atomic<int> g_lastRainRoofProbeStatus{-1};
ULONGLONG g_lastHapticPausingPollTick = 0;
std::array<std::atomic<bool>, XUSER_MAX_COUNT> g_sprintToggleActive{};
std::array<std::atomic<ULONGLONG>, XUSER_MAX_COUNT> g_sprintSlowCandidateTick{};
std::array<std::atomic<float>, XUSER_MAX_COUNT> g_sprintStartStickMagnitude{};
std::atomic<bool> g_adaptiveSubscriptionsReady{false};
ULONGLONG g_lastAdaptiveSubscriptionAttempt = 0;
std::atomic<ULONGLONG> g_bowPullStartedTick{0};
std::atomic<ULONGLONG> g_lastBowShotHapticTick{0};
std::atomic<bool> g_bowTriggerSuppressedByLeftTrigger{false};
ULONGLONG g_lastBowEquippedPollTick = 0;
int32_t g_lastLoggedSubWeaponId = INT32_MIN;
int g_lastLoggedSubWeaponEnable = -1;
int g_lastLoggedBowDisabled = -1;
int g_lastLoggedBowSafeArea = -1;
ULONGLONG g_lastEnvironmentPollTick = 0;
ULONGLONG g_lastEnvironmentSendTick = 0;
float g_lastRainLevel = -1.0f;
float g_lastWindLevel = -1.0f;
#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
std::atomic<bool> g_rainPreviewEnabled{false};
bool g_rainPreviewKeyHeld = false;
#endif
std::atomic<ULONGLONG> g_lastMenuSelectionTick{0};
std::atomic<ULONGLONG> g_lastMenuNavigationInputTick{0};
std::atomic<ULONGLONG> g_lastMenuSelectionSuppressionLogTick{0};
std::atomic<ULONGLONG> g_lastMenuDecisionTick{0};
std::atomic<ULONGLONG> g_lastMenuCancelTick{0};
std::atomic<ULONGLONG> g_lastShortcutSelectionTick{0};
std::atomic<ULONGLONG> g_lastItemTick{0};
std::atomic<ULONGLONG> g_lastUseItemTick{0};
std::atomic<ULONGLONG> g_lastInteractionTick{0};
std::atomic<ULONGLONG> g_pendingInteractionTick{0};
std::atomic<bool> g_pendingInteractionFeedback{false};
std::atomic<ULONGLONG> g_lastGimmickSuccessTick{0};
std::atomic<ULONGLONG> g_lastDoorClassifiedTick{0};
std::atomic<ULONGLONG> g_pendingDoorFeedbackTick{0};
std::atomic<ULONGLONG> g_lastDoorOpeningTick{0};
std::atomic<ULONGLONG> g_lastLeverTick{0};
std::atomic<ULONGLONG> g_lastElevatorStartTick{0};
std::atomic<ULONGLONG> g_lastElevatorMoveTick{0};
std::atomic<ULONGLONG> g_lastElevatorStopTick{0};
std::atomic<uintptr_t> g_activeElevator{0};
std::atomic<ULONGLONG> g_lastTreasureChestOpenTick{0};
std::atomic<ULONGLONG> g_lastTreasureChestPushTick{0};
std::atomic<ULONGLONG> g_lastTreasureChestLidLandTick{0};
std::atomic<ULONGLONG> g_pendingTreasureChestOpenCueTick{0};
std::atomic<ULONGLONG> g_pendingTreasureChestPushCueTick{0};
std::atomic<ULONGLONG> g_pendingTreasureChestLidLandTick{0};
std::atomic<ULONGLONG> g_treasureChestProbeUntil{0};
std::atomic<uintptr_t> g_activeTreasureChest{0};
std::atomic<ULONGLONG> g_lastFootstepTick{0};
std::atomic<ULONGLONG> g_lastMapFocusTick{0};
std::atomic<ULONGLONG> g_lastMapMarkerTick{0};
std::atomic<ULONGLONG> g_lastMapVisibleTick{0};
std::atomic<ULONGLONG> g_lastLongPressCompleteTick{0};
std::atomic<ULONGLONG> g_lastOniStrengthTick{0};
std::atomic<ULONGLONG> g_lastOniStrengthBarrierTick{0};
std::atomic<ULONGLONG> g_lastOniEyeTick{0};
std::atomic<ULONGLONG> g_lastOniTransformationTick{0};
std::mutex g_normalAttackMutex;
normal_attack_haptics::Tracker g_normalAttackTracker{};
normal_attack_haptics::ChargeTracker g_oniChargeAttackTracker{};
normal_attack_haptics::ChargeTracker g_ordinaryChargeAttackTracker{};
std::mutex g_ordinaryLightWaveMutex;
uintptr_t g_ordinaryLightWaveAction{};
uint8_t g_ordinaryLightWaveStage{};
ULONGLONG g_ordinaryLightWaveTick{};
std::mutex g_ordinaryHeavyImpactMutex;
uintptr_t g_ordinaryHeavyImpactAction{};
uint8_t g_ordinaryHeavyImpactStage{};
ULONGLONG g_ordinaryHeavyDownstrokeTick{};
std::atomic<bool> g_wallRunHapticActive{false};
std::atomic<bool> g_wallRunAlongWallActive{false};
std::mutex g_wallRunLandingMutex;
wall_run_haptics::LandingTracker g_wallRunLandingTracker{};
std::atomic<ULONGLONG> g_lastWallRunAirRefreshLogTick{0};
std::atomic<uintptr_t> g_wallRunActionObject{0};
std::atomic<ULONGLONG> g_lastWallRunStateTick{0};
std::atomic<ULONGLONG> g_lastWallRunRefreshTick{0};
std::atomic<ULONGLONG> g_wallRunImpulseUntilTick{0};
std::atomic<uintptr_t> g_lastWallRunEnteredObject{0};
std::atomic<ULONGLONG> g_lastWallRunEnteredTick{0};
std::atomic<ULONGLONG> g_lastWallRunContactTick{0};
std::atomic<ULONGLONG> g_lastWallRunLandTick{0};
std::array<std::atomic<ULONGLONG>, 2> g_lastWallRunFootTick{};
std::atomic<bool> g_ladderHapticActive{false};
std::atomic<uint8_t> g_ladderHapticProfile{
    ladder_haptics::ClimbProfile};
std::atomic<ULONGLONG> g_lastLadderStateTick{0};
std::atomic<ULONGLONG> g_lastLadderRefreshTick{0};
std::atomic<uintptr_t> g_ladderActionObject{0};
std::atomic<uintptr_t> g_lastLadderEnteredObject{0};
std::atomic<ULONGLONG> g_lastLadderEnteredTick{0};
std::array<std::atomic<ULONGLONG>, 8> g_lastPlayerReactionTick{};
std::array<std::atomic<ULONGLONG>, 14> g_lastCombatSpeakerTick{};
std::atomic<ULONGLONG> g_lastCombatSpeakerActionTick{0};
std::atomic<uint32_t> g_lastCombatSpeakerCue{0};
struct RecentSoundTrigger {
    ULONGLONG tick{};
    uint32_t triggerId{};
    uint32_t eventId{};
    const char* source{};
};
std::mutex g_recentSoundTriggerMutex;
std::array<RecentSoundTrigger, 32> g_recentSoundTriggers{};
size_t g_recentSoundTriggerWriteIndex = 0;
std::atomic<ULONGLONG> g_lastSoulCollectTick{0};
std::atomic<ULONGLONG> g_lastSoulFlowEvidenceTick{0};
std::atomic<bool> g_blackSoulHapticActive{false};
std::atomic<ULONGLONG> g_lastBlackSoulStateTick{0};
std::atomic<ULONGLONG> g_lastBlackSoulRefreshTick{0};
std::atomic<int32_t> g_lastBlackSoulMainState{0};
std::atomic<int32_t> g_lastBlackSoulSubState{0};
std::atomic<ULONGLONG> g_blackSoulFinishUntilTick{0};
std::atomic<ULONGLONG> g_lastGauntletVoiceTick{0};
std::atomic<bool> g_soulSpeakerActive{false};
std::atomic<bool> g_soulHapticActive{false};
std::atomic<ULONGLONG> g_lastSoulHapticRefreshTick{0};
std::atomic<bool> g_soulActionActive{false};
std::atomic<bool> g_soulSearchHapticActive{false};
std::atomic<ULONGLONG> g_soulSearchCandidateTick{0};
std::atomic<ULONGLONG> g_lastSoulSearchRefreshTick{0};
std::array<std::atomic<ULONGLONG>, 2> g_lastFootstepSideTick{};
std::atomic<uint32_t> g_lastLoggedSurface{0xFFFFFFFFu};
std::atomic<ULONGLONG> g_lastSprintButtonTick{0};
// REFramework passes this value type unboxed to the hook. TypeDB field offsets
// include the 0x10-byte managed-object header, so SpeakerId 0x20 becomes 0x10.
uint32_t g_voiceSpeakerOffset = 0x10;
struct VoiceRequestState {
    bool gauntlet = false;
    bool voiceSent = false;
};
thread_local std::vector<VoiceRequestState> g_voiceRequestStack;
struct RepelSoundCallState {
    API::ManagedObject* behavior = nullptr;
    API::ManagedObject* partner = nullptr;
    uint32_t grappleType = 0;
    int argumentCount = 0;
};
thread_local std::vector<RepelSoundCallState> g_repelSoundCallStack;
struct ProjectileRepelSoundCallState {
    API::ManagedObject* behavior = nullptr;
    uint32_t trigger = 0;
    bool recentRepelInput = false;
};
thread_local std::vector<ProjectileRepelSoundCallState> g_projectileRepelSoundCallStack;
struct MeleeRepelCallState {
    melee_repel_detection::Evidence evidence{};
    int32_t style = -1;
    ULONGLONG callStarted = 0;
    uint64_t resultId = 0;
};
thread_local std::vector<MeleeRepelCallState> g_meleeRepelCallStack;
thread_local uint32_t g_resolvingSoundTriggerId = 0;
thread_local int32_t g_resolvingSoundTriggerInfoIndex = -1;
API::Method* g_combatGetTriggerInfoMethod = nullptr;
struct LongPressHapticState {
    bool initialized = false;
    bool active = false;
    bool wasOnTimer = false;
    float lastProgress = 0.0f;
    float maxProgress = 0.0f;
    ULONGLONG lastSendTick = 0;
    int32_t lastSendBucket = -1;
    int32_t lastLogQuarter = -1;
};
std::mutex g_longPressMutex;
std::unordered_map<uintptr_t, LongPressHapticState> g_longPressStates;
thread_local std::vector<API::ManagedObject*> g_longPressUpdateStack;
LongPressHapticState g_equipmentEnhanceLongPressState;
thread_local std::vector<uintptr_t> g_equipmentEnhanceGaugeStack;
std::atomic<ULONGLONG> g_lastEquipmentEnhanceVisibleTick{0};
std::atomic<ULONGLONG> g_lastEquipmentEnhanceCompleteTick{0};

void PlayHapticTrigger(uint32_t triggerId, float gain);
void PlayHapticTriggerStereo(uint32_t triggerId, float leftGain, float rightGain);
void SetEffectState(uint32_t bit, bool enabled);
void RefreshEffectLocked();
void RefreshBowChargePhase();

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    return path.substr(0, separator);
}

std::wstring ModulePath(HMODULE module) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

bool ResolvePathsLocked() {
    if (!g_helperPath.empty() && !g_gameDirectory.empty() && !g_pipeName.empty()) {
        return true;
    }

    HMODULE pluginModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_moduleAnchor),
            &pluginModule)) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Could not locate plugin module error=%lu",
            GetLastError());
        return false;
    }

    const std::wstring pluginDirectory = DirectoryOf(ModulePath(pluginModule));
    g_gameDirectory = DirectoryOf(ModulePath(nullptr));
    if (pluginDirectory.empty() || g_gameDirectory.empty()) {
        API::get()->log_warn("[CodexDualSenseBridge] Could not resolve module directories");
        return false;
    }
    g_helperPath = pluginDirectory + L"\\codex_dualsense_hid_helper.exe";
    g_pipeName = L"\\\\.\\pipe\\CodexOnimushaDualSense_" +
        std::to_wstring(GetCurrentProcessId());
    return true;
}

void CloseHelperHandlesLocked() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_helperProcess != nullptr) {
        CloseHandle(g_helperProcess);
        g_helperProcess = nullptr;
    }
}

bool StartHelperLocked() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        return true;
    }
    if (!ResolvePathsLocked()) {
        return false;
    }
    if (g_touchState == nullptr) {
        const std::wstring mappingName = dualsense_touch_input::MappingName(
            GetCurrentProcessId());
        g_touchMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, sizeof(dualsense_touch_input::SharedState),
            mappingName.c_str());
        if (g_touchMapping != nullptr) {
            g_touchState = static_cast<dualsense_touch_input::SharedState*>(
                MapViewOfFile(g_touchMapping, FILE_MAP_READ | FILE_MAP_WRITE,
                    0, 0, sizeof(dualsense_touch_input::SharedState)));
        }
        if (g_touchState == nullptr) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Touch input mapping unavailable error=%lu",
                GetLastError());
            if (g_touchMapping != nullptr) CloseHandle(g_touchMapping);
            g_touchMapping = nullptr;
        }
    }
    if (GetFileAttributesW(g_helperPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] HID helper is missing error=%lu",
            GetLastError());
        return false;
    }

    if (g_helperProcess != nullptr) {
        CloseHandle(g_helperProcess);
        g_helperProcess = nullptr;
    }

    std::wstring commandLine = L"\"" + g_helperPath + L"\" \"" + g_pipeName + L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Could not initialize helper mitigation error=%lu",
            GetLastError());
        return false;
    }
    DWORD64 mitigationPolicy =
        PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON;
    if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
            &mitigationPolicy,
            sizeof(mitigationPolicy),
            nullptr,
            nullptr)) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        API::get()->log_warn(
            "[CodexDualSenseBridge] Could not configure helper mitigation error=%lu",
            error);
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
            g_helperPath.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            g_gameDirectory.c_str(),
            &startup.StartupInfo,
            &process);
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Could not launch HID helper error=%lu",
            GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    g_helperProcess = process.hProcess;

    DWORD lastError = ERROR_FILE_NOT_FOUND;
    for (int attempt = 0; attempt < 100; ++attempt) {
        g_pipe = CreateFileW(
            g_pipeName.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            API::get()->log_info(
                "[CodexDualSenseBridge] HID helper connected pid=%lu",
                process.dwProcessId);
            return true;
        }
        lastError = GetLastError();
        if (WaitForSingleObject(g_helperProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (lastError == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(g_pipeName.c_str(), 25);
        } else {
            Sleep(25);
        }
    }

    API::get()->log_warn(
        "[CodexDualSenseBridge] Could not connect to HID helper error=%lu",
        lastError);
    CloseHelperHandlesLocked();
    return false;
}

bool WritePipeLocked(const std::array<uint8_t, OutputReportLength>& report) {
    if (g_pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytesWritten = 0;
    const BOOL ok = WriteFile(
        g_pipe,
        report.data(),
        static_cast<DWORD>(report.size()),
        &bytesWritten,
        nullptr);
    return ok && bytesWritten == report.size();
}

bool SendReportLocked(const std::array<uint8_t, OutputReportLength>& report) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!StartHelperLocked()) {
            return false;
        }
        if (WritePipeLocked(report)) {
            return true;
        }

        const DWORD error = GetLastError();
        API::get()->log_warn(
            "[CodexDualSenseBridge] Pipe write failed error=%lu; restarting helper",
            error);
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
        if (g_helperProcess != nullptr) {
            WaitForSingleObject(g_helperProcess, 750);
            CloseHandle(g_helperProcess);
            g_helperProcess = nullptr;
        }
    }
    return false;
}

uint8_t NormalizedStrength(float value) {
    return static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 8.0f)),
        0,
        8));
}

uint8_t NormalizedFrequency(float value) {
    return static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)),
        0,
        255));
}

void EncodeMultiZoneEffect(
    std::array<uint8_t, OutputReportLength>& report,
    size_t offset,
    const std::array<float, 10>& normalizedStrengths,
    float normalizedFrequency) {
    uint16_t activeZones = 0;
    uint32_t strengthZones = 0;
    for (size_t index = 0; index < normalizedStrengths.size(); ++index) {
        const uint8_t strength = NormalizedStrength(normalizedStrengths[index]);
        if (strength == 0) {
            continue;
        }
        activeZones |= static_cast<uint16_t>(1u << index);
        strengthZones |= static_cast<uint32_t>((strength - 1u) & 0x07u) << (3u * index);
    }

    const uint8_t frequency = NormalizedFrequency(normalizedFrequency);
    report[offset + 0] = frequency == 0 ? TriggerEffectFeedback : TriggerEffectVibration;
    report[offset + 1] = static_cast<uint8_t>(activeZones & 0xFFu);
    report[offset + 2] = static_cast<uint8_t>((activeZones >> 8u) & 0xFFu);
    report[offset + 3] = static_cast<uint8_t>(strengthZones & 0xFFu);
    report[offset + 4] = static_cast<uint8_t>((strengthZones >> 8u) & 0xFFu);
    report[offset + 5] = static_cast<uint8_t>((strengthZones >> 16u) & 0xFFu);
    report[offset + 6] = static_cast<uint8_t>((strengthZones >> 24u) & 0xFFu);
    report[offset + 9] = frequency;
}

void EncodeBowEffect(
    std::array<uint8_t, OutputReportLength>& report,
    size_t offset,
    uint8_t startPosition,
    uint8_t endPosition,
    uint8_t strength,
    uint8_t snapForce) {
    const uint16_t startAndEndZones = static_cast<uint16_t>(
        (1u << startPosition) | (1u << endPosition));
    const uint8_t forcePair = static_cast<uint8_t>(
        ((strength - 1u) & 0x07u) |
        (((snapForce - 1u) & 0x07u) << 3u));
    report[offset + 0] = TriggerEffectBow;
    report[offset + 1] = static_cast<uint8_t>(startAndEndZones & 0xFFu);
    report[offset + 2] = static_cast<uint8_t>((startAndEndZones >> 8u) & 0xFFu);
    report[offset + 3] = forcePair;
}

bool ApplyBowLocked(bool fullyDrawn) {
    // cBowLoop is entered when the draw animation reaches its held/full state.
    // Only then add a restrained trigger texture. It spans every zone because
    // players do not necessarily keep R2 at full
    // physical travel while the game itself reports a fully drawn bow.
    constexpr std::array<float, 10> fullDrawTexture{
        0.0f, 0.0f, 0.36f, 0.38f, 0.40f,
        0.42f, 0.44f, 0.46f, 0.48f, 0.48f};

    std::array<uint8_t, OutputReportLength> report{};
    report[0] = 0x02;
    report[1] = RightTriggerFlag | LeftTriggerFlag;
    if (fullyDrawn) {
        EncodeMultiZoneEffect(
            report,
            RightTriggerOffset,
            fullDrawTexture,
            0.18f);
    } else {
        // Use the controller's dedicated bow-string effect instead of relying
        // on positional feedback slopes, whose per-zone force is flattened by
        // some DualSense firmware revisions. A moderate draw force and light
        // snap-back produce a softer take-up followed by growing string load.
        EncodeBowEffect(report, RightTriggerOffset, 1, 8, 4, 2);
    }
    report[LeftTriggerOffset] = TriggerEffectOff;
    return SendReportLocked(report);
}

bool ApplyAbsorbLocked() {
    // Retain the supplied DSX position/frequency while raising its very light
    // amplitude 2 to a moderate 4. This strengthens only the L2 point texture;
    // it does not add resistance or alter the bow's independent R2 effect.
    std::array<uint8_t, OutputReportLength> report{};
    report[0] = 0x02;
    report[1] = RightTriggerFlag | LeftTriggerFlag;
    report[RightTriggerOffset] = TriggerEffectOff;
    adaptive_trigger_effects::EncodeDsxVibration(
        report,
        LeftTriggerOffset,
        adaptive_trigger_effects::SoulAbsorbStartPosition,
        adaptive_trigger_effects::SoulAbsorbAmplitude,
        adaptive_trigger_effects::SoulAbsorbFrequencyHz);
    return SendReportLocked(report);
}

bool ResetTriggersLocked() {
    std::array<uint8_t, OutputReportLength> report{};
    report[0] = 0x02;
    report[1] = RightTriggerFlag | LeftTriggerFlag;
    report[RightTriggerOffset] = TriggerEffectOff;
    report[LeftTriggerOffset] = TriggerEffectOff;
    return SendReportLocked(report);
}

bool SendHapticLocked(uint32_t triggerId, float leftGain, float rightGain) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticPlayMessage;
    message[1] = 2;
    std::memcpy(message.data() + 4, &triggerId, sizeof(triggerId));
    std::memcpy(message.data() + 8, &leftGain, sizeof(leftGain));
    std::memcpy(message.data() + 12, &rightGain, sizeof(rightGain));
    return SendReportLocked(message);
}

bool StopHapticsLocked(
    bool preserveNormalAttack = false,
    bool preserveExecution = false,
    bool preserveTreasureChest = false) {
    g_liveMotorSources.Clear();
    g_sentMotorPower.fill(-1.0f);
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticStopAllMessage;
    message[1] = preserveNormalAttack ? 1u : 0u;
    message[2] = preserveExecution ? 1u : 0u;
    message[3] = preserveTreasureChest ? 1u : 0u;
    return SendReportLocked(message);
}

bool SendNormalAttackHaptic(normal_attack_haptics::Kind kind, normal_attack_haptics::Operation operation,
    uint32_t swingLeadFrames = 0u, uint32_t secondarySwingLeadFrames = 0u) {
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = normal_attack_haptics::Message;
    message[1] = static_cast<uint8_t>(kind);
    message[2] = static_cast<uint8_t>(operation);
    std::memcpy(message.data() + 4, &swingLeadFrames, sizeof(swingLeadFrames));
    std::memcpy(message.data() + 8, &secondarySwingLeadFrames, sizeof(secondarySwingLeadFrames));
    return SendReportLocked(message);
}

bool SendOniChargeReleaseHaptic(normal_attack_haptics::Kind kind,
    oni_charge_release_haptics::Kind overrideKind =
        oni_charge_release_haptics::Kind::None) {
    const auto releaseKind = overrideKind != oni_charge_release_haptics::Kind::None
        ? overrideKind
        : (kind == normal_attack_haptics::Kind::ChargeHeavy
            ? oni_charge_release_haptics::Kind::Heavy
            : oni_charge_release_haptics::Kind::Light);
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = oni_charge_release_haptics::Message;
    message[1] = static_cast<uint8_t>(releaseKind);
    return SendReportLocked(message);
}

bool SendBowShotHaptic(bow_shot_haptics::Kind kind) {
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = bow_shot_haptics::Message;
    message[1] = static_cast<uint8_t>(kind);
    return SendReportLocked(message);
}

void CancelNormalAttackHaptics() {
    {
        std::lock_guard lock{g_normalAttackMutex};
        g_normalAttackTracker.Clear();
        g_oniChargeAttackTracker.Clear();
        g_ordinaryChargeAttackTracker.Clear();
    }
    {
        std::lock_guard lock{g_ordinaryLightWaveMutex};
        g_ordinaryLightWaveAction = 0;
        g_ordinaryLightWaveStage = 0;
        g_ordinaryLightWaveTick = 0;
    }
    {
        std::lock_guard lock{g_ordinaryHeavyImpactMutex};
        g_ordinaryHeavyImpactAction = 0;
        g_ordinaryHeavyImpactStage = 0;
        g_ordinaryHeavyDownstrokeTick = 0;
    }
    SendNormalAttackHaptic(normal_attack_haptics::Kind::None, normal_attack_haptics::Operation::Cancel);
}

bool StopMotorHapticsLocked(bool preserveTreasureChest = false) {
    g_liveMotorSources.Clear();
    g_sentMotorPower.fill(-1.0f);
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticStopMotorMessage;
    message[1] = preserveTreasureChest ? 1u : 0u;
    return SendReportLocked(message);
}

bool StopWaveformsLocked() {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticStopWaveformsMessage;
    return SendReportLocked(message);
}

float FloatArgument(void* argument) {
    const uint32_t bits = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(argument));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool SendMotorPowerLocked(uint32_t motor, float power) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticMotorPowerMessage;
    std::memcpy(message.data() + 4, &motor, sizeof(motor));
    std::memcpy(message.data() + 8, &power, sizeof(power));
    return SendReportLocked(message);
}

void ClearLiveMotorStateLocked(const char* reason) {
    // Clear ONLY live targets. Menu/timed pulses and authored textures are
    // independent and must not be erased by a polled pause transition.
    g_liveMotorSources.Clear();
    g_sentMotorPower.fill(-1.0f);
    for (uint32_t motor = 0; motor < 4; ++motor) {
        if (SendMotorPowerLocked(motor, 0.0f)) g_sentMotorPower[motor] = 0.0f;
    }
    API::get()->log_info("[CodexDualSenseBridge] liveMotor cache cleared reason=%s revision=%s",
        reason, live_motor_state::Revision);
}

bool SendEnvironmentLocked(float rainLevel, float windLevel) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticEnvironmentMessage;
    std::memcpy(message.data() + 4, &rainLevel, sizeof(rainLevel));
    std::memcpy(message.data() + 8, &windLevel, sizeof(windLevel));
    return SendReportLocked(message);
}

bool SendEnvironmentInteractionLocked(
    EnvironmentInteractionProfile profile,
    float intensity) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticEnvironmentInteractionMessage;
    message[1] = static_cast<uint8_t>(profile);
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    std::memcpy(message.data() + 4, &intensity, sizeof(intensity));
    return SendReportLocked(message);
}

bool SendTreasureChestMotorFallbackLocked(
    EnvironmentInteractionProfile profile,
    float intensity);
void ArmTreasureChestNativeMotorFallback(
    EnvironmentInteractionProfile profile,
    float intensity);

void PlayEnvironmentInteractionHaptic(
    EnvironmentInteractionProfile profile,
    float intensity,
    const char* source,
    bool logEvent = true) {
    const bool treasureChestProfile =
        profile == EnvironmentInteractionProfile::TreasureChestOpen ||
        profile == EnvironmentInteractionProfile::TreasureChestPush ||
        profile == EnvironmentInteractionProfile::TreasureChestLidLand;
    bool sent = false;
    bool motorFallback = false;
    {
        std::lock_guard lock{g_mutex};
        sent = SendEnvironmentInteractionLocked(profile, intensity);
        motorFallback = treasureChestProfile &&
            SendTreasureChestMotorFallbackLocked(profile, intensity);
    }
    if (treasureChestProfile && UseTreasureChestLegacyMotorFallback) {
        // The helper path is retained for authored PCM texture, while this
        // second path calls the game's own GamePadDevice motor setter and is
        // refreshed from OnPresent for the duration of this stage.
        ArmTreasureChestNativeMotorFallback(profile, intensity);
    }
    if (logEvent) {
        API::get()->log_info(
            "[CodexDualSenseBridge] environmentInteraction profile=%u source=%s write=%s",
            static_cast<unsigned>(profile),
            source != nullptr ? source : "unknown",
            sent ? "ok" : "failed");
    }
    if (treasureChestProfile) {
        API::get()->log_info(
            "[CodexDualSenseBridge] treasureChestMotorFallback profile=%u mode=%s write=%s revision=chest-motor-r2",
            static_cast<unsigned>(profile),
            UseTreasureChestLegacyMotorFallback ? "legacy+pcm" : "pcm-only",
            UseTreasureChestLegacyMotorFallback
                ? (motorFallback ? "ok" : "failed")
                : "not-sent");
    }
}

bool SendFootstepLocked(
    uint32_t surface,
    uint32_t side,
    uint32_t gait,
    uint32_t stepType,
    float pace) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticFootstepMessage;
    std::memcpy(message.data() + 4, &surface, sizeof(surface));
    std::memcpy(message.data() + 8, &side, sizeof(side));
    std::memcpy(message.data() + 12, &gait, sizeof(gait));
    std::memcpy(message.data() + 16, &stepType, sizeof(stepType));
    std::memcpy(message.data() + 20, &pace, sizeof(pace));
    return SendReportLocked(message);
}

bool SendMapZoomLocked(float direction, float intensity) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticMapZoomMessage;
    std::memcpy(message.data() + 4, &direction, sizeof(direction));
    std::memcpy(message.data() + 8, &intensity, sizeof(intensity));
    return SendReportLocked(message);
}

bool SendLongPressLocked(
    uint8_t operation,
    float progress,
    uint8_t profile = 0) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticLongPressMessage;
    message[1] = operation;
    message[2] = profile;
    progress = std::clamp(progress, 0.0f, 1.0f);
    std::memcpy(message.data() + 4, &progress, sizeof(progress));
    return SendReportLocked(message);
}

void SendLongPressFeedback(
    uint8_t operation,
    float progress,
    uint8_t profile = 0) {
    std::lock_guard lock{g_mutex};
    SendLongPressLocked(operation, progress, profile);
}

void PlayOniStrengthHaptic(uint8_t profile) {
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticOniStrengthMessage;
    message[1] = profile;
    SendReportLocked(message);
}

void PlayOniEyeHaptic() {
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticOniEyeMessage;
    SendReportLocked(message);
}

void PlayOniTransformationHaptic() {
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = oni_transformation_haptics::Message;
    SendReportLocked(message);
}

bool SendWallRunHaptic(wall_run_haptics::Event event,
    wall_run_haptics::Side side = wall_run_haptics::Side::Center) {
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = wall_run_haptics::Message;
    message[1] = static_cast<uint8_t>(event);
    message[2] = static_cast<uint8_t>(side);
    const float intensity = event == wall_run_haptics::Event::Stop ||
        event == wall_run_haptics::Event::Detach ? 0.0f : 1.0f;
    std::memcpy(message.data() + 4, &intensity, sizeof(intensity));
    const uint32_t impulseFrames = wall_run_haptics::ImpulseFrames(event);
    if (impulseFrames != 0) {
        g_wallRunImpulseUntilTick.store(GetTickCount64() +
            impulseFrames * 1000u / wall_run_haptics::SampleRate,
            std::memory_order_relaxed);
    }
    return SendReportLocked(message);
}

void StopWallRunHaptics() {
    {
        std::lock_guard lock{g_wallRunLandingMutex};
        g_wallRunLandingTracker.Reset();
    }
    const bool active = g_wallRunHapticActive.exchange(false, std::memory_order_relaxed);
    g_wallRunAlongWallActive.store(false, std::memory_order_relaxed);
    const ULONGLONG impulse = g_wallRunImpulseUntilTick.exchange(0, std::memory_order_relaxed);
    g_wallRunActionObject.store(0, std::memory_order_relaxed);
    g_lastWallRunStateTick.store(0, std::memory_order_relaxed);
    g_lastWallRunRefreshTick.store(0, std::memory_order_relaxed);
    if (active || impulse != 0) SendWallRunHaptic(wall_run_haptics::Event::Stop);
}

bool SendContinuousPlayerHapticLocked(
    ContinuousPlayerHaptic profile,
    ContinuousHapticOperation operation,
    float intensity,
    ladder_haptics::Side side = ladder_haptics::Side::Center);

bool SendLadderHaptic(
    ladder_haptics::Profile profile,
    ladder_haptics::Operation operation,
    ladder_haptics::Side side,
    float intensity = 1.0f) {
    std::lock_guard lock{g_mutex};
    const bool sent = SendContinuousPlayerHapticLocked(
        static_cast<ContinuousPlayerHaptic>(profile),
        static_cast<ContinuousHapticOperation>(operation),
        intensity,
        side);
    return sent;
}

void StopLadderHaptics() {
    const bool active = g_ladderHapticActive.exchange(
        false, std::memory_order_relaxed);
    const uint64_t stateTick = g_lastLadderStateTick.exchange(
        0, std::memory_order_relaxed);
    g_lastLadderRefreshTick.store(0, std::memory_order_relaxed);
    g_ladderActionObject.store(0, std::memory_order_relaxed);
    if (!active && stateTick == 0) return;

    const auto profile = static_cast<ladder_haptics::Profile>(
        g_ladderHapticProfile.load(std::memory_order_relaxed));
    const bool sent = SendLadderHaptic(
        profile,
        ladder_haptics::Operation::Stop,
        ladder_haptics::Side::Center,
        0.0f);
    API::get()->log_info(
        "[CodexDualSenseBridge] ladderHaptic stop write=%s",
        sent ? "ok" : "failed");
}

void PlayCombatHapticProfile(
    CombatHapticProfile profile,
    uint8_t motionVariant = 0u) {
    if (profile == CombatHapticProfile::Parry ||
        profile == CombatHapticProfile::ProjectileParry) {
        const ULONGLONG now = GetTickCount64();
        g_repelStandardRumbleSuppressionUntil.store(
            now + 320, std::memory_order_release);
        g_repelStandardRumbleSuppressedCount.store(0, std::memory_order_relaxed);
        // Stop a motor value left active by the frame immediately before the
        // confirmed result. Subsequent non-zero writes are filtered below.
        if (g_originalXInputSetState != nullptr) {
            XINPUT_VIBRATION zero{};
            for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
                g_originalXInputSetState(user, &zero);
            }
        }
        API::get()->log_info(
            "[CodexDualSenseBridge] Repel standard-rumble isolation armed profile=%u duration=320ms",
            static_cast<unsigned>(profile));
    } else {
        const auto executionKind = execution_haptics::KindFromProfile(
            static_cast<uint8_t>(profile));
        if (executionKind != execution_haptics::Kind::None) {
            const ULONGLONG now = GetTickCount64();
            g_executionStandardRumbleSuppressionUntil.store(
                now + execution_haptics::SuppressionMs(executionKind),
                std::memory_order_release);
            g_executionStandardRumbleSuppressedCount.store(
                0, std::memory_order_relaxed);
            if (g_originalXInputSetState != nullptr) {
                XINPUT_VIBRATION zero{};
                for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user)
                    g_originalXInputSetState(user, &zero);
            }
            API::get()->log_info(
                "[CodexDualSenseBridge] Execution standard-rumble isolation armed profile=%u duration=%ums",
                static_cast<unsigned>(profile),
                execution_haptics::SuppressionMs(executionKind));
        }
    }
    std::lock_guard lock{g_mutex};
    if (profile == CombatHapticProfile::Parry ||
        profile == CombatHapticProfile::ProjectileParry ||
        execution_haptics::KindFromProfile(static_cast<uint8_t>(profile)) !=
            execution_haptics::Kind::None) {
        // The helper clears generic motors for the authored isolation window;
        // its sender cache must not resurrect that old value on a heartbeat.
        ClearLiveMotorStateLocked("combat-isolation");
    }
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticCombatActionMessage;
    message[1] = static_cast<uint8_t>(profile);
    message[2] = static_cast<uint8_t>(motionVariant & 0x03u);
    const bool sent = SendReportLocked(message);
    API::get()->log_info(
        "[CodexDualSenseBridge] combatHaptic profile=%u variant=%u write=%s",
        static_cast<unsigned>(profile),
        static_cast<unsigned>(motionVariant & 0x03u),
        sent ? "ok" : "failed");
}

void PlayCombatActionHaptic(ControllerSpeakerCue cue) {
    CombatHapticProfile profile{};
    switch (cue) {
    case ControllerSpeakerCue::CombatGuard:
    case ControllerSpeakerCue::CombatGuardDirectional:
        profile = CombatHapticProfile::Guard;
        break;
    case ControllerSpeakerCue::CombatDeflect:
        // A Deflect cue must never select a Repel haptic profile: the helper
        // would then automatically pair the wrong cue 12/13 speaker clip.
        profile = static_cast<CombatHapticProfile>(
            combat_defense_feedback::HapticProfile(combat_defense_feedback::Kind::Deflect));
        break;
    case ControllerSpeakerCue::CombatParryBlade:
        profile = CombatHapticProfile::Parry;
        break;
    case ControllerSpeakerCue::CombatParryProjectile:
        profile = CombatHapticProfile::ProjectileParry;
        break;
    case ControllerSpeakerCue::CombatIssen:
        profile = CombatHapticProfile::Issen;
        break;
    case ControllerSpeakerCue::CombatExecution: {
        // Execution haptics are dispatched from the concrete attack action
        // before speaker debounce. Setup states can emit the same speaker cue.
        return;
    }
    default:
        return;
    }

    PlayCombatHapticProfile(profile);
}

bool SendPlayerReactionLocked(PlayerHapticProfile profile, float intensity,
    float durationSeconds = 0.0f) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticPlayerReactionMessage;
    const uint32_t profileValue = static_cast<uint32_t>(profile);
    std::memcpy(message.data() + 4, &profileValue, sizeof(profileValue));
    std::memcpy(message.data() + 8, &intensity, sizeof(intensity));
    std::memcpy(message.data() + 12, &durationSeconds, sizeof(durationSeconds));
    return SendReportLocked(message);
}

bool SendControllerSpeakerCueLocked(ControllerSpeakerCue cue,
    combat_defense_feedback::DeflectSpeakerVariant variant =
        combat_defense_feedback::DeflectSpeakerVariant::Short,
    bool promoteExisting = false) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = ControllerSpeakerMessage;
    if (cue == ControllerSpeakerCue::CombatDeflect) {
        message[1] = static_cast<uint8_t>(variant);
        message[2] = promoteExisting ? 1u : 0u;
    }
    const uint32_t cueValue = static_cast<uint32_t>(cue);
    std::memcpy(message.data() + 4, &cueValue, sizeof(cueValue));
    return SendReportLocked(message);
}

void PlayControllerSpeakerCue(ControllerSpeakerCue cue,
    combat_defense_feedback::DeflectSpeakerVariant variant =
        combat_defense_feedback::DeflectSpeakerVariant::Short) {
    std::lock_guard lock{g_mutex};
    const bool sent = SendControllerSpeakerCueLocked(cue, variant);
    API::get()->log_info(
        "[CodexDualSenseBridge] controllerSpeaker cue=%u variant=%u write=%s",
        static_cast<uint32_t>(cue),
        static_cast<unsigned>(variant),
        sent ? "ok" : "failed");
}

bool SendGauntletVoiceLocked(uint8_t operation, uint32_t triggerId) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = GauntletVoiceMessage;
    message[1] = operation;
    std::memcpy(message.data() + 4, &triggerId, sizeof(triggerId));
    return SendReportLocked(message);
}

void PlayGauntletVoice(uint32_t triggerId) {
    std::lock_guard lock{g_mutex};
    const bool sent = SendGauntletVoiceLocked(1, triggerId);
    API::get()->log_info(
        "[CodexDualSenseBridge] gauntletVoice trigger=%u write=%s",
        triggerId,
        sent ? "ok" : "failed");
}

void StopGauntletVoice() {
    std::lock_guard lock{g_mutex};
    const bool sent = SendGauntletVoiceLocked(2, 0);
    API::get()->log_info(
        "[CodexDualSenseBridge] gauntletVoice stop write=%s",
        sent ? "ok" : "failed");
}

void SetSoulSpeakerState(bool enabled) {
    const bool previous = g_soulSpeakerActive.exchange(
        enabled, std::memory_order_relaxed);
    if (previous != enabled) {
        PlayControllerSpeakerCue(enabled
            ? ControllerSpeakerCue::SoulStart
            : ControllerSpeakerCue::SoulStop);
    }
}

bool SendContinuousPlayerHapticLocked(
    ContinuousPlayerHaptic profile,
    ContinuousHapticOperation operation,
    float intensity,
    ladder_haptics::Side side) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticContinuousPlayerMessage;
    message[1] = static_cast<uint8_t>(profile);
    message[2] = static_cast<uint8_t>(operation);
    message[3] = static_cast<uint8_t>(side);
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    std::memcpy(message.data() + 4, &intensity, sizeof(intensity));
    return SendReportLocked(message);
}

void SetSoulSearchHaptic(bool enabled) {
    if (enabled && g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        enabled = false;
    }
    if (enabled && (g_soulHapticActive.load(std::memory_order_relaxed) ||
        g_blackSoulHapticActive.load(std::memory_order_relaxed))) {
        enabled = false;
    }
    const bool previous = g_soulSearchHapticActive.exchange(
        enabled, std::memory_order_relaxed);
    if (previous == enabled) return;

    if (enabled) {
        g_lastSoulSearchRefreshTick.store(
            GetTickCount64(), std::memory_order_relaxed);
    } else {
        g_lastSoulSearchRefreshTick.store(0, std::memory_order_relaxed);
    }
    std::lock_guard lock{g_mutex};
    const bool sent = SendContinuousPlayerHapticLocked(
        ContinuousPlayerHaptic::SoulSearch,
        enabled
            ? ContinuousHapticOperation::Start
            : ContinuousHapticOperation::Stop,
        enabled ? 1.0f : 0.0f);
    API::get()->log_info(
        "[CodexDualSenseBridge] soulSearchHaptic active=%d write=%s",
        enabled ? 1 : 0,
        sent ? "ok" : "failed");
}

void SetSoulAbsorptionHaptic(bool enabled) {
    if (enabled && g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        enabled = false;
    }
    if (enabled) {
        // Never layer the empty-action texture under a real soul stream.
        SetSoulSearchHaptic(false);
        g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
    } else if (g_soulActionActive.load(std::memory_order_relaxed)) {
        // If the action remains held after the last soul arrives, wait briefly
        // before returning to the empty-search texture.
        g_soulSearchCandidateTick.store(
            GetTickCount64(), std::memory_order_relaxed);
    }
    const bool previous = g_soulHapticActive.exchange(
        enabled, std::memory_order_relaxed);
    if (previous == enabled) return;

    if (enabled) {
        g_lastSoulHapticRefreshTick.store(
            GetTickCount64(), std::memory_order_relaxed);
    } else {
        g_lastSoulHapticRefreshTick.store(0, std::memory_order_relaxed);
    }
    std::lock_guard lock{g_mutex};
    const bool sent = SendContinuousPlayerHapticLocked(
        ContinuousPlayerHaptic::SoulAbsorption,
        enabled
            ? ContinuousHapticOperation::Start
            : ContinuousHapticOperation::Stop,
        enabled ? 1.0f : 0.0f);
    API::get()->log_info(
        "[CodexDualSenseBridge] soulAbsorptionHaptic active=%d write=%s",
        enabled ? 1 : 0,
        sent ? "ok" : "failed");
}

void PlaySoulAbsorptionAccent() {
    if (!g_soulHapticActive.load(std::memory_order_relaxed)) return;
    std::lock_guard lock{g_mutex};
    SendContinuousPlayerHapticLocked(
        ContinuousPlayerHaptic::SoulAbsorption,
        ContinuousHapticOperation::Accent,
        1.0f);
}

bool BeginEvent(
    std::atomic<ULONGLONG>& lastTick,
    ULONGLONG minimumIntervalMs);

void PlaySprintStartFeedback(float intensity) {
    if (!BeginEvent(g_lastSprintButtonTick, 180)) return;
    std::lock_guard lock{g_mutex};
    const bool sent = SendContinuousPlayerHapticLocked(
        ContinuousPlayerHaptic::Sprint,
        ContinuousHapticOperation::Accent,
        intensity);
    API::get()->log_info(
        "[CodexDualSenseBridge] sprintButtonFeedback mode=start intensity=%.2f write=%s",
        intensity,
        sent ? "ok" : "failed");
}

void PlaySprintSlowdownFeedback() {
    std::lock_guard lock{g_mutex};
    const bool sent = SendContinuousPlayerHapticLocked(
        ContinuousPlayerHaptic::Sprint,
        ContinuousHapticOperation::Decelerate,
        0.38f);
    API::get()->log_info(
        "[CodexDualSenseBridge] sprintButtonFeedback mode=slow intensity=0.38 write=%s",
        sent ? "ok" : "failed");
}

void UpdateSoulActionInputState(DWORD userIndex, bool held) {
    if (userIndex >= XUSER_MAX_COUNT) return;
    const uint32_t userBit = 1u << userIndex;
    if (!held) {
        g_soulSearchSuppressedUserMask.fetch_and(
            ~userBit, std::memory_order_relaxed);
    }
    const bool previous = g_soulActionInputHeld[userIndex].exchange(
        held, std::memory_order_relaxed);
    if (previous == held) return;

    bool anyHeld = false;
    for (const auto& inputHeld : g_soulActionInputHeld) {
        if (inputHeld.load(std::memory_order_relaxed)) {
            anyHeld = true;
            break;
        }
    }
    const bool previousAggregate = g_soulActionActive.exchange(
        anyHeld, std::memory_order_relaxed);
    if (previousAggregate == anyHeld) return;

    if (anyHeld) {
        g_bowTriggerSuppressedByLeftTrigger.store(
            true, std::memory_order_relaxed);
        g_bowPullStartedTick.store(0, std::memory_order_relaxed);
        {
            std::lock_guard lock{g_mutex};
            g_activeEffects &= ~BowEffectMask;
            RefreshEffectLocked();
        }
        g_soulSearchCandidateTick.store(
            GetTickCount64(), std::memory_order_relaxed);
    } else {
        g_bowTriggerSuppressedByLeftTrigger.store(
            false, std::memory_order_relaxed);
        // Force the next presentation tick to poll immediately and restore the
        // idle bow effect if the bow is still selected.
        g_lastBowEquippedPollTick = 0;
        // The successful-absorption callback is the authoritative start of a
        // real soul stream on this PC build.  Keep its trigger effect stable
        // for the held action, then clear it exactly once on physical release.
        SetEffectState(AbsorbEffect, false);
        SetSoulAbsorptionHaptic(false);
        SetSoulSpeakerState(false);
        g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
        SetSoulSearchHaptic(false);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] soulAbsorptionInput source=xinput active=%d bowTriggerSuppressed=%d",
        anyHeld ? 1 : 0,
        anyHeld ? 1 : 0);
}

void SuppressSoulSearchForHeldInputs() {
    uint32_t heldMask = 0;
    for (DWORD userIndex = 0; userIndex < XUSER_MAX_COUNT; ++userIndex) {
        if (g_soulActionInputHeld[userIndex].load(std::memory_order_relaxed)) {
            heldMask |= 1u << userIndex;
        }
    }
    if (heldMask == 0) return;

    g_soulSearchSuppressedUserMask.fetch_or(
        heldMask, std::memory_order_relaxed);
    g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
    SetSoulSearchHaptic(false);
    API::get()->log_info(
        "[CodexDualSenseBridge] emptySoulHaptic suppressedUntilRelease userMask=0x%X",
        heldMask);
}

void StopSoulStreamFeedback(bool suppressUntilTriggerRelease) {
    if (suppressUntilTriggerRelease) {
        // Once the last real soul has arrived, do not fall straight back to
        // the empty-search texture while L2 is still physically held.
        SuppressSoulSearchForHeldInputs();
    }
    g_lastSoulFlowEvidenceTick.store(0, std::memory_order_relaxed);
    SetEffectState(AbsorbEffect, false);
    SetSoulAbsorptionHaptic(false);
    SetSoulSpeakerState(false);
}

void RefreshGameplayHapticsPauseState() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastHapticPausingPollTick < 40) return;
    g_lastHapticPausingPollTick = now;

    const std::vector<void*> noArguments;
    bool paused = false;
    bool stateAvailable = false;
    const char* source = "unavailable";
    if (g_menuPauseGetter != nullptr || g_anyPauseGetter != nullptr) {
        auto* pauseManager = API::get()->get_managed_singleton(
            "app.PauseManager");
        if (pauseManager != nullptr) {
            if (g_menuPauseGetter != nullptr) {
                const reframework::InvokeRet result =
                    g_menuPauseGetter->invoke(pauseManager, noArguments);
                if (!result.exception_thrown) {
                    paused = result.byte != 0;
                    stateAvailable = true;
                }
            }
            if (g_anyPauseGetter != nullptr) {
                const reframework::InvokeRet result =
                    g_anyPauseGetter->invoke(pauseManager, noArguments);
                if (!result.exception_thrown) {
                    paused = paused || result.byte != 0;
                    stateAvailable = true;
                }
            }
            source = "PauseManager";
        }
    }
    if (!stateAvailable && g_hapticPausingGetter != nullptr) {
        const reframework::InvokeRet result =
            g_hapticPausingGetter->invoke(nullptr, noArguments);
        if (!result.exception_thrown) {
            paused = result.byte != 0;
            stateAvailable = true;
            source = "HDVibrationComponentManager";
        }
    }
    if (!stateAvailable) return;

    const bool previous = g_gameplayHapticsPaused.exchange(
        paused, std::memory_order_relaxed);
    if (paused == previous) return;

    API::get()->log_info(
        "[CodexDualSenseBridge] gameplayHaptics paused=%d source=%s",
        paused ? 1 : 0,
        source);
    if (paused) {
        CancelNormalAttackHaptics();
        g_executionStandardRumbleSuppressionUntil.store(0, std::memory_order_relaxed);
        StopWallRunHaptics();
        StopLadderHaptics();
        g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
        SetSoulSearchHaptic(false);
        StopSoulStreamFeedback(false);
        std::lock_guard lock{g_mutex};
        ClearLiveMotorStateLocked("pause-poll");
    }
}

void UpdateBowEquippedPrearm(API::ManagedObject* subWeapon) {
    int32_t subWeaponId = -1;
    int enabledValue = -1;
    int bowDisabledValue = -1;
    int bowDisableTimedOutValue = -1;
    int bowSafeAreaValue = -1;
    bool bowAvailable = false;
    if (!g_gameplayHapticsPaused.load(std::memory_order_relaxed) &&
        !g_bowTriggerSuppressedByLeftTrigger.load(std::memory_order_relaxed) &&
        g_subWeaponIdGetter != nullptr && subWeapon != nullptr) {
        const std::vector<void*> noArguments;
        const auto idResult = g_subWeaponIdGetter->invoke(
            subWeapon, noArguments);
        if (!idResult.exception_thrown) {
            subWeaponId = static_cast<int32_t>(idResult.dword);
        }
        if (g_subWeaponEnableGetter != nullptr) {
            const auto enableResult = g_subWeaponEnableGetter->invoke(
                subWeapon, noArguments);
            if (!enableResult.exception_thrown) {
                enabledValue = enableResult.byte != 0 ? 1 : 0;
            }
        }
        auto* selector = reinterpret_cast<API::ManagedObject*>(
            g_playerActionSelector.load(std::memory_order_relaxed));
        if (selector != nullptr && g_bowDisableTimerEnabledGetter != nullptr &&
            g_bowDisableTimerIsTimeOutGetter != nullptr) {
            if (auto* timerField = selector->get_field<API::ManagedObject*>(
                    "_DisableBowTimer"); timerField != nullptr && *timerField != nullptr) {
                const auto enabledResult = g_bowDisableTimerEnabledGetter->invoke(
                    *timerField, noArguments);
                const auto timeOutResult = g_bowDisableTimerIsTimeOutGetter->invoke(
                    *timerField, noArguments);
                if (!enabledResult.exception_thrown &&
                    !timeOutResult.exception_thrown) {
                    const bool timerEnabled = enabledResult.byte != 0;
                    const bool timedOut = timeOutResult.byte != 0;
                    bowDisableTimedOutValue = timedOut ? 1 : 0;
                    bowDisabledValue = timerEnabled && !timedOut ? 1 : 0;
                }
            }
        }
        // PlayerSubWeaponDef.ID: NONE=0, BOW=1.  The game's own
        // _DisableBowTimer is authoritative for traversal/scripted states that
        // temporarily reject draw input. This keeps R2 pre-armed before the
        // first press without enabling it in a state where the bow cannot be
        // drawn.
        auto* entity = reinterpret_cast<API::ManagedObject*>(
            g_playerCharacterEntity.load(std::memory_order_relaxed));
        if (entity != nullptr && g_currentZoneSafeAreaGetter != nullptr) {
            const auto normalSafeResult = g_currentZoneSafeAreaGetter->invoke(
                entity, std::vector<void*>{reinterpret_cast<void*>(0)});
            const auto mysterySafeResult = g_currentZoneSafeAreaGetter->invoke(
                entity, std::vector<void*>{reinterpret_cast<void*>(1)});
            if (!normalSafeResult.exception_thrown &&
                !mysterySafeResult.exception_thrown) {
                bowSafeAreaValue = normalSafeResult.byte != 0 ||
                    mysterySafeResult.byte != 0 ? 1 : 0;
            }
        }
        bowAvailable = subWeaponId == 1 && enabledValue == 1 &&
            bowDisabledValue == 0 && bowSafeAreaValue == 0;
    }

    if (subWeaponId != g_lastLoggedSubWeaponId ||
        enabledValue != g_lastLoggedSubWeaponEnable ||
        bowDisabledValue != g_lastLoggedBowDisabled ||
        bowSafeAreaValue != g_lastLoggedBowSafeArea) {
        g_lastLoggedSubWeaponId = subWeaponId;
        g_lastLoggedSubWeaponEnable = enabledValue;
        g_lastLoggedBowDisabled = bowDisabledValue;
        g_lastLoggedBowSafeArea = bowSafeAreaValue;
        API::get()->log_info(
            "[CodexDualSenseBridge] bowAvailability subWeaponId=%d isEnable=%d disabled=%d timedOut=%d safeArea=%d armed=%d paused=%d",
            subWeaponId,
            enabledValue,
            bowDisabledValue,
            bowDisableTimedOutValue,
            bowSafeAreaValue,
            bowAvailable ? 1 : 0,
            g_gameplayHapticsPaused.load(std::memory_order_relaxed) ? 1 : 0);
    }
    SetEffectState(BowEquippedEffect, bowAvailable);
}

void RefreshBowEquippedPrearm() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastBowEquippedPollTick < 50) return;
    g_lastBowEquippedPollTick = now;

    API::ManagedObject* subWeapon = nullptr;
    auto* entity = reinterpret_cast<API::ManagedObject*>(
        g_playerCharacterEntity.load(std::memory_order_relaxed));
    if (entity != nullptr) {
        auto* subWeaponField = entity->get_field<API::ManagedObject*>(
            "<SubWeapon>k__BackingField");
        if (subWeaponField != nullptr) subWeapon = *subWeaponField;
    }
    UpdateBowEquippedPrearm(subWeapon);
}

void SetBlackSoulAbsorptionHaptic(bool enabled, bool completed = false) {
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        enabled = false;
        completed = false;
    }
    const bool previous = g_blackSoulHapticActive.exchange(
        enabled, std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    completed = completed && previous && !enabled;
    const ULONGLONG previousFinish = g_blackSoulFinishUntilTick.exchange(
        completed ? now + black_soul_haptics::FinishDurationMs : 0,
        std::memory_order_relaxed);
    const ULONGLONG refreshed = g_lastBlackSoulRefreshTick.load(
        std::memory_order_relaxed);
    if (previous == enabled && (!enabled || now - refreshed < 200) &&
        (enabled || previousFinish == 0)) return;
    if (enabled) SetSoulSearchHaptic(false);
    g_lastBlackSoulRefreshTick.store(enabled ? now : 0,
        std::memory_order_relaxed);
    std::lock_guard lock{g_mutex};
    const bool sent = SendContinuousPlayerHapticLocked(
        ContinuousPlayerHaptic::BlackSoulAbsorption,
        !enabled ? (completed ? ContinuousHapticOperation::Accent :
            ContinuousHapticOperation::Stop) :
            (previous ? ContinuousHapticOperation::Refresh :
                ContinuousHapticOperation::Start),
        (enabled || completed) ? 1.0f : 0.0f);
    if (previous != enabled) {
        API::get()->log_info(
            "[CodexDualSenseBridge] blackSoulAbsorptionHaptic active=%d finish=%d write=%s",
            enabled ? 1 : 0, completed ? 1 : 0, sent ? "ok" : "failed");
    }
}

void RefreshContinuousPlayerHaptics() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG wallImpulseUntil = g_wallRunImpulseUntilTick.load(std::memory_order_relaxed);
    if (wallImpulseUntil != 0 && now >= wallImpulseUntil)
        g_wallRunImpulseUntilTick.store(0, std::memory_order_relaxed);
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        StopWallRunHaptics();
    } else if (g_wallRunHapticActive.load(std::memory_order_relaxed) &&
        !wall_run_haptics::EvidenceFresh(now,
            g_lastWallRunStateTick.load(std::memory_order_relaxed))) {
        StopWallRunHaptics();
    }
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        StopLadderHaptics();
    } else if (g_ladderHapticActive.load(std::memory_order_relaxed) &&
        !ladder_haptics::EvidenceFresh(now,
            g_lastLadderStateTick.load(std::memory_order_relaxed))) {
        StopLadderHaptics();
    }
    bool wallLandingFallback = false;
    {
        std::lock_guard lock{g_wallRunLandingMutex};
        if (!g_gameplayHapticsPaused.load(std::memory_order_relaxed))
            wallLandingFallback = g_wallRunLandingTracker.PublishFallback(now);
    }
    if (wallLandingFallback && !g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        g_lastWallRunLandTick.store(now, std::memory_order_relaxed);
        const bool sent = SendWallRunHaptic(wall_run_haptics::Event::LandSmall);
        API::get()->log_info("[CodexDualSenseBridge] wallRun landingFallback source=firstFootContact event=%u write=%s",
            static_cast<unsigned>(wall_run_haptics::Event::LandSmall), sent ? "ok" : "failed");
    }
    const ULONGLONG finishUntil = g_blackSoulFinishUntilTick.load(
        std::memory_order_relaxed);
    if (finishUntil != 0 && now >= finishUntil) {
        g_blackSoulFinishUntilTick.store(0, std::memory_order_relaxed);
    }
    if (finishUntil != 0 &&
        g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        SetBlackSoulAbsorptionHaptic(false);
    }
    if (g_blackSoulHapticActive.load(std::memory_order_relaxed) &&
        (g_gameplayHapticsPaused.load(std::memory_order_relaxed) ||
         !black_soul_haptics::EvidenceFresh(now,
            g_lastBlackSoulStateTick.load(std::memory_order_relaxed)))) {
        SetBlackSoulAbsorptionHaptic(false);
    }
    if (g_soulHapticActive.load(std::memory_order_relaxed)) {
        // executeSoulAbsorptionSuccess arrives 20-200 ms apart while a real
        // stream is active in the tested PC build.  A quiet window marks the
        // end even if the player keeps L2 held, which the action input alone
        // cannot distinguish from an empty absorption pose.
        constexpr ULONGLONG SoulFlowEndWindowMs = 420;
        const ULONGLONG evidenceTick = g_lastSoulFlowEvidenceTick.load(
            std::memory_order_relaxed);
        if (g_gameplayHapticsPaused.load(std::memory_order_relaxed) ||
            !g_soulActionActive.load(std::memory_order_relaxed) ||
            evidenceTick == 0 || now < evidenceTick ||
            now - evidenceTick >= SoulFlowEndWindowMs) {
            StopSoulStreamFeedback(true);
        }
    }

    if (g_soulHapticActive.load(std::memory_order_relaxed)) {
        const ULONGLONG previous = g_lastSoulHapticRefreshTick.load(
            std::memory_order_relaxed);
        if (previous == 0 || now - previous >= 200) {
            g_lastSoulHapticRefreshTick.store(now, std::memory_order_relaxed);
            std::lock_guard lock{g_mutex};
            SendContinuousPlayerHapticLocked(
                ContinuousPlayerHaptic::SoulAbsorption,
                ContinuousHapticOperation::Refresh,
                1.0f);
        }
    }

    const bool searching =
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed) &&
        g_soulActionActive.load(std::memory_order_relaxed) &&
        !g_soulHapticActive.load(std::memory_order_relaxed) &&
        !g_blackSoulHapticActive.load(std::memory_order_relaxed) &&
        g_soulSearchSuppressedUserMask.load(std::memory_order_relaxed) == 0;
    if (searching) {
        ULONGLONG candidate = g_soulSearchCandidateTick.load(
            std::memory_order_relaxed);
        if (candidate == 0) {
            candidate = now;
            g_soulSearchCandidateTick.store(
                candidate, std::memory_order_relaxed);
        }
        if (now - candidate >= 140) {
            if (!g_soulSearchHapticActive.load(std::memory_order_relaxed)) {
                SetSoulSearchHaptic(true);
            } else {
                const ULONGLONG previous = g_lastSoulSearchRefreshTick.load(
                    std::memory_order_relaxed);
                if (previous == 0 || now - previous >= 200) {
                    g_lastSoulSearchRefreshTick.store(
                        now, std::memory_order_relaxed);
                    std::lock_guard lock{g_mutex};
                    SendContinuousPlayerHapticLocked(
                        ContinuousPlayerHaptic::SoulSearch,
                        ContinuousHapticOperation::Refresh,
                        1.0f);
                }
            }
        }
    } else {
        g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
        SetSoulSearchHaptic(false);
    }

}

bool SendMotorPulseLocked(
    uint32_t motor,
    float power,
    float duration,
    bool attenuate,
    int32_t easeType) {
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticMotorPulseMessage;
    std::memcpy(message.data() + 4, &motor, sizeof(motor));
    std::memcpy(message.data() + 8, &power, sizeof(power));
    std::memcpy(message.data() + 12, &duration, sizeof(duration));
    message[16] = attenuate ? 1 : 0;
    std::memcpy(message.data() + 20, &easeType, sizeof(easeType));
    return SendReportLocked(message);
}

bool SendTreasureChestMotorFallbackLocked(
    EnvironmentInteractionProfile profile,
    float intensity) {
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    bool leftSent = false;
    bool rightSent = false;
    switch (profile) {
    case EnvironmentInteractionProfile::TreasureChestOpen:
        // A short, light latch click. The existing PCM voice keeps the two
        // internal clicks; this motor pair makes the stage mechanically
        // readable even when Bluetooth PCM haptics are not felt.
        rightSent = SendMotorPulseLocked(
            1, intensity * 0.50f, 0.050f, true, 0);
        leftSent = SendMotorPulseLocked(
            0, intensity * 0.24f, 0.070f, true, 0);
        break;
    case EnvironmentInteractionProfile::TreasureChestPush:
        // Sustained low-actuator resistance while the lid is being pushed.
        leftSent = SendMotorPulseLocked(
            0, intensity * 0.70f, 0.480f, true, 12);
        rightSent = SendMotorPulseLocked(
            1, intensity * 0.36f, 0.320f, true, 12);
        break;
    case EnvironmentInteractionProfile::TreasureChestLidLand:
        // A brief, heavy impact followed by a smaller high-actuator edge.
        leftSent = SendMotorPulseLocked(
            0, intensity * 0.98f, 0.160f, true, 12);
        rightSent = SendMotorPulseLocked(
            1, intensity * 0.72f, 0.100f, true, 12);
        break;
    default:
        return false;
    }
    return leftSent && rightSent;
}

bool InvokeNativeGameMotorPower(uint32_t motor, float power) {
    const uintptr_t deviceKey =
        g_nativeGamePadDevice.load(std::memory_order_acquire);
    API::Method* method = g_nativeSetMotorPowerMethod;
    if (deviceKey == 0 || method == nullptr || g_nativeMotorInvocation) {
        return false;
    }

    g_nativeMotorInvocation = true;
    bool succeeded = true;
    try {
        auto* device = reinterpret_cast<API::ManagedObject*>(deviceKey);
        method->call<void>(device, motor, power);
    } catch (...) {
        succeeded = false;
    }
    g_nativeMotorInvocation = false;
    return succeeded;
}

void ArmTreasureChestNativeMotorFallback(
    EnvironmentInteractionProfile profile,
    float intensity) {
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    std::array<float, 2> power{};
    ULONGLONG durationMs = 0;
    switch (profile) {
    case EnvironmentInteractionProfile::TreasureChestOpen:
        power = {intensity * 0.24f, intensity * 0.50f};
        durationMs = 70;
        break;
    case EnvironmentInteractionProfile::TreasureChestPush:
        power = {intensity * 0.70f, intensity * 0.36f};
        durationMs = 480;
        break;
    case EnvironmentInteractionProfile::TreasureChestLidLand:
        power = {intensity * 0.98f, intensity * 0.72f};
        durationMs = 160;
        break;
    default:
        return;
    }

    const ULONGLONG until = GetTickCount64() + durationMs;
    {
        std::lock_guard lock{g_mutex};
        g_nativeTreasureChestMotorPower = power;
        g_nativeTreasureChestMotorUntil = {until, until};
    }

    const bool leftSent = InvokeNativeGameMotorPower(0, power[0]);
    const bool rightSent = InvokeNativeGameMotorPower(1, power[1]);
    XINPUT_VIBRATION vibration{
        static_cast<WORD>(std::lround(power[0] * 65535.0f)),
        static_cast<WORD>(std::lround(power[1] * 65535.0f))};
    const DWORD xinputUser = g_lastXInputUser.load(std::memory_order_relaxed);
    // Route this synthetic pulse through the same bridge entry point as the
    // game's successful XInput writes. Calling the original function here
    // bypassed the bridge's live-motor mirror, so the API could return OK
    // while the DualSense audio-haptics path received no corresponding pulse.
    const DWORD xinputResult = g_originalXInputSetState != nullptr
        ? HookedXInputSetState(xinputUser, &vibration)
        : ERROR_PROC_NOT_FOUND;
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestGameMotor profile=%u native=%s/%s/%s xinputUser=%lu xinput=%s left=%.3f right=%.3f durationMs=%llu",
        static_cast<unsigned>(profile),
        g_nativeGamePadDevice.load(std::memory_order_relaxed) != 0 &&
            g_nativeSetMotorPowerMethod != nullptr ? "ready" : "unavailable",
        leftSent ? "left-ok" : "left-failed",
        rightSent ? "right-ok" : "right-failed",
        static_cast<unsigned long>(xinputUser),
        xinputResult == ERROR_SUCCESS ? "ok" : "failed",
        power[0],
        power[1],
        static_cast<unsigned long long>(durationMs));
}

bool SendTreasureChestXInputMotor(const std::array<float, 2>& power) {
    if (g_originalXInputSetState == nullptr) return false;
    const DWORD user = g_lastXInputUser.load(std::memory_order_relaxed);
    XINPUT_VIBRATION vibration{
        static_cast<WORD>(std::lround(std::clamp(power[0], 0.0f, 1.0f) * 65535.0f)),
        static_cast<WORD>(std::lround(std::clamp(power[1], 0.0f, 1.0f) * 65535.0f))};
    return HookedXInputSetState(user, &vibration) == ERROR_SUCCESS;
}

void RefreshTreasureChestNativeMotorFallback() {
    const ULONGLONG now = GetTickCount64();
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        {
            std::lock_guard lock{g_mutex};
            g_nativeTreasureChestMotorPower.fill(0.0f);
            g_nativeTreasureChestMotorUntil.fill(0);
        }
        SendTreasureChestXInputMotor({0.0f, 0.0f});
        return;
    }

    std::array<float, 2> power{};
    std::array<bool, 2> active{};
    {
        std::lock_guard lock{g_mutex};
        for (size_t motor = 0; motor < power.size(); ++motor) {
            if (g_nativeTreasureChestMotorUntil[motor] == 0) continue;
            active[motor] = true;
            if (now < g_nativeTreasureChestMotorUntil[motor]) {
                power[motor] = g_nativeTreasureChestMotorPower[motor];
            } else {
                power[motor] = 0.0f;
                g_nativeTreasureChestMotorPower[motor] = 0.0f;
                g_nativeTreasureChestMotorUntil[motor] = 0;
            }
        }
    }

    for (size_t motor = 0; motor < power.size(); ++motor) {
        if (active[motor]) {
            InvokeNativeGameMotorPower(
                static_cast<uint32_t>(motor), power[motor]);
        }
    }
    if (active[0] || active[1]) {
        SendTreasureChestXInputMotor(power);
    }
}

bool ReadActiveTreasureChestMotorOverride(std::array<float, 2>& power) {
    power.fill(0.0f);
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed)) return false;

    const ULONGLONG now = GetTickCount64();
    bool active = false;
    std::lock_guard lock{g_mutex};
    for (size_t motor = 0; motor < power.size(); ++motor) {
        if (g_nativeTreasureChestMotorUntil[motor] == 0) continue;
        if (now < g_nativeTreasureChestMotorUntil[motor]) {
            power[motor] = g_nativeTreasureChestMotorPower[motor];
            active = active || power[motor] > 0.0f;
        } else {
            g_nativeTreasureChestMotorPower[motor] = 0.0f;
            g_nativeTreasureChestMotorUntil[motor] = 0;
        }
    }
    return active;
}

void PlayMotorPulse(
    uint32_t motor,
    float power,
    float duration,
    bool attenuate,
    int32_t easeType) {
    if (motor >= 4 || !std::isfinite(power) || !std::isfinite(duration)) return;
    power = std::clamp(power, 0.0f, 1.0f);
    duration = std::clamp(duration, 0.012f, 5.0f);
    if (power <= 0.001f) return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG executionUntil =
        g_executionStandardRumbleSuppressionUntil.load(std::memory_order_acquire);
    if (now < executionUntil) {
        const uint32_t count = g_executionStandardRumbleSuppressedCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count <= 8) {
            API::get()->log_info(
                "[CodexDualSenseBridge] Execution skipped game motor pulse motor=%u power=%.3f duration=%.3f count=%u",
                motor, power, duration, count);
        }
        return;
    }
    std::lock_guard lock{g_mutex};
    const bool same =
        std::fabs(power - g_lastPulsePower[motor]) < 0.01f &&
        std::fabs(duration - g_lastPulseDuration[motor]) < 0.002f;
    if (same && now - g_lastPulseTick[motor] < 16) return;
    const bool sent = SendMotorPulseLocked(
        motor, power, duration, attenuate, easeType);
    if (!same || now - g_lastPulseTick[motor] >= 180) {
        API::get()->log_info(
            "[CodexDualSenseBridge] motorPulse=%u power=%.3f duration=%.3f attenuation=%s ease=%d write=%s",
            motor,
            power,
            duration,
            attenuate ? "yes" : "no",
            easeType,
            sent ? "ok" : "failed");
    }
    g_lastPulsePower[motor] = power;
    g_lastPulseDuration[motor] = duration;
    g_lastPulseTick[motor] = now;
}

void PlayCapturedPreset(captured_haptic_presets::Preset preset) {
    // Concrete success precedes detailEnter, so cancelling an already-pending
    // generic cue is not enough. The trace also awards the item at OPENED.
    // Restrict deduplication to the corresponding chest stage's short window.
    const ULONGLONG now = GetTickCount64();
    const bool generic = preset == captured_haptic_presets::Preset::GenericInteract;
    const bool item = preset == captured_haptic_presets::Preset::GetItem;
    const ULONGLONG chestTick = generic
        ? g_lastTreasureChestOpenTick.load(std::memory_order_relaxed)
        : (item ? g_lastTreasureChestLidLandTick.load(std::memory_order_relaxed) : 0);
    const ULONGLONG window = generic ? 600 : 400;
    if (chestTick != 0 && now >= chestTick && now - chestTick < window) {
        API::get()->log_info(
            "[CodexDualSenseBridge] treasureChest suppressed capturedPreset=%u ageMs=%llu",
            static_cast<unsigned>(preset), static_cast<unsigned long long>(now - chestTick));
        return;
    }
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = HapticCapturedPresetMessage;
    message[1] = static_cast<uint8_t>(preset);
    const bool sent = SendReportLocked(message);
    API::get()->log_info(
        "[CodexDualSenseBridge] capturedPreset=%u write=%s",
        static_cast<unsigned>(preset),
        sent ? "ok" : "failed");
}

bool BeginEvent(std::atomic<ULONGLONG>& lastTick, ULONGLONG minimumIntervalMs) {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous = lastTick.load(std::memory_order_relaxed);
    while (now - previous >= minimumIntervalMs) {
        if (lastTick.compare_exchange_weak(
                previous,
                now,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void PublishPendingInteractionFeedback() {
    if (!g_pendingInteractionFeedback.load(std::memory_order_relaxed)) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG pendingTick =
        g_pendingInteractionTick.load(std::memory_order_relaxed);
    if (pendingTick == 0 || now - pendingTick < 140) return;
    if (!g_pendingInteractionFeedback.exchange(
            false, std::memory_order_relaxed)) {
        return;
    }
    if (now - pendingTick > 1200) return;

    PlayCapturedPreset(captured_haptic_presets::Preset::GenericInteract);
}

void QueueDoorOpeningFeedback(ULONGLONG now, const char* source) {
    ULONGLONG empty = 0;
    if (g_pendingDoorFeedbackTick.compare_exchange_strong(
            empty,
            now,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        API::get()->log_info(
            "[CodexDualSenseBridge] doorFeedbackQueued source=%s delayMs=720",
            source);
    }
}

void PublishPendingDoorFeedback() {
    const ULONGLONG pendingTick =
        g_pendingDoorFeedbackTick.load(std::memory_order_relaxed);
    if (pendingTick == 0) return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG age = now - pendingTick;
    if (age < 720) return;

    ULONGLONG expected = pendingTick;
    if (!g_pendingDoorFeedbackTick.compare_exchange_strong(
            expected,
            0,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    if (age > 2500 || !BeginEvent(g_lastDoorOpeningTick, 1600)) return;

    API::get()->log_info(
        "[CodexDualSenseBridge] doorFeedbackPublished ageMs=%llu",
        static_cast<unsigned long long>(age));
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::Door,
        1.0f,
        "doorInteraction+720ms");
}

void QueueTreasureChestLidLandFeedback(ULONGLONG openingTick) {
    g_pendingTreasureChestLidLandTick.store(
        openingTick, std::memory_order_relaxed);
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=lidLand queued delayMs=%llu source=OPENING",
        static_cast<unsigned long long>(
            treasure_chest_haptics::FallbackLidImpactDelayMs));
}

void QueueTreasureChestOpenFeedback(ULONGLONG interactionTick) {
    g_pendingTreasureChestOpenCueTick.store(
        interactionTick, std::memory_order_relaxed);
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=lift queued fallbackDelayMs=%llu source=interaction-success",
        static_cast<unsigned long long>(
            treasure_chest_haptics::OpenCueFallbackDelayMs));
}

void PublishPendingTreasureChestOpenFeedback(bool forceForOpening = false) {
    const ULONGLONG interactionTick =
        g_pendingTreasureChestOpenCueTick.load(std::memory_order_relaxed);
    if (interactionTick == 0) return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG age = now - interactionTick;
    if (!forceForOpening &&
        age < treasure_chest_haptics::OpenCueFallbackDelayMs) {
        return;
    }

    ULONGLONG expected = interactionTick;
    if (!g_pendingTreasureChestOpenCueTick.compare_exchange_strong(
            expected, 0,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed) ||
        g_activeTreasureChest.load(std::memory_order_relaxed) == 0 ||
        age > 1600) {
        API::get()->log_info(
            "[CodexDualSenseBridge] treasureChestStage=open scheduledDiscard ageMs=%llu paused=%d",
            static_cast<unsigned long long>(age),
            g_gameplayHapticsPaused.load(std::memory_order_relaxed) ? 1 : 0);
        return;
    }

    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::TreasureChestOpen,
        1.0f,
        forceForOpening
            ? "GimmickTreasureBox.OPENING-lift"
            : "Gm002.interaction+900ms-lift-fallback");
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=lift published ageMs=%llu openingState=%d",
        static_cast<unsigned long long>(age),
        forceForOpening ? 1 : 0);
}

void QueueTreasureChestPushFeedback(ULONGLONG openingTick) {
    g_pendingTreasureChestPushCueTick.store(
        openingTick, std::memory_order_relaxed);
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=push queued delayMs=%llu source=OPENING",
        static_cast<unsigned long long>(
            treasure_chest_haptics::PushCueDelayMs));
}

void PublishPendingTreasureChestPushFeedback() {
    const ULONGLONG openingTick =
        g_pendingTreasureChestPushCueTick.load(std::memory_order_relaxed);
    if (openingTick == 0) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG age = now - openingTick;
    if (age < treasure_chest_haptics::PushCueDelayMs) return;

    ULONGLONG expected = openingTick;
    if (!g_pendingTreasureChestPushCueTick.compare_exchange_strong(
            expected, 0,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed) ||
        g_activeTreasureChest.load(std::memory_order_relaxed) == 0 ||
        age > treasure_chest_haptics::PushCueDelayMs + 1200) {
        API::get()->log_info(
            "[CodexDualSenseBridge] treasureChestStage=push scheduledDiscard ageMs=%llu paused=%d",
            static_cast<unsigned long long>(age),
            g_gameplayHapticsPaused.load(std::memory_order_relaxed) ? 1 : 0);
        return;
    }
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::TreasureChestPush,
        1.0f,
        "GimmickTreasureBox.OPENING+600ms-push");
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=push scheduledPublished ageMs=%llu",
        static_cast<unsigned long long>(age));
}

void PublishPendingTreasureChestLidLandFeedback() {
    const ULONGLONG openingTick =
        g_pendingTreasureChestLidLandTick.load(std::memory_order_relaxed);
    if (openingTick == 0) return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG age = now - openingTick;
    if (age < treasure_chest_haptics::FallbackLidImpactDelayMs) return;

    ULONGLONG expected = openingTick;
    if (!g_pendingTreasureChestLidLandTick.compare_exchange_strong(
            expected, 0,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }

    const ULONGLONG opened =
        g_lastTreasureChestOpenTick.load(std::memory_order_relaxed);
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed) ||
        g_activeTreasureChest.load(std::memory_order_relaxed) == 0 ||
        opened == 0 || now < opened || now - opened > 12000 ||
        age > treasure_chest_haptics::FallbackLidImpactDelayMs + 1200) {
        API::get()->log_info(
            "[CodexDualSenseBridge] treasureChestStage=lidLand scheduledDiscard ageMs=%llu paused=%d",
            static_cast<unsigned long long>(age),
            g_gameplayHapticsPaused.load(std::memory_order_relaxed) ? 1 : 0);
        return;
    }
    if (!BeginEvent(
            g_lastTreasureChestLidLandTick,
            treasure_chest_haptics::LidImpactDedupeMs)) {
        return;
    }

    g_pendingTreasureChestPushCueTick.store(0, std::memory_order_relaxed);
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::TreasureChestLidLand,
        1.0f,
        "GimmickTreasureBox.OPENING+1700ms");
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=lidLand scheduledPublished ageMs=%llu",
        static_cast<unsigned long long>(age));
}

void LogMotorSourceLocked(live_motor_state::Kind kind, uintptr_t source,
    uint32_t motor, float power, bool success) {
    const ULONGLONG now = GetTickCount64();
    const size_t bucket = (kind == live_motor_state::Kind::XInput ? 4u : 0u) + motor;
    if (g_motorSourceLogCount >= 24 && now - g_lastMotorSourceLogTick[bucket] < 1000) return;
    ++g_motorSourceLogCount;
    g_lastMotorSourceLogTick[bucket] = now;
    API::get()->log_info(
        "[CodexDualSenseBridge] motorSource kind=%s key=%llX motor=%u raw=%.6f accepted=%d paused=%d",
        live_motor_state::Name(kind), static_cast<unsigned long long>(source), motor,
        power, success ? 1 : 0, g_gameplayHapticsPaused.load() ? 1 : 0);
}

void PublishMotorPowerLocked(bool heartbeat = false) {
    const ULONGLONG now = GetTickCount64();
    const size_t expired = g_liveMotorSources.ExpireNative(now);
    if (expired != 0) API::get()->log_info(
        "[CodexDualSenseBridge] liveMotor expired nativeChannels=%zu", expired);
    const auto mix = g_liveMotorSources.Resolve();
    for (size_t motor = 0; motor < mix.power.size(); ++motor) {
        const float power = mix.power[motor];
        if (!live_motor_state::ShouldSend(g_sentMotorPower[motor], power, heartbeat)) continue;
        const float previous = g_sentMotorPower[motor];
        const bool sent = SendMotorPowerLocked(static_cast<uint32_t>(motor), power);
        // A failed write is not an acknowledged state. Retry on the next
        // callback/heartbeat, including zero and helper restarts.
        if (sent) g_sentMotorPower[motor] = power;
        const bool changed = previous != power;
        const bool edge = previous < 0.0f || (previous == 0.0f) != (power == 0.0f);
        if (edge || ((!sent || changed) && now - g_lastMotorLogTick[motor] >= 100)) {
            API::get()->log_info(
                "[CodexDualSenseBridge] liveMotor=%zu power=%.6f write=%s source=%s key=%llX nativeMirrors=%zu",
                motor, power, sent ? "ok" : "failed", live_motor_state::Name(mix.motorKind[motor]),
                static_cast<unsigned long long>(mix.source[motor]), g_liveMotorSources.NativeCount());
            g_lastMotorLogTick[motor] = now;
        }
    }
}

DWORD WINAPI HookedXInputSetState(
    DWORD userIndex,
    XINPUT_VIBRATION* vibration) {
    XINPUT_VIBRATION zeroVibration{};
    XINPUT_VIBRATION mergedVibration{};
    XINPUT_VIBRATION* forwardedVibration = vibration;
    const ULONGLONG xinputNow = GetTickCount64();
    const ULONGLONG repelSuppressionUntil =
        g_repelStandardRumbleSuppressionUntil.load(std::memory_order_acquire);
    const ULONGLONG executionSuppressionUntil =
        g_executionStandardRumbleSuppressionUntil.load(std::memory_order_acquire);
    bool oniFinisherSlashWindow = false;
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
        const uint8_t stage = g_oniChargeFallbackKind[user].load(
            std::memory_order_relaxed);
        const ULONGLONG stageTick = g_oniChargeFallbackTick[user].load(
            std::memory_order_relaxed);
        if (stage == OniFinisherSlashSentCode && stageTick != 0 &&
            xinputNow >= stageTick && xinputNow - stageTick <= 300u) {
            oniFinisherSlashWindow = true;
            break;
        }
    }
    if (vibration != nullptr &&
        (oniFinisherSlashWindow ||
         combat_defense_feedback::ShouldSuppressStandardRumble(
             xinputNow,
             std::max(repelSuppressionUntil, executionSuppressionUntil),
             vibration->wLeftMotorSpeed,
             vibration->wRightMotorSpeed))) {
        forwardedVibration = &zeroVibration;
        if (oniFinisherSlashWindow) {
            API::get()->log_info(
                "[CodexDualSenseBridge] oniFinisher suppressed native XInput rumble user=%lu",
                userIndex);
        } else {
            const bool execution = xinputNow < executionSuppressionUntil;
            const uint32_t count = (execution
                ? g_executionStandardRumbleSuppressedCount
                : g_repelStandardRumbleSuppressedCount).fetch_add(
                    1, std::memory_order_relaxed) + 1;
            if (count <= 4) {
                API::get()->log_info(
                    "[CodexDualSenseBridge] %s suppressed XInput rumble user=%lu left=%u right=%u count=%u",
                    execution ? "Execution" : "Repel",
                    userIndex,
                    static_cast<unsigned>(vibration->wLeftMotorSpeed),
                    static_cast<unsigned>(vibration->wRightMotorSpeed),
                    count);
            }
        }
    }

    // The game writes XInput motor state again after the chest callback, often
    // with a zero vibration. Merge the authored chest pulse into that final
    // write so a later game frame cannot erase the feedback before the pad
    // receives it. Keep this local rather than mutating the game's buffer.
    std::array<float, 2> chestMotorPower{};
    if (UseTreasureChestLegacyMotorFallback &&
        ReadActiveTreasureChestMotorOverride(chestMotorPower)) {
        if (forwardedVibration != nullptr) {
            mergedVibration = *forwardedVibration;
        }
        mergedVibration.wLeftMotorSpeed = std::max(
            mergedVibration.wLeftMotorSpeed,
            static_cast<WORD>(std::lround(
                std::clamp(chestMotorPower[0], 0.0f, 1.0f) * 65535.0f)));
        mergedVibration.wRightMotorSpeed = std::max(
            mergedVibration.wRightMotorSpeed,
            static_cast<WORD>(std::lround(
                std::clamp(chestMotorPower[1], 0.0f, 1.0f) * 65535.0f)));
        forwardedVibration = &mergedVibration;

        ULONGLONG previousLog =
            g_lastTreasureChestXInputOverrideLogTick.load(std::memory_order_relaxed);
        if (previousLog == 0 || xinputNow - previousLog >= 200) {
            if (g_lastTreasureChestXInputOverrideLogTick.compare_exchange_strong(
                    previousLog,
                    xinputNow,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                API::get()->log_info(
                    "[CodexDualSenseBridge] treasureChestXInputOverride user=%lu left=%.3f right=%.3f",
                    userIndex,
                    chestMotorPower[0],
                    chestMotorPower[1]);
            }
        }
    }

    uint32_t spatialTrigger = 0;
    float spatialGain = 0.0f;
    const DWORD result = g_originalXInputSetState != nullptr
        ? g_originalXInputSetState(userIndex, forwardedVibration)
        : ERROR_DEVICE_NOT_CONNECTED;
    if (result == ERROR_SUCCESS && userIndex < XUSER_MAX_COUNT) {
        g_lastXInputUser.store(userIndex, std::memory_order_relaxed);
    }
    if (userIndex < XUSER_MAX_COUNT && vibration != nullptr) {
        {
            std::lock_guard lock{g_mutex};
            const float left = static_cast<float>(forwardedVibration->wLeftMotorSpeed) / 65535.0f;
            const float right = static_cast<float>(forwardedVibration->wRightMotorSpeed) / 65535.0f;
            const bool accepted = result == ERROR_SUCCESS && !g_gameplayHapticsPaused.load();
            g_liveMotorSources.XInput(userIndex, left, right, accepted);
            LogMotorSourceLocked(live_motor_state::Kind::XInput, userIndex, 0, left, accepted);
            LogMotorSourceLocked(live_motor_state::Kind::XInput, userIndex, 1, right, accepted);
            PublishMotorPowerLocked();

            // Distance-attenuated presets reach this point only after the game
            // has calculated listener distance.  Use that final motor level as
            // the gain for the matching authored haptic clip instead of playing
            // it at full strength when the request was first queued.
            const ULONGLONG now = GetTickCount64();
            if (g_pendingSpatialTrigger != 0) {
                if (now - g_pendingSpatialTick > 250) {
                    g_pendingSpatialTrigger = 0;
                } else {
                    const float finalPower = std::max(left, right);
                    if (accepted && finalPower > 0.005f) {
                        spatialTrigger = g_pendingSpatialTrigger;
                        spatialGain = std::clamp(
                            std::pow(finalPower, 0.70f) * 1.05f,
                            0.0f,
                            1.0f);
                        g_pendingSpatialTrigger = 0;
                    }
                }
            }
        }
        if (spatialTrigger != 0) {
            PlayHapticTrigger(spatialTrigger, spatialGain);
        }
    }
    return result;
}

bool EmitOniChargeRelease(normal_attack_haptics::Kind kind, const char* source, ULONGLONG heldMs) {
    using normal_attack_haptics::Kind;
    if (!normal_attack_haptics::IsChargeKind(kind)) return false;
    const ULONGLONG now = GetTickCount64();
    const auto previousKind = static_cast<Kind>(
        g_lastOniChargeReleaseKind.load(std::memory_order_relaxed));
    const ULONGLONG previousTick = g_lastOniChargeReleaseTick.load(std::memory_order_relaxed);
    if (previousKind == kind && previousTick != 0 && now >= previousTick && now - previousTick < 140u)
        return false;
    g_lastOniChargeReleaseKind.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
    g_lastOniChargeReleaseTick.store(now, std::memory_order_relaxed);
    const bool sent = SendOniChargeReleaseHaptic(kind);
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
        if (g_oniChargeFallbackKind[user].load(std::memory_order_relaxed) ==
            static_cast<uint8_t>(kind)) {
            g_oniChargeFallbackKind[user].store(0, std::memory_order_relaxed);
            g_oniChargeFallbackTick[user].store(0, std::memory_order_relaxed);
        }
    }
    g_pendingOniChargeReleaseKind.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
    g_pendingOniChargeReleaseTick.store(now, std::memory_order_relaxed);
    g_pendingOniChargeOwner.store(0, std::memory_order_relaxed);
    API::get()->log_info(
        "[CodexDualSenseBridge] oniCharge release kind=%u source=%s heldMs=%llu write=%s",
        static_cast<unsigned>(kind), source != nullptr ? source : "unknown", heldMs,
        sent ? "ok" : "failed");
    return sent;
}

void UpdateOniChargeInputState(DWORD userIndex, uint16_t buttons, ULONGLONG now) {
    using normal_attack_haptics::Kind;
    using normal_attack_haptics::Operation;
    if (userIndex >= XUSER_MAX_COUNT) return;
    // Reuse the proven Oni charge state machine for the R1 one-cut finisher.
    // Keeping it here avoids a second parallel input state and also makes X/Y
    // charge and R1 finisher mutually exclusive, matching the game actions.
    const uint8_t desiredCode = (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0
        ? OniFinisherInputCode
        : ((buttons & XINPUT_GAMEPAD_Y) != 0
            ? static_cast<uint8_t>(Kind::ChargeHeavy)
            : ((buttons & XINPUT_GAMEPAD_X) != 0
                ? static_cast<uint8_t>(Kind::ChargeLight)
                : static_cast<uint8_t>(Kind::None)));
    const Kind desired = desiredCode == OniFinisherInputCode
        ? Kind::ChargeHeavy : static_cast<Kind>(desiredCode);
    const bool oniActive = g_oniTransformationActive.load(std::memory_order_acquire) &&
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed);
    const uint8_t previousCode =
        g_oniChargeInputKind[userIndex].load(std::memory_order_relaxed);
    const bool previousFinisher = previousCode == OniFinisherInputCode;
    const Kind previousKind = previousFinisher
        ? Kind::ChargeHeavy : static_cast<Kind>(previousCode);
    bool feedback = g_oniChargeFeedbackActive[userIndex].load(std::memory_order_relaxed);
    const uint8_t finisherStage =
        g_oniChargeFallbackKind[userIndex].load(std::memory_order_relaxed);
    const ULONGLONG finisherStageTick =
        g_oniChargeFallbackTick[userIndex].load(std::memory_order_relaxed);

    if (!oniActive) {
        if (feedback) SendNormalAttackHaptic(previousKind, Operation::ChargeCancel);
        g_oniChargeButtonDownTick[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeRefreshTick[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeInputKind[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeFeedbackActive[userIndex].store(false, std::memory_order_relaxed);
        g_oniChargeFallbackKind[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeFallbackTick[userIndex].store(0, std::memory_order_relaxed);
        return;
    }
    if (desired == Kind::None) {
        if ((finisherStage == OniFinisherPreparingCode ||
             finisherStage == OniFinisherSlashSentCode) &&
            finisherStageTick != 0 && now >= finisherStageTick &&
            now - finisherStageTick < 1600u) {
            if (feedback) {
                SendNormalAttackHaptic(Kind::ChargeHeavy, Operation::ChargeCancel);
                g_oniChargeFeedbackActive[userIndex].store(false, std::memory_order_relaxed);
            }
            g_oniChargeButtonDownTick[userIndex].store(0, std::memory_order_relaxed);
            g_oniChargeRefreshTick[userIndex].store(0, std::memory_order_relaxed);
            g_oniChargeInputKind[userIndex].store(0, std::memory_order_relaxed);
            return;
        }
        const Kind fallbackKind = static_cast<Kind>(
            g_oniChargeFallbackKind[userIndex].load(std::memory_order_relaxed));
        const ULONGLONG fallbackTick =
            g_oniChargeFallbackTick[userIndex].load(std::memory_order_relaxed);
        if (IsChargeKind(fallbackKind) && fallbackTick != 0 &&
            now >= fallbackTick && now - fallbackTick >= 420u) {
            EmitOniChargeRelease(fallbackKind, "xinput-delayed-fallback", now - fallbackTick);
        }
        if (feedback && previousFinisher) {
            SendNormalAttackHaptic(Kind::ChargeHeavy, Operation::ChargeCancel);
            API::get()->log_info(
                "[CodexDualSenseBridge] oniFinisher cancelled before preparation");
        } else if (feedback && normal_attack_haptics::IsChargeKind(previousKind)) {
            const ULONGLONG down = g_oniChargeButtonDownTick[userIndex].load(std::memory_order_relaxed);
            const auto releasedKind = static_cast<Kind>(
                g_lastOniChargeReleaseKind.load(std::memory_order_relaxed));
            const ULONGLONG releasedAt = g_lastOniChargeReleaseTick.load(std::memory_order_relaxed);
            if (releasedKind != previousKind || releasedAt < down) {
                g_oniChargeFallbackKind[userIndex].store(
                    static_cast<uint8_t>(previousKind), std::memory_order_relaxed);
                g_oniChargeFallbackTick[userIndex].store(now, std::memory_order_relaxed);
                API::get()->log_info(
                    "[CodexDualSenseBridge] oniCharge fallback scheduled kind=%u graceMs=420",
                    static_cast<unsigned>(previousKind));
            } else {
                API::get()->log_info(
                    "[CodexDualSenseBridge] oniCharge xinput release suppressed; managed action already emitted kind=%u",
                    static_cast<unsigned>(previousKind));
            }
        }
        g_oniChargeButtonDownTick[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeRefreshTick[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeInputKind[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeFeedbackActive[userIndex].store(false, std::memory_order_relaxed);
        return;
    }
    if (desiredCode == OniFinisherInputCode &&
        (finisherStage == OniFinisherPreparingCode ||
         finisherStage == OniFinisherSlashSentCode)) {
        // The game executes this move while R1 is still held. Once preparation
        // starts, never restart the continuous charge bed.
        if (finisherStage == OniFinisherPreparingCode &&
            finisherStageTick != 0 && now >= finisherStageTick &&
            now - finisherStageTick >= 1350u) {
            uint8_t expected = OniFinisherPreparingCode;
            if (g_oniChargeFallbackKind[userIndex].compare_exchange_strong(
                    expected, OniFinisherSlashSentCode,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                g_oniChargeFallbackTick[userIndex].store(now, std::memory_order_relaxed);
                const bool sent = SendOniChargeReleaseHaptic(
                    Kind::ChargeHeavy,
                    oni_charge_release_haptics::Kind::FinisherSlash);
                API::get()->log_info(
                    "[CodexDualSenseBridge] oniFinisher slash source=timed-fallback write=%s",
                    sent ? "ok" : "failed");
            }
        }
        return;
    }
    if (desiredCode != previousCode) {
        if (feedback) SendNormalAttackHaptic(previousKind, Operation::ChargeCancel);
        g_oniChargeInputKind[userIndex].store(desiredCode, std::memory_order_relaxed);
        g_oniChargeButtonDownTick[userIndex].store(now, std::memory_order_relaxed);
        g_oniChargeRefreshTick[userIndex].store(0, std::memory_order_relaxed);
        g_oniChargeFeedbackActive[userIndex].store(false, std::memory_order_relaxed);
        return;
    }
    const ULONGLONG down = g_oniChargeButtonDownTick[userIndex].load(std::memory_order_relaxed);
    if (down == 0) {
        g_oniChargeButtonDownTick[userIndex].store(now, std::memory_order_relaxed);
        return;
    }
    // A normal tap stays silent. R1 uses a slightly earlier threshold so the
    // held pressure begins with the visible one-cut preparation.
    const ULONGLONG holdThreshold = desiredCode == OniFinisherInputCode ? 150u : 170u;
    if (!feedback && now - down >= holdThreshold) {
        const bool sent = SendNormalAttackHaptic(desired, Operation::ChargeStart);
        g_oniChargeFeedbackActive[userIndex].store(true, std::memory_order_relaxed);
        g_oniChargeRefreshTick[userIndex].store(now, std::memory_order_relaxed);
        API::get()->log_info(
            "[CodexDualSenseBridge] oniChargeInput start kind=%u finisher=%u thresholdMs=%llu write=%s",
            static_cast<unsigned>(desired), desiredCode == OniFinisherInputCode ? 1u : 0u,
            now - down, sent ? "ok" : "failed");
    } else if (feedback) {
        if (desiredCode == OniFinisherInputCode && now - down >= OniFinisherPrepareMs) {
            SendNormalAttackHaptic(Kind::ChargeHeavy, Operation::ChargeCancel);
            g_oniChargeFeedbackActive[userIndex].store(false, std::memory_order_relaxed);
            g_oniChargeFallbackKind[userIndex].store(
                OniFinisherPreparingCode, std::memory_order_relaxed);
            g_oniChargeFallbackTick[userIndex].store(now, std::memory_order_relaxed);
            const bool sent = SendOniChargeReleaseHaptic(
                Kind::ChargeHeavy,
                oni_charge_release_haptics::Kind::FinisherDraw);
            API::get()->log_info(
                "[CodexDualSenseBridge] oniFinisher preparation holdStopped=%llu draw=%s",
                now - down, sent ? "ok" : "failed");
            return;
        }
        const ULONGLONG refresh = g_oniChargeRefreshTick[userIndex].load(std::memory_order_relaxed);
        if (refresh == 0 || now - refresh >= 100u) {
            SendNormalAttackHaptic(desired, Operation::ChargeRefresh);
            g_oniChargeRefreshTick[userIndex].store(now, std::memory_order_relaxed);
        }
    }
}

DWORD WINAPI HookedXInputGetState(DWORD userIndex, XINPUT_STATE* state) {
    const DWORD result = g_originalXInputGetState != nullptr
        ? g_originalXInputGetState(userIndex, state)
        : ERROR_DEVICE_NOT_CONNECTED;
    if (userIndex >= XUSER_MAX_COUNT) return result;

    if (result == ERROR_SUCCESS && state != nullptr && userIndex == 0 &&
        g_touchState != nullptr &&
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed) &&
        !g_titleActive.load(std::memory_order_relaxed) &&
        g_playerCharacterEntity.load(std::memory_order_relaxed) != 0) {
        const uint64_t packed = static_cast<uint64_t>(InterlockedCompareExchange64(
            &g_touchState->packed, 0, 0));
        const uint32_t flags = dualsense_touch_input::FreshFlags(
            packed, GetTickCount());
        if ((flags & dualsense_touch_input::Left) != 0)
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if ((flags & dualsense_touch_input::Right) != 0)
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    }

    uint16_t buttons = 0;
    if (result == ERROR_SUCCESS && state != nullptr) {
        buttons = state->Gamepad.wButtons;
    }
    const uint16_t previous = g_lastXInputButtons[userIndex].exchange(
        buttons, std::memory_order_relaxed);
    if (result != ERROR_SUCCESS || state == nullptr) {
        {
            std::lock_guard lock{g_mutex};
            if (g_liveMotorSources.HasXInput(userIndex)) {
                g_liveMotorSources.RemoveXInput(userIndex);
                PublishMotorPowerLocked();
            }
        }
        UpdateSoulActionInputState(userIndex, false);
        g_sprintToggleActive[userIndex].store(false, std::memory_order_relaxed);
        g_sprintSlowCandidateTick[userIndex].store(0, std::memory_order_relaxed);
        g_sprintStartStickMagnitude[userIndex].store(
            0.0f, std::memory_order_relaxed);
        const ULONGLONG disconnectedAt = GetTickCount64();
        UpdateOniChargeInputState(userIndex, 0, disconnectedAt);
        return result;
    }

    constexpr uint16_t NavigationButtons =
        XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT |
        XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if ((buttons & NavigationButtons) != 0 ||
        std::abs(static_cast<int32_t>(state->Gamepad.sThumbLX)) > 8000 ||
        std::abs(static_cast<int32_t>(state->Gamepad.sThumbLY)) > 8000 ||
        std::abs(static_cast<int32_t>(state->Gamepad.sThumbRX)) > 8000 ||
        std::abs(static_cast<int32_t>(state->Gamepad.sThumbRY)) > 8000) {
        g_lastMenuNavigationInputTick.store(GetTickCount64(), std::memory_order_relaxed);
    }

    // Retain the physical chord for the explicit success/native shell-sound
    // checks. A directional-guard state or reused vibration ID by itself is
    // not proof of Repel. DualSense Cross is XInput A through Steam Input.
    const bool parryChordHeld =
        (buttons & ParryInputChord) == ParryInputChord;
    const bool parryChordWasHeld =
        (previous & ParryInputChord) == ParryInputChord;
    if (parryChordHeld && !parryChordWasHeld) {
        g_lastParryInputTick.store(GetTickCount64(), std::memory_order_relaxed);
        API::get()->log_info(
            "[CodexDualSenseBridge] parryInputChord user=%u",
            static_cast<unsigned>(userIndex));
    }

    // requestSoulAbsorptionAction is not dispatched by the empty-action path
    // in this PC build. Read the already-hooked XInput L2 axis instead, with
    // press/release hysteresis, while the spawner flag remains authoritative
    // for whether a real soul stream is present.
    bool soulActionHeld = g_soulActionInputHeld[userIndex].load(
        std::memory_order_relaxed);
    if (state->Gamepad.bLeftTrigger >= 30) {
        soulActionHeld = true;
    } else if (state->Gamepad.bLeftTrigger <= 18) {
        soulActionHeld = false;
    }
    constexpr uint16_t MenuButtons =
        XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
    const bool menuButtonPressed =
        (buttons & MenuButtons) != 0 && (previous & MenuButtons) == 0;
    if (menuButtonPressed && soulActionHeld &&
        !g_soulHapticActive.load(std::memory_order_relaxed)) {
        g_soulSearchSuppressedUserMask.fetch_or(
            1u << userIndex, std::memory_order_relaxed);
        g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
        SetSoulSearchHaptic(false);
    }
    UpdateSoulActionInputState(userIndex, soulActionHeld);

    const float axisX = static_cast<float>(state->Gamepad.sThumbLX) / 32768.0f;
    const float axisY = static_cast<float>(state->Gamepad.sThumbLY) / 32768.0f;
    const float magnitude = std::min(
        1.0f,
        std::sqrt(axisX * axisX + axisY * axisY));
    const ULONGLONG now = GetTickCount64();
    UpdateOniChargeInputState(userIndex, buttons, now);
    if (g_sprintToggleActive[userIndex].load(std::memory_order_relaxed)) {
        const float startMagnitude = std::max(
            0.30f,
            g_sprintStartStickMagnitude[userIndex].load(
                std::memory_order_relaxed));
        const float slowThreshold = std::max(0.30f, startMagnitude * 0.86f);
        if (magnitude < slowThreshold) {
            ULONGLONG slowTick = g_sprintSlowCandidateTick[userIndex].load(
                std::memory_order_relaxed);
            if (slowTick == 0) {
                g_sprintSlowCandidateTick[userIndex].store(
                    now, std::memory_order_relaxed);
            } else if (now - slowTick >= 140 &&
                g_sprintToggleActive[userIndex].exchange(
                    false, std::memory_order_relaxed)) {
                g_sprintSlowCandidateTick[userIndex].store(
                    0, std::memory_order_relaxed);
                g_sprintStartStickMagnitude[userIndex].store(
                    0.0f, std::memory_order_relaxed);
                PlaySprintSlowdownFeedback();
            }
        } else {
            // Require 140 ms continuously below the adaptive threshold so a
            // momentary analog-stick fluctuation cannot end sprint state.
            g_sprintSlowCandidateTick[userIndex].store(
                0, std::memory_order_relaxed);
        }
    } else {
        g_sprintSlowCandidateTick[userIndex].store(0, std::memory_order_relaxed);
        g_sprintStartStickMagnitude[userIndex].store(
            0.0f, std::memory_order_relaxed);
    }

    const bool leftStickPressed =
        (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 &&
        (previous & XINPUT_GAMEPAD_LEFT_THUMB) == 0;
    if (!leftStickPressed) return result;

    // Only treat L3 as sprint while the movement stick is displaced. This
    // avoids feedback for stationary clicks or unrelated menu bindings.
    if (magnitude >= 0.30f) {
        const bool wasSprinting = g_sprintToggleActive[userIndex].exchange(
            true, std::memory_order_relaxed);
        if (!wasSprinting) {
            g_sprintToggleActive[userIndex].store(
                true, std::memory_order_relaxed);
            g_sprintSlowCandidateTick[userIndex].store(
                0, std::memory_order_relaxed);
            g_sprintStartStickMagnitude[userIndex].store(
                magnitude, std::memory_order_relaxed);
            const float intensity = std::clamp(
                0.72f + (magnitude - 0.30f) * 0.40f,
                0.72f,
                1.0f);
            PlaySprintStartFeedback(intensity);
        } else {
            API::get()->log_info(
                "[CodexDualSenseBridge] sprintButtonFeedback mode=already-active suppressed");
        }
    }
    return result;
}

bool PatchXInputSetStateImport(ULONG_PTR* slot) {
    if (slot == nullptr) return false;
    const ULONG_PTR hook = reinterpret_cast<ULONG_PTR>(&HookedXInputSetState);
    const ULONG_PTR current = *slot;
    if (current == hook) return true;

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        return false;
    }
    if (current != 0) {
        g_originalXInputSetState = reinterpret_cast<XInputSetStateFn>(current);
    }
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(hook));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    return g_originalXInputSetState != nullptr;
}

bool PatchXInputGetStateImport(ULONG_PTR* slot) {
    if (slot == nullptr) return false;
    const ULONG_PTR hook = reinterpret_cast<ULONG_PTR>(&HookedXInputGetState);
    const ULONG_PTR current = *slot;
    if (current == hook) return true;

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        return false;
    }
    if (current != 0) {
        g_originalXInputGetState = reinterpret_cast<XInputGetStateFn>(current);
    }
    InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(hook));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    return g_originalXInputGetState != nullptr;
}

bool HookXInputSetStateImport() {
    auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (module == nullptr) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        module + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* library = reinterpret_cast<const char*>(module + descriptor->Name);
        if (_stricmp(library, "XINPUT1_4.dll") != 0 &&
            _stricmp(library, "XINPUT1_3.dll") != 0 &&
            _stricmp(library, "XINPUT9_1_0.dll") != 0) {
            continue;
        }

        auto* importThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            module + descriptor->FirstThunk);
        auto* nameThunk = descriptor->OriginalFirstThunk != 0
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(module + descriptor->OriginalFirstThunk)
            : importThunk;
        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++importThunk) {
            bool isSetState = false;
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) {
                isSetState = IMAGE_ORDINAL(nameThunk->u1.Ordinal) == 3;
            } else {
                const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    module + nameThunk->u1.AddressOfData);
                isSetState = std::strcmp(
                    reinterpret_cast<const char*>(import->Name),
                    "XInputSetState") == 0;
            }
            if (!isSetState) continue;

            g_xinputSetStateIatSlot = &importThunk->u1.Function;
            return PatchXInputSetStateImport(g_xinputSetStateIatSlot);
        }
    }
    return false;
}

bool HookXInputGetStateImport() {
    auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (module == nullptr) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 ||
        directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        module + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* library = reinterpret_cast<const char*>(
            module + descriptor->Name);
        if (_stricmp(library, "XINPUT1_4.dll") != 0 &&
            _stricmp(library, "XINPUT1_3.dll") != 0 &&
            _stricmp(library, "XINPUT9_1_0.dll") != 0) {
            continue;
        }

        auto* importThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            module + descriptor->FirstThunk);
        auto* nameThunk = descriptor->OriginalFirstThunk != 0
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                module + descriptor->OriginalFirstThunk)
            : importThunk;
        for (; nameThunk->u1.AddressOfData != 0;
             ++nameThunk, ++importThunk) {
            bool isGetState = false;
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) {
                isGetState = IMAGE_ORDINAL(nameThunk->u1.Ordinal) == 2;
            } else {
                const auto* import =
                    reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                        module + nameThunk->u1.AddressOfData);
                isGetState = std::strcmp(
                    reinterpret_cast<const char*>(import->Name),
                    "XInputGetState") == 0;
            }
            if (!isGetState) continue;

            g_xinputGetStateIatSlot = &importThunk->u1.Function;
            return PatchXInputGetStateImport(g_xinputGetStateIatSlot);
        }
    }
    return false;
}

bool EnsureAdaptiveEventSubscriptions() {
    if (g_adaptiveSubscriptionsReady.load()) return true;

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastAdaptiveSubscriptionAttempt < 1000) return false;
    g_lastAdaptiveSubscriptionAttempt = now;

    auto* type = API::get()->tdb()->find_type("app.AdaptiveTriggerManager");
    if (type == nullptr) return false;
    auto* awake = type->find_method("awakeGA");
    auto* manager = API::get()->get_managed_singleton("app.AdaptiveTriggerManager");
    if (awake == nullptr || manager == nullptr) return false;

    // The shipped PC method exits before registering the bow and Spawner
    // absorption flag handlers unless the runtime platform nibble is PS5 (5).
    // Locate that short gate in the reflected awakeGA body and change only its
    // conditional branch to an unconditional jump into the subscription code.
    auto* code = static_cast<uint8_t*>(awake->get_function_raw());
    if (code == nullptr) return false;
    constexpr std::array<uint8_t, 8> gate{
        0x83, 0xE0, 0x0F, 0x83, 0xF8, 0x05, 0x74, 0x08};
    uint8_t* branch = nullptr;
    for (size_t offset = 0; offset + gate.size() <= 64; ++offset) {
        if (std::memcmp(code + offset, gate.data(), gate.size()) == 0) {
            branch = code + offset + 6;
            break;
        }
    }
    if (branch == nullptr) {
        API::get()->log_error(
            "[CodexDualSenseBridge] Adaptive subscription platform gate not found");
        return false;
    }
    if (*branch != 0xEB) {
        DWORD oldProtection = 0;
        if (!VirtualProtect(branch, 1, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            API::get()->log_error(
                "[CodexDualSenseBridge] Could not patch adaptive subscription gate error=%lu",
                GetLastError());
            return false;
        }
        *branch = 0xEB;
        DWORD ignored = 0;
        VirtualProtect(branch, 1, oldProtection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), branch, 1);
    }

    awake->invoke(manager, std::vector<void*>{});
    g_adaptiveSubscriptionsReady.store(true);
    API::get()->log_info(
        "[CodexDualSenseBridge] Enabled PC bow/absorption adaptive-trigger event subscriptions");
    return true;
}

int PreRainVisualFade(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 3 && argv != nullptr) {
        const float alpha = std::clamp(FloatArgument(argv[2]), 0.0f, 1.0f);
        g_rainVisualAlpha.store(alpha, std::memory_order_relaxed);
#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
        const float previous = g_lastLoggedRainVisualAlpha.load(
            std::memory_order_relaxed);
        if (std::fabs(alpha - previous) >= 0.02f) {
            g_lastLoggedRainVisualAlpha.store(alpha, std::memory_order_relaxed);
            API::get()->log_info(
                "[CodexDualSenseBridge] rainVisualAlpha=%.3f",
                alpha);
        }
#endif
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PrePlayerSupporterUpdate(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr) {
        auto* supporter = reinterpret_cast<API::ManagedObject*>(argv[1]);
        if (auto* entity = supporter->get_field<API::ManagedObject*>(
                "_PlayerCharacterEntity"); entity != nullptr && *entity != nullptr) {
            g_playerCharacterEntity.store(
                reinterpret_cast<uintptr_t>(*entity),
                std::memory_order_relaxed);
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void RefreshOniTransformationLiveState() {
    // The start/end callbacks are useful event cues, but endSoulBoostEffect is
    // not guaranteed on every forced exit or scene transition. Poll the same
    // authoritative fields used by the attack hooks so a stale start event can
    // never make ordinary R1 input look like the Oni one-cut finisher.
    bool active = false;
    bool ordinary = false;
    auto* entity = reinterpret_cast<API::ManagedObject*>(
        g_playerCharacterEntity.load(std::memory_order_relaxed));
    if (entity != nullptr) {
        auto** supporterField = entity->get_field<API::ManagedObject*>(
            "<SoulBoostSupporter>k__BackingField");
        auto* supporter = supporterField != nullptr ? *supporterField : nullptr;
        if (supporter != nullptr) {
            auto* oni = supporter->get_field<bool>("_SoulBoostEffectTypeOni");
            auto* effect = supporter->get_field<bool>("_IsSoulBoostEffectActive");
            active = oni != nullptr && *oni && effect != nullptr && *effect;
            auto* boost = supporter->get_field<bool>("<IsSoulBoost>k__BackingField");
            auto* boostType = supporter->get_field<int32_t>("<BoostType>k__BackingField");
            ordinary = boost != nullptr && boostType != nullptr && oni != nullptr &&
                effect != nullptr && normal_attack_haptics::IsOrdinaryAttackState(
                    *boost, *boostType, *oni, *effect);
        }
    }
    const bool wasOrdinary = g_ordinaryFormActive.exchange(
        ordinary, std::memory_order_release);
    if (wasOrdinary != ordinary) {
        API::get()->log_info(
            "[CodexDualSenseBridge] ordinaryForm live=%u source=present-fields",
            ordinary ? 1u : 0u);
    }
    const bool previous = g_oniTransformationActive.exchange(
        active, std::memory_order_release);
    if (previous != active) {
        API::get()->log_info(
            "[CodexDualSenseBridge] oniTransformation live=%u source=present-fields",
            active ? 1u : 0u);
    }
}

struct RainRoofVec3 {
    float x{};
    float y{};
    float z{};
    float w{};
};

void LogRainRoofProbeStatus(int status, const char* detail) {
#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
    const int previous = g_lastRainRoofProbeStatus.exchange(
        status, std::memory_order_relaxed);
    if (previous != status) {
        API::get()->log_info(
            "[CodexDualSenseBridge] rainRoof status=%d detail=%s",
            status, detail);
    }
#else
    (void)status;
    (void)detail;
#endif
}

bool PollRainRoofCover(ULONGLONG now, bool& known) {
    known = false;
    if (now - g_lastRainRoofPollTick < 180) {
        known = g_playerCharacterEntity.load(std::memory_order_relaxed) != 0;
        return g_rainRoofCovered;
    }
    g_lastRainRoofPollTick = now;

    auto* entity = reinterpret_cast<API::ManagedObject*>(
        g_playerCharacterEntity.load(std::memory_order_relaxed));
    if (entity == nullptr) {
        LogRainRoofProbeStatus(0, "no-player-entity");
        return false;
    }
    auto* ownerField = entity->get_field<API::ManagedObject*>("_Owner");
    if (ownerField == nullptr || *ownerField == nullptr) {
        LogRainRoofProbeStatus(1, "no-player-owner");
        return false;
    }

    struct Access {
        API::TypeDefinition* queryType{};
        API::TypeDefinition* filterType{};
        API::Method* getTransform{};
        API::Method* getPosition{};
        API::Method* setRay{};
        API::Method* clearOptions{};
        API::Method* enableAllHits{};
        API::Method* enableNearSort{};
        API::Method* enableInsideHits{};
        API::Method* getFilterInfo{};
        API::Method* setFilterInfo{};
        API::Method* setFilterGroup{};
        API::Method* setFilterMaskBits{};
        API::Method* setFilterLayer{};
        API::Method* castRay{};
        bool castRayUsesResultArgument{};
        int32_t sceneQueryLayer{3};
        API::Method* getNumContactPoints{};
    };
    static const Access access = [] {
        Access value{};
        auto* tdb = API::get()->tdb();
        auto* gameObjectType = tdb->find_type("via.GameObject");
        auto* transformType = tdb->find_type("via.Transform");
        value.queryType = tdb->find_type("via.physics.CastRayQuery");
        value.filterType = tdb->find_type("via.physics.FilterInfo");
        auto* resultType = tdb->find_type("via.physics.CastRayResult");
        auto* physicsType = tdb->find_type("via.physics.System");
        value.getTransform = gameObjectType != nullptr
            ? gameObjectType->find_method("get_Transform") : nullptr;
        value.getPosition = transformType != nullptr
            ? transformType->find_method("get_Position") : nullptr;
        if (value.queryType != nullptr) {
            for (auto* method : value.queryType->get_methods()) {
                if (method != nullptr && method->get_name() != nullptr &&
                    std::strcmp(method->get_name(), "setRay") == 0 &&
                    method->get_num_params() == 2) {
                    value.setRay = method;
                    break;
                }
            }
            value.clearOptions = value.queryType->find_method("clearOptions");
            value.enableAllHits = value.queryType->find_method("enableAllHits");
            value.enableNearSort = value.queryType->find_method("enableNearSort");
            value.enableInsideHits = value.queryType->find_method("enableInsideHits");
            value.getFilterInfo = value.queryType->find_method("get_FilterInfo");
            value.setFilterInfo = value.queryType->find_method("set_FilterInfo");
        }
        if (value.filterType != nullptr) {
            value.setFilterGroup = value.filterType->find_method("set_Group");
            value.setFilterMaskBits = value.filterType->find_method("set_MaskBits");
            value.setFilterLayer = value.filterType->find_method("set_Layer");
        }
        if (physicsType != nullptr) {
            for (auto* method : physicsType->get_methods()) {
                if (method == nullptr || method->get_name() == nullptr ||
                    std::strcmp(method->get_name(), "castRay") != 0) {
                    continue;
                }
                if (method->get_num_params() == 2) {
                    value.castRay = method;
                    value.castRayUsesResultArgument = true;
                    break;
                }
                if (method->get_num_params() == 1 && value.castRay == nullptr) {
                    value.castRay = method;
                }
            }
        }
        if (auto* layerType = tdb->find_type("app.CollisionManager.Layer")) {
            for (auto* field : layerType->get_fields()) {
                if (field != nullptr && field->is_literal() &&
                    field->get_name() != nullptr &&
                    std::strcmp(field->get_name(), "Bullet") == 0 &&
                    field->get_init_data() != nullptr) {
                    value.sceneQueryLayer = *static_cast<const int32_t*>(
                        field->get_init_data());
                    break;
                }
            }
        }
        value.getNumContactPoints = resultType != nullptr
            ? resultType->find_method("get_NumContactPoints") : nullptr;
        return value;
    }();
    if (access.queryType == nullptr || access.getTransform == nullptr ||
        access.getPosition == nullptr || access.setRay == nullptr ||
        access.castRay == nullptr || access.getNumContactPoints == nullptr) {
        LogRainRoofProbeStatus(2, "missing-metadata");
        return false;
    }

    const auto transformResult = access.getTransform->invoke(
        *ownerField, std::vector<void*>{});
    if (transformResult.exception_thrown || transformResult.ptr == nullptr) {
        LogRainRoofProbeStatus(3, "transform-failed");
        return false;
    }
    auto* transform = reinterpret_cast<API::ManagedObject*>(transformResult.ptr);
    const auto positionResult = access.getPosition->invoke(
        transform, std::vector<void*>{});
    if (positionResult.exception_thrown) {
        LogRainRoofProbeStatus(4, "position-failed");
        return false;
    }
    RainRoofVec3 start{};
    std::memcpy(&start, positionResult.bytes.data(), sizeof(start));
    if (!std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(start.z)) {
        LogRainRoofProbeStatus(5, "position-invalid");
        return false;
    }
    start.y += 1.25f;
    RainRoofVec3 end = start;
    end.y += 60.0f;

    auto* query = access.queryType->create_instance();
    if (query == nullptr) {
        LogRainRoofProbeStatus(6, "query-create-failed");
        return false;
    }
    const auto setRayResult = access.setRay->invoke(
        query, std::vector<void*>{&start, &end});
    if (setRayResult.exception_thrown) {
        LogRainRoofProbeStatus(7, "set-ray-failed");
        return false;
    }
    if (access.clearOptions != nullptr) {
        access.clearOptions->invoke(query, std::vector<void*>{});
    }
    if (access.enableAllHits != nullptr) {
        access.enableAllHits->invoke(query, std::vector<void*>{});
    }
    if (access.enableNearSort != nullptr) {
        access.enableNearSort->invoke(query, std::vector<void*>{});
    }
    if (access.enableInsideHits != nullptr) {
        access.enableInsideHits->invoke(query, std::vector<void*>{});
    }
    if (access.getFilterInfo != nullptr && access.setFilterInfo != nullptr &&
        access.setFilterGroup != nullptr && access.setFilterMaskBits != nullptr &&
        access.setFilterLayer != nullptr) {
        const auto filterResult = access.getFilterInfo->invoke(
            query, std::vector<void*>{});
        if (!filterResult.exception_thrown && filterResult.ptr != nullptr) {
            auto* filter = reinterpret_cast<API::ManagedObject*>(filterResult.ptr);
            access.setFilterGroup->invoke(
                filter, std::vector<void*>{reinterpret_cast<void*>(0)});
            access.setFilterMaskBits->invoke(
                filter,
                std::vector<void*>{reinterpret_cast<void*>(
                    static_cast<uintptr_t>(0xFFFFFFFFFFFFFFFEull))});
            access.setFilterLayer->invoke(
                filter, std::vector<void*>{reinterpret_cast<void*>(
                    static_cast<intptr_t>(access.sceneQueryLayer))});
            access.setFilterInfo->invoke(query, std::vector<void*>{filter});
        }
    }

    auto* physics = reinterpret_cast<API::ManagedObject*>(
        API::get()->get_native_singleton("via.physics.System"));
    if (physics == nullptr) {
        LogRainRoofProbeStatus(8, "physics-singleton-missing");
        return false;
    }
    API::ManagedObject* result = nullptr;
    reframework::InvokeRet castResult{};
    if (access.castRayUsesResultArgument) {
        result = access.queryType != nullptr && access.filterType != nullptr
            ? API::get()->tdb()->find_type("via.physics.CastRayResult")->create_instance()
            : nullptr;
        if (result == nullptr) {
            LogRainRoofProbeStatus(9, "result-create-failed");
            return false;
        }
        castResult = access.castRay->invoke(
            physics, std::vector<void*>{query, result});
    } else {
        castResult = access.castRay->invoke(
            physics, std::vector<void*>{query});
        result = reinterpret_cast<API::ManagedObject*>(castResult.ptr);
    }
    if (castResult.exception_thrown || result == nullptr) {
        LogRainRoofProbeStatus(9, "cast-ray-failed");
        return false;
    }
    const auto countResult = access.getNumContactPoints->invoke(
        result, std::vector<void*>{});
    if (countResult.exception_thrown) {
        LogRainRoofProbeStatus(10, "contact-count-failed");
        return false;
    }

    known = true;
    LogRainRoofProbeStatus(11, "success");
    g_rainRoofCovered = countResult.dword > 0;
#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
    const int covered = g_rainRoofCovered ? 1 : 0;
    if (covered != g_lastLoggedRainRoofCovered) {
        g_lastLoggedRainRoofCovered = covered;
        API::get()->log_info(
            "[CodexDualSenseBridge] rainRoof covered=%d contacts=%u layer=%d castArgs=%d pos=(%.2f,%.2f,%.2f)",
            covered, countResult.dword, access.sceneQueryLayer,
            access.castRayUsesResultArgument ? 2 : 1,
            start.x, start.y, start.z);
    }
#endif
    return g_rainRoofCovered;
}

void PublishSyntheticEnvironment() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastEnvironmentPollTick < 100) return;
    g_lastEnvironmentPollTick = now;

    float rainLevel = 0.0f;
    int32_t weather = 0;
    bool rainy = false;
    bool appWeatherAvailable = false;
    const char* rainSource = "unavailable";
    if (auto* weatherManager = API::get()->get_managed_singleton(
            "app.AppWeatherManager")) {
        const std::vector<void*> noArguments;
        if (g_isRainyWeatherGetter != nullptr) {
            const reframework::InvokeRet result =
                g_isRainyWeatherGetter->invoke(weatherManager, noArguments);
            if (!result.exception_thrown) {
                rainy = result.byte != 0;
                appWeatherAvailable = true;
            }
        }
        if (appWeatherAvailable) rainSource = "AppWeatherManager";
    }

    // The sound manager can lag or stay dry in visual-rain scenes, but it is
    // still useful as a fallback for older builds and localized rain zones.
    if (auto* environment = API::get()->get_managed_singleton(
            "app.SoundEnvironmentManager")) {
        if (auto* value = environment->get_field<bool>("_IsWeatherRainy")) {
            if (*value) {
                rainy = true;
                if (!appWeatherAvailable) rainSource = "SoundEnvironmentManager";
            }
        }
        if (auto* value = environment->get_field<int32_t>("_CurrentWeather")) {
            weather = *value;
        }
    }
    // WeatherDef.ID: Rainy=5, HeavyRainy=8. Keep ordinary rain below heavy
    // rain so combat impulses retain clear dynamic headroom.
    if (rainy || weather == 5 || weather == 8) {
        rainLevel = weather == 8 ? 1.0f : 0.62f;
    }

    // Global weather remains rainy under cover, while the rain VFX executor
    // receives the actual camera/player-local fade used to hide rain beneath
    // roofs and inside buildings. Mirror that authored visibility into the
    // haptic level. Keep unknown (-1) as full rain so a missing executor never
    // disables outdoor feedback. A dry weather transition clears stale scene
    // data before the next rainy area loads.
    if (rainLevel <= 0.0f) {
        g_rainVisualAlpha.store(-1.0f, std::memory_order_relaxed);
    } else {
        const float rainVisualAlpha =
            g_rainVisualAlpha.load(std::memory_order_relaxed);
        if (rainVisualAlpha >= 0.0f) {
            rainLevel *= std::clamp(rainVisualAlpha, 0.0f, 1.0f);
            if (rainVisualAlpha < 0.995f) {
                rainSource = "rain-vfx-fade";
            }
        }
    }
    if (rainLevel > 0.0f) {
        bool roofKnown = false;
        if (PollRainRoofCover(now, roofKnown) && roofKnown) {
            rainLevel = 0.0f;
            rainSource = "roof-ray";
        }
    }

    float windRaw = 0.0f;
    bool windUsesEnumScale = false;
    std::string windValueType{"unavailable"};
    if (auto* windType = API::get()->tdb()->find_type("app.SoundWindManager")) {
        if (auto* windField = windType->find_field(
                "<WindPower>k__BackingField")) {
            if (void* data = windField->get_data_raw(nullptr, false)) {
                // WIND_POWER is a 32-bit enum in this game, not a float.  The
                // earlier float reinterpretation turned POWER_1..POWER_15 into
                // denormalized values that were subsequently rounded to zero.
                // Keep a System.Single fallback for engine builds that expose
                // an already-normalized game parameter instead.
                if (auto* fieldType = windField->get_type()) {
                    windValueType = fieldType->get_full_name();
                    if (windValueType.empty() && fieldType->get_name() != nullptr) {
                        windValueType = fieldType->get_name();
                    }
                }
                if (windValueType.find("System.Single") != std::string::npos) {
                    std::memcpy(&windRaw, data, sizeof(windRaw));
                } else if (windValueType.find("WIND_POWER") != std::string::npos) {
                    windUsesEnumScale = true;
                    int32_t windPower = 0;
                    std::memcpy(&windPower, data, sizeof(windPower));
                    // POWER_0..POWER_15 are valid intensity values. MAX (16)
                    // and POWER_ZERO (17) are sentinels, not stronger wind.
                    if (windPower >= 0 && windPower <= 15) {
                        windRaw = static_cast<float>(windPower);
                    }
                }
            }
        }
    }
    if (!std::isfinite(windRaw)) windRaw = 0.0f;
    // Enum POWER_1 means 1/15 strength; only genuine System.Single fields use
    // the already-normalized 0..1 convention.
    float windLevel = windUsesEnumScale || windRaw > 1.01f
        ? std::clamp(windRaw / 15.0f, 0.0f, 1.0f)
        : std::clamp(windRaw, 0.0f, 1.0f);
    if (windLevel < 0.08f) windLevel = 0.0f;

#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
    if (g_rainPreviewEnabled.load(std::memory_order_relaxed)) {
        rainLevel = 0.62f;
        rainSource = "F8-preview";
    }
#endif

    // Environment weather keeps updating behind menus. Do not republish that
    // bed after the pause transition has cleared haptics; resume it only when
    // gameplay is active again.
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        rainLevel = 0.0f;
        windLevel = 0.0f;
        rainSource = "paused";
    }

    const bool changed =
        std::fabs(rainLevel - g_lastRainLevel) >= 0.01f ||
        std::fabs(windLevel - g_lastWindLevel) >= 0.01f;
    if (!changed && now - g_lastEnvironmentSendTick < 1000) return;

    bool sent = false;
    {
        std::lock_guard lock{g_mutex};
        sent = SendEnvironmentLocked(rainLevel, windLevel);
    }
    if (changed) {
        API::get()->log_info(
            "[CodexDualSenseBridge] environment rain=%.3f weather=%d source=%s wind=%.3f raw=%.3f type=%s write=%s",
            rainLevel,
            weather,
            rainSource,
            windLevel,
            windRaw,
            windValueType.c_str(),
            sent ? "ok" : "failed");
    }
    g_lastRainLevel = rainLevel;
    g_lastWindLevel = windLevel;
    g_lastEnvironmentSendTick = now;
}

#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
void RefreshRainPreviewHotkey() {
    const bool held = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (held && !g_rainPreviewKeyHeld) {
        const bool enabled = !g_rainPreviewEnabled.load(std::memory_order_relaxed);
        g_rainPreviewEnabled.store(enabled, std::memory_order_relaxed);
        g_lastEnvironmentSendTick = 0;
        g_lastRainLevel = -1.0f;
        API::get()->log_info(
            "[CodexDualSenseBridge] rainPreview enabled=%d hotkey=F8",
            enabled ? 1 : 0);
    }
    g_rainPreviewKeyHeld = held;
}
#endif

void OnPresent() {
    EnsureAdaptiveEventSubscriptions();
    RefreshGameplayHapticsPauseState();
    RefreshOniTransformationLiveState();
    RefreshBowEquippedPrearm();
    RefreshBowChargePhase();
#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
    RefreshRainPreviewHotkey();
#endif
    RefreshTreasureChestNativeMotorFallback();
    PublishSyntheticEnvironment();
    PublishPendingInteractionFeedback();
    PublishPendingDoorFeedback();
    PublishPendingTreasureChestOpenFeedback();
    PublishPendingTreasureChestPushFeedback();
    PublishPendingTreasureChestLidLandFeedback();
    RefreshContinuousPlayerHaptics();
    // Monitor only slots with a successful game rumble write. Do not invent
    // authority merely because some other connected pad can be enumerated.
    const ULONGLONG motorNow = GetTickCount64();
    if (motorNow - g_lastMotorHeartbeatTick >= live_motor_state::HeartbeatMs) {
        g_lastMotorHeartbeatTick = motorNow;
        std::lock_guard lock{g_mutex};
        if (g_originalXInputGetState != nullptr && motorNow - g_lastMotorConnectionPollTick >= 250) {
            g_lastMotorConnectionPollTick = motorNow;
            for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
                if (!g_liveMotorSources.HasXInput(user)) continue;
                XINPUT_STATE state{};
                if (g_originalXInputGetState(user, &state) != ERROR_SUCCESS) {
                    g_liveMotorSources.RemoveXInput(user);
                    API::get()->log_info("[CodexDualSenseBridge] liveMotor disconnected xinput=%lu", user);
                }
            }
        }
        PublishMotorPowerLocked(true);
    }
    if (g_xinputSetStateIatSlot != nullptr) {
        const ULONG_PTR setHook =
            reinterpret_cast<ULONG_PTR>(&HookedXInputSetState);
        if (*g_xinputSetStateIatSlot != setHook &&
            PatchXInputSetStateImport(g_xinputSetStateIatSlot)) {
            const uint32_t count = g_xinputRehookCount.fetch_add(1) + 1;
            API::get()->log_info(
                "[CodexDualSenseBridge] Restored XInputSetState import hook count=%u",
                count);
        }
    }
    if (g_xinputGetStateIatSlot != nullptr) {
        const ULONG_PTR getHook =
            reinterpret_cast<ULONG_PTR>(&HookedXInputGetState);
        if (*g_xinputGetStateIatSlot != getHook &&
            PatchXInputGetStateImport(g_xinputGetStateIatSlot)) {
            const uint32_t count = g_xinputGetRehookCount.fetch_add(1) + 1;
            API::get()->log_info(
                "[CodexDualSenseBridge] Restored XInputGetState import hook count=%u",
                count);
        }
    }
}

void StopHelperLocked() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        std::array<uint8_t, OutputReportLength> stopHaptics{};
        stopHaptics[0] = HapticStopAllMessage;
        WritePipeLocked(stopHaptics);
        std::array<uint8_t, OutputReportLength> report{};
        report[0] = 0x02;
        report[1] = RightTriggerFlag | LeftTriggerFlag;
        report[RightTriggerOffset] = TriggerEffectOff;
        report[LeftTriggerOffset] = TriggerEffectOff;
        WritePipeLocked(report);
        FlushFileBuffers(g_pipe);
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_helperProcess != nullptr) {
        WaitForSingleObject(g_helperProcess, 1500);
        CloseHandle(g_helperProcess);
        g_helperProcess = nullptr;
    }
}

void PlayHapticTrigger(uint32_t triggerId, float gain) {
    PlayHapticTriggerStereo(triggerId, gain, gain);
}

void PlayHapticTriggerStereo(uint32_t triggerId, float leftGain, float rightGain) {
    if (triggerId == 0 || triggerId == InvalidVibrationId) {
        return;
    }
    std::lock_guard lock{g_mutex};
    const ULONGLONG now = GetTickCount64();
    leftGain = std::clamp(leftGain, 0.0f, 1.0f);
    rightGain = std::clamp(rightGain, 0.0f, 1.0f);
    const bool duplicate = triggerId == g_lastHapticTrigger &&
        std::fabs(leftGain - g_lastHapticLeftGain) < 0.01f &&
        std::fabs(rightGain - g_lastHapticRightGain) < 0.01f &&
        now - g_lastHapticTick <= 80;
    if (duplicate) {
        return;
    }
    g_lastHapticTrigger = triggerId;
    g_lastHapticLeftGain = leftGain;
    g_lastHapticRightGain = rightGain;
    g_lastHapticTick = now;
    const bool sent = SendHapticLocked(triggerId, leftGain, rightGain);

    // Mirror native vibration unchanged. Its reused ID is not evidence for a
    // particular defense sound; explicit success/native sound hooks own cues.
    API::get()->log_info(
        "[CodexDualSenseBridge] hapticTrigger=%u left=%.3f right=%.3f write=%s",
        triggerId,
        leftGain,
        rightGain,
        sent ? "ok" : "failed");
}

void QueueSpatialHaptic(uint32_t triggerId) {
    if (triggerId == 0 || triggerId == InvalidVibrationId) return;
    std::lock_guard lock{g_mutex};
    g_pendingSpatialTrigger = triggerId;
    g_pendingSpatialTick = GetTickCount64();
}

void RefreshEffectLocked() {
    bool sent = false;
    const char* name = "off";
    if ((g_activeEffects & AbsorbEffect) != 0) {
        sent = ApplyAbsorbLocked();
        name = "absorb-left";
    } else if ((g_activeEffects & BowEffectMask) != 0) {
        const bool fullyDrawn =
            (g_activeEffects & BowFullChargeEffect) != 0;
        sent = ApplyBowLocked(fullyDrawn);
        name = fullyDrawn ? "bow-right-full" : "bow-right-pull";
    } else {
        sent = ResetTriggersLocked();
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] effect=%s activeMask=0x%X write=%s",
        name,
        g_activeEffects,
        sent ? "ok" : "failed");
}

void SetEffectState(uint32_t bit, bool enabled) {
    std::lock_guard lock{g_mutex};
    const uint32_t previous = g_activeEffects;
    if (enabled) {
        g_activeEffects |= bit;
    } else {
        g_activeEffects &= ~bit;
    }
    if (g_activeEffects == previous) {
        return;
    }
    RefreshEffectLocked();
}

void SetBowActionState(
    uint32_t actionBit,
    bool enabled,
    bool fullyDrawn,
    bool updateFullDrawState) {
    std::lock_guard lock{g_mutex};
    const uint32_t previous = g_activeEffects;
    if (enabled) {
        g_activeEffects |= actionBit;
    } else {
        g_activeEffects &= ~actionBit;
    }
    if (updateFullDrawState) {
        if (fullyDrawn) {
            g_activeEffects |= BowFullChargeEffect;
        } else {
            g_activeEffects &= ~BowFullChargeEffect;
        }
    }
    if (g_activeEffects != previous) {
        RefreshEffectLocked();
    }
}

void RefreshBowChargePhase() {
    const ULONGLONG started = g_bowPullStartedTick.load(
        std::memory_order_relaxed);
    if (started == 0 ||
        g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - started < BowFullDrawVisualDelayMs) return;

    std::lock_guard lock{g_mutex};
    const uint32_t actionMask = BowMainActionEffect | BowSubActionEffect;
    if ((g_activeEffects & actionMask) != 0 &&
        (g_activeEffects & BowFullChargeEffect) == 0) {
        g_activeEffects |= BowFullChargeEffect;
        RefreshEffectLocked();
        API::get()->log_info(
            "[CodexDualSenseBridge] bowCharge phase=visual-full elapsed=%llu",
            now - started);
    }
    g_bowPullStartedTick.store(0, std::memory_order_relaxed);
}

bool HookBooleanArg(int argc, void** argv, uint32_t bit) {
    if (argc < 3 || argv == nullptr) {
        API::get()->log_warn("[CodexDualSenseBridge] Invalid hook arguments argc=%d", argc);
        return false;
    }
    const bool enabled = reinterpret_cast<uintptr_t>(argv[2]) != 0;
    SetEffectState(bit, enabled);
    return enabled;
}

int PreBowFlagChanged(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    HookBooleanArg(argc, argv, BowEventEffect);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreBowSubWeaponUpdate(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr) {
        UpdateBowEquippedPrearm(
            reinterpret_cast<API::ManagedObject*>(argv[1]));
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PrePlayerActionSelectorUpdateSubWeapon(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr) {
        const uintptr_t selector = reinterpret_cast<uintptr_t>(argv[1]);
        const uintptr_t previous = g_playerActionSelector.exchange(
            selector, std::memory_order_relaxed);
        if (previous == 0) {
            API::get()->log_info(
                "[CodexDualSenseBridge] Bow disable gate captured action selector");
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreAbsorbFlagChanged(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Invalid absorption hook arguments argc=%d",
            argc);
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const bool enabled = reinterpret_cast<uintptr_t>(argv[2]) != 0;
    if (!enabled) {
        g_lastSoulFlowEvidenceTick.store(0, std::memory_order_relaxed);
    }
    SetEffectState(AbsorbEffect, enabled);
    SetSoulAbsorptionHaptic(enabled);
    SetSoulSpeakerState(enabled);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreSoulAbsorptionActionRequested(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Invalid soul action arguments argc=%d",
            argc);
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const bool enabled = reinterpret_cast<uintptr_t>(argv[2]) != 0;
    // Retain this hook as diagnostics for game revisions where it is emitted,
    // but XInput L2 is authoritative: the current build does not dispatch this
    // method for the empty absorption animation.
    API::get()->log_info(
        "[CodexDualSenseBridge] soulAbsorptionAction source=managed observed=%d",
        enabled ? 1 : 0);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreBlackSoulAbsorptionUpdated(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* supporter = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = supporter->get_type_definition();
    if (type == nullptr || type->get_full_name() !=
        "app.cPlayerBlackSoulAbsorptionSupporter")
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const auto* mainState = supporter->get_field<int32_t>(
        "<_MainMysteryAbsorbState>k__BackingField");
    const auto* subState = supporter->get_field<int32_t>(
        "<_SubMysteryAbsorbState>k__BackingField");
    if (mainState == nullptr || subState == nullptr)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int32_t main = *mainState;
    const int32_t sub = *subState;
    const int32_t previousMain = g_lastBlackSoulMainState.exchange(
        main, std::memory_order_relaxed);
    const int32_t previousSub = g_lastBlackSoulSubState.exchange(
        sub, std::memory_order_relaxed);
    const bool paused = g_gameplayHapticsPaused.load(std::memory_order_relaxed);
    const bool absorbing = black_soul_haptics::IsAbsorbing(main, sub, paused);
    g_lastBlackSoulStateTick.store(absorbing ? GetTickCount64() : 0,
        std::memory_order_relaxed);
    const bool previous = g_blackSoulHapticActive.load(std::memory_order_relaxed);
    const bool completed = black_soul_haptics::Completed(previousMain,
        previousSub, main, sub, paused, previous);
    // Observe state every frame, but send a stop/finish only on the edge. A
    // persistent FINISH state must not truncate the one-shot on the next frame.
    if (absorbing || previous) SetBlackSoulAbsorptionHaptic(absorbing, completed);
    if (previous != absorbing) {
        API::get()->log_info(
            "[CodexDualSenseBridge] blackSoulAbsorption state main=%d sub=%d",
            main, sub);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreSoulAbsorptionSuccess(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    // AdaptiveTriggerManager's absorption event is not emitted in the PC
    // runtime path used by Steam Input.  This callback is emitted only when a
    // real soul reaches the player, so use its first event to arm the DSX
    // profile.  SetEffectState is edge-triggered, therefore additional souls
    // refresh the authored haptic accent without restarting trigger resistance.
    if (g_soulActionActive.load(std::memory_order_relaxed) &&
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        g_lastSoulFlowEvidenceTick.store(
            GetTickCount64(), std::memory_order_relaxed);
        SetEffectState(AbsorbEffect, true);
        SetSoulAbsorptionHaptic(true);
        SetSoulSpeakerState(true);
    }
    // Several souls can complete on adjacent frames. Preserve distinct
    // pickups but coalesce duplicate callbacks emitted by the same soul.
    if (BeginEvent(g_lastSoulCollectTick, 28)) {
        PlaySoulAbsorptionAccent();
        PlayControllerSpeakerCue(ControllerSpeakerCue::SoulCollect);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

bool GetBowActionType(
    int argc,
    void** argv,
    std::string& typeName) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return false;
    }
    auto* object = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = object->get_type_definition();
    if (type == nullptr) {
        return false;
    }
    typeName = type->get_full_name();
    return typeName.find("app.PlayerTwoHandedAction.cBow") == 0 ||
        typeName.find("app.PlayerTwoHandedSubAction.cBow") == 0;
}

int PreBowActionEntered(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    std::string typeName;
    if (!GetBowActionType(argc, argv, typeName)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const bool isStart = typeName.find("cBowStart") != std::string::npos;
    const bool isReload = typeName.find("cBowReload") != std::string::npos;
    const bool isFullDraw = typeName.find("cBowLoop") != std::string::npos;
    const bool isShot = typeName.find("cBowShot") != std::string::npos;
    const bool isEnd = typeName.find("cBowEnd") != std::string::npos;
    if (isShot &&
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed) &&
        BeginEvent(g_lastBowShotHapticTick, 70)) {
        bool charged = false;
        {
            std::lock_guard lock{g_mutex};
            charged = (g_activeEffects & BowFullChargeEffect) != 0;
        }
        const auto shotKind = charged
            ? bow_shot_haptics::Kind::Charged
            : bow_shot_haptics::Kind::Quick;
        const bool sent = SendBowShotHaptic(shotKind);
        API::get()->log_info(
            "[CodexDualSenseBridge] bowShotHaptic kind=%s write=%s",
            charged ? "charged" : "quick",
            sent ? "ok" : "failed");
    }
    // Keep the pull resistance armed through Shot. Clearing it here makes the
    // next press outrun the HID update, so the resistance is only felt after
    // the finger is already halfway down. End is the actual bow-cycle stop.
    const bool enabled = !isEnd;
    if (isStart || isReload) {
        g_bowPullStartedTick.store(GetTickCount64(), std::memory_order_relaxed);
    } else if (isFullDraw || isShot || isEnd) {
        g_bowPullStartedTick.store(0, std::memory_order_relaxed);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] bowAction=%s phase=enter enabled=%d",
        typeName.c_str(),
        enabled ? 1 : 0);
    const uint32_t actionBit =
        typeName.find("app.PlayerTwoHandedSubAction.cBow") == 0
            ? BowSubActionEffect
            : BowMainActionEffect;
    if (isEnd) {
        std::lock_guard lock{g_mutex};
        const uint32_t previous = g_activeEffects;
        // End closes the draw/shot state, but the independently polled bow
        // selection remains armed so the next press cannot outrun the HID
        // update.
        g_activeEffects &= ~BowActionEffectMask;
        if (g_activeEffects != previous) RefreshEffectLocked();
    } else {
        SetBowActionState(
            actionBit,
            enabled,
            isFullDraw,
            isStart || isReload || isFullDraw || isShot);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreBowActionExited(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    std::string typeName;
    if (!GetBowActionType(argc, argv, typeName)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] bowAction=%s phase=exit enabled=0",
        typeName.c_str());
    const uint32_t actionBit =
        typeName.find("app.PlayerTwoHandedSubAction.cBow") == 0
            ? BowSubActionEffect
            : BowMainActionEffect;
    // Shot exit only ends the projectile sub-state; the player remains in the
    // bow cycle and may already be holding R2 for the next draw. Preserve the
    // pre-armed resistance until Reload/Start continues or BowEnd clears it.
    if (typeName.find("cBowShot") != std::string::npos) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const bool exitsFullDraw = typeName.find("cBowLoop") != std::string::npos;
    SetBowActionState(
        actionBit,
        false,
        false,
        exitsFullDraw);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreReset(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    CancelNormalAttackHaptics();
    {
        std::lock_guard lock{g_mutex};
        g_activeEffects = 0;
        RefreshEffectLocked();
    }
    g_bowPullStartedTick.store(0, std::memory_order_relaxed);
    g_lastBowShotHapticTick.store(0, std::memory_order_relaxed);
    g_bowTriggerSuppressedByLeftTrigger.store(false, std::memory_order_relaxed);
    g_soulActionActive.store(false, std::memory_order_relaxed);
    for (auto& held : g_soulActionInputHeld) {
        held.store(false, std::memory_order_relaxed);
    }
    g_soulSearchSuppressedUserMask.store(0, std::memory_order_relaxed);
    g_lastParryInputTick.store(0, std::memory_order_relaxed);
    g_lastSoulFlowEvidenceTick.store(0, std::memory_order_relaxed);
    g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
    g_lastBlackSoulStateTick.store(0, std::memory_order_relaxed);
    SetBlackSoulAbsorptionHaptic(false);
    g_lastBlackSoulMainState.store(0, std::memory_order_relaxed);
    g_lastBlackSoulSubState.store(0, std::memory_order_relaxed);
    SetSoulSearchHaptic(false);
    SetSoulAbsorptionHaptic(false);
    SetSoulSpeakerState(false);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PlayTitleHaptic(uint8_t cue) {
    std::lock_guard lock{g_mutex};
    std::array<uint8_t, OutputReportLength> message{};
    message[0] = TitleHapticMessage;
    message[1] = cue;
    SendReportLocked(message);
}

int PreTitleEnter(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_titleActive.store(true);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int PreTitleLeave(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_titleActive.store(false);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int PreTitleOpen(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_titleActive.store(true);
    if (BeginEvent(g_lastTitleAccent, 500)) PlayTitleHaptic(3);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int PreTitleStartMenu(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_titleActive.load() && BeginEvent(g_lastTitleAccent, 500)) PlayTitleHaptic(4);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreTitleDecision(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (BeginEvent(g_lastTitleDecision, 150)) {
        API::get()->log_info("[CodexDualSenseBridge] Title decision callback");
        PlayTitleHaptic(1);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreTitleVisible(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!argv || !argv[1]) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const auto now = GetTickCount64();
    const auto last = g_lastTitleVisible.exchange(now);
    g_titleActive.store(true);
    auto* gui = reinterpret_cast<API::ManagedObject*>(argv[1]);
    static uintptr_t owner = 0;
    static int32_t previousIndex = -1;
    static bool pressEnded = false;
    static bool initialized = false;
    if (owner != reinterpret_cast<uintptr_t>(gui) || last == 0 || now - last > 500) {
        owner = reinterpret_cast<uintptr_t>(gui);
        previousIndex = -1;
        initialized = false;
        API::get()->log_info("[CodexDualSenseBridge] Title visible polling active");
    }
    auto* pressField = gui->get_field<API::ManagedObject*>("_PressAnyButtonMenu");
    if (pressField && *pressField) {
        auto* ended = (*pressField)->get_field<bool>("_IsEnd");
        if (ended) {
            if (initialized && !pressEnded && *ended && BeginEvent(g_lastTitleAccent, 500)) {
                PlayTitleHaptic(4);
                API::get()->log_info("[CodexDualSenseBridge] Title press-any-button completed");
            }
            pressEnded = *ended;
        }
    }
    initialized = true;
    auto* menuField = gui->get_field<API::ManagedObject*>("_TitleMenu");
    if (!menuField || !*menuField) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* inputField = (*menuField)->get_field<API::ManagedObject*>("_InputCtrl");
    if (!inputField || !*inputField) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* input = *inputField;
    auto* controlField = input->get_field<API::ManagedObject*>("_Ctrl");
    if (!controlField || !*controlField) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* control = *controlField;
    auto* type = control->get_type_definition();
    auto* getter = type->find_method("get_SelectedIndex");
    if (!getter) getter = type->find_method("get_SelectIndex");
    static bool reported = false;
    if (!reported) {
        reported = true;
        API::get()->log_info("[CodexDualSenseBridge] Title list type=%s getter=%s", type->get_full_name().c_str(), getter ? getter->get_name() : "missing");
        if (!getter) for (auto* method : type->get_methods()) {
            if (method && std::string(method->get_name()).find("Select") != std::string::npos)
                API::get()->log_info("[CodexDualSenseBridge] Title list accessor %s params=%u", method->get_name(), method->get_num_params());
        }
    }
    if (!getter || getter->get_num_params() != 0) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    reframework::InvokeRet result{};
    const auto invokeStatus = API::get()->sdk()->method->invoke(*getter, control, nullptr, 0, &result, sizeof(result));
    static bool resultReported = false;
    if (!resultReported) {
        resultReported = true;
        API::get()->log_info("[CodexDualSenseBridge] Title index read status=%d exception=%d value=%d return=%s", static_cast<int>(invokeStatus), result.exception_thrown, static_cast<int32_t>(result.dword), getter->get_return_type()->get_full_name().c_str());
    }
    if (invokeStatus != REFRAMEWORK_ERROR_NONE || result.exception_thrown) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int32_t index = static_cast<int32_t>(result.dword);
    if (previousIndex >= 0 && index >= 0 && index != previousIndex &&
        now - g_lastTitleAccent.load() > 300 && BeginEvent(g_lastMenuSelectionTick, 60)) {
        PlayTitleHaptic(0);
        API::get()->log_info("[CodexDualSenseBridge] Title selection %d -> %d", previousIndex, index);
    }
    previousIndex = index;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreMenuSelection(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG navigationTick = g_lastMenuNavigationInputTick.load(
        std::memory_order_relaxed);
    // Animated credits text also raises ctrlSelectionChanged. Only a recent
    // physical navigation input can turn that generic UI callback into a cue.
    if (navigationTick == 0 || now < navigationTick ||
        now - navigationTick > 250u) {
        if (BeginEvent(g_lastMenuSelectionSuppressionLogTick, 2000u))
            API::get()->log_info(
                "[CodexDualSenseBridge] menuSelection suppressed without navigation input");
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    if (g_titleActive.load() && now - g_lastTitleVisible.load() < 250) {
        if (now - g_lastTitleAccent.load() > 300 &&
            now - g_lastTitleDecision.load() > 180 &&
            BeginEvent(g_lastMenuSelectionTick, 60)) PlayTitleHaptic(0);
    } else if (BeginEvent(g_lastMenuSelectionTick, 32)) {
        // A crisp high-frequency detent with a short low-frequency body.  The
        // previous single-lane 18 ms tick fell below the perceptual threshold
        // of the controller's haptics endpoint on the tested system.
        PlayMotorPulse(1, 0.105f, 0.032f, true, 0);
        PlayMotorPulse(0, 0.050f, 0.040f, true, 0);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreLongPressTimerGauge(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    g_longPressUpdateStack.push_back(
        argc >= 2 && argv != nullptr && argv[1] != nullptr
            ? reinterpret_cast<API::ManagedObject*>(argv[1])
            : nullptr);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostLongPressTimerGauge(
    void**,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (g_longPressUpdateStack.empty()) return;
    API::ManagedObject* info = g_longPressUpdateStack.back();
    g_longPressUpdateStack.pop_back();
    if (info == nullptr) return;

    const auto* lapseRate = info->get_field<float>("_LapseRate");
    const auto* selfRate = info->get_field<float>("_SelfAngleRate");
    const auto* isSelf = info->get_field<bool>("_IsSelf");
    const auto* isOnTimer = info->get_field<bool>("_IsOnTimer");
    const auto* isIncrease = info->get_field<bool>("_IsIncrease");
    const auto* vanish = info->get_field<bool>("_VanishLongPress");
    const auto* requestStart =
        info->get_field<bool>("_RequestLongPressSEStart");
    const auto* requestEnd =
        info->get_field<bool>("_RequestLongPressSEEnd");
    if (isOnTimer == nullptr || isIncrease == nullptr) return;

    float rawProgress = 0.0f;
    const bool useSelf = isSelf != nullptr && *isSelf && selfRate != nullptr &&
        std::isfinite(*selfRate);
    if (useSelf) {
        rawProgress = *selfRate;
    } else if (lapseRate != nullptr && std::isfinite(*lapseRate)) {
        rawProgress = *lapseRate;
    } else {
        return;
    }
    const float progress = std::clamp(rawProgress, 0.0f, 1.0f);
    const bool onTimer = *isOnTimer;
    const bool increasing = *isIncrease;
    const bool isVanishing = vanish != nullptr && *vanish;
    const bool startRequested = requestStart != nullptr && *requestStart;
    const bool endRequested = requestEnd != nullptr && *requestEnd;
    const ULONGLONG now = GetTickCount64();
    const uintptr_t key = reinterpret_cast<uintptr_t>(info);

    uint8_t operation = 0;
    float operationProgress = progress;
    bool logStart = false;
    bool logComplete = false;
    bool logCancel = false;
    int32_t logQuarter = -1;
    float maximum = 0.0f;
    {
        std::lock_guard lock{g_longPressMutex};
        if (g_longPressStates.size() >= 64 &&
            g_longPressStates.find(key) == g_longPressStates.end()) {
            for (auto it = g_longPressStates.begin();
                 it != g_longPressStates.end();) {
                if (!it->second.active) {
                    it = g_longPressStates.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto& state = g_longPressStates[key];
        const bool firstSample = !state.initialized;
        const bool progressRestarted = state.initialized && !state.active &&
            increasing && progress > state.lastProgress + 0.002f &&
            state.lastProgress < 0.08f;
        const bool timerStarted = onTimer && increasing &&
            ((!firstSample && !state.wasOnTimer) ||
             (firstSample && progress > 0.002f) || progressRestarted);
        const bool detectedStart = startRequested || timerStarted;
        bool startedNow = false;

        if (!state.active && detectedStart) {
            state.active = true;
            state.maxProgress = progress;
            state.lastSendTick = 0;
            state.lastSendBucket = -1;
            state.lastLogQuarter = 0;
            startedNow = true;
            logStart = true;
        }

        if (state.active) {
            state.maxProgress = std::max(state.maxProgress, progress);
            maximum = state.maxProgress;
            const bool completed = progress >= 0.985f ||
                (endRequested && state.maxProgress >= 0.94f);
            // Some menu implementations, notably equipment enhancement, keep
            // their SE-end/vanish flags asserted while the timer is actively
            // increasing. Treat those flags as an end only after the timer has
            // stopped; otherwise every frame becomes start -> cancel.
            const bool explicitEnd = (endRequested || isVanishing) &&
                (!onTimer || !increasing);
            const bool cancelled = !completed &&
                (explicitEnd || (!onTimer && !startedNow));
            if (completed) {
                operation = 3;
                operationProgress = 1.0f;
                state.active = false;
                state.lastSendBucket = -1;
                logComplete = true;
                g_lastLongPressCompleteTick.store(now, std::memory_order_relaxed);
            } else if (cancelled) {
                operation = 2;
                operationProgress = progress;
                state.active = false;
                state.lastSendBucket = -1;
                logCancel = true;
            } else {
                const int32_t bucket = static_cast<int32_t>(
                    std::floor(progress * 32.0f));
                if (state.lastSendTick == 0 || now - state.lastSendTick >= 28 ||
                    bucket != state.lastSendBucket) {
                    operation = 1;
                    state.lastSendTick = now;
                    state.lastSendBucket = bucket;
                }
                const int32_t quarter = std::min(
                    3,
                    static_cast<int32_t>(std::floor(progress * 4.0f)));
                if (quarter > state.lastLogQuarter) {
                    state.lastLogQuarter = quarter;
                    logQuarter = quarter;
                }
            }
        }

        state.initialized = true;
        state.wasOnTimer = onTimer;
        state.lastProgress = progress;
    }

    const ULONGLONG enhancementVisibleTick =
        g_lastEquipmentEnhanceVisibleTick.load(std::memory_order_relaxed);
    const bool equipmentProfile = enhancementVisibleTick != 0 &&
        now >= enhancementVisibleTick && now - enhancementVisibleTick <= 250;
    if (operation != 0) {
        SendLongPressFeedback(
            operation,
            operationProgress,
            equipmentProfile ? 1 : 0);
    }
    if (logStart) {
        API::get()->log_info(
            "[CodexDualSenseBridge] longPress=start progress=%.3f raw=%.3f self=%d timer=%d increase=%d equipment=%d",
            progress,
            rawProgress,
            useSelf ? 1 : 0,
            onTimer ? 1 : 0,
            increasing ? 1 : 0,
            equipmentProfile ? 1 : 0);
    }
    if (logQuarter > 0) {
        API::get()->log_info(
            "[CodexDualSenseBridge] longPress=progress value=%d%%",
            logQuarter * 25);
    }
    if (logComplete) {
        API::get()->log_info(
            "[CodexDualSenseBridge] longPress=complete max=%.3f",
            maximum);
    } else if (logCancel) {
        API::get()->log_info(
            "[CodexDualSenseBridge] longPress=cancel max=%.3f progress=%.3f",
            maximum,
            progress);
    }
}

int PreEquipmentEnhanceVisible(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    g_lastEquipmentEnhanceVisibleTick.store(
        GetTickCount64(), std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreEquipmentEnhanceGauge(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    g_equipmentEnhanceGaugeStack.push_back(
        argc >= 2 && argv != nullptr
            ? reinterpret_cast<uintptr_t>(argv[1])
            : 0);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostEquipmentEnhanceGauge(
    void** retValue,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (g_equipmentEnhanceGaugeStack.empty()) return;
    const uintptr_t owner = g_equipmentEnhanceGaugeStack.back();
    g_equipmentEnhanceGaugeStack.pop_back();
    if (owner == 0 || retValue == nullptr) return;

    const float rawProgress = FloatArgument(*retValue);
    if (!std::isfinite(rawProgress) || rawProgress < -0.05f ||
        rawProgress > 1.05f) {
        return;
    }
    const float progress = std::clamp(rawProgress, 0.0f, 1.0f);
    const ULONGLONG now = GetTickCount64();
    uint8_t operation = 0;
    float operationProgress = progress;
    bool logStart = false;
    bool logCancel = false;
    int32_t logQuarter = -1;
    float maximum = 0.0f;
    {
        std::lock_guard lock{g_longPressMutex};
        auto& state = g_equipmentEnhanceLongPressState;
        const bool firstSample = !state.initialized;
        const bool increasing = !firstSample &&
            progress > state.lastProgress + 0.002f;
        const bool reset = !firstSample &&
            progress + 0.018f < state.lastProgress;
        const ULONGLONG lastComplete =
            g_lastEquipmentEnhanceCompleteTick.load(std::memory_order_relaxed);
        const bool completionCooldown = lastComplete != 0 &&
            now >= lastComplete && now - lastComplete < 300;

        if (!state.active && !completionCooldown && increasing &&
            progress > 0.004f) {
            state.active = true;
            state.maxProgress = progress;
            state.lastSendTick = 0;
            state.lastSendBucket = -1;
            state.lastLogQuarter = 0;
            logStart = true;
        }

        if (state.active) {
            state.maxProgress = std::max(state.maxProgress, progress);
            maximum = state.maxProgress;
            const bool completed = progress >= 0.985f;
            const bool cancelled = !completed && reset &&
                state.maxProgress > 0.015f;
            if (completed) {
                operation = 3;
                operationProgress = 1.0f;
                state.active = false;
                state.lastSendBucket = -1;
                g_lastLongPressCompleteTick.store(now, std::memory_order_relaxed);
                g_lastEquipmentEnhanceCompleteTick.store(
                    now, std::memory_order_relaxed);
            } else if (cancelled) {
                operation = 2;
                state.active = false;
                state.lastSendBucket = -1;
                logCancel = true;
            } else {
                const int32_t bucket = static_cast<int32_t>(
                    std::floor(progress * 32.0f));
                if (state.lastSendTick == 0 ||
                    now - state.lastSendTick >= 28 ||
                    bucket != state.lastSendBucket) {
                    operation = 1;
                    state.lastSendTick = now;
                    state.lastSendBucket = bucket;
                }
                const int32_t quarter = std::min(
                    3,
                    static_cast<int32_t>(std::floor(progress * 4.0f)));
                if (quarter > state.lastLogQuarter) {
                    state.lastLogQuarter = quarter;
                    logQuarter = quarter;
                }
            }
        }

        state.initialized = true;
        state.lastProgress = progress;
    }

    if (operation != 0) {
        SendLongPressFeedback(operation, operationProgress, 1);
    }
    if (logStart) {
        API::get()->log_info(
            "[CodexDualSenseBridge] equipmentEnhanceLongPress=start progress=%.3f owner=%p",
            progress,
            reinterpret_cast<void*>(owner));
    }
    if (logQuarter > 0) {
        API::get()->log_info(
            "[CodexDualSenseBridge] equipmentEnhanceLongPress=progress value=%d%%",
            logQuarter * 25);
    }
    if (logCancel) {
        API::get()->log_info(
            "[CodexDualSenseBridge] equipmentEnhanceLongPress=cancel max=%.3f",
            maximum);
    }
}

int PreEquipmentEnhanceComplete(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    const ULONGLONG now = GetTickCount64();
    bool alreadyCompleted = false;
    {
        std::lock_guard lock{g_longPressMutex};
        auto& state = g_equipmentEnhanceLongPressState;
        state.active = false;
        state.lastProgress = 0.0f;
        state.maxProgress = 0.0f;
        state.lastSendBucket = -1;
        alreadyCompleted = now - g_lastEquipmentEnhanceCompleteTick.load(
            std::memory_order_relaxed) < 180;
    }
    if (!alreadyCompleted) {
        SendLongPressFeedback(3, 1.0f, 1);
        g_lastLongPressCompleteTick.store(now, std::memory_order_relaxed);
        g_lastEquipmentEnhanceCompleteTick.store(now, std::memory_order_relaxed);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] equipmentEnhanceLongPress=complete duplicate=%d",
        alreadyCompleted ? 1 : 0);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

bool ReadReturnedActionId(void** retVal, uint64_t& packedActionId) {
    packedActionId = 0;
    if (retVal == nullptr || *retVal == nullptr) return false;

    // ACTION_ID is an eight-byte value type. On Win64 this method uses a
    // hidden return buffer and RAX points back to that buffer. Validate the
    // range before reading so a future ABI change cannot turn a direct scalar
    // return into an invalid pointer dereference.
    const uintptr_t address = reinterpret_cast<uintptr_t>(*retVal);
    if (address < 0x10000) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address),
            &region,
            sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) +
        region.RegionSize;
    if (address > regionEnd || regionEnd - address < sizeof(packedActionId)) {
        return false;
    }
    std::memcpy(&packedActionId, reinterpret_cast<const void*>(address),
        sizeof(packedActionId));
    return true;
}

void PostOniStrengthSelected(
    void** retVal,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    uint64_t packedActionId = 0;
    if (!ReadReturnedActionId(retVal, packedActionId) || packedActionId == 0 ||
        !BeginEvent(g_lastOniStrengthTick, 520)) {
        return;
    }

    PlayOniStrengthHaptic(0);
    // The audio-actuator waveform carries the charge/release texture.  Add a
    // compact low/high motor-derived core so the actual blow remains readable
    // on controllers whose haptics audio endpoint has conservative gain.
    PlayMotorPulse(0, 0.34f, 0.135f, true, 12);
    PlayMotorPulse(1, 0.28f, 0.085f, true, 12);
    API::get()->log_info(
        "[CodexDualSenseBridge] oniStrength=destroy actionCategory=%d actionIndex=%d",
        static_cast<int32_t>(packedActionId & 0xFFFFFFFFu),
        static_cast<int32_t>(packedActionId >> 32));
}

int PreOniStrengthBarrierInteraction(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr ||
        !BeginEvent(g_lastOniStrengthBarrierTick, 1200)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    // Runtime metadata narrows IBreakableInteractGimmick to Gm019, Gm025 and
    // Gm051_000. Gm019 is the explosive/throwable family and Gm051_000 is the
    // pole cluster; only Gm025 owns the HOLD/BREAK/EFFECT/SLAM interaction
    // state machine used by the Oni-strength barrier path.
    PlayOniStrengthHaptic(1);
    // Barrier rupture needs a firmer physical centre than the longer debris
    // waveform alone provides.
    PlayMotorPulse(0, 0.42f, 0.190f, true, 12);
    PlayMotorPulse(1, 0.34f, 0.120f, true, 12);
    API::get()->log_info(
        "[CodexDualSenseBridge] oniStrength=maliceBarrier interaction=Gm025");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreOniEyeActivation(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (g_gameplayHapticsPaused.load(std::memory_order_acquire) ||
        !BeginEvent(g_lastOniEyeTick, 900)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    PlayOniEyeHaptic();
    API::get()->log_info(
        "[CodexDualSenseBridge] oniEye=sonarActivation bowTriggerGate=physical-L2 source=cPlayerOniEyeActivationSupporter.nortifyStartActivation");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

thread_local std::vector<API::ManagedObject*> g_oniTransformationStartStack;

int PreOniTransformationStart(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    g_oniTransformationStartStack.push_back(
        argc >= 2 && argv != nullptr
            ? reinterpret_cast<API::ManagedObject*>(argv[1])
            : nullptr);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostOniTransformationStart(
    void**,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (g_oniTransformationStartStack.empty()) return;
    auto* supporter = g_oniTransformationStartStack.back();
    g_oniTransformationStartStack.pop_back();
    if (supporter == nullptr ||
        g_gameplayHapticsPaused.load(std::memory_order_acquire)) return;
    auto* oni = supporter->get_field<bool>("_SoulBoostEffectTypeOni");
    auto* active = supporter->get_field<bool>("_IsSoulBoostEffectActive");
    const bool oniStarting = oni != nullptr && *oni && active != nullptr && *active;
    if (!oniStarting || !BeginEvent(g_lastOniTransformationTick, 1200u)) return;
    g_oniTransformationActive.store(true, std::memory_order_release);
    PlayOniTransformationHaptic();
    API::get()->log_info(
        "[CodexDualSenseBridge] oniTransformation=start source=cPlayerSoulBoostSupporter.startSoulBoostEffect");
}

int PreSoulBoostEffectEnd(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr) {
        auto* supporter = reinterpret_cast<API::ManagedObject*>(argv[1]);
        auto* oni = supporter->get_field<bool>("_SoulBoostEffectTypeOni");
        if (oni != nullptr && *oni) {
            g_oniTransformationActive.store(false, std::memory_order_release);
            g_pendingOniChargeReleaseKind.store(0, std::memory_order_relaxed);
            g_pendingOniChargeReleaseTick.store(0, std::memory_order_relaxed);
            g_pendingOniChargeOwner.store(0, std::memory_order_relaxed);
            API::get()->log_info(
                "[CodexDualSenseBridge] oniTransformation=end source=cPlayerSoulBoostSupporter.endSoulBoostEffect");
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

struct NormalAttackEntry { API::ManagedObject* action{}; };
thread_local std::vector<NormalAttackEntry> g_normalAttackEntryStack;

struct FightingSpiritSnapshot {
    API::ManagedObject* entity{};
    API::ManagedObject* supporter{};
    int active{-1}, powerUp{-1}, oni{-1}, effect{-1};
    int32_t boostType{-999};
};

FightingSpiritSnapshot ReadFightingSpiritState(API::ManagedObject* player) {
    FightingSpiritSnapshot state{};
    if (player == nullptr) return state;
    auto** entityField = player->get_field<API::ManagedObject*>("_PlayerCharacterEntity");
    state.entity = entityField != nullptr ? *entityField : nullptr;
    if (state.entity == nullptr) return state;
    auto** supporterField = state.entity->get_field<API::ManagedObject*>("<SoulBoostSupporter>k__BackingField");
    state.supporter = supporterField != nullptr ? *supporterField : nullptr;
    if (state.supporter == nullptr) return state;
    if (auto* value = state.supporter->get_field<bool>("<IsSoulBoost>k__BackingField")) state.active = *value;
    if (auto* value = state.supporter->get_field<bool>("_SoulBoostEffectTypePowerUp")) state.powerUp = *value;
    if (auto* value = state.supporter->get_field<bool>("_SoulBoostEffectTypeOni")) state.oni = *value;
    if (auto* value = state.supporter->get_field<bool>("_IsSoulBoostEffectActive")) state.effect = *value;
    if (auto* value = state.supporter->get_field<int32_t>("<BoostType>k__BackingField")) state.boostType = *value;
    return state;
}

bool ResetOrdinaryLightWaveForNewCharge(uintptr_t action) {
    std::lock_guard lock{g_ordinaryLightWaveMutex};
    const bool previousReleased = g_ordinaryLightWaveStage != 0;
    if (action != g_ordinaryLightWaveAction || previousReleased) {
        g_ordinaryLightWaveAction = action;
        g_ordinaryLightWaveStage = 0;
        g_ordinaryLightWaveTick = 0;
    }
    return previousReleased;
}

bool EmitOrdinaryLightWaveSmall(uintptr_t action, const char* source) {
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard lock{g_ordinaryLightWaveMutex};
        if (g_ordinaryLightWaveAction == action && g_ordinaryLightWaveStage != 0)
            return false;
        g_ordinaryLightWaveAction = action;
        g_ordinaryLightWaveStage = 1;
        g_ordinaryLightWaveTick = now;
    }
    using normal_attack_haptics::Kind;
    using normal_attack_haptics::Operation;
    SendNormalAttackHaptic(Kind::ChargeLight, Operation::ChargeCancel);
    const bool sent = SendNormalAttackHaptic(Kind::ChargeLight,
        Operation::OrdinaryLightWaveSmall);
    API::get()->log_info(
        "[CodexDualSenseBridge] ordinaryLightWave small action=%p source=%s write=%s",
        reinterpret_cast<void*>(action), source, sent ? "ok" : "failed");
    return sent;
}

bool EmitOrdinaryLightWaveLarge(uintptr_t action) {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG delay = 0;
    {
        std::lock_guard lock{g_ordinaryLightWaveMutex};
        if (g_ordinaryLightWaveAction != action || g_ordinaryLightWaveStage != 1 ||
            now < g_ordinaryLightWaveTick || now - g_ordinaryLightWaveTick > 1500u)
            return false;
        delay = now - g_ordinaryLightWaveTick;
        g_ordinaryLightWaveStage = 2;
    }
    const bool sent = SendNormalAttackHaptic(normal_attack_haptics::Kind::ChargeLight,
        normal_attack_haptics::Operation::OrdinaryLightWaveLarge);
    API::get()->log_info(
        "[CodexDualSenseBridge] ordinaryLightWave large action=%p delayMs=%llu write=%s",
        reinterpret_cast<void*>(action), delay, sent ? "ok" : "failed");
    return sent;
}

bool ResetOrdinaryHeavyImpactForNewCharge(uintptr_t action) {
    std::lock_guard lock{g_ordinaryHeavyImpactMutex};
    const bool previousReleased = g_ordinaryHeavyImpactStage != 0;
    if (action != g_ordinaryHeavyImpactAction || previousReleased) {
        g_ordinaryHeavyImpactAction = action;
        g_ordinaryHeavyImpactStage = 0;
        g_ordinaryHeavyDownstrokeTick = 0;
    }
    return previousReleased;
}

bool EmitOrdinaryHeavyDownstroke(uintptr_t action, const char* source) {
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard lock{g_ordinaryHeavyImpactMutex};
        if (g_ordinaryHeavyImpactAction == action && g_ordinaryHeavyImpactStage != 0)
            return false;
        g_ordinaryHeavyImpactAction = action;
        g_ordinaryHeavyImpactStage = 1;
        g_ordinaryHeavyDownstrokeTick = now;
    }
    using normal_attack_haptics::Kind;
    using normal_attack_haptics::Operation;
    SendNormalAttackHaptic(Kind::ChargeHeavy, Operation::ChargeCancel);
    const bool sent = SendNormalAttackHaptic(Kind::ChargeHeavy,
        Operation::OrdinaryHeavyDownstroke);
    API::get()->log_info(
        "[CodexDualSenseBridge] ordinaryHeavyImpact downstroke action=%p source=%s write=%s",
        reinterpret_cast<void*>(action), source, sent ? "ok" : "failed");
    return sent;
}

bool EmitOrdinaryHeavyGroundCrack(uintptr_t action) {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG delay = 0;
    {
        std::lock_guard lock{g_ordinaryHeavyImpactMutex};
        if (g_ordinaryHeavyImpactAction != action || g_ordinaryHeavyImpactStage != 1 ||
            now < g_ordinaryHeavyDownstrokeTick ||
            now - g_ordinaryHeavyDownstrokeTick > 1500u)
            return false;
        delay = now - g_ordinaryHeavyDownstrokeTick;
        g_ordinaryHeavyImpactStage = 2;
    }
    const bool sent = SendNormalAttackHaptic(normal_attack_haptics::Kind::ChargeHeavy,
        normal_attack_haptics::Operation::OrdinaryHeavyGroundCrack);
    API::get()->log_info(
        "[CodexDualSenseBridge] ordinaryHeavyImpact groundCrack action=%p delayMs=%llu write=%s",
        reinterpret_cast<void*>(action), delay, sent ? "ok" : "failed");
    return sent;
}

struct ChargeCheckCall {
    API::ManagedObject* action{};
    normal_attack_haptics::Kind kind{};
    ULONGLONG started{};
    bool ordinary{};
};
thread_local std::vector<ChargeCheckCall> g_chargeCheckStack;

int PreChargeCheckEnd(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace normal_attack_haptics;
    ChargeCheckCall call{};
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr &&
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        call.action = reinterpret_cast<API::ManagedObject*>(argv[1]);
        auto* type = call.action->get_type_definition();
        call.kind = type != nullptr ? ClassifyChargeAction(type->get_full_name()) : Kind::None;
        call.started = GetTickCount64();
        auto* ownerField = call.action->get_field<API::ManagedObject*>("_OwnerCharacter");
        auto* owner = ownerField != nullptr ? *ownerField : nullptr;
        const auto state = ReadFightingSpiritState(owner);
        const bool oniActive = IsOniAttackState(state.oni == 1, state.effect == 1) ||
            g_oniTransformationActive.load(std::memory_order_acquire);
        call.ordinary = state.supporter != nullptr && IsOrdinaryAttackState(
            state.active == 1, state.boostType, state.oni == 1, state.effect == 1);
        if (call.kind != Kind::None && (oniActive || call.ordinary)) {
            Operation operation;
            {
                std::lock_guard lock{g_normalAttackMutex};
                auto& tracker = call.ordinary ? g_ordinaryChargeAttackTracker : g_oniChargeAttackTracker;
                operation = tracker.Observe(call.started,
                    reinterpret_cast<uintptr_t>(call.action), reinterpret_cast<uintptr_t>(
                        owner != nullptr ? owner : call.action),
                    call.kind, true);
            }
            if (call.ordinary) {
                if (operation == Operation::ChargeStart || operation == Operation::ChargeRefresh) {
                    const bool sent = SendNormalAttackHaptic(call.kind, operation);
                    if (operation == Operation::ChargeStart)
                        API::get()->log_info(
                            "[CodexDualSenseBridge] ordinaryCharge managed start kind=%u action=%s write=%s",
                            static_cast<unsigned>(call.kind), type->get_full_name().c_str(),
                            sent ? "ok" : "failed");
                }
            } else if (operation == Operation::ChargeStart) {
                {
                    std::lock_guard lock{g_normalAttackMutex};
                    g_oniChargeAttackTracker.Release(call.started,
                        reinterpret_cast<uintptr_t>(call.action));
                }
                EmitOniChargeRelease(call.kind, "cAttack*Charge.checkEnd-entry", 0u);
                API::get()->log_info(
                    "[CodexDualSenseBridge] oniCharge managed swing kind=%u action=%s",
                    static_cast<unsigned>(call.kind), type->get_full_name().c_str());
            } else if (operation == Operation::ChargeRefresh) {
                // Once the concrete charge attack exists, checkEnd belongs to
                // its swing animation. Do not restart the earlier input-held
                // charge bed while that animation is playing.
            }
        } else {
            call.action = nullptr;
            call.kind = Kind::None;
        }
    }
    g_chargeCheckStack.push_back(call);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostChargeCheckEnd(void** retValue,
    REFrameworkTypeDefinitionHandle, unsigned long long) {
    using namespace normal_attack_haptics;
    if (g_chargeCheckStack.empty()) return;
    const auto call = g_chargeCheckStack.back();
    g_chargeCheckStack.pop_back();
    if (call.action == nullptr || !IsChargeKind(call.kind)) return;
    const bool ended = retValue != nullptr &&
        (reinterpret_cast<uintptr_t>(*retValue) & 0xFFu) != 0;
    if (!ended) return;
    Kind released;
    {
        std::lock_guard lock{g_normalAttackMutex};
        auto& tracker = call.ordinary ? g_ordinaryChargeAttackTracker : g_oniChargeAttackTracker;
        released = tracker.Release(GetTickCount64(),
            reinterpret_cast<uintptr_t>(call.action));
    }
    if (released != Kind::None) {
        if (call.ordinary) {
            const bool sent = released == Kind::ChargeLight
                ? EmitOrdinaryLightWaveSmall(reinterpret_cast<uintptr_t>(call.action),
                    "checkEnd")
                : EmitOrdinaryHeavyDownstroke(reinterpret_cast<uintptr_t>(call.action),
                    "checkEnd");
            if (sent)
                API::get()->log_info(
                    "[CodexDualSenseBridge] ordinaryCharge managed release kind=%u write=%s",
                    static_cast<unsigned>(released), sent ? "ok" : "failed");
        } else
            EmitOniChargeRelease(released, "cAttack*Charge.checkEnd=true",
                GetTickCount64() - call.started);
    }
}

bool IsEnhancedAttackHapticActive(API::ManagedObject* player) {
    // Read the already-established gameplay flags only. No method invocation
    // from inside an attack hook, and no additional/overlapping diagnostic hooks.
    const auto state = ReadFightingSpiritState(player);
    g_oniTransformationActive.store(
        state.oni == 1 && state.effect == 1, std::memory_order_release);
    const bool allowed = normal_attack_haptics::IsEnhancedAttackState(
        state.active == 1, state.boostType, state.oni == 1, state.effect == 1);
    static std::mutex diagnosticMutex;
    static uint64_t previousSignature = UINT64_MAX;
    static ULONGLONG lastLog{};
    const uint64_t signature =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(state.supporter)) ^
        (static_cast<uint64_t>(static_cast<uint32_t>(state.boostType)) << 32u) ^
        (static_cast<uint64_t>(state.active + 1) << 8u) ^
        (static_cast<uint64_t>(state.powerUp + 1) << 10u) ^
        (static_cast<uint64_t>(state.oni + 1) << 12u) ^
        (static_cast<uint64_t>(state.effect + 1) << 14u);
    const auto now = GetTickCount64();
    {
        std::lock_guard lock{diagnosticMutex};
        if (signature != previousSignature || now - lastLog >= 10000u) {
            previousSignature = signature;
            lastLog = now;
            auto* supporterType = state.supporter != nullptr ? state.supporter->get_type_definition() : nullptr;
            API::get()->log_info("[CodexDualSenseBridge] enhancedAttackState entity=%p supporter=%p type=%s active=%d powerUp=%d oni=%d effect=%d boostType=%d allowed=%d source=fields-only",
                state.entity, state.supporter, supporterType != nullptr ? supporterType->get_full_name().c_str() : "<null>",
                state.active, state.powerUp, state.oni, state.effect, state.boostType, allowed ? 1 : 0);
        }
    }
    return allowed;
}

int PreNormalAttackEntered(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace normal_attack_haptics;
    g_normalAttackEntryStack.emplace_back();
    if (argc < 2 || argv == nullptr || argv[1] == nullptr ||
        g_gameplayHapticsPaused.load(std::memory_order_relaxed))
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* action = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = action->get_type_definition();
    if (type == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const auto name = type->get_full_name();
    if (ClassifyAction(name) == Kind::None && ClassifyChargeAction(name) == Kind::None)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    g_normalAttackEntryStack.back().action = action;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostNormalAttackEntered(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    using namespace normal_attack_haptics;
    if (g_normalAttackEntryStack.empty()) return;
    const auto entry = g_normalAttackEntryStack.back();
    g_normalAttackEntryStack.pop_back();
    if (entry.action == nullptr || g_gameplayHapticsPaused.load(std::memory_order_relaxed)) return;
    auto* type = entry.action->get_type_definition();
    if (type == nullptr) return;
    const auto name = type->get_full_name();
    Kind kind = ClassifyAction(name);
    const Kind chargeKind = ClassifyChargeAction(name);
    auto* owner = entry.action->get_field<API::ManagedObject*>("_OwnerCharacter");
    if (owner == nullptr || *owner == nullptr) return;
    if (chargeKind != Kind::None) {
        const auto state = ReadFightingSpiritState(*owner);
        const bool oniActive = IsOniAttackState(state.oni == 1, state.effect == 1);
        const bool ordinary = state.supporter != nullptr && IsOrdinaryAttackState(
            state.active == 1, state.boostType, state.oni == 1, state.effect == 1);
        auto* chargePhase = entry.action->get_field<int32_t>("_Phase");
        if (ordinary && chargeKind == Kind::ChargeLight && chargePhase != nullptr) {
            if (*chargePhase == 1 && ResetOrdinaryLightWaveForNewCharge(
                    reinterpret_cast<uintptr_t>(entry.action))) {
                std::lock_guard lock{g_normalAttackMutex};
                g_ordinaryChargeAttackTracker.Clear();
            }
            auto* attackTrigger = entry.action->get_field<bool>("_IsAttackTrigger");
            if (*chargePhase == 4 && attackTrigger != nullptr && *attackTrigger)
                EmitOrdinaryLightWaveSmall(reinterpret_cast<uintptr_t>(entry.action),
                    "attack-trigger");
        }
        if (ordinary && chargeKind == Kind::ChargeHeavy && chargePhase != nullptr) {
            if (*chargePhase == 1 && ResetOrdinaryHeavyImpactForNewCharge(
                    reinterpret_cast<uintptr_t>(entry.action))) {
                std::lock_guard lock{g_normalAttackMutex};
                g_ordinaryChargeAttackTracker.Clear();
            }
            auto* attackTrigger = entry.action->get_field<bool>("_IsAttackTrigger");
            if (*chargePhase == 4 && attackTrigger != nullptr && *attackTrigger)
                EmitOrdinaryHeavyDownstroke(reinterpret_cast<uintptr_t>(entry.action),
                    "attack-trigger");
        }
        Operation operation;
        {
            std::lock_guard lock{g_normalAttackMutex};
            auto& tracker = ordinary ? g_ordinaryChargeAttackTracker : g_oniChargeAttackTracker;
            operation = tracker.Observe(GetTickCount64(),
                reinterpret_cast<uintptr_t>(entry.action), reinterpret_cast<uintptr_t>(*owner),
                chargeKind, oniActive || ordinary, chargePhase != nullptr && *chargePhase == 4);
        }
        if (operation != Operation::NoEvent) {
            if (ordinary && chargeKind == Kind::ChargeLight &&
                operation == Operation::ChargeRelease) {
                EmitOrdinaryLightWaveSmall(reinterpret_cast<uintptr_t>(entry.action),
                    "phase-end");
                return;
            }
            if (ordinary && chargeKind == Kind::ChargeHeavy &&
                operation == Operation::ChargeRelease) {
                EmitOrdinaryHeavyDownstroke(reinterpret_cast<uintptr_t>(entry.action),
                    "phase-end");
                return;
            }
            const bool sent = SendNormalAttackHaptic(chargeKind, operation);
            if (operation != Operation::ChargeRefresh)
                API::get()->log_info(
                    "[CodexDualSenseBridge] chargeAttack kind=%u operation=%u action=%s phase=%d oni=%d ordinary=%d write=%s",
                    static_cast<unsigned>(chargeKind), static_cast<unsigned>(operation), name.c_str(),
                    chargePhase != nullptr ? *chargePhase : -1, oniActive ? 1 : 0,
                    ordinary ? 1 : 0, sent ? "ok" : "failed");
        }
        return;
    }
    auto* phase = entry.action->get_field<int32_t>("_Phase");
    if (phase == nullptr || kind == Kind::None) return;
    const bool enhancedAttack = IsEnhancedAttackHapticActive(*owner);
    const auto pendingChargeKind = static_cast<Kind>(
        g_pendingOniChargeReleaseKind.load(std::memory_order_relaxed));
    const auto pendingChargeTick = g_pendingOniChargeReleaseTick.load(std::memory_order_relaxed);
    const auto now = GetTickCount64();
    const auto currentState = ReadFightingSpiritState(*owner);
    if (IsChargeKind(pendingChargeKind) && BaseKind(pendingChargeKind) == kind &&
        now >= pendingChargeTick && now - pendingChargeTick <= 800u &&
        IsOniAttackState(currentState.oni == 1, currentState.effect == 1)) {
        g_pendingOniChargeOwner.store(reinterpret_cast<uintptr_t>(*owner), std::memory_order_relaxed);
        API::get()->log_info(
            "[CodexDualSenseBridge] oniChargeInput correlated kind=%u action=%s delayMs=%llu",
            static_cast<unsigned>(pendingChargeKind), name.c_str(), now - pendingChargeTick);
    }
    Operation operation;
    {
        std::lock_guard lock{g_normalAttackMutex};
        // TypeDB PHASE: MOVE=0 (approach), ATTACK=1 (actual swing).
        operation = g_normalAttackTracker.Observe(now, reinterpret_cast<uintptr_t>(entry.action),
            reinterpret_cast<uintptr_t>(*owner), kind,
            *phase == 1 && enhancedAttack);
    }
    if (operation == Operation::NoEvent) {
        static std::atomic<uint32_t> gatedSamples{0};
        if (*phase == 1 && !enhancedAttack &&
            gatedSamples.fetch_add(1, std::memory_order_relaxed) < 12) {
            API::get()->log_info(
                "[CodexDualSenseBridge] normalAttack gated=enhanced-state-inactive action=%s",
                name.c_str());
        }
        return;
    }
    const bool sent = SendNormalAttackHaptic(kind, operation, SwingLeadFrames(name, kind),
        SecondarySwingLeadFrames(name));
    if (operation != Operation::Refresh)
        API::get()->log_info("[CodexDualSenseBridge] normalAttack kind=%u operation=%u action=%s phase=%d soulBoost=%d write=%s internal-always-on",
            static_cast<unsigned>(kind), static_cast<unsigned>(operation), name.c_str(), *phase,
            enhancedAttack ? 1 : 0, sent ? "ok" : "failed");
}

int PreNormalAttackExited(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace normal_attack_haptics;
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    Kind kind, chargeKind, ordinaryChargeKind;
    {
        std::lock_guard lock{g_normalAttackMutex};
        kind = g_normalAttackTracker.Finish(reinterpret_cast<uintptr_t>(argv[1]));
        chargeKind = g_oniChargeAttackTracker.Release(GetTickCount64(), reinterpret_cast<uintptr_t>(argv[1]));
        ordinaryChargeKind = g_ordinaryChargeAttackTracker.Release(GetTickCount64(), reinterpret_cast<uintptr_t>(argv[1]));
    }
    if (kind != Kind::None) {
        const bool sent = SendNormalAttackHaptic(kind, Operation::End);
        API::get()->log_info("[CodexDualSenseBridge] normalAttack exit kind=%u write=%s", static_cast<unsigned>(kind), sent ? "ok" : "failed");
    }
    if (chargeKind != Kind::None) {
        const bool sent = SendNormalAttackHaptic(chargeKind, Operation::ChargeRelease);
        API::get()->log_info("[CodexDualSenseBridge] oniChargeAttack release kind=%u write=%s",
            static_cast<unsigned>(chargeKind), sent ? "ok" : "failed");
    }
    if (ordinaryChargeKind != Kind::None) {
        const bool sent = SendNormalAttackHaptic(ordinaryChargeKind, Operation::ChargeRelease);
        API::get()->log_info("[CodexDualSenseBridge] ordinaryChargeAttack release kind=%u write=%s",
            static_cast<unsigned>(ordinaryChargeKind), sent ? "ok" : "failed");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

struct NormalAttackHitCall { normal_attack_haptics::Kind kind{}; uint32_t uniqueId{}; };
thread_local std::vector<NormalAttackHitCall> g_normalAttackHitStack;
int PreOrdinaryLightChargeSweepProbe(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (argc >= 3 && argv != nullptr && argv[1] != nullptr) {
        auto* action = reinterpret_cast<API::ManagedObject*>(argv[1]);
        auto* type = action->get_type_definition();
        const auto chargeKind = type != nullptr
            ? normal_attack_haptics::ClassifyChargeAction(type->get_full_name())
            : normal_attack_haptics::Kind::None;
        if (normal_attack_haptics::IsChargeKind(chargeKind)) {
            auto* ownerField = action->get_field<API::ManagedObject*>("_OwnerCharacter");
            const auto state = ReadFightingSpiritState(
                ownerField != nullptr ? *ownerField : nullptr);
            if (state.supporter != nullptr && normal_attack_haptics::IsOrdinaryAttackState(
                    state.active == 1, state.boostType, state.oni == 1, state.effect == 1)) {
                if (chargeKind == normal_attack_haptics::Kind::ChargeLight) {
                    EmitOrdinaryLightWaveLarge(reinterpret_cast<uintptr_t>(action));
                } else {
                    EmitOrdinaryHeavyGroundCrack(reinterpret_cast<uintptr_t>(action));
                }
            }
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int PreNormalAttackHit(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace normal_attack_haptics;
    NormalAttackHitCall state{};
    if (argc >= 3 && argv != nullptr && argv[1] != nullptr && argv[2] != nullptr &&
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        auto* behavior = reinterpret_cast<API::ManagedObject*>(argv[1]);
        auto* param = reinterpret_cast<API::ManagedObject*>(argv[2]);
        auto* paramType = param->get_type_definition();
        if (paramType != nullptr && paramType->get_full_name() == "app.cCharacterApplyDamage.cApplyParam") {
            auto* targetField = behavior->get_field<API::ManagedObject*>("_Character");
            auto* infoField = param->get_field<API::ManagedObject*>("DamageInfo");
            auto* target = targetField != nullptr ? *targetField : nullptr;
            auto* info = infoField != nullptr ? *infoField : nullptr;
            auto* attackerField = info != nullptr ? info->get_field<API::ManagedObject*>("<AttackCharacter>k__BackingField") : nullptr;
            auto* attacker = attackerField != nullptr ? *attackerField : nullptr;
            auto* targetType = target != nullptr ? target->get_type_definition() : nullptr;
            auto* damage = param->get_field<float>("Damage");
            auto* health = param->get_field<int32_t>("HealthDecrease");
            auto* uniqueId = param->get_field<uint32_t>("UniqueID");
            const bool damaged = (health != nullptr && *health > 0) ||
                (damage != nullptr && std::isfinite(*damage) && *damage > 0.0f);
            const bool enemy = targetType != nullptr && (targetType->get_full_name() == "app.EnemyCharacter" ||
                targetType->is_derived_from("app.EnemyCharacter"));
            const auto attackState = ReadFightingSpiritState(attacker);
            const bool enhanced = IsEnhancedAttackHapticActive(attacker);
            const bool ordinary = attackState.supporter != nullptr && IsOrdinaryAttackState(
                attackState.active == 1, attackState.boostType,
                attackState.oni == 1, attackState.effect == 1);
            if (enemy && uniqueId != nullptr && (enhanced || ordinary)) {
                state.uniqueId = *uniqueId;
                std::lock_guard lock{g_normalAttackMutex};
                const auto now = GetTickCount64();
                if (enhanced) {
                    state.kind = g_normalAttackTracker.AcceptHit(now, reinterpret_cast<uintptr_t>(attacker),
                        reinterpret_cast<uintptr_t>(target), state.uniqueId, damaged);
                    if (state.kind == Kind::None)
                        state.kind = g_oniChargeAttackTracker.AcceptHit(now, reinterpret_cast<uintptr_t>(attacker),
                            reinterpret_cast<uintptr_t>(target), state.uniqueId, damaged);
                } else {
                    state.kind = g_ordinaryChargeAttackTracker.AcceptHit(now,
                        reinterpret_cast<uintptr_t>(attacker), reinterpret_cast<uintptr_t>(target),
                        state.uniqueId, damaged);
                }
                const Kind pendingKind = static_cast<Kind>(
                    g_pendingOniChargeReleaseKind.load(std::memory_order_relaxed));
                const auto pendingTick = g_pendingOniChargeReleaseTick.load(std::memory_order_relaxed);
                const auto pendingOwner = g_pendingOniChargeOwner.load(std::memory_order_relaxed);
                if (enhanced && IsChargeKind(pendingKind) && damaged && pendingOwner == reinterpret_cast<uintptr_t>(attacker) &&
                    now >= pendingTick && now - pendingTick <= 1200u) {
                    state.kind = pendingKind;
                    if (!BeginEvent(g_lastOniChargeHitTick, 90u)) state.kind = Kind::None;
                }
            }
            static std::atomic<uint32_t> samples{0};
            if (samples.fetch_add(1, std::memory_order_relaxed) < 12)
                API::get()->log_info("[CodexDualSenseBridge] normalAttackHit sample target=%s enemy=%d attacker=%d damage=%.2f hp=%d accepted=%u",
                    targetType != nullptr ? targetType->get_full_name().c_str() : "<null>", enemy ? 1 : 0,
                    attacker != nullptr ? 1 : 0, damage != nullptr ? *damage : 0.0f, health != nullptr ? *health : 0,
                    static_cast<unsigned>(state.kind));
        }
    }
    g_normalAttackHitStack.push_back(state);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void PostNormalAttackHit(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (g_normalAttackHitStack.empty()) return;
    const auto state = g_normalAttackHitStack.back(); g_normalAttackHitStack.pop_back();
    if (state.kind == normal_attack_haptics::Kind::None || g_gameplayHapticsPaused.load(std::memory_order_relaxed)) return;
    const auto operation = normal_attack_haptics::IsChargeKind(state.kind)
        ? normal_attack_haptics::Operation::ChargeHit
        : normal_attack_haptics::Operation::Hit;
    const bool sent = SendNormalAttackHaptic(state.kind, operation);
    API::get()->log_info("[CodexDualSenseBridge] normalAttackHit kind=%u damageId=%u write=%s internal-always-on",
        static_cast<unsigned>(state.kind), state.uniqueId, sent ? "ok" : "failed");
}

API::ManagedObject* GetWallRunAction(int argc, void** argv,
    wall_run_haptics::Phase& phase) {
    phase = wall_run_haptics::Phase::None;
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) return nullptr;
    auto* action = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = action->get_type_definition();
    if (type == nullptr) return nullptr;
    phase = wall_run_haptics::ClassifyAction(type->get_full_name());
    return phase != wall_run_haptics::Phase::None ? action : nullptr;
}

wall_run_haptics::Side ResolveWallRunSide(API::ManagedObject* action) {
    const auto* wallStyle = action->get_field<int32_t>("_WallStyle");
    return wallStyle != nullptr ? wall_run_haptics::FromWallStyle(*wallStyle) :
        wall_run_haptics::Side::Center;
}

int PreWallRunActionEntered(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace wall_run_haptics;
    Phase phase{};
    auto* action = GetWallRunAction(argc, argv, phase);
    if (action == nullptr || g_gameplayHapticsPaused.load(std::memory_order_relaxed))
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const ULONGLONG now = GetTickCount64();
    const uintptr_t object = reinterpret_cast<uintptr_t>(action);
    if (g_lastWallRunEnteredObject.load(std::memory_order_relaxed) == object &&
        now - g_lastWallRunEnteredTick.load(std::memory_order_relaxed) < 120)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL; // derived/base detailEnter pair
    g_lastWallRunEnteredObject.store(object, std::memory_order_relaxed);
    g_lastWallRunEnteredTick.store(now, std::memory_order_relaxed);

    const bool landing = phase == Phase::LandSmall || phase == Phase::LandLarge;
    bool emitLanding = true;
    {
        std::lock_guard lock{g_wallRunLandingMutex};
        if (landing) emitLanding = g_wallRunLandingTracker.AcceptExplicit(now);
        else g_wallRunLandingTracker.ObserveDeparture(phase, now);
    }

    Event event = Event::Detach;
    switch (phase) {
    case Phase::Takeoff: event = Event::Takeoff; break;
    case Phase::Contact:
        event = BeginEvent(g_lastWallRunContactTick, 160) ? Event::Contact : Event::Move;
        break;
    case Phase::Move: event = Event::Move; break;
    case Phase::Leap: event = Event::Leap; break;
    case Phase::GroundLeap: event = Event::GroundLeap; break;
    case Phase::LandSmall: event = Event::LandSmall; break;
    case Phase::LandLarge: event = Event::LandLarge; break;
    case Phase::Fall: event = Event::Detach; break;
    default: break;
    }
    const bool along = AlongWall(phase);
    const bool continuous = along || SustainsFlight(phase);
    g_wallRunAlongWallActive.store(along, std::memory_order_relaxed);
    g_wallRunHapticActive.store(continuous, std::memory_order_relaxed);
    g_wallRunActionObject.store(continuous ? object : 0, std::memory_order_relaxed);
    g_lastWallRunStateTick.store(continuous ? now : 0, std::memory_order_relaxed);
    g_lastWallRunRefreshTick.store(continuous ? now : 0, std::memory_order_relaxed);
    if (landing && !emitLanding) {
        API::get()->log_info("[CodexDualSenseBridge] wallRun landingDuplicate suppressed action=%s",
            action->get_type_definition()->get_full_name().c_str());
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    if (landing)
        g_lastWallRunLandTick.store(now, std::memory_order_relaxed);
    const Side side = ResolveWallRunSide(action);
    const bool sent = SendWallRunHaptic(event, side);
    API::get()->log_info("[CodexDualSenseBridge] wallRun phase=%u event=%u side=%u action=%s write=%s",
        static_cast<unsigned>(phase), static_cast<unsigned>(event), static_cast<unsigned>(side),
        action->get_type_definition()->get_full_name().c_str(), sent ? "ok" : "failed");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreWallRunActionUpdated(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace wall_run_haptics;
    Phase phase{};
    auto* action = GetWallRunAction(argc, argv, phase);
    if (action == nullptr || (!AlongWall(phase) && !SustainsFlight(phase)) ||
        g_gameplayHapticsPaused.load(std::memory_order_relaxed))
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const ULONGLONG now = GetTickCount64();
    const bool along = AlongWall(phase);
    g_wallRunAlongWallActive.store(along, std::memory_order_relaxed);
    g_wallRunActionObject.store(reinterpret_cast<uintptr_t>(action), std::memory_order_relaxed);
    g_lastWallRunStateTick.store(now, std::memory_order_relaxed);
    const bool previous = g_wallRunHapticActive.exchange(true, std::memory_order_relaxed);
    const ULONGLONG refreshed = g_lastWallRunRefreshTick.load(std::memory_order_relaxed);
    if (!previous || now - refreshed >= RefreshIntervalMs) {
        g_lastWallRunRefreshTick.store(now, std::memory_order_relaxed);
        const bool sent = SendWallRunHaptic(along ? Event::Move : Event::AirMove,
            ResolveWallRunSide(action));
        if (!along && BeginEvent(g_lastWallRunAirRefreshLogTick, 1000))
            API::get()->log_info("[CodexDualSenseBridge] wallRun airRefresh phase=%u action=%s write=%s",
                static_cast<unsigned>(phase), action->get_type_definition()->get_full_name().c_str(),
                sent ? "ok" : "failed");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreWallRunActionExited(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    wall_run_haptics::Phase phase{};
    auto* action = GetWallRunAction(argc, argv, phase);
    if (action == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    uintptr_t expected = reinterpret_cast<uintptr_t>(action);
    if (g_wallRunActionObject.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) {
        g_wallRunHapticActive.store(false, std::memory_order_relaxed);
        g_wallRunAlongWallActive.store(false, std::memory_order_relaxed);
        g_lastWallRunStateTick.store(0, std::memory_order_relaxed);
        g_lastWallRunRefreshTick.store(0, std::memory_order_relaxed);
        SendWallRunHaptic(wall_run_haptics::Event::Detach);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

API::ManagedObject* GetLadderAction(int argc, void** argv,
    ladder_haptics::Phase& phase) {
    phase = ladder_haptics::Phase::None;
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) return nullptr;
    auto* action = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = action->get_type_definition();
    if (type == nullptr) return nullptr;
    phase = ladder_haptics::ClassifyAction(type->get_full_name());
    return phase != ladder_haptics::Phase::None ? action : nullptr;
}

ladder_haptics::Profile LadderProfileFor(ladder_haptics::Phase phase) {
    return ladder_haptics::IsFastDown(phase)
        ? ladder_haptics::Profile::FastSlide
        : ladder_haptics::Profile::Climb;
}

float LadderIntensityFor(ladder_haptics::Phase phase) {
    switch (phase) {
    case ladder_haptics::Phase::FastDown: return 0.98f;
    case ladder_haptics::Phase::FastUp: return 0.90f;
    case ladder_haptics::Phase::Start: return 0.78f;
    default: return 0.72f;
    }
}

void MarkLadderInactiveWithoutStop() {
    g_ladderHapticActive.store(false, std::memory_order_relaxed);
    g_lastLadderStateTick.store(0, std::memory_order_relaxed);
    g_lastLadderRefreshTick.store(0, std::memory_order_relaxed);
    g_ladderActionObject.store(0, std::memory_order_relaxed);
}

int PreLadderActionEntered(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace ladder_haptics;
    Phase phase{};
    auto* action = GetLadderAction(argc, argv, phase);
    if (action == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;

    const ULONGLONG now = GetTickCount64();
    const uintptr_t object = reinterpret_cast<uintptr_t>(action);
    const ULONGLONG previousEntered = g_lastLadderEnteredTick.load(
        std::memory_order_relaxed);
    if (g_lastLadderEnteredObject.load(std::memory_order_relaxed) == object &&
        now >= previousEntered && now - previousEntered < 120) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    g_lastLadderEnteredObject.store(object, std::memory_order_relaxed);
    g_lastLadderEnteredTick.store(now, std::memory_order_relaxed);

    const std::string typeName = action->get_type_definition()->get_full_name();
    const Side side = SideFromAction(typeName);
    if (phase == Phase::Idle || phase == Phase::Fall || phase == Phase::Land) {
        StopLadderHaptics();
        API::get()->log_info(
            "[CodexDualSenseBridge] ladder phase=%u stop action=%s",
            static_cast<unsigned>(phase), typeName.c_str());
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    if (phase == Phase::End || phase == Phase::FastStop) {
        const auto profile = static_cast<Profile>(
            g_ladderHapticProfile.load(std::memory_order_relaxed));
        const bool sent = SendLadderHaptic(
            profile,
            Operation::Decelerate,
            side,
            LadderIntensityFor(phase));
        MarkLadderInactiveWithoutStop();
        API::get()->log_info(
            "[CodexDualSenseBridge] ladder phase=%u decelerate action=%s write=%s",
            static_cast<unsigned>(phase), typeName.c_str(), sent ? "ok" : "failed");
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    if (!Sustains(phase) && phase != Phase::Start) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    if (g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const Profile profile = LadderProfileFor(phase);
    const uint8_t oldProfile = g_ladderHapticProfile.load(
        std::memory_order_relaxed);
    const bool wasActive = g_ladderHapticActive.exchange(
        true, std::memory_order_relaxed);
    const bool restart = !wasActive || oldProfile != static_cast<uint8_t>(profile);
    g_ladderHapticProfile.store(static_cast<uint8_t>(profile),
        std::memory_order_relaxed);
    g_ladderActionObject.store(object, std::memory_order_relaxed);
    g_lastLadderStateTick.store(now, std::memory_order_relaxed);
    g_lastLadderRefreshTick.store(now, std::memory_order_relaxed);

    const bool stateSent = SendLadderHaptic(
        profile,
        restart ? Operation::Start : Operation::Refresh,
        side,
        LadderIntensityFor(phase));
    const bool accentSent = SendLadderHaptic(
        profile,
        Operation::Accent,
        side,
        LadderIntensityFor(phase));
    API::get()->log_info(
        "[CodexDualSenseBridge] ladder phase=%u profile=%u side=%u action=%s state=%s accent=%s",
        static_cast<unsigned>(phase), static_cast<unsigned>(profile),
        static_cast<unsigned>(side), typeName.c_str(),
        stateSent ? "ok" : "failed", accentSent ? "ok" : "failed");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreLadderActionUpdated(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    using namespace ladder_haptics;
    Phase phase{};
    auto* action = GetLadderAction(argc, argv, phase);
    if (action == nullptr || !Sustains(phase) ||
        g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const ULONGLONG now = GetTickCount64();
    const std::string typeName = action->get_type_definition()->get_full_name();
    const Side side = SideFromAction(typeName);
    const Profile profile = LadderProfileFor(phase);
    const uint8_t oldProfile = g_ladderHapticProfile.load(
        std::memory_order_relaxed);
    const bool wasActive = g_ladderHapticActive.exchange(
        true, std::memory_order_relaxed);
    g_ladderActionObject.store(reinterpret_cast<uintptr_t>(action),
        std::memory_order_relaxed);
    g_lastLadderStateTick.store(now, std::memory_order_relaxed);
    const ULONGLONG previousRefresh = g_lastLadderRefreshTick.load(
        std::memory_order_relaxed);
    if (!wasActive || oldProfile != static_cast<uint8_t>(profile) ||
        previousRefresh == 0 || now - previousRefresh >= RefreshIntervalMs) {
        g_ladderHapticProfile.store(static_cast<uint8_t>(profile),
            std::memory_order_relaxed);
        g_lastLadderRefreshTick.store(now, std::memory_order_relaxed);
        const auto operation = !wasActive ||
            oldProfile != static_cast<uint8_t>(profile)
            ? Operation::Start : Operation::Refresh;
        const bool sent = SendLadderHaptic(
            profile, operation, side, LadderIntensityFor(phase));
        if (operation == Operation::Start) {
            API::get()->log_info(
                "[CodexDualSenseBridge] ladder updateStart phase=%u profile=%u action=%s write=%s",
                static_cast<unsigned>(phase), static_cast<unsigned>(profile),
                typeName.c_str(), sent ? "ok" : "failed");
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreLadderActionExited(int argc, void** argv,
    REFrameworkTypeDefinitionHandle*, unsigned long long) {
    ladder_haptics::Phase phase{};
    auto* action = GetLadderAction(argc, argv, phase);
    if (action == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    uintptr_t expected = reinterpret_cast<uintptr_t>(action);
    if (g_ladderActionObject.compare_exchange_strong(
            expected, 0, std::memory_order_relaxed)) {
        StopLadderHaptics();
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreShortcutWheelUpdate(
    int,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    auto* wheel = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* currentIndex = wheel->get_field<int32_t>(
        "<SelectedIndex>k__BackingField");
    if (currentIndex == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const uintptr_t wheelKey = reinterpret_cast<uintptr_t>(argv[1]);
    int32_t previousIndex = *currentIndex;
    bool changed = false;
    {
        std::lock_guard lock{g_mutex};
        if (g_shortcutWheelSelections.size() >= 8 &&
            g_shortcutWheelSelections.find(wheelKey) ==
                g_shortcutWheelSelections.end()) {
            g_shortcutWheelSelections.clear();
        }
        auto [entry, inserted] = g_shortcutWheelSelections.try_emplace(
            wheelKey, *currentIndex);
        if (!inserted && entry->second != *currentIndex) {
            previousIndex = entry->second;
            entry->second = *currentIndex;
            changed = true;
        }
    }

    if (!changed || *currentIndex < 0 ||
        !BeginEvent(g_lastShortcutSelectionTick, 38)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    // A compact detent for crossing a radial-wheel segment.  There is no
    // authored vibration preset for this UI event in the PC data.
    PlayMotorPulse(1, 0.135f, 0.032f, true, 0);
    PlayMotorPulse(0, 0.060f, 0.042f, true, 0);
    API::get()->log_info(
        "[CodexDualSenseBridge] shortcutWheelSelection old=%d new=%d",
        previousIndex,
        *currentIndex);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PlayMapMarkerFeedback(bool placed) {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG visibleTick =
        g_lastMapVisibleTick.load(std::memory_order_relaxed);
    if (visibleTick == 0 || now - visibleTick > 120) return;
    if (!BeginEvent(g_lastMapMarkerTick, 110)) return;

    if (placed) {
        // A firm center click followed by a short low confirmation body.
        PlayMotorPulse(1, 0.190f, 0.040f, true, 0);
        PlayMotorPulse(0, 0.110f, 0.065f, true, 12);
    } else {
        // Removal is deliberately softer and weighted to the low actuator.
        PlayMotorPulse(0, 0.145f, 0.052f, true, 0);
        PlayMotorPulse(1, 0.075f, 0.030f, true, 0);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] mapMarker=%s",
        placed ? "placed" : "removed");
}

void UpdateMapHaptics(API::ManagedObject* map, bool includeScale) {
    if (map == nullptr) return;

    if (includeScale) {
        if (auto* enabled = map->get_field<bool>("_IsEnableMap");
            enabled != nullptr && !*enabled) {
            return;
        }
    }

    float scaleProgress = 0.0f;
    bool hasScale = false;
    if (includeScale) {
        if (auto* scale = map->get_field<float>("_MapScaleProgress");
            scale != nullptr && std::isfinite(*scale)) {
            scaleProgress = *scale;
            hasScale = true;
        }
    }

    bool hasCursor = false;
    bool absorbed = false;
    int32_t absorbedIndex = -1;
    if (auto* cursorField = map->get_field<API::ManagedObject*>("_MapCursor");
        cursorField != nullptr && *cursorField != nullptr) {
        auto* cursor = *cursorField;
        auto* isAbsorbed = cursor->get_field<bool>(
            "<IsAbsorbed>k__BackingField");
        auto* index = cursor->get_field<int32_t>(
            "<AbsorbedIndex>k__BackingField");
        if (isAbsorbed != nullptr && index != nullptr) {
            hasCursor = true;
            absorbed = *isAbsorbed;
            absorbedIndex = *index;
        }
    }

    const ULONGLONG now = GetTickCount64();
    const uintptr_t key = reinterpret_cast<uintptr_t>(map);
    bool sendZoom = false;
    bool zoomStarted = false;
    float zoomDirection = 0.0f;
    float zoomIntensity = 0.0f;
    bool focusChanged = false;
    {
        std::lock_guard lock{g_mapMutex};
        if (g_mapHapticStates.size() >= 12 &&
            g_mapHapticStates.find(key) == g_mapHapticStates.end()) {
            g_mapHapticStates.clear();
        }
        auto [entry, inserted] = g_mapHapticStates.try_emplace(key);
        MapHapticState& state = entry->second;

        if (hasScale) {
            if (state.scaleInitialized) {
                const float delta = scaleProgress - state.scaleProgress;
                const float magnitude = std::fabs(delta);
                if (magnitude >= 0.00005f) {
                    const int32_t direction = delta > 0.0f ? 1 : -1;
                    zoomStarted = state.lastZoomMotionTick == 0 ||
                        now - state.lastZoomMotionTick > 110 ||
                        direction != state.lastZoomDirection;
                    state.lastZoomMotionTick = now;
                    state.lastZoomDirection = direction;
                    if (now - state.lastZoomSendTick >= 14) {
                        state.lastZoomSendTick = now;
                        sendZoom = true;
                        zoomDirection = static_cast<float>(direction);
                        zoomIntensity = std::clamp(
                            0.58f + magnitude * 26.0f,
                            0.58f,
                            1.0f);
                    }
                }
            }
            state.scaleProgress = scaleProgress;
            state.scaleInitialized = true;
        }

        if (hasCursor) {
            if (state.cursorInitialized && absorbed &&
                (!state.absorbed || absorbedIndex != state.absorbedIndex)) {
                focusChanged = true;
            }
            state.absorbed = absorbed;
            state.absorbedIndex = absorbedIndex;
            state.cursorInitialized = true;
        }
        (void)inserted;
    }

    if (sendZoom) {
        bool sent = false;
        {
            std::lock_guard lock{g_mutex};
            sent = SendMapZoomLocked(zoomDirection, zoomIntensity);
        }
        if (zoomStarted) {
            API::get()->log_info(
                "[CodexDualSenseBridge] mapZoom direction=%s intensity=%.2f write=%s",
                zoomDirection > 0.0f ? "in" : "out",
                zoomIntensity,
                sent ? "ok" : "failed");
        }
    }

    if (focusChanged && absorbedIndex >= 0 &&
        BeginEvent(g_lastMapFocusTick, 48)) {
        // A compact detent only when the cursor actually locks to a new icon.
        PlayMotorPulse(1, 0.115f, 0.030f, true, 0);
        PlayMotorPulse(0, 0.052f, 0.038f, true, 0);
        API::get()->log_info(
            "[CodexDualSenseBridge] mapCursorAbsorbed index=%d",
            absorbedIndex);
    }
}

int PreWorldMapUpdate(
    int,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argv != nullptr && argv[1] != nullptr) {
        g_lastMapVisibleTick.store(GetTickCount64(), std::memory_order_relaxed);
        UpdateMapHaptics(
            reinterpret_cast<API::ManagedObject*>(argv[1]),
            true);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreMirrorMapUpdate(
    int,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argv != nullptr && argv[1] != nullptr) {
        g_lastMapVisibleTick.store(GetTickCount64(), std::memory_order_relaxed);
        UpdateMapHaptics(
            reinterpret_cast<API::ManagedObject*>(argv[1]),
            false);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreMapFreePinUpdated(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    PlayMapMarkerFeedback(true);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreMapFreePinCleared(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    PlayMapMarkerFeedback(false);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreMapTargetPlayed(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 3 && argv != nullptr) {
        const bool placed = reinterpret_cast<uintptr_t>(argv[2]) != 0;
        PlayMapMarkerFeedback(placed);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

const char* PlayerProfileName(PlayerHapticProfile profile) {
    switch (profile) {
    case PlayerHapticProfile::Hit: return "hit";
    case PlayerHapticProfile::Stab: return "stab";
    case PlayerHapticProfile::Collision: return "collision";
    case PlayerHapticProfile::Knockdown: return "knockdown";
    case PlayerHapticProfile::Grab: return "grab";
    case PlayerHapticProfile::Dodge: return "dodge";
    case PlayerHapticProfile::JustDodge: return "just-dodge";
    default: return "unknown";
    }
}

bool IsPlayerDamageAction(const std::string& typeName) {
    if (typeName.find("app.Player") != 0) return false;
    if (typeName.find("DamageAction.") != std::string::npos) return true;
    return typeName.find("app.PlayerCommonAction.c") == 0 &&
        typeName.find("Damage") != std::string::npos;
}

bool IsPlayerGrabDamageAction(const std::string& typeName) {
    return IsPlayerDamageAction(typeName) &&
        (typeName.find("GrabDamage") != std::string::npos ||
         typeName.find("GrappleDamage") != std::string::npos ||
         typeName.find("RestrictDamage") != std::string::npos ||
         typeName.find("Capture") != std::string::npos);
}

bool IsPlayerDodgeAction(const std::string& typeName) {
    if (typeName.find("app.Player") != 0 ||
        typeName.find("Action.c") == std::string::npos) {
        return false;
    }
    const bool dodge =
        typeName.find("cDodge") != std::string::npos ||
        typeName.find("cJustDodge") != std::string::npos;
    const bool attackFollowUp =
        typeName.find("DodgeAttack") != std::string::npos ||
        typeName.find("AttackAfterJustDodge") != std::string::npos ||
        typeName.find("AirAttackAfterJustDodge") != std::string::npos;
    const bool runTransition =
        typeName.find("DodgeToRun") != std::string::npos;
    return dodge && !attackFollowUp && !runTransition;
}

PlayerHapticProfile ClassifyPlayerDamage(
    const std::string& typeName,
    float& intensity) {
    if (IsPlayerGrabDamageAction(typeName)) {
        intensity = 0.98f;
        return PlayerHapticProfile::Grab;
    }
    if (typeName.find("StabDamage") != std::string::npos) {
        intensity = 0.90f;
        return PlayerHapticProfile::Stab;
    }
    if (typeName.find("DashAtkDamage") != std::string::npos ||
        typeName.find("WindPressureDamage") != std::string::npos ||
        typeName.find("GuardBreak") != std::string::npos ||
        typeName.find("RikidoBreak") != std::string::npos ||
        typeName.find("KneelingDamage") != std::string::npos) {
        intensity = 0.92f;
        return PlayerHapticProfile::Collision;
    }
    if (typeName.find("SmashDamage") != std::string::npos ||
        typeName.find("SlamDamage") != std::string::npos ||
        typeName.find("LaunchDamage") != std::string::npos ||
        typeName.find("LandDamage") != std::string::npos ||
        typeName.find("FallDie") != std::string::npos ||
        typeName.find("FallReturn") != std::string::npos ||
        typeName.find(".cDown") != std::string::npos ||
        typeName.find(".cDie") != std::string::npos) {
        intensity = typeName.find("Small") != std::string::npos
            ? 0.76f
            : 0.98f;
        return PlayerHapticProfile::Knockdown;
    }

    intensity = 0.70f;
    if (typeName.find("SmallDamage") != std::string::npos ||
        typeName.find("LowerDamage") != std::string::npos) {
        intensity = 0.58f;
    } else if (typeName.find("MediumDamage") != std::string::npos) {
        intensity = 0.76f;
    } else if (typeName.find("LargeDamage") != std::string::npos) {
        intensity = 0.96f;
    }
    return PlayerHapticProfile::Hit;
}

void PlayPlayerReaction(
    PlayerHapticProfile profile,
    float intensity,
    const std::string& actionType) {
    const size_t profileIndex = static_cast<size_t>(profile);
    if (profileIndex >= g_lastPlayerReactionTick.size()) return;
    constexpr std::array<ULONGLONG, 8> MinimumIntervals{
        0, 75, 115, 120, 120, 420, 210, 180};
    if (!BeginEvent(
            g_lastPlayerReactionTick[profileIndex],
            MinimumIntervals[profileIndex])) {
        return;
    }

    bool sent = false;
    {
        std::lock_guard lock{g_mutex};
        sent = SendPlayerReactionLocked(
            profile,
            std::clamp(intensity, 0.0f, 1.0f));
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] playerReaction=%s intensity=%.2f action=%s write=%s",
        PlayerProfileName(profile),
        intensity,
        actionType.c_str(),
        sent ? "ok" : "failed");
}

execution_haptics::MotionSignature ReadExecutionMotionSignature(
    API::ManagedObject* action) {
    execution_haptics::MotionSignature signature{};
    if (action == nullptr) return signature;
    auto** motionField = action->get_field<API::ManagedObject*>("_MotionID");
    if (motionField == nullptr || *motionField == nullptr) return signature;
    auto* motion = *motionField;
    if (const auto* value = motion->get_field<uint64_t>("<GroupID>k__BackingField"))
        signature.groupId = *value;
    if (const auto* value = motion->get_field<uint32_t>("<SetID>k__BackingField"))
        signature.setId = *value;
    if (const auto* value = motion->get_field<int32_t>("<LayerType>k__BackingField"))
        signature.layer = static_cast<uint8_t>(*value);
    if (const auto* value = motion->get_field<int32_t>("<LR>k__BackingField"))
        signature.leftRight = static_cast<uint8_t>(*value);
    if (const auto* value = motion->get_field<int32_t>("<ConnectType>k__BackingField"))
        signature.connectType = static_cast<uint8_t>(*value);
    return signature;
}

void TryPlayExecutionHaptic(
    API::ManagedObject* action,
    const std::string& typeName,
    const char* hookPath) {
    const auto executionKind = execution_haptics::Classify(typeName);
    if (executionKind == execution_haptics::Kind::None ||
        !BeginEvent(g_lastExecutionHapticTick, 700)) {
        return;
    }
    const auto signature = ReadExecutionMotionSignature(action);
    const uint8_t motionVariant = execution_haptics::MotionVariant(signature);
    const auto profile = static_cast<CombatHapticProfile>(
        execution_haptics::ProfileValue(executionKind));
    PlayCombatHapticProfile(profile, motionVariant);
    API::get()->log_info(
        "[CodexDualSenseBridge] executionHaptic kind=%u profile=%u variant=%u group=%llu set=%u layer=%u lr=%u connect=%u path=%s action=%s",
        static_cast<unsigned>(executionKind),
        static_cast<unsigned>(profile),
        static_cast<unsigned>(motionVariant),
        static_cast<unsigned long long>(signature.groupId),
        static_cast<unsigned>(signature.setId),
        static_cast<unsigned>(signature.layer),
        static_cast<unsigned>(signature.leftRight),
        static_cast<unsigned>(signature.connectType),
        hookPath,
        typeName.c_str());
}

int PrePlayerReactionEntered(
    int,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* action = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = action->get_type_definition();
    if (type == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const std::string typeName = type->get_full_name();
    TryPlayExecutionHaptic(action, typeName, "shared-reaction");

    if (IsPlayerDamageAction(typeName)) {
        // Damage-action sentinels do not represent a physical impact.
        if (typeName.find("cDoNothing") == std::string::npos) {
            float intensity = 0.70f;
            const PlayerHapticProfile profile =
                ClassifyPlayerDamage(typeName, intensity);
            PlayPlayerReaction(profile, intensity, typeName);
        }
    } else if (IsPlayerDodgeAction(typeName)) {
        const bool justDodge =
            typeName.find("JustDodge") != std::string::npos;
        PlayPlayerReaction(
            justDodge
                ? PlayerHapticProfile::JustDodge
                : PlayerHapticProfile::Dodge,
            justDodge ? 0.90f : 0.60f,
            typeName);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PrePlayerGrabSustainUpdated(
    int,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* action = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = action->get_type_definition();
    if (type == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const std::string typeName = type->get_full_name();
    if (IsPlayerGrabDamageAction(typeName)) {
        // A long grab/restriction can remain in detailUpdate for seconds.
        // PlayPlayerReaction applies a per-profile interval so this becomes
        // a readable sequence of tightening pulses instead of frame spam.
        PlayPlayerReaction(PlayerHapticProfile::Grab, 0.90f, typeName);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

const char* ControllerSpeakerCueName(ControllerSpeakerCue cue) {
    switch (cue) {
    case ControllerSpeakerCue::SoulStart: return "soul-start";
    case ControllerSpeakerCue::SoulCollect: return "soul-collect";
    case ControllerSpeakerCue::SoulStop: return "soul-stop";
    case ControllerSpeakerCue::CombatClash: return "combat-clash";
    case ControllerSpeakerCue::CombatGuard: return "combat-guard";
    case ControllerSpeakerCue::CombatDeflect: return "combat-deflect";
    case ControllerSpeakerCue::CombatExecution: return "combat-execution";
    case ControllerSpeakerCue::CombatClashHit: return "combat-clash-hit";
    case ControllerSpeakerCue::CombatClashSuccess: return "combat-clash-success";
    case ControllerSpeakerCue::CombatGuardDirectional:
        return "combat-guard-directional";
    case ControllerSpeakerCue::CombatIssen: return "combat-issen";
    case ControllerSpeakerCue::CombatParryBlade: return "combat-parry-blade";
    case ControllerSpeakerCue::CombatParryProjectile:
        return "combat-parry-projectile";
    default: return "none";
    }
}

ControllerSpeakerCue ClassifyCombatSpeakerAction(
    const std::string& typeName) {
    const bool player = typeName.find("app.Player") == 0;
    const bool enemy = typeName.find("app.Enemy") == 0 ||
        typeName.find("app.Em") == 0;
    if (!player && !enemy) return ControllerSpeakerCue::None;

    const bool issen = typeName.find("Issen") != std::string::npos;
    if (player && execution_haptics::Classify(typeName) !=
            execution_haptics::Kind::None) {
        return ControllerSpeakerCue::CombatExecution;
    }
    if (enemy && issen &&
        (typeName.find("Die") != std::string::npos ||
         typeName.find("Finish") != std::string::npos)) {
        return typeName.find("BreakIssen") != std::string::npos
            ? ControllerSpeakerCue::CombatExecution
            : ControllerSpeakerCue::CombatIssen;
    }
    if (player &&
        (typeName.ends_with(".cCounterIssen") ||
         typeName.ends_with(".cCounterIssenRelease") ||
         typeName.ends_with(".cCounterIssenMultipleAttack") ||
         typeName.find("ChainIssenFinishAttack") != std::string::npos)) {
        return ControllerSpeakerCue::CombatIssen;
    }

    const bool excludedParryState =
        typeName.find("PreParry") != std::string::npos ||
        typeName.find("ParryAttack") != std::string::npos ||
        typeName.find("AttackAfterParry") != std::string::npos ||
        typeName.find("AttackFinishAfterParry") != std::string::npos;
    const bool successfulParry =
        typeName.find("Parry") != std::string::npos &&
        !excludedParryState;
    const bool successfulJustGuard =
        typeName.find("JustGuard") != std::string::npos &&
        typeName.find("PreJustGuard") == std::string::npos &&
        typeName.find("WaitJustGuard") == std::string::npos &&
        typeName.find("JustGuardAttack") == std::string::npos &&
        typeName.find("JustGuardAdjust") == std::string::npos;
    if (player && successfulParry) {
        // These names were previously guessed as Repel/arrow Repel. Runtime
        // PARRY evidence proved they also belong to Deflect; defer their sound
        // to native setGrapple/setShellParry instead of guessing from the name.
        if (combat_defense_feedback::RequiresNativeParrySound(typeName))
            return ControllerSpeakerCue::None;
        return ControllerSpeakerCue::CombatDeflect;
    }
    if (player && successfulJustGuard) {
        return ControllerSpeakerCue::CombatDeflect;
    }

    if (player &&
        typeName.find("DirectionalGuardHit") != std::string::npos &&
        typeName.find("GuardBreak") == std::string::npos) {
        // A guard hit plus an input chord is not a confirmed successful Repel.
        // tryJustGuard's verified success path handles the actual melee cue.
        return ControllerSpeakerCue::CombatGuardDirectional;
    }
    if (player &&
        (typeName.find("DamageGuard") != std::string::npos ||
         typeName.find("LadderGuardHit") != std::string::npos) &&
        typeName.find("GuardBreak") == std::string::npos) {
        return ControllerSpeakerCue::CombatGuard;
    }

    if (player &&
        (typeName.find("DeadHeat") != std::string::npos ||
         typeName.find("BladeLock") != std::string::npos ||
         typeName.find("AttackRepelled") != std::string::npos)) {
        if (typeName.find("Start") != std::string::npos) {
            return ControllerSpeakerCue::CombatClash;
        }
        // DeadHeat's first and second successful inputs are intermediate
        // blade contacts.  The game fires the same DeadHeat-hit event for
        // both; only the sequence-ending action owns the success accent.
        if (typeName.find("SuccessFirst") != std::string::npos ||
            typeName.find("SuccessSecond") != std::string::npos) {
            return ControllerSpeakerCue::CombatClashHit;
        }
        if (typeName.find("Success") != std::string::npos ||
            typeName.find("Win") != std::string::npos ||
            typeName.find("Lose") != std::string::npos ||
            typeName.find("StopGrapple") != std::string::npos) {
            return ControllerSpeakerCue::CombatClashSuccess;
        }
        return ControllerSpeakerCue::CombatClashHit;
    }
    return ControllerSpeakerCue::None;
}

bool IsCombatSpeakerHookOwner(const std::string& typeName) {
    if (typeName.find("app.Player") != 0 &&
        typeName.find("app.Enemy") != 0 &&
        typeName.find("app.Em") != 0) {
        return false;
    }
    constexpr const char* Tokens[] = {
        "DeadHeat", "BladeLock", "AttackRepelled", "JustGuard", "Parry",
        "DamageGuard", "DirectionalGuardHit", "DirectionalGuardInfluence",
        "LadderGuard", "Issen",
        "FatalBlow"
    };
    for (const char* token : Tokens) {
        if (typeName.find(token) != std::string::npos) return true;
    }
    return false;
}

void EmitCombatActionFeedback(
    ControllerSpeakerCue cue,
    const std::string& typeName,
    combat_defense_feedback::DeflectSpeakerVariant variant =
        combat_defense_feedback::DeflectSpeakerVariant::Short) {
    const size_t cueIndex = static_cast<size_t>(cue);
    if (cue == ControllerSpeakerCue::None ||
        cueIndex >= g_lastCombatSpeakerTick.size()) {
        // Keep this diagnostic limited to low-frequency action transitions.
        // It exposes any still-unmapped Issen state without bringing back the
        // high-frequency sound-engine hooks removed in 2.1.19.
        if (typeName.find("Issen") != std::string::npos ||
            typeName.find("FatalBlow") != std::string::npos) {
            API::get()->log_info(
                "[CodexDualSenseBridge] combatSpeakerCandidate action=%s",
                typeName.c_str());
        }
        return;
    }

    // Shared base and concrete action entry points can both run for one state
    // transition. Per-cue intervals remove those duplicates while preserving
    // the separate impacts in a multi-step blade lock.
    constexpr std::array<ULONGLONG, 14> MinimumIntervals{
        0, 0, 0, 0, 115, 90, 135, 1100, 90, 115, 90, 420, 135, 135};
    if (cue == ControllerSpeakerCue::CombatDeflect) {
        if (g_gameplayHapticsPaused.load(std::memory_order_acquire)) return;
        bool firstImpact = false;
        {
            std::lock_guard lock{g_deflectEpisodeMutex};
            firstImpact = g_deflectEpisodeGate.Observe(GetTickCount64());
        }
        if (!firstImpact) {
            // The native cinematic flag can become available just after the
            // first sound callback. Extend the SAME contact recording without
            // restarting it, layering another copy, or firing another haptic.
            if (variant == combat_defense_feedback::DeflectSpeakerVariant::Slow) {
                std::lock_guard lock{g_mutex};
                SendControllerSpeakerCueLocked(cue, variant, true);
            }
            const uint32_t repeats = g_deflectRepeatedCallbackCount.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (repeats <= 4 || repeats % 64 == 0) {
                API::get()->log_info(
                    "[CodexDualSenseBridge] deflectRepeatedCallback suppressed=%u",
                    repeats);
            }
            return;
        }
    } else if (!BeginEvent(
            g_lastCombatSpeakerTick[cueIndex],
            MinimumIntervals[cueIndex])) {
        return;
    }

    // Correlate the game's own Wwise trigger resolution with this confirmed
    // combat action.  The sound hooks below use this timestamp/cue pair to
    // report only events created immediately after the action, instead of
    // flooding the log with unrelated battle audio.
    g_lastCombatSpeakerCue.store(
        static_cast<uint32_t>(cue),
        std::memory_order_relaxed);
    g_lastCombatSpeakerActionTick.store(
        GetTickCount64(),
        std::memory_order_release);

    PlayCombatActionHaptic(cue);
    // Repel speaker audio is paired atomically with its confirmed haptic
    // profile by the helper. Sending a second pipe message here could arrive
    // after a pause/scene reset and silently lose the sound while the haptic
    // still played. Other combat cues retain the established message path.
    if (cue != ControllerSpeakerCue::CombatParryBlade &&
        cue != ControllerSpeakerCue::CombatParryProjectile) {
        PlayControllerSpeakerCue(cue, variant);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] combatSpeaker=%s action=%s",
        ControllerSpeakerCueName(cue),
        typeName.c_str());
}

int PreCombatSpeakerEntered(
    int,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* action = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = action->get_type_definition();
    if (type == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const std::string typeName = type->get_full_name();
    // Concrete FatalBlow overrides (horizontal/diagonal/arm/down/const) do not
    // pass through the shared player-reaction entry used by Single. Dispatch
    // from the combat-action hook as well; the shared gate prevents a duplicate
    // if a runtime build invokes both paths for the same transition.
    TryPlayExecutionHaptic(action, typeName, "combat-action");
    // Keep the existing shell-skill activation texture, but do not attach a
    // Deflect sound to activation before a real projectile result arrives.
    if (typeName.starts_with("app.Player") && typeName.ends_with(".cParryShell") &&
        BeginEvent(g_lastParryActivationTick, 135)) {
        PlayCombatHapticProfile(CombatHapticProfile::ParryActivation);
    }
    EmitCombatActionFeedback(ClassifyCombatSpeakerAction(typeName), typeName);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreNativeRepelSound(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    RepelSoundCallState state{};
    state.argumentCount = argc;
    if (argc >= 7 && argv != nullptr && argv[1] != nullptr) {
        state.behavior = reinterpret_cast<API::ManagedObject*>(argv[1]);
        state.grappleType = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(argv[2]));
        if (g_cinematicParrySchemaReady)
            state.partner = reinterpret_cast<API::ManagedObject*>(argv[3]);
    }
    g_repelSoundCallStack.push_back(state);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

API::Field* ResolveNativeParryField(
    const char* ownerName, const char* fieldName, const char* expectedType) {
    auto* owner = API::get()->tdb()->find_type(ownerName);
    auto* field = owner != nullptr ? owner->find_field(fieldName) : nullptr;
    auto* type = field != nullptr ? field->get_type() : nullptr;
    return field != nullptr && !field->is_static() && type != nullptr &&
        type->get_full_name() == expectedType ? field : nullptr;
}

API::ManagedObject* ReadNativeParryReference(
    API::ManagedObject* owner, API::Field* field, const char* expectedType) {
    if (owner == nullptr || field == nullptr) return nullptr;
    auto** value = static_cast<API::ManagedObject**>(field->get_data_raw(owner, false));
    auto* object = value != nullptr ? *value : nullptr;
    auto* type = object != nullptr ? object->get_type_definition() : nullptr;
    if (type == nullptr || (type->get_full_name() != expectedType &&
        !type->is_derived_from(expectedType))) return nullptr;
    return object;
}

bool IsNativeCinematicParry(API::ManagedObject* partner, uint32_t& category) {
    category = InvalidVibrationId;
    if (partner == nullptr || !g_cinematicParrySchemaReady) return false;
    auto* type = partner->get_type_definition();
    if (type == nullptr || (type->get_full_name() != "app.EnemyCharacter" &&
        !type->is_derived_from("app.EnemyCharacter"))) return false;
    // Reproduce the enemy's native isGrappleParryLarge semantics using exposed
    // fields on the partner passed to setGrapple: pending action first, then
    // current action; category 2 selects EnemyAction in CharacterArgumentData.
    auto* action = ReadNativeParryReference(
        partner, g_parryRequestActionField, "app.cCharacterActionInfo");
    if (action == nullptr) action = ReadNativeParryReference(
        partner, g_parryCurrentActionField, "app.cCharacterActionInfo");
    if (action == nullptr) return false;
    auto* actionCategory = static_cast<uint32_t*>(g_parryCategoryField->get_data_raw(action, false));
    if (actionCategory == nullptr) return false;
    category = *actionCategory;
    if (category != 2) return false;
    auto* argument = ReadNativeParryReference(
        partner, g_parryArgumentField, "app.cCharacterArgumentData");
    auto* enemyArgument = ReadNativeParryReference(
        argument, g_parryEnemyActionField, "app.cCharacterArgumentData.cEnemyActionArgument");
    if (enemyArgument == nullptr) return false;
    auto* large = static_cast<bool*>(g_parryLargeField->get_data_raw(enemyArgument, false));
    return combat_defense_feedback::IsCinematicParryArgument(
        true, category, large != nullptr && *large);
}

void PostNativeRepelSound(
    void**,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (g_repelSoundCallStack.empty()) return;
    const RepelSoundCallState state = g_repelSoundCallStack.back();
    g_repelSoundCallStack.pop_back();
    // Report a bounded sample before filtering, so an absent PARRY log is not
    // mistaken for an absent callback or a failed action by the player.
    static std::atomic<uint32_t> sampledCalls{0};
    if (sampledCalls.fetch_add(1, std::memory_order_relaxed) < 12) {
        API::get()->log_info(
            "[CodexDualSenseBridge] nativeGrappleSoundCall argc=%d type=%u behavior=%s",
            state.argumentCount, state.grappleType,
            state.behavior != nullptr ? "present" : "absent");
    }
    if (state.behavior == nullptr ||
        (state.grappleType != combat_defense_feedback::NativeParry &&
         state.grappleType != combat_defense_feedback::NativeBlock)) return;
    auto* characterField = state.behavior->get_field<API::ManagedObject*>("_Character");
    auto* character = characterField != nullptr ? *characterField : nullptr;
    auto* characterType = character != nullptr ? character->get_type_definition() : nullptr;
    const std::string characterName = characterType != nullptr
        ? characterType->get_full_name() : "<null>";
    auto* trigger = state.behavior->get_field<uint32_t>("_ParryTrigger");
    auto* endTrigger = state.behavior->get_field<uint32_t>("_ParryEndTrigger");
    auto* continueTrigger = state.behavior->get_field<uint32_t>("_BlockContinueTrigger");
    auto* shell = state.behavior->get_field<bool>("_ShellParry");
    const bool player = characterName.find("app.Player") == 0;
    API::get()->log_info(
        "[CodexDualSenseBridge] nativeGrappleSoundResolved type=%u character=%s player=%d trigger=%u endTrigger=%u continueTrigger=%u shell=%d",
        state.grappleType, characterName.c_str(), player ? 1 : 0,
        trigger != nullptr ? *trigger : InvalidVibrationId,
        endTrigger != nullptr ? *endTrigger : InvalidVibrationId,
        continueTrigger != nullptr ? *continueTrigger : InvalidVibrationId,
        shell != nullptr && *shell ? 1 : 0);
    const auto kind = combat_defense_feedback::ClassifyNativeGrapple(
        state.grappleType, player,
        g_gameplayHapticsPaused.load(std::memory_order_acquire),
        trigger != nullptr ? *trigger : InvalidVibrationId);
    // Every PARRY callback must reach the shared episode gate. Filtering it by
    // a 450-ms emission cooldown hid the intervening observations and replayed
    // the same impact throughout a prolonged slow-motion Deflect.
    if (kind == combat_defense_feedback::Kind::Deflect) {
        uint32_t category = InvalidVibrationId;
        const bool cinematic = IsNativeCinematicParry(state.partner, category);
        auto* partnerType = state.partner != nullptr ? state.partner->get_type_definition() : nullptr;
        static std::atomic<int> lastCinematicState{-1};
        static std::atomic<unsigned> cinematicSamples{0};
        const bool stateChanged = lastCinematicState.exchange(cinematic ? 1 : 0) != (cinematic ? 1 : 0);
        if (cinematicSamples.fetch_add(1, std::memory_order_relaxed) < 8 || stateChanged) {
            API::get()->log_info(
                "[CodexDualSenseBridge] nativeParryCinematic large=%d available=%d trigger=%u partner=%s category=%u",
                cinematic ? 1 : 0, g_cinematicParrySchemaReady ? 1 : 0,
                trigger != nullptr ? *trigger : InvalidVibrationId,
                partnerType != nullptr ? partnerType->get_full_name().c_str() : "<null>", category);
        }
        EmitCombatActionFeedback(ControllerSpeakerCue::CombatDeflect,
            "app.snd_user_data.SoundHitSwitchListBehavior.setGrapple(PARRY)",
            combat_defense_feedback::ClassifyDeflectSpeakerVariant(
                trigger != nullptr ? *trigger : InvalidVibrationId,
                endTrigger != nullptr ? *endTrigger : InvalidVibrationId,
                cinematic));
    }
    // BLOCK already speaks from the verified tryJustGuard(success) path.
}

int PreNativeProjectileRepelSound(
    int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    ProjectileRepelSoundCallState state{};
    if (argc >= 5 && argv != nullptr && argv[1] != nullptr) {
        state.behavior = reinterpret_cast<API::ManagedObject*>(argv[1]);
        state.trigger = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argv[2]));
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG inputTick = g_lastParryInputTick.load(std::memory_order_relaxed);
        state.recentRepelInput = inputTick != 0 && now >= inputTick && now - inputTick <= 900;
    }
    g_projectileRepelSoundCallStack.push_back(state);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostNativeProjectileRepelSound(
    void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (g_projectileRepelSoundCallStack.empty()) return;
    const auto state = g_projectileRepelSoundCallStack.back();
    g_projectileRepelSoundCallStack.pop_back();
    if (state.behavior == nullptr) return;
    auto* characterField = state.behavior->get_field<API::ManagedObject*>("_Character");
    auto* character = characterField != nullptr ? *characterField : nullptr;
    auto* type = character != nullptr ? character->get_type_definition() : nullptr;
    const bool player = type != nullptr && type->get_full_name().find("app.Player") == 0;
    const bool paused = g_gameplayHapticsPaused.load(std::memory_order_acquire);
    const auto kind = combat_defense_feedback::ClassifyNativeShell(
        player, paused, state.trigger, state.recentRepelInput);
    API::get()->log_info(
        "[CodexDualSenseBridge] nativeProjectileRepelSound player=%d trigger=%u recentInput=%d paused=%d",
        player ? 1 : 0, state.trigger, state.recentRepelInput ? 1 : 0, paused ? 1 : 0);
    if (kind != combat_defense_feedback::Kind::ProjectileRepel) return;
    EmitCombatActionFeedback(ControllerSpeakerCue::CombatParryProjectile,
        "app.snd_user_data.SoundHitSwitchListBehavior.setShellParry(L1+Cross)");
    g_lastParryInputTick.store(0, std::memory_order_relaxed);
}

int PreMeleeRepelResult(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    MeleeRepelCallState state{};
    state.callStarted = GetTickCount64();
    if (argc >= 3 && argv != nullptr && argv[1] != nullptr && argv[2] != nullptr) {
        auto* supporter = reinterpret_cast<API::ManagedObject*>(argv[1]);
        auto* type = supporter->get_type_definition();
        state.evidence.playerSupporter = type != nullptr &&
            (type->get_full_name() == "app.cPlayerJustGuardSupporter" ||
             type->is_derived_from("app.cPlayerJustGuardSupporter"));
        if (state.evidence.playerSupporter) {
            auto* info = reinterpret_cast<API::ManagedObject*>(argv[2]);
            auto* attacker = info->get_field<API::ManagedObject*>("AttackCharacter");
            state.evidence.attackCharacterPresent = attacker != nullptr && *attacker != nullptr;
            if (auto* style = info->get_field<int32_t>("JustGuardStyle")) state.style = *style;
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG inputTick = g_lastParryInputTick.load(std::memory_order_relaxed);
            state.evidence.recentRepelInput = inputTick != 0 && now >= inputTick &&
                now - inputTick <= 900;
            state.evidence.gameplayPaused = g_gameplayHapticsPaused.load(std::memory_order_acquire);
            // Nested concrete/base implementations belong to the same original
            // defensive result. Give that call one identity, not a time cooldown
            // that could discard a genuinely new, rapidly following impact.
            for (auto parent = g_meleeRepelCallStack.rbegin();
                 parent != g_meleeRepelCallStack.rend(); ++parent) {
                if (parent->evidence.playerSupporter) {
                    state.callStarted = parent->callStarted;
                    state.resultId = parent->resultId;
                    break;
                }
            }
            if (state.resultId == 0) {
                state.resultId = g_defenseResultSequence.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            }
        }
    }
    g_meleeRepelCallStack.push_back(state);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostMeleeRepelResult(
    void** retValue,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (g_meleeRepelCallStack.empty()) return;
    MeleeRepelCallState state = g_meleeRepelCallStack.back();
    g_meleeRepelCallStack.pop_back();
    if (!state.evidence.playerSupporter) return;
    // Managed Boolean occupies the low byte; upper return-register bits do not
    // form part of the result. Never change that result or any game argument.
    state.evidence.succeeded = retValue != nullptr &&
        (reinterpret_cast<uintptr_t>(*retValue) & 0xFFu) != 0;
    state.evidence.gameplayPaused = state.evidence.gameplayPaused ||
        g_gameplayHapticsPaused.load(std::memory_order_acquire);
    if (state.evidence.succeeded && state.evidence.attackCharacterPresent &&
        !state.evidence.gameplayPaused) {
        std::lock_guard lock{g_deflectEpisodeMutex};
        g_deflectEpisodeGate.ConfirmSuccess(
            GetTickCount64(), state.callStarted, state.resultId);
    }
    if (state.evidence.recentRepelInput || state.evidence.succeeded) {
        API::get()->log_info(
            "[CodexDualSenseBridge] meleeRepelResult success=%d attacker=%d recentInput=%d style=%d paused=%d",
            state.evidence.succeeded ? 1 : 0,
            state.evidence.attackCharacterPresent ? 1 : 0,
            state.evidence.recentRepelInput ? 1 : 0, state.style,
            state.evidence.gameplayPaused ? 1 : 0);
    }
    if (!melee_repel_detection::ShouldEmit(state.evidence)) return;
    EmitCombatActionFeedback(
        ControllerSpeakerCue::CombatParryBlade,
        "app.cCharacterJustGuardSupporter.tryJustGuard(success,L1+Cross)");
    g_lastParryInputTick.store(0, std::memory_order_relaxed);
}

void RecordRecentSoundTrigger(
    uint32_t triggerId,
    uint32_t eventId,
    const char* source) {
    if ((triggerId == 0 || triggerId == 0xFFFFFFFFu) &&
        (eventId == 0 || eventId == 0xFFFFFFFFu)) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard lock{g_recentSoundTriggerMutex};
        g_recentSoundTriggers[g_recentSoundTriggerWriteIndex] = {
            now,
            triggerId,
            eventId,
            source};
        g_recentSoundTriggerWriteIndex =
            (g_recentSoundTriggerWriteIndex + 1) % g_recentSoundTriggers.size();
    }

    const ULONGLONG actionTick =
        g_lastCombatSpeakerActionTick.load(std::memory_order_acquire);
    if (actionTick == 0 || now < actionTick || now - actionTick > 350) return;
    const auto cue = static_cast<ControllerSpeakerCue>(
        g_lastCombatSpeakerCue.load(std::memory_order_relaxed));
    API::get()->log_info(
        "[CodexDualSenseBridge] combatSoundAfter cue=%s trigger=%u event=%u age=%llu source=%s",
        ControllerSpeakerCueName(cue),
        triggerId,
        eventId,
        static_cast<unsigned long long>(now - actionTick),
        source != nullptr ? source : "unknown");
}

int PreCombatSoundTriggerInfo(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr || argv[2] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* info = reinterpret_cast<API::ManagedObject*>(argv[2]);
    const auto* triggerField = info->get_field<uint32_t>("_TriggerId");
    const auto* eventField = info->get_field<uint32_t>("_EventId");
    if (triggerField == nullptr || eventField == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    RecordRecentSoundTrigger(
        *triggerField,
        *eventField,
        "SoundTriggerInfo");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreCombatSoundTriggerId(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 3 && argv != nullptr) {
        RecordRecentSoundTrigger(
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argv[2])),
            0,
            "UInt32");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostMenuDecision(
    void** retValue,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    const ULONGLONG markerTick =
        g_lastMapMarkerTick.load(std::memory_order_relaxed);
    const ULONGLONG longPressCompleteTick =
        g_lastLongPressCompleteTick.load(std::memory_order_relaxed);
    if (retValue == nullptr || reinterpret_cast<uintptr_t>(*retValue) == 0 ||
        (markerTick != 0 && GetTickCount64() - markerTick < 85) ||
        (longPressCompleteTick != 0 &&
            GetTickCount64() - longPressCompleteTick < 140) ||
        !BeginEvent(g_lastMenuDecisionTick, 55)) {
        return;
    }
    if (g_titleActive.load() && GetTickCount64() - g_lastTitleVisible.load() < 250) {
        if (GetTickCount64() - g_lastTitleAccent.load() > 300 && BeginEvent(g_lastTitleDecision, 150)) PlayTitleHaptic(1);
        return;
    }
    PlayMotorPulse(1, 0.165f, 0.040f, true, 0);
    PlayMotorPulse(0, 0.080f, 0.052f, true, 0);
}

void PostMenuCancel(
    void** retValue,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (retValue == nullptr || reinterpret_cast<uintptr_t>(*retValue) == 0 ||
        !BeginEvent(g_lastMenuCancelTick, 55)) {
        return;
    }
    if (g_titleActive.load() && GetTickCount64() - g_lastTitleVisible.load() < 250) {
        PlayTitleHaptic(2);
        return;
    }
    PlayMotorPulse(0, 0.125f, 0.044f, true, 0);
    PlayMotorPulse(1, 0.062f, 0.030f, true, 0);
}

int PreItemObtained(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 4 || argv == nullptr ||
        reinterpret_cast<uintptr_t>(argv[3]) == 0 ||
        !BeginEvent(g_lastItemTick, 80)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    PlayCapturedPreset(captured_haptic_presets::Preset::GetItem);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreItemUsed(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    // Entering the concrete use-item action happens only after the game has
    // accepted the request.  This avoids feedback for an empty slot or for an
    // item that cannot be used in the current state.  Derived action methods
    // can call their base implementation, so coalesce the resulting hooks.
    if (!BeginEvent(g_lastUseItemTick, 250)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    PlayCapturedPreset(captured_haptic_presets::Preset::UseItem);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int32_t ReadVoiceSpeakerId(void* value) {
    if (value == nullptr) return -1;
    int32_t speakerId = -1;
    __try {
        std::memcpy(
            &speakerId,
            static_cast<const uint8_t*>(value) + g_voiceSpeakerOffset,
            sizeof(speakerId));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return speakerId;
}

bool ReadVoiceMessageGuid(void* value, GUID& messageGuid) {
    if (value == nullptr) return false;
    GUID copy{};
    __try {
        std::memcpy(
            &copy,
            static_cast<const uint8_t*>(value),
            sizeof(copy));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    messageGuid = copy;
    return true;
}

uint32_t ResolveGauntletVoiceTrigger(const GUID& messageGuid) {
    for (const auto& mapping : GauntletVoiceMappings) {
        if (IsEqualGUID(messageGuid, mapping.messageGuid)) {
            return mapping.triggerId;
        }
    }
    return 0;
}

bool IsPackagedGauntletVoiceTrigger(uint32_t triggerId) {
    for (const auto& mapping : GauntletVoiceMappings) {
        if (mapping.triggerId == triggerId) return true;
    }
    return false;
}

int PreCreateVoiceParam(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 5 || argv == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const int32_t actorType = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(argv[3]));
    const int32_t talkerId = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(argv[4]));
    API::get()->log_info(
        "[CodexDualSenseBridge] voiceParam create actorType=%d talkerId=%d",
        actorType,
        talkerId);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreVoiceRequestStart(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    const int32_t speakerId = argc >= 3 && argv != nullptr
        ? ReadVoiceSpeakerId(argv[2])
        : -1;
    const bool gauntlet = speakerId == GauntletSpeakerIndex;
    g_voiceRequestStack.push_back({gauntlet, false});
    GUID messageGuid{};
    const bool hasMessageGuid = argc >= 3 && argv != nullptr &&
        ReadVoiceMessageGuid(argv[2], messageGuid);
    if (hasMessageGuid) {
        API::get()->log_info(
            "[CodexDualSenseBridge] voice request speaker=%d "
            "message=%08lX-%04hX-%04hX-%02hhX%02hhX-"
            "%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
            speakerId,
            messageGuid.Data1,
            messageGuid.Data2,
            messageGuid.Data3,
            messageGuid.Data4[0],
            messageGuid.Data4[1],
            messageGuid.Data4[2],
            messageGuid.Data4[3],
            messageGuid.Data4[4],
            messageGuid.Data4[5],
            messageGuid.Data4[6],
            messageGuid.Data4[7]);
    } else {
        API::get()->log_warn(
            "[CodexDualSenseBridge] voice request speaker=%d message=<unreadable>",
            speakerId);
    }
    if (gauntlet) {
        const uint32_t triggerId = hasMessageGuid
            ? ResolveGauntletVoiceTrigger(messageGuid)
            : 0;
        if (triggerId != 0) {
            g_voiceRequestStack.back().voiceSent = true;
            if (BeginEvent(g_lastGauntletVoiceTick, 500)) {
                API::get()->log_info(
                    "[CodexDualSenseBridge] gauntletVoice resolved trigger=%u",
                    triggerId);
                PlayGauntletVoice(triggerId);
            }
        } else {
            API::get()->log_warn(
                "[CodexDualSenseBridge] gauntletVoice message is not mapped");
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostVoiceRequestStart(
    void**,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (!g_voiceRequestStack.empty()) g_voiceRequestStack.pop_back();
}

int PreVoiceRequestStop(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 3 && argv != nullptr &&
        ReadVoiceSpeakerId(argv[2]) == GauntletSpeakerIndex) {
        StopGauntletVoice();
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreDialogueTriggerFull(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const uint32_t triggerId = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(argv[2]));
    if (g_voiceRequestStack.empty()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    VoiceRequestState& state = g_voiceRequestStack.back();
    if (!state.gauntlet || state.voiceSent) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    if (IsPackagedGauntletVoiceTrigger(triggerId)) {
        state.voiceSent = true;
        if (BeginEvent(g_lastGauntletVoiceTick, 500)) {
            PlayGauntletVoice(triggerId);
        }
    } else if (triggerId != 0) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] gauntletVoice runtime trigger=%u is not mapped",
            triggerId);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostDialogueTriggerFull(
    void**,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    g_resolvingSoundTriggerId = 0;
    g_resolvingSoundTriggerInfoIndex = -1;
}

int PreCombatGetTriggerInfo(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    g_resolvingSoundTriggerInfoIndex =
        argc >= 3 && argv != nullptr
            ? static_cast<int32_t>(reinterpret_cast<intptr_t>(argv[2]))
            : -1;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void PostCombatGetTriggerInfo(
    void** retValue,
    REFrameworkTypeDefinitionHandle,
    unsigned long long) {
    if (retValue == nullptr || *retValue == nullptr) return;
    auto* info = reinterpret_cast<API::ManagedObject*>(*retValue);
    const auto* triggerField = info->get_field<uint32_t>("_TriggerId");
    const auto* eventField = info->get_field<uint32_t>("_EventId");
    if (triggerField == nullptr || eventField == nullptr) return;
    RecordRecentSoundTrigger(
        *triggerField,
        *eventField,
        "ResolvedTriggerInfo");
    if (g_resolvingSoundTriggerId != 0) {
        API::get()->log_info(
            "[CodexDualSenseBridge] resolvedSound requested=%u index=%d trigger=%u event=%u",
            g_resolvingSoundTriggerId,
            g_resolvingSoundTriggerInfoIndex,
            *triggerField,
            *eventField);
    }
}

int PreCombatCreateRequestInfo(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr || argv[1] == nullptr ||
        g_combatGetTriggerInfoMethod == nullptr ||
        g_resolvingSoundTriggerId == 0) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const int32_t infoIndex = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(argv[2]));
    auto* container = reinterpret_cast<API::ManagedObject*>(argv[1]);
    std::vector<void*> args{
        reinterpret_cast<void*>(static_cast<intptr_t>(infoIndex))};
    const auto result = g_combatGetTriggerInfoMethod->invoke(container, args);
    if (result.ptr == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* info = reinterpret_cast<API::ManagedObject*>(result.ptr);
    const auto* triggerField = info->get_field<uint32_t>("_TriggerId");
    const auto* eventField = info->get_field<uint32_t>("_EventId");
    if (triggerField == nullptr || eventField == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    RecordRecentSoundTrigger(
        *triggerField,
        *eventField,
        "CreateRequestInfo");
    const ULONGLONG actionTick =
        g_lastCombatSpeakerActionTick.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    if (actionTick != 0 && now >= actionTick && now - actionTick <= 350) {
        API::get()->log_info(
            "[CodexDualSenseBridge] resolvedSound requested=%u index=%d trigger=%u event=%u",
            g_resolvingSoundTriggerId,
            infoIndex,
            *triggerField,
            *eventField);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

API::Method* FindTypedMethod(
    API::TypeDefinition* type,
    const char* methodName,
    uint32_t parameterCount,
    uint32_t typedParameterIndex,
    const char* parameterTypeName) {
    if (type == nullptr) return nullptr;
    for (auto* method : type->get_methods()) {
        if (method == nullptr || std::strcmp(method->get_name(), methodName) != 0 ||
            method->get_num_params() != parameterCount) {
            continue;
        }
        const auto parameters = method->get_params();
        if (typedParameterIndex >= parameters.size() ||
            parameters[typedParameterIndex].t == nullptr) {
            continue;
        }
        auto* parameterType = reinterpret_cast<API::TypeDefinition*>(
            parameters[typedParameterIndex].t);
        if (parameterType->get_full_name() == parameterTypeName) return method;
    }
    return nullptr;
}

void DumpCombatSoundContainerSchema(API::TypeDefinition* type) {
    if (type == nullptr) return;
    API::get()->log_info(
        "[CodexDualSenseBridge] SoundContainer schema type=%s fields=%u methods=%u",
        type->get_full_name().c_str(),
        type->get_num_fields(),
        type->get_num_methods());
    for (auto* field : type->get_fields()) {
        if (field == nullptr) continue;
        auto* fieldType = field->get_type();
        API::get()->log_info(
            "[CodexDualSenseBridge] SoundContainer field name=%s type=%s offset=0x%X",
            field->get_name(),
            fieldType != nullptr ? fieldType->get_full_name().c_str() : "<null>",
            field->get_offset_from_base());
    }
    for (auto* method : type->get_methods()) {
        if (method == nullptr || method->get_name() == nullptr) continue;
        const std::string name = method->get_name();
        if (name.find("trigger") == std::string::npos &&
            name.find("Trigger") == std::string::npos &&
            name.find("info") == std::string::npos &&
            name.find("Info") == std::string::npos &&
            name.find("cache") == std::string::npos &&
            name.find("Cache") == std::string::npos) {
            continue;
        }
        std::string signature;
        const auto parameters = method->get_params();
        for (size_t i = 0; i < parameters.size(); ++i) {
            auto* parameterType = reinterpret_cast<API::TypeDefinition*>(
                parameters[i].t);
            if (i != 0) signature += ",";
            signature += parameterType != nullptr
                ? parameterType->get_full_name()
                : "<null>";
        }
        auto* returnType = method->get_return_type();
        API::get()->log_info(
            "[CodexDualSenseBridge] SoundContainer method name=%s params=(%s) return=%s",
            name.c_str(),
            signature.c_str(),
            returnType != nullptr ? returnType->get_full_name().c_str() : "<null>");
    }
}

uint32_t ResolvePlayerGroundMaterial(API::ManagedObject* playerEntity) {
    if (playerEntity == nullptr) return 0;
    auto* ownerField = playerEntity->get_field<API::ManagedObject*>("_Owner");
    if (ownerField == nullptr || *ownerField == nullptr) return 0;

    struct GroundMaterialAccess {
        API::TypeDefinition* soundContainerType{};
        API::TypeDefinition* groundInfoType{};
        API::Method* getComponent{};
        API::Method* getExtension{};
        API::Method* getMaterialId{};
    };
    static const GroundMaterialAccess access = [] {
        GroundMaterialAccess value{};
        auto* tdb = API::get()->tdb();
        auto* gameObjectType = tdb->find_type("via.GameObject");
        value.soundContainerType = tdb->find_type("soundlib.SoundContainer");
        value.groundInfoType = tdb->find_type(
            "app.snd_user_data.SoundEffectGroundInfoBehavior");
        value.getComponent = FindTypedMethod(
            gameObjectType, "getComponent", 1, 0, "System.Type");
        value.getExtension = FindTypedMethod(
            value.soundContainerType, "getExtension", 1, 0, "System.Type");
        value.getMaterialId = value.groundInfoType != nullptr
            ? value.groundInfoType->find_method("get_MaterialId")
            : nullptr;
        return value;
    }();
    if (access.soundContainerType == nullptr || access.groundInfoType == nullptr ||
        access.getComponent == nullptr) {
        return 0;
    }

    std::vector<void*> componentArgs{
        access.soundContainerType->get_runtime_type()};
    const auto componentResult = access.getComponent->invoke(
        *ownerField, componentArgs);
    if (componentResult.exception_thrown || componentResult.ptr == nullptr) return 0;
    auto* soundContainer = reinterpret_cast<API::ManagedObject*>(componentResult.ptr);

    if (access.getExtension != nullptr && access.getMaterialId != nullptr) {
        std::vector<void*> extensionArgs{access.groundInfoType->get_runtime_type()};
        const auto extensionResult = access.getExtension->invoke(
            soundContainer, extensionArgs);
        if (!extensionResult.exception_thrown && extensionResult.ptr != nullptr) {
            auto* groundInfo = reinterpret_cast<API::ManagedObject*>(extensionResult.ptr);
            const auto materialResult = access.getMaterialId->invoke(
                groundInfo, std::vector<void*>{});
            if (!materialResult.exception_thrown) return materialResult.dword;
        }
    }

    // The sound container mirrors the same value after its ground extension
    // updates.  Keep it as a fallback for early frames during component setup.
    if (auto* material = soundContainer->get_field<uint32_t>(
            "<LatestMaterialId>k__BackingField")) {
        return *material;
    }
    return 0;
}

int PrePlayerFootLanding(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 4 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    auto* footEffect = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* playerEntity = footEffect->get_field<API::ManagedObject*>(
        "_PlayerEntity");
    if (playerEntity == nullptr || *playerEntity == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    g_playerCharacterEntity.store(
        reinterpret_cast<uintptr_t>(*playerEntity),
        std::memory_order_relaxed);

    const int32_t stepType = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(argv[2]));
    const int32_t footType = static_cast<int32_t>(
        reinterpret_cast<intptr_t>(argv[3]));
    const ULONGLONG wallNow = GetTickCount64();
    if (g_wallRunHapticActive.load(std::memory_order_relaxed) &&
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed) &&
        wall_run_haptics::EvidenceFresh(wallNow,
            g_lastWallRunStateTick.load(std::memory_order_relaxed))) {
        // TypeDB StepType: Contact=0, Lift=1, Step=2, Slide=3. Only actual
        // contacts/steps get a kick; lifting the foot must not vibrate.
        if (g_wallRunAlongWallActive.load(std::memory_order_relaxed) &&
            wall_run_haptics::IsFootContact(stepType, footType)) {
            const uint32_t wallSide = static_cast<uint32_t>(footType) & 1u;
            if (BeginEvent(g_lastWallRunFootTick[wallSide], 110)) {
                const bool sent = SendWallRunHaptic(wall_run_haptics::Event::Step,
                    wallSide == 0 ? wall_run_haptics::Side::Left : wall_run_haptics::Side::Right);
                API::get()->log_info("[CodexDualSenseBridge] wallRun footContact side=%u write=%s",
                    wallSide, sent ? "ok" : "failed");
            }
        }
        return REFRAMEWORK_HOOK_CALL_ORIGINAL; // no duplicate ground footstep
    }
    const ULONGLONG wallLand = g_lastWallRunLandTick.load(std::memory_order_relaxed);
    if (wallLand != 0 && wallNow >= wallLand && wallNow - wallLand < 160)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL; // dedicated landing absorbs this contact
    if (!g_gameplayHapticsPaused.load(std::memory_order_relaxed)) {
        wall_run_haptics::LandingTracker::ContactResult wallContact;
        {
            std::lock_guard lock{g_wallRunLandingMutex};
            wallContact = g_wallRunLandingTracker.OfferContact(wallNow, stepType, footType);
        }
        if (wallContact != wall_run_haptics::LandingTracker::ContactResult::Ignored) {
            if (wallContact == wall_run_haptics::LandingTracker::ContactResult::Queued)
                API::get()->log_info("[CodexDualSenseBridge] wallRun landingContact queued step=%d foot=%d",
                    stepType, footType);
            return REFRAMEWORK_HOOK_CALL_ORIGINAL; // fallback replaces the first ordinary footstep
        }
    }
    if (stepType == 1 || stepType < 0 || stepType > 3) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const uint32_t side = (footType & 1) == 0 ? 0u : 1u;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG previousSide =
        g_lastFootstepSideTick[side].load(std::memory_order_relaxed);
    const ULONGLONG sideDebounce = stepType == 3 ? 160 : 135;
    if (previousSide != 0 && now - previousSide < sideDebounce) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    g_lastFootstepSideTick[side].store(now, std::memory_order_relaxed);

    const ULONGLONG previous =
        g_lastFootstepTick.exchange(now, std::memory_order_relaxed);
    if (previous != 0 && now - previous < 55) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const float intervalMs = previous == 0
        ? 460.0f
        : static_cast<float>(std::min<ULONGLONG>(now - previous, 900));
    const float pace = std::clamp((460.0f - intervalMs) / 250.0f, 0.0f, 1.0f);
    const uint32_t gait = pace >= 0.68f ? 2u : (pace >= 0.25f ? 1u : 0u);
    const uint32_t surface = ResolvePlayerGroundMaterial(*playerEntity);

    bool sent = false;
    {
        std::lock_guard lock{g_mutex};
        sent = SendFootstepLocked(surface, side, gait,
            static_cast<uint32_t>(stepType), pace);
    }
    const uint32_t previousSurface = g_lastLoggedSurface.exchange(
        surface, std::memory_order_relaxed);
    if (surface != previousSurface || previous == 0) {
        API::get()->log_info(
            "[CodexDualSenseBridge] playerFootstep surface=%u gait=%u pace=%.2f step=%d write=%s",
            surface,
            gait,
            pace,
            stepType,
            sent ? "ok" : "failed");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreInteractionEntered(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (!BeginEvent(g_lastInteractionTick, 120)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG doorClassified =
        g_lastDoorClassifiedTick.load(std::memory_order_relaxed);
    // Runtime traces show the door success callback can precede detailEnter by
    // roughly 145 ms. Keep the classification alive long enough to suppress
    // the generic interaction that is scheduled afterwards, rather than only
    // trying to cancel a pending event that does not exist yet.
    if (doorClassified != 0 && now - doorClassified <= 500) {
        g_pendingInteractionFeedback.store(false, std::memory_order_relaxed);
        API::get()->log_info(
            "[CodexDualSenseBridge] genericInteractionSuppressed=recentDoor ageMs=%llu",
            static_cast<unsigned long long>(now - doorClassified));
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const ULONGLONG chestOpened = g_lastTreasureChestOpenTick.load(std::memory_order_relaxed);
    if (chestOpened != 0 && now >= chestOpened && now - chestOpened < 600) {
        g_pendingInteractionFeedback.store(false, std::memory_order_relaxed);
        API::get()->log_info(
            "[CodexDualSenseBridge] genericInteractionSuppressed=recentTreasureChest ageMs=%llu",
            static_cast<unsigned long long>(now - chestOpened));
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    // Other interactions retain the existing GM100 feedback with only
    // 140 ms of latency.
    g_pendingInteractionTick.store(now, std::memory_order_relaxed);
    g_pendingInteractionFeedback.store(true, std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

bool IsTreasureChest(API::TypeDefinition* type) {
    if (type == nullptr) return false;
    const std::string typeName = type->get_full_name();
    // Gm002_010 is the detached-lid chest with release/fall/land states.
    // Gm002_000 is the regular treasure-box family used by the chest that
    // was observed in the latest runtime trace. Both should get the distinct
    // opening cue. Gm002_000 uses the base lifecycle as a timing fallback;
    // its OPENED state has not been proven to match the physical lid contact.
    return typeName == "app.Gm002_010" ||
        type->is_derived_from("app.Gm002_010") ||
        typeName == "app.Gm002_000" ||
        type->is_derived_from("app.Gm002_000");
}

std::string TreasureBoxStateName(uint32_t state) {
    auto* enumType = API::get()->tdb()->find_type(
        "app.GimmickTreasureBox.STATE");
    if (enumType != nullptr) {
        for (auto* field : enumType->get_fields()) {
            if (field == nullptr || !field->is_literal() ||
                field->get_init_data() == nullptr) {
                continue;
            }
            const auto value = *static_cast<const int32_t*>(
                field->get_init_data());
            if (static_cast<uint32_t>(value) == state) {
                return field->get_name();
            }
        }
    }
    return "UNKNOWN";
}

bool IsRecentTreasureChestHaptic(ULONGLONG now) {
    constexpr ULONGLONG PreserveWindowMs = 900;
    const ULONGLONG probeUntil =
        g_treasureChestProbeUntil.load(std::memory_order_relaxed);
    if (probeUntil != 0 && now <= probeUntil) {
        return true;
    }
    const std::array<ULONGLONG, 3> stageTicks{
        g_lastTreasureChestOpenTick.load(std::memory_order_relaxed),
        g_lastTreasureChestPushTick.load(std::memory_order_relaxed),
        g_lastTreasureChestLidLandTick.load(std::memory_order_relaxed),
    };
    for (const ULONGLONG tick : stageTicks) {
        if (tick != 0 && now >= tick && now - tick <= PreserveWindowMs) {
            return true;
        }
    }
    return false;
}

bool IsRecentLargeTreasureChestStage(ULONGLONG now, uintptr_t objectKey) {
    const uintptr_t active =
        g_activeTreasureChest.load(std::memory_order_relaxed);
    const ULONGLONG opened =
        g_lastTreasureChestOpenTick.load(std::memory_order_relaxed);
    if (active == 0 || opened == 0 || now < opened ||
        now - opened > 12000) {
        return false;
    }
    if (active == objectKey) return true;

    // The body and the detached lid can be represented by different managed
    // objects: the open success callback is raised by the Gm002_000 body,
    // while release/float-state callbacks are raised by the Gm002_010 lid.
    // Keep the short-lived chest sequence, but let the physical lid object
    // take ownership of its later stages instead of dropping them on an
    // object-identity mismatch.
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=objectRelink active=%p stageObject=%p ageMs=%llu",
        reinterpret_cast<void*>(active),
        reinterpret_cast<void*>(objectKey),
        static_cast<unsigned long long>(now - opened));
    g_activeTreasureChest.store(objectKey, std::memory_order_relaxed);
    return true;
}

int PreLargeTreasureChestPush(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* chest = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = chest->get_type_definition();
    if (!IsTreasureChest(type)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const ULONGLONG now = GetTickCount64();
    g_treasureChestProbeUntil.store(now + 5000, std::memory_order_relaxed);
    const uintptr_t objectKey = reinterpret_cast<uintptr_t>(chest);
    if (!IsRecentLargeTreasureChestStage(now, objectKey) ||
        !BeginEvent(g_lastTreasureChestPushTick, 180)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::TreasureChestPush,
        1.0f,
        "Gm002_010.release");
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=push type=%s",
        type->get_full_name().c_str());
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreLargeTreasureChestFloatState(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr || argv[1] == nullptr ||
        argv[2] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* chest = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = chest->get_type_definition();
    if (!IsTreasureChest(type)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    g_treasureChestProbeUntil.store(GetTickCount64() + 5000,
        std::memory_order_relaxed);

    // TypeDB enum order: NONE=0, SETUP=1, FLOAT=2, RELEASE=3,
    // FALL=4, LAND=5, END=6. LAND is the first frame at which the detached
    // lid has reached the ground, so it owns the landing impact.
    const uint32_t state = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(argv[2]));
    constexpr uint32_t LargeTreasureChestLandState = 5u;
    if (state != LargeTreasureChestLandState) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const ULONGLONG now = GetTickCount64();
    const uintptr_t objectKey = reinterpret_cast<uintptr_t>(chest);
    if (!IsRecentLargeTreasureChestStage(now, objectKey) ||
        !BeginEvent(
            g_lastTreasureChestLidLandTick,
            treasure_chest_haptics::LidImpactDedupeMs)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    g_pendingTreasureChestPushCueTick.store(0, std::memory_order_relaxed);
    g_pendingTreasureChestLidLandTick.store(0, std::memory_order_relaxed);
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::TreasureChestLidLand,
        1.0f,
        "Gm002_010.changeFloatState(LAND)");
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureChestStage=lidLand state=%u type=%s",
        state,
        type->get_full_name().c_str());
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreTreasureBoxStateChanged(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr || argv[1] == nullptr ||
        argv[2] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    auto* chest = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = chest->get_type_definition();
    if (!IsTreasureChest(type)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    g_treasureChestProbeUntil.store(GetTickCount64() + 5000,
        std::memory_order_relaxed);

    const uint32_t state = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(argv[2]));
    const std::string stateName = TreasureBoxStateName(state);
    const std::string typeName = type->get_full_name();
    API::get()->log_info(
        "[CodexDualSenseBridge] treasureBoxState type=%s state=%u name=%s",
        typeName.c_str(), state, stateName.c_str());

    // Gm002_000 is the body object observed in the live trace. Its detached
    // lid, when present, is not guaranteed to expose the Gm002_010 methods on
    // the same managed object, so use the base treasure-box lifecycle as a
    // fallback for the two later physical beats.
    const bool regularTreasureBox = typeName == "app.Gm002_000" ||
        type->is_derived_from("app.Gm002_000");
    if (!regularTreasureBox || !IsRecentLargeTreasureChestStage(
            GetTickCount64(), reinterpret_cast<uintptr_t>(chest))) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    if (stateName == "OPENING" &&
        BeginEvent(g_lastTreasureChestPushTick, 180)) {
        PublishPendingTreasureChestOpenFeedback(true);
        QueueTreasureChestPushFeedback(GetTickCount64());
        QueueTreasureChestLidLandFeedback(GetTickCount64());
        API::get()->log_info(
            "[CodexDualSenseBridge] treasureChestStage=liftThenPush state=%u name=%s type=%s",
            state, stateName.c_str(), typeName.c_str());
    } else if (stateName == "OPENED") {
        g_pendingTreasureChestPushCueTick.store(0, std::memory_order_relaxed);
        g_pendingTreasureChestLidLandTick.store(0, std::memory_order_relaxed);
        if (!BeginEvent(
                g_lastTreasureChestLidLandTick,
                treasure_chest_haptics::LidImpactDedupeMs)) {
            API::get()->log_info(
                "[CodexDualSenseBridge] treasureChestStage=lidLand OPENED fallback suppressed=scheduled-impact");
            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        }
        PlayEnvironmentInteractionHaptic(
            EnvironmentInteractionProfile::TreasureChestLidLand,
            1.0f,
            "GimmickTreasureBox.changeState(OPENED)");
        API::get()->log_info(
            "[CodexDualSenseBridge] treasureChestStage=lidLand state=%u name=%s type=%s",
            state, stateName.c_str(), typeName.c_str());
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreGimmickInteractionSucceeded(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    auto* object = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = object->get_type_definition();
    if (type == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const std::string typeName = type->get_full_name();
    if (typeName != "app.AppGimmickBase" &&
        !type->is_derived_from("app.AppGimmickBase")) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    if (IsTreasureChest(type)) {
        g_pendingTreasureChestOpenCueTick.store(0, std::memory_order_relaxed);
        g_pendingTreasureChestPushCueTick.store(0, std::memory_order_relaxed);
        g_pendingTreasureChestLidLandTick.store(0, std::memory_order_relaxed);
        g_treasureChestProbeUntil.store(GetTickCount64() + 5000,
            std::memory_order_relaxed);
        g_activeTreasureChest.store(
            reinterpret_cast<uintptr_t>(object), std::memory_order_relaxed);
        const bool cancelled = g_pendingInteractionFeedback.exchange(
            false, std::memory_order_relaxed);
        if (BeginEvent(g_lastTreasureChestOpenTick, 500)) {
            QueueTreasureChestOpenFeedback(GetTickCount64());
            API::get()->log_info(
                "[CodexDualSenseBridge] treasureChestStage=lift pending type=%s pendingGenericCancelled=%s",
                typeName.c_str(),
                cancelled ? "yes" : "already-cleared");
        }
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    // Gm025 already has its own high-energy Oni-strength barrier profile.
    // Suppress the shared success callback so that rupture is not doubled.
    if (typeName == "app.Gm025") return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const bool door = typeName == "app.GimmickDoor" ||
        type->is_derived_from("app.GimmickDoor");
    // Door success is raised before the character's hand animation reaches the
    // door. Remember the classification because it may precede detailEnter,
    // then wait for the first physical opening update to emit the only cue.
    if (door) {
        const ULONGLONG now = GetTickCount64();
        g_lastDoorClassifiedTick.store(now, std::memory_order_relaxed);
        QueueDoorOpeningFeedback(now, "success");
        const bool cancelled = g_pendingInteractionFeedback.exchange(
            false, std::memory_order_relaxed);
        API::get()->log_info(
            "[CodexDualSenseBridge] doorClassifier=success pendingGenericCancelled=%s",
            cancelled ? "yes" : "already-cleared");
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    if (!BeginEvent(g_lastGimmickSuccessTick, 125)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::GenericSuccess,
        0.82f,
        typeName.c_str());
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

bool WasRecentlyInteracting(ULONGLONG now) {
    const ULONGLONG interaction =
        g_lastInteractionTick.load(std::memory_order_relaxed);
    const ULONGLONG longPress =
        g_lastLongPressCompleteTick.load(std::memory_order_relaxed);
    return (interaction != 0 && now - interaction <= 8000) ||
        (longPress != 0 && now - longPress <= 2500);
}

int PreDoorOpeningUpdated(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* object = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = object->get_type_definition();
    if (type == nullptr ||
        (type->get_full_name() != "app.GimmickDoor" &&
         !type->is_derived_from("app.GimmickDoor"))) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    // updateOpening is called on each frame in which the concrete door is
    // physically advancing. A long debounce makes its first frame the sole
    // haptic trigger and prevents the old end-aligned second pulse.
    if (!WasRecentlyInteracting(GetTickCount64()) ||
        !BeginEvent(g_lastDoorOpeningTick, 2500)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::Door,
        1.0f,
        type->get_full_name().c_str());
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreDoorOpenRequested(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle* type,
    unsigned long long threadContext) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    // requestOpen may occur either before or after detailEnter. Persist the
    // classification for the former ordering and cancel a pending generic cue
    // for the latter. The first updateOpening frame owns the only door cue.
    const ULONGLONG now = GetTickCount64();
    g_lastDoorClassifiedTick.store(now, std::memory_order_relaxed);
    QueueDoorOpeningFeedback(now, "requestOpen");
    const bool cancelled = g_pendingInteractionFeedback.exchange(
        false, std::memory_order_relaxed);
    API::get()->log_info(
        "[CodexDualSenseBridge] doorClassifier=requestOpen pendingGenericCancelled=%s",
        cancelled ? "yes" : "already-cleared");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreLeverChanged(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const ULONGLONG now = GetTickCount64();
    // Some lever state machines are also used by scripted remote mechanisms.
    // Require a recent player interaction before publishing the mechanical snap.
    if (!WasRecentlyInteracting(now) || !BeginEvent(g_lastLeverTick, 180)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    auto* object = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* type = object->get_type_definition();
    const std::string typeName =
        type != nullptr ? type->get_full_name() : "unknown-lever";
    PlayEnvironmentInteractionHaptic(
        EnvironmentInteractionProfile::Lever,
        1.0f,
        typeName.c_str());
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

bool PlayerIsOnElevator(API::ManagedObject* elevator) {
    if (elevator == nullptr) return false;
    auto* inArea = elevator->get_field<bool>("_IsPlInArea");
    return inArea != nullptr && *inArea;
}

int PreElevatorMoveStarted(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* elevator = reinterpret_cast<API::ManagedObject*>(argv[1]);
    if (!PlayerIsOnElevator(elevator)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;

    const uintptr_t key = reinterpret_cast<uintptr_t>(elevator);
    const uintptr_t previous = g_activeElevator.exchange(
        key, std::memory_order_relaxed);
    if ((previous != key || BeginEvent(g_lastElevatorStartTick, 400))) {
        g_lastElevatorStartTick.store(GetTickCount64(), std::memory_order_relaxed);
        PlayEnvironmentInteractionHaptic(
            EnvironmentInteractionProfile::ElevatorStart,
            1.0f,
            "GimmickElevator.changeStateEvent_Move");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreElevatorMoveUpdated(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* elevator = reinterpret_cast<API::ManagedObject*>(argv[1]);
    const uintptr_t key = reinterpret_cast<uintptr_t>(elevator);
    if (!PlayerIsOnElevator(elevator)) {
        uintptr_t expected = key;
        if (g_activeElevator.compare_exchange_strong(
                expected, 0, std::memory_order_relaxed) &&
            BeginEvent(g_lastElevatorStopTick, 250)) {
            PlayEnvironmentInteractionHaptic(
                EnvironmentInteractionProfile::ElevatorStop,
                0.88f,
                "GimmickElevator.playerLeftArea");
        }
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const uintptr_t previous = g_activeElevator.exchange(
        key, std::memory_order_relaxed);
    if (previous != key && BeginEvent(g_lastElevatorStartTick, 250)) {
        PlayEnvironmentInteractionHaptic(
            EnvironmentInteractionProfile::ElevatorStart,
            1.0f,
            "GimmickElevator.updateMoveState");
    }
    if (BeginEvent(g_lastElevatorMoveTick, 72)) {
        PlayEnvironmentInteractionHaptic(
            EnvironmentInteractionProfile::ElevatorMove,
            0.88f,
            "GimmickElevator.updateMoveState",
            false);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreElevatorMoveStopped(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* elevator = reinterpret_cast<API::ManagedObject*>(argv[1]);
    const uintptr_t key = reinterpret_cast<uintptr_t>(elevator);
    uintptr_t expected = key;
    const bool wasActive = g_activeElevator.compare_exchange_strong(
        expected, 0, std::memory_order_relaxed);
    if ((wasActive || PlayerIsOnElevator(elevator)) &&
        BeginEvent(g_lastElevatorStopTick, 250)) {
        PlayEnvironmentInteractionHaptic(
            EnvironmentInteractionProfile::ElevatorStop,
            1.0f,
            "GimmickElevator.changeStateEvent_Stop");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreCutscenePlayingFrame(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    const ULONGLONG now = GetTickCount64();
    g_cutscenePlayHeartbeatTick.store(now, std::memory_order_release);
    if (argc < 2 || argv == nullptr || argv[1] == nullptr ||
        g_cutsceneResourceGetter == nullptr ||
        g_cutsceneFlowFrameGetter == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* player = reinterpret_cast<API::ManagedObject*>(argv[1]);
    const std::vector<void*> noArguments;
    const auto resourceResult = g_cutsceneResourceGetter->invoke(player, noArguments);
    const auto frameResult = g_cutsceneFlowFrameGetter->invoke(player, noArguments);
    if (resourceResult.exception_thrown || resourceResult.ptr == nullptr ||
        frameResult.exception_thrown) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* resource = reinterpret_cast<API::ManagedObject*>(resourceResult.ptr);
    const auto* eventIdField = resource->get_field<int32_t>("_EventID");
    const auto* flowIdField = player->get_field<int32_t>("_CurrentFlow");
    if (eventIdField == nullptr || flowIdField == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    double frame = 0.0;
    std::memcpy(&frame, frameResult.bytes.data(), sizeof(frame));
    if (!std::isfinite(frame) || frame < 0.0) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    float cuePower = 0.0f;
    float cueSpanFrames = 0.0f;
    {
        std::lock_guard stateLock{g_cutsceneCueStateMutex};
        const uintptr_t playerKey = reinterpret_cast<uintptr_t>(player);
        const bool continuous = g_cutsceneCuePlayer == playerKey &&
            g_cutsceneCueEventId == *eventIdField &&
            g_cutsceneCueFlowId == *flowIdField &&
            g_cutsceneCueLastFrame >= 0.0 &&
            frame >= g_cutsceneCueLastFrame &&
            frame - g_cutsceneCueLastFrame <= 8.0 &&
            now >= g_cutsceneCueLastTick &&
            now - g_cutsceneCueLastTick < 1000;
        if (continuous && frame > g_cutsceneCueLastFrame) {
            for (const auto& cue : kAuthoredCutsceneCues) {
                if (cue.eventId == *eventIdField && cue.flowId == *flowIdField &&
                    cue.frame > g_cutsceneCueLastFrame && cue.frame <= frame) {
                    const float span = cue.endFrame - cue.frame;
                    if (cue.power > cuePower ||
                        (cue.power == cuePower && span > cueSpanFrames)) {
                        cuePower = cue.power;
                        cueSpanFrames = span;
                    }
                }
            }
        }
        g_cutsceneCuePlayer = playerKey;
        g_cutsceneCueEventId = *eventIdField;
        g_cutsceneCueFlowId = *flowIdField;
        g_cutsceneCueLastFrame = frame;
        g_cutsceneCueLastTick = now;
    }
    if (cuePower <= 0.0f || g_menuPauseGetter == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* pauseManager = API::get()->get_managed_singleton("app.PauseManager");
    if (pauseManager == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const auto menuResult = g_menuPauseGetter->invoke(pauseManager, noArguments);
    if (menuResult.exception_thrown || menuResult.byte != 0) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const float intensity = std::clamp(0.38f + 0.62f * cuePower, 0.38f, 1.0f);
    // Track ranges are an imperfect duration hint. Keep short cues unchanged,
    // then extend longer authored ranges conservatively with a hard cap.
    const float duration = std::clamp(
        0.36f + std::max(0.0f, cueSpanFrames - 18.0f) * 0.011f,
        0.36f, 1.25f);
    bool sent = false;
    {
        std::lock_guard lock{g_mutex};
        sent = SendPlayerReactionLocked(
            PlayerHapticProfile::CinematicImpact, intensity, duration);
    }
    g_lastCutsceneAuthoredCueTick.store(now, std::memory_order_relaxed);
    API::get()->log_info(
        "[CodexDualSenseBridge] cutsceneAuthoredCue event=%d flow=%d frame=%.1f power=%.2f span=%.0f duration=%.2f write=%s",
        *eventIdField, *flowIdField, frame, cuePower, cueSpanFrames,
        duration, sent ? "ok" : "failed");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreCutsceneCameraShake(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 3 || argv == nullptr || argv[2] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG heartbeat =
        g_cutscenePlayHeartbeatTick.load(std::memory_order_acquire);
    if (heartbeat == 0 || now < heartbeat || now - heartbeat > 160) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const ULONGLONG lastAuthored = g_lastCutsceneAuthoredCueTick.load(std::memory_order_relaxed);
    if (lastAuthored != 0 && now >= lastAuthored && now - lastAuthored < 180) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    // PauseManager's broad IsPaused state can include a playing cinematic.
    // Query menu pause separately so an actual pause screen remains silent.
    if (g_menuPauseGetter == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* pauseManager = API::get()->get_managed_singleton("app.PauseManager");
    if (pauseManager == nullptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const std::vector<void*> noArguments;
    const auto menuResult = g_menuPauseGetter->invoke(pauseManager, noArguments);
    if (menuResult.exception_thrown || menuResult.byte != 0) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    auto* param = reinterpret_cast<API::ManagedObject*>(argv[2]);
    const auto* durationField = param->get_field<float>("_Duration");
    const auto* positionField = param->get_field<bool>("_PosActive");
    const auto* rotationField = param->get_field<bool>("_RotActive");
    const auto* padField = param->get_field<bool>("_PadActive");
    const bool hasVisualShake =
        (positionField != nullptr && *positionField) ||
        (rotationField != nullptr && *rotationField);
    if (durationField == nullptr || !std::isfinite(*durationField) ||
        *durationField < 0.08f || *durationField > 2.5f ||
        !hasVisualShake ||
        (padField != nullptr && *padField)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    const uintptr_t paramKey = reinterpret_cast<uintptr_t>(param);
    const ULONGLONG lastParamTick =
        g_lastCutsceneCameraShakeParamTick.load(std::memory_order_relaxed);
    if (g_lastCutsceneCameraShakeParam.load(std::memory_order_relaxed) == paramKey &&
        now >= lastParamTick &&
        now - lastParamTick < std::max<ULONGLONG>(
            700, static_cast<ULONGLONG>(*durationField * 1000.0f) + 150)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    if (!BeginEvent(g_lastCutsceneCameraShakeTick, 240)) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    g_lastCutsceneCameraShakeParam.store(paramKey, std::memory_order_relaxed);
    g_lastCutsceneCameraShakeParamTick.store(now, std::memory_order_relaxed);

    const auto* priorityField = param->get_field<int32_t>("_Priority");
    const int32_t priority = priorityField != nullptr ? *priorityField : 4;
    float intensity = std::clamp(
        0.48f + 0.04f * static_cast<float>(priority), 0.48f, 0.88f);
    if (*durationField > 1.6f) intensity *= 0.85f;
    const float duration = std::clamp(*durationField, 0.36f, 1.25f);
    bool sent = false;
    {
        std::lock_guard lock{g_mutex};
        sent = SendPlayerReactionLocked(
            PlayerHapticProfile::CinematicImpact, intensity, duration);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] cutsceneCameraShake duration=%.3f hapticDuration=%.2f priority=%d intensity=%.2f write=%s",
        *durationField, duration, priority, intensity, sent ? "ok" : "failed");
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreAdvancedVibration(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 4 || argv == nullptr || argv[3] == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Invalid haptic hook arguments argc=%d",
            argc);
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    auto* owner = reinterpret_cast<API::ManagedObject*>(argv[1]);
    auto* preset = reinterpret_cast<API::ManagedObject*>(argv[3]);
    uint32_t triggerId = 0;
    float gain = 1.0f;
    if (owner != nullptr) {
        if (auto* targetRate = owner->get_field<float>("_TargetRate")) {
            gain = *targetRate;
        }
    }
    if (auto* value = preset->get_field<uint32_t>("_WwiseTriggerId")) {
        triggerId = *value;
    } else if (auto* value = preset->get_field<uint32_t>("_VibrationTrigger")) {
        triggerId = *value;
    }
    if (IsRecentTreasureChestHaptic(GetTickCount64())) {
        API::get()->log_info(
            "[CodexDualSenseBridge] chestProbe tryADVibration trigger=%u gain=%.3f argc=%d",
            triggerId, gain, argc);
    }
    if (triggerId != 0 && triggerId != InvalidVibrationId) {
        PlayHapticTrigger(triggerId, gain);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreMotorVibrationRequest(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    // Steam Input exposes the physical DualSense to the game as a virtual
    // XInput pad.  In that mode the game skips tryADVibration, but every
    // gameplay rumble still passes through this overload first.  Capcom's
    // normal preset _VibrationTypeId is the same key used by the matching
    // cADVibrationPresetWwise._WwiseTriggerId entry.
    if (argc < 4 || argv == nullptr) {
        static std::atomic<bool> malformedArgumentsReported{false};
        if (!malformedArgumentsReported.exchange(true, std::memory_order_relaxed)) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Malformed motor vibration hook arguments argc=%d",
                argc);
        }
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    // Null request/preset objects are legitimate no-op paths inside the game.
    // They contain no vibration data to mirror and should not flood the log.
    if (argv[2] == nullptr || argv[3] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }

    auto* motorVibration = reinterpret_cast<API::ManagedObject*>(argv[2]);
    auto* preset = reinterpret_cast<API::ManagedObject*>(argv[3]);
    uint32_t triggerId = 0;
    bool distanceAttenuation = false;
    if (auto* value = preset->get_field<uint32_t>("_VibrationTypeId")) {
        triggerId = *value;
    }
    if (auto* value = preset->get_field<bool>("_IsDistanceAttenuation")) {
        distanceAttenuation = *value;
    }

    int32_t motor = -1;
    int32_t easeType = 0;
    float power = 0.0f;
    float duration = 0.0f;
    bool attenuate = false;
    if (auto* value = motorVibration->get_field<int32_t>("_MotorType")) {
        motor = *value;
    }
    if (auto* value = motorVibration->get_field<float>("_Power")) {
        power = *value;
    }
    if (auto* value = motorVibration->get_field<float>("_Time")) {
        duration = *value;
    }
    if (auto* value = motorVibration->get_field<bool>("_IsTimeAttenuation")) {
        attenuate = *value;
    }
    if (auto* value = motorVibration->get_field<int32_t>("_TimeAttenuationType")) {
        easeType = *value;
    }

    bool oniFinisherNativePulse = false;
    if (triggerId == 0 || triggerId == InvalidVibrationId) {
        const bool slashSignature = motor >= 0 && motor < 2 &&
            power >= 0.62f && power <= 0.78f &&
            duration >= 0.18f && duration <= 0.26f && !attenuate;
        if (slashSignature &&
            g_oniTransformationActive.load(std::memory_order_acquire)) {
            const ULONGLONG now = GetTickCount64();
            for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
                uint8_t stage = g_oniChargeFallbackKind[user].load(
                    std::memory_order_relaxed);
                const ULONGLONG stageTick = g_oniChargeFallbackTick[user].load(
                    std::memory_order_relaxed);
                if (stage == OniFinisherPreparingCode && stageTick != 0 &&
                    now >= stageTick && now - stageTick <= 1600u) {
                    if (g_oniChargeFallbackKind[user].compare_exchange_strong(
                            stage, OniFinisherSlashSentCode,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
                        g_oniChargeFallbackTick[user].store(now, std::memory_order_relaxed);
                        const bool sent = SendOniChargeReleaseHaptic(
                            normal_attack_haptics::Kind::ChargeHeavy,
                            oni_charge_release_haptics::Kind::FinisherSlash);
                        API::get()->log_info(
                            "[CodexDualSenseBridge] oniFinisher slash source=native-motor motor=%d power=%.3f duration=%.3f write=%s",
                            motor, power, duration, sent ? "ok" : "failed");
                    }
                    oniFinisherNativePulse = true;
                    break;
                }
                if (stage == OniFinisherSlashSentCode && stageTick != 0 &&
                    now >= stageTick && now - stageTick <= 120u) {
                    oniFinisherNativePulse = true;
                    break;
                }
            }
        }
    }
    if (IsRecentTreasureChestHaptic(GetTickCount64())) {
        API::get()->log_info(
            "[CodexDualSenseBridge] chestProbe addVibrationRequest trigger=%u distance=%s motor=%d power=%.3f duration=%.3f attenuate=%s ease=%d argc=%d",
            triggerId,
            distanceAttenuation ? "yes" : "no",
            motor,
            power,
            duration,
            attenuate ? "yes" : "no",
            easeType,
            argc);
    }

    if (triggerId != 0 && triggerId != InvalidVibrationId) {
        if (distanceAttenuation) {
            QueueSpatialHaptic(triggerId);
        } else {
            PlayHapticTrigger(triggerId, 1.0f);
        }
    } else if (!distanceAttenuation && !oniFinisherNativePulse) {
        if (motor >= 0 && motor < 4) {
            PlayMotorPulse(
                static_cast<uint32_t>(motor),
                power,
                duration,
                attenuate,
                easeType);
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PrePostVibrationEvent(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 5 || argv == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const uint32_t triggerId = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(argv[2]));
    const float leftGain = FloatArgument(argv[3]);
    const float rightGain = FloatArgument(argv[4]);
    if (IsRecentTreasureChestHaptic(GetTickCount64())) {
        API::get()->log_info(
            "[CodexDualSenseBridge] chestProbe onPostVibrationEvent trigger=%u left=%.3f right=%.3f argc=%d",
            triggerId, leftGain, rightGain, argc);
    }
    PlayHapticTriggerStereo(triggerId, leftGain, rightGain);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreSetMotorPower(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc < 4 || argv == nullptr || argv[1] == nullptr) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    g_nativeGamePadDevice.store(
        reinterpret_cast<uintptr_t>(argv[1]), std::memory_order_release);
    if (g_nativeMotorInvocation) {
        // Native chest fallback is intentionally allowed to reach the game's
        // own setter without being mirrored back into the helper pipe.
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const uint32_t motor = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(argv[2]));
    if (motor >= 4) {
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    const float power = live_motor_state::Sanitize(FloatArgument(argv[3]));
    const ULONGLONG now = GetTickCount64();
    if (IsRecentTreasureChestHaptic(now)) {
        API::get()->log_info(
            "[CodexDualSenseBridge] chestProbe setMotorPower motor=%u power=%.3f argc=%d",
            motor, power, argc);
    }
    const ULONGLONG executionUntil =
        g_executionStandardRumbleSuppressionUntil.load(std::memory_order_acquire);
    if (power > 0.0f && now < executionUntil) {
        const uint32_t count = g_executionStandardRumbleSuppressedCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count <= 8) {
            API::get()->log_info(
                "[CodexDualSenseBridge] Execution skipped live motor write motor=%u power=%.3f count=%u",
                motor, power, count);
        }
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    const ULONGLONG suppressionUntil =
        g_repelStandardRumbleSuppressionUntil.load(std::memory_order_acquire);
    if (power > 0.0f && combat_defense_feedback::RepelImpactWindowActive(
            now, suppressionUntil)) {
        const uint32_t count = g_repelStandardRumbleSuppressedCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count <= 8) {
            API::get()->log_info(
                "[CodexDualSenseBridge] Repel skipped game motor write motor=%u power=%.3f count=%u",
                motor,
                power,
                count);
        }
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    std::lock_guard lock{g_mutex};
    const bool accepted = !g_gameplayHapticsPaused.load();
    const uintptr_t source = reinterpret_cast<uintptr_t>(argv[1]);
    LogMotorSourceLocked(live_motor_state::Kind::NativePad, source, motor, power, accepted);
    if (accepted) g_liveMotorSources.Native(source, motor, power, now);
    PublishMotorPowerLocked();
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreGetMotorPower(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr) {
        const uintptr_t deviceKey = reinterpret_cast<uintptr_t>(argv[1]);
        const uintptr_t previous = g_nativeGamePadDevice.exchange(
            deviceKey, std::memory_order_acq_rel);
        if (previous != deviceKey) {
            API::get()->log_info(
                "[CodexDualSenseBridge] Native GamePadDevice captured via getMotorPower device=%p",
                reinterpret_cast<void*>(deviceKey));
        }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreResetMotors(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    if (argc >= 2 && argv != nullptr && argv[1] != nullptr) {
        std::lock_guard lock{g_mutex};
        const ULONGLONG now = GetTickCount64();
        const bool preserveTreasureChest =
            !g_gameplayHapticsPaused.load(std::memory_order_relaxed) &&
            IsRecentTreasureChestHaptic(now);
        StopMotorHapticsLocked(preserveTreasureChest);
        if (preserveTreasureChest) {
            API::get()->log_info(
                "[CodexDualSenseBridge] motor reset preserved recent treasure chest haptic");
        }
        API::get()->log_info("[CodexDualSenseBridge] liveMotor cache cleared reason=resetMotors");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreStopMotorHaptics(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    std::lock_guard lock{g_mutex};
    const ULONGLONG now = GetTickCount64();
    const bool preserveTreasureChest =
        !g_gameplayHapticsPaused.load(std::memory_order_relaxed) &&
        IsRecentTreasureChestHaptic(now);
    StopMotorHapticsLocked(preserveTreasureChest);
    if (preserveTreasureChest) {
        API::get()->log_info(
            "[CodexDualSenseBridge] stop motor preserved recent treasure chest haptic");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreStopWaveforms(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    std::lock_guard lock{g_mutex};
    StopWaveformsLocked();
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PreStopHaptics(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    // Pause/menu transitions clear the game's haptics, but XInput continues to
    // report a held L2. Keep empty-soul feedback disarmed until that physical
    // input is released so the next polling frame cannot restart the texture.
    SuppressSoulSearchForHeldInputs();
    StopWallRunHaptics();
    StopLadderHaptics();
    StopSoulStreamFeedback(false);
    g_lastBlackSoulStateTick.store(0, std::memory_order_relaxed);
    SetBlackSoulAbsorptionHaptic(false);
    g_lastBlackSoulMainState.store(0, std::memory_order_relaxed);
    g_lastBlackSoulSubState.store(0, std::memory_order_relaxed);
    g_soulActionActive.store(false, std::memory_order_relaxed);
    for (auto& held : g_soulActionInputHeld) {
        held.store(false, std::memory_order_relaxed);
    }
    g_soulSearchHapticActive.store(false, std::memory_order_relaxed);
    g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
    g_lastSoulSearchRefreshTick.store(0, std::memory_order_relaxed);
    std::lock_guard lock{g_mutex};
    // Game-originated rumble cleanup must not erase the independent swing or
    // cutting-contact envelope. Real pause/reset paths explicitly cancel it.
    const bool paused = g_gameplayHapticsPaused.load(std::memory_order_relaxed);
    const bool preserveExecution = !paused && GetTickCount64() <
        g_executionStandardRumbleSuppressionUntil.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    const bool preserveTreasureChest = !paused &&
        IsRecentTreasureChestHaptic(now);
    StopHapticsLocked(!paused, preserveExecution, preserveTreasureChest);
    if (preserveTreasureChest) {
        API::get()->log_info(
            "[CodexDualSenseBridge] stop haptics preserved recent treasure chest haptic");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int PrePauseHaptics(
    int argc,
    void** argv,
    REFrameworkTypeDefinitionHandle* type,
    unsigned long long id) {
    const bool previous = g_gameplayHapticsPaused.exchange(
        true, std::memory_order_relaxed);
    if (!previous) {
        API::get()->log_info(
            "[CodexDualSenseBridge] gameplayHaptics paused=1");
    }
    CancelNormalAttackHaptics();
    g_executionStandardRumbleSuppressionUntil.store(0, std::memory_order_relaxed);
    return PreStopHaptics(argc, argv, type, id);
}

int PreResumeHaptics(
    int,
    void**,
    REFrameworkTypeDefinitionHandle*,
    unsigned long long) {
    const bool previous = g_gameplayHapticsPaused.exchange(
        false, std::memory_order_relaxed);
    if (previous) {
        API::get()->log_info(
            "[CodexDualSenseBridge] gameplayHaptics paused=0");
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void OnLuaStateDestroyed(lua_State*) {
    {
        std::lock_guard lock{g_mapMutex};
        g_mapHapticStates.clear();
    }
    {
        std::lock_guard lock{g_longPressMutex};
        g_longPressStates.clear();
        g_equipmentEnhanceLongPressState = {};
        g_lastEquipmentEnhanceVisibleTick.store(0, std::memory_order_relaxed);
        g_lastEquipmentEnhanceCompleteTick.store(0, std::memory_order_relaxed);
    }
    g_longPressUpdateStack.clear();
    g_equipmentEnhanceGaugeStack.clear();
    {
        std::lock_guard lock{g_mutex};
        g_activeEffects = 0;
        g_liveMotorSources.Clear();
        g_shortcutWheelSelections.clear();
        g_lastRainLevel = -1.0f;
        g_lastWindLevel = -1.0f;
        g_lastEnvironmentPollTick = 0;
        g_lastEnvironmentSendTick = 0;
        g_lastFootstepTick.store(0, std::memory_order_relaxed);
        for (auto& tick : g_lastFootstepSideTick) {
            tick.store(0, std::memory_order_relaxed);
        }
        g_lastMapFocusTick.store(0, std::memory_order_relaxed);
        g_lastMapMarkerTick.store(0, std::memory_order_relaxed);
        g_lastMapVisibleTick.store(0, std::memory_order_relaxed);
        g_lastOniEyeTick.store(0, std::memory_order_relaxed);
        g_bowTriggerSuppressedByLeftTrigger.store(false, std::memory_order_relaxed);
        g_playerActionSelector.store(0, std::memory_order_relaxed);
        g_lastOniTransformationTick.store(0, std::memory_order_relaxed);
        g_oniTransformationActive.store(false, std::memory_order_relaxed);
        g_ordinaryFormActive.store(false, std::memory_order_relaxed);
        g_pendingOniChargeReleaseKind.store(0, std::memory_order_relaxed);
        g_pendingOniChargeReleaseTick.store(0, std::memory_order_relaxed);
        g_pendingOniChargeOwner.store(0, std::memory_order_relaxed);
        g_lastOniChargeHitTick.store(0, std::memory_order_relaxed);
        for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
            g_oniChargeButtonDownTick[user].store(0, std::memory_order_relaxed);
            g_oniChargeRefreshTick[user].store(0, std::memory_order_relaxed);
            g_oniChargeInputKind[user].store(0, std::memory_order_relaxed);
            g_oniChargeFeedbackActive[user].store(false, std::memory_order_relaxed);
            g_oniChargeFallbackKind[user].store(0, std::memory_order_relaxed);
            g_oniChargeFallbackTick[user].store(0, std::memory_order_relaxed);
        }
        g_lastTreasureChestOpenTick.store(0, std::memory_order_relaxed);
        g_lastTreasureChestPushTick.store(0, std::memory_order_relaxed);
        g_lastTreasureChestLidLandTick.store(0, std::memory_order_relaxed);
        g_activeTreasureChest.store(0, std::memory_order_relaxed);
        {
            std::lock_guard normalLock{g_normalAttackMutex};
            g_normalAttackTracker.Clear();
            g_oniChargeAttackTracker.Clear();
            g_ordinaryChargeAttackTracker.Clear();
        }
        g_wallRunHapticActive.store(false, std::memory_order_relaxed);
        g_wallRunAlongWallActive.store(false, std::memory_order_relaxed);
        {
            std::lock_guard landingLock{g_wallRunLandingMutex};
            g_wallRunLandingTracker.Reset();
        }
        g_lastWallRunAirRefreshLogTick.store(0, std::memory_order_relaxed);
        g_wallRunActionObject.store(0, std::memory_order_relaxed);
        g_lastWallRunStateTick.store(0, std::memory_order_relaxed);
        g_lastWallRunRefreshTick.store(0, std::memory_order_relaxed);
        g_wallRunImpulseUntilTick.store(0, std::memory_order_relaxed);
        g_lastWallRunEnteredObject.store(0, std::memory_order_relaxed);
        g_lastWallRunEnteredTick.store(0, std::memory_order_relaxed);
        g_lastWallRunContactTick.store(0, std::memory_order_relaxed);
        g_lastWallRunLandTick.store(0, std::memory_order_relaxed);
        for (auto& tick : g_lastWallRunFootTick) tick.store(0, std::memory_order_relaxed);
        g_ladderHapticActive.store(false, std::memory_order_relaxed);
        g_ladderHapticProfile.store(ladder_haptics::ClimbProfile,
            std::memory_order_relaxed);
        g_lastLadderStateTick.store(0, std::memory_order_relaxed);
        g_lastLadderRefreshTick.store(0, std::memory_order_relaxed);
        g_ladderActionObject.store(0, std::memory_order_relaxed);
        g_lastLadderEnteredObject.store(0, std::memory_order_relaxed);
        g_lastLadderEnteredTick.store(0, std::memory_order_relaxed);
        for (auto& tick : g_lastPlayerReactionTick) {
            tick.store(0, std::memory_order_relaxed);
        }
        g_cutscenePlayHeartbeatTick.store(0, std::memory_order_relaxed);
        g_lastCutsceneCameraShakeTick.store(0, std::memory_order_relaxed);
        g_lastCutsceneCameraShakeParam.store(0, std::memory_order_relaxed);
        g_lastCutsceneCameraShakeParamTick.store(0, std::memory_order_relaxed);
        g_lastCutsceneAuthoredCueTick.store(0, std::memory_order_relaxed);
        {
            std::lock_guard stateLock{g_cutsceneCueStateMutex};
            g_cutsceneCuePlayer = 0;
            g_cutsceneCueEventId = -1;
            g_cutsceneCueFlowId = -1;
            g_cutsceneCueLastFrame = -1.0;
            g_cutsceneCueLastTick = 0;
        }
        g_lastSoulCollectTick.store(0, std::memory_order_relaxed);
        g_lastSoulFlowEvidenceTick.store(0, std::memory_order_relaxed);
        g_blackSoulHapticActive.store(false, std::memory_order_relaxed);
        g_lastBlackSoulStateTick.store(0, std::memory_order_relaxed);
        g_lastBlackSoulRefreshTick.store(0, std::memory_order_relaxed);
        g_lastBlackSoulMainState.store(0, std::memory_order_relaxed);
        g_lastBlackSoulSubState.store(0, std::memory_order_relaxed);
        g_blackSoulFinishUntilTick.store(0, std::memory_order_relaxed);
        g_soulSpeakerActive.store(false, std::memory_order_relaxed);
        g_soulHapticActive.store(false, std::memory_order_relaxed);
        g_lastSoulHapticRefreshTick.store(0, std::memory_order_relaxed);
        g_soulActionActive.store(false, std::memory_order_relaxed);
        for (auto& held : g_soulActionInputHeld) {
            held.store(false, std::memory_order_relaxed);
        }
        g_soulSearchSuppressedUserMask.store(0, std::memory_order_relaxed);
        g_lastParryInputTick.store(0, std::memory_order_relaxed);
        g_repelStandardRumbleSuppressionUntil.store(0, std::memory_order_relaxed);
        g_repelStandardRumbleSuppressedCount.store(0, std::memory_order_relaxed);
        g_executionStandardRumbleSuppressionUntil.store(0, std::memory_order_relaxed);
        g_executionStandardRumbleSuppressedCount.store(0, std::memory_order_relaxed);
        g_lastExecutionHapticTick.store(0, std::memory_order_relaxed);
        g_gameplayHapticsPaused.store(false, std::memory_order_relaxed);
        g_soulSearchHapticActive.store(false, std::memory_order_relaxed);
        g_soulSearchCandidateTick.store(0, std::memory_order_relaxed);
        g_lastSoulSearchRefreshTick.store(0, std::memory_order_relaxed);
        g_lastSprintButtonTick.store(0, std::memory_order_relaxed);
        for (auto& buttons : g_lastXInputButtons) {
            buttons.store(0, std::memory_order_relaxed);
        }
        for (auto& active : g_sprintToggleActive) {
            active.store(false, std::memory_order_relaxed);
        }
        for (auto& tick : g_sprintSlowCandidateTick) {
            tick.store(0, std::memory_order_relaxed);
        }
        for (auto& magnitude : g_sprintStartStickMagnitude) {
            magnitude.store(0.0f, std::memory_order_relaxed);
        }
        g_lastLoggedSurface.store(0xFFFFFFFFu, std::memory_order_relaxed);
        g_sentMotorPower.fill(-1.0f);
        StopHelperLocked();
    }
}

bool AddHook(API::TypeDefinition* managerType, const char* methodName, REFPreHookFn callback) {
    auto* method = managerType->find_method(methodName);
    if (method == nullptr) {
        API::get()->log_error(
            "[CodexDualSenseBridge] Required method missing: app.AdaptiveTriggerManager.%s",
            methodName);
        return false;
    }
    const unsigned int hookId = method->add_hook(callback, nullptr, false);
    if (hookId == 0) {
        API::get()->log_error(
            "[CodexDualSenseBridge] Hook failed: app.AdaptiveTriggerManager.%s",
            methodName);
        return false;
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Hooked app.AdaptiveTriggerManager.%s id=%u",
        methodName,
        hookId);
    return true;
}

bool AddOptionalHook(
    const char* typeName,
    const char* methodName,
    REFPreHookFn callback) {
    auto* type = API::get()->tdb()->find_type(typeName);
    if (type == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Optional haptics type missing: %s",
            typeName);
        return false;
    }
    auto* method = type->find_method(methodName);
    if (method == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Optional haptics method missing: %s.%s",
            typeName,
            methodName);
        return false;
    }
    const unsigned int hookId = method->add_hook(callback, nullptr, false);
    if (hookId == 0) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Optional haptics hook failed: %s.%s",
            typeName,
            methodName);
        return false;
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Hooked %s.%s id=%u",
        typeName,
        methodName,
        hookId);
    return true;
}

bool AddOptionalPostHook(
    const char* typeName,
    const char* methodName,
    REFPostHookFn callback) {
    auto* type = API::get()->tdb()->find_type(typeName);
    if (type == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Optional event type missing: %s",
            typeName);
        return false;
    }
    auto* method = type->find_method(methodName);
    if (method == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Optional event method missing: %s.%s",
            typeName,
            methodName);
        return false;
    }
    const unsigned int hookId = method->add_hook(nullptr, callback, false);
    if (hookId == 0) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Optional event post-hook failed: %s.%s",
            typeName,
            methodName);
        return false;
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Hooked %s.%s post id=%u",
        typeName,
        methodName,
        hookId);
    return true;
}

uint32_t AddInteractionEntryHooks() {
    auto* tdb = API::get()->tdb();
    void* emptyStub = nullptr;
    if (auto* emptyType = tdb->find_type("app.cGimmickInteractionControllerBase")) {
        if (auto* emptyMethod = emptyType->find_method("start")) {
            emptyStub = emptyMethod->get_function_raw();
        }
    }

    std::unordered_set<void*> hookedFunctions;
    uint32_t count = 0;
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;

        const std::string ownerName = owner->get_full_name();
        const char* methodName = method->get_name();
        const bool playerInteractionEntry =
            ownerName.find("app.PlayerCommonAction.") == 0 &&
            ownerName.find("Interact") != std::string::npos &&
            std::strcmp(methodName, "detailEnter") == 0;
        const bool controllerInteractionStart =
            ownerName.find("InteractionController") != std::string::npos &&
            std::strcmp(methodName, "start") == 0;
        if (!playerInteractionEntry && !controllerInteractionStart) continue;

        void* function = method->get_function_raw();
        if (function == nullptr || function == emptyStub ||
            !hookedFunctions.insert(function).second) {
            continue;
        }
        const unsigned int hookId = method->add_hook(PreInteractionEntered, nullptr, false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Interaction hook failed: %s.%s",
                ownerName.c_str(),
                methodName);
            continue;
        }
        ++count;
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked interaction event %s.%s id=%u",
            ownerName.c_str(),
            methodName,
            hookId);
    }
    return count;
}

uint32_t AddNormalAttackHapticHook() {
    auto* tdb = API::get()->tdb();
    auto* type = tdb->find_type("app.PlayerCommonAction.cAttackMultiMotionBase");
    auto* chargeBase = tdb->find_type("app.PlayerCommonAction.cAttackChargeBase");
    auto* playerActionBase = tdb->find_type("app.PlayerActionBase.cPlayerActionBase");
    auto* soundType = tdb->find_type("app.snd_user_data.SoundHitSwitchListBehavior");
    struct Target { API::TypeDefinition* type; const char* name; uint32_t params; REFPreHookFn pre; REFPostHookFn post; API::Method* method{}; };
    std::array<Target, 8> targets{{
        {type, "detailEnter", 0u, PreNormalAttackEntered, PostNormalAttackEntered},
        {type, "detailUpdate", 0u, PreNormalAttackEntered, PostNormalAttackEntered},
        {type, "detailExit", 0u, PreNormalAttackExited, nullptr},
        {chargeBase, "detailEnter", 0u, PreNormalAttackEntered, PostNormalAttackEntered},
        {chargeBase, "detailUpdate", 0u, PreNormalAttackEntered, PostNormalAttackEntered},
        {chargeBase, "detailExit", 0u, PreNormalAttackExited, nullptr},
        {playerActionBase, "updateSweepShell", 1u, PreOrdinaryLightChargeSweepProbe, nullptr},
        {soundType, "applyHitSound", 1u, PreNormalAttackHit, PostNormalAttackHit}
    }};
    std::unordered_map<void*, uint32_t> uses;
    for (auto& target : targets) {
        target.method = target.type != nullptr ? target.type->find_method(target.name) : nullptr;
        if (target.method == nullptr || target.method->get_declaring_type() != target.type ||
            target.method->get_num_params() != target.params || target.method->get_function_raw() == nullptr) { target.method = nullptr; continue; }
        uses.emplace(target.method->get_function_raw(), 0u);
    }
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* candidate = tdb->get_method(index);
        if (candidate == nullptr) continue;
        const auto found = uses.find(candidate->get_function_raw());
        if (found != uses.end()) ++found->second;
    }
    uint32_t hooked = 0;
    std::unordered_set<void*> registered;
    for (const auto& target : targets) {
        if (target.method == nullptr || uses[target.method->get_function_raw()] != 1u) {
            API::get()->log_warn("[CodexDualSenseBridge] Normal attack hook unavailable/shared %s", target.name); continue;
        }
        const auto id = target.method->add_hook(target.pre, target.post, false);
        if (id != 0) {
            ++hooked;
            registered.insert(target.method->get_function_raw());
        }
        API::get()->log_info("[CodexDualSenseBridge] Normal attack hook %s.%s id=%u strict-actions=10 internal=always-on fighting-spirit-or-oni",
            target.type->get_full_name().c_str(), target.name, id);
    }
    constexpr const char* ChargeTypes[] = {
        "app.PlayerOneHandedAction.cAttackWeakCharge",
        "app.PlayerOneHandedAction.cAttackStrongCharge"};
    for (const char* typeName : ChargeTypes) {
        auto* chargeType = tdb->find_type(typeName);
        auto* method = chargeType != nullptr ? chargeType->find_method("checkEnd") : nullptr;
        void* function = method != nullptr ? method->get_function_raw() : nullptr;
        if (method == nullptr || method->get_num_params() != 0 || function == nullptr ||
            registered.contains(function)) continue;
        const auto id = method->add_hook(PreChargeCheckEnd, PostChargeCheckEnd, false);
        if (id == 0) continue;
        registered.insert(function);
        ++hooked;
        API::get()->log_info(
            "[CodexDualSenseBridge] Light/heavy charge action hook %s.checkEnd id=%u",
            typeName, id);
    }
    return hooked;
}

uint32_t AddWallRunHapticHooks() {
    struct Target {
        const char* typeName;
        const char* methodName;
        REFPreHookFn callback;
        API::Method* method{};
    };
    std::array<Target, 10> targets{{
        {"app.PlayerCommonAction.cWallRunActionBase", "detailEnter", PreWallRunActionEntered},
        {"app.PlayerCommonAction.cWallRunStartJumpBase", "detailEnter", PreWallRunActionEntered},
        {"app.PlayerCommonAction.cWallRunLandBase", "detailEnter", PreWallRunActionEntered},
        {"app.PlayerCommonAction.cWallRunFallBase", "detailEnter", PreWallRunActionEntered},
        {"app.PlayerCommonAction.cWallRunActionBase", "detailUpdate", PreWallRunActionUpdated},
        {"app.PlayerCommonAction.cWallRunLoopMotion", "detailUpdate", PreWallRunActionUpdated},
        {"app.PlayerCommonAction.cWallRunOneMotion", "detailUpdate", PreWallRunActionUpdated},
        {"app.PlayerCommonAction.cWallRunStartJumpBase", "detailUpdate", PreWallRunActionUpdated},
        {"app.PlayerCommonAction.cWallRunFallBase", "detailUpdate", PreWallRunActionUpdated},
        {"app.PlayerCommonAction.cWallRunActionBase", "detailExit", PreWallRunActionExited}
    }};
    auto* tdb = API::get()->tdb();
    std::unordered_map<void*, uint32_t> uses;
    for (auto& target : targets) {
        auto* type = tdb->find_type(target.typeName);
        target.method = type != nullptr ? type->find_method(target.methodName) : nullptr;
        if (target.method != nullptr && (target.method->get_num_params() != 0 ||
            target.method->get_declaring_type() != type)) target.method = nullptr;
        if (target.method != nullptr && target.method->get_function_raw() != nullptr)
            uses.emplace(target.method->get_function_raw(), 0);
    }
    // A real implementation must not be a shared return/no-op stub. Checking
    // only these ten addresses keeps registration bounded and conservative.
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        const auto found = uses.find(method->get_function_raw());
        if (found != uses.end()) ++found->second;
    }
    uint32_t hooked = 0;
    std::unordered_set<void*> registered;
    for (const auto& target : targets) {
        void* function = target.method != nullptr ? target.method->get_function_raw() : nullptr;
        if (function == nullptr || uses[function] != 1 ||
            !registered.insert(function).second) {
            API::get()->log_warn("[CodexDualSenseBridge] Wall Run method missing/shared, skipped %s.%s",
                target.typeName, target.methodName);
            continue;
        }
        const unsigned hookId = target.method->add_hook(target.callback, nullptr, false);
        if (hookId != 0) ++hooked;
        API::get()->log_info("[CodexDualSenseBridge] Wall Run hook %s.%s id=%u",
            target.typeName, target.methodName, hookId);
    }
    API::get()->log_info("[CodexDualSenseBridge] Wall Run haptics coverage=%u/10 takeoff/flight/contact/loop/leap/landing strict-player-actions",
        hooked);
    return hooked;
}

uint32_t AddLadderHapticHooks() {
    struct Target {
        const char* typeName;
        const char* methodName;
        REFPreHookFn callback;
        API::Method* method{};
    };
    std::array<Target, 6> targets{{
        {"app.PlayerCommonAction.cLadderActionBase", "detailEnter",
            PreLadderActionEntered},
        {"app.PlayerCommonAction.cLadderActionBase", "detailExit",
            PreLadderActionExited},
        {"app.PlayerCommonAction.cLadderOneMotion", "detailUpdate",
            PreLadderActionUpdated},
        {"app.PlayerCommonAction.cLadderLoopBase", "detailUpdate",
            PreLadderActionUpdated},
        {"app.PlayerCommonAction.cLadderFallBase", "detailEnter",
            PreLadderActionEntered},
        {"app.PlayerCommonAction.cLadderFallBase", "detailUpdate",
            PreLadderActionUpdated}
    }};

    auto* tdb = API::get()->tdb();
    std::unordered_map<void*, uint32_t> uses;
    for (auto& target : targets) {
        auto* type = tdb->find_type(target.typeName);
        target.method = type != nullptr ? type->find_method(target.methodName) : nullptr;
        if (target.method != nullptr && (target.method->get_num_params() != 0 ||
            target.method->get_declaring_type() != type)) {
            target.method = nullptr;
        }
        if (target.method != nullptr && target.method->get_function_raw() != nullptr) {
            uses.emplace(target.method->get_function_raw(), 0);
        }
    }
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        const auto found = uses.find(method->get_function_raw());
        if (found != uses.end()) ++found->second;
    }

    uint32_t hooked = 0;
    std::unordered_set<void*> registered;
    for (const auto& target : targets) {
        void* function = target.method != nullptr
            ? target.method->get_function_raw() : nullptr;
        if (function == nullptr || uses[function] != 1u ||
            !registered.insert(function).second) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Ladder method missing/shared, skipped %s.%s",
                target.typeName, target.methodName);
            continue;
        }
        const unsigned hookId = target.method->add_hook(
            target.callback, nullptr, false);
        if (hookId != 0) ++hooked;
        API::get()->log_info(
            "[CodexDualSenseBridge] Ladder hook %s.%s id=%u",
            target.typeName, target.methodName, hookId);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Ladder haptics coverage=%u/6 strict-player-actions",
        hooked);
    return hooked;
}

uint32_t AddGimmickInteractionSuccessHooks() {
    auto* tdb = API::get()->tdb();
    std::unordered_map<void*, uint32_t> functionUseCount;
    functionUseCount.reserve(tdb->get_num_methods());
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        if (void* function = method->get_function_raw(); function != nullptr) {
            ++functionUseCount[function];
        }
    }
    std::unordered_set<void*> hookedFunctions;
    uint32_t count = 0;
    uint32_t sharedSkipped = 0;
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr || method->is_static() ||
            std::strcmp(method->get_name(), "onGmInteract_Success") != 0) {
            continue;
        }
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;
        const std::string ownerName = owner->get_full_name();
        if (ownerName != "app.AppGimmickBase" &&
            !owner->is_derived_from("app.AppGimmickBase")) {
            continue;
        }

        void* function = method->get_function_raw();
        if (function == nullptr) continue;
        if (functionUseCount[function] != 1) {
            ++sharedSkipped;
            continue;
        }
        if (!hookedFunctions.insert(function).second) {
            continue;
        }
        const unsigned int hookId = method->add_hook(
            PreGimmickInteractionSucceeded, nullptr, false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Gimmick success hook failed: %s.%s",
                ownerName.c_str(), method->get_name());
            continue;
        }
        ++count;
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked gimmick success %s.%s id=%u",
            ownerName.c_str(), method->get_name(), hookId);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Gimmick success registration unique=%u sharedStubsSkipped=%u",
        count,
        sharedSkipped);
    return count;
}

uint32_t AddLargeTreasureChestHapticHooks() {
    uint32_t count = 0;
    if (AddOptionalHook(
            "app.GimmickTreasureBox",
            "changeState",
            PreTreasureBoxStateChanged)) {
        ++count;
    }
    if (AddOptionalHook(
            "app.GimmickTreasureBox",
            "changeStateEvent",
            PreTreasureBoxStateChanged)) {
        ++count;
    }
    if (AddOptionalHook(
            "app.Gm002_010",
            "release",
            PreLargeTreasureChestPush)) {
        ++count;
    }
    if (AddOptionalHook(
            "app.Gm002_010",
            "changeFloatState",
            PreLargeTreasureChestFloatState)) {
        ++count;
    }
    if (AddOptionalHook(
            "app.Gm002_010",
            "changeFloatStateEvent",
            PreLargeTreasureChestFloatState)) {
        ++count;
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Large treasure chest haptics coverage=%u/5 base-state+push/release/land-state",
        count);
    return count;
}

uint32_t AddDoorOpeningUpdateHooks() {
    auto* tdb = API::get()->tdb();
    std::unordered_map<void*, uint32_t> functionUseCount;
    functionUseCount.reserve(tdb->get_num_methods());
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        if (void* function = method->get_function_raw(); function != nullptr) {
            ++functionUseCount[function];
        }
    }

    std::unordered_set<void*> hookedFunctions;
    uint32_t count = 0;
    uint32_t sharedSkipped = 0;
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr || method->is_static() ||
            std::strcmp(method->get_name(), "updateOpening") != 0) {
            continue;
        }
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;
        const std::string ownerName = owner->get_full_name();
        if (ownerName != "app.GimmickDoor" &&
            !owner->is_derived_from("app.GimmickDoor")) {
            continue;
        }

        void* function = method->get_function_raw();
        if (function == nullptr) continue;
        // Generic compiler stubs can be shared by unrelated methods. Hook only
        // concrete, address-unique implementations and validate the runtime
        // object again in the callback.
        if (functionUseCount[function] != 1) {
            ++sharedSkipped;
            continue;
        }
        if (!hookedFunctions.insert(function).second) continue;
        const unsigned int hookId = method->add_hook(
            PreDoorOpeningUpdated, nullptr, false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Door opening hook failed: %s.%s",
                ownerName.c_str(), method->get_name());
            continue;
        }
        ++count;
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked door opening update %s.%s id=%u",
            ownerName.c_str(), method->get_name(), hookId);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Door opening registration unique=%u sharedStubsSkipped=%u",
        count,
        sharedSkipped);
    return count;
}

uint32_t AddLeverChangeHooks() {
    auto* tdb = API::get()->tdb();
    std::unordered_map<void*, uint32_t> functionUseCount;
    functionUseCount.reserve(tdb->get_num_methods());
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        if (void* function = method->get_function_raw(); function != nullptr) {
            ++functionUseCount[function];
        }
    }
    std::unordered_set<void*> hookedFunctions;
    uint32_t count = 0;
    uint32_t sharedSkipped = 0;
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr || method->is_static() ||
            std::strcmp(method->get_name(), "leverChangeEvent") != 0) {
            continue;
        }
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;
        const std::string ownerName = owner->get_full_name();
        if (ownerName.find("app.") != 0) continue;

        void* function = method->get_function_raw();
        if (function == nullptr) continue;
        if (functionUseCount[function] != 1) {
            ++sharedSkipped;
            continue;
        }
        if (!hookedFunctions.insert(function).second) {
            continue;
        }
        const unsigned int hookId = method->add_hook(
            PreLeverChanged, nullptr, false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Lever hook failed: %s.%s",
                ownerName.c_str(), method->get_name());
            continue;
        }
        ++count;
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked lever change %s.%s id=%u",
            ownerName.c_str(), method->get_name(), hookId);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Lever registration unique=%u sharedStubsSkipped=%u",
        count,
        sharedSkipped);
    return count;
}

uint32_t AddBowActionHooks() {
    auto* tdb = API::get()->tdb();
    std::unordered_set<void*> hookedEnterFunctions;
    std::unordered_set<void*> hookedExitFunctions;
    uint32_t count = 0;

    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;

        const std::string ownerName = owner->get_full_name();
        const bool bowOwner =
            ownerName.find("app.PlayerTwoHandedAction.cBow") == 0 ||
            ownerName.find("app.PlayerTwoHandedSubAction.cBow") == 0;
        if (!bowOwner) continue;

        const char* methodName = method->get_name();
        const bool isEnter = std::strcmp(methodName, "detailEnter") == 0;
        const bool isExit = std::strcmp(methodName, "detailExit") == 0;
        if (!isEnter && !isExit) continue;

        void* function = method->get_function_raw();
        auto& functions = isEnter ? hookedEnterFunctions : hookedExitFunctions;
        if (function == nullptr || !functions.insert(function).second) continue;

        const unsigned int hookId = method->add_hook(
            isEnter ? PreBowActionEntered : PreBowActionExited,
            nullptr,
            false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Bow action hook failed: %s.%s",
                ownerName.c_str(),
                methodName);
            continue;
        }
        ++count;
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked bow action %s.%s id=%u",
            ownerName.c_str(),
            methodName,
            hookId);
    }

    return count;
}

uint32_t AddUseItemActionHooks() {
    auto* tdb = API::get()->tdb();
    std::unordered_set<void*> hookedFunctions;
    uint32_t count = 0;

    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr) continue;
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;

        const std::string ownerName = owner->get_full_name();
        const bool useItemAction =
            ownerName.find("app.Player") == 0 &&
            ownerName.find("SubAction.cUseItem") != std::string::npos;
        if (!useItemAction || std::strcmp(method->get_name(), "detailEnter") != 0) {
            continue;
        }

        void* function = method->get_function_raw();
        if (function == nullptr || !hookedFunctions.insert(function).second) {
            continue;
        }
        const unsigned int hookId = method->add_hook(PreItemUsed, nullptr, false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Use-item hook failed: %s.detailEnter",
                ownerName.c_str());
            continue;
        }
        ++count;
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked use-item event %s.detailEnter id=%u",
            ownerName.c_str(),
            hookId);
    }
    return count;
}

struct PlayerReactionHookCoverage {
    uint32_t damage = 0;
    uint32_t dodge = 0;
    uint32_t sharedGrabEntry = 0;
    uint32_t grabSustain = 0;
};

PlayerReactionHookCoverage AddPlayerReactionHooks() {
    auto* tdb = API::get()->tdb();
    std::unordered_set<void*> hookedFunctions;
    std::unordered_set<void*> hookedSustainFunctions;
    PlayerReactionHookCoverage coverage{};

    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr ||
            std::strcmp(method->get_name(), "detailEnter") != 0) {
            continue;
        }
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;
        const std::string ownerName = owner->get_full_name();
        const bool damage = IsPlayerDamageAction(ownerName);
        const bool dodge = IsPlayerDodgeAction(ownerName);
        if (!damage && !dodge) continue;

        void* function = method->get_function_raw();
        if (function == nullptr || !hookedFunctions.insert(function).second) {
            continue;
        }
        const unsigned int hookId = method->add_hook(
            PrePlayerReactionEntered,
            nullptr,
            false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Player reaction hook failed: %s.detailEnter",
                ownerName.c_str());
            continue;
        }
        if (damage) ++coverage.damage;
        if (dodge) ++coverage.dodge;
    }

    // Most concrete grab/restriction damage actions inherit detailEnter from
    // cGeneralGrappleBase. Its declaring type does not contain "Damage", so
    // the normal scan above cannot discover that shared entry point. The
    // callback still checks the runtime concrete type, excluding player-
    // initiated grapples and other non-damage users of this common base.
    if (auto* type = tdb->find_type(
            "app.PlayerCommonAction.cGeneralGrappleBase")) {
        if (auto* method = type->find_method("detailEnter")) {
            void* function = method->get_function_raw();
            if (function != nullptr && hookedFunctions.insert(function).second) {
                const unsigned int hookId = method->add_hook(
                    PrePlayerReactionEntered,
                    nullptr,
                    false);
                if (hookId != 0) {
                    ++coverage.sharedGrabEntry;
                } else {
                    API::get()->log_warn(
                        "[CodexDualSenseBridge] Shared grab entry hook failed");
                }
            }
        }
    }

    // Sustain inherited and overridden grab loops. The shared base covers the
    // majority; the scan adds concrete overrides such as RestrictDamageLoop.
    auto hookGrabSustain = [&](API::Method* method) {
        if (method == nullptr) return;
        void* function = method->get_function_raw();
        if (function == nullptr ||
            !hookedSustainFunctions.insert(function).second) {
            return;
        }
        const unsigned int hookId = method->add_hook(
            PrePlayerGrabSustainUpdated,
            nullptr,
            false);
        if (hookId != 0) {
            ++coverage.grabSustain;
        } else {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Grab sustain hook failed");
        }
    };

    if (auto* type = tdb->find_type(
            "app.PlayerCommonAction.cGeneralGrappleBase")) {
        hookGrabSustain(type->find_method("detailUpdate"));
    }
    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr ||
            std::strcmp(method->get_name(), "detailUpdate") != 0) {
            continue;
        }
        auto* owner = method->get_declaring_type();
        if (owner != nullptr &&
            IsPlayerGrabDamageAction(owner->get_full_name())) {
            hookGrabSustain(method);
        }
    }
    return coverage;
}


uint32_t AddCombatSpeakerHooks() {
    auto* tdb = API::get()->tdb();
    std::unordered_set<void*> hookedFunctions;
    uint32_t count = 0;

    for (uint32_t index = 0; index < tdb->get_num_methods(); ++index) {
        auto* method = tdb->get_method(index);
        if (method == nullptr ||
            std::strcmp(method->get_name(), "detailEnter") != 0) {
            continue;
        }
        auto* owner = method->get_declaring_type();
        if (owner == nullptr) continue;
        const std::string ownerName = owner->get_full_name();
        if (!IsCombatSpeakerHookOwner(ownerName)) continue;

        void* function = method->get_function_raw();
        if (function == nullptr || !hookedFunctions.insert(function).second) {
            continue;
        }
        const unsigned int hookId = method->add_hook(
            PreCombatSpeakerEntered,
            nullptr,
            false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Combat speaker hook failed: %s.detailEnter",
                ownerName.c_str());
            continue;
        }
        ++count;
    }

    // Resolve these known runtime types explicitly as well. Function-address
    // deduplication prevents a second hook when the table scan already found
    // their implementation; do not assume that detailEnter is inherited.
    constexpr const char* ExactRepelActionTypes[] = {
        "app.PlayerOneHandedAction.cParryBlow",
        "app.PlayerOneHandedAction.cParryDefend",
        "app.PlayerOneHandedAction.cParryShell",
        "app.PlayerOneHandedDamageAction.cDamageDirectionalGuardHit",
        "app.PlayerTwoHandedDamageAction.cDamageDirectionalGuardHit"
    };
    for (const char* typeName : ExactRepelActionTypes) {
        auto* type = tdb->find_type(typeName);
        if (type == nullptr) {
            API::get()->log_info(
                "[CodexDualSenseBridge] Repel action type unavailable: %s",
                typeName);
            continue;
        }
        auto* method = type->find_method("detailEnter");
        if (method == nullptr) {
            API::get()->log_info(
                "[CodexDualSenseBridge] Repel action detailEnter unavailable: %s",
                typeName);
            continue;
        }
        void* function = method->get_function_raw();
        if (function == nullptr || !hookedFunctions.insert(function).second) {
            continue;
        }
        const unsigned int hookId = method->add_hook(
            PreCombatSpeakerEntered,
            nullptr,
            false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Repel action hook failed: %s.detailEnter",
                typeName);
            continue;
        }
        ++count;
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked exact repel action %s.detailEnter id=%u",
            typeName,
            hookId);
    }

    return count;
}

void LogRelevantMethodInventory(const char* typeName) {
    auto* type = API::get()->tdb()->find_type(typeName);
    if (type == nullptr) {
        API::get()->log_info(
            "[CodexDualSenseBridge] methodInventory type=%s unavailable",
            typeName);
        return;
    }

    for (auto* method : type->get_methods()) {
        if (method == nullptr || method->get_name() == nullptr) continue;
        const std::string methodName = method->get_name();
        if (methodName.find("Motor") == std::string::npos &&
            methodName.find("motor") == std::string::npos &&
            methodName.find("Vibrat") == std::string::npos &&
            methodName.find("vibrat") == std::string::npos) {
            continue;
        }

        std::string parameterTypes;
        const auto parameters = method->get_params();
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) parameterTypes += ",";
            if (parameters[index].t == nullptr) {
                parameterTypes += "<null>";
                continue;
            }
            auto* parameterType = reinterpret_cast<API::TypeDefinition*>(
                parameters[index].t);
            parameterTypes += parameterType->get_full_name();
        }
        API::get()->log_info(
            "[CodexDualSenseBridge] methodInventory type=%s method=%s/%u params=%s function=%p",
            typeName,
            methodName.c_str(),
            method->get_num_params(),
            parameterTypes.c_str(),
            method->get_function_raw());
    }
}

bool AddOptionalHookOverload(
    const char* typeName,
    const char* methodName,
    uint32_t parameterCount,
    uint32_t typedParameterIndex,
    const char* parameterTypeName,
    REFPreHookFn callback) {
    auto* type = API::get()->tdb()->find_type(typeName);
    if (type == nullptr) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Optional overload type missing: %s",
            typeName);
        return false;
    }

    for (auto* method : type->get_methods()) {
        if (method == nullptr || std::strcmp(method->get_name(), methodName) != 0 ||
            method->get_num_params() != parameterCount) {
            continue;
        }
        const auto parameters = method->get_params();
        if (typedParameterIndex >= parameters.size() ||
            parameters[typedParameterIndex].t == nullptr) {
            continue;
        }
        auto* parameterType = reinterpret_cast<API::TypeDefinition*>(
            parameters[typedParameterIndex].t);
        if (parameterType->get_full_name() != parameterTypeName) {
            continue;
        }

        const unsigned int hookId = method->add_hook(callback, nullptr, false);
        if (hookId == 0) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Optional overload hook failed: %s.%s",
                typeName,
                methodName);
            return false;
        }
        if (std::strcmp(typeName, "via.hid.GamePadDevice") == 0 &&
            std::strcmp(methodName, "setMotorPower") == 0) {
            g_nativeSetMotorPowerMethod = method;
            API::get()->log_info(
                "[CodexDualSenseBridge] Native GamePadDevice.setMotorPower retained for treasure chest fallback");
        }
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked %s.%s/%u id=%u",
            typeName,
            methodName,
            parameterCount,
            hookId);
        return true;
    }

    API::get()->log_warn(
        "[CodexDualSenseBridge] Optional overload missing: %s.%s/%u parameter[%u]=%s",
        typeName,
        methodName,
        parameterCount,
        typedParameterIndex,
        parameterTypeName);
    return false;
}

} // namespace

extern "C" __declspec(dllexport) void reframework_plugin_required_version(
    REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(
    const REFrameworkPluginInitializeParam* param) {
    API::initialize(param);
#ifdef ONIMUSHA_LOCAL_DEVELOPMENT
    API::get()->log_info(
        "[CodexDualSenseBridge] LOCAL DEVELOPMENT: product v2.2 / core v4.1");
#else
    API::get()->log_info(
        "[CodexDualSenseBridge] Initializing product v2.2 (core v4.1)");
#endif

    auto* managerType = API::get()->tdb()->find_type("app.AdaptiveTriggerManager");
    if (managerType == nullptr) {
        API::get()->log_error("[CodexDualSenseBridge] app.AdaptiveTriggerManager missing");
        return false;
    }

    const bool hooked =
        AddHook(managerType, "<awakeGA>b__6_0", PreBowFlagChanged) &&
        AddHook(managerType, "<awakeGA>b__6_1", PreAbsorbFlagChanged) &&
        AddHook(managerType, "resetAdaptiveTrigger", PreReset) &&
        AddHook(managerType, "destroyGA", PreReset);
    if (!hooked) {
        return false;
    }

    if (auto* hapticManagerType = API::get()->tdb()->find_type(
            "via.hid.HDVibrationComponentManager")) {
        g_hapticPausingGetter =
            hapticManagerType->find_method("get_Pausing");
    }
    g_parryArgumentField = ResolveNativeParryField(
        "app.CharacterBase", "<ArgumentData>k__BackingField", "app.cCharacterArgumentData");
    g_parryRequestActionField = ResolveNativeParryField(
        "app.CharacterBase", "<RequestActionInfo>k__BackingField", "app.cCharacterActionInfo");
    g_parryCurrentActionField = ResolveNativeParryField(
        "app.CharacterBase", "<CurrentActionInfo>k__BackingField", "app.cCharacterActionInfo");
    g_parryCategoryField = ResolveNativeParryField(
        "app.cCharacterActionInfo", "<Category>k__BackingField", "app.CharacterDef.ACTION_REQUEST_CATEGORY");
    g_parryEnemyActionField = ResolveNativeParryField(
        "app.cCharacterArgumentData", "<EnemyAction>k__BackingField",
        "app.cCharacterArgumentData.cEnemyActionArgument");
    g_parryLargeField = ResolveNativeParryField(
        "app.cCharacterArgumentData.cEnemyActionArgument",
        "<IsGrappleParryLarge>k__BackingField", "System.Boolean");
    g_cinematicParryFieldsReady = g_parryArgumentField != nullptr &&
        g_parryRequestActionField != nullptr && g_parryCurrentActionField != nullptr &&
        g_parryCategoryField != nullptr && g_parryEnemyActionField != nullptr &&
        g_parryLargeField != nullptr;
    if (auto* pauseManagerType = API::get()->tdb()->find_type(
            "app.PauseManager")) {
        g_menuPauseGetter =
            pauseManagerType->find_method("get_IsMenuPause");
    }
    if (auto* pauseManagerBaseType = API::get()->tdb()->find_type(
            "ace.PauseManagerBase")) {
        g_anyPauseGetter =
            pauseManagerBaseType->find_method("get_IsPaused");
    }
    if (auto* weatherManagerType = API::get()->tdb()->find_type(
            "app.AppWeatherManager")) {
        g_isRainyWeatherGetter =
            weatherManagerType->find_method("isRainy");
    }
    if (auto* subWeaponType = API::get()->tdb()->find_type(
            "app.cPlayerSubWeaponSupporter")) {
        g_subWeaponIdGetter = subWeaponType->find_method("getSubWeaponId");
        g_subWeaponEnableGetter = subWeaponType->find_method("isEnable");
    }
    if (auto* playerType = API::get()->tdb()->find_type(
            "app.cPlayerCharacterEntity")) {
        g_currentZoneSafeAreaGetter = playerType->find_method(
            "isCurrentZoneSafeArea");
    }
    if (auto* timerType = API::get()->tdb()->find_type("ace.cTimer")) {
        g_bowDisableTimerEnabledGetter = timerType->find_method("get_Enabled");
        g_bowDisableTimerIsTimeOutGetter = timerType->find_method("get_IsTimeOut");
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Pause polling menu=%s any=%s fallback=%s",
        g_menuPauseGetter != nullptr ? "enabled" : "unavailable",
        g_anyPauseGetter != nullptr ? "enabled" : "unavailable",
        g_hapticPausingGetter != nullptr ? "enabled" : "unavailable");
    if (auto* cutscenePlayerType = API::get()->tdb()->find_type(
            "ace.cTimelineEventPlayerBase")) {
        g_cutsceneResourceGetter = cutscenePlayerType->find_method("get_Resource");
        g_cutsceneFlowFrameGetter = cutscenePlayerType->find_method("get_FlowFrame");
    }
    const bool cutscenePlaybackHooked = AddOptionalHook(
        "app.TimelineEventPlayer",
        "updateStatusPlay",
        PreCutscenePlayingFrame);
    const bool cutsceneShakeHooked = AddOptionalHookOverload(
        "app.cCameraShakeManager",
        "addShake",
        4,
        0,
        "app.camera_shake.cOneShakeParam",
        PreCutsceneCameraShake);
    API::get()->log_info(
        "[CodexDualSenseBridge] cutscene coverage playback=%s shake=%s authored=%s cues=%zu",
        cutscenePlaybackHooked ? "enabled" : "unavailable",
        cutsceneShakeHooked ? "enabled" : "unavailable",
        g_cutsceneResourceGetter != nullptr && g_cutsceneFlowFrameGetter != nullptr
            ? "enabled" : "unavailable",
        kAuthoredCutsceneCues.size());
    API::get()->log_info(
        "[CodexDualSenseBridge] Rain polling isRainy=%s soundFallback=enabled",
        g_isRainyWeatherGetter != nullptr ? "enabled" : "unavailable");
    const bool rainVisualFadeHooked = AddOptionalHook(
        "app.AppVfxRainFadeExecutor",
        "applyRainFadeAlphaRate",
        PreRainVisualFade);
    API::get()->log_info(
        "[CodexDualSenseBridge] Rain visual fade probe=%s",
        rainVisualFadeHooked ? "enabled" : "unavailable");
    const bool playerSupporterHooked = AddOptionalHook(
        "app.cPlayerGameObjectSupporter",
        "updateInOut",
        PrePlayerSupporterUpdate);
    API::get()->log_info(
        "[CodexDualSenseBridge] Rain roof ray player tracking=%s",
        playerSupporterHooked ? "enabled" : "unavailable");
    const bool bowSubWeaponUpdateHooked = AddOptionalHook(
        "app.cPlayerSubWeaponSupporter",
        "detailUpdateCommon",
        PreBowSubWeaponUpdate);
    const bool bowDisableGateHooked = AddOptionalHook(
        "app.cPlayerActionSelector",
        "update",
        PrePlayerActionSelectorUpdateSubWeapon);
    API::get()->log_info(
        "[CodexDualSenseBridge] Bow pre-arm frame hook=%s disableGate=%s timer=%s safeArea=%s",
        bowSubWeaponUpdateHooked ? "enabled" : "unavailable",
        bowDisableGateHooked ? "enabled" : "unavailable",
        g_bowDisableTimerEnabledGetter != nullptr &&
            g_bowDisableTimerIsTimeOutGetter != nullptr
                ? "enabled" : "unavailable",
        g_currentZoneSafeAreaGetter != nullptr ? "enabled" : "unavailable");
    const uint32_t bowActionHookCount = AddBowActionHooks();
    if (bowActionHookCount == 0) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Bow action lifecycle hooks unavailable; event fallback only");
    } else {
        API::get()->log_info(
            "[CodexDualSenseBridge] Bow adaptive-trigger lifecycle coverage: %u hooks",
            bowActionHookCount);
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Bow pre-arm selection polling id=%s enable=%s",
        g_subWeaponIdGetter != nullptr ? "enabled" : "unavailable",
        g_subWeaponEnableGetter != nullptr ? "enabled" : "unavailable");

    const bool hookedTryAdvanced = AddOptionalHook(
        "ace.PadVibrationManager`1<app.cAppAdvancedVibrationBase>",
        "tryADVibration",
        PreAdvancedVibration);
    const bool hookedMotorRequest = AddOptionalHookOverload(
        "ace.PadVibrationManager`1<app.cAppAdvancedVibrationBase>",
        "addVibrationRequest",
        9,
        1,
        "ace.user_data.PadVibrationPresetList.cPadVibrationPreset",
        PreMotorVibrationRequest);
    const bool hookedAdvancedFallback = AddOptionalHook(
        "app.cAppAdvancedVibrationBase",
        "triggerVibration",
        PreAdvancedVibration);
    const bool hookedPostVibration = AddOptionalHookOverload(
        "ace.cAceAdvancedVibrationBase",
        "onPostVibrationEvent",
        3,
        0,
        "System.UInt32",
        PrePostVibrationEvent);
    const bool hookedLiveMotor = AddOptionalHookOverload(
        "via.hid.GamePadDevice",
        "setMotorPower",
        2,
        0,
        "via.hid.GamePadMotor",
        PreSetMotorPower);
    AddOptionalHookOverload(
        "via.hid.GamePadDevice",
        "getMotorPower",
        1,
        0,
        "via.hid.GamePadMotor",
        PreGetMotorPower);
    AddOptionalHook(
        "via.hid.GamePadDevice",
        "resetMotors",
        PreResetMotors);
    AddOptionalHook(
        "ace.PadVibrationManager`1<app.cAppAdvancedVibrationBase>",
        "stopAllVibration",
        PreStopHaptics);
    AddOptionalHook(
        "ace.PadVibrationManager`1<app.cAppAdvancedVibrationBase>",
        "requestPauseVibration",
        PreStopHaptics);
    AddOptionalHook(
        "ace.PadVibrationManager`1<app.cAppAdvancedVibrationBase>",
        "stopAllMotorVibration",
        PreStopMotorHaptics);
    AddOptionalHook(
        "ace.cAceAdvancedVibrationBase",
        "stopAllNormalADVibration",
        PreStopWaveforms);
    AddOptionalHook(
        "ace.cAceAdvancedVibrationBase",
        "stopAllPostVibration",
        PreStopWaveforms);
    AddOptionalHook(
        "ace.cAceAdvancedVibrationBase",
        "pauseAllVibration",
        PrePauseHaptics);
    AddOptionalHook(
        "ace.cAceAdvancedVibrationBase",
        "resumeAllVibration",
        PreResumeHaptics);
    LogRelevantMethodInventory("via.hid.GamePadDevice");
    LogRelevantMethodInventory("via.hid.GamePadManager");
    LogRelevantMethodInventory(
        "ace.PadVibrationManager`1<app.cAppAdvancedVibrationBase>");
    LogRelevantMethodInventory("app.cAppAdvancedVibrationBase");
    LogRelevantMethodInventory("ace.cAceAdvancedVibrationBase");
    if (!hookedMotorRequest && !hookedTryAdvanced && !hookedAdvancedFallback &&
        !hookedPostVibration && !hookedLiveMotor) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Waveform event hooks unavailable; adaptive triggers remain enabled");
    }

    AddOptionalHook(
        "app.SoundGetItemEventHandler",
        "onGetItem",
        PreItemObtained);
    const bool soulSpeakerHooked = AddOptionalHook(
        "app.cPlayerSoulAbsorptionSupporter",
        "executeSoulAbsorptionSuccess",
        PreSoulAbsorptionSuccess);
    const bool blackSoulHooked = AddOptionalHook(
        "app.cPlayerBlackSoulAbsorptionSupporter", "update",
        PreBlackSoulAbsorptionUpdated);
    API::get()->log_info(
        "[CodexDualSenseBridge] Black soul continuous haptics coverage=%s state=main/sub ABSORB + FINISH settle",
        blackSoulHooked ? "yes" : "no");
    const bool soulActionHooked = AddOptionalHookOverload(
        "app.cPlayerSoulAbsorptionSupporter",
        "requestSoulAbsorptionAction",
        1,
        0,
        "System.Boolean",
        PreSoulAbsorptionActionRequested);

    if (auto* voiceParamType = API::get()->tdb()->find_type(
            "app.speak.VOICE_REQUEST_PARAM")) {
        for (auto* field : voiceParamType->get_fields()) {
            if (field != nullptr &&
                std::strcmp(field->get_name(), "SpeakerId") == 0) {
                const uint32_t boxedOffset = field->get_offset_from_base();
                g_voiceSpeakerOffset = boxedOffset >= 0x10
                    ? boxedOffset - 0x10
                    : boxedOffset;
                break;
            }
        }
    }
    auto* voiceManager = API::get()->tdb()->find_type(
        "app.SoundDialogueTriggerManager");
    auto* subtitleManager = API::get()->tdb()->find_type(
        "app.cAppDialogueSubTitle");
    auto* soundContainer = API::get()->tdb()->find_type(
        "soundlib.SoundContainer");
    auto* voiceStart = FindTypedMethod(
        voiceManager,
        "requestStartVoice",
        3,
        0,
        "app.speak.VOICE_REQUEST_PARAM");
    auto* voiceStop = FindTypedMethod(
        voiceManager,
        "requestStopVoice",
        2,
        0,
        "app.speak.VOICE_REQUEST_PARAM");
    auto* voiceTrigger = FindTypedMethod(
        soundContainer,
        "trigger",
        11,
        0,
        "System.UInt32");
    auto* voiceCreate = FindTypedMethod(
        subtitleManager,
        "createVoiceParam",
        3,
        0,
        "via.gui.DialogueTalkData");
    const bool voiceStartHooked = voiceStart != nullptr &&
        voiceStart->add_hook(
            PreVoiceRequestStart, PostVoiceRequestStart, false) != 0;
    const bool voiceStopHooked = voiceStop != nullptr &&
        voiceStop->add_hook(PreVoiceRequestStop, nullptr, false) != 0;
    const bool voiceTriggerHooked = voiceTrigger != nullptr &&
        voiceTrigger->add_hook(PreDialogueTriggerFull, nullptr, false) != 0;
    const bool voiceCreateHooked = voiceCreate != nullptr &&
        voiceCreate->add_hook(PreCreateVoiceParam, nullptr, false) != 0;
    API::get()->log_info(
        "[CodexDualSenseBridge] Controller speaker coverage soulLifecycle=%s soulCollect=%s gauntletVoice=%s start=%s stop=%s create=%s speakerOffset=0x%X",
        soulActionHooked ? "yes" : "adaptive-event-only",
        soulSpeakerHooked ? "yes" : "no",
        voiceTriggerHooked ? "yes" : "no",
        voiceStartHooked ? "yes" : "no",
        voiceStopHooked ? "yes" : "no",
        voiceCreateHooked ? "yes" : "no",
        g_voiceSpeakerOffset);
    const uint32_t useItemHookCount = AddUseItemActionHooks();
    if (useItemHookCount == 0) {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Use-item action hooks unavailable");
    } else {
        API::get()->log_info(
            "[CodexDualSenseBridge] Use-item action coverage: %u hooks",
            useItemHookCount);
    }
    const PlayerReactionHookCoverage reactionCoverage =
        AddPlayerReactionHooks();
    API::get()->log_info(
        "[CodexDualSenseBridge] Player reaction coverage damage=%u dodge=%u sharedGrabEntry=%u grabSustain=%u",
        reactionCoverage.damage,
        reactionCoverage.dodge,
        reactionCoverage.sharedGrabEntry,
        reactionCoverage.grabSustain);
    const uint32_t combatSpeakerHookCount = AddCombatSpeakerHooks();
      auto* repelSoundMethod = FindTypedMethod(
        API::get()->tdb()->find_type("app.snd_user_data.SoundHitSwitchListBehavior"),
         "setGrapple", 5, 0, "app.GrappleTableDef.GRAPPLE_TYPE_Fixed");
    if (repelSoundMethod != nullptr && g_cinematicParryFieldsReady) {
        const auto parameters = repelSoundMethod->get_params();
        auto* partnerType = parameters.size() == 5
            ? reinterpret_cast<API::TypeDefinition*>(parameters[1].t) : nullptr;
        g_cinematicParrySchemaReady = partnerType != nullptr &&
            partnerType->get_full_name() == "app.CharacterBase";
    }
    const unsigned int repelSoundHookId = repelSoundMethod != nullptr
        ? repelSoundMethod->add_hook(PreNativeRepelSound, PostNativeRepelSound, false) : 0;
    g_cinematicParrySchemaReady = g_cinematicParrySchemaReady && repelSoundHookId != 0;
    API::get()->log_info(
        "[CodexDualSenseBridge] Cinematic Parry argument state=%s fields=%s partnerArg=3 source=EnemyAction.IsGrappleParryLarge (read-only)",
        g_cinematicParrySchemaReady ? "enabled" : "unavailable",
        g_cinematicParryFieldsReady ? "enabled" : "unavailable");
    auto* projectileRepelSoundMethod = FindTypedMethod(
        API::get()->tdb()->find_type("app.snd_user_data.SoundHitSwitchListBehavior"),
        "setShellParry", 3, 0, "System.UInt32");
    const unsigned int projectileRepelSoundHookId = projectileRepelSoundMethod != nullptr
        ? projectileRepelSoundMethod->add_hook(
            PreNativeProjectileRepelSound, PostNativeProjectileRepelSound, false) : 0;
    // This is the character-attack check, not tryShellJustGuard or the optional
    // tryBlowJustGuard branch. Disassembly confirms a successful overwrite can
    // return true here even when tryBlowJustGuard did not handle the attack.
    auto* meleeRepelMethod = FindTypedMethod(
        API::get()->tdb()->find_type("app.cCharacterJustGuardSupporter"),
        "tryJustGuard", 1, 0, "app.cCharacterJustGuardSupporter.cInfo");
    const unsigned int meleeRepelHookId = meleeRepelMethod != nullptr
        ? meleeRepelMethod->add_hook(PreMeleeRepelResult, PostMeleeRepelResult, false) : 0;
    API::get()->log_info(
        "[CodexDualSenseBridge] Combat speaker coverage actionHooks=%u nativeRepelSoundHook=%u meleeRepelResultHook=%u projectileRepelSoundHook=%u diagnosticSoundHooks=disabled",
        combatSpeakerHookCount, repelSoundHookId, meleeRepelHookId, projectileRepelSoundHookId);
    const bool footstepHooked = AddOptionalHook(
        "app.EPVExpertFootLandingCustom",
        "play",
        PrePlayerFootLanding);
    API::get()->log_info(
        "[CodexDualSenseBridge] Synthetic environment coverage footstep=%s rain=yes wind=yes",
        footstepHooked ? "yes" : "no");

    const bool titleEnter = AddOptionalHook("app.TitleState", "enter", PreTitleEnter);
    const bool titleLeave = AddOptionalHook("app.TitleState", "leave", PreTitleLeave);
    const bool titleOpen = AddOptionalHook("app.GUI010000", "onOpen", PreTitleOpen);
    const bool titleStart = AddOptionalHook("app.GUI010000.cGUITitleMenu", "startMenu", PreTitleStartMenu);
    const bool titlePoll = AddOptionalHook("app.GUI010000", "guiVisibleUpdate", PreTitleVisible);
    const bool titleDecision = AddOptionalHook("app.GUI010000.cGUITitleMenu", "callback_ListDecide", PreTitleDecision);
    AddOptionalHook("app.TitleState", "onTitleMenuSelected", PreTitleDecision);
    API::get()->log_info("[CodexDualSenseBridge] Title polling=%d directDecision=%d", titlePoll, titleDecision);
    API::get()->log_info("[CodexDualSenseBridge] Title haptics enter=%d leave=%d open=%d start=%d", titleEnter, titleLeave, titleOpen, titleStart);
    constexpr const char* MenuInputType =
        "ace.cGUIInputCtrl`2<app.GUIID.ID,app.UIKey.TYPE>";
    AddOptionalPostHook(MenuInputType, "funcDecideEvent", PostMenuDecision);
    AddOptionalPostHook(MenuInputType, "funcCancelEvent", PostMenuCancel);
    bool longPressGaugeHooked = false;
    if (auto* longPressType = API::get()->tdb()->find_type(
            "app.cGUIGameKeyIconTypeManager.IconTypeInfo")) {
        if (auto* timerGauge = longPressType->find_method(
                "updateOnTimerGauge")) {
            longPressGaugeHooked = timerGauge->add_hook(
                PreLongPressTimerGauge,
                PostLongPressTimerGauge,
                false) != 0;
        }
    }
    API::get()->log_info(
        "[CodexDualSenseBridge] Long-press haptics coverage timerGauge=%s",
        longPressGaugeHooked ? "yes" : "no");
    const bool equipmentEnhanceVisibleHooked = AddOptionalHook(
        "app.GUI030102",
        "guiVisibleUpdate",
        PreEquipmentEnhanceVisible);
    const bool equipmentEnhanceStatusHooked = AddOptionalHook(
        "app.GUI030102",
        "updateEnhanceStatus",
        PreEquipmentEnhanceVisible);
    bool equipmentEnhanceGaugeHooked = false;
    if (auto* enhanceDetailType = API::get()->tdb()->find_type(
            "app.GUI030102.cDetailWindow")) {
        if (auto* enhanceGauge = enhanceDetailType->find_method(
                "getLongPressGaugeWidthRate")) {
            equipmentEnhanceGaugeHooked = enhanceGauge->add_hook(
                PreEquipmentEnhanceGauge,
                PostEquipmentEnhanceGauge,
                false) != 0;
        }
    }
    const bool equipmentEnhanceCompleteHooked = AddOptionalHook(
        "app.GUI030102",
        "doEnhance",
        PreEquipmentEnhanceComplete);
    API::get()->log_info(
        "[CodexDualSenseBridge] Equipment enhancement long-press coverage visible=%s status=%s gauge=%s complete=%s",
        equipmentEnhanceVisibleHooked ? "yes" : "no",
        equipmentEnhanceStatusHooked ? "yes" : "no",
        equipmentEnhanceGaugeHooked ? "yes" : "no",
        equipmentEnhanceCompleteHooked ? "yes" : "no");
    const bool oniStrengthHooked = AddOptionalPostHook(
        "app.cPlayerActionSelector",
        "selectBlowAttackAction",
        PostOniStrengthSelected);
    const bool oniStrengthBarrierHooked = AddOptionalHook(
        "app.Gm025",
        "interactEvent",
        PreOniStrengthBarrierInteraction);
    API::get()->log_info(
        "[CodexDualSenseBridge] Oni Strength coverage destroy=%s maliceBarrier=%s longPressProgress=%s",
        oniStrengthHooked ? "yes" : "no",
        oniStrengthBarrierHooked ? "yes" : "no",
        longPressGaugeHooked ? "yes" : "no");
    const bool oniEyeHooked = AddOptionalHook(
        "app.cPlayerOniEyeActivationSupporter",
        "nortifyStartActivation",
        PreOniEyeActivation);
    bool oniTransformationHooked = false;
    bool oniTransformationEndHooked = false;
    if (auto* type = API::get()->tdb()->find_type("app.cPlayerSoulBoostSupporter")) {
        if (auto* method = type->find_method("startSoulBoostEffect")) {
            oniTransformationHooked = method->add_hook(
                PreOniTransformationStart, PostOniTransformationStart, false) != 0;
        }
        if (auto* method = type->find_method("endSoulBoostEffect")) {
            oniTransformationEndHooked = method->add_hook(
                PreSoulBoostEffectEnd, nullptr, false) != 0;
        }
    }
    AddWallRunHapticHooks();
    AddLadderHapticHooks();
    const uint32_t normalAttackHooked = AddNormalAttackHapticHook();
    API::get()->log_info("[CodexDualSenseBridge] Attack haptics hooks=%u base/damage/charge paths; ordinary charge, fighting spirit and Oni",
        normalAttackHooked);
    API::get()->log_info(
        "[CodexDualSenseBridge] Oni Eye sonar haptics activation=%s",
        oniEyeHooked ? "yes" : "no");
    API::get()->log_info(
        "[CodexDualSenseBridge] Oni transformation haptics activation=%s end-state=%s",
        oniTransformationHooked ? "yes" : "no",
        oniTransformationEndHooked ? "yes" : "no");
    constexpr const char* MenuSelectionTypes[] = {
        "ace.cGUIInputCtrl_SimpleList`2<app.GUIID.ID,app.UIKey.TYPE>",
        "ace.cGUIInputCtrl_ScrollList`2<app.GUIID.ID,app.UIKey.TYPE>",
        "ace.cGUIInputCtrl_ScrollGrid`2<app.GUIID.ID,app.UIKey.TYPE>",
        "ace.cGUIInputCtrl_ItemsControlLink`2<app.GUIID.ID,app.UIKey.TYPE>",
        "ace.cGUIInputCtrl_NumberSelection`2<app.GUIID.ID,app.UIKey.TYPE>",
        "ace.cGUIInputCtrl_FluentScrollList`2<app.GUIID.ID,app.UIKey.TYPE>",
        "ace.cGUIInputCtrl_FluentScrollGrid`2<app.GUIID.ID,app.UIKey.TYPE>",
        "ace.cGUIInputCtrl_FluentItemsControlLink`2<app.GUIID.ID,app.UIKey.TYPE>"
    };
    uint32_t menuSelectionHookCount = 0;
    for (const char* typeName : MenuSelectionTypes) {
        if (AddOptionalHook(typeName, "ctrlSelectionChanged", PreMenuSelection)) {
            ++menuSelectionHookCount;
        }
    }
    const bool shortcutWheelHooked = AddOptionalHook(
        "app.GUI020212",
        "guiVisibleUpdate",
        PreShortcutWheelUpdate);
    const bool worldMapHooked = AddOptionalHook(
        "app.GUI060000",
        "guiVisibleUpdate",
        PreWorldMapUpdate);
    const bool mirrorMapHooked = AddOptionalHook(
        "app.GUI030100.cMap",
        "mapDispUpdate",
        PreMirrorMapUpdate);
    const bool freePinSetHooked = AddOptionalHook(
        "app.GUI060000.cFreePin",
        "updatePin",
        PreMapFreePinUpdated);
    const bool freePinClearHooked = AddOptionalHook(
        "app.GUI060000.cFreePin",
        "clearPin",
        PreMapFreePinCleared);
    const bool targetPinHooked = AddOptionalHook(
        "app.GUI060000.cIconBase",
        "playTarget",
        PreMapTargetPlayed);
    const uint32_t interactionHookCount = AddInteractionEntryHooks();
    const uint32_t gimmickSuccessHookCount =
        AddGimmickInteractionSuccessHooks();
    const uint32_t largeTreasureChestHookCount =
        AddLargeTreasureChestHapticHooks();
    const uint32_t leverHookCount = AddLeverChangeHooks();
    const bool doorClassifierHooked = AddOptionalHook(
        "app.GimmickDoor",
        "requestOpen",
        PreDoorOpenRequested);
    const bool elevatorStartHooked = AddOptionalHook(
        "app.GimmickElevator",
        "changeStateEvent_Move",
        PreElevatorMoveStarted);
    const bool elevatorMoveHooked = AddOptionalHook(
        "app.GimmickElevator",
        "updateMoveState",
        PreElevatorMoveUpdated);
    const bool elevatorStopHooked = AddOptionalHook(
        "app.GimmickElevator",
        "changeStateEvent_Stop",
        PreElevatorMoveStopped);
    API::get()->log_info(
        "[CodexDualSenseBridge] UI/item coverage menuSelectionHooks=%u shortcutWheel=%s interactionHooks=%u",
        menuSelectionHookCount,
        shortcutWheelHooked ? "yes" : "no",
        interactionHookCount);
    API::get()->log_info(
        "[CodexDualSenseBridge] Environment interaction coverage success=%u doorCue=timed doorClassifier=%s lever=%u elevatorStart=%s elevatorMove=%s elevatorStop=%s",
        gimmickSuccessHookCount,
        doorClassifierHooked ? "yes" : "no",
        leverHookCount,
        elevatorStartHooked ? "yes" : "no",
        elevatorMoveHooked ? "yes" : "no",
        elevatorStopHooked ? "yes" : "no");
    API::get()->log_info(
        "[CodexDualSenseBridge] Large treasure chest coverage hooks=%u target=app.Gm002_000/app.Gm002_010 stages=open/push/lid-land revision=chest-output-r1",
        largeTreasureChestHookCount);
    API::get()->log_info(
        "[CodexDualSenseBridge] Map coverage world=%s mirror=%s freePinSet=%s freePinClear=%s targetPin=%s",
        worldMapHooked ? "yes" : "no",
        mirrorMapHooked ? "yes" : "no",
        freePinSetHooked ? "yes" : "no",
        freePinClearHooked ? "yes" : "no",
        targetPinHooked ? "yes" : "no");

    if (HookXInputSetStateImport()) {
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked executable XInputSetState import");
    } else {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Executable XInputSetState import hook unavailable");
    }
    if (HookXInputGetStateImport()) {
        API::get()->log_info(
            "[CodexDualSenseBridge] Hooked executable XInputGetState import for L3 sprint edge");
    } else {
        API::get()->log_warn(
            "[CodexDualSenseBridge] Executable XInputGetState import hook unavailable; L3 sprint accent disabled");
    }

    param->functions->on_lua_state_destroyed(OnLuaStateDestroyed);
    param->functions->on_present(OnPresent);
    {
        std::lock_guard lock{g_mutex};
        if (!StartHelperLocked()) {
            API::get()->log_warn(
                "[CodexDualSenseBridge] Helper unavailable at startup; will retry on an event");
        }
    }
    API::get()->log_info("[CodexDualSenseBridge] Ready");
    return true;
}
