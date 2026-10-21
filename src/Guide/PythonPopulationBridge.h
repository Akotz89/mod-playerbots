#ifndef MOD_PLAYERBOTS_PYTHON_POPULATION_BRIDGE_H
#define MOD_PLAYERBOTS_PYTHON_POPULATION_BRIDGE_H

#include "Define.h"

#include <string>
#include <vector>

class Player;

namespace PlayerbotsPopulation
{
struct CourtesyCandidate
{
    uint32 characterGuid{0};
};

std::vector<CourtesyCandidate> ReadCourtesyCandidates(uint32& cursor, uint32 scanBudget, uint32 resultBudget,
    uint32* scanned = nullptr, bool* exhausted = nullptr);
bool CourtesyEligible(uint32 characterGuid);

struct OwnerBindingState
{
    bool available{false};
    bool bound{false};
    bool committed{false};
    bool alreadyBound{false};
    uint32 accountId{0};
    uint32 characterGuid{0};
    uint8 highWater{0};
    uint8 currentEpoch{0};
    uint8 maximumOwnerLevel{0};
    uint8 globalPhaseCap{0};
    uint64 generation{0};
    std::string reason;
};

constexpr uint8 CappedOwnerEpoch(uint8 highWater, uint8 maximumOwnerLevel, uint8 globalPhaseCap) noexcept
{
    uint8 maximum = highWater < maximumOwnerLevel ? highWater : maximumOwnerLevel;
    return maximum < globalPhaseCap ? maximum : globalPhaseCap;
}

OwnerBindingState ReadOwnerBindingState();
OwnerBindingState BindOwner(Player* player);
} // namespace PlayerbotsPopulation

#endif
