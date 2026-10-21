/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChatCommandTrigger.h"
#include "Playerbots.h"
#include "ObjectAccessor.h"

ChatCommandTrigger::ChatCommandTrigger(PlayerbotAI* botAI, std::string const command)
    : Trigger(botAI, command), triggered(false)
{
}

void ChatCommandTrigger::ExternalEvent(std::string const paramName, Player* eventPlayer)
{
    ExternalEvent(paramName, eventPlayer, CommandOrigin::Internal, 0);
}

void ChatCommandTrigger::ExternalEvent(std::string const paramName, Player* eventPlayer, CommandOrigin eventOrigin,
                                       uint64 humanOwner)
{
    param = paramName;
    ownerGuid = eventPlayer ? eventPlayer->GetGUID().GetRawValue() : 0;
    origin = eventOrigin;
    admittedHumanOwner = humanOwner;
    triggered = true;
}

Event ChatCommandTrigger::Check()
{
    if (!triggered)
        return Event();

    Player* owner = ownerGuid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(ownerGuid)) : nullptr;
    if (ownerGuid && (!owner || !owner->IsInWorld()))
        return Event();

    Event event(getName(), param, owner);
    event.SetCommandOrigin(origin, admittedHumanOwner);
    return event;
}

void ChatCommandTrigger::Reset()
{
    triggered = false;
    ownerGuid = 0;
    origin = CommandOrigin::Internal;
    admittedHumanOwner = 0;
}
