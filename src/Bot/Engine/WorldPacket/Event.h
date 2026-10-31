/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_EVENT_H
#define PLAYERBOTS_EVENT_H

#include "WorldPacket.h"

class ObjectGuid;
class Player;

enum class CommandOrigin
{
    Human,
    Internal
};

class Event
{
public:
    Event(Event const& other)
        : source(other.source), param(other.param), packet(other.packet), ownerGuid(other.ownerGuid),
          commandOrigin(other.commandOrigin), admittedHumanOwner(other.admittedHumanOwner) {}
    Event() {}
    Event(std::string const source) : source(source) {}
    Event(std::string const source, std::string const param, Player* owner = nullptr);
    Event(std::string const source, WorldPacket& packet, Player* owner = nullptr);
    Event(std::string const source, ObjectGuid object, Player* owner = nullptr);
    virtual ~Event() {}

    std::string const GetSource() { return source; }
    std::string const getParam() { return param; }
    WorldPacket& getPacket() { return packet; }
    ObjectGuid getObject();
    Player* getOwner();
    void SetCommandOrigin(CommandOrigin origin, uint64 humanOwner = 0)
    {
        commandOrigin = origin;
        admittedHumanOwner = origin == CommandOrigin::Human ? humanOwner : 0;
    }
    CommandOrigin GetCommandOrigin() const { return commandOrigin; }
    uint64 GetAdmittedHumanOwner() const { return admittedHumanOwner; }
    bool operator!() const { return source.empty(); }

protected:
    std::string source;
    std::string param;
    WorldPacket packet;
    uint64 ownerGuid = 0;
    CommandOrigin commandOrigin = CommandOrigin::Internal;
    uint64 admittedHumanOwner = 0;
};

#endif
