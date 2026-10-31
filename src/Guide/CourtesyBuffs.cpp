/* AGPL-3.0-or-later. CPI-224 ambient courtesy, bounded by the existing population tick. */
#include "Guide/CourtesyBuffs.h"
#include "Guide/CourtesyBuffPolicy.h"
#include "Guide/PythonPopulationBridge.h"
#include "BotActivityRegistry.h"
#include "CellImpl.h"
#include "Config.h"
#include "GridNotifiers.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ProgressionSystem.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

namespace
{
using PlayerbotsCourtesy::Settings;
std::shared_ptr<Settings const> CourtesyConfig;
std::atomic<uint32> CourtesyEpoch{0};
uint32 CourtesyCursor=0, CourtesyDelay=0, CourtesyReportDelay=0;
struct CourtesyCounters
{
    std::atomic<uint64> scans{0}, grants{0}, targets{0}, attempts{0}, prepared{0}, success{0}, skips{0}, micros{0}, exhausted{0};
} CourtesyMetrics;
uint64 CourtesyClockMicros()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool CourtesyGeneration(uint32 generation)
{
    return generation && CourtesyEpoch.load(std::memory_order_acquire)==generation;
}
#include "CourtesyBuffNative.inc"

struct CourtesyVisitor
{
    Player* bot;
    PlayerbotAI* ai;
    Settings const& cfg;
    ProgressionSystem::SharedPhaseSnapshot const& phase;
    uint64 deadline;
    uint32 remaining;
    bool attempted=false, exhausted=false;
    template<class T> void Visit(T&) { }
    void Visit(PlayerMapType& players)
    {
        for (auto it=players.begin(); it!=players.end(); ++it)
        {
            if (attempted) return;
            if (!remaining || CourtesyClockMicros()>=deadline) { exhausted=true; return; }
            --remaining;
            ++CourtesyMetrics.targets;
            CourtesyOutcome result=AttemptCourtesy(bot,it->GetSource(),ai,cfg,phase,deadline);
            if (result==CourtesyOutcome::Skipped) { ++CourtesyMetrics.skips; continue; }
            attempted=true;
            ++CourtesyMetrics.attempts;
            if (result!=CourtesyOutcome::Rejected) ++CourtesyMetrics.prepared;
            if (result==CourtesyOutcome::Applied) ++CourtesyMetrics.success;
            else if (result==CourtesyOutcome::Rejected) ++CourtesyMetrics.skips;
        }
    }
};
}

void PlayerbotsCourtesy::Cancel()
{
    CourtesyEpoch.fetch_add(1,std::memory_order_acq_rel);
    std::atomic_store(&CourtesyConfig,std::shared_ptr<Settings const>{});
    CourtesyCursor=CourtesyDelay=0;
}
void PlayerbotsCourtesy::Reload()
{
    Cancel();
    Settings cfg;
    auto read=[](char const* key) { return sConfigMgr->GetOption<std::string>(
        std::string("AiPlayerbot.Courtesy.")+key,""); };
    if (!Parse(read,cfg)) return;
    cfg.generation=CourtesyEpoch.load(std::memory_order_acquire);
    if (!cfg.generation) return;
    std::atomic_store(&CourtesyConfig,std::make_shared<Settings const>(cfg));
}
void PlayerbotsCourtesy::Tick(uint32 diff)
{
    // This caller already runs even with random autologin disabled.
    CourtesyReportDelay+=std::min<uint32>(diff,60000);
    if (CourtesyReportDelay>=60000)
    {
        CourtesyReportDelay=0;
        LOG_INFO("playerbots", "courtesy scans={} grants={} targets={} attempts={} prepared={} success={} skips={} us={} exhausted={}",
            CourtesyMetrics.scans.load(),CourtesyMetrics.grants.load(),CourtesyMetrics.targets.load(),
            CourtesyMetrics.attempts.load(),CourtesyMetrics.prepared.load(),CourtesyMetrics.success.load(),
            CourtesyMetrics.skips.load(),CourtesyMetrics.micros.load(),CourtesyMetrics.exhausted.load());
    }
    auto cfg=std::atomic_load(&CourtesyConfig);
    if (!cfg) return;
    if (diff<CourtesyDelay) { CourtesyDelay-=diff; return; }
    CourtesyDelay=cfg->intervalMs;
    uint64 started=CourtesyClockMicros();
    uint32 scanned=0; bool exhausted=false;
    auto candidates=PlayerbotsPopulation::ReadCourtesyCandidates(CourtesyCursor,cfg->scanBudget,cfg->grantBudget,&scanned,&exhausted);
    CourtesyMetrics.scans+=scanned;
    if (exhausted) ++CourtesyMetrics.exhausted;
    for (auto const& row:candidates)
    {
        if (CourtesyClockMicros()-started>=cfg->timeBudgetUs) { ++CourtesyMetrics.exhausted; break; }
        // Native world-tick lookup only; the permit stores no pointer/callback/target.
        Player* bot=ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(row.characterGuid));
        if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld()) { ++CourtesyMetrics.skips; continue; }
        if (PlayerbotAI* ai=GET_PLAYERBOT_AI(bot))
        {
            ai->ArmCourtesyBuff(Permit(cfg->generation,getMSTime()));
            ++CourtesyMetrics.grants;
        }
    }
    CourtesyMetrics.micros+=CourtesyClockMicros()-started;
}
void PlayerbotsCourtesy::TryBuff(Player* bot,PlayerbotAI* ai)
{
    uint64 permit=ai->TakeCourtesyBuff(); // No global lock/work for unpermitted bot updates.
    if (!permit) return;
    auto cfg=std::atomic_load(&CourtesyConfig);
    if (!cfg || !FreshPermit(permit,cfg->generation,getMSTime()) || !CourtesyGeneration(cfg->generation))
    { ++CourtesyMetrics.skips; return; }
    auto phase=ProgressionSystem::GetSharedPhaseSnapshot();
    if (!CourtesyActor(bot,ai,phase)) { ++CourtesyMetrics.skips; return; }
    uint64 started=CourtesyClockMicros();
    CourtesyVisitor visitor{bot,ai,*cfg,phase,started+cfg->timeBudgetUs,cfg->targetBudget};
    Cell::VisitObjects(bot,visitor,float(cfg->radius));
    if (visitor.exhausted) ++CourtesyMetrics.exhausted;
    CourtesyMetrics.micros+=CourtesyClockMicros()-started;
}
