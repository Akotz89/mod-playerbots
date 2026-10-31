/* AGPL-3.0-or-later. CPI-224 bounded native courtesy policy. */
#ifndef PLAYERBOTS_COURTESY_POLICY_H
#define PLAYERBOTS_COURTESY_POLICY_H
#include <cstdint>
#include <string>
namespace PlayerbotsCourtesy
{
struct Settings
{
    std::uint32_t generation{0}, intervalMs{0}, scanBudget{0}, grantBudget{0}, targetBudget{0};
    std::uint32_t radius{0}, manaPercent{0}, timeBudgetUs{0};
};
inline bool Number(std::string const& text, std::uint32_t low, std::uint32_t high, std::uint32_t& out)
{
    if (text.empty() || text.size()>10) return false;
    std::uint64_t n=0;
    for (char c:text) { if(c<'0'||c>'9') return false; n=n*10+(c-'0'); }
    if(n<low||n>high) return false;
    out=static_cast<std::uint32_t>(n); return true;
}
template<class Read> bool Parse(Read const& read, Settings& out)
{
    Settings value;
    if(read("Enable")!="1" || !Number(read("IntervalMs"),1000,60000,value.intervalMs) ||
       !Number(read("ScanBudget"),1,64,value.scanBudget) || !Number(read("GrantBudget"),1,8,value.grantBudget) ||
       !Number(read("TargetBudget"),1,32,value.targetBudget) || !Number(read("Radius"),1,30,value.radius) ||
       !Number(read("MinimumManaPercent"),1,100,value.manaPercent) ||
       !Number(read("TimeBudgetUs"),100,5000,value.timeBudgetUs)) return false;
    out=value; return true;
}
inline std::uint64_t Permit(std::uint32_t generation, std::uint32_t now)
{ return (std::uint64_t(generation)<<32)|now; }
inline bool FreshPermit(std::uint64_t permit, std::uint32_t generation, std::uint32_t now)
{ return permit && generation && std::uint32_t(permit>>32)==generation && std::uint32_t(now-std::uint32_t(permit))<=1000; }
}
#endif
