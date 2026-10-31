/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AutoMaintenanceOnLevelupAction.h"
#include "BroadcastHelper.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "OwnerLearningControl.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "Trigger.h"

bool AutoMaintenanceOnLevelupAction::Execute(Event event)
{
    if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false))
    {
        // ProcessTriggers resets before actions run; retain this existing event until idle.
        context->GetTrigger("levelup")->ExternalEvent(event.getPacket(), event.getOwner());
        return false;
    }
    AutoPickTalents();
    AutoLearnSpell();
    AutoTeleportForLevel();
    AutoUpgradeEquip();

    return true;
}

void AutoMaintenanceOnLevelupAction::AutoTeleportForLevel()
{
    if (!sPlayerbotAIConfig.autoTeleportForLevel || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    if (botAI->HasGameClientMaster())
        return;

    sRandomPlayerbotMgr.RandomTeleportForLevel(bot);
    return;
}

void AutoMaintenanceOnLevelupAction::AutoPickTalents()
{
    if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false)) return;
    if (bot->GetFreeTalentPoints() && owner_learning::Owned(botAI))
        owner_learning::ContinueTalents(botAI);
    else if (bot->GetFreeTalentPoints() && !botAI->HasGameClientMaster() && sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        // Preserve the separate unowned cohort's earned-only continuation.
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitTalentsTree(true, true, false);
    }
    // Pet points have their own earned budget, independent of owner points.
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitPetTalents();
}

void AutoMaintenanceOnLevelupAction::AutoLearnSpell()
{
    std::ostringstream out;
    LearnSpells(&out);

    if (!out.str().empty())
    {
        std::string const temp = out.str();
        out.seekp(0);
        out << "Learned spells: ";
        out << temp;
        out.seekp(-2, out.cur);
        out << ".";
        botAI->TellMaster(out);
    }
    return;
}

void AutoMaintenanceOnLevelupAction::LearnSpells(std::ostringstream* /*out*/)
{
    BroadcastHelper::BroadcastLevelup(botAI, bot);
    // Trainer opportunities remain owned by the existing RPG trainer action.
    // Native quest reward delivery is the only owner of quest spell rewards.
}

void AutoMaintenanceOnLevelupAction::LearnTrainerSpells(std::ostringstream* /*out*/)
{
}

void AutoMaintenanceOnLevelupAction::LearnQuestSpells(std::ostringstream* /*out*/)
{
}

std::string const AutoMaintenanceOnLevelupAction::FormatSpell(SpellInfo const* sInfo)
{
    std::ostringstream out;
    std::string const rank = sInfo->Rank[0];

    if (rank.empty())
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << "]|h|r";
    else
        out << "|cffffffff|Hspell:" << sInfo->Id << "|h[" << sInfo->SpellName[LOCALE_enUS] << " " << rank << "]|h|r";

    return out.str();
}

void AutoMaintenanceOnLevelupAction::AutoUpgradeEquip()
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    if (sPlayerbotAIConfig.randomBotPersistentProgression)
        return;

    PlayerbotFactory factory(bot, bot->GetLevel());

    factory.CleanupConsumables();

    factory.InitAmmo();
    factory.InitReagents();
    factory.InitFood();
    factory.InitConsumables();
    factory.InitPotions();

    if (sPlayerbotAIConfig.autoUpgradeEquip)
        factory.InitEquipment(true);
}
