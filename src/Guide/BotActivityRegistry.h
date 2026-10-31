#ifndef MOD_PLAYERBOT_BOT_ACTIVITY_REGISTRY_H
#define MOD_PLAYERBOT_BOT_ACTIVITY_REGISTRY_H

// ---------------------------------------------------------------------------
// Shared bot-activity reservation.
//
// Header-only owner-aware reservation registry. Modules can share this state
// when linked into the same binary; dynamic-module sharing is not established.
//
// Claims are all-or-nothing across a complete member set and carry an activity
// kind, generation, and objective. Release is owner-checked. This closes the
// check-then-reserve and blind-release races in the published implementation.
// Other users must include this canonical header and reconcile persisted
// activity state after restart.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace BotActivity
{
    enum class OwnerKind : uint8_t
    {
        DungeonSimRun = 1,
        DungeonSimGuildMission = 2,
        DungeonClearTest = 3,
        OutdoorQuest = 4,
        WorldPvp = 5,
        CourtesyBuff = 6
    };

    struct OwnerToken
    {
        OwnerKind Kind = OwnerKind::DungeonSimRun;
        uint64_t Generation = 0;
        uint64_t Objective = 0;

        bool IsValid() const { return Generation != 0; }
        bool operator==(OwnerToken const& other) const
        {
            return Kind == other.Kind && Generation == other.Generation && Objective == other.Objective;
        }
    };

    inline std::mutex& _lock()
    {
        static std::mutex m;
        return m;
    }

    inline std::unordered_map<uint32_t, OwnerToken>& _reservations()
    {
        static std::unordered_map<uint32_t, OwnerToken> reservations;
        return reservations;
    }

    // Atomically reserves the complete GUID set. The call is idempotent for the
    // same owner token and fails without changing state if any GUID belongs to
    // another owner. Callers must use a new Generation for every activity.
    inline bool TryReserveMany(OwnerToken const& owner, std::vector<uint32_t> guids)
    {
        if (!owner.IsValid() || guids.empty())
            return false;

        std::sort(guids.begin(), guids.end());
        guids.erase(std::unique(guids.begin(), guids.end()), guids.end());
        if (guids.front() == 0)
            return false;

        std::lock_guard<std::mutex> guard(_lock());
        for (uint32_t guid : guids)
        {
            auto const existing = _reservations().find(guid);
            if (existing != _reservations().end() && !(existing->second == owner))
                return false;
        }
        for (uint32_t guid : guids)
            _reservations()[guid] = owner;
        return true;
    }

    // Releases only reservations owned by this exact generation. A stale or
    // competing activity can never erase another owner's claim.
    inline void ReleaseMany(OwnerToken const& owner, std::vector<uint32_t> const& guids)
    {
        if (!owner.IsValid())
            return;

        std::lock_guard<std::mutex> guard(_lock());
        for (uint32_t guid : guids)
        {
            auto const existing = _reservations().find(guid);
            if (existing != _reservations().end() && existing->second == owner)
                _reservations().erase(existing);
        }
    }

    inline bool IsReserved(uint32_t guidLow)
    {
        std::lock_guard<std::mutex> guard(_lock());
        return _reservations().count(guidLow) != 0;
    }
}

#endif // MOD_PLAYERBOT_BOT_ACTIVITY_REGISTRY_H
