#ifndef PLAYERBOTS_EARNED_TALENT_RANK_H
#define PLAYERBOTS_EARNED_TALENT_RANK_H
#include <cstdint>
namespace earned_talent
{
// current/desired are acquired rank counts; native LearnTalent takes a zero-based rank.
inline bool NextRank(std::uint32_t current, std::uint32_t desired, std::uint32_t available,
                     std::uint32_t& nativeRank)
{
    if (current >= 5 || desired == 0 || desired > 5 || current >= desired || available == 0)
        return false;
    std::uint32_t const remaining = desired - current;
    std::uint32_t const spend = available < remaining ? available : remaining;
    nativeRank = current + spend - 1;
    return true;
}
}
#endif
