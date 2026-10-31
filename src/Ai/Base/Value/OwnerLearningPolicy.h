#ifndef PLAYERBOTS_OWNER_LEARNING_POLICY_H
#define PLAYERBOTS_OWNER_LEARNING_POLICY_H
#include <array>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
namespace owner_learning
{
constexpr uint32_t MaxAuthoredSpecs = 20;
struct TalentChoice
{
    bool automatic = true;
    uint32_t spec = 0; // Authored index + 1; zero for an unset or custom plan.
    std::string plan;
};
struct State
{
    uint64_t owner = 0;
    uint32_t revision = 0;
    bool valid = true;
    bool trainer = true;
    std::array<TalentChoice, 2> talents;
};
inline bool Number(std::string const& s, uint64_t limit, uint64_t& n)
{
    if (s.empty() || (s.size() > 1 && s[0] == '0')) return false;
    n = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9' || n > (limit - (c - '0')) / 10) return false;
        n = n * 10 + (c - '0');
    }
    return n <= limit;
}
inline bool Plan(std::string const& s)
{
    if (s.empty()) return true;
    if (s.size() > 512) return false;
    unsigned sections = 1, digits = 0;
    for (char c : s)
    {
        if (c == '-') { if (!digits || sections == 3) return false; ++sections; digits = 0; }
        else if (c >= '0' && c <= '5') ++digits;
        else return false;
    }
    return sections == 3 && digits != 0;
}
inline State Invalid()
{
    State s; s.valid = false; s.trainer = false;
    for (auto& t : s.talents) t.automatic = false;
    return s;
}
inline State Decode(std::string const& text)
{
    if (text.empty()) return State{};
    std::vector<std::string> f;
    std::size_t p = 0, next;
    do { next = text.find('|', p); f.push_back(text.substr(p, next - p)); p = next + 1; }
    while (next != std::string::npos);
    if (f.size() != 10 || f[0] != "1") return Invalid();
    State s; uint64_t n;
    if (!Number(f[1], UINT64_MAX, n)) return Invalid();
    s.owner = n;
    if (!Number(f[2], UINT32_MAX, n)) return Invalid();
    s.revision = n;
    if (f[3] != "0" && f[3] != "1") return Invalid();
    s.trainer = f[3] == "1";
    for (unsigned i = 0; i < 2; ++i)
    {
        unsigned k = 4 + i * 3;
        if (f[k] != "0" && f[k] != "1") return Invalid();
        s.talents[i].automatic = f[k] == "1";
        if (!Number(f[k+1], MaxAuthoredSpecs, n) || !Plan(f[k+2])) return Invalid();
        if (n && !f[k+2].empty()) return Invalid();
        s.talents[i].spec = n; s.talents[i].plan = f[k+2];
    }
    return s;
}
inline std::string Encode(State const& s)
{
    if (!s.valid) return "INVALID";
    std::ostringstream o; o << "1|" << s.owner << '|' << s.revision << '|' << s.trainer;
    for (auto const& t : s.talents) o << '|' << t.automatic << '|' << t.spec << '|' << t.plan;
    return o.str();
}
inline State ForOwner(State s, uint64_t owner)
{
    if (!s.valid || !owner) return Invalid();
    if (s.owner != owner)
    {
        // Retain the global revision when a new owner invalidates old locks.
        if (s.owner && s.revision == UINT32_MAX) return Invalid();
        uint32_t rev = s.revision + (s.owner ? 1 : 0);
        s = State{}; s.revision = rev; s.owner = owner;
    }
    return s;
}
inline bool SetMode(State& s, bool trainer, unsigned slot, bool automatic, uint32_t expected)
{
    if (!s.valid || !s.owner || slot >= 2 || expected != s.revision || s.revision == UINT32_MAX) return false;
    if (trainer) s.trainer = automatic;
    else
    {
        s.talents[slot].automatic = automatic;
        if (automatic) { s.talents[slot].spec = 0; s.talents[slot].plan.clear(); }
    }
    ++s.revision; return true;
}
struct AutoCandidate
{
    uint32_t spec = 0; // One based authored index.
    int rolePriority = 0;
    std::map<uint32_t, uint32_t> ranks;
};
inline uint32_t SelectAuto(std::vector<AutoCandidate> const& candidates,
    std::map<uint32_t, uint32_t> const& acquired, uint32_t freePoints)
{
    uint32_t best = 0, bestSpend = 0;
    int bestPriority = std::numeric_limits<int>::min();
    for (auto const& candidate : candidates)
    {
        if (!candidate.spec || candidate.spec > MaxAuthoredSpecs) continue;
        bool compatible = true; uint32_t headroom = 0;
        for (auto const& current : acquired)
        {
            auto it = candidate.ranks.find(current.first);
            if (it == candidate.ranks.end() || it->second < current.second) { compatible = false; break; }
        }
        if (!compatible) continue;
        for (auto const& desired : candidate.ranks)
        {
            auto it = acquired.find(desired.first);
            uint32_t current = it == acquired.end() ? 0 : it->second;
            if (!desired.second || desired.second > 5) { compatible = false; break; }
            headroom += desired.second - current;
        }
        if (!compatible) continue;
        uint32_t spend = std::min(headroom, freePoints);
        if (freePoints && !spend) continue;
        if (!best || candidate.rolePriority > bestPriority ||
            (candidate.rolePriority == bestPriority && (spend > bestSpend || (spend == bestSpend && candidate.spec < best))))
        { best = candidate.spec; bestPriority = candidate.rolePriority; bestSpend = spend; }
    }
    return best;
}

}
#endif
