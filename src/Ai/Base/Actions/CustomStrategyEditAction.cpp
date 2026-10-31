/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CustomStrategyEditAction.h"
#include "CustomStrategy.h"
#include "Event.h"
#include "Playerbots.h"
#include <charconv>
#include <limits>
#include <set>

bool CustomStrategyEditAction::Execute(Event event)
{
    std::string text = event.getParam();
    size_t pos = text.find(" ");
    if (pos == std::string::npos)
        return PrintHelp();

    std::string const name = text.substr(0, pos);
    text = text.substr(pos + 1);

    pos = text.find(" ");
    if (pos == std::string::npos)
        pos = text.size();

    std::string const idx = text.substr(0, pos);
    text = pos >= text.size() ? "" : text.substr(pos + 1);

    if (idx == "?")
        return Print(name);

    uint32 index = 0;
    char const* begin = idx.data();
    if (!idx.empty() && idx.front() == '+')
        ++begin;
    auto const [end, error] = std::from_chars(begin, idx.data() + idx.size(), index);
    if (error != std::errc() || end != idx.data() + idx.size() || index > std::numeric_limits<int32>::max())
        return PrintHelp();

    return Edit(name, index, text);
}

bool CustomStrategyEditAction::PrintHelp()
{
    botAI->TellMaster("=== Custom strategies ===");

    uint32 owner = botAI->GetBot()->GetGUID().GetCounter();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_BY_OWNER);
    stmt->SetData(0, owner);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            std::string const name = fields[0].Get<std::string>();
            botAI->TellMaster(name);
        } while (result->NextRow());
    }

    botAI->TellMaster("Usage: cs <name> <idx> <command>");
    return false;
}

bool CustomStrategyEditAction::Print(std::string const name)
{
    uint32 owner = botAI->GetBot()->GetGUID().GetCounter();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_COUNT_BY_OWNER_AND_NAME);
    stmt->SetData(0, owner);
    stmt->SetData(1, name);
    PreparedQueryResult count = PlayerbotsDatabase.Query(stmt);
    if (!count || count->GetRowCount() != 1 || count->Fetch()[0].IsNull())
    {
        botAI->TellMaster("Custom strategy could not be read");
        return false;
    }
    uint64 expected = count->Fetch()[0].Get<uint64>();

    PreparedQueryResult result;
    if (expected != 0)
    {
        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_BY_OWNER_AND_NAME);
        stmt->SetData(0, owner);
        stmt->SetData(1, name);
        result = PlayerbotsDatabase.Query(stmt);
        if (!result || result->GetRowCount() != expected)
        {
            botAI->TellMaster("Custom strategy could not be read");
            return false;
        }
    }

    std::ostringstream out;
    out << "=== " << name << " ===";
    botAI->TellMaster(out.str());

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 idx = fields[0].Get<uint32>();
            std::string const action = fields[1].Get<std::string>();

            PrintActionLine(idx, action);
        } while (result->NextRow());
    }

    return true;
}

bool CustomStrategyEditAction::Edit(std::string const name, uint32 idx, std::string const command)
{
    uint32 owner = botAI->GetBot()->GetGUID().GetCounter();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_COUNT);
    stmt->SetData(0, owner);
    stmt->SetData(1, name);
    stmt->SetData(2, idx);
    PreparedQueryResult existing = PlayerbotsDatabase.Query(stmt);
    if (!existing)
    {
        botAI->TellMaster("Custom strategy could not be saved");
        return false;
    }
    AiObjectContext* context = botAI->GetAiObjectContext();
    std::set<CustomStrategy*> strategies;
    std::set<std::string> names{name};
    for (std::string const& created : context->GetStrategies())
    {
        if (CustomStrategy* strategy = dynamic_cast<CustomStrategy*>(context->GetStrategy(created)))
        {
            strategies.insert(strategy);
            names.insert(strategy->getQualifier());
        }
    }
    for (auto const& entry : CustomStrategy::actionLinesCache)
        if (!entry.second.empty())
            names.insert(entry.first);

    std::set<std::string> affectedNames;
    for (std::string const& candidate : names)
    {
        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_NAMES_EQUAL);
        stmt->SetData(0, candidate);
        stmt->SetData(1, candidate);
        stmt->SetData(2, candidate);
        stmt->SetData(3, name);
        PreparedQueryResult equal = PlayerbotsDatabase.Query(stmt);
        if (!equal || equal->GetRowCount() != 1 || equal->Fetch()[0].IsNull())
        {
            botAI->TellMaster("Custom strategy could not be saved");
            return false;
        }
        uint64 matches = equal->Fetch()[0].Get<uint64>();
        if (matches > 1)
        {
            botAI->TellMaster("Custom strategy could not be saved");
            return false;
        }
        if (matches == 1)
            affectedNames.insert(candidate);
    }

    for (auto i = strategies.begin(); i != strategies.end();)
    {
        if (!affectedNames.count((*i)->getQualifier()))
            i = strategies.erase(i);
        else
            ++i;
    }
    if (CustomStrategy* strategy = dynamic_cast<CustomStrategy*>(context->GetStrategy("custom::" + name)))
        strategies.insert(strategy);

    if (existing->Fetch()[0].Get<uint64>() != 0)
    {
        if (command.empty())
        {
            stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_CUSTOM_STRATEGY);
            stmt->SetData(0, name);
            stmt->SetData(1, owner);
            stmt->SetData(2, idx);
            PlayerbotsDatabase.DirectExecute(stmt);
        }
        else
        {
            stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_CUSTOM_STRATEGY);
            stmt->SetData(0, command);
            stmt->SetData(1, name);
            stmt->SetData(2, owner);
            stmt->SetData(3, idx);
            PlayerbotsDatabase.DirectExecute(stmt);
        }
    }
    else if (!command.empty())
    {
        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_CUSTOM_STRATEGY);
        stmt->SetData(0, name);
        stmt->SetData(1, owner);
        stmt->SetData(2, idx);
        stmt->SetData(3, command);
        PlayerbotsDatabase.DirectExecute(stmt);
    }

    for (CustomStrategy* strategy : strategies)
        strategy->Reset();
    for (std::string const& affected : affectedNames)
        CustomStrategy::actionLinesCache[affected].clear();

    stmt = PlayerbotsDatabase.GetPreparedStatement(command.empty() ? PLAYERBOTS_SEL_CUSTOM_STRATEGY_COUNT :
                                                    PLAYERBOTS_SEL_CUSTOM_STRATEGY_BY_OWNER_AND_NAME_AND_IDX);
    stmt->SetData(0, owner);
    stmt->SetData(1, name);
    stmt->SetData(2, idx);
    PreparedQueryResult saved = PlayerbotsDatabase.Query(stmt);
    bool verified = bool(saved);
    if (saved)
    {
        if (command.empty())
            verified = saved->Fetch()[0].Get<uint64>() == 0;
        else
        {
            do
            {
                if (saved->Fetch()[0].Get<std::string>() != command)
                {
                    verified = false;
                    break;
                }
            } while (saved->NextRow());
        }
    }
    if (!verified)
    {
        botAI->TellMaster("Custom strategy save could not be verified; current engine rules retained");
        return false;
    }

    for (CustomStrategy* strategy : strategies)
    {
        if (!strategy->Reload())
        {
            botAI->TellMaster("Custom strategy saved, but reload failed; current engine rules retained");
            return false;
        }
    }
    if (!strategies.empty())
        botAI->ReInitCurrentEngine();

    PrintActionLine(idx, command);

    botAI->TellMaster("Custom strategy saved");

    return true;
}

bool CustomStrategyEditAction::PrintActionLine(uint32 idx, std::string const command)
{
    std::ostringstream out;
    out << "#" << idx << " " << command;
    botAI->TellMaster(out.str());
    return true;
}
