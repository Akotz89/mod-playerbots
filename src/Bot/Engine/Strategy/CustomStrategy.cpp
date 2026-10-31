/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CustomStrategy.h"
#include "Playerbots.h"
#include <regex>
#include <stdexcept>

std::map<std::string, std::string> CustomStrategy::actionLinesCache;

NextAction toNextAction(std::string const action)
{
    std::vector<std::string> tokens = split(action, '!');

    if (tokens.empty() || tokens[0].empty())
        throw std::invalid_argument("Invalid action");

    if (tokens.size() == 2)
        return NextAction(tokens[0], atof(tokens[1].c_str()));

    if (tokens.size() == 1)
        return NextAction(tokens[0], ACTION_NORMAL);

    LOG_ERROR("playerbots", "Invalid action {}", action.c_str());

    throw std::invalid_argument("Invalid action");
}

std::vector<NextAction> toNextActionArray(const std::string actions)
{
    const std::vector<std::string> tokens = split(actions, ',');
    std::vector<NextAction> res = {};

    for (const std::string& token : tokens)
        res.push_back(toNextAction(token));

    return res;
}

TriggerNode* toTriggerNode(std::string const actionLine)
{
    std::vector<std::string> tokens = split(actionLine, '>');
    if (tokens.size() == 2)
        return new TriggerNode(tokens[0], toNextActionArray(tokens[1]));

    LOG_ERROR("playerbots", "Invalid action line {}", actionLine.c_str());
    return nullptr;
}

CustomStrategy::CustomStrategy(PlayerbotAI* botAI) : Strategy(botAI), Qualified() {}

void CustomStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    if (!loaded)
    {
        if (actionLinesCache[qualifier].empty())
        {
            if (!Reload())
            {
                LOG_ERROR("playerbots", "Could not load custom strategy {}", qualifier);
            }
        }
        else
        {
            actionLines.clear();
            std::vector<std::string> tokens = split(actionLinesCache[qualifier], '\n');
            std::regex tpl("\\(nullptr,\\s*'.+',\\s*'(.+)'\\)(,|;)");
            for (std::vector<std::string>::iterator i = tokens.begin(); i != tokens.end(); ++i)
            {
                std::string const line = *i;
                for (std::sregex_iterator j = std::sregex_iterator(line.begin(), line.end(), tpl);
                     j != std::sregex_iterator(); ++j)
                {
                    std::smatch match = *j;
                    std::string const actionLine = match[1].str();
                    if (!actionLine.empty())
                        actionLines.push_back(actionLine);
                }
            }
            loaded = true;
        }
    }

    for (std::vector<std::string>::iterator i = actionLines.begin(); i != actionLines.end(); ++i)
    {
        try
        {
            if (TriggerNode* tn = toTriggerNode(*i))
                triggers.push_back(tn);
        }
        catch (std::invalid_argument const&)
        {
            LOG_ERROR("playerbots", "Invalid custom strategy action line {}", *i);
        }
    }
}

bool CustomStrategy::Reload()
{
    std::vector<std::string> lines;
    if (!LoadActionLines((uint32)botAI->GetBot()->GetGUID().GetCounter(), lines) ||
        (lines.empty() && !LoadActionLines(0, lines)))
        return false;

    actionLines.swap(lines);
    loaded = true;
    return true;
}

bool CustomStrategy::LoadActionLines(uint32 owner, std::vector<std::string>& lines)
{
    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_COUNT_BY_OWNER_AND_NAME);
    stmt->SetData(0, owner);
    stmt->SetData(1, qualifier);
    PreparedQueryResult count = PlayerbotsDatabase.Query(stmt);
    if (!count)
        return false;
    uint64 expected = count->Fetch()[0].Get<uint64>();
    if (expected == 0)
        return true;

    stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_CUSTOM_STRATEGY_BY_OWNER_AND_NAME);
    stmt->SetData(0, owner);
    stmt->SetData(1, qualifier);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return false;

    do
    {
        lines.push_back(result->Fetch()[1].Get<std::string>());
    } while (result->NextRow());
    return lines.size() == expected;
}

void CustomStrategy::Reset()
{
    actionLinesCache[qualifier].clear();
    loaded = false;
}
