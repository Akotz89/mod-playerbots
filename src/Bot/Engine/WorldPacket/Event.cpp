/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Event.h"
#include "Playerbots.h"
#include "ObjectAccessor.h"

Event::Event(std::string const source, std::string const param, Player* owner)
    : source(source), param(param), ownerGuid(owner ? owner->GetGUID().GetRawValue() : 0)
{
}

Event::Event(std::string const source, WorldPacket& packet, Player* owner)
    : source(source), packet(packet), ownerGuid(owner ? owner->GetGUID().GetRawValue() : 0)
{
}

Event::Event(std::string const source, ObjectGuid object, Player* owner)
    : source(source), ownerGuid(owner ? owner->GetGUID().GetRawValue() : 0)
{
    packet << object;
}

Player* Event::getOwner()
{
    return ownerGuid ? ObjectAccessor::FindConnectedPlayer(ObjectGuid(ownerGuid)) : nullptr;
}

ObjectGuid Event::getObject()
{
    if (packet.empty())
        return ObjectGuid::Empty;

    WorldPacket p(packet);
    p.rpos(0);

    ObjectGuid guid;
    p >> guid;

    return guid;
}
