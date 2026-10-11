#include "PopulationBridge.h"
#include "AccountMgr.h"
#include "Config.h"
#include "Cryptography/CryptoHash.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Guide/PythonPopulationBridge.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "WorldSession.h"
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
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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
};

std::mutex ProgressionMutex;
std::unordered_map<uint32, ManagedProgressionPolicy> ManagedPolicies;
std::unordered_map<std::string, uint32> AdmittedProgressionIdentities;
constexpr uint32 ProgressionPolicyLeaseMilliseconds = 5000;
std::atomic<uint64> StrategicGeneration{1};
std::atomic<int8> OwnerLedgerAvailability{-1};

std::string PopulationRealmId()
{
    return std::to_string(sConfigMgr->GetOption<uint32>("RealmID", 1));
}

std::string Sha256Hex(std::string const& value)
{
    auto const digest = Acore::Crypto::SHA256::GetDigestOf(value);
    return Acore::Impl::ByteArrayToHexStr(digest.data(), digest.size());
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
    std::unordered_map<uint32, ManagedProgressionPolicy> inventory;
    std::unordered_map<std::string, uint32> admitted;
    uint64 expected = count->Fetch()[0].Get<uint64>();
    if (expected)
    {
        QueryResult rows = PlayerbotsDatabase.Query(
            "SELECT character_guid, identity_key, admitted FROM playerbots_population_identity "
            "WHERE character_guid IS NOT NULL");
        if (!rows)
            throw std::runtime_error("Cannot read durable managed progression inventory");
        do
        {
            Field* fields = rows->Fetch();
            uint32 guid = fields[0].Get<uint32>();
            inventory.emplace(guid, ManagedProgressionPolicy{});
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

bool ManagedCanProgress(Player* player);

std::string StrategicControlReason(Player* player)
{
    if (!player || !player->IsInWorld()) return "offline";
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai || !sRandomPlayerbotMgr.IsRandomBot(player)) return "not_autonomous_session";
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

class managed_population_progression : public PlayerScript
{
public:
    managed_population_progression() : PlayerScript("managed_population_progression") { }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!ManagedCanProgress(player))
            amount = 0;
    }

    bool OnPlayerCanGiveLevel(Player* player, uint8 /*newLevel*/) override
    {
        return ManagedCanProgress(player);
    }
};

class PlayerbotsPopulationBridge final : public Bridge
{
public:
    StrategicSnapshot ReadQuestSnapshot(std::string const& identityKey,
                                        uint32 observedQuestId) override
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
        }
        QueryResult binding = PlayerbotsDatabase.Query(
            "SELECT account_id, character_guid FROM playerbots_population_identity "
            "WHERE identity_key = '{}' AND admitted = 1", identityKey);
        if (!binding || binding->Fetch()[0].IsNull() || binding->Fetch()[1].IsNull() ||
            binding->Fetch()[1].Get<uint32>() != result.characterGuid ||
            !binding->Fetch()[0].Get<uint32>())
            throw std::runtime_error("strategic identity binding unavailable or inconsistent");
        result.accountId = binding->Fetch()[0].Get<uint32>();
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
                result.currentGoalQuestId = goal->questId;
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

    StrategicGoalResult SetQuestGoal(std::string const& identityKey, uint32 questId) override
    {
        StrategicSnapshot snapshot = ReadQuestSnapshot(identityKey, questId);
        if (!snapshot.controlAllowed)
            return {snapshot.reason == "invalid_identity" || snapshot.reason == "not_admitted" ?
                        "rejected" : "deferred", snapshot.reason, questId};
        Player* player = sRandomPlayerbotMgr.GetPlayerBot(snapshot.characterGuid);
        std::string reason = StrategicQuestReason(player, questId);
        if (!reason.empty())
            return {"rejected", reason, questId};
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        NewRpgInfo& info = ai->rpgInfo;
        auto const* goal = std::get_if<NewRpgInfo::DoQuest>(&info.data);
        if (goal && goal->questId == questId && info.pythonQuestGoal &&
            info.pythonGoalGeneration == StrategicGeneration.load())
        {
            info.pythonGoalRefreshedAt = getMSTime();
            return {"accepted", "renewed", questId};
        }
        if (!std::holds_alternative<NewRpgInfo::Idle>(info.data))
            return {"deferred", "existing_native_goal", questId};
        info.ChangeToDoQuest(questId, sObjectMgr->GetQuestTemplate(questId));
        info.pythonQuestGoal = true;
        info.pythonGoalAccountId = snapshot.accountId;
        info.pythonGoalRefreshedAt = getMSTime();
        info.pythonGoalGeneration = StrategicGeneration.load();
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
                if (!info.pythonQuestGoal || !goal || goal->questId != row.questId)
                    reason = "not_owned_goal";
                else if (!info.pythonGoalAccountId || !player->GetSession() ||
                    player->GetSession()->GetAccountId() != info.pythonGoalAccountId)
                    reason = "account_changed";
                else if (info.pythonGoalGeneration != StrategicGeneration.load())
                    reason = "generation_changed";
                else if (GetMSTimeDiffToNow(info.pythonGoalRefreshedAt) > ProgressionPolicyLeaseMilliseconds)
                    reason = "lease_expired";
                else
                {
                    reason = StrategicControlReason(player);
                    if (reason.empty())
                        reason = StrategicQuestReason(player, row.questId);
                    if (reason.empty())
                        info.pythonGoalRefreshedAt = getMSTime();
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
            "SELECT admitted, character_guid FROM playerbots_population_identity WHERE identity_key = '{}' "
            "AND wave_key = '{}' AND manifest_digest = '{}' AND account_id IS NOT NULL "
            "AND character_guid IS NOT NULL", identityKey, waveKey, manifestDigest);
        if (!row || row->Fetch()[0].Get<uint8>() != 1 || row->Fetch()[1].IsNull())
            return false;
        uint32 guid = row->Fetch()[1].Get<uint32>();
        if (!guid)
            return false;
        std::lock_guard<std::mutex> lock(ProgressionMutex);
        ManagedPolicies.try_emplace(guid);
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
            accepted.push_back({guid, {std::min(policy.effectiveCeiling, phase.levelCeiling),
                                       policy.held, now}});
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
        StrategicGeneration.fetch_add(1);
        try
        {
            {
                std::lock_guard<std::mutex> lock(ProgressionMutex);
                for (auto& entry : ManagedPolicies)
                    entry.second = ManagedProgressionPolicy{};
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
    void OnUpdate(uint32 diff) override { PopulationBridge.DrainRevokedSessions(diff); }
};
} // namespace

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
