#ifndef PARTY_REACTIVE_CLAIMS_H
#define PARTY_REACTIVE_CLAIMS_H
#include <cstdint>
#include <map>
#include <mutex>
#include <chrono>
#include <tuple>
namespace PartyReactiveClaims {
struct Claim { std::uint64_t actor; std::uint64_t expiry; };
inline std::mutex mutex;
using Target=std::tuple<std::uint32_t,std::uint32_t,std::uint64_t>;
inline std::map<Target, Claim> claims;
inline std::map<std::uint64_t,std::uint64_t> participants;
inline std::uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void Prune() { auto now=Now(); for(auto i=claims.begin();i!=claims.end();) if(i->second.expiry<=now)i=claims.erase(i);else ++i; for(auto i=participants.begin();i!=participants.end();)if(i->second<=now)i=participants.erase(i);else ++i; }
inline bool Register(std::uint64_t actor) { std::lock_guard<std::mutex> guard(mutex);Prune();if(!actor || (participants.size()>=640 && !participants.count(actor)))return false;participants[actor]=Now()+35000;return true; }
inline bool Acquire(std::uint64_t actor,std::uint64_t target,std::uint64_t expiry,std::uint32_t map,std::uint32_t instance) { std::lock_guard<std::mutex> guard(mutex);Prune();if(!participants.count(actor))return false; Target key{map,instance,target};for(auto const& existing:claims)if(existing.second.actor==actor && existing.first!=key)return false;auto i=claims.find(key); if(i!=claims.end()&&i->second.actor!=actor)return false; if(!target||(!claims.count(key)&&claims.size()>=64))return false; claims[key]={actor,expiry}; return true; }
inline void Release(std::uint64_t actor,std::uint64_t target,std::uint32_t map,std::uint32_t instance) { std::lock_guard<std::mutex> guard(mutex);auto i=claims.find(Target{map,instance,target});if(i!=claims.end()&&i->second.actor==actor)claims.erase(i); }
inline bool Allowed(std::uint64_t actor,std::uint64_t target,std::uint32_t map,std::uint32_t instance,bool currentControlled=false) { std::lock_guard<std::mutex> guard(mutex);Prune();if(!currentControlled)return true; Target key{map,instance,target};auto i=claims.find(key); if(i!=claims.end()&&i->second.actor!=actor)return false; for(auto const& c:claims)if(c.second.actor==actor&&c.first!=key)return false; return true; }
}
#endif
