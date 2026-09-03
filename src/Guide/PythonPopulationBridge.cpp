#include "PopulationBridge.h"
#include "AccountMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ProgressionSystem.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "Random.h"
#include <algorithm>
#include <atomic>
#include <cctype>

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

class PlayerbotsPopulationBridge final : public Bridge
{
public:
    OwnerHighWater ReadOwnerHighWater() override
    {
        OwnerHighWater result;
        result.characterGuid = sConfigMgr->GetOption<uint32>("Python.Population.OwnerCharacterGuid", 0);
        result.observedEpoch = static_cast<uint64>(GameTime::GetGameTime().count());
        if (!result.characterGuid)
            return result;
        Player* owner = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(result.characterGuid));
        if (owner && owner->GetSession() && !GET_PLAYERBOT_AI(owner))
        {
            result.level = owner->GetLevel();
            result.authenticated = true;
        }
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
            "SELECT admitted FROM playerbots_population_identity WHERE identity_key = '{}'", identityKey);
        return row && row->Fetch()[0].Get<uint8>() == 1;
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
        if (fields[1].Get<uint8>() == 1)
            return true;
        CapacitySnapshot capacity = ReadCapacitySnapshot();
        if (!capacity.valid || !capacity.availableSessions)
            return false;
        uint32 guid = fields[0].Get<uint32>();
        if (!sRandomPlayerbotMgr.PopulationRequestSession(guid))
            return false;
        PlayerbotsDatabase.DirectExecute(
            "UPDATE playerbots_population_identity SET session_requested = 1 "
            "WHERE identity_key = '{}' AND admitted = 1", identityKey);
        return true;
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
        return true;
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
            if (QueryResult rows = PlayerbotsDatabase.Query(
                    "SELECT character_guid FROM playerbots_population_identity "
                    "WHERE session_requested = 1 AND character_guid IS NOT NULL"))
            {
                do
                {
                    sRandomPlayerbotMgr.PopulationReleaseSession(rows->Fetch()[0].Get<uint32>());
                } while (rows->NextRow());
            }
            PlayerbotsDatabase.DirectExecute(
                "UPDATE playerbots_population_identity SET session_requested = 0");
            _capacityGeneration.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

private:
    std::atomic<uint64> _capacityGeneration{1};
};

PlayerbotsPopulationBridge PopulationBridge;
} // namespace

void RegisterPythonPopulationBridge()
{
    if (!RegisterBridge(&PopulationBridge))
        LOG_ERROR("playerbots", "Unable to register the required Python population-v1 bridge.");
}
