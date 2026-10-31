#ifndef PLAYERBOTS_OWNER_LEARNING_CONTROL_H
#define PLAYERBOTS_OWNER_LEARNING_CONTROL_H
#include "OwnerLearningPolicy.h"
#include "Value.h"
#include "PlayerbotAI.h"
#include "AiObjectContext.h"
#include "PlayerbotRepository.h"
#include "PlayerbotAIConfig.h"
#include "Player.h"
#include "WorldSession.h"
#include "DBCStores.h"
#include "PlayerbotFactory.h"
#include "Group.h"
#include "GroupReference.h"
#include <cctype>
namespace owner_learning
{
class SavedValue : public ManualSetValue<std::string>
{
public:
    explicit SavedValue(PlayerbotAI* ai) : ManualSetValue<std::string>(ai, "", "owner learning") {}
    std::string const Save() override { return value; }
    bool Load(std::string const text) override
    {
        value = Encode(Decode(text));
        return Decode(text).valid;
    }
};
inline bool Owned(PlayerbotAI* ai)
{
    if (!ai || !ai->GetBot() || !ai->GetMaster() || ai->GetBot() == ai->GetMaster() ||
        !ai->HasGameClientMaster() || !ai->GetBot()->GetSession()) return false;
    return !(sPlayerbotAIConfig.randomBotPersistentProgression &&
        sPlayerbotAIConfig.IsInRandomAccountList(ai->GetBot()->GetSession()->GetAccountId()));
}
inline State Read(PlayerbotAI* ai)
{
    if (!Owned(ai)) return Invalid();
    auto* value = ai->GetAiObjectContext()->GetValue<std::string>("owner learning");
    if (!value) return Invalid();
    State const saved = Decode(value->Get());
    State const state = ForOwner(saved, ai->GetMaster()->GetGUID().GetRawValue());
    if (state.valid && state.owner != saved.owner)
    {
        value->Set(Encode(state));
        PlayerbotRepository::instance().Save(ai);
    }
    return state;
}
inline void Store(PlayerbotAI* ai, State const& s)
{
    if (!Owned(ai) || !s.valid || s.owner != ai->GetMaster()->GetGUID().GetRawValue()) return;
    ai->GetAiObjectContext()->GetValue<std::string>("owner learning")->Set(Encode(s));
    PlayerbotRepository::instance().Save(ai);
}
inline bool AutomaticTrainer(PlayerbotAI* ai) { State s = Read(ai); return s.valid && s.revision != UINT32_MAX && s.trainer; }
inline bool AutomaticTalents(PlayerbotAI* ai)
{
    State s = Read(ai); unsigned slot = ai && ai->GetBot() ? ai->GetBot()->GetActiveSpec() : 2;
    return s.valid && s.revision != UINT32_MAX && slot < 2 && s.talents[slot].automatic;
}
inline void ManualTrainer(PlayerbotAI* ai)
{
    State s = Read(ai);
    if (SetMode(s, true, 0, false, s.revision)) Store(ai, s);
}
inline bool ManualTalents(PlayerbotAI* ai, unsigned slot)
{
    State s = Read(ai);
    if (!SetMode(s, false, slot, false, s.revision)) return false;
    Store(ai, s); return true;
}
// Bounded objective: compatible authored class plans, party-role deficit priority,
// then earned-point headroom, then lowest authored index. This is not a combat optimizer.
inline uint32_t ChooseAutoTalents(PlayerbotAI* ai, std::string* why = nullptr)
{
    static_assert(MAX_SPECNO == MaxAuthoredSpecs, "Update saved-state authored bound with config capacity");
    if (why) *why = "AUTO_NO_ELIGIBLE_CONTEXT";
    if (!AutomaticTalents(ai)) return 0;
    Player* bot = ai->GetBot();
    std::map<uint32_t, TalentEntry const*> positions;
    std::map<uint32_t, uint32_t> acquired;
    for (uint32_t i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        auto const* t = sTalentStore.LookupEntry(i);
        auto const* tab = t ? sTalentTabStore.LookupEntry(t->TalentTab) : nullptr;
        if (!tab || tab->tabpage >= 3 || !(tab->ClassMask & bot->getClassMask())) continue;
        positions[(tab->tabpage << 16) | (t->Row << 8) | t->Col] = t;
        for (uint8_t r = 0; r < 5; ++r)
            if (t->RankID[r] && bot->HasTalent(t->RankID[r], bot->GetActiveSpec())) acquired[t->TalentID] = r + 1;
    }
    bool tank = false, heal = false;
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive()) continue;
            tank = tank || PlayerbotAI::IsTank(member);
            heal = heal || PlayerbotAI::IsHeal(member);
        }
    std::vector<AutoCandidate> candidates;
    uint32_t cls = bot->getClass();
    if (cls >= MAX_CLASSES) return 0;
    for (uint32_t spec = 0; spec < MAX_SPECNO; ++spec)
    {
        if (sPlayerbotAIConfig.premadeSpecName[cls][spec].empty()) continue;
        AutoCandidate c; c.spec = spec + 1;
        bool valid = true;
        int start = bot->GetLevel();
        while (start > 1 && start < 80 && sPlayerbotAIConfig.parsedSpecLinkOrder[cls][spec][start].empty()) --start;
        for (int level = start; level <= 80 && valid; ++level)
            for (auto const& p : sPlayerbotAIConfig.parsedSpecLinkOrder[cls][spec][level])
            {
                if (p.size() != 4 || p[0] >= 3 || p[1] > 255 || p[2] > 255 || !p[3] || p[3] > 5)
                    { valid = false; break; }
                auto it = positions.find((p[0] << 16) | (p[1] << 8) | p[2]);
                if (it == positions.end() || !it->second->RankID[p[3]-1]) { valid = false; break; }
                auto& rank = c.ranks[it->second->TalentID]; rank = std::max(rank, p[3]);
            }
        if (!valid || c.ranks.empty()) continue;
        std::array<uint32_t, 3> tabs = {{0,0,0}};
        for (auto const& rank : c.ranks)
        {
            auto const* t = sTalentStore.LookupEntry(rank.first);
            auto const* tab = t ? sTalentTabStore.LookupEntry(t->TalentTab) : nullptr;
            if (!tab) { valid = false; break; }
            tabs[tab->tabpage] += rank.second;
            auto dependency = c.ranks.find(t->DependsOn);
            if (t->DependsOn && (dependency == c.ranks.end() || dependency->second <= t->DependsOnRank)) valid = false;
            uint32_t earlier = 0;
            for (auto const& other : c.ranks)
            {
                auto const* previous = sTalentStore.LookupEntry(other.first);
                if (previous && previous->TalentTab == t->TalentTab && previous->Row < t->Row) earlier += other.second;
            }
            if (earlier < t->Row * 5) valid = false;
        }
        if (!valid) continue;
        unsigned tab = 0;
        for (unsigned i = 1; i < 3; ++i) if (tabs[i] > tabs[tab]) tab = i;
        std::string name = sPlayerbotAIConfig.premadeSpecName[cls][spec];
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        bool const tankPlan = name.find("tank") != std::string::npos || name.find("bear") != std::string::npos ||
            (cls == CLASS_WARRIOR && tab == 2) || (cls == CLASS_PALADIN && tab == 1);
        bool const healPlan = (cls == CLASS_PRIEST && tab < 2) ||
            ((cls == CLASS_DRUID || cls == CLASS_SHAMAN) && tab == 2) || (cls == CLASS_PALADIN && tab == 0);
        c.rolePriority = !tank && tankPlan ? 3 : (!heal && healPlan ? 2 : (!tankPlan && !healPlan ? 1 : 0));
        candidates.push_back(c);
    }
    uint32_t selected = SelectAuto(candidates, acquired, bot->GetFreeTalentPoints());
    if (why)
    {
        *why = "AUTO_NO_COMPATIBLE_AUTHORED_PLAN";
        for (auto const& candidate : candidates)
            if (candidate.spec == selected)
            {
                uint32_t headroom = 0;
                for (auto const& rank : candidate.ranks)
                    headroom += rank.second - (acquired.count(rank.first) ? acquired[rank.first] : 0);
                *why = "AUTO_ROLE_PRIORITY_" + std::to_string(candidate.rolePriority) + "_HEADROOM_" +
                    std::to_string(std::min(headroom, bot->GetFreeTalentPoints())) + "_SPEC_" + std::to_string(selected);
            }
    }
    auto state = Read(ai);
    auto& choice = state.talents[bot->GetActiveSpec()];
    bool const changed = choice.spec != selected || !choice.plan.empty();
    choice.spec = selected; choice.plan.clear();
    if (changed) Store(ai, state);
    return selected;
}
inline uint32_t RunAutoTalents(PlayerbotAI* ai, std::string* why = nullptr)
{
    if (!Owned(ai) || ai->GetBot()->IsInCombat() || ai->GetBot()->IsNonMeleeSpellCast(false))
    { if (why) *why = "BUSY"; return 0; }
    uint32_t const before = ai->GetBot()->GetFreeTalentPoints();
    uint32_t selected = ChooseAutoTalents(ai, why);
    if (selected && selected <= MAX_SPECNO && AutomaticTalents(ai) && ai->GetBot()->GetFreeTalentPoints())
        PlayerbotFactory::InitTalentsBySpecNo(ai->GetBot(), selected - 1, false);
    if (ai->GetBot()->GetFreeTalentPoints() < before) ai->ResetStrategies();
    return selected;
}
// Continue the owner's saved blueprint; never reset, switch, or select another build.
inline void ContinueTalents(PlayerbotAI* ai)
{
    if (!Owned(ai)) return;
    Player* bot = ai->GetBot();
    if (!bot->GetFreeTalentPoints() || bot->IsInCombat() || bot->IsNonMeleeSpellCast(false)) return;
    State const state = Read(ai);
    unsigned const slot = bot->GetActiveSpec();
    if (!state.valid || state.revision == UINT32_MAX || slot >= 2) return;
    auto const& choice = state.talents[slot];
    if (choice.automatic) { RunAutoTalents(ai); return; }
    uint32_t const before = bot->GetFreeTalentPoints();
    if (choice.spec && choice.spec <= MAX_SPECNO)
        PlayerbotFactory::InitTalentsBySpecNo(bot, choice.spec - 1, false);
    else if (!choice.plan.empty())
        PlayerbotFactory::InitTalentsByParsedSpecLink(bot,
            PlayerbotAIConfig::ParseTempTalentsOrder(bot->getClass(), choice.plan), false);
    if (bot->GetFreeTalentPoints() < before) ai->ResetStrategies();
}

}
#endif
