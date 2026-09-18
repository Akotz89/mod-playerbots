#ifndef MOD_PLAYERBOTS_PYTHON_POPULATION_BRIDGE_H
#define MOD_PLAYERBOTS_PYTHON_POPULATION_BRIDGE_H

#include "Define.h"

#include <string>

class Player;

namespace PlayerbotsPopulation
{
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
    uint64 generation{0};
    std::string reason;
};

OwnerBindingState ReadOwnerBindingState();
OwnerBindingState BindOwner(Player* player);
} // namespace PlayerbotsPopulation

#endif
