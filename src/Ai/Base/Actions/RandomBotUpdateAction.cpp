/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RandomBotUpdateAction.h"
#include "Event.h"
#include "Playerbots.h"
#include "ScriptMgr.h"

bool RandomBotUpdateAction::Execute(Event /*event*/)
{
    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Community activity modules can atomically own an RNDBOT for a real
    // gameplay activity. Do not let periodic factory maintenance independently
    // disband that owned group, randomize the character, or teleport it away.
    // The default hook result is true, so ordinary Playerbots behavior is
    // unchanged when no activity owner is installed.
    if (!sScriptMgr->OnPlayerbotCanRunRandomBotMaintenance(bot))
        return true;

    if (bot->GetGroup() && botAI->GetGroupLeader())
    {
        PlayerbotAI* groupLeaderBotAI = GET_PLAYERBOT_AI(botAI->GetGroupLeader());
        if (!groupLeaderBotAI || IsSelfBot(botAI->GetGroupLeader()))
            return true;
    }

    if (botAI->HasPlayerNearby(sPlayerbotAIConfig.grindDistance))
        return true;

    return sRandomPlayerbotMgr.ProcessBot(bot);
}

bool RandomBotUpdateAction::isUseful() { return AI_VALUE(bool, "random bot update"); }
