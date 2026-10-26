#include "PopulationBridge.h"
#include "AccountMgr.h"
#include "Config.h"
#include "Cryptography/CryptoHash.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GameEventMgr.h"
#include "PoolMgr.h"
#include "Guide/PythonPopulationBridge.h"
#include "Guide/CourtesyBuffs.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "AiObjectContext.h"
#include "TravelMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "Item.h"
#include "Bag.h"
#include "Creature.h"
#include "ConditionMgr.h"
#include "DisableMgr.h"
#include "AiFactory.h"
#include "RandomItemMgr.h"
#include "StatsWeightCalculator.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "WorldSession.h"
#include "World.h"
#include "Playerbots.h"
#include "ProgressionSystem.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "Utilities/Util.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <locale>
#include <limits>
#include <tuple>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace PyEng::Population;

namespace
{
bool Hex64(std::string const& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character)
    {
        return std::isdigit(character) || (character >= 'a' && character <= 'f');
    });
}

bool AcquisitionHash64(std::string const& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character)
    {
        return std::isxdigit(character);
    });
}

bool AccountName(std::string const& value)
{
    return !value.empty() && value.size() <= 17 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character)
           {
               return std::isupper(character) || std::isdigit(character) || character == '_';
           });
}

bool WaveKey(std::string const& value)
{
    return !value.empty() && value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character)
           {
               return std::isalnum(character) || character == '-' || character == ':';
    });
}

struct ManagedProgressionPolicy
{
    uint8 ceiling{0};
    bool held{true};
    uint32 refreshedAt{0};
    bool schedulerEligible{false};
    std::string phaseConfigDigest;
    bool admitted{false};
    uint32 accountId{0};
};

std::mutex ProgressionMutex;
std::map<uint32, ManagedProgressionPolicy> ManagedPolicies;
std::unordered_map<std::string, uint32> AdmittedProgressionIdentities;
constexpr uint32 ProgressionPolicyLeaseMilliseconds = 5000;
std::atomic<uint64> StrategicGeneration{1};
std::atomic<int8> OwnerLedgerAvailability{-1};
// Value-owned journal shared by map-worker observations and the admitted world tick. Reset/logout retains evidence.
std::recursive_mutex AcquisitionMutex;
struct ActiveAcquisitionRecord
{
    uint64 recordId{0}, admittedDispatchSequence{0};
    std::unordered_set<uint64> observedDeathDispatches;
};
std::unordered_map<uint32, ActiveAcquisitionRecord> AcquisitionActiveRecords;
struct AcquisitionCaptureOwner
{
    std::string identityKey;
    QuestAcquisitionCaptureRequest binding;
    uint64 recordId{0}; // One finite accepted window. ACK never rearms this owner.
};
std::unordered_map<uint32, AcquisitionCaptureOwner> AcquisitionCaptureOwners;
bool SameCaptureOwner(AcquisitionCaptureOwner const& owner, std::string const& identity, QuestAcquisitionCaptureRequest const& value)
{
    auto const& old = owner.binding;
    return owner.identityKey == identity && old.captureId == value.captureId && old.accountId == value.accountId &&
        old.characterGuid == value.characterGuid && old.sessionIncarnation == value.sessionIncarnation &&
        old.strategicGeneration == value.strategicGeneration && old.phaseDigest == value.phaseDigest &&
        old.questId == value.questId && old.rewardKind == value.rewardKind && old.rewardIndex == value.rewardIndex &&
        old.itemEntry == value.itemEntry && old.rewardCount == value.rewardCount;
}
struct AcquisitionBinding
{
    std::string identityKey, rewardKind, phaseDigest;
    uint32 accountId{0}, questId{0}, rewardIndex{0}, itemEntry{0}, rewardCount{0};
    uint64 characterGuid{0}, sessionIncarnation{0}, goalRevision{0}, strategicGeneration{0};
};
std::map<uint64, QuestAcquisitionAttempt> AcquisitionAttempts;
struct PendingAcquisitionDelivery
{
    uint64 recordId{0};
    uint32 questId{0}, rewardIndex{0}, itemEntry{0}, count{0};
    bool choice{false};
};
std::map<uint64, PendingAcquisitionDelivery> PendingAcquisitionDeliveries;
uint64 AcquisitionRecordSequence = 0;
uint64 AcquisitionEventSequence = 0;
uint64 AcquisitionTicks()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void AppendAcquisitionEvent(QuestAcquisitionAttempt& attempt, std::string_view kind,
    uint32 map, WorldPosition const& target = WorldPosition(), bool outcome = true,
    uint64 itemGuid = 0, uint32 itemEntry = 0, uint32 count = 0, std::string_view reason = "", uint64 operationId = 0, std::string_view measurementScope = "action_window")
{
    std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
    QuestAcquisitionEvent event;
    event.eventSequence = ++AcquisitionEventSequence; event.monotonicTicks = AcquisitionTicks();
    event.eventKind = kind; event.reason = reason; event.mapId = map;
    event.operationId = operationId; event.measurementScope = measurementScope;
    event.targetMap = target.GetMapId(); event.targetX = target.GetPositionX();
    event.targetY = target.GetPositionY(); event.targetZ = target.GetPositionZ();
    event.outcome = outcome; event.itemGuid = itemGuid; event.itemEntry = itemEntry; event.count = count;
    attempt.events.push_back(std::move(event));
}
std::string Sha256Hex(std::string const& value);
std::string AcquisitionRecordDigest(QuestAcquisitionAttempt const& value)
{
    std::ostringstream bytes;
    bytes.imbue(std::locale::classic());
    auto field = [&](auto const& input)
    {
        std::ostringstream token; token.imbue(std::locale::classic()); token << std::hexfloat << input;
        auto text = token.str(); bytes << text.size() << ':' << text;
    };
    field("core1-native-acquisition-record-digest-v2");
    field(value.recordId);
    field(value.identityKey);
    field(value.accountId);
    field(value.characterGuid);
    field(value.sessionIncarnation);
    field(value.strategicGeneration);
    field(value.goalRevision);
    field(value.phaseDigest);
    field(value.sourceKey);
    field(value.questId);
    field(value.rewardKind);
    field(value.rewardIndex);
    field(value.itemEntry);
    field(value.rewardCount);
    field(value.monotonicFrequency);
    field(value.validWindow);
    field(value.terminal);
    field(value.validAttempt);
    field(value.actionTerminal);
    field(value.pendingDeliveries);
    field(value.terminalOutcome);
    field(value.movementCoverage);
    field(value.deathCoverage);
    field(value.routeCoverage);
    field(value.failureTerminalCoverage);
    field(value.postActionContextLost);
    field(value.completeDestinationAttribution);
    field(value.turnInFlight);
    field(value.completeQuestObserved);
    field(value.moving);
    field(value.observerFailure);
    field(value.events.size());
    for (auto const& row : value.events)
    {
        field(row.eventSequence);
        field(row.monotonicTicks);
        field(row.eventKind);
        field(row.reason);
        field(row.mapId);
        field(row.targetMap);
        field(row.targetX);
        field(row.targetY);
        field(row.targetZ);
        field(row.itemGuid);
        field(row.itemEntry);
        field(row.count);
        field(row.outcome);
        field(row.operationId);
        field(row.measurementScope);
    }
    field(value.predicates.size());
    for (auto const& row : value.predicates)
    {
        field(row.name);
        field(row.scope);
        field(row.reason);
        field(row.eventSequence);
        field(row.monotonicTicks);
        field(row.evaluated);
        field(row.result);
    }
    field(value.rewardReceipts.size());
    for (auto const& row : value.rewardReceipts)
    {
        field(row.eventSequence);
        field(row.monotonicTicks);
        field(row.countSum);
        field(row.questId);
        field(row.rewardIndex);
        field(row.itemEntry);
        field(row.count);
        field(row.rewardKind);
        field(row.deliveryReason);
        field(row.complete);
        field(row.recordId);
        field(row.operationId);
        field(row.deliveryPending);
        field(row.deliveryKnown);
        field(row.transactionCommitted);
        field(row.queuedComplete);
        field(row.destinations.size());
        for (auto const& destination : row.destinations)
        {
            field(destination.bagSlot);
            field(destination.plannedCount);
            field(destination.beforeEntry);
            field(destination.itemEntry);
            field(destination.beforeCount);
            field(destination.afterCount);
            field(destination.delta);
            field(destination.beforeGuid);
            field(destination.itemGuid);
            field(destination.matched);
        }
        field(row.mailEnvelopes.size());
        for (auto const& envelope : row.mailEnvelopes)
        {
            field(envelope.mailId); field(envelope.receiverGuid); field(envelope.deliverTime); field(envelope.queued);
            field(envelope.attachments.size());
            for (auto const& item : envelope.attachments)
            {
                field(item.itemGuid); field(item.itemEntry); field(item.count);
                field(item.savedEntry); field(item.savedCount); field(item.savedValueMatched);
            }
        }
    }
    return Sha256Hex(bytes.str());
}
AcquisitionRewardReceipt CopyAcquisitionRewardReceipt(NativeQuestRewardObservation const& value, uint64 recordId)
{
    AcquisitionRewardReceipt receipt;
    receipt.recordId = recordId; receipt.operationId = value.operationId;
    receipt.questId = value.questId; receipt.rewardKind = value.rewardKind; receipt.rewardIndex = value.rewardIndex;
    receipt.itemEntry = value.itemEntry; receipt.count = value.count; receipt.countSum = value.countSum;
    receipt.complete = value.complete; receipt.deliveryReason = value.deliveryReason;
    receipt.deliveryPending = value.deliveryPending; receipt.deliveryKnown = value.deliveryKnown;
    receipt.transactionCommitted = value.transactionCommitted; receipt.queuedComplete = value.queuedComplete;
    for (auto const& destination : value.destinations)
        receipt.destinations.push_back({destination.bagSlot, destination.plannedCount, destination.beforeEntry,
            destination.itemEntry, destination.beforeCount, destination.afterCount, destination.delta,
            destination.beforeGuid, destination.itemGuid, destination.matched});
    for (auto const& envelope : value.mailEnvelopes)
    {
        AcquisitionMailEnvelope mail;
        mail.mailId = envelope.mailId; mail.receiverGuid = envelope.receiverGuid; mail.deliverTime = envelope.deliverTime; mail.queued = envelope.queued;
        for (auto const& item : envelope.attachments)
            mail.attachments.push_back({item.itemGuid, item.itemEntry, item.count, item.savedEntry, item.savedCount, item.savedValueMatched});
        receipt.mailEnvelopes.push_back(std::move(mail));
    }
    return receipt;
}

void MarkAcquisitionObserverFailure(QuestAcquisitionAttempt& attempt) noexcept
{
    if (attempt.terminal) return; // A finalized original cannot be changed by a later recorder's observation failure.
    auto active = AcquisitionActiveRecords.find(uint32(attempt.characterGuid));
    if (active != AcquisitionActiveRecords.end() && active->second.recordId == attempt.recordId) AcquisitionActiveRecords.erase(active);
    attempt.observerFailure = true; attempt.validWindow = false; attempt.validAttempt = false;
    attempt.movementCoverage = false; attempt.routeCoverage = false; attempt.deathCoverage = false;
    attempt.failureTerminalCoverage = false;
    attempt.actionTerminal = true; attempt.terminal = attempt.pendingDeliveries == 0; attempt.turnInFlight = false;
}
void MarkAcquisitionObserverFailure(uint64 recordId) noexcept
{
    try
    {
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto found = AcquisitionAttempts.find(recordId);
        if (found != AcquisitionAttempts.end()) MarkAcquisitionObserverFailure(found->second);
    }
    catch (...) { } // An observation failure must never escape into an ordinary native action.
}

void InvalidateAcquisition(QuestAcquisitionAttempt& attempt, std::string_view reason) noexcept
{
    try
    {
        if (attempt.terminal) return;
        if (attempt.actionTerminal)
        {
            if (!attempt.postActionContextLost)
            {
                AppendAcquisitionEvent(attempt, "invalidated", 0, {}, false, 0, 0, 0, reason, 0, "delivery_after_action");
                attempt.postActionContextLost = true;
            }
            return; // The fully witnessed action window is immutable; only its delivery journal remains open.
        }
        if (attempt.moving) AppendAcquisitionEvent(attempt, "move_end", 0, {}, false, 0, 0, 0, reason);
        bool const afterAction = attempt.actionTerminal;
        auto active = AcquisitionActiveRecords.find(uint32(attempt.characterGuid));
        if (active != AcquisitionActiveRecords.end() && active->second.recordId == attempt.recordId) AcquisitionActiveRecords.erase(active);
        attempt.moving = false; attempt.validWindow = false; attempt.validAttempt = false; attempt.actionTerminal = true;
        attempt.failureTerminalCoverage = false;
        attempt.terminalOutcome = reason; attempt.turnInFlight = false;
        AppendAcquisitionEvent(attempt, "invalidated", 0, {}, false, 0, 0, 0, reason, 0, afterAction ? "delivery_after_action" : "action_window");
        attempt.terminal = attempt.pendingDeliveries == 0;
    }
    catch (...) { MarkAcquisitionObserverFailure(attempt); }
}


std::string PopulationRealmId()
{
    return std::to_string(sConfigMgr->GetOption<uint32>("RealmID", 1));
}

std::string Sha256Hex(std::string const& value)
{
    auto const digest = Acore::Crypto::SHA256::GetDigestOf(value);
    return Acore::Impl::ByteArrayToHexStr(digest.data(), digest.size());
}

uint64 NativeProcessId()
{
#ifdef _WIN32
    return static_cast<uint64>(_getpid());
#else
    return static_cast<uint64>(getpid());
#endif
}

uint64 MonotonicNanoseconds()
{
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool HasV2OwnerLedger()
{
    int8 cached = OwnerLedgerAvailability.load(std::memory_order_acquire);
    if (cached >= 0)
        return cached == 1;

    QueryResult tables = PlayerbotsDatabase.Query(
        "SELECT COUNT(DISTINCT table_name) FROM information_schema.tables "
        "WHERE table_schema = DATABASE() AND table_name IN "
        "('population_director_schedule', 'population_director_owner_state', "
        "'population_director_owner_binding_audit', 'population_director_identity')");
    QueryResult ownerColumns = PlayerbotsDatabase.Query(
        "SELECT COUNT(*) FROM information_schema.columns "
        "WHERE table_schema = DATABASE() AND table_name = 'population_director_owner_state' "
        "AND column_name IN ('realm_id', 'binding_state', 'owner_account_id', "
        "'owner_character_guid', 'highest_owner_level_seen', 'current_epoch', "
        "'generation', 'last_reconciled_at')");
    bool available = tables && ownerColumns && tables->Fetch()[0].Get<uint64>() == 4 &&
        ownerColumns->Fetch()[0].Get<uint64>() == 8;
    OwnerLedgerAvailability.store(available ? 1 : 0, std::memory_order_release);
    return available;
}

struct ResolvedOwner
{
    uint8 level{0};
    std::string name;
    std::string reason;
};

ResolvedOwner ResolveOwner(uint32 accountId, uint32 characterGuid)
{
    ResolvedOwner result;
    if (!accountId || !characterGuid)
    {
        result.reason = "owner_identity_missing";
        return result;
    }

    QueryResult account = LoginDatabase.Query("SELECT id FROM account WHERE id = {}", accountId);
    if (!account)
    {
        result.reason = "owner_account_not_found";
        return result;
    }

    QueryResult character = CharacterDatabase.Query(
        "SELECT account, level, name FROM characters WHERE guid = {}", characterGuid);
    if (!character)
    {
        result.reason = "owner_character_not_found";
        return result;
    }
    Field* fields = character->Fetch();
    if (fields[0].Get<uint32>() != accountId)
    {
        result.reason = "owner_character_account_mismatch";
        return result;
    }

    QueryResult autonomousAccount = PlayerbotsDatabase.Query(
        "SELECT account_type FROM playerbots_account_type "
        "WHERE account_id = {} AND account_type IN (1, 2) LIMIT 1", accountId);
    if (autonomousAccount)
    {
        result.reason = "owner_account_is_autonomous";
        return result;
    }

    QueryResult legacyAccount = PlayerbotsDatabase.Query(
        "SELECT COUNT(*) FROM playerbots_population_account WHERE account_id = {}", accountId);
    if (!legacyAccount)
    {
        result.reason = "population_account_ledger_unavailable";
        return result;
    }
    if (legacyAccount->Fetch()[0].Get<uint64>() != 0)
    {
        result.reason = "owner_overlaps_population_account";
        return result;
    }

    QueryResult legacyIdentity = PlayerbotsDatabase.Query(
        "SELECT COUNT(*) FROM playerbots_population_identity WHERE character_guid = {}", characterGuid);
    if (!legacyIdentity)
    {
        result.reason = "population_identity_ledger_unavailable";
        return result;
    }
    if (legacyIdentity->Fetch()[0].Get<uint64>() != 0)
    {
        result.reason = "owner_overlaps_population_identity";
        return result;
    }

    QueryResult managedIdentity = PlayerbotsDatabase.Query(
        "SELECT COUNT(*) FROM population_director_identity WHERE realm_id = '{}' "
        "AND (account_id = {} OR character_guid = {})",
        PopulationRealmId(), accountId, characterGuid);
    if (!managedIdentity)
    {
        result.reason = "population_identity_ledger_unavailable";
        return result;
    }
    if (managedIdentity->Fetch()[0].Get<uint64>() != 0)
    {
        result.reason = "owner_overlaps_population_identity";
        return result;
    }

    Player* connected = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(characterGuid));
    if (connected && (!connected->GetSession() || connected->GetSession()->IsBot() ||
        (GET_PLAYERBOT_AI(connected) && !IsSelfBot(connected)) ||
        connected->GetSession()->GetAccountId() != accountId ||
        sRandomPlayerbotMgr.IsRandomBot(connected)))
    {
        result.reason = "owner_session_is_not_human";
        return result;
    }

    result.level = fields[1].Get<uint8>();
    result.name = fields[2].Get<std::string>();
    if (connected)
        result.level = std::max(result.level, connected->GetLevel());
    if (!result.level || result.level > 80)
        result.reason = "owner_level_out_of_range";
    return result;
}

void AdvanceOwnerHighWater(PlayerbotsPopulation::OwnerBindingState const& state, uint8 level)
{
    if (!state.bound || level <= state.highWater || level > state.maximumOwnerLevel || level > 80)
        return;

    uint8 effectiveEpoch = PlayerbotsPopulation::CappedOwnerEpoch(
        level, state.maximumOwnerLevel, state.globalPhaseCap);
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    transaction->Append(
        "UPDATE population_director_owner_state SET highest_owner_level_seen = {}, "
        "current_epoch = {}, generation = generation + 1, last_reconciled_at = UTC_TIMESTAMP(6) "
        "WHERE realm_id = '{}' AND binding_state = 'bound' AND owner_account_id = {} "
        "AND owner_character_guid = {} AND generation = {} AND highest_owner_level_seen < {}",
        level, effectiveEpoch, PopulationRealmId(), state.accountId, state.characterGuid, state.generation, level);
    PlayerbotsDatabase.DirectCommitTransaction(transaction);
}

void LoadManagedProgressionInventory()
{
    QueryResult table = PlayerbotsDatabase.Query(
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() "
        "AND table_name = 'playerbots_population_identity'");
    if (!table)
        throw std::runtime_error("Cannot determine managed progression ledger availability");
    if (table->Fetch()[0].Get<uint64>() == 0)
    {
        if (sConfigMgr->GetOption<bool>("Python.Population.Enabled", false))
            throw std::runtime_error("Population capability requires its durable identity ledger");
        return;
    }
    QueryResult count = PlayerbotsDatabase.Query(
        "SELECT COUNT(*) FROM playerbots_population_identity WHERE character_guid IS NOT NULL");
    if (!count)
        throw std::runtime_error("Cannot initialize durable managed progression inventory");
    std::map<uint32, ManagedProgressionPolicy> inventory;
    std::unordered_map<std::string, uint32> admitted;
    uint64 expected = count->Fetch()[0].Get<uint64>();
    if (expected)
    {
        QueryResult rows = PlayerbotsDatabase.Query(
            "SELECT character_guid, identity_key, admitted, account_id FROM playerbots_population_identity "
            "WHERE character_guid IS NOT NULL");
        if (!rows)
            throw std::runtime_error("Cannot read durable managed progression inventory");
        do
        {
            Field* fields = rows->Fetch();
            uint32 guid = fields[0].Get<uint32>();
            ManagedProgressionPolicy policy;
            policy.accountId = fields[3].IsNull() ? 0 : fields[3].Get<uint32>();
            policy.admitted = fields[2].Get<uint8>() == 1 && policy.accountId != 0;
            inventory.emplace(guid, std::move(policy));
            if (fields[2].Get<uint8>() == 1)
                admitted.emplace(fields[1].Get<std::string>(), guid);
        } while (rows->NextRow());
        if (inventory.size() != expected)
            throw std::runtime_error("Managed progression inventory changed during initialization");
    }
    std::lock_guard<std::mutex> lock(ProgressionMutex);
    ManagedPolicies = std::move(inventory);
    AdmittedProgressionIdentities = std::move(admitted);
}

bool ValidRewardPreference(Quest const* quest, std::string const& kind, uint32 index, uint32 item)
{
    if (!quest) return false;
    if (kind.empty()) return index == 0 && item == 0;
    if (!item) return false;
    if (kind == "choice")
        return index < QUEST_REWARD_CHOICES_COUNT && quest->RewardChoiceItemId[index] == item;
    if (kind == "guaranteed")
        return index < QUEST_REWARDS_COUNT && quest->RewardItemId[index] == item;
    return false;
}

bool ManagedCanProgress(Player* player);
bool CourtesyPolicyFresh(ManagedProgressionPolicy const& policy, ProgressionSystem::SharedPhaseSnapshot const& phase);

std::string StrategicControlReason(Player* player)
{
    if (!player || !player->IsInWorld()) return "offline";
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai || !sRandomPlayerbotMgr.IsRandomBot(player)) return "not_autonomous_session";
    WorldSession* session = player->GetSession();
    if (!session) return "missing_session";
    if (session->isLogingOut()) return "logging_out";
    if (player->IsBeingTeleported()) return "teleporting";
    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context) return "travel_context_unavailable";
    auto* travelValue = context->GetValue<TravelTarget*>("travel target");
    if (!travelValue) return "travel_value_unavailable";
    TravelTarget* travelTarget = travelValue->Get();
    if (!travelTarget) return "travel_target_unavailable";
    if (travelTarget->isForced()) return "forced_travel";
    if (ai->HasGameClientMaster() || player->GetGroup()) return "human_or_group_owned";
    if (!player->IsAlive()) return "dead";
    if (player->IsInCombat()) return "combat";
    if (player->GetMap()->IsDungeon() || player->InBattleground() || player->IsInFlight())
        return "unsupported_activity";
    if (!ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)) return "new_rpg_disabled";
    if (!ManagedCanProgress(player)) return "progression_held";
    return {};
}

std::string StrategicQuestStatus(Player* player, uint32 questId)
{
    if (!questId) return "none";
    if (player->IsQuestRewarded(questId)) return "rewarded";
    switch (player->GetQuestStatus(questId))
    {
        case QUEST_STATUS_INCOMPLETE: return "incomplete";
        case QUEST_STATUS_COMPLETE: return "complete";
        case QUEST_STATUS_FAILED: return "failed";
        default: return "none";
    }
}

std::string StrategicQuestReason(Player* player, uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest) return "unknown_quest";
    std::string status = StrategicQuestStatus(player, questId);
    if (status != "incomplete" && status != "complete") return "not_active_quest";
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (ai->lowPriorityQuest.contains(questId)) return "native_failed_quest";
    auto const* pois = sObjectMgr->GetQuestPOIVector(questId);
    if (!pois || pois->empty()) return "no_quest_poi";
    return {};
}

// Copied route evidence only; no destination, point, AI or session handle crosses a tick.
std::string FindInactiveQuestGiver(Player* player, uint32 questId, int32& entry, WorldPosition& position)
{
    if (!player || !player->GetSession()) return "offline";
    ObjectGuid guid = player->GetGUID();
    uint64 session = player->GetSession()->GetPartySessionIncarnation();
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    uint64 generation = StrategicGeneration.load();
    auto current = [&]()
    {
        return ObjectAccessor::FindPlayer(guid) == player && player->GetSession() &&
            player->GetSession()->GetPartySessionIncarnation() == session && GET_PLAYERBOT_AI(player) == ai &&
            StrategicGeneration.load() == generation && sObjectMgr->GetQuestTemplate(questId) == quest &&
            StrategicControlReason(player).empty();
    };
    if (!quest || !ai || !current()) return "acquire_actor_changed";
    if (StrategicQuestStatus(player, questId) != "none") return "not_inactive_quest";
    if (ai->lowPriorityQuest.contains(questId)) return "native_failed_quest";
    auto const* pois = sObjectMgr->GetQuestPOIVector(questId);
    if (!pois || pois->empty()) return "no_quest_poi";
    bool take = player->CanTakeQuest(quest, false);
    if (!current()) return "acquire_actor_changed";
    ai->rpgInfo.ObserveQuestAcquisition(player, "CanTakeQuest", {}, take);
    if (!take) return "cannot_take_quest";
    bool add = player->CanAddQuest(quest, false);
    if (!current()) return "acquire_actor_changed";
    ai->rpgInfo.ObserveQuestAcquisition(player, "CanAddQuest", {}, add);
    if (!add) return "cannot_add_quest";
    struct Giver { int32 entry; WorldPosition pos; };
    std::vector<Giver> candidates;
    auto found = sTravelMgr.quests.find(questId);
    if (found == sTravelMgr.quests.end() || !found->second) return "no_quest_giver_route";
    // Inspect only this quest's existing indexed routes, never the global giver catalog.
    for (auto* destination : found->second->questGivers)
    {
        auto* relation = dynamic_cast<QuestRelationTravelDestination*>(destination);
        if (!relation || relation->getRelation() != 0 || relation->GetQuestTemplate() != quest) continue;
        bool full = relation->isFull();
        ai->rpgInfo.ObserveQuestAcquisition(player, "route_decision", {}, !full, 0, 0, 0, "indexed_giver_fullness");
        if (full) continue;
        auto points = relation->getPoints();
        if (points.empty() || !points.front()) continue; // isActive itself reads front().
        for (auto* point : points)
        {
            if (!point || point->GetMapId() != player->GetMapId()) continue;
            if (entry && (entry != relation->getEntry() || position != *point)) continue;
            candidates.push_back({relation->getEntry(), *point});
        }
    }
    for (auto const& candidate : candidates)
    {
        // Re-resolve immediately before the script-capable native availability check.
        auto lookup = sTravelMgr.quests.find(questId);
        if (lookup == sTravelMgr.quests.end() || !lookup->second) return "no_quest_giver_route";
        bool active = false;
        for (auto* destination : lookup->second->questGivers)
        {
            auto* relation = dynamic_cast<QuestRelationTravelDestination*>(destination);
            if (!relation || relation->getRelation() != 0 || relation->getEntry() != candidate.entry ||
                relation->GetQuestTemplate() != quest || relation->isFull()) continue;
            auto points = relation->getPoints();
            if (points.empty() || !points.front() || std::none_of(points.begin(), points.end(), [&](WorldPosition* point)
                { return point && *point == candidate.pos; })) continue;
            active = relation->isActive(player);
            break; // No native route pointer is touched after the callback.
        }
        if (!current()) return "acquire_actor_changed";
        ai->rpgInfo.ObserveQuestAcquisition(player, "route_decision", candidate.pos, active, 0, 0, 0, "indexed_giver_eligibility");
        if (!active) continue;
        entry = candidate.entry;
        position = candidate.pos;
        return {};
    }
    return "no_active_quest_giver_route";
}

bool ManagedCanProgress(Player* player)
{
    if (!player)
        return true;
    std::lock_guard<std::mutex> lock(ProgressionMutex);
    auto found = ManagedPolicies.find(player->GetGUID().GetCounter());
    if (found == ManagedPolicies.end())
        return true;
    ManagedProgressionPolicy const& policy = found->second;
    return !policy.held && policy.ceiling &&
        GetMSTimeDiffToNow(policy.refreshedAt) <= ProgressionPolicyLeaseMilliseconds &&
        player->GetLevel() < policy.ceiling;
}

// BEGIN NATIVE GEAR OBSERVATION
GearItem CopyGearItem(Item const* item)
{
    GearItem result;
    if (item)
    {
        result.entry = item->GetEntry();
        result.instanceGuid = item->GetGUID().GetRawValue();
        result.count = item->GetCount();
        result.randomPropertyId = item->GetItemRandomPropertyId();
        result.randomPropertyKnown = true;
    }
    return result;
}

std::string GearRequirementReason(InventoryResult result)
{
    switch (result)
    {
        case EQUIP_ERR_OK: return {};
        case EQUIP_ERR_ITEM_NOT_FOUND: return "item_not_found";
        case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM: return "class_race_or_faction";
        case EQUIP_ERR_NO_REQUIRED_PROFICIENCY: return "required_skill_or_spell";
        case EQUIP_ERR_CANT_EQUIP_SKILL: return "required_skill_rank";
        case EQUIP_ERR_CANT_EQUIP_LEVEL_I: return "required_level";
        case EQUIP_ERR_CANT_DO_RIGHT_NOW: return "holiday_inactive";
        default: return "native_requirement_error";
    }
}

void ReadGearScore(StatsWeightCalculator& calculator, GearItem const& item, uint8 slot,
                   bool& valid, float& score, std::string& reason)
{
    if (!item.entry)
    {
        reason = "empty_slot";
        return;
    }
    if (!sObjectMgr->GetItemTemplate(item.entry))
    {
        reason = "item_not_found";
        return;
    }
    float value = calculator.CalculateItem(item.entry, item.randomPropertyId, slot);
    if (!std::isfinite(value))
    {
        reason = "non_finite_native_score";
        return;
    }
    score = value;
    valid = true;
}

GearReward ObserveGearReward(Player* player, StatsWeightCalculator& calculator, std::string const& kind, uint8 index, uint32 entry, uint32 count,
    std::vector<GearEquipment> const* equipment = nullptr)
{
    GearReward reward;
    reward.kind = kind;
    reward.index = index;
    reward.item.entry = entry;
    reward.item.count = count;
    // Rewards have no earned instance or rolled random property yet.
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    InventoryResult requirements = player->CanUseItemRequirements(proto);
    reward.requirementResult = static_cast<uint32>(requirements);
    reward.requirementReason = GearRequirementReason(requirements);
    if (!proto)
        reward.reason = "item_not_found";
    else if (!count)
        reward.reason = "invalid_reward_count";
    else
    {
        reward.classificationAllowed = proto->Class == ITEM_CLASS_WEAPON ?
            sRandomItemMgr.CanEquipWeapon(proto, player->getClass()) :
            proto->Class == ITEM_CLASS_ARMOR ?
            sRandomItemMgr.CanEquipArmor(proto, player->getClass(), player->GetLevel()) : false;
        if (!reward.classificationAllowed)
            reward.classificationReason = "native_item_usage_classification_rejected";
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (player->FindEquipSlotBase(proto, slot, true) != slot)
                continue;
            GearSlotComparison comparison;
            comparison.slot = slot;
            comparison.current = equipment ? (*equipment)[slot - EQUIPMENT_SLOT_START].item :
                CopyGearItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
            ReadGearScore(calculator, reward.item, slot, comparison.candidateScoreValid,
                          comparison.candidateScore, comparison.candidateScoreReason);
            ReadGearScore(calculator, comparison.current, slot, comparison.currentScoreValid,
                          comparison.currentScore, comparison.currentScoreReason);
            reward.slots.push_back(std::move(comparison));
        }
        if (reward.slots.empty())
            reward.reason = "no_compatible_equipment_slot";
    }
    return reward;
}

void PopulateGearObservation(Player* player, GearSnapshot& result, StrategicSnapshot const& snapshot)
{
    ObjectGuid actorGuid = player->GetGUID();
    uint64 session = player->GetSession()->GetPartySessionIncarnation();
    PlayerbotAI* actorAI = GET_PLAYERBOT_AI(player);
    auto current = [&]()
    {
        return ObjectAccessor::FindPlayer(actorGuid) == player && player->GetSession() &&
            player->GetSession()->GetPartySessionIncarnation() == session && GET_PLAYERBOT_AI(player) == actorAI;
    };
    result.classId = player->getClass();
    result.specTab = AiFactory::GetPlayerSpecTab(player);
    result.role = PlayerbotAI::IsHeal(player) ? "heal" : PlayerbotAI::IsCaster(player) ? "caster" :
        PlayerbotAI::IsTank(player) ? "tank" : PlayerbotAI::IsMelee(player) ? "melee" : "ranged";
    result.mapId = player->GetMapId();
    result.instanceId = player->GetInstanceId();
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        result.equipment.push_back({slot, CopyGearItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))});

    StatsWeightCalculator calculator(player);
    calculator.SetItemSetBonus(false);
    calculator.SetOverflowPenalty(false);
    // Existing manager lookup may populate its PvP event cache; it does not change the character.
    calculator.SetPvpSpec(sRandomPlayerbotMgr.IsSpecPvp(player->GetGUID().GetCounter(), player->getClass()));
    std::vector<StrategicQuest> quests = snapshot.quests;
    if (snapshot.observedQuestId && std::none_of(quests.begin(), quests.end(), [&](StrategicQuest const& quest)
        { return quest.questId == snapshot.observedQuestId; }))
        quests.push_back({snapshot.observedQuestId, snapshot.observedQuestStatus, false, "not_in_quest_log"});
    for (StrategicQuest const& observed : quests)
    {
        GearQuest row;
        row.questId = observed.questId;
        row.status = observed.status;
        row.reason = observed.rejectionReason;
        Quest const* quest = sObjectMgr->GetQuestTemplate(row.questId);
        if (row.status == "none" && row.questId == snapshot.observedQuestId)
        {
            WorldPosition giver;
            row.reason = FindInactiveQuestGiver(player, row.questId, row.giverEntry, giver);
            if (row.reason.empty())
            {
                row.giverMap = giver.GetMapId();
                row.giverX = giver.GetPositionX();
                row.giverY = giver.GetPositionY();
                row.giverZ = giver.GetPositionZ();
            }
        }
        if (!current())
        {
            result.controlAllowed = false;
            result.reason = "acquire_actor_changed";
            return;
        }
        if (quest != sObjectMgr->GetQuestTemplate(row.questId)) row.reason = "quest_template_changed";
        if (!quest)
            row.reason = "unknown_quest";
        if (!row.reason.empty())
        {
            result.quests.push_back(std::move(row));
            continue;
        }
        auto append = [&](std::string const& kind, uint8 index, uint32 entry, uint32 count)
        {
            if (!entry && !count)
                return;
            GearReward reward = ObserveGearReward(player, calculator, kind, index, entry, count, &result.equipment);
            row.rewards.push_back(std::move(reward));
        };
        for (uint8 index = 0; index < QUEST_REWARDS_COUNT; ++index)
            append("fixed", index, quest->RewardItemId[index], quest->RewardItemIdCount[index]);
        for (uint8 index = 0; index < QUEST_REWARD_CHOICES_COUNT; ++index)
            append("choice", index, quest->RewardChoiceItemId[index], quest->RewardChoiceItemCount[index]);
        result.quests.push_back(std::move(row));
    }
}
// END NATIVE GEAR OBSERVATION

// Resolve the ORIGINAL dispatch entry by copied binding before touching a potentially stale callback pointer.
Player* ResolveAcquisitionCallbackActor(Player* expected, uint32 hookId) noexcept
{
    try
    {
        auto context = sScriptMgr->ReadQuestLifecycleContext();
        if (!context.active || !context.dispatchSequence || context.hookId != hookId || !context.actorGuid || !context.accountId || !context.sessionIncarnation) return nullptr;
        uint64 recordId = 0;
        {
            std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
            auto active = AcquisitionActiveRecords.find(uint32(context.actorGuid));
            if (active == AcquisitionActiveRecords.end() || active->second.admittedDispatchSequence >= context.dispatchSequence) return nullptr;
            auto found = AcquisitionAttempts.find(active->second.recordId);
            if (found == AcquisitionAttempts.end() || found->second.actionTerminal || found->second.terminal ||
                found->second.characterGuid != context.actorGuid || found->second.accountId != context.accountId ||
                found->second.sessionIncarnation != context.sessionIncarnation) return nullptr;
            recordId = found->second.recordId;
        }
        auto* live = ObjectAccessor::FindPlayerByLowGUID(context.actorGuid);
        if (!live || live != expected || live->GetGUID().GetRawValue() != context.actorGuid || !live->GetSession() ||
            live->GetSession()->GetAccountId() != context.accountId || live->GetSession()->GetPartySessionIncarnation() != context.sessionIncarnation) return nullptr;
        auto* ai = GET_PLAYERBOT_AI(live);
        return ai && ai->rpgInfo.pythonAcquisitionRecord == recordId ? live : nullptr;
    }
    catch (...) { return nullptr; }
}

class managed_population_progression : public PlayerScript
{
public:
    managed_population_progression() : PlayerScript("managed_population_progression") { }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!ManagedCanProgress(player))
            amount = 0;
    }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_UPDATE);
        if (!player) return;
        if (auto* ai = GET_PLAYERBOT_AI(player); ai && ai->rpgInfo.pythonAcquisitionRecord)
        {
            if (player->IsInFlight() || player->IsBeingTeleported() || player->GetTransport())
            {
                ai->rpgInfo.ObserveQuestAcquisition(player, "unmeasured_travel", WorldPosition(player), false, 0, 0, 0,
                    "passive_transport_flight_or_teleport_not_in_movement_sample_scope");
                return;
            }
            ai->rpgInfo.ObserveQuestAcquisition(player, "movement_sample", WorldPosition(player), player->isMoving());
            auto const* goal = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
            if (goal && player->GetQuestStatus(goal->questId) == QUEST_STATUS_FAILED)
                ai->rpgInfo.ObserveQuestAcquisition(player, "quest_failed", {}, false, 0, 0, 0, "native_quest_status_failed");
        }
    }
    bool OnPlayerBeforeTeleport(Player* player, uint32 /*mapId*/, float /*x*/, float /*y*/, float /*z*/, float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_BEFORE_TELEPORT);
        if (player) if (auto* ai = GET_PLAYERBOT_AI(player))
            ai->rpgInfo.ObserveQuestAcquisition(player, "unmeasured_travel", WorldPosition(player), false, 0, 0, 0,
                "native_teleport_attempt_outside_sampled_walking_scope");
        return true; // Passive observation never changes the ordinary teleport decision.
    }
    void OnPlayerMapChanged(Player* player) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_MAP_CHANGED);
        if (!player) return;
        if (auto* ai = GET_PLAYERBOT_AI(player))
            ai->rpgInfo.ObserveQuestAcquisition(player, "unmeasured_travel", WorldPosition(player), false, 0, 0, 0,
                "native_map_transition_not_in_movement_sample_scope");
    }
    void OnPlayerJustDied(Player* player) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_PLAYER_JUST_DIED);
        if (!player) return;
        if (auto* ai = GET_PLAYERBOT_AI(player)) ai->rpgInfo.ObserveQuestAcquisition(player, "death");
    }
    void OnPlayerLogout(Player* player) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_LOGOUT);
        if (!player) return;
        if (auto* ai = GET_PLAYERBOT_AI(player))
        {
            ai->rpgInfo.CloseQuestAcquisition("logout");
        }
    }
    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_QUEST_ABANDON);
        if (!player) return;
        if (auto* ai = GET_PLAYERBOT_AI(player))
        {
            auto const* goal = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
            auto context = sScriptMgr->ReadQuestLifecycleContext();
            if (goal && goal->questId == questId && goal->questId == context.questId)
                ai->rpgInfo.ObserveQuestAcquisition(player, "quest_abandoned");
        }
    }
    bool ShouldObserveQuestReward(uint64 actorGuid, uint32 accountId, uint64 session, uint32 questId) override
    {
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto owner = AcquisitionCaptureOwners.find(uint32(actorGuid));
        if (owner == AcquisitionCaptureOwners.end() || !owner->second.recordId) return false;
        auto found = AcquisitionAttempts.find(owner->second.recordId);
        auto active = AcquisitionActiveRecords.find(uint32(actorGuid));
        return found != AcquisitionAttempts.end() && active != AcquisitionActiveRecords.end() && active->second.recordId == found->first &&
            !found->second.actionTerminal && !found->second.terminal && found->second.validWindow && found->second.turnInFlight &&
            found->second.characterGuid == actorGuid && found->second.accountId == accountId &&
            found->second.sessionIncarnation == session && found->second.questId == questId &&
            owner->second.binding.characterGuid == actorGuid && owner->second.binding.accountId == accountId &&
            owner->second.binding.sessionIncarnation == session && owner->second.binding.questId == questId &&
            owner->second.binding.strategicGeneration == StrategicGeneration.load() &&
            owner->second.binding.rewardKind == found->second.rewardKind && owner->second.binding.rewardIndex == found->second.rewardIndex &&
            owner->second.binding.itemEntry == found->second.itemEntry && owner->second.binding.rewardCount == found->second.rewardCount;
    }

    void OnPlayerQuestRewardDestinations(NativeQuestRewardObservation const& value) override
    {
        // Late transaction outcomes resolve only their exact original operation; they never rebind a live actor.
        {
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto pending = value.operationId ? PendingAcquisitionDeliveries.find(value.operationId) : PendingAcquisitionDeliveries.end();
        if (value.operationId && !value.deliveryPending && pending != PendingAcquisitionDeliveries.end())
        {
            auto found = AcquisitionAttempts.find(pending->second.recordId);
            if (found == AcquisitionAttempts.end()) return; // ACK cannot remove pending deliveries.
            auto& attempt = found->second;
            auto const link = pending->second;
            bool matching = attempt.accountId == value.accountId && attempt.characterGuid == value.actorGuid &&
                attempt.sessionIncarnation == value.sessionIncarnation && link.questId == value.questId &&
                link.rewardIndex == value.rewardIndex && link.itemEntry == value.itemEntry && link.count == value.count &&
                value.rewardKind == (link.choice ? "choice" : "guaranteed");
            try
            {
                auto receipt = CopyAcquisitionRewardReceipt(value, attempt.recordId);
                if (!matching) { receipt.complete = false; receipt.deliveryKnown = false; receipt.deliveryReason = "mail_delivery_original_link_mismatch"; }
                auto kind = !matching || !value.deliveryKnown ? "mail_delivery_unknown" : value.transactionCommitted ? "mail_delivery_committed" : "mail_delivery_failed";
                AppendAcquisitionEvent(attempt, kind, 0, {}, matching && value.complete, 0, value.itemEntry, value.count,
                    receipt.deliveryReason, value.operationId, attempt.actionTerminal ? "delivery_after_action" : "action_window");
                receipt.eventSequence = attempt.events.back().eventSequence; receipt.monotonicTicks = attempt.events.back().monotonicTicks;
                attempt.rewardReceipts.push_back(std::move(receipt));
                if (attempt.rewardKind == value.rewardKind && attempt.rewardIndex == value.rewardIndex && attempt.itemEntry == value.itemEntry)
                    attempt.completeDestinationAttribution = matching && value.complete && attempt.rewardCount == value.count;
                if (!matching) { attempt.validWindow = false; attempt.validAttempt = false; }
            }
            catch (...) { MarkAcquisitionObserverFailure(attempt); }
            if (attempt.pendingDeliveries) --attempt.pendingDeliveries;
            PendingAcquisitionDeliveries.erase(pending);
            attempt.terminal = attempt.actionTerminal && attempt.pendingDeliveries == 0;
            return;
        }
        }
        auto* player = ObjectAccessor::FindPlayerByLowGUID(value.actorGuid);
        if (!player || !player->GetSession() || player->GetGUID().GetRawValue() != value.actorGuid ||
            player->GetSession()->GetPartySessionIncarnation() != value.sessionIncarnation ||
            player->GetSession()->GetAccountId() != value.accountId) return;
        auto* ai = GET_PLAYERBOT_AI(player);
        if (!ai) return;
        uint64 const recordId = ai->rpgInfo.pythonAcquisitionRecord;
        {
            std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
            auto found = AcquisitionAttempts.find(recordId);
            if (found == AcquisitionAttempts.end() || found->second.terminal || found->second.actionTerminal ||
                !found->second.turnInFlight || found->second.questId != value.questId) return;
        }
        ai->rpgInfo.ObserveQuestAcquisition(player, "reward_destinations", {}, value.complete, 0, value.itemEntry, value.count, value.deliveryReason);
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto found = AcquisitionAttempts.find(recordId);
        if (found == AcquisitionAttempts.end() || found->second.terminal || !found->second.turnInFlight ||
            found->second.actionTerminal || found->second.questId != value.questId) return;
        auto& attempt = found->second;
        try
        {
            if (!attempt.validWindow || !attempt.turnInFlight || attempt.actionTerminal) return;
            auto receipt = CopyAcquisitionRewardReceipt(value, attempt.recordId);
            if (value.operationId && value.deliveryPending)
            {
                auto inserted = PendingAcquisitionDeliveries.emplace(value.operationId, PendingAcquisitionDelivery{
                    attempt.recordId, value.questId, value.rewardIndex, value.itemEntry, value.count, value.rewardKind == "choice"});
                if (!inserted.second) { InvalidateAcquisition(attempt, "duplicate_mail_operation"); return; }
                ++attempt.pendingDeliveries;
                AppendAcquisitionEvent(attempt, "mail_delivery_pending", player->GetMapId(), {}, false, 0, value.itemEntry, value.count,
                    value.deliveryReason, value.operationId);
            }
            else if (value.operationId)
                AppendAcquisitionEvent(attempt, "mail_delivery_unknown", player->GetMapId(), {}, false, 0, value.itemEntry, value.count,
                    value.deliveryReason, value.operationId);
            receipt.eventSequence = attempt.events.back().eventSequence; receipt.monotonicTicks = attempt.events.back().monotonicTicks;
            attempt.rewardReceipts.push_back(std::move(receipt));
            if (attempt.rewardKind == value.rewardKind && attempt.rewardIndex == value.rewardIndex && attempt.itemEntry == value.itemEntry)
                attempt.completeDestinationAttribution = value.complete && attempt.rewardCount == value.count;
        }
        catch (...) { MarkAcquisitionObserverFailure(attempt); }
    }

    void OnPlayerQuestRewardItem(Player* player, Item* item, uint32 count) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_QUEST_REWARD_ITEM);
        if (!player) return;
        if (!item || !player) return;
        // Earlier listeners may destroy/move the legacy raw Item*: compare against live carried storage before dereferencing.
        Item* live = nullptr;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END && !live; ++slot)
            if (auto* candidate = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); candidate == item) live = candidate;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END && !live; ++bagSlot)
            if (auto* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize() && !live; ++slot)
                    if (auto* candidate = bag->GetItemByPos(slot); candidate == item) live = candidate;
        if (auto* ai = GET_PLAYERBOT_AI(player))
        {
            if (live) ai->rpgInfo.ObserveQuestAcquisition(player, "item_stored", {}, true,
                live->GetGUID().GetRawValue(), live->GetEntry(), count);
            else ai->rpgInfo.ObserveQuestAcquisition(player, "item_hook_instance_unavailable", {}, false, 0, 0, count);
        }
    }
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        player = ResolveAcquisitionCallbackActor(player, PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST);
        if (!player) return;
        if (auto* ai = GET_PLAYERBOT_AI(player))
        {
            auto const* goal = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
            auto context = sScriptMgr->ReadQuestLifecycleContext();
            if (goal && goal->questId == context.questId && quest && sObjectMgr->GetQuestTemplate(goal->questId) == quest)
                ai->rpgInfo.ObserveQuestAcquisition(player, "quest_terminal");
        }
    }

    bool OnPlayerCanGiveLevel(Player* player, uint8 /*newLevel*/) override
    {
        return ManagedCanProgress(player);
    }
};

class PlayerbotsPopulationBridge final : public Bridge
{
    uint64 _providerConstructedMonotonicNs{0};
    std::string _processIncarnation;
    std::unordered_map<std::string, std::vector<GearVendorOffer>> _vendorQuotes;
    std::unordered_map<std::string, uint64> _vendorQuoteRevision;
    std::unordered_map<std::string, std::unordered_set<std::string>> _vendorConsumed;
    std::unordered_set<std::string> _vendorInFlight;
    std::unordered_set<std::string> _vendorAmbiguous;
public:
    PlayerbotsPopulationBridge()
        : _providerConstructedMonotonicNs(MonotonicNanoseconds()),
          _processIncarnation("pid:" + std::to_string(NativeProcessId()) +
              ":provider_constructed_monotonic_ns:" + std::to_string(_providerConstructedMonotonicNs))
    {
    }

    StrategicSnapshot ReadQuestSnapshot(std::string const& identityKey,
                                        uint32 observedQuestId) override
    {
        return ReadQuestSnapshotImpl(identityKey, observedQuestId, true);
    }

    StrategicSnapshot ReadQuestSnapshotImpl(std::string const& identityKey,
                                          uint32 observedQuestId, bool refreshDurable)
    {
        StrategicSnapshot result;
        result.identityKey = identityKey;
        result.observedQuestId = observedQuestId;
        if (!Hex64(identityKey))
        {
            result.reason = "invalid_identity";
            return result;
        }
        {
            std::lock_guard<std::mutex> lock(ProgressionMutex);
            auto found = AdmittedProgressionIdentities.find(identityKey);
            if (found == AdmittedProgressionIdentities.end())
            {
                result.reason = "not_admitted";
                return result;
            }
            result.characterGuid = found->second;
            if (!refreshDurable)
            {
                auto policy = ManagedPolicies.find(result.characterGuid);
                if (policy == ManagedPolicies.end() || !policy->second.admitted || !policy->second.accountId)
                {
                    result.reason = "admission_cache_unavailable";
                    return result;
                }
                result.accountId = policy->second.accountId;
            }
        }
        if (refreshDurable)
        {
            QueryResult binding = PlayerbotsDatabase.Query(
                "SELECT account_id, character_guid FROM playerbots_population_identity "
                "WHERE identity_key = '{}' AND admitted = 1", identityKey);
            if (!binding || binding->Fetch()[0].IsNull() || binding->Fetch()[1].IsNull() ||
                binding->Fetch()[1].Get<uint32>() != result.characterGuid ||
                !binding->Fetch()[0].Get<uint32>())
                throw std::runtime_error("strategic identity binding unavailable or inconsistent");
            result.accountId = binding->Fetch()[0].Get<uint32>();
        }
        Player* player = sRandomPlayerbotMgr.GetPlayerBot(result.characterGuid);
        if (!player)
        {
            result.reason = "offline";
            return result;
        }
        if (!player->GetSession() || player->GetSession()->GetAccountId() != result.accountId)
            throw std::runtime_error("strategic session account differs from durable binding");
        result.online = player->IsInWorld();
        result.level = player->GetLevel();
        result.xp = player->GetUInt32Value(PLAYER_XP);
        result.reason = StrategicControlReason(player);
        result.controlAllowed = result.reason.empty();
        result.observedQuestStatus = StrategicQuestStatus(player, observedQuestId);
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (ai)
        {
            if (auto const* goal = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
            {
                result.currentGoalQuestId = goal->questId;
                result.currentGoalAcquiring = ai->rpgInfo.pythonQuestGoal && goal->acquiring &&
                    goal->identityKey == identityKey && ai->rpgInfo.pythonGoalAccountId == result.accountId &&
                    ai->rpgInfo.pythonGoalSession == player->GetSession()->GetPartySessionIncarnation() &&
                    ai->rpgInfo.pythonGoalGeneration == StrategicGeneration.load() &&
                    GetMSTimeDiffToNow(ai->rpgInfo.pythonGoalRefreshedAt) <= ProgressionPolicyLeaseMilliseconds;
            }
        }
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = player->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            StrategicQuest row;
            row.questId = questId;
            row.status = StrategicQuestStatus(player, questId);
            row.rejectionReason = result.controlAllowed ? StrategicQuestReason(player, questId) : result.reason;
            row.eligible = row.rejectionReason.empty();
            result.quests.push_back(std::move(row));
        }
        return result;
    }

    GearSnapshot ReadGearSnapshot(std::string const& identityKey, uint32 observedQuestId) override
    {
        StrategicSnapshot snapshot = ReadQuestSnapshotImpl(identityKey, observedQuestId, false);
        GearSnapshot result;
        result.identityKey = snapshot.identityKey;
        result.accountId = snapshot.accountId;
        result.characterGuid = snapshot.characterGuid;
        result.online = snapshot.online;
        result.controlAllowed = snapshot.controlAllowed;
        result.reason = snapshot.reason;
        result.level = snapshot.level;
        if (!result.accountId || !result.characterGuid)
            return result;
        Player* player = sRandomPlayerbotMgr.GetPlayerBot(result.characterGuid);
        if (!player || !player->IsInWorld())
        {
            result.online = false;
            result.controlAllowed = false;
            result.reason = "offline";
            return result;
        }
        if (!player->GetSession() || player->GetGUID().GetRawValue() != result.characterGuid ||
            player->GetSession()->GetAccountId() != result.accountId || player->GetSession()->isLogingOut() ||
            !player->GetSession()->GetPartySessionIncarnation())
            throw std::runtime_error("strategic session differs from admitted durable actor");
        result.sessionIncarnation = player->GetSession()->GetPartySessionIncarnation();
        result.strategicGeneration = StrategicGeneration.load();
        if (!result.controlAllowed)
            return result;
        // Synchronous world-thread observation: no callbacks, yields or retained native pointers.
        PopulateGearObservation(player, result, snapshot);
        return result;
    }

    QuestAcquisitionCaptureResult SetQuestAcquisitionCapture(std::string const& identityKey,
        QuestAcquisitionCaptureRequest const& request) override
    {
        QuestAcquisitionCaptureResult result;
        result.captureId = request.captureId; result.sessionIncarnation = request.sessionIncarnation;
        result.strategicGeneration = request.strategicGeneration; // Echo ORIGINAL request, including recovery/disarm.
        if (!Hex64(identityKey) || request.captureId.empty() || !request.accountId || !request.characterGuid ||
            !request.sessionIncarnation || !request.strategicGeneration || !AcquisitionHash64(request.phaseDigest) ||
            !request.questId || !request.itemEntry || !request.rewardCount ||
            (request.rewardKind != "guaranteed" && request.rewardKind != "choice"))
            { result.reason = "invalid_capture_binding"; return result; }
        auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
        bool admitted = false;
        {
            std::lock_guard<std::mutex> policyLock(ProgressionMutex);
            auto identity = AdmittedProgressionIdentities.find(identityKey);
            auto policy = ManagedPolicies.find(uint32(request.characterGuid));
            admitted = identity != AdmittedProgressionIdentities.end() && identity->second == request.characterGuid &&
                policy != ManagedPolicies.end() && policy->second.admitted && policy->second.accountId == request.accountId &&
                (!request.enabled || (!policy->second.held && CourtesyPolicyFresh(policy->second, phase)));
        }
        auto* player = admitted ? sRandomPlayerbotMgr.GetPlayerBot(request.characterGuid) : nullptr;
        if (!player || !player->IsInWorld() || !player->GetSession() || player->GetGUID().GetRawValue() != request.characterGuid ||
            player->GetSession()->GetAccountId() != request.accountId || player->GetSession()->isLogingOut() ||
            !player->GetSession()->GetPartySessionIncarnation())
            { result.reason = "current_capture_owner_not_authenticated"; return result; }
        uint64 const currentSession = player->GetSession()->GetPartySessionIncarnation();
        uint64 const currentGeneration = StrategicGeneration.load();
        Quest const* quest = sObjectMgr->GetQuestTemplate(request.questId);
        if (request.enabled && (currentSession != request.sessionIncarnation || currentGeneration != request.strategicGeneration ||
            phase.policyConfigDigest != request.phaseDigest))
            { result.reason = "capture_session_generation_or_phase_changed"; return result; }
        if (request.enabled && (!ValidRewardPreference(quest, request.rewardKind, request.rewardIndex, request.itemEntry) ||
            request.rewardCount != (request.rewardKind == "choice" ? quest->RewardChoiceItemCount[request.rewardIndex] : quest->RewardItemIdCount[request.rewardIndex])))
            { result.reason = "capture_reward_template_mismatch"; return result; }
        std::unique_lock<std::recursive_mutex> journal(AcquisitionMutex);
        auto found = AcquisitionCaptureOwners.find(uint32(request.characterGuid));
        // A stale live registration cannot block a new current session/phase/generation. Originals remain recoverable.
        if (request.enabled && found != AcquisitionCaptureOwners.end() &&
            (found->second.binding.sessionIncarnation != currentSession || found->second.binding.strategicGeneration != currentGeneration ||
             found->second.binding.phaseDigest != phase.policyConfigDigest || found->second.binding.accountId != request.accountId))
        {
            auto old = AcquisitionAttempts.find(found->second.recordId);
            if (old != AcquisitionAttempts.end()) InvalidateAcquisition(old->second, "capture_owner_context_lost");
            AcquisitionCaptureOwners.erase(found); found = AcquisitionCaptureOwners.end();
        }
        if (found != AcquisitionCaptureOwners.end() && !SameCaptureOwner(found->second, identityKey, request))
            { result.reason = "capture_owner_conflict"; return result; }
        if (!request.enabled)
        {
            if (found == AcquisitionCaptureOwners.end())
                { result.status = "absent"; result.reason = "no_current_capture_owner"; return result; }
            result.recordId = found->second.recordId;
            // Preserve any genuine enclosing death witness before censoring the active original.
            // Close validates current actor/policy outside the journal lock, never journal -> ProgressionMutex.
            auto* ai = GET_PLAYERBOT_AI(player);
            if (result.recordId && ai && ai->rpgInfo.pythonAcquisitionRecord == result.recordId)
            {
                journal.unlock();
                ai->rpgInfo.CloseQuestAcquisition("capture_disarmed");
                journal.lock();
                found = AcquisitionCaptureOwners.find(uint32(request.characterGuid));
                if (found == AcquisitionCaptureOwners.end())
                    { result.status = "absent"; result.recordId = 0; result.reason = "no_current_capture_owner"; return result; }
                if (!SameCaptureOwner(found->second, identityKey, request))
                    { result.recordId = 0; result.reason = "capture_owner_conflict"; return result; }
            }
            auto original = AcquisitionAttempts.find(result.recordId);
            if (original != AcquisitionAttempts.end()) InvalidateAcquisition(original->second, "capture_disarmed");
            if (auto* ai = GET_PLAYERBOT_AI(player); ai && ai->rpgInfo.pythonAcquisitionRecord == result.recordId)
                ai->rpgInfo.pythonAcquisitionRecord = 0;
            AcquisitionCaptureOwners.erase(found);
            result.status = "disarmed"; result.reason = "original_records_and_pending_deliveries_retained"; return result;
        }
        if (found == AcquisitionCaptureOwners.end())
        {
            AcquisitionCaptureOwner owner; owner.identityKey = identityKey; owner.binding = request;
            found = AcquisitionCaptureOwners.emplace(uint32(request.characterGuid), std::move(owner)).first;
        }
        result.status = "armed"; result.recordId = found->second.recordId;
        result.reason = result.recordId ? "finite_capture_already_bound" : "awaiting_matching_accepted_goal";
        return result;
    }

    QuestAcquisitionSnapshot ReadQuestAcquisitionSnapshot(std::string const& identityKey,
        uint32 questId, std::string const& kind, uint32 index, uint32 item) override
    {
        QuestAcquisitionSnapshot result; result.identityKey = identityKey;
        result.strategicGeneration = StrategicGeneration.load();
        auto phase = ReadPhaseSnapshot(); result.phaseDigest = phase.policyDigest;
        if (!Hex64(identityKey)) { result.reason = "invalid_identity"; return result; }
        if (!questId || !item || (kind != "guaranteed" && kind != "choice") ||
            (kind == "guaranteed" ? index >= QUEST_REWARDS_COUNT : index >= QUEST_REWARD_CHOICES_COUNT))
            { result.reason = "invalid_reward_request"; return result; }
        {
            std::lock_guard<std::mutex> lock(ProgressionMutex);
            auto identity = AdmittedProgressionIdentities.find(identityKey);
            if (identity == AdmittedProgressionIdentities.end()) { result.reason = "not_admitted"; return result; }
            auto policy = ManagedPolicies.find(identity->second);
            if (policy == ManagedPolicies.end() || !policy->second.admitted || !policy->second.accountId)
                { result.reason = "admission_cache_unavailable"; return result; }
            result.characterGuid = identity->second; result.accountId = policy->second.accountId;
        }
        auto* currentPlayer = sRandomPlayerbotMgr.GetPlayerBot(result.characterGuid);
        uint64 const currentGuid = currentPlayer ? currentPlayer->GetGUID().GetRawValue() : 0;
        uint32 const currentAccount = currentPlayer && currentPlayer->GetSession() ? currentPlayer->GetSession()->GetAccountId() : 0;
        uint64 const currentSession = currentPlayer && currentPlayer->GetSession() ? currentPlayer->GetSession()->GetPartySessionIncarnation() : 0;
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        for (auto const& entry : AcquisitionAttempts)
        {
            auto const& attempt = entry.second;
            if (attempt.identityKey != identityKey || attempt.accountId != result.accountId ||
                attempt.characterGuid != result.characterGuid || attempt.questId != questId ||
                attempt.rewardKind != kind || attempt.rewardIndex != index || attempt.itemEntry != item) continue;
            auto copy = attempt;
            bool sameSession = currentGuid == attempt.characterGuid && currentAccount == attempt.accountId &&
                currentSession && currentSession == attempt.sessionIncarnation;
            if (!sameSession || copy.strategicGeneration != result.strategicGeneration || copy.phaseDigest != result.phaseDigest)
            {
                copy.currentContextMatches = false;
            }
            copy.recordSha256 = attempt.terminal ? AcquisitionRecordDigest(attempt) : "";
            result.attempts.push_back(std::move(copy));
        }
        result.valid = phase.enabled && phase.policyValid && !result.phaseDigest.empty() &&
            result.strategicGeneration == StrategicGeneration.load();
        result.reason = result.valid ? (result.attempts.empty() ? "no_native_observation" : "") : "phase_or_generation_invalid";
        return result;
    }

    std::vector<AcquisitionAcknowledgementResult> AcknowledgeQuestAcquisition(std::string const& identityKey,
        std::vector<AcquisitionAcknowledgement> const& acknowledgements) override
    {
        auto owner = ReadQuestSnapshotImpl(identityKey, 0, false);
        auto* player = owner.accountId ? sRandomPlayerbotMgr.GetPlayerBot(owner.characterGuid) : nullptr;
        bool authenticated = owner.online && player && player->GetSession() &&
            player->GetGUID().GetRawValue() == owner.characterGuid && player->GetSession()->GetAccountId() == owner.accountId &&
            !player->GetSession()->isLogingOut() && player->GetSession()->GetPartySessionIncarnation();
        auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
        if (authenticated)
        {
            std::lock_guard<std::mutex> lock(ProgressionMutex);
            auto identity = AdmittedProgressionIdentities.find(identityKey);
            auto policy = ManagedPolicies.find(owner.characterGuid);
            // Closed originals authenticate the current durable actor, not permission for new effects.
            authenticated = identity != AdmittedProgressionIdentities.end() && identity->second == owner.characterGuid &&
                policy != ManagedPolicies.end() && policy->second.admitted && policy->second.accountId == owner.accountId;
        }
        std::vector<AcquisitionAcknowledgementResult> results;
        results.reserve(acknowledgements.size());
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        for (auto const& proof : acknowledgements)
        {
            AcquisitionAcknowledgementResult result;
            result.recordId = proof.recordId; result.recordSha256 = proof.recordSha256; result.durableSha256 = proof.durableSha256;
            auto found = AcquisitionAttempts.find(proof.recordId);
            if (!authenticated) result.reason = "current_owner_not_authenticated";
            else if (!AcquisitionHash64(proof.recordSha256) || !AcquisitionHash64(proof.durableSha256)) result.reason = "verified_custody_hash_required";
            else if (found == AcquisitionAttempts.end()) result.reason = "record_not_found_no_release_claim";
            else
            {
                auto const& record = found->second;
                if (record.identityKey != identityKey || record.accountId != owner.accountId || record.characterGuid != owner.characterGuid ||
                    proof.accountId != record.accountId || proof.characterGuid != record.characterGuid ||
                    proof.sessionIncarnation != record.sessionIncarnation || proof.strategicGeneration != record.strategicGeneration ||
                    proof.goalRevision != record.goalRevision || proof.phaseDigest != record.phaseDigest || proof.sourceKey != record.sourceKey)
                    result.reason = "original_record_owner_or_binding_mismatch";
                else if (!record.terminal || record.turnInFlight || record.pendingDeliveries)
                    result.reason = "record_active_or_delivery_unconfirmed";
                else if (AcquisitionRecordDigest(record) != proof.recordSha256) result.reason = "original_record_hash_mismatch";
                else
                {
                    result.recoveredClosedContext = record.sessionIncarnation != player->GetSession()->GetPartySessionIncarnation() ||
                        record.strategicGeneration != StrategicGeneration.load() || record.phaseDigest != phase.policyConfigDigest;
                    result.status = "released"; result.reason = "caller_verified_original_durable_custody";
                    if (auto* ai = GET_PLAYERBOT_AI(player); ai && ai->rpgInfo.pythonAcquisitionRecord == proof.recordId)
                        ai->rpgInfo.pythonAcquisitionRecord = 0;
                    AcquisitionAttempts.erase(found);
                }
            }
            results.push_back(std::move(result));
        }
        return results;
    }


    SourcePredicateContext ReadSourcePredicates(std::string const& identityKey, SourcePredicateQuery const& query) override
    {
        if (!Hex64(identityKey) || query.identityKey != identityKey || !query.accountId || !query.characterGuid || !query.sessionIncarnation ||
            query.candidateId.empty() || !Hex64(query.generation) || !Hex64(query.graphDigest) || !Hex64(query.manifestDigest) ||
            !Hex64(query.projectionDigest) || !Hex64(query.queryDigest) || !AcquisitionHash64(query.policyDigest))
            throw std::runtime_error("source predicate binding malformed");
        std::set<std::string> ids;
        for (auto const& row : query.predicates)
        {
            if (!Hex64(row.predicateId) || !Hex64(row.definitionSha256) || !ids.insert(row.predicateId).second ||
                row.sourceId.empty() || row.sourceKind.empty() || !row.sourceEntry || !row.itemId || row.conditionId.empty() ||
                row.astPath.empty() || row.ownerKind.empty() || !row.ownerEntry ||
                (row.locusKind != "source_edge" && row.locusKind != "source_node" && row.locusKind != "item_node" && row.locusKind != "reagent_edge" && row.locusKind != "reagent_node") ||
                (row.hasReward && (row.rewardItem != row.itemId || (row.rewardKind != "guaranteed" && row.rewardKind != "choice"))))
                throw std::runtime_error("source predicate row malformed or duplicate");
        }
        // IDs are hex64, so this is the exact existing canonical JSON digest of sorted IDs, without escaping ambiguity.
        std::string idJson = "[";
        for (auto const& id : ids) { if (idJson.size() > 1) idJson += ','; idJson += '"' + id + '"'; }
        idJson += ']';
        auto lowerDigest = [](std::string const& bytes)
        {
            auto digest = Sha256Hex(bytes);
            for (auto& character : digest) character = char(std::tolower(static_cast<unsigned char>(character)));
            return digest;
        };
        if (lowerDigest(idJson) != query.queryDigest) throw std::runtime_error("source predicate query closure digest mismatch");
        SourcePredicateContext result; result.binding = query;
        auto snapshot = ReadQuestSnapshotImpl(identityKey, 0, false);
        auto phase = ReadPhaseSnapshot();
        auto* player = sRandomPlayerbotMgr.GetPlayerBot(snapshot.characterGuid);
        bool admitted = snapshot.controlAllowed && player && player->IsInWorld() && player->GetSession() &&
            snapshot.accountId == query.accountId && player->GetGUID().GetRawValue() == query.characterGuid &&
            player->GetSession()->GetAccountId() == query.accountId && player->GetSession()->GetPartySessionIncarnation() == query.sessionIncarnation &&
            StrategicGeneration.load() == query.strategicGeneration && phase.enabled && phase.policyValid &&
            phase.phaseId == query.phaseId && phase.policyDigest == query.policyDigest && phase.phaseId == "1_19" && query.plannerPhase == 1;
        if (admitted)
        {
            auto sharedPhase = ProgressionSystem::GetSharedPhaseSnapshot();
            std::lock_guard<std::mutex> lock(ProgressionMutex);
            auto identity = AdmittedProgressionIdentities.find(identityKey);
            auto policy = ManagedPolicies.find(snapshot.characterGuid);
            admitted = identity != AdmittedProgressionIdentities.end() && identity->second == snapshot.characterGuid &&
                policy != ManagedPolicies.end() && policy->second.accountId == query.accountId && !policy->second.held &&
                CourtesyPolicyFresh(policy->second, sharedPhase);
        }
        std::ostringstream semantics; semantics.imbue(std::locale::classic());
        auto fact = [&](auto const& value) { std::ostringstream token; token.imbue(std::locale::classic()); token << value; auto text = token.str(); semantics << text.size() << ':' << text; };
        fact("core1-source-predicates-v1"); fact(query.candidateId); fact(query.generation); fact(query.graphDigest);
        fact(query.manifestDigest); fact(query.projectionDigest); fact(identityKey); fact(snapshot.accountId);
        fact(player ? player->GetGUID().GetRawValue() : uint64(0));
        fact(player && player->GetSession() ? player->GetSession()->GetPartySessionIncarnation() : uint64(0));
        fact(StrategicGeneration.load()); fact(phase.phaseId); fact(phase.policyDigest); fact(admitted);
        auto const& dispatch = ScriptRegistry<PlayerScript>::EnabledHooks;
        for (auto hook : {PLAYERHOOK_CAN_ACCEPT_QUEST, PLAYERHOOK_CAN_PROGRESS_QUEST_OBJECTIVE, PLAYERHOOK_CAN_REWARD_QUEST, PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE})
        {
            fact(uint32(hook));
            if (uint32(hook) < dispatch.size()) for (auto const* script : dispatch[hook]) if (script) fact(script->GetName());
            fact("end_dispatch");
        }
        for (auto const& row : query.predicates)
        {
            SourcePredicateVerdict verdict; verdict.predicateId = row.predicateId; verdict.definitionSha256 = row.definitionSha256;
            fact(row.predicateId); fact(row.definitionSha256); fact(row.sourceKind); fact(row.sourceEntry); fact(row.itemId);
            fact(row.hasReward); fact(row.rewardKind); fact(row.rewardIndex); fact(row.rewardItem); fact(row.locusKind); fact(row.ownerKind); fact(row.ownerEntry);
            fact(row.definition.type); for (auto number : row.definition.values) fact(number);
            if (!admitted) verdict.reason = "actor_session_generation_phase_or_control_mismatch";
            else
            {
                bool uint32Values = true;
                for (auto number : row.definition.values) if (number < 0 || uint64(number) > uint64(std::numeric_limits<uint32>::max())) uint32Values = false;
                Quest const* quest = row.sourceKind == "quest" ? sObjectMgr->GetQuestTemplate(row.sourceEntry) : nullptr;
                if (uint32Values && quest && row.hasReward && ValidRewardPreference(quest, row.rewardKind, row.rewardIndex, row.rewardItem))
                {
                    auto rewardItem = row.rewardKind == "choice" ? quest->RewardChoiceItemId[row.rewardIndex] : quest->RewardItemId[row.rewardIndex];
                    auto rewardCount = row.rewardKind == "choice" ? quest->RewardChoiceItemCount[row.rewardIndex] : quest->RewardItemIdCount[row.rewardIndex];
                    fact(rewardItem); fact(rewardCount);
                    uint32 const lowLevelHideDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
                    fact(quest->GetQuestLevel()); fact(lowLevelHideDiff); fact(quest->GetType()); fact(quest->GetSuggestedPlayers());
                    auto sourcePolicy = ProgressionSystem::ReadQuestSourcePolicy(quest->GetQuestId());
                    fact(sourcePolicy.templateKnown); fact(sourcePolicy.configured); fact(sourcePolicy.ready); fact(sourcePolicy.templateAllowed);
                    fact(sDisableMgr->IsDisabledFor(DISABLE_TYPE_QUEST, quest->GetQuestId(), nullptr));
                    auto match = [&](uint32 type, uint32 a, uint32 b, uint32 c)
                    {
                        if (row.definition.type != type || row.definition.values[0] != a || row.definition.values[1] != b || row.definition.values[2] != c) return false;
                        verdict.matchedDefinition.type = type; verdict.matchedDefinition.values[0] = a;
                        verdict.matchedDefinition.values[1] = b; verdict.matchedDefinition.values[2] = c;
                        return true;
                    };
                    bool matched = false;
                    bool questOwner = row.ownerKind == "quest" && row.ownerEntry == quest->GetQuestId();
                    if (questOwner && (row.locusKind == "source_edge" || row.locusKind == "source_node"))
                    {
                        auto conditions = sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_QUEST_AVAILABLE, quest->GetQuestId());
                        for (auto const* condition : conditions)
                            if (condition && !condition->ReferenceId && !condition->ScriptId && condition->ConditionTarget == 0 &&
                                match(uint32(condition->ConditionType), condition->ConditionValue1, condition->ConditionValue2, condition->ConditionValue3))
                                { matched = true; break; } // AST owns the native negative/else/reference composition, never aggregate truth here.
                        if (!matched)
                            matched = (quest->GetRequiredClasses() && match(15, quest->GetRequiredClasses(), 0, 0)) ||
                                (quest->GetAllowableRaces() && match(16, quest->GetAllowableRaces(), 0, 0)) ||
                                (quest->GetRequiredSkill() && quest->GetRequiredSkillValue() && match(7, quest->GetRequiredSkill(), quest->GetRequiredSkillValue(), 0)) ||
                                match(27, quest->GetMinLevel(), 3, 0) || (quest->GetMaxLevel() && match(27, quest->GetMaxLevel(), 4, 0));
                        // Exact native ordinary executor level rules at the authenticated quest owner.
                        // Nonpositive quest level uses actor level (Player::GetQuestLevel), so no fixed derived leaf is invented.
                        if (!matched && quest->GetQuestLevel() > 0)
                        {
                            int64 const capableLevel = std::max<int64>(0, int64(quest->GetQuestLevel()) - 3);
                            int64 const worthyLevel = int64(quest->GetQuestLevel()) + lowLevelHideDiff;
                            matched = match(27, uint32(capableLevel), 3, 0) ||
                                (worthyLevel >= 0 && uint64(worthyLevel) <= std::numeric_limits<uint32>::max() && match(27, uint32(worthyLevel), 4, 0));
                        }
                    }
                    else if (row.locusKind == "item_node" && row.ownerKind == "item" && row.ownerEntry == rewardItem)
                    {
                        if (auto const* item = sObjectMgr->GetItemTemplate(rewardItem))
                            matched = (item->AllowableClass && match(15, item->AllowableClass, 0, 0)) ||
                                (item->AllowableRace && match(16, item->AllowableRace, 0, 0)) ||
                                match(27, item->RequiredLevel, 3, 0);
                    }
                    if (matched)
                    {
                        auto const& definition = verdict.matchedDefinition; uint32 a = uint32(definition.values[0]), b = uint32(definition.values[1]);
                        bool supported = definition.values[2] == 0, truth = false;
                        if (supported) switch (definition.type)
                        {
                            case 15: supported = b == 0 && a != 0; fact(player->getClassMask()); truth = bool(player->getClassMask() & a); break;
                            case 16: supported = b == 0 && a != 0; fact(player->getRaceMask()); truth = bool(player->getRaceMask() & a); break;
                            case 7: supported = a != 0; fact(player->HasSkill(a)); fact(player->GetBaseSkillValue(a)); truth = player->HasSkill(a) && player->GetBaseSkillValue(a) >= b; break;
                            case 8: supported = b == 0 && a != 0; fact(player->GetQuestRewardStatus(a)); truth = player->GetQuestRewardStatus(a); break;
                            case 27:
                            {
                                uint32 level = player->GetLevel(); fact(level); supported = b <= 4;
                                switch (b) { case 0: truth = level == a; break; case 1: truth = level > a; break; case 2: truth = level < a; break; case 3: truth = level >= a; break; case 4: truth = level <= a; break; }
                                break;
                            }
                            default: supported = false; break;
                        }
                        if (supported)
                        {
                            verdict.verdict = truth ? "true" : "false"; verdict.reason = "actual_loaded_definition_and_current_native_primitive";
                            verdict.sourceKind = "quest"; verdict.sourceEntry = quest->GetQuestId(); verdict.itemId = rewardItem;
                            verdict.hasReward = true; verdict.rewardKind = row.rewardKind == "choice" ? "choice" : "guaranteed";
                            verdict.rewardIndex = row.rewardIndex; verdict.rewardItem = rewardItem;
                            verdict.ownerKind = questOwner ? "quest" : "item"; verdict.ownerEntry = questOwner ? quest->GetQuestId() : rewardItem;
                        }
                        else verdict.reason = "native_definition_primitive_semantics_unsupported";
                    }
                }
            }
            fact(verdict.verdict); fact(verdict.reason);
            if (verdict.verdict != "unknown") { fact(verdict.matchedDefinition.type); for (auto number : verdict.matchedDefinition.values) fact(number); }
            result.verdicts.push_back(std::move(verdict));
        }
        auto finalPhase = ReadPhaseSnapshot();
        result.valid = admitted && StrategicGeneration.load() == query.strategicGeneration && ObjectAccessor::FindPlayer(player->GetGUID()) == player &&
            player->GetSession() && player->GetSession()->GetPartySessionIncarnation() == query.sessionIncarnation &&
            finalPhase.enabled && finalPhase.policyValid && finalPhase.phaseId == phase.phaseId && finalPhase.policyDigest == phase.policyDigest;
        result.reason = result.valid ? "" : "actor_session_generation_phase_or_control_mismatch";
        if (!result.valid) for (auto& verdict : result.verdicts) { verdict.verdict = "unknown"; verdict.reason = result.reason; }
        fact(result.valid); result.observedStateGeneration = lowerDigest(semantics.str());
        return result;
    }

    LoadedSourceSnapshot ReadLoadedSourceSnapshot(
        std::vector<LoadedSourceNaturalKey> const& requestedKeys) override
    {
        if (requestedKeys.empty() || requestedKeys.size() > 64)
            throw std::runtime_error("loaded source snapshot query must contain 1..64 natural keys");

        LoadedSourceSnapshot result;
        result.processIncarnation = _processIncarnation;
        result.providerConstructedMonotonicNs = _providerConstructedMonotonicNs;
        result.captureMonotonicNs = MonotonicNanoseconds();
        result.reloadGenerationAvailable = false;
        auto finishCopyOut = [&]()
        {
            result.copyOutCompletedMonotonicNs = MonotonicNanoseconds();
            result.copyOutDurationNs = result.copyOutCompletedMonotonicNs >= result.captureMonotonicNs ?
                result.copyOutCompletedMonotonicNs - result.captureMonotonicNs : 0;
        };
        result.phase = ReadPhaseSnapshot();
        if (!result.phase.enabled || !result.phase.policyValid)
        {
            result.reason = "shared_phase_unavailable";
            finishCopyOut();
            return result;
        }

        std::vector<LoadedSourceNaturalKey> keys = requestedKeys;
        std::sort(keys.begin(), keys.end(), [](LoadedSourceNaturalKey const& left, LoadedSourceNaturalKey const& right)
        {
            return std::tie(left.sourceTable, left.entry, left.slot, left.item) <
                std::tie(right.sourceTable, right.entry, right.slot, right.item);
        });
        std::set<std::tuple<std::string, uint32, uint32, uint32>> uniqueKeys;
        for (LoadedSourceNaturalKey const& key : keys)
        {
            if (key.sourceTable.empty() || key.sourceTable.size() > 64 || !key.entry || !key.item ||
                !uniqueKeys.emplace(key.sourceTable, key.entry, key.slot, key.item).second)
                throw std::runtime_error("loaded source natural key malformed or duplicated");

            LoadedVendorSourceObservation row;
            row.sourceTable = key.sourceTable; row.entry = key.entry; row.slot = key.slot; row.item = key.item;
            row.family = key.sourceTable == "npc_vendor" ? "vendor" : "unknown";
            row.stableJoinKey = key.sourceTable == "npc_vendor" ?
                "vendor:" + std::to_string(key.entry) + ":slot:" + std::to_string(key.slot) +
                    ":item:" + std::to_string(key.item) : "";
            if (key.sourceTable != "npc_vendor")
                row.reason = "unsupported_native_source_table";
            else
            {
                row.lookupEvaluated = true;
                VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(key.entry);
                if (!list)
                    row.reason = "vendor_source_list_not_loaded";
                else if (key.slot >= list->GetItemCount())
                    row.reason = "vendor_source_slot_not_loaded";
                else
                {
                    VendorItem const* offer = list->GetItem(key.slot);
                    if (!offer)
                        row.reason = "vendor_source_slot_empty";
                    else if (offer->item != key.item)
                        row.reason = "vendor_source_slot_item_mismatch";
                    else
                    {
                        row.matched = true;
                        row.maxCount = offer->maxcount;
                        row.incrementTime = offer->incrtime;
                        row.extendedCost = offer->ExtendedCost;
                        if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(offer->item))
                        {
                            row.itemTemplateKnown = true;
                            row.buyPrice = item->BuyPrice;
                            row.buyCount = item->BuyCount;
                            row.itemRequiredLevel = item->RequiredLevel;
                            row.allowableClass = item->AllowableClass;
                            row.allowableRace = item->AllowableRace;
                            row.requiredSkill = item->RequiredSkill;
                            row.requiredSkillRank = item->RequiredSkillRank;
                        }
                        else
                            row.reason = "vendor_item_template_not_loaded";
                        // Actor-specific vendor condition truth remains on the existing
                        // ReadVendorOffers/action path; this static source snapshot does not infer it.
                    }
                }

                // These are the current Core1 vendor projection fields for which this
                // loaded native row has no directly equivalent, proven semantic value.
                for (char const* field : {"purchase_minimum", "purchase_maximum", "chance_numerator",
                    "chance_denominator", "frontier_minimum", "frontier_maximum", "stages", "phase_mask",
                    "spawn_mask", "event_ids", "pool_ids", "condition", "group_size", "capabilities",
                    "profession_skill_id", "profession_skill_value", "alternative", "reagents", "unresolved"})
                    if (std::find(row.unknownSemantics.begin(), row.unknownSemantics.end(), field) == row.unknownSemantics.end())
                        row.unknownSemantics.emplace_back(field);
                row.unknownSemantics.emplace_back("VerifiedBuild");
                if (!row.itemTemplateKnown)
                    for (char const* field : {"item_template", "buy_price", "buy_count", "item_requirements"})
                        row.unknownSemantics.emplace_back(field);
            }
            row.unknownMetrics = {"copper", "travel", "attempt", "risk", "contention"};
            result.rows.push_back(std::move(row));
        }

        PhaseSnapshot const finalPhase = ReadPhaseSnapshot();
        if (!finalPhase.enabled || !finalPhase.policyValid || finalPhase.phaseId != result.phase.phaseId ||
            finalPhase.levelCeiling != result.phase.levelCeiling || finalPhase.policyDigest != result.phase.policyDigest)
        {
            result.reason = "shared_phase_changed_during_loaded_source_snapshot";
            result.rows.clear();
            finishCopyOut();
            return result;
        }

        std::ostringstream snapshot;
        snapshot.imbue(std::locale::classic());
        auto field = [&](auto const& value)
        {
            std::ostringstream token; token.imbue(std::locale::classic()); token << value;
            std::string const text = token.str(); snapshot << text.size() << ':' << text;
        };
        field("cpi211-loaded-item-sources-v1");
        field("rows"); field(result.rows.size());
        for (LoadedVendorSourceObservation const& row : result.rows)
        {
            field("row");
            field("family");
            field(row.family); field("source_table"); field(row.sourceTable);
            field("natural_key.entry"); field(row.entry); field("natural_key.slot"); field(row.slot);
            field("natural_key.item"); field(row.item); field("stable_join_key"); field(row.stableJoinKey);
            field("lookup_evaluated"); field(row.lookupEvaluated);
            field("matched"); field(row.matched); field("reason"); field(row.reason);
            field("item_template_known"); field(row.itemTemplateKnown);
            field("maxcount"); field(row.maxCount); field("incrtime"); field(row.incrementTime);
            field("ExtendedCost"); field(row.extendedCost); field("buy_price"); field(row.buyPrice);
            field("buy_count"); field(row.buyCount); field("required_level"); field(row.itemRequiredLevel);
            field("allowable_class"); field(row.allowableClass); field("allowable_race"); field(row.allowableRace);
            field("required_skill"); field(row.requiredSkill); field("required_skill_rank"); field(row.requiredSkillRank);
            field("unknown_semantics"); field(row.unknownSemantics.size());
            for (std::string const& unknown : row.unknownSemantics) field(unknown);
            field("unknown_metrics"); field(row.unknownMetrics.size());
            for (std::string const& unknown : row.unknownMetrics) field(unknown);
        }
        result.sourceSnapshotId = Sha256Hex(snapshot.str());
        result.valid = true;
        finishCopyOut();
        return result;
    }

    AcquisitionCatalog ReadAcquisitionCatalog() override
    {
        AcquisitionCatalog result;
        result.observationGeneration = StrategicGeneration.load();
        result.phase = ReadPhaseSnapshot();
        if (!result.phase.enabled || !result.phase.policyValid)
        {
            result.reason = "shared_phase_not_valid";
            return result;
        }
        // Synchronous native world-thread read; only owned values escape.
        result.playerHookRegistryInitialized = !ScriptRegistry<PlayerScript>::EnabledHooks.empty();
        for (auto const& hook : std::vector<std::pair<uint32, std::string>>{
            {PLAYERHOOK_CAN_ACCEPT_QUEST, "OnPlayerCanAcceptQuest"},
            {PLAYERHOOK_CAN_PROGRESS_QUEST_OBJECTIVE, "OnPlayerCanProgressQuestObjective"},
            {PLAYERHOOK_CAN_REWARD_QUEST, "OnPlayerCanRewardQuest"},
            {PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE, "OnPlayerBeforeQuestComplete"}})
        {
            AcquisitionHookDispatch dispatch; dispatch.hookId = hook.first; dispatch.hookName = hook.second;
            auto const& enabled = ScriptRegistry<PlayerScript>::EnabledHooks;
            if (hook.first < enabled.size()) for (auto const* script : enabled[hook.first])
                if (script) dispatch.scriptNames.push_back(script->GetName());
            result.playerHookDispatch.push_back(std::move(dispatch));
        }
        for (auto const& row : *sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const& item = row.second;
            result.items.push_back({row.first, item.Class, item.SubClass, item.InventoryType,
                item.ItemLevel, item.RequiredLevel, item.AllowableClass, item.AllowableRace,
                item.RequiredSkill, item.RequiredSkillRank, item.Name1, item.RandomProperty, item.RandomSuffix});
        }
        // Index loaded source registries once per catalogue read, rather than re-scan all spawns per quest.
        std::map<uint32, std::set<int32>> giverEntriesByQuest;
        for (auto const& relation : *sObjectMgr->GetCreatureQuestRelationMap()) giverEntriesByQuest[relation.second].insert(int32(relation.first));
        for (auto const& relation : *sObjectMgr->GetGOQuestRelationMap()) giverEntriesByQuest[relation.second].insert(-int32(relation.first));
        for (auto const& relation : sPoolMgr->mQuestCreatureRelation) giverEntriesByQuest[relation.first].insert(int32(relation.second));
        for (auto const& relation : sPoolMgr->mQuestGORelation) giverEntriesByQuest[relation.first].insert(-int32(relation.second));
        auto eventRelationsByQuest = sGameEventMgr->CopyQuestEventRelations();
        for (auto const& quest : eventRelationsByQuest) for (auto const& relation : quest.second) giverEntriesByQuest[quest.first].insert(relation.first);
        std::map<uint32, std::vector<uint32>> itemGiversByQuest;
        for (auto const& item : *sObjectMgr->GetItemTemplateStore()) if (item.second.StartQuest) itemGiversByQuest[item.second.StartQuest].push_back(item.first);
        std::map<int32, std::vector<uint32>> spawnIdsByEntry;
        for (auto const& spawn : sObjectMgr->GetAllCreatureData())
        {
            for (auto entry : std::set<uint32>{spawn.second.id, spawn.second.id2, spawn.second.id3})
                if (entry) spawnIdsByEntry[int32(entry)].push_back(spawn.first);
        }
        for (auto const& spawn : sObjectMgr->GetAllGOData()) spawnIdsByEntry[-int32(spawn.second.id)].push_back(spawn.first);
        std::map<uint32, std::vector<int32>> creatureEvents, objectEvents;
        auto indexEvents = [&](auto const& events, auto& output)
        {
            for (uint32 slot = 0; slot < events.size(); ++slot)
                for (auto guid : events[slot]) output[guid].push_back(int32(slot) - int32(sGameEventMgr->GetEventMap().size()) + 1);
        };
        indexEvents(sGameEventMgr->GameEventCreatureGuids, creatureEvents);
        indexEvents(sGameEventMgr->GameEventGameobjectGuids, objectEvents);
        for (auto const& row : sObjectMgr->GetQuestTemplates())
        {
            Quest const* quest = row.second;
            if (!quest)
                throw std::runtime_error("null loaded acquisition quest template");
            AcquisitionQuest value{row.first, quest->GetMinLevel(), quest->GetMaxLevel(),
                quest->GetRequiredClasses(), quest->GetAllowableRaces(), quest->GetFlags(), quest->GetType(), {}};
            value.requiredSkill = quest->GetRequiredSkill(); value.requiredSkillRank = quest->GetRequiredSkillValue();
            value.nativeQuestLevel = quest->GetQuestLevel(); value.nativeSuggestedPlayers = quest->GetSuggestedPlayers();
            value.nativeLowLevelHideDiff = sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
            value.questDisabled = sDisableMgr->IsDisabledFor(DISABLE_TYPE_QUEST, row.first, nullptr);
            auto sourcePolicy = ProgressionSystem::ReadQuestSourcePolicy(row.first);
            value.phaseTemplateKnown = sourcePolicy.templateKnown; value.phaseGateConfigured = sourcePolicy.configured;
            value.phaseGateReady = sourcePolicy.ready; value.phaseTemplateAllowed = sourcePolicy.templateAllowed;
            for (uint32 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
                value.requiredItems.push_back({index, quest->RequiredItemId[index], quest->RequiredItemCount[index]});
            for (uint32 index = 0; index < QUEST_SOURCE_ITEM_IDS_COUNT; ++index)
                value.requiredSourceItems.push_back({index, quest->ItemDrop[index], quest->ItemDropQuantity[index]});
            for (uint32 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
                value.requiredTargets.push_back({index, quest->RequiredNpcOrGoCount[index], quest->RequiredNpcOrGo[index]});
            value.previousQuestId = quest->GetPrevQuestId(); value.nextQuestId = quest->GetNextQuestId();
            value.nextQuestInChain = quest->GetNextQuestInChain(); value.exclusiveGroup = quest->GetExclusiveGroup();
            value.signedMoney = quest->GetRewOrReqMoney();
            value.minimumRepFaction = quest->GetRequiredMinRepFaction(); value.minimumRepValue = quest->GetRequiredMinRepValue();
            value.maximumRepFaction = quest->GetRequiredMaxRepFaction(); value.maximumRepValue = quest->GetRequiredMaxRepValue();
            value.repObjectiveFaction = quest->GetRepObjectiveFaction(); value.repObjectiveValue = quest->GetRepObjectiveValue();
            value.repObjectiveFaction2 = quest->GetRepObjectiveFaction2(); value.repObjectiveValue2 = quest->GetRepObjectiveValue2();
            if (auto const* dependents = sObjectMgr->GetBreadcrumbsForQuest(row.first)) value.breadcrumbDependents = *dependents;
            value.timed = quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED);
            value.explorationOrEvent = quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT);
            value.castObjective = quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_CAST);
            value.requiredPlayerKills = quest->GetPlayersSlain();
            value.sourceItem = quest->GetSrcItemId(); value.sourceItemCount = quest->GetSrcItemCount();
            value.sourceSpell = quest->GetSrcSpell(); value.timeAllowed = quest->GetTimeAllowed();
            value.eventId = quest->GetEventIdForQuest(); value.breadcrumbQuest = quest->GetBreadcrumbForQuestId();
            value.rewardMailTemplate = quest->GetRewMailTemplateId();
            value.repeatable = quest->IsRepeatable(); value.daily = quest->IsDaily(); value.weekly = quest->IsWeekly();
            value.monthly = quest->IsMonthly(); value.seasonal = quest->IsSeasonal(); value.dungeonFinder = quest->IsDFQuest();
            value.previousQuests = quest->prevQuests; value.previousChainQuests = quest->prevChainQuests;
            auto group = sObjectMgr->mExclusiveQuestGroups.equal_range(value.exclusiveGroup);
            if (value.exclusiveGroup) for (auto it = group.first; it != group.second; ++it) value.exclusiveQuests.push_back(it->second);
            auto conditions = sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_QUEST_AVAILABLE, row.first);
            for (auto const* condition : conditions)
            {
                if (!condition) throw std::runtime_error("null loaded acquisition condition");
                value.conditions.push_back({uint32(condition->SourceType), condition->SourceGroup, condition->SourceId,
                    condition->ElseGroup, uint32(condition->ConditionType), condition->SourceEntry,
                    condition->ConditionValue1, condition->ConditionValue2, condition->ConditionValue3,
                    condition->ReferenceId, condition->ScriptId, condition->ConditionTarget, condition->NegativeCondition});
            }
            auto routes = sTravelMgr.quests.find(row.first);
            if (routes != sTravelMgr.quests.end() && routes->second)
            {
                auto appendGiver = [&](auto const& destinations)
                {
                    for (auto* destination : destinations)
                    {
                        auto* relation = dynamic_cast<QuestRelationTravelDestination*>(destination);
                        if (!relation || relation->GetQuestTemplate() != quest) continue;
                        AcquisitionGiverScript giver; giver.entry = relation->getEntry(); giver.relation = relation->getRelation();
                        if (giver.entry > 0)
                        {
                            if (auto const* creature = sObjectMgr->GetCreatureTemplate(giver.entry))
                            {
                                giver.templateKnown = true; giver.scriptId = creature->ScriptID; giver.aiName = creature->AIName;
                                giver.registryResolved = !giver.scriptId;
                                if (giver.scriptId) if (auto const* script = ScriptRegistry<CreatureScript>::GetScriptById(giver.scriptId))
                                    { giver.registeredScriptName = script->GetName(); giver.registryResolved = true; }
                            }
                        }
                        else if (auto const* object = sObjectMgr->GetGameObjectTemplate(-giver.entry))
                        {
                            giver.templateKnown = true; giver.scriptId = object->ScriptId; giver.aiName = object->AIName;
                            giver.registryResolved = !giver.scriptId;
                            if (giver.scriptId) if (auto const* script = ScriptRegistry<GameObjectScript>::GetScriptById(giver.scriptId))
                                { giver.registeredScriptName = script->GetName(); giver.registryResolved = true; }
                        }
                        value.giverScripts.push_back(std::move(giver));
                    }
                };
                appendGiver(routes->second->questGivers); appendGiver(routes->second->questTakers);
            }
            // Complete loaded giver branches, including inactive event/pool relations, never just a selected route.
            auto const& entries = giverEntriesByQuest[row.first];
            auto const& eventRelations = eventRelationsByQuest[row.first];
            value.questEventIds = sGameEventMgr->CopyQuestEventIds(row.first);
            for (auto const& relation : eventRelations) value.questEventIds.push_back(relation.second);
            auto addPoolChain = [&](uint32 pool, auto& output)
            {
                std::set<uint32> seen;
                while (pool)
                {
                    if (!seen.insert(pool).second) { value.giverSourceMissing.push_back("pool_parent_cycle"); break; }
                    output.push_back(pool); pool = sPoolMgr->IsPartOfAPool<Pool>(pool);
                }
            };
            addPoolChain(sPoolMgr->IsPartOfAPool<Quest>(row.first), value.questPoolIds);
            value.itemGivers = itemGiversByQuest[row.first];
            if (!value.itemGivers.empty()) value.giverSourceMissing.push_back("item_start_quest_requires_separate_giver_branch");
            if (!sGameEventMgr->QuestSourceRegistryReady()) value.giverSourceMissing.push_back("event_registry_not_initialized");
            for (int32 entry : entries)
            {
                value.giverEntries.push_back(entry);
                if (std::none_of(value.giverScripts.begin(), value.giverScripts.end(), [&](auto const& existing) { return existing.entry == entry && existing.relation == 0; }))
                {
                    AcquisitionGiverScript giver; giver.entry = entry;
                    if (entry > 0)
                    {
                        if (auto const* creature = sObjectMgr->GetCreatureTemplate(entry))
                        {
                            giver.templateKnown = true; giver.scriptId = creature->ScriptID; giver.aiName = creature->AIName;
                            giver.registryResolved = !giver.scriptId;
                            if (giver.scriptId) if (auto const* script = ScriptRegistry<CreatureScript>::GetScriptById(giver.scriptId))
                                { giver.registeredScriptName = script->GetName(); giver.registryResolved = true; }
                        }
                    }
                    else if (auto const* object = sObjectMgr->GetGameObjectTemplate(-entry))
                    {
                        giver.templateKnown = true; giver.scriptId = object->ScriptId; giver.aiName = object->AIName;
                        giver.registryResolved = !giver.scriptId;
                        if (giver.scriptId) if (auto const* script = ScriptRegistry<GameObjectScript>::GetScriptById(giver.scriptId))
                            { giver.registeredScriptName = script->GetName(); giver.registryResolved = true; }
                    }
                    value.giverScripts.push_back(std::move(giver));
                }
                auto appendSpawn = [&](uint32 guid, auto const& spawn, bool multispawn)
                {
                    AcquisitionGiverSpawn observed;
                    observed.entry = entry; observed.guid = guid; observed.mapId = spawn.mapid;
                    observed.phaseMask = spawn.phaseMask; observed.spawnMask = spawn.spawnMask;
                    observed.spawnGroupId = spawn.spawnGroupId; observed.scriptId = spawn.ScriptId; observed.multispawn = multispawn;
                    if (auto const* groupData = sObjectMgr->GetSpawnGroupData(spawn.spawnGroupId)) observed.spawnGroupFlags = uint32(groupData->flags);
                    else value.giverSourceMissing.push_back("spawn_group_template_missing");
                    if (spawn.spawnGroupId > 1) value.giverSourceMissing.push_back("conditional_spawn_group_not_evaluated");
                    if (multispawn) value.giverSourceMissing.push_back("multispawn_branch_selection_not_evaluated");
                    if (spawn.ScriptId) value.giverSourceMissing.push_back("spawn_script_semantics_not_evaluated");
                    observed.eventIds = entry > 0 ? creatureEvents[guid] : objectEvents[guid];
                    for (auto const& relation : eventRelations) if (relation.first == entry) observed.eventIds.push_back(int32(relation.second));
                    addPoolChain(entry > 0 ? sPoolMgr->GetCreaturePoolId(guid) : sPoolMgr->GetGameObjectPoolId(guid), observed.poolIds);
                    for (auto pool : observed.poolIds)
                    {
                        auto poolEvents = sGameEventMgr->CopyPoolEventIds(pool);
                        observed.eventIds.insert(observed.eventIds.end(), poolEvents.begin(), poolEvents.end());
                    }
                    std::sort(observed.eventIds.begin(), observed.eventIds.end());
                    observed.eventIds.erase(std::unique(observed.eventIds.begin(), observed.eventIds.end()), observed.eventIds.end());
                    value.giverSpawns.push_back(std::move(observed));
                };
                auto before = value.giverSpawns.size();
                for (auto guid : spawnIdsByEntry[entry])
                {
                    if (entry > 0)
                    {
                        if (auto const* spawn = sObjectMgr->GetCreatureData(guid)) appendSpawn(guid, *spawn, spawn->id2 || spawn->id3);
                    }
                    else if (auto const* spawn = sObjectMgr->GetGameObjectData(guid)) appendSpawn(guid, *spawn, false);
                }
                if (before == value.giverSpawns.size()) value.giverSourceMissing.push_back("giver_entry_has_no_loaded_static_spawn");
            }
            if (entries.empty()) value.giverSourceMissing.push_back("no_loaded_creature_or_gameobject_giver_relation");
            value.giverSourceComplete = value.giverSourceMissing.empty();
            for (uint8 index = 0; index < QUEST_REWARDS_COUNT; ++index)
                if (quest->RewardItemId[index] || quest->RewardItemIdCount[index])
                    value.rewards.push_back({"fixed", index, quest->RewardItemId[index], quest->RewardItemIdCount[index]});
            for (uint8 index = 0; index < QUEST_REWARD_CHOICES_COUNT; ++index)
                if (quest->RewardChoiceItemId[index] || quest->RewardChoiceItemCount[index])
                    value.rewards.push_back({"choice", index, quest->RewardChoiceItemId[index], quest->RewardChoiceItemCount[index]});
            result.quests.push_back(std::move(value));
        }
        auto phase = ReadPhaseSnapshot();
        result.valid = result.observationGeneration == StrategicGeneration.load() && phase.enabled &&
            phase.policyValid && phase.phaseId == result.phase.phaseId &&
            phase.levelCeiling == result.phase.levelCeiling && phase.policyDigest == result.phase.policyDigest;
        if (!result.valid)
        {
            result.items.clear(); result.quests.clear();
            result.reason = "acquisition_observation_generation_changed";
        }
        return result;
    }

    GearResources ReadGearResources(std::string const& identityKey,
        std::vector<uint32> const& skillIds, std::vector<uint32> const& itemIds,
        std::vector<uint32> const& questIds) override
    {
        if (skillIds.size() > 64 || itemIds.size() > 256 || questIds.size() > 256)
            throw std::runtime_error("gear resource query exceeds its ID bound");
        std::unordered_set<uint32> skills, items, quests;
        for (uint32 id : skillIds)
            if (!id || !skills.insert(id).second || !sSkillLineStore.LookupEntry(id))
                throw std::runtime_error("invalid or duplicate gear resource skill ID");
        for (uint32 id : itemIds)
            if (!id || !items.insert(id).second || !sObjectMgr->GetItemTemplate(id))
                throw std::runtime_error("invalid or duplicate gear resource item ID");
        for (uint32 id : questIds)
            if (!id || !quests.insert(id).second || !sObjectMgr->GetQuestTemplate(id))
                throw std::runtime_error("invalid or duplicate gear resource quest ID");
        StrategicSnapshot snapshot = ReadQuestSnapshotImpl(identityKey, 0, false);
        GearResources result;
        result.identityKey = snapshot.identityKey;
        result.accountId = snapshot.accountId;
        result.characterGuid = snapshot.characterGuid;
        result.online = snapshot.online;
        result.controlAllowed = snapshot.controlAllowed;
        result.reason = snapshot.reason;
        if (!result.controlAllowed)
            return result;
        Player* player = sRandomPlayerbotMgr.GetPlayerBot(result.characterGuid);
        if (!player || !player->IsInWorld())
        {
            result.online = false;
            result.controlAllowed = false;
            result.reason = "offline";
            return result;
        }
        if (!player->GetSession() || player->GetSession()->GetAccountId() != result.accountId)
            throw std::runtime_error("strategic session account differs from durable binding");
        result.reason = StrategicControlReason(player);
        result.controlAllowed = result.reason.empty();
        if (!result.controlAllowed)
            return result;
        // Synchronous copied values: pure learned rank, carried/equipped counts, no bank.
        result.mapId = player->GetMapId();
        result.zoneId = player->GetZoneId();
        result.level = player->GetLevel();
        result.moneyCopper = player->GetMoney();
        for (uint32 id : skillIds)
            result.professionSkillValues.emplace(id, player->GetPureSkillValue(id));
        for (uint32 id : itemIds)
            result.itemCounts.emplace(id, player->GetItemCount(id, false));
        for (uint32 id : questIds)
            if (player->IsQuestRewarded(id))
                result.rewardedQuestIds.push_back(id);
        return result;
    }

    std::vector<GearVendorOffer> ReadVendorOffers(std::string const& identityKey,
        std::vector<uint32> const& itemIds) override
    {
        if (itemIds.size() > 64) throw std::runtime_error("vendor item query budget");
        std::unordered_set<uint32> ids;
        for (uint32 id : itemIds) if (!id || !ids.insert(id).second) throw std::runtime_error("invalid vendor item query");
        StrategicSnapshot snapshot = ReadQuestSnapshotImpl(identityKey, 0, false);
        std::vector<GearVendorOffer> offers;
        if (!snapshot.controlAllowed || _vendorInFlight.count(identityKey) || _vendorAmbiguous.count(identityKey)) return offers;
        Player* player = sRandomPlayerbotMgr.GetPlayerBot(snapshot.characterGuid);
        PlayerbotAI* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
        if (!ai || player->GetSession()->GetCurrentVendor() ||
            !std::holds_alternative<NewRpgInfo::Idle>(ai->rpgInfo.data) || ai->rpgInfo.pythonQuestGoal) return offers;
        auto* nearby = ai->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs");
        if (!nearby) return offers;
        GuidVector vendors = nearby->Get();
        if (vendors.size() > 64) vendors.resize(64);
        StatsWeightCalculator calculator(player);
        calculator.SetItemSetBonus(false); calculator.SetOverflowPenalty(false);
        calculator.SetPvpSpec(sRandomPlayerbotMgr.IsSpecPvp(player->GetGUID().GetCounter(), player->getClass()));
        for (ObjectGuid guid : vendors)
        {
            Creature* vendor = player->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_VENDOR);
            VendorItemData const* list = vendor ? vendor->GetVendorItems() : nullptr;
            if (!list) continue;
            for (uint32 slot = 0; slot < std::min<uint32>(list->GetItemCount(), 256); ++slot)
            {
                VendorItem const* offer = list->GetItem(slot);
                if (!offer || !ids.count(offer->item) || offers.size() >= 64) continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer->item);
                GearVendorOffer row;
                row.vendorGuid = guid.GetRawValue(); row.vendorEntry = vendor->GetEntry(); row.vendorSlot = slot;
                row.sourceId = "vendor:" + std::to_string(row.vendorEntry) + ":slot:" + std::to_string(slot) +
                    ":item:" + std::to_string(offer->item) + ":cost:" + std::to_string(offer->ExtendedCost);
                row.accountId = snapshot.accountId; row.characterGuid = snapshot.characterGuid;
                row.phaseDigest = ProgressionSystem::GetSharedPhaseSnapshot().policyConfigDigest;
                row.level = player->GetLevel(); row.classId = player->getClass(); row.specTab = AiFactory::GetPlayerSpecTab(player);
                row.maxCount = offer->maxcount; row.incrTime = offer->incrtime;
                row.vendorListToken = reinterpret_cast<uintptr_t>(list); row.itemTemplateToken = reinterpret_cast<uintptr_t>(proto);
                row.session = player->GetSession()->GetPartySessionIncarnation();
                row.generation = StrategicGeneration.load(); row.goalRevision = ai->rpgInfo.pythonGoalRevision;
                row.moneyCopper = player->GetMoney(); row.stock = vendor->GetVendorItemCurrentCount(offer);
                row.reward = ObserveGearReward(player, calculator, "vendor", 0, offer->item, proto ? proto->BuyCount : 0);
                if (ai->HasCheat(BotCheatMask::gold)) row.reason = "gold_cheat_unsupported";
                else if (!proto || offer->ExtendedCost || proto->BuyCount != 1 || proto->GetMaxStackSize() != 1 ||
                    proto->RandomProperty || proto->RandomSuffix || !proto->BuyPrice || proto->BuyPrice > MAX_MONEY_AMOUNT)
                    row.reason = "unsupported_ordinary_equipment_offer";
                else if (offer->maxcount && !row.stock) row.reason = "out_of_stock";
                else if (player->GetItemCount(offer->item, false)) row.reason = "already_owned";
                else
                {
                    row.priceCopper = uint32(std::floor(proto->BuyPrice * player->GetReputationPriceDiscount(vendor)));
                    ConditionList conditions = sConditionMgr->GetConditionsForNpcVendorEvent(row.vendorEntry, offer->item);
                    if (!sConditionMgr->IsObjectMeetToConditions(player, vendor, conditions)) row.reason = "vendor_conditions";
                    else if (row.priceCopper > row.moneyCopper) row.reason = "insufficient_money";
                }
                std::string witness = identityKey + "|" + row.sourceId + "|" + std::to_string(row.vendorGuid) +
                    "|" + std::to_string(row.session) + "|" + std::to_string(row.generation) + "|" +
                    std::to_string(row.goalRevision) + "|" + std::to_string(row.moneyCopper) + "|" +
                    std::to_string(row.priceCopper) + "|" + std::to_string(row.stock) + "|" + row.phaseDigest +
                    "|" + std::to_string(row.vendorListToken) + "|" + std::to_string(row.itemTemplateToken) +
                    "|" + std::to_string(row.maxCount) + "|" + std::to_string(row.incrTime) +
                    "|" + std::to_string(row.level) + "|" + std::to_string(row.classId) + "|" + std::to_string(row.specTab);
                for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
                {
                    row.equipment.push_back({i, CopyGearItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))});
                    witness += "|" + std::to_string(row.equipment.back().item.instanceGuid) + ":" +
                        std::to_string(row.equipment.back().item.entry);
                }
                row.witnessId = Sha256Hex(witness);
                offers.push_back(std::move(row));
            }
        }
        auto previous = _vendorQuotes.find(identityKey);
        bool same = previous != _vendorQuotes.end() && previous->second.size() == offers.size() &&
            std::equal(offers.begin(), offers.end(), previous->second.begin(),
                [](GearVendorOffer const& a, GearVendorOffer const& b) { return a.witnessId == b.witnessId; });
        uint64& revision = _vendorQuoteRevision[identityKey];
        if (!same && ++revision == 0) throw std::runtime_error("vendor quote revision exhausted");
        // Restoring an earlier quote set must never reissue its historical request identity.
        for (auto& offer : offers) offer.requestId = Sha256Hex(offer.witnessId + "|" + std::to_string(revision));
        auto& consumed = _vendorConsumed[identityKey];
        for (auto it = consumed.begin(); it != consumed.end();)
            if (std::none_of(offers.begin(), offers.end(), [&](GearVendorOffer const& row) { return row.requestId == *it; }))
                it = consumed.erase(it);
            else ++it;
        _vendorQuotes[identityKey] = offers;
        return offers;
    }

    VendorPurchaseResult BuyVendorItem(VendorPurchaseRequest const& request) override
    {
        VendorPurchaseResult result; result.requestId = request.requestId; result.item = request.item;
        if (!Hex64(request.identityKey)) { result.reason = "invalid_identity"; return result; }
        if (_vendorAmbiguous.count(request.identityKey)) { result.reason = "vendor_reconciliation_required"; return result; }
        if (_vendorInFlight.count(request.identityKey)) { result.reason = "reentrant_request"; return result; }
        if (!Hex64(request.requestId) || _vendorConsumed[request.identityKey].count(request.requestId))
        { result.reason = "consumed_request"; return result; }
        auto found = _vendorQuotes.find(request.identityKey);
        if (found == _vendorQuotes.end()) { result.reason = "unknown_quote"; return result; }
        auto quote = std::find_if(found->second.begin(), found->second.end(), [&](GearVendorOffer const& row)
            { return row.requestId == request.requestId && row.sourceId == request.sourceId && row.reward.item.entry == request.item; });
        if (quote == found->second.end()) { result.reason = "ineligible_quote"; return result; }
        if (!quote->reason.empty()) { result.reason = quote->reason; return result; }
        GearVendorOffer pinned = *quote;
        auto comparison = std::find_if(pinned.reward.slots.begin(), pinned.reward.slots.end(), [&](GearSlotComparison const& row)
            { return row.slot == request.equipmentSlot; });
        if (comparison == pinned.reward.slots.end() || !comparison->candidateScoreValid ||
            (!comparison->currentScoreValid && comparison->current.entry) ||
            comparison->candidateScore <= (comparison->current.entry ? comparison->currentScore : 0) ||
            pinned.reward.requirementResult || !pinned.reward.classificationAllowed || !pinned.reward.reason.empty())
        { result.reason = "nonpositive_or_unsupported_upgrade"; return result; }
        Player* player = sRandomPlayerbotMgr.GetPlayerBot(pinned.characterGuid);
        if (!player) { result.reason = "offline"; return result; }
        ObjectGuid guid = player->GetGUID();
        PlayerbotAI* originalAI = GET_PLAYERBOT_AI(player);
        auto locate = [=]() -> Player*
        {
            Player* actor = ObjectAccessor::FindPlayer(guid);
            return actor == player && actor->GetSession() &&
                actor->GetSession()->GetAccountId() == pinned.accountId &&
                actor->GetSession()->GetPartySessionIncarnation() == pinned.session ? actor : nullptr;
        };
        auto current = [&, pinned]()
        {
            Player* actor = locate();
            if (!actor || GET_PLAYERBOT_AI(actor) != originalAI || !originalAI ||
                originalAI->HasCheat(BotCheatMask::gold) || StrategicGeneration.load() != pinned.generation ||
                originalAI->rpgInfo.pythonGoalRevision != pinned.goalRevision ||
                !std::holds_alternative<NewRpgInfo::Idle>(originalAI->rpgInfo.data) || originalAI->rpgInfo.pythonQuestGoal ||
                actor->GetLevel() != pinned.level || actor->getClass() != pinned.classId ||
                AiFactory::GetPlayerSpecTab(actor) != pinned.specTab || !StrategicControlReason(actor).empty()) return false;
            auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
            if (phase.policyConfigDigest != pinned.phaseDigest) return false;
            {
                std::lock_guard<std::mutex> lock(ProgressionMutex);
                auto identity = AdmittedProgressionIdentities.find(request.identityKey);
                auto policy = ManagedPolicies.find(pinned.characterGuid);
                if (identity == AdmittedProgressionIdentities.end() || identity->second != pinned.characterGuid ||
                    policy == ManagedPolicies.end() || policy->second.accountId != pinned.accountId ||
                    !CourtesyPolicyFresh(policy->second, phase)) return false;
            }
            for (auto const& equipped : pinned.equipment)
            {
                GearItem now = CopyGearItem(actor->GetItemByPos(INVENTORY_SLOT_BAG_0, equipped.slot));
                if (now.entry != equipped.item.entry || now.instanceGuid != equipped.item.instanceGuid ||
                    now.randomPropertyId != equipped.item.randomPropertyId) return false;
            }
            StatsWeightCalculator calculator(actor);
            calculator.SetItemSetBonus(false); calculator.SetOverflowPenalty(false);
            calculator.SetPvpSpec(sRandomPlayerbotMgr.IsSpecPvp(actor->GetGUID().GetCounter(), actor->getClass()));
            GearReward fresh = ObserveGearReward(actor, calculator, "vendor", 0, request.item, 1);
            auto slot = std::find_if(fresh.slots.begin(), fresh.slots.end(), [&](GearSlotComparison const& row)
                { return row.slot == request.equipmentSlot; });
            return !fresh.requirementResult && fresh.classificationAllowed && fresh.reason.empty() &&
                slot != fresh.slots.end() && slot->candidateScoreValid &&
                slot->candidateScore == comparison->candidateScore && slot->currentScore == comparison->currentScore &&
                (slot->current.entry == 0 || slot->currentScoreValid);
        };
        _vendorInFlight.insert(request.identityKey);
        // Consume before callbacks, including refusal: a replay must obtain a new native witness.
        _vendorConsumed[request.identityKey].insert(request.requestId);
        struct Flight { std::unordered_set<std::string>& set; std::string key; ~Flight() { set.erase(key); } } flight{_vendorInFlight, request.identityKey};
        Player::GuardedVendorRequest native;
        native.vendorGuid = ObjectGuid(pinned.vendorGuid); native.vendorEntry = pinned.vendorEntry;
        native.vendorSlot = pinned.vendorSlot; native.item = request.item; native.price = pinned.priceCopper;
        native.stock = pinned.stock; native.money = pinned.moneyCopper; native.maxCount = pinned.maxCount;
        native.incrTime = pinned.incrTime; native.vendorListToken = pinned.vendorListToken;
        native.itemTemplateToken = pinned.itemTemplateToken; native.locate = locate; native.current = current;
        auto receipt = Player::BuyItemFromVendorGuarded(native);
        result.status = receipt.status; result.reason = receipt.reason; result.stage = receipt.stage;
        result.priceCopper = receipt.price; result.moneyBefore = receipt.moneyBefore; result.moneyAfter = receipt.moneyAfter;
        result.itemBefore = receipt.itemBefore; result.itemAfter = receipt.itemAfter; result.itemGuid = receipt.itemGuid.GetRawValue();
        result.observedMoneyAfter = receipt.observedMoneyAfter; result.observedItemAfter = receipt.observedItemAfter;
        result.observationValid = receipt.observationValid;
        if (receipt.status == "indeterminate_after_effect") _vendorAmbiguous.insert(request.identityKey);
        return result;
    }

    StrategicGoalResult SetQuestGoal(std::string const& identityKey, uint32 questId,
        std::string const& rewardKind, uint32 rewardIndex, uint32 rewardItem) override
    {
        StrategicSnapshot snapshot = ReadQuestSnapshot(identityKey, questId);
        if (!snapshot.controlAllowed)
            return {snapshot.reason == "invalid_identity" || snapshot.reason == "not_admitted" ?
                        "rejected" : "deferred", snapshot.reason, questId};
        Player* player = sRandomPlayerbotMgr.GetPlayerBot(snapshot.characterGuid);
        if (!player || !player->GetSession()) return {"deferred", "offline", questId};
        ObjectGuid actorGuid = player->GetGUID();
        PlayerbotAI* actorAI = GET_PLAYERBOT_AI(player);
        if (!actorAI) return {"deferred", "offline", questId};
        uint64 goalRevision = actorAI->rpgInfo.pythonGoalRevision;
        uint64 session = player->GetSession()->GetPartySessionIncarnation();
        uint64 generation = StrategicGeneration.load();
        Quest const* originalQuest = sObjectMgr->GetQuestTemplate(questId);
        int32 giverEntry = 0;
        WorldPosition giverPosition;
        bool acquiring = snapshot.observedQuestStatus == "none" && !rewardKind.empty();
        std::string reason = acquiring ? FindInactiveQuestGiver(player, questId, giverEntry, giverPosition) :
            StrategicQuestReason(player, questId);
        if (!reason.empty())
            return {"rejected", reason, questId};
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!ValidRewardPreference(quest, rewardKind, rewardIndex, rewardItem))
            return {"rejected", "reward_template_mismatch", questId};
        // Availability checks may run scripts. Resolve the actor again before retaining the goal.
        if (ObjectAccessor::FindPlayer(actorGuid) != player ||
            sRandomPlayerbotMgr.GetPlayerBot(snapshot.characterGuid) != player || !player->GetSession() ||
            player->GetSession()->GetPartySessionIncarnation() != session || GET_PLAYERBOT_AI(player) != actorAI ||
            StrategicGeneration.load() != generation || originalQuest != quest ||
            actorAI->rpgInfo.pythonGoalRevision != goalRevision ||
            player->GetSession()->GetAccountId() != snapshot.accountId ||
            !StrategicControlReason(player).empty()) return {"deferred", "actor_changed", questId};
        {
            auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
            std::lock_guard<std::mutex> lock(ProgressionMutex);
            auto identity = AdmittedProgressionIdentities.find(identityKey);
            auto policy = ManagedPolicies.find(snapshot.characterGuid);
            if (identity == AdmittedProgressionIdentities.end() || identity->second != snapshot.characterGuid ||
                policy == ManagedPolicies.end() || policy->second.accountId != snapshot.accountId ||
                !CourtesyPolicyFresh(policy->second, phase)) return {"deferred", "phase_or_binding_changed", questId};
        }
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        NewRpgInfo& info = ai->rpgInfo;
        auto* goal = std::get_if<NewRpgInfo::DoQuest>(&info.data);
        if (goal && goal->questId == questId && info.pythonQuestGoal &&
            info.pythonGoalGeneration == StrategicGeneration.load() &&
            info.pythonGoalAccountId == snapshot.accountId && goal->identityKey == identityKey &&
            info.pythonGoalSession == player->GetSession()->GetPartySessionIncarnation() &&
            GetMSTimeDiffToNow(info.pythonGoalRefreshedAt) <= ProgressionPolicyLeaseMilliseconds)
        {
            if (goal->questTemplateToken != reinterpret_cast<uintptr_t>(quest))
            {
                info.Reset();
                return {"deferred", "quest_template_changed", questId};
            }
            if (!goal->acquiring && acquiring) return {"deferred", "quest_log_lost", questId};
            if (goal->acquiring && acquiring && (goal->giverEntry != giverEntry || goal->giverPosition != giverPosition))
                return {"deferred", "giver_changed", questId};
            goal->acquiring = acquiring;
            goal->rewardKind = rewardKind;
            goal->rewardIndex = rewardIndex;
            goal->rewardItem = rewardItem;
            info.pythonGoalRefreshedAt = getMSTime();
            info.StartQuestAcquisition(player);
            return {"accepted", "renewed", questId};
        }
        if (!std::holds_alternative<NewRpgInfo::Idle>(info.data))
            return {"deferred", "existing_native_goal", questId};
        info.ChangeToDoQuest(questId, sObjectMgr->GetQuestTemplate(questId));
        goal = std::get_if<NewRpgInfo::DoQuest>(&info.data);
        goal->identityKey = identityKey;
        goal->questTemplateToken = reinterpret_cast<uintptr_t>(quest);
        goal->quest = nullptr;
        goal->acquiring = acquiring;
        goal->giverEntry = giverEntry;
        goal->giverPosition = giverPosition;
        goal->rewardKind = rewardKind;
        goal->rewardIndex = rewardIndex;
        goal->rewardItem = rewardItem;
        info.pythonQuestGoal = true;
        info.pythonGoalAccountId = snapshot.accountId;
        info.pythonGoalSession = player->GetSession()->GetPartySessionIncarnation();
        info.pythonGoalRefreshedAt = getMSTime();
        info.pythonGoalGeneration = StrategicGeneration.load();
        info.StartQuestAcquisition(player);
        return {"accepted", "selected", questId};
    }

    std::vector<StrategicGoalResult> RenewQuestGoals(
        std::vector<StrategicGoalRenewal> const& goals) override
    {
        if (goals.size() > 256)
            throw std::runtime_error("quest renewal batch exceeds 256");
        std::unordered_set<std::string> unique;
        for (StrategicGoalRenewal const& row : goals)
            if (!Hex64(row.identityKey) || !row.questId || !unique.insert(row.identityKey).second)
                throw std::runtime_error("invalid or duplicate quest renewal");
        std::vector<StrategicGoalResult> result;
        result.reserve(goals.size());
        for (StrategicGoalRenewal const& row : goals)
        {
            uint32 guid = 0;
            {
                std::lock_guard<std::mutex> lock(ProgressionMutex);
                auto found = AdmittedProgressionIdentities.find(row.identityKey);
                if (found != AdmittedProgressionIdentities.end())
                    guid = found->second;
            }
            Player* player = guid ? sRandomPlayerbotMgr.GetPlayerBot(guid) : nullptr;
            PlayerbotAI* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
            std::string reason;
            if (!guid)
                reason = "not_admitted";
            else if (!player || !player->IsInWorld() || !ai)
                reason = "offline";
            else
            {
                NewRpgInfo& info = ai->rpgInfo;
                auto const* goal = std::get_if<NewRpgInfo::DoQuest>(&info.data);
                ObjectGuid actorGuid = player->GetGUID();
                uint64 session = player->GetSession() ? player->GetSession()->GetPartySessionIncarnation() : 0;
                if (!info.pythonQuestGoal || !goal || goal->questId != row.questId || goal->identityKey != row.identityKey)
                    reason = "not_owned_goal";
                else if (!info.pythonGoalAccountId || !player->GetSession() ||
                    player->GetSession()->GetAccountId() != info.pythonGoalAccountId ||
                    player->GetSession()->GetPartySessionIncarnation() != info.pythonGoalSession)
                    reason = "account_changed";
                else if (info.pythonGoalGeneration != StrategicGeneration.load())
                    reason = "generation_changed";
                else if (GetMSTimeDiffToNow(info.pythonGoalRefreshedAt) > ProgressionPolicyLeaseMilliseconds)
                    reason = "lease_expired";
                else
                {
                    reason = StrategicControlReason(player);
                    if (reason.empty())
                    {
                        if (goal->acquiring && StrategicQuestStatus(player, row.questId) == "none")
                            reason = info.ValidatePythonQuestAcquire(player) ? "" : "acquire_goal_invalid";
                        else
                        {
                            uint32 rewardIndex = goal->rewardIndex;
                            reason = info.ValidatePythonQuestTurnIn(player, sObjectMgr->GetQuestTemplate(row.questId), rewardIndex) ?
                                "" : "active_goal_invalid";
                        }
                    }
                    if (reason.empty())
                    {
                        if (ObjectAccessor::FindPlayer(actorGuid) != player || !player->GetSession() ||
                            player->GetSession()->GetPartySessionIncarnation() != session || GET_PLAYERBOT_AI(player) != ai)
                            reason = "actor_changed";
                        else
                            ai->rpgInfo.pythonGoalRefreshedAt = getMSTime();
                    }
                }
            }
            result.push_back({reason.empty() ? "accepted" : "deferred",
                              reason.empty() ? "renewed" : reason, row.questId});
        }
        return result;
    }

    OwnerHighWater ReadOwnerHighWater() override
    {
        OwnerHighWater result;
        result.observedEpoch = static_cast<uint64>(GameTime::GetGameTime().count());
        PlayerbotsPopulation::OwnerBindingState state = PlayerbotsPopulation::ReadOwnerBindingState();
        if (!state.available || !state.bound)
            return result;

        ResolvedOwner owner = ResolveOwner(state.accountId, state.characterGuid);
        if (!owner.reason.empty())
            return result;
        if (owner.level > state.maximumOwnerLevel)
            return result;

        if (owner.level > state.highWater)
        {
            PlayerbotsPopulation::OwnerBindingState previous = state;
            AdvanceOwnerHighWater(state, owner.level);
            state = PlayerbotsPopulation::ReadOwnerBindingState();
            if (!state.available || !state.bound ||
                state.accountId != previous.accountId || state.characterGuid != previous.characterGuid ||
                state.maximumOwnerLevel != previous.maximumOwnerLevel || state.globalPhaseCap != previous.globalPhaseCap ||
                state.generation <= previous.generation || state.highWater < owner.level)
                return result;
        }

        uint8 level = std::max(state.highWater, owner.level);
        if (!level || level > state.maximumOwnerLevel || level > 80 ||
            state.currentEpoch != PlayerbotsPopulation::CappedOwnerEpoch(
                state.highWater, state.maximumOwnerLevel, state.globalPhaseCap))
            return result;
        result.characterGuid = state.characterGuid;
        result.level = level;
        result.authenticated = true;
        return result;
    }

    std::vector<EarnedProgress> ReadEarnedSnapshot(
        std::vector<std::string> const& identityKeys) override
    {
        std::vector<EarnedProgress> result;
        result.reserve(identityKeys.size());
        for (std::string const& key : identityKeys)
        {
            if (!Hex64(key))
                continue;
            QueryResult binding = PlayerbotsDatabase.Query(
                "SELECT character_guid, session_requested FROM playerbots_population_identity "
                "WHERE identity_key = '{}' AND admitted = 1 AND character_guid IS NOT NULL", key);
            if (!binding)
                continue;
            Field* bound = binding->Fetch();
            uint32 guid = bound[0].Get<uint32>();
            QueryResult character = CharacterDatabase.Query(
                "SELECT level, xp, totaltime, map, zone FROM characters WHERE guid = {}", guid);
            if (!character)
                continue;
            Field* fields = character->Fetch();
            EarnedProgress row;
            row.identityKey = key;
            row.characterGuid = guid;
            row.level = fields[0].Get<uint8>();
            row.xp = fields[1].Get<uint32>();
            row.playedSeconds = fields[2].Get<uint32>();
            row.mapId = fields[3].Get<uint16>();
            row.zoneId = fields[4].Get<uint16>();
            row.sessionRequested = bound[1].Get<uint8>() != 0;
            row.online = sRandomPlayerbotMgr.GetPlayerBot(guid) != nullptr;
            // A completed failed login has neither a queued holder nor a connected
            // player. Clear only an acknowledged absent session; loading and
            // human-owned sessions remain durable for later reconciliation.
            if (row.sessionRequested && !sRandomPlayerbotMgr.PopulationSessionExists(guid) &&
                ReleaseSession(key))
                row.sessionRequested = false;
            result.push_back(std::move(row));
        }
        return result;
    }

    bool ReserveIdentity(std::string const& identityKey, std::string const& accountKey,
                         std::string const& guidAllocationKey, std::string const& waveKey) override
    {
        if (!Hex64(identityKey) || !Hex64(accountKey) || !Hex64(guidAllocationKey) || !WaveKey(waveKey))
            return false;
        PlayerbotsDatabase.DirectExecute(
            "INSERT IGNORE INTO playerbots_population_identity "
            "(identity_key, account_key, guid_allocation_key, wave_key) VALUES ('{}','{}','{}','{}')",
            identityKey, accountKey, guidAllocationKey, waveKey);
        QueryResult row = PlayerbotsDatabase.Query(
            "SELECT account_key, guid_allocation_key, wave_key FROM playerbots_population_identity "
            "WHERE identity_key = '{}'", identityKey);
        if (!row)
            return false;
        Field* fields = row->Fetch();
        return fields[0].Get<std::string>() == accountKey &&
               fields[1].Get<std::string>() == guidAllocationKey &&
               fields[2].Get<std::string>() == waveKey;
    }

    uint32 EnsureAccount(std::string const& accountKey, std::string const& accountName,
                         uint8 expectedManifestSlots) override
    {
        if (!Hex64(accountKey) || !AccountName(accountName) || !expectedManifestSlots || expectedManifestSlots > 9)
            return 0;
        PlayerbotsDatabase.DirectExecute(
            "INSERT IGNORE INTO playerbots_population_account (account_key, account_name, expected_slots) "
            "VALUES ('{}','{}',{})", accountKey, accountName, expectedManifestSlots);
        QueryResult ledger = PlayerbotsDatabase.Query(
            "SELECT account_name, expected_slots, account_id FROM playerbots_population_account "
            "WHERE account_key = '{}'", accountKey);
        if (!ledger)
            return 0;
        Field* fields = ledger->Fetch();
        if (fields[0].Get<std::string>() != accountName || fields[1].Get<uint8>() != expectedManifestSlots)
            return 0;
        uint32 accountId = fields[2].IsNull() ? 0 : fields[2].Get<uint32>();
        if (!accountId)
        {
            accountId = AccountMgr::GetId(accountName);
            if (!accountId)
            {
                std::string password;
                password.reserve(32);
                for (uint8 index = 0; index < 32; ++index)
                    password.push_back(static_cast<char>(urand('!', '~')));
                if (sAccountMgr->CreateAccount(accountName, password) != AccountOpResult::AOR_OK)
                    return 0;
                accountId = AccountMgr::GetId(accountName);
            }
            if (!accountId)
                return 0;
            PlayerbotsDatabase.DirectExecute(
                "UPDATE playerbots_population_account SET account_id = {} WHERE account_key = '{}' "
                "AND (account_id IS NULL OR account_id = {})", accountId, accountKey, accountId);
            PlayerbotsDatabase.DirectExecute(
                "INSERT INTO playerbots_account_type (account_id, account_type) VALUES ({}, 1) "
                "ON DUPLICATE KEY UPDATE account_type = IF(account_type IN (0,1), 1, account_type)", accountId);
        }
        QueryResult readback = PlayerbotsDatabase.Query(
            "SELECT account_id FROM playerbots_population_account WHERE account_key = '{}'", accountKey);
        return readback && !readback->Fetch()[0].IsNull() ? readback->Fetch()[0].Get<uint32>() : 0;
    }

    uint32 EnsureCharacter(std::string const& identityKey, uint32 accountId,
                           CharacterCreationFields const& fields) override
    {
        if (!Hex64(identityKey) || !accountId)
            return 0;
        QueryResult reservation = PlayerbotsDatabase.Query(
            "SELECT a.account_id, i.character_guid FROM playerbots_population_identity i "
            "JOIN playerbots_population_account a ON a.account_key = i.account_key "
            "WHERE i.identity_key = '{}'", identityKey);
        if (!reservation)
            return 0;
        Field* stored = reservation->Fetch();
        if (stored[0].IsNull() || stored[0].Get<uint32>() != accountId)
            return 0;
        if (!stored[1].IsNull())
            return stored[1].Get<uint32>();
        RandomPlayerbotFactory::ManifestCharacterSpec spec;
        spec.name = fields.characterName;
        spec.race = fields.raceId;
        spec.cls = fields.classId;
        spec.level = fields.initialLevel;
        spec.gender = fields.gender;
        spec.skin = fields.skin;
        spec.face = fields.face;
        spec.hairStyle = fields.hairStyle;
        spec.hairColor = fields.hairColor;
        spec.facialHair = fields.facialHair;
        return RandomPlayerbotFactory::EnsureManifestBot(accountId, spec);
    }

    IdentityBinding BindIdentityOnce(std::string const& identityKey, uint32 accountId,
                                     uint32 characterGuid, std::string const& manifestDigest) override
    {
        IdentityBinding result;
        if (!Hex64(identityKey) || !Hex64(manifestDigest) || !accountId || !characterGuid)
            return result;
        PlayerbotsDatabase.DirectExecute(
            "UPDATE playerbots_population_identity SET account_id = {}, character_guid = {}, "
            "manifest_digest = '{}' WHERE identity_key = '{}' AND "
            "(account_id IS NULL OR account_id = {}) AND "
            "(character_guid IS NULL OR character_guid = {}) AND "
            "(manifest_digest IS NULL OR manifest_digest = '{}')",
            accountId, characterGuid, manifestDigest, identityKey, accountId, characterGuid, manifestDigest);
        QueryResult row = PlayerbotsDatabase.Query(
            "SELECT account_id, character_guid, manifest_digest FROM playerbots_population_identity "
            "WHERE identity_key = '{}'", identityKey);
        if (!row)
            return result;
        Field* fields = row->Fetch();
        if (fields[0].IsNull() || fields[1].IsNull() || fields[2].IsNull() ||
            fields[0].Get<uint32>() != accountId || fields[1].Get<uint32>() != characterGuid ||
            fields[2].Get<std::string>() != manifestDigest)
            return result;
        {
            std::lock_guard<std::mutex> lock(ProgressionMutex);
            ManagedPolicies.try_emplace(characterGuid);
        }
        result.identityKey = identityKey;
        result.accountId = accountId;
        result.characterGuid = characterGuid;
        return result;
    }

    bool SetAdmission(std::string const& identityKey, std::string const& waveKey,
                      std::string const& manifestDigest) override
    {
        if (!Hex64(identityKey) || !Hex64(manifestDigest) || !WaveKey(waveKey))
            return false;
        PlayerbotsDatabase.DirectExecute(
            "UPDATE playerbots_population_identity SET admitted = 1 WHERE identity_key = '{}' "
            "AND wave_key = '{}' AND manifest_digest = '{}' AND account_id IS NOT NULL "
            "AND character_guid IS NOT NULL", identityKey, waveKey, manifestDigest);
        QueryResult row = PlayerbotsDatabase.Query(
            "SELECT admitted, character_guid, account_id FROM playerbots_population_identity WHERE identity_key = '{}' "
            "AND wave_key = '{}' AND manifest_digest = '{}' AND account_id IS NOT NULL "
            "AND character_guid IS NOT NULL", identityKey, waveKey, manifestDigest);
        if (!row || row->Fetch()[0].Get<uint8>() != 1 || row->Fetch()[1].IsNull())
            return false;
        uint32 guid = row->Fetch()[1].Get<uint32>();
        uint32 accountId = row->Fetch()[2].IsNull() ? 0 : row->Fetch()[2].Get<uint32>();
        if (!guid || !accountId)
            return false;
        std::lock_guard<std::mutex> lock(ProgressionMutex);
        auto& durable = ManagedPolicies[guid];
        durable.admitted = true;
        durable.accountId = accountId;
        AdmittedProgressionIdentities[identityKey] = guid;
        return true;
    }

    bool ApplyProgressionPolicy(std::vector<ProgressionPolicy> const& policies) override
    {
        if (policies.size() > 256)
            return false;
        std::vector<std::pair<uint32, ManagedProgressionPolicy>> accepted;
        std::unordered_set<uint32> unique;
        uint32 now = getMSTime();
        PhaseSnapshot phase = ReadPhaseSnapshot();
        if (!phase.enabled || !phase.policyValid || !phase.levelCeiling)
            return false;
        std::lock_guard<std::mutex> lock(ProgressionMutex);
        for (ProgressionPolicy const& policy : policies)
        {
            if (!Hex64(policy.identityKey) || !policy.effectiveCeiling || policy.effectiveCeiling > 80)
                return false;
            auto binding = AdmittedProgressionIdentities.find(policy.identityKey);
            if (binding == AdmittedProgressionIdentities.end())
                return false;
            uint32 guid = binding->second;
            if (!guid || !unique.insert(guid).second)
                return false;
            auto durable = ManagedPolicies.find(guid);
            if (durable == ManagedPolicies.end() || !durable->second.admitted || !durable->second.accountId)
                return false;
            accepted.push_back({guid, {std::min(policy.effectiveCeiling, phase.levelCeiling),
                policy.held, now, policy.schedulerEligible, phase.policyDigest,
                durable->second.admitted, durable->second.accountId}});
        }
        for (auto const& entry : accepted)
            if (ManagedPolicies.find(entry.first) == ManagedPolicies.end())
                return false;
        for (auto const& entry : accepted)
            ManagedPolicies.at(entry.first) = entry.second;
        return true;
    }

    bool RequestSession(std::string const& identityKey, uint64 capacityGeneration) override
    {
        if (!Hex64(identityKey) || capacityGeneration != _capacityGeneration.load(std::memory_order_acquire))
            return false;
        QueryResult row = PlayerbotsDatabase.Query(
            "SELECT character_guid, session_requested FROM playerbots_population_identity "
            "WHERE identity_key = '{}' AND admitted = 1 AND character_guid IS NOT NULL", identityKey);
        if (!row)
            return false;
        Field* fields = row->Fetch();
        bool alreadyRequested = fields[1].Get<uint8>() == 1;
        CapacitySnapshot capacity = ReadCapacitySnapshot();
        if (!capacity.valid || (!alreadyRequested && !capacity.availableSessions))
            return false;
        uint32 guid = fields[0].Get<uint32>();
        if (!sRandomPlayerbotMgr.PopulationRequestSession(guid))
            return false;
        PlayerbotsDatabase.DirectExecute(
            "UPDATE playerbots_population_identity SET session_requested = 1 "
            "WHERE identity_key = '{}' AND admitted = 1", identityKey);
        QueryResult acknowledged = PlayerbotsDatabase.Query(
            "SELECT session_requested FROM playerbots_population_identity "
            "WHERE identity_key = '{}' AND admitted = 1", identityKey);
        return acknowledged && acknowledged->Fetch()[0].Get<uint8>() == 1;
    }

    bool ReleaseSession(std::string const& identityKey) override
    {
        if (!Hex64(identityKey))
            return false;
        QueryResult row = PlayerbotsDatabase.Query(
            "SELECT character_guid FROM playerbots_population_identity WHERE identity_key = '{}' "
            "AND character_guid IS NOT NULL", identityKey);
        if (!row)
            return false;
        if (!sRandomPlayerbotMgr.PopulationReleaseSession(row->Fetch()[0].Get<uint32>()))
            return false;
        PlayerbotsDatabase.DirectExecute(
            "UPDATE playerbots_population_identity SET session_requested = 0 WHERE identity_key = '{}'", identityKey);
        QueryResult acknowledged = PlayerbotsDatabase.Query(
            "SELECT session_requested FROM playerbots_population_identity WHERE identity_key = '{}'", identityKey);
        return acknowledged && acknowledged->Fetch()[0].Get<uint8>() == 0;
    }

    CapacitySnapshot ReadCapacitySnapshot() override
    {
        CapacitySnapshot result;
        result.generation = _capacityGeneration.load(std::memory_order_acquire);
        result.onlineLimit = sConfigMgr->GetOption<uint32>("Python.Population.NativeOnlineLimit", 0);
        result.observedEpoch = static_cast<uint64>(GameTime::GetGameTime().count());
        QueryResult row = PlayerbotsDatabase.Query(
            "SELECT COUNT(*) FROM playerbots_population_identity WHERE session_requested = 1");
        uint32 requested = row ? static_cast<uint32>(row->Fetch()[0].Get<uint64>()) : result.onlineLimit;
        result.availableSessions = requested < result.onlineLimit ? result.onlineLimit - requested : 0;
        result.valid = result.onlineLimit > 0 && row != nullptr;
        return result;
    }

    PhaseSnapshot ReadPhaseSnapshot() override
    {
        ProgressionSystem::SharedPhaseSnapshot raw = ProgressionSystem::GetSharedPhaseSnapshot();
        PhaseSnapshot result;
        result.enabled = raw.enabled;
        result.policyValid = raw.policyValid;
        result.phaseId = raw.phaseId;
        result.levelCeiling = raw.phaseLevelCeiling;
        result.policyDigest = raw.policyConfigDigest;
        return result;
    }

    bool FailClosed() noexcept override
    {
        try
        {
            {
                std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
                for (auto& entry : AcquisitionAttempts) InvalidateAcquisition(entry.second, "strategic_generation_lost");
                AcquisitionCaptureOwners.clear(); // Only live registrations; original journals/pending futures retained.
                StrategicGeneration.fetch_add(1);
            }
            {
                std::lock_guard<std::mutex> lock(ProgressionMutex);
                for (auto& entry : ManagedPolicies)
                {
                    entry.second.ceiling = 0;
                    entry.second.held = true;
                    entry.second.refreshedAt = 0;
                    entry.second.schedulerEligible = false;
                    entry.second.phaseConfigDigest.clear();
                }
            }
            _capacityGeneration.fetch_add(1, std::memory_order_acq_rel);
            if (!_cleanupPending)
            {
                _cleanupAfter.clear();
                _cleanupPending = true;
            }
            // Logout is asynchronous and retryable. The durable ledger remains
            // requested until native release and DB readback both acknowledge it.
            _cleanupNeedsRetry = true;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void DrainRevokedSessions(uint32 diff)
    {
        if (!_cleanupPending)
            return;
        if (diff < _cleanupDelay)
        {
            _cleanupDelay -= diff;
            return;
        }
        _cleanupDelay = 1000;
        try
        {
            QueryResult rows = PlayerbotsDatabase.Query(
                "SELECT identity_key FROM (SELECT identity_key FROM playerbots_population_identity "
                "WHERE session_requested = 1 AND identity_key > '{}' ORDER BY identity_key LIMIT 16) "
                "AS pending UNION ALL SELECT '' ORDER BY identity_key", _cleanupAfter);
            if (!rows)
                return; // DB failure is not evidence of an empty ledger.
            bool found = false;
            uint32 started = getMSTime();
            do
            {
                std::string key = rows->Fetch()[0].Get<std::string>();
                if (key.empty())
                    continue;
                found = true;
                _cleanupAfter = key;
                bool resumed = false;
                {
                    std::lock_guard<std::mutex> lock(ProgressionMutex);
                    auto binding = AdmittedProgressionIdentities.find(key);
                    if (binding != AdmittedProgressionIdentities.end())
                    {
                        auto policy = ManagedPolicies.find(binding->second);
                        resumed = policy != ManagedPolicies.end() && policy->second.refreshedAt &&
                            GetMSTimeDiffToNow(policy->second.refreshedAt) <=
                                ProgressionPolicyLeaseMilliseconds;
                    }
                }
                if (resumed)
                    continue;
                _cleanupNeedsRetry = true;
                ReleaseSession(key); // Refusal leaves the durable row for retry.
                if (GetMSTimeDiffToNow(started) >= 50)
                    break;
            } while (rows->NextRow());
            if (!found)
            {
                if (!_cleanupNeedsRetry)
                    _cleanupPending = false;
                _cleanupNeedsRetry = false;
                _cleanupAfter.clear();
            }
        }
        catch (...)
        {
            // Keep the revoked ledger durable and retry on a later world tick.
        }
    }

private:
    std::atomic<uint64> _capacityGeneration{1};
    bool _cleanupPending{false};
    bool _cleanupNeedsRetry{false};
    uint32 _cleanupDelay{0};
    std::string _cleanupAfter;
};

PlayerbotsPopulationBridge PopulationBridge;

class managed_population_cleanup : public WorldScript
{
public:
    managed_population_cleanup() : WorldScript("managed_population_cleanup") { }
    void OnUpdate(uint32 diff) override
    {
        PopulationBridge.DrainRevokedSessions(diff);
        sScriptMgr->PollQuestRewardMailTransactions();
        // Detect destruction/loss even when no further AI action or player callback can run.
        try
        {
        bool capturesRegistered = false;
        {
            std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
            capturesRegistered = !AcquisitionCaptureOwners.empty();
        }
        auto phase = capturesRegistered ? ProgressionSystem::GetSharedPhaseSnapshot() : ProgressionSystem::SharedPhaseSnapshot{};
        std::set<std::pair<uint32, uint32>> admittedOwners;
        if (capturesRegistered)
        {
            std::lock_guard<std::mutex> policyLock(ProgressionMutex);
            for (auto const& policy : ManagedPolicies)
                if (!policy.second.held && CourtesyPolicyFresh(policy.second, phase))
                    admittedOwners.emplace(policy.first, policy.second.accountId);
        }
        {
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        for (auto it = AcquisitionCaptureOwners.begin(); it != AcquisitionCaptureOwners.end(); )
        {
            auto const& owner = it->second;
            auto* actor = sRandomPlayerbotMgr.GetPlayerBot(it->first);
            bool current = admittedOwners.count({it->first, owner.binding.accountId}) && actor && actor->GetSession() &&
                !actor->GetSession()->isLogingOut() && actor->GetGUID().GetRawValue() == owner.binding.characterGuid &&
                actor->GetSession()->GetAccountId() == owner.binding.accountId &&
                actor->GetSession()->GetPartySessionIncarnation() == owner.binding.sessionIncarnation &&
                StrategicGeneration.load() == owner.binding.strategicGeneration && phase.policyConfigDigest == owner.binding.phaseDigest;
            if (current) { ++it; continue; }
            auto original = AcquisitionAttempts.find(owner.recordId);
            if (original != AcquisitionAttempts.end()) InvalidateAcquisition(original->second, "capture_owner_lifetime_lost");
            if (actor) if (auto* ai = GET_PLAYERBOT_AI(actor); ai && ai->rpgInfo.pythonAcquisitionRecord == owner.recordId)
                ai->rpgInfo.pythonAcquisitionRecord = 0;
            it = AcquisitionCaptureOwners.erase(it); // Not a journal erase or acknowledgement.
        }
        for (auto& entry : AcquisitionAttempts)
        {
            auto& attempt = entry.second;
            if (attempt.terminal) continue;
            auto* player = sRandomPlayerbotMgr.GetPlayerBot(attempt.characterGuid);
            auto* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
            if (!player || !player->GetSession() || !ai || ai->rpgInfo.pythonAcquisitionRecord != attempt.recordId ||
                player->GetSession()->GetPartySessionIncarnation() != attempt.sessionIncarnation ||
                player->GetSession()->GetAccountId() != attempt.accountId || StrategicGeneration.load() != attempt.strategicGeneration)
                InvalidateAcquisition(attempt, "native_actor_or_recorder_lost");
        }
        }
        }
        catch (...) { } // Observation cleanup failure retains original ownership/evidence for the next ordinary tick.
        PlayerbotsCourtesy::Tick(diff);
    }
    void OnAfterConfigLoad(bool /*reload*/) override { PlayerbotsCourtesy::Reload(); }
    void OnShutdown() override { PlayerbotsCourtesy::Cancel(); }
};
} // namespace

namespace
{
bool CourtesyPolicyFresh(ManagedProgressionPolicy const& policy,
                         ProgressionSystem::SharedPhaseSnapshot const& phase)
{
    return policy.admitted && policy.accountId && policy.schedulerEligible && policy.refreshedAt &&
        policy.ceiling && phase.enabled && phase.policyValid && phase.phaseLevelCeiling &&
        policy.phaseConfigDigest == phase.policyConfigDigest && !policy.phaseConfigDigest.empty() &&
        GetMSTimeDiffToNow(policy.refreshedAt) <= ProgressionPolicyLeaseMilliseconds;
}
}

void NewRpgInfo::StartQuestAcquisition(Player* player) noexcept
{
    try
    {
        auto const* goal = std::get_if<DoQuest>(&data);
        if (!pythonQuestGoal || !goal || !player || !player->GetSession() || goal->rewardKind.empty()) return;
        Quest const* quest = sObjectMgr->GetQuestTemplate(goal->questId);
        if (!ValidRewardPreference(quest, goal->rewardKind, goal->rewardIndex, goal->rewardItem)) return;
        std::string captureId;
        {
            std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
            auto owner = AcquisitionCaptureOwners.find(player->GetGUID().GetCounter());
            if (owner == AcquisitionCaptureOwners.end() || owner->second.recordId) return;
            captureId = owner->second.binding.captureId; // No capture-owned allocation before this explicit owner gate.
        }
        auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
        std::unique_lock<std::recursive_mutex> journal(AcquisitionMutex);
        auto captureCurrent = [&]()
        {
            auto owner = AcquisitionCaptureOwners.find(player->GetGUID().GetCounter());
            if (owner == AcquisitionCaptureOwners.end() || owner->second.recordId || owner->second.binding.captureId != captureId) return false;
            auto const& binding = owner->second.binding;
            return owner->second.identityKey == goal->identityKey && binding.accountId == pythonGoalAccountId &&
                binding.characterGuid == player->GetGUID().GetRawValue() && binding.sessionIncarnation == pythonGoalSession &&
                binding.sessionIncarnation == player->GetSession()->GetPartySessionIncarnation() &&
                binding.strategicGeneration == pythonGoalGeneration && binding.strategicGeneration == StrategicGeneration.load() &&
                binding.phaseDigest == phase.policyConfigDigest && binding.questId == goal->questId &&
                binding.rewardKind == goal->rewardKind && binding.rewardIndex == goal->rewardIndex && binding.itemEntry == goal->rewardItem &&
                binding.rewardCount == (goal->rewardKind == "choice" ? quest->RewardChoiceItemCount[goal->rewardIndex] : quest->RewardItemIdCount[goal->rewardIndex]);
        };
        if (!captureCurrent()) return;
        auto found = AcquisitionAttempts.find(pythonAcquisitionRecord);
        if (found != AcquisitionAttempts.end())
        {
            auto const& previous = found->second;
            if (!previous.terminal && !previous.actionTerminal && previous.identityKey == goal->identityKey && previous.questId == goal->questId &&
                previous.rewardKind == goal->rewardKind && previous.rewardIndex == goal->rewardIndex && previous.itemEntry == goal->rewardItem &&
                previous.goalRevision == pythonGoalRevision && previous.strategicGeneration == pythonGoalGeneration &&
                previous.sessionIncarnation == pythonGoalSession && previous.phaseDigest == phase.policyConfigDigest) return;
            journal.unlock();
            CloseQuestAcquisition("goal_replaced");
            journal.lock();
            if (!captureCurrent()) return;
        }
        QuestAcquisitionAttempt attempt;
        attempt.recordId = ++AcquisitionRecordSequence; attempt.identityKey = goal->identityKey;
        attempt.accountId = pythonGoalAccountId; attempt.characterGuid = player->GetGUID().GetRawValue();
        attempt.sessionIncarnation = pythonGoalSession; attempt.strategicGeneration = pythonGoalGeneration;
        attempt.goalRevision = pythonGoalRevision; attempt.phaseDigest = phase.policyConfigDigest;
        attempt.questId = goal->questId; attempt.rewardKind = goal->rewardKind; attempt.rewardIndex = goal->rewardIndex;
        attempt.itemEntry = goal->rewardItem;
        attempt.rewardCount = goal->rewardKind == "choice" ? quest->RewardChoiceItemCount[goal->rewardIndex] : quest->RewardItemIdCount[goal->rewardIndex];
        attempt.sourceKey = "quest:" + std::to_string(goal->questId) + ":" + goal->rewardKind + ":" +
            std::to_string(goal->rewardIndex) + ":" + std::to_string(goal->rewardItem);
        AppendAcquisitionEvent(attempt, "accepted", player->GetMapId(), goal->giverPosition);
        for (auto const& name : {"CanTakeQuest", "CanAddQuest"})
            attempt.predicates.push_back({name, "SetQuestGoal.FindInactiveQuestGiver.actor_session_goal_phase",
                goal->acquiring ? "actual_successful_native_call;revalidate_before_accept" : "active_goal_not_evaluated",
                attempt.events.back().eventSequence, attempt.events.back().monotonicTicks, goal->acquiring, goal->acquiring});
        attempt.predicates.push_back({"native_script_dispatch_semantics", "source_global", "actual_enabled_dispatch_names_in_catalogue;handler_bodies_not_inferred;actor_result_not_universal",
            0, 0, false, false});
        attempt.predicates.push_back({"native_attempt_lifecycle", "ordinary_NewRpg_quest_same_map_walking_lifecycle",
            "accepted_goal_to_observed_native_terminal;all_other_reset_control_source_or_session_loss_censored;death_counts_without_forced_terminal",
            attempt.events.back().eventSequence, attempt.events.back().monotonicTicks, true, true});
        pythonAcquisitionRecord = attempt.recordId;
        AcquisitionAttempts.emplace(attempt.recordId, std::move(attempt));
        AcquisitionCaptureOwners.at(player->GetGUID().GetCounter()).recordId = pythonAcquisitionRecord;
        auto context = sScriptMgr->ReadQuestLifecycleContext();
        ActiveAcquisitionRecord active;
        active.recordId = pythonAcquisitionRecord;
        active.admittedDispatchSequence = context.active ? context.dispatchSequence : 0;
        AcquisitionActiveRecords[player->GetGUID().GetCounter()] = std::move(active);
        journal.unlock();
        // Include a movement already in progress at admission, and validate the initial supported context.
        ObserveQuestAcquisition(player, "movement_sample", WorldPosition(player), player->isMoving());
    }
    catch (...) { MarkAcquisitionObserverFailure(pythonAcquisitionRecord); }
}

void NewRpgInfo::CloseQuestAcquisition(std::string_view reason) noexcept
{
    try
    {
        // If a preceding native listener clears the goal inside a death dispatch, retain its known original deaths first.
        auto context = sScriptMgr->ReadQuestLifecycleContext();
        if (context.active)
        {
            auto* fresh = ResolveAcquisitionCallbackActor(ObjectAccessor::FindPlayerByLowGUID(context.actorGuid), context.hookId);
            if (fresh) if (auto* ai = GET_PLAYERBOT_AI(fresh); ai && &ai->rpgInfo == this)
                ObserveQuestAcquisition(fresh, "movement_sample", WorldPosition(fresh), fresh->isMoving());
        }
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto found = AcquisitionAttempts.find(pythonAcquisitionRecord);
        if (found != AcquisitionAttempts.end())
        {
            if (!found->second.terminal && (reason == "logout" || reason == "goal_reset_or_lost"))
                AppendAcquisitionEvent(found->second, reason == "logout" ? "logout" : "reset", 0, {}, false, 0, 0, 0, reason, 0,
                    found->second.actionTerminal ? "delivery_after_action" : "action_window");
            InvalidateAcquisition(found->second, reason);
            auto active = AcquisitionActiveRecords.find(uint32(found->second.characterGuid));
            if (active != AcquisitionActiveRecords.end() && active->second.recordId == found->second.recordId) AcquisitionActiveRecords.erase(active);
        }
        pythonAcquisitionRecord = 0;
    }
    catch (...) { MarkAcquisitionObserverFailure(pythonAcquisitionRecord); pythonAcquisitionRecord = 0; }
}

namespace
{
bool AcquisitionOwnerCurrent(NewRpgInfo const& info, Player* player, AcquisitionBinding const& attempt, std::string_view kind)
{
        auto const* goal = std::get_if<NewRpgInfo::DoQuest>(&info.data);
        auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
        bool current = player && player->GetSession() && GET_PLAYERBOT_AI(player) &&
            &GET_PLAYERBOT_AI(player)->rpgInfo == &info && ObjectAccessor::FindPlayer(player->GetGUID()) == player &&
            info.pythonQuestGoal && goal && goal->identityKey == attempt.identityKey && goal->questId == attempt.questId &&
            goal->rewardKind == attempt.rewardKind && goal->rewardIndex == attempt.rewardIndex && goal->rewardItem == attempt.itemEntry &&
            goal->questTemplateToken == reinterpret_cast<uintptr_t>(sObjectMgr->GetQuestTemplate(goal->questId)) &&
            player->GetGUID().GetRawValue() == attempt.characterGuid && info.pythonGoalAccountId == attempt.accountId &&
            player->GetSession()->GetAccountId() == attempt.accountId && info.pythonGoalSession == attempt.sessionIncarnation &&
            player->GetSession()->GetPartySessionIncarnation() == attempt.sessionIncarnation &&
            info.pythonGoalRevision == attempt.goalRevision && info.pythonGoalGeneration == attempt.strategicGeneration &&
            StrategicGeneration.load() == attempt.strategicGeneration && phase.enabled && phase.policyValid &&
            phase.policyConfigDigest == attempt.phaseDigest &&
            GetMSTimeDiffToNow(info.pythonGoalRefreshedAt) <= ProgressionPolicyLeaseMilliseconds;
        if (current)
        {
            Quest const* source = sObjectMgr->GetQuestTemplate(attempt.questId);
            current = ValidRewardPreference(source, attempt.rewardKind, attempt.rewardIndex, attempt.itemEntry) &&
                attempt.rewardCount == (attempt.rewardKind == "choice" ? source->RewardChoiceItemCount[attempt.rewardIndex] :
                    source->RewardItemIdCount[attempt.rewardIndex]);
            // Dead/combat can be observed while the ordinary attempt continues, or when its validator actually ends it.
            // All ownership/activity/policy changes instead censor the window; a native death itself is never a terminal.
            auto control = StrategicControlReason(player);
            current = current && (kind == "unmeasured_travel" || ((control.empty() || control == "dead" || control == "combat") &&
                !player->IsBeingTeleported() && !player->IsInFlight() && !player->GetTransport() &&
                !player->GetMap()->IsDungeon() && !player->InBattleground()));
        }
        if (current)
        {
            std::lock_guard<std::mutex> lock(ProgressionMutex);
            auto identity = AdmittedProgressionIdentities.find(attempt.identityKey);
            auto policy = ManagedPolicies.find(player->GetGUID().GetCounter());
            current = identity != AdmittedProgressionIdentities.end() && identity->second == player->GetGUID().GetCounter() &&
                policy != ManagedPolicies.end() && policy->second.accountId == attempt.accountId && !policy->second.held && CourtesyPolicyFresh(policy->second, phase);
        }
        return current;
}
}

void NewRpgInfo::ObserveQuestAcquisition(Player* player, std::string_view kind,
    WorldPosition const& target, bool outcome, uint64 itemGuid, uint32 itemEntry, uint32 count, std::string_view reason) noexcept
{
    try
    {
        uint64 const recordId = pythonAcquisitionRecord;
        if (!recordId) return;
        AcquisitionBinding binding;
        {
            std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
            auto found = AcquisitionAttempts.find(recordId);
            if (found == AcquisitionAttempts.end() || found->second.terminal || found->second.actionTerminal) return;
            auto const& value = found->second;
            binding = {value.identityKey, value.rewardKind, value.phaseDigest, value.accountId, value.questId, value.rewardIndex,
                value.itemEntry, value.rewardCount, value.characterGuid, value.sessionIncarnation, value.goalRevision, value.strategicGeneration};
        }
        bool const current = AcquisitionOwnerCurrent(*this, player, binding, kind);
        std::vector<NativeQuestLifecycleContext> deaths;
        for (uint32 depth = 0; ; ++depth)
        {
            auto context = sScriptMgr->ReadQuestLifecycleContext(depth);
            if (!context.dispatchSequence) break;
            if (context.active && context.hookId == PLAYERHOOK_ON_PLAYER_JUST_DIED && context.actorGuid == binding.characterGuid &&
                context.accountId == binding.accountId && context.sessionIncarnation == binding.sessionIncarnation) deaths.push_back(context);
        }
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto found = AcquisitionAttempts.find(recordId);
        if (found == AcquisitionAttempts.end() || found->second.terminal || found->second.actionTerminal) return;
        auto& attempt = found->second;
        if (pythonAcquisitionRecord != recordId) { InvalidateAcquisition(attempt, "recorder_rebound_during_observation"); return; }
        if (!current) { InvalidateAcquisition(attempt, "actor_session_goal_or_phase_lost"); return; }
        auto active = AcquisitionActiveRecords.find(uint32(attempt.characterGuid));
        if (active == AcquisitionActiveRecords.end() || active->second.recordId != recordId)
            { InvalidateAcquisition(attempt, "active_record_index_lost"); return; }
        // A preceding listener may terminate this attempt inside a nested death callback.
        // Preserve every genuine original-entry death before that terminal, never credit an older event to a new record.
        bool boundDeath = false;
        for (auto context = deaths.rbegin(); context != deaths.rend(); ++context)
        {
            if (context->dispatchSequence <= active->second.admittedDispatchSequence) continue;
            boundDeath = true;
            if (!active->second.observedDeathDispatches.insert(context->dispatchSequence).second) continue;
            AppendAcquisitionEvent(attempt, "death", player->GetMapId(), {}, false, 0, 0, 0,
                "native_death_dispatch:" + std::to_string(context->dispatchSequence));
            if (attempt.moving) AppendAcquisitionEvent(attempt, "move_end", player->GetMapId());
            attempt.moving = false;
        }
        if (kind == "death")
        {
            if (!boundDeath) InvalidateAcquisition(attempt, "death_dispatch_original_binding_unavailable");
            return; // Already appended/deduplicated from the genuine value-bound native dispatch above.
        }
        if (kind == "unmeasured_travel")
        {
            AppendAcquisitionEvent(attempt, kind, player->GetMapId(), target, false, 0, 0, 0, reason);
            attempt.movementCoverage = false;
            InvalidateAcquisition(attempt, reason);
            return;
        }
        if (kind == "movement_sample")
        {
            if (attempt.moving == outcome) return;
            attempt.moving = outcome;
            AppendAcquisitionEvent(attempt, outcome ? "move_begin" : "move_end", player->GetMapId(), target);
            return;
        }
        if ((kind == "item_stored" || kind == "item_hook_instance_unavailable" || kind == "reward_destinations" || kind == "quest_terminal") && !attempt.turnInFlight) return;
        if (kind == "turnin_begin")
        {
            if (attempt.turnInFlight) { InvalidateAcquisition(attempt, "nested_turnin"); return; }
            attempt.turnInFlight = true;
        }
        if (kind == "quest_terminal") attempt.completeQuestObserved = true;
        AppendAcquisitionEvent(attempt, kind, player->GetMapId(), target, outcome, itemGuid, itemEntry, count,
            kind == "item_stored" ? "returned_instance_only;slot_and_all_destinations_not_attributed" : reason);
        if (kind == "CanTakeQuest" || kind == "CanAddQuest" || kind == "CanRewardQuest")
            attempt.predicates.push_back({std::string(kind), "ordinary_native_action.actor_session_goal_phase",
                "actual_native_result;action_revalidation_required;not_universal_source_truth",
                attempt.events.back().eventSequence, attempt.events.back().monotonicTicks, true, outcome});
        if (kind == "quest_failed" || kind == "attempt_failed" || kind == "quest_abandoned")
        {
            if (attempt.moving) AppendAcquisitionEvent(attempt, "move_end", player->GetMapId(), target);
            attempt.moving = false; attempt.turnInFlight = false;
            attempt.actionTerminal = true; attempt.validAttempt = attempt.validWindow;
            auto active = AcquisitionActiveRecords.find(uint32(attempt.characterGuid));
            if (active != AcquisitionActiveRecords.end() && active->second.recordId == attempt.recordId) AcquisitionActiveRecords.erase(active);
            attempt.terminalOutcome = kind;
            attempt.failureTerminalCoverage = attempt.validWindow && !attempt.observerFailure;
            AppendAcquisitionEvent(attempt, "attempt_terminal", player->GetMapId(), {}, false, 0, 0, 0, kind);
            attempt.terminal = attempt.pendingDeliveries == 0;
        }
    }
    catch (...) { MarkAcquisitionObserverFailure(pythonAcquisitionRecord); }
}

void NewRpgInfo::InvalidateQuestAcquisitionRecord(uint64 recordId, std::string_view reason) noexcept
{
    try
    {
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto found = AcquisitionAttempts.find(recordId);
        if (found != AcquisitionAttempts.end()) InvalidateAcquisition(found->second, reason);
    }
    catch (...) { MarkAcquisitionObserverFailure(recordId); }
}

void NewRpgInfo::EndQuestAcquisitionTurnIn(uint64 recordId, bool ownerCurrent) noexcept
{
    try
    {
        std::lock_guard<std::recursive_mutex> journal(AcquisitionMutex);
        auto found = AcquisitionAttempts.find(recordId);
        if (found == AcquisitionAttempts.end() || found->second.terminal) return;
        auto& attempt = found->second;
        if (!ownerCurrent || attempt.strategicGeneration != StrategicGeneration.load())
            { InvalidateAcquisition(attempt, "turnin_owner_changed"); return; }
        if (!attempt.turnInFlight) return;
        if (attempt.moving) AppendAcquisitionEvent(attempt, "move_end", 0);
        attempt.moving = false; attempt.turnInFlight = false;
        attempt.actionTerminal = true; attempt.validAttempt = attempt.validWindow;
        auto active = AcquisitionActiveRecords.find(uint32(attempt.characterGuid));
        if (active != AcquisitionActiveRecords.end() && active->second.recordId == attempt.recordId) AcquisitionActiveRecords.erase(active);
        attempt.terminalOutcome = attempt.completeQuestObserved ? "reward_quest_completed" : "turnin_not_confirmed";
        attempt.failureTerminalCoverage = attempt.validWindow && !attempt.observerFailure;
        AppendAcquisitionEvent(attempt, "turnin_end", 0, {}, attempt.completeQuestObserved, 0, 0, 0, attempt.terminalOutcome);
        attempt.terminal = attempt.pendingDeliveries == 0;
    }
    catch (...) { MarkAcquisitionObserverFailure(recordId); }
}

bool NewRpgInfo::ValidatePythonQuestTurnIn(Player* player, Quest const* quest, uint32& rewardIndex)
{
    if (!pythonQuestGoal) return true;
    auto const* goal = std::get_if<DoQuest>(&data);
    bool valid = player && quest && goal && goal->questId == quest->GetQuestId() &&
        goal->questTemplateToken == reinterpret_cast<uintptr_t>(quest) &&
        quest == sObjectMgr->GetQuestTemplate(goal->questId) &&
        player->GetSession() && pythonGoalAccountId &&
        player->GetSession()->GetAccountId() == pythonGoalAccountId &&
        player->GetSession()->GetPartySessionIncarnation() == pythonGoalSession &&
        pythonGoalGeneration == StrategicGeneration.load() &&
        GetMSTimeDiffToNow(pythonGoalRefreshedAt) <= ProgressionPolicyLeaseMilliseconds &&
        StrategicControlReason(player).empty() && StrategicQuestReason(player, goal->questId).empty();
    if (valid)
    {
        auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
        std::lock_guard<std::mutex> lock(ProgressionMutex);
        auto identity = AdmittedProgressionIdentities.find(goal->identityKey);
        auto policy = ManagedPolicies.find(player->GetGUID().GetCounter());
        valid = identity != AdmittedProgressionIdentities.end() && identity->second == player->GetGUID().GetCounter() &&
            policy != ManagedPolicies.end() && policy->second.accountId == pythonGoalAccountId &&
            CourtesyPolicyFresh(policy->second, phase);
    }
    if (valid)
    {
        valid = ValidRewardPreference(quest, goal->rewardKind, goal->rewardIndex, goal->rewardItem);
        valid = valid && (goal->rewardKind != "choice" || rewardIndex == goal->rewardIndex) &&
            !player->GetQuestRewardStatus(goal->questId);
    }
    if (!valid)
    {
        // Preserve the ordinary validator's reset, but distinguish its witnessed failure from context loss.
        // Observe revalidates the entire original owner/source/count/phase before accepting a terminal.
        try
        {
        if (pythonAcquisitionRecord && player && player->GetSession() && goal && quest && quest == sObjectMgr->GetQuestTemplate(goal->questId) &&
            goal->questTemplateToken == reinterpret_cast<uintptr_t>(quest) &&
            ValidRewardPreference(quest, goal->rewardKind, goal->rewardIndex, goal->rewardItem) &&
            (goal->rewardKind != "choice" || rewardIndex == goal->rewardIndex) && !player->GetQuestRewardStatus(goal->questId))
        {
            auto status = player->GetQuestStatus(goal->questId);
            auto control = StrategicControlReason(player);
            auto refusal = StrategicQuestReason(player, goal->questId);
            if (status == QUEST_STATUS_FAILED)
                ObserveQuestAcquisition(player, "quest_failed", {}, false, 0, 0, 0, "native_quest_status_failed_before_validator_reset");
            else if (control == "dead" || control == "combat" ||
                (control.empty() && (refusal == "native_failed_quest" || refusal == "no_quest_poi" || refusal == "not_active_quest")))
                ObserveQuestAcquisition(player, "attempt_failed", {}, false, 0, 0, 0,
                    control.empty() ? std::string_view(refusal) : std::string_view(control));
        }
        }
        catch (...) { MarkAcquisitionObserverFailure(pythonAcquisitionRecord); }
        Reset();
    }
    return valid;
}

bool NewRpgInfo::ValidatePythonQuestAcquire(Player* player, ObjectGuid giver)
{
    auto const* stored = std::get_if<DoQuest>(&data);
    if (!pythonQuestGoal || !stored || !stored->acquiring || !player || !player->GetSession()) return false;
    DoQuest goal = *stored;
    ObjectGuid actorGuid = player->GetGUID();
    uint64 session = pythonGoalSession;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    uint64 generation = pythonGoalGeneration;
    uint64 revision = pythonGoalRevision;
    auto current = [&]()
    {
        if (ObjectAccessor::FindPlayer(actorGuid) != player || !player->GetSession() ||
            player->GetSession()->GetPartySessionIncarnation() != session || GET_PLAYERBOT_AI(player) != ai ||
            !ai || &ai->rpgInfo != this) return false;
        auto const* live = std::get_if<DoQuest>(&data);
        bool valid = pythonGoalRevision == revision && pythonQuestGoal && live && live->acquiring && live->questId == goal.questId &&
            live->identityKey == goal.identityKey && live->rewardKind == goal.rewardKind &&
            live->rewardIndex == goal.rewardIndex && live->rewardItem == goal.rewardItem &&
            live->giverEntry == goal.giverEntry && live->giverPosition == goal.giverPosition &&
            pythonGoalGeneration == generation && generation == StrategicGeneration.load() &&
            pythonGoalAccountId && player->GetSession()->GetAccountId() == pythonGoalAccountId &&
            GetMSTimeDiffToNow(pythonGoalRefreshedAt) <= ProgressionPolicyLeaseMilliseconds &&
            reinterpret_cast<uintptr_t>(sObjectMgr->GetQuestTemplate(goal.questId)) == goal.questTemplateToken &&
            ValidRewardPreference(sObjectMgr->GetQuestTemplate(goal.questId), goal.rewardKind, goal.rewardIndex, goal.rewardItem) &&
            !goal.rewardKind.empty() && StrategicControlReason(player).empty();
        if (!valid) return false;
        auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
        std::lock_guard<std::mutex> lock(ProgressionMutex);
        auto identity = AdmittedProgressionIdentities.find(goal.identityKey);
        auto policy = ManagedPolicies.find(player->GetGUID().GetCounter());
        return identity != AdmittedProgressionIdentities.end() && identity->second == player->GetGUID().GetCounter() &&
            policy != ManagedPolicies.end() && policy->second.accountId == pythonGoalAccountId &&
            CourtesyPolicyFresh(policy->second, phase);
    };
    if (!current()) return false;
    int32 entry = goal.giverEntry;
    WorldPosition position = goal.giverPosition;
    auto refusal = FindInactiveQuestGiver(player, goal.questId, entry, position);
    if (!refusal.empty() || !current())
    {
        if (pythonAcquisitionRecord) try
        {
            if (current()) ObserveQuestAcquisition(player, "acquire_refused", {}, false, 0, 0, 0, refusal);
        }
        catch (...) { MarkAcquisitionObserverFailure(pythonAcquisitionRecord); }
        return false;
    }
    if (giver)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*player, giver);
        if (!object || (entry > 0 ? object->GetTypeId() != TYPEID_UNIT : object->GetTypeId() != TYPEID_GAMEOBJECT) ||
            object->GetEntry() != uint32(entry > 0 ? entry : -entry) || object->GetMapId() != position.GetMapId()) return false;
        WorldPosition actual(object);
        if (actual.distance(&position) > 10.0f) return false;
        bool interact = player->CanInteractWithQuestGiver(object);
        if (!current() || !interact) return false;
    }
    return true;
}

std::vector<PlayerbotsPopulation::CourtesyCandidate> PlayerbotsPopulation::ReadCourtesyCandidates(
    uint32& cursor, uint32 scanBudget, uint32 resultBudget, uint32* scanned, bool* exhausted)
{
    if (scanned) *scanned = 0;
    if (exhausted) *exhausted = false;
    std::vector<CourtesyCandidate> result;
    if (!scanBudget || scanBudget > 64 || !resultBudget || resultBudget > 8)
        return result;
    auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
    std::lock_guard<std::mutex> lock(ProgressionMutex);
    if (ManagedPolicies.empty())
        return result;
    result.reserve(resultBudget);
    auto current = ManagedPolicies.upper_bound(cursor);
    if (current == ManagedPolicies.end()) current = ManagedPolicies.begin();
    uint32 first = current->first;
    for (uint32 examined = 0; examined < scanBudget; ++examined)
    {
        cursor = current->first;
        if (scanned) *scanned = examined + 1;
        if (CourtesyPolicyFresh(current->second, phase))
            result.push_back({current->first});
        if (++current == ManagedPolicies.end()) current = ManagedPolicies.begin();
        if (current->first == first) break;
        if (examined + 1 == scanBudget || result.size() == resultBudget)
        {
            if (exhausted) *exhausted = true;
            break;
        }
    }
    return result;
}

bool PlayerbotsPopulation::CourtesyEligible(uint32 characterGuid)
{
    auto phase = ProgressionSystem::GetSharedPhaseSnapshot();
    uint32 accountId = 0;
    {
        std::lock_guard<std::mutex> lock(ProgressionMutex);
        auto found = ManagedPolicies.find(characterGuid);
        if (found == ManagedPolicies.end() || !CourtesyPolicyFresh(found->second, phase)) return false;
        accountId = found->second.accountId;
    }
    Player* player = sRandomPlayerbotMgr.GetPlayerBot(characterGuid);
    return player && player->IsInWorld() && player->GetSession() &&
        player->GetSession()->GetAccountId() == accountId;
}

PlayerbotsPopulation::OwnerBindingState PlayerbotsPopulation::ReadOwnerBindingState()
{
    OwnerBindingState result;
    if (!HasV2OwnerLedger())
    {
        result.reason = "v2_owner_ledger_unavailable";
        return result;
    }

    std::string realmId = PopulationRealmId();
    QueryResult row = PlayerbotsDatabase.Query(
        "SELECT o.binding_state, o.owner_account_id, o.owner_character_guid, "
        "o.highest_owner_level_seen, o.current_epoch, o.generation, "
        "s.maximum_owner_level, s.global_phase_cap "
        "FROM population_director_owner_state o "
        "JOIN population_director_schedule s ON s.realm_id = o.realm_id "
        "WHERE o.realm_id = '{}'",
        realmId);
    if (!row)
    {
        result.reason = "owner_state_not_installed_for_realm";
        return result;
    }

    result.available = true;
    Field* fields = row->Fetch();
    std::string bindingState = fields[0].Get<std::string>();
    bool hasAccount = !fields[1].IsNull();
    bool hasCharacter = !fields[2].IsNull();
    result.highWater = fields[3].Get<uint8>();
    result.currentEpoch = fields[4].Get<uint8>();
    result.generation = fields[5].Get<uint64>();
    uint8 maximumOwnerLevel = fields[6].Get<uint8>();
    uint8 globalPhaseCap = fields[7].Get<uint8>();
    result.maximumOwnerLevel = maximumOwnerLevel;
    result.globalPhaseCap = globalPhaseCap;

    if ((hasAccount != hasCharacter) || (bindingState != "unbound" && bindingState != "bound") ||
        result.generation == 0 || !result.highWater || result.highWater > 80 ||
        !maximumOwnerLevel || maximumOwnerLevel > 80 || !globalPhaseCap || globalPhaseCap > 80 ||
        result.highWater > maximumOwnerLevel ||
        result.currentEpoch != PlayerbotsPopulation::CappedOwnerEpoch(
            result.highWater, maximumOwnerLevel, globalPhaseCap))
    {
        result.reason = "owner_state_invalid";
        return result;
    }

    if (bindingState == "unbound")
    {
        if (hasAccount || hasCharacter)
        {
            result.reason = "owner_state_invalid_unbound_pair";
            return result;
        }
        result.reason = "unbound";
        return result;
    }

    if (!hasAccount || !hasCharacter)
    {
        result.reason = "owner_state_invalid_bound_pair";
        return result;
    }
    result.accountId = fields[1].Get<uint32>();
    result.characterGuid = fields[2].Get<uint32>();
    if (!result.accountId || !result.characterGuid)
    {
        result.reason = "owner_state_invalid_bound_pair";
        return result;
    }
    result.bound = true;
    result.reason = "bound";
    return result;
}

PlayerbotsPopulation::OwnerBindingState PlayerbotsPopulation::BindOwner(Player* player)
{
    OwnerBindingState result = ReadOwnerBindingState();
    if (!result.available || (result.reason != "bound" && result.reason != "unbound"))
        return result;
    if (!player || !player->IsInWorld() || !player->GetSession() || player->GetSession()->IsBot())
    {
        result.reason = "owner_session_is_not_human";
        return result;
    }
    if (sRandomPlayerbotMgr.IsRandomBot(player))
    {
        result.reason = "owner_account_is_autonomous";
        return result;
    }

    uint32 accountId = player->GetSession()->GetAccountId();
    uint32 characterGuid = player->GetGUID().GetCounter();
    if (result.bound)
    {
        if (result.accountId != accountId || result.characterGuid != characterGuid)
        {
            result.reason = "owner_already_bound";
            return result;
        }

        ResolvedOwner owner = ResolveOwner(accountId, characterGuid);
        if (!owner.reason.empty())
        {
            result.reason = owner.reason;
            return result;
        }
        result.committed = true;
        result.alreadyBound = true;
        result.reason = "already_bound";
        return result;
    }

    ResolvedOwner owner = ResolveOwner(accountId, characterGuid);
    if (!owner.reason.empty())
    {
        result.reason = owner.reason;
        return result;
    }

    std::string realmId = PopulationRealmId();
    std::string actor = player->GetName();
    PlayerbotsDatabase.EscapeString(actor);
    std::string evidenceDigest = Sha256Hex(
        "owner-resolution-v2|" + realmId + "|" + std::to_string(accountId) + "|" +
        std::to_string(characterGuid) + "|" + std::to_string(owner.level) + "|" + actor);
    uint64 nextGeneration = result.generation + 1;
    std::string eventKey = Sha256Hex(
        "owner-bind-v2|" + realmId + "|" + std::to_string(result.generation) + "|" +
        std::to_string(accountId) + "|" + std::to_string(characterGuid));
    std::string transitionDigest = Sha256Hex(
        "owner-bind-transition-v2|" + eventKey + "|" + evidenceDigest + "|" +
        std::to_string(nextGeneration));

    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    transaction->Append(
        "UPDATE population_director_owner_state SET binding_state = 'bound', "
        "owner_account_id = {}, owner_character_guid = {}, generation = generation + 1, "
        "last_reconciled_at = UTC_TIMESTAMP(6) WHERE realm_id = '{}' AND binding_state = 'unbound' "
        "AND owner_account_id IS NULL AND owner_character_guid IS NULL AND generation = {}",
        accountId, characterGuid, realmId, result.generation);
    transaction->Append(
        "INSERT INTO population_director_owner_binding_audit "
        "(realm_id, event_key, operation, expected_generation, next_generation, "
        "previous_account_id, previous_character_guid, selected_account_id, selected_character_guid, "
        "actor, selected_at_utc, resolution_evidence_digest, transition_digest) "
        "SELECT '{}', '{}', 'bind', {}, {}, NULL, NULL, {}, {}, '{}', UTC_TIMESTAMP(6), '{}', '{}' "
        "FROM population_director_owner_state WHERE realm_id = '{}' AND binding_state = 'bound' "
        "AND owner_account_id = {} AND owner_character_guid = {} AND generation = {}",
        realmId, eventKey, result.generation, nextGeneration, accountId, characterGuid, actor,
        evidenceDigest, transitionDigest, realmId, accountId, characterGuid, nextGeneration);
    PlayerbotsDatabase.DirectCommitTransaction(transaction);

    OwnerBindingState readback = ReadOwnerBindingState();
    if (!readback.available || !readback.bound || readback.accountId != accountId ||
        readback.characterGuid != characterGuid || readback.generation != nextGeneration)
    {
        result.reason = readback.bound ? "owner_binding_cas_conflict" : "owner_binding_not_committed";
        return result;
    }

    QueryResult audit = PlayerbotsDatabase.Query(
        "SELECT selected_account_id, selected_character_guid, operation "
        "FROM population_director_owner_binding_audit WHERE realm_id = '{}' AND event_key = '{}'",
        realmId, eventKey);
    if (!audit || audit->Fetch()[0].Get<uint32>() != accountId ||
        audit->Fetch()[1].Get<uint32>() != characterGuid ||
        audit->Fetch()[2].Get<std::string>() != "bind")
    {
        result.reason = "owner_binding_audit_readback_failed";
        return result;
    }

    readback.committed = true;
    readback.reason = "bound";
    return readback;
}

void InitializeManagedPopulationProgression()
{
    LoadManagedProgressionInventory();
}

void RegisterPythonPopulationBridge()
{
    new managed_population_progression();
    new managed_population_cleanup();
    if (!RegisterBridge(&PopulationBridge))
        LOG_ERROR("playerbots", "Unable to register the required Python population-v1 bridge.");
}
