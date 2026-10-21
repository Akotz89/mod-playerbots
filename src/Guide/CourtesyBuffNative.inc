/* AGPL-3.0-or-later. Buff-family selection inspired by Rockhopper1776/mod-rndbot-buffs
 * ee129e220b0c379e592f3db92437736d345eac04. Scheduling and native guards are rewritten.
 * Included by the real native caller and the focused ABI-shim fixture. */
struct CourtesyFamily { uint32 root, equivalent, playerClass; };
constexpr CourtesyFamily CourtesyFamilies[] = {
    {1243,21562,CLASS_PRIEST}, {14752,27681,CLASS_PRIEST}, {976,27683,CLASS_PRIEST},
    {1459,23028,CLASS_MAGE}, {1126,21849,CLASS_DRUID},
    {20217,25898,CLASS_PALADIN}, {19740,25782,CLASS_PALADIN},
    {19742,25894,CLASS_PALADIN}, {20911,25899,CLASS_PALADIN}
};
enum class CourtesyOutcome { Skipped, Rejected, Prepared, Applied };
std::atomic<uint64> CourtesyClaimSequence{0};
struct CourtesyClaim
{
    BotActivity::OwnerToken owner;
    std::vector<uint32> guids;
    bool acquired;
    CourtesyClaim(uint32 caster, uint32 target):
        owner{BotActivity::OwnerKind::CourtesyBuff, ++CourtesyClaimSequence,
              (uint64(caster)<<32)|target}, guids{caster,target},
        acquired(BotActivity::TryReserveMany(owner,guids)) { }
    ~CourtesyClaim() { if (acquired) BotActivity::ReleaseMany(owner,guids); }
};

bool CourtesyActor(Player* bot, PlayerbotAI* ai, ProgressionSystem::SharedPhaseSnapshot const& phase)
{
    if (!bot || !ai || !phase.enabled || !phase.policyValid || !phase.phaseLevelCeiling ||
        !bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || !bot->GetSession() ||
        bot->GetSession()->isLogingOut() || !bot->GetSession()->IsBot() ||
        bot->GetSession()->GetPlayer()!=bot || bot->IsGameMaster() || !bot->IsAlive() ||
        bot->IsInCombat() || bot->InBattleground() || bot->InArena() || bot->IsInFlight() ||
        !bot->GetMap() || bot->GetMap()->IsDungeon() || bot->IsNonMeleeSpellCast(false) ||
        bot->HasUnitState(UNIT_STATE_LOST_CONTROL) || ai->HasGameClientMaster() ||
        !PlayerbotsPopulation::CourtesyEligible(bot->GetGUID().GetCounter()) ||
        BotActivity::IsReserved(bot->GetGUID().GetCounter())) return false;
    // A fully admitted autonomous group remains ambient; human/unknown ownership does not.
    if (Group* group=bot->GetGroup())
    {
        if (group->GetMembersCount()>MAXRAIDSIZE) return false;
        for (auto const& member:group->GetMemberSlots())
            if (!PlayerbotsPopulation::CourtesyEligible(member.guid.GetCounter()) ||
                BotActivity::IsReserved(member.guid.GetCounter())) return false;
    }
    return true;
}

bool CourtesyTarget(Player* bot, Player* target, PlayerbotsCourtesy::Settings const& cfg)
{
    return target && target!=bot && target->IsInWorld() && !target->IsDuringRemoveFromWorld() &&
        target->GetSession() && !target->GetSession()->isLogingOut() &&
        target->GetSession()->GetPlayer()==target && !target->GetSession()->IsBot() &&
        !GET_PLAYERBOT_AI(target) && !target->IsGameMaster() && target->IsAlive() &&
        !target->IsInCombat() && !target->InBattleground() && !target->InArena() && !target->IsInFlight() &&
        bot->GetMap()==target->GetMap() && bot->InSamePhase(target) &&
        bot->GetTeamId()==target->GetTeamId() && bot->IsFriendlyTo(target) &&
        bot->IsWithinDistInMap(target,float(cfg.radius)) && bot->IsWithinLOSInMap(target) &&
        !BotActivity::IsReserved(target->GetGUID().GetCounter());
}

bool CourtesyAura(Player* target, uint32 root, uint64 deadline)
{
    uint32 id=root;
    for (uint32 n=0; id && n<16; ++n)
    {
        if (CourtesyClockMicros()>=deadline) return true;
        if (target->HasAura(id)) return true;
        uint32 next=sSpellMgr->GetNextSpellInChain(id);
        if (next==id) return true;
        id=next;
    }
    return id!=0; // Unsupported/cyclic rank chains fail closed.
}

uint32 CourtesyRank(Player* bot, Player* target, uint32 root,
                    ProgressionSystem::SharedPhaseSnapshot const& phase, uint64 deadline)
{
    uint32 best=0, id=root;
    for (uint32 n=0; id && n<16; ++n)
    {
        if (CourtesyClockMicros()>=deadline) return 0;
        SpellInfo const* info=sSpellMgr->GetSpellInfo(id);
        if (!info || sSpellMgr->GetFirstSpellInChain(id)!=root) return 0;
        if (bot->HasSpell(id) && info->SpellLevel<=phase.phaseLevelCeiling &&
            info->GetAuraRankForLevel(target->GetLevel())==info && info->IsPositive() &&
            !info->IsPassive() && sScriptMgr->OnPlayerCanLearnProgressionSpell(bot,id)) best=id;
        uint32 next=sSpellMgr->GetNextSpellInChain(id);
        if (next==id) return 0;
        id=next;
    }
    return id ? 0 : best;
}

CourtesyOutcome AttemptCourtesy(Player* bot, Player* target, PlayerbotAI* ai,
    PlayerbotsCourtesy::Settings const& cfg, ProgressionSystem::SharedPhaseSnapshot const& phase, uint64 deadline)
{
    if (!CourtesyActor(bot,ai,phase) || !CourtesyTarget(bot,target,cfg) ||
        bot->GetPowerPct(POWER_MANA)<cfg.manaPercent) return CourtesyOutcome::Skipped;
    for (CourtesyFamily const& family:CourtesyFamilies)
    {
        if (CourtesyClockMicros()>=deadline) return CourtesyOutcome::Skipped;
        if (family.playerClass!=bot->getClass() || CourtesyAura(target,family.root,deadline) ||
            CourtesyAura(target,family.equivalent,deadline)) continue;
        uint32 id=CourtesyRank(bot,target,family.root,phase,deadline);
        if (!id || bot->HasSpellCooldown(id) || (bot->GetPet() && bot->GetPet()->HasSpell(id))) continue;
        SpellInfo const* info=sSpellMgr->GetSpellInfo(id);
        if (info->PowerType!=POWER_MANA || !bot->IsWithinDistInMap(target,info->GetMaxRange(true)) ||
            (bot->isMoving() && info->CalcCastTime(bot))) continue;
        bool reagent=false;
        for (uint32 n=0; n<MAX_SPELL_REAGENTS; ++n)
            reagent=reagent || (info->Reagent[n]>0 && info->ReagentCount[n]>0);
        if (reagent) continue; // Courtesy never consumes inventory; no prayer/greater/reagent variant.
        if (CourtesyClockMicros()>=deadline || !CourtesyGeneration(cfg.generation) ||
            !PlayerbotsPopulation::CourtesyEligible(bot->GetGUID().GetCounter())) return CourtesyOutcome::Skipped;
        // Claim atomically across native prepare, so competing activities cannot win a check/cast race.
        CourtesyClaim claim(bot->GetGUID().GetCounter(),target->GetGUID().GetCounter());
        if (!claim.acquired) return CourtesyOutcome::Skipped;
        // Numeric rank is final. Native non-triggered prepare owns power/GCD/reagents/cast rules.
        if (!ai->CastSpell(id,target)) return CourtesyOutcome::Rejected;
        return target->HasAura(id) ? CourtesyOutcome::Applied : CourtesyOutcome::Prepared;
    }
    return CourtesyOutcome::Skipped;
}
