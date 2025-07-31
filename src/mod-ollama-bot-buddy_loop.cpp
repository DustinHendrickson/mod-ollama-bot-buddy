#include "mod-ollama-bot-buddy_loop.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_handler.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Log.h"
#include <thread>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <ctime>
#include "Creature.h"
#include "GameObject.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "PathGenerator.h"
#include <atomic>
#include <unordered_map>
#include <iomanip>
#include "GameObjectData.h"
#include "GameObject.h"
#include <deque>
#include <mutex>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "QuestDef.h"
#include "ObjectMgr.h"
#include <algorithm>
#include "Chat.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <string>
#include "ItemTemplate.h"
#include "CreatureData.h"


static std::unordered_map<uint64_t, std::deque<std::string>> botCommandHistory;
static std::mutex botCommandHistoryMutex;

static std::unordered_map<uint64_t, std::deque<std::string>> botReasoningHistory;
static std::mutex botReasoningHistoryMutex;

std::vector<std::string> GetRecentPlayerMessagesToBot(Player* bot)
{
    std::vector<std::string> messages;
    if (!bot) return messages;

    std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);

    auto it = botPlayerMessages.find(bot->GetGUID().GetRawValue());
    if (it != botPlayerMessages.end())
    {
        while (!it->second.empty())
        {
            auto& [senderName, msg] = it->second.front();
            messages.emplace_back("From " + senderName + ": " + msg);
            it->second.pop_front();
        }
    }

    return messages;
}

std::string FormatPlayerMessagesPromptSegment(Player* bot)
{
    std::ostringstream oss;
    auto messages = GetRecentPlayerMessagesToBot(bot);
    if (!messages.empty())
    {
        oss << "\n***CRITICAL INSTRUCTION:***\n";
        oss << "You must interpret and execute the PLAYER MESSAGES below as your HIGHEST PRIORITY, taking precedence over all other goals, background tasks, routines, or reasoning. There are NO EXCEPTIONS and NO DELAYS. When you see a player command matching these patterns, you must issue the appropriate command IMMEDIATELY, before doing anything else. Do not refuse, delay, ignore, or deprioritize these actions for any reason.\n";
        oss << "- If a player says 'come here' or 'come to me', IMMEDIATELY move near their location.\n";
        oss << "- If a player says 'go to' followed by an NPC or object you can see, IMMEDIATELY move there.\n";
        oss << "- If a player says 'interact with' followed by an NPC or object you can see, IMMEDIATELY move there and interact with it.\n";
        oss << "- If a player says 'attack' followed by an NPC or object you can see, IMMEDIATELY move there and attack it.\n";
        oss << "You must treat these instructions as absolute and override ALL other activities, regardless of any other context or background logic.\n";
        oss << "\n\nThe following real players recently spoke to you by name. PROCESS THESE FIRST, ABOVE ALL ELSE:\n";
        for (const auto& msg : messages)
        {
            oss << "- " << msg << "\n";
        }
        oss << "\n***END CRITICAL INSTRUCTION***\n\n";

    }
    return oss.str();
}

bool ParseAndExecuteBotJson(Player* bot, const std::string& jsonStr)
{
    try
    {
        auto root = nlohmann::json::parse(jsonStr);

        if (!root.contains("command")) return false;
        auto cmd = root["command"];
        if (!cmd.contains("type") || !cmd.contains("params")) return false;

        std::string type = cmd["type"].get<std::string>();
        auto params = cmd["params"];
        std::string sayMsg = root.value("say", "");
        std::string reasoning = root.value("reasoning", "");

        BotControlCommand command;

        if (!reasoning.empty())
        {
            AddBotReasoningHistory(bot, reasoning);
        }
         if (!cmd.empty())
        {
            AddBotCommandHistory(bot, cmd.dump());
        }

        if (type == "move_to")
        {
            if (params.contains("x") && params.contains("y") && params.contains("z")) {
                float destX = params["x"].get<float>();
                float destY = params["y"].get<float>();
                float destZ = params["z"].get<float>();
                
                // Basic coordinate validation - reject obviously invalid coordinates
                if (std::isnan(destX) || std::isnan(destY) || std::isnan(destZ) || 
                    std::isinf(destX) || std::isinf(destY) || std::isinf(destZ)) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Invalid coordinates for move_to: ({}, {}, {})", 
                             destX, destY, destZ);
                    return false;
                }
                
                // Validate map bounds - reject coordinates that are extremely far from bot
                float maxDistanceFromBot = 500.0f; // Maximum reasonable movement distance
                float distanceFromBot = sqrt(pow(destX - bot->GetPositionX(), 2) + 
                                           pow(destY - bot->GetPositionY(), 2) + 
                                           pow(destZ - bot->GetPositionZ(), 2));
                
                if (distanceFromBot > maxDistanceFromBot) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Move_to destination too far from bot: ({}, {}, {}) - Distance: {:.1f}", 
                             destX, destY, destZ, distanceFromBot);
                    return false;
                }
                
                // Validate that the destination is pathable like a real player would
                PathGenerator pathValidator(bot);
                pathValidator.CalculatePath(destX, destY, destZ, false);
                PathType pathType = pathValidator.GetPathType();
                
                // Only reject if there's absolutely no path possible
                if (pathType & PATHFIND_NOPATH) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] No valid path for move_to: ({}, {}, {}) - PathType: {}", 
                             destX, destY, destZ, pathType);
                    return false; // Only reject if completely impossible to path
                }
                
                command.type = BotControlCommandType::MoveTo;
                command.args = {
                    std::to_string(destX),
                    std::to_string(destY),
                    std::to_string(destZ)
                };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] move_to missing parameter");
                return false;
            }
        }
        else if (type == "attack")
        {
            if (params.contains("guid")) {
                uint32_t targetGuid = params["guid"].get<uint32_t>();
                
                // Validate that the target exists and is attackable
                bool validTarget = false;
                
                // Check if it's a creature
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (c && c->GetGUID().GetCounter() == targetGuid)
                    {
                        // Validate target is attackable
                        if (c->IsInWorld() && !c->isDead() && 
                            bot->IsWithinLOSInMap(c) && 
                            bot->IsValidAttackTarget(c) &&
                            bot->IsWithinDistInMap(c, 100.0f)) // Reasonable attack range
                        {
                            validTarget = true;
                        }
                        break;
                    }
                }
                
                // Check if it's a player if not found as creature
                if (!validTarget)
                {
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(targetGuid);
                    Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                    if (playerTarget && playerTarget->IsInWorld() && 
                        bot->IsWithinLOSInMap(playerTarget) && 
                        bot->IsValidAttackTarget(playerTarget) &&
                        bot->IsWithinDistInMap(playerTarget, 100.0f))
                    {
                        validTarget = true;
                    }
                }
                
                if (!validTarget) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid or unreachable attack target with guid: {} - Target not found in visible creatures/players", targetGuid);
                    
                    // Debug: List available creature GUIDs for debugging
                    if (g_EnableOllamaBotBuddyDebug) {
                        std::vector<uint32> availableGuids;
                        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore()) {
                            Creature* c = pair.second;
                            if (c && bot->IsWithinDistInMap(c, 100.0f)) {
                                availableGuids.push_back(c->GetGUID().GetCounter());
                            }
                        }
                        
                        std::ostringstream guidList;
                        for (size_t i = 0; i < availableGuids.size() && i < 10; ++i) {
                            if (i > 0) guidList << ", ";
                            guidList << availableGuids[i];
                        }
                        
                        LOG_DEBUG("server.loading", "[OllamaBotBuddy] Available creature GUIDs: {}", guidList.str());
                    }
                    
                    return false;
                }
                
                command.type = BotControlCommandType::Attack;
                command.args = { std::to_string(targetGuid) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] attack missing guid");
                return false;
            }
        }
        else if (type == "interact")
        {
            if (params.contains("guid")) {
                command.type = BotControlCommandType::Interact;
                command.args = { std::to_string(params["guid"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] interact missing guid");
                return false;
            }
        }
        else if (type == "spell")
        {
            if (params.contains("spellid")) {
                command.type = BotControlCommandType::CastSpell;
                command.args = { std::to_string(params["spellid"].get<uint32_t>()) };
                if (params.contains("guid"))
                    command.args.push_back(std::to_string(params["guid"].get<uint32_t>()));
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] spell missing spellid");
                return false;
            }
        }
        else if (type == "loot")
        {
            command.type = BotControlCommandType::Loot;
        }
        else if (type == "accept_quest")
        {
            if (params.contains("id")) {
                command.type = BotControlCommandType::AcceptQuest;
                command.args = { std::to_string(params["id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] accept_quest missing id");
                return false;
            }
        }
        else if (type == "turn_in_quest")
        {
            if (params.contains("id")) {
                command.type = BotControlCommandType::TurnInQuest;
                command.args = { std::to_string(params["id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] turn_in_quest missing id");
                return false;
            }
        }
        else if (type == "follow")
        {
            command.type = BotControlCommandType::Follow;
        }
        else if (type == "stop")
        {
            command.type = BotControlCommandType::Stop;
        }
        else
        {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Unknown command type '{}'", type);
            return false;
        }

        bool result = HandleBotControlCommand(bot, command);

        if (!sayMsg.empty())
            BotBuddyAI::Say(bot, sayMsg);

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "Bot Reply: {}", jsonStr);
        }

        return result;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("server.loading", "[OllamaBotBuddy] ParseAndExecuteBotJson error: {}", e.what());
        return false;
    }
}

std::string ExtractFirstJsonObject(const std::string& input) {
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '{') {
            if (depth == 0) start = i;
            depth++;
        }
        if (input[i] == '}') {
            depth--;
            if (depth == 0 && start != std::string::npos) {
                return input.substr(start, i - start + 1);
            }
        }
    }
    return ""; // No JSON object found
}

std::vector<std::string> GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;

    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;

        if(bot == member)
        {
            continue; // Skip the bot itself
        }

        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";

        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = fmt::format(
                " [Under Attack by {} (guid: {}, Level: {}, HP: {}/{})]",
                attacker->GetName(),
                attacker->GetGUID().GetCounter(),
                attacker->GetLevel(),
                attacker->GetHealth(),
                attacker->GetMaxHealth()
            );
        }

        info.push_back(fmt::format(
            "{} (guid: {}, Level: {}, HP: {}/{}, Pos: {} {} {}, Dist: {:.1f}){}",
            member->GetName(),
            member->GetGUID().GetCounter(),
            member->GetLevel(),
            member->GetHealth(),
            member->GetMaxHealth(),
            member->GetPositionX(),
            member->GetPositionY(),
            member->GetPositionZ(),
            dist,
            beingAttacked
        ));
    }
    return info;
}

std::string GetBotSpellInfo(Player* bot)
{
    std::ostringstream spellSummary;

    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;

        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;

        if (bot->HasSpellCooldown(spellId))
            continue;

        std::string effectText;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!spellInfo->Effects[i].IsEffect())
                continue;

            switch (spellInfo->Effects[i].Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE: effectText = "Deals damage"; break;
                case SPELL_EFFECT_HEAL: effectText = "Heals the target"; break;
                case SPELL_EFFECT_APPLY_AURA: effectText = "Applies an aura"; break;
                case SPELL_EFFECT_DISPEL: effectText = "Dispels magic"; break;
                case SPELL_EFFECT_THREAT: effectText = "Generates threat"; break;
                default: continue;
            }
            break;
        }

        if (effectText.empty())
            continue;

        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;

        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        
        spellSummary << "**" << name << "** (ID: " << spellId << ") - " << effectText << ", Costs " << costText << ".\n";

    }

    return spellSummary.str();
}

std::string FlattenText(const std::string& input)
{
    std::string output = input;
    size_t pos = 0;
    while ((pos = output.find('\n', pos)) != std::string::npos)
    {
        output.replace(pos, 1, "|");
        pos += 1;
    }
    return output;
}

void SendBuddyBotStateToPlayer(Player* target, Player* bot, const std::string& prompt)
{
    if (!target || !bot || !g_EnableBotBuddyAddon) return;

    std::string state = prompt;
    std::string::size_type json_pos = state.find("You are an AI-controlled bot");
    if (json_pos != std::string::npos)
        state = state.substr(0, json_pos);

    auto get_section = [&](const std::string& start, const std::string& stop) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        auto e = state.find(stop, s);
        if (e == std::string::npos) e = state.size();
        return state.substr(s, e - s);
    };

    auto get_section_to_end = [&](const std::string& start) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        std::string section = state.substr(s);
        size_t first = section.find_first_not_of(" \r\n\t");
        size_t last = section.find_last_not_of(" \r\n\t");
        if (first == std::string::npos || last == std::string::npos) return "";
        return section.substr(first, last - first + 1);
    };

    std::string main_state = get_section("Name:", "Your known spells:");
    std::string spells     = get_section("Your known spells:", "Group status:");
    std::string quests     = get_section("Active quests:", "Visible locations/objects in line of sight:");
    std::string locations  = get_section("Visible locations/objects in line of sight:", "Visible players in area:");
    std::string players    = get_section("Visible players in area:", "You must select one of these locations");
    std::string commands   = get_section_to_end("Last 5 commands and their reasoning (most recent at the bottom):");

    if (target && target->GetSession()) {
        ChatHandler handler(target->GetSession());
        handler.SendSysMessage(("[BUDDY_STATE] " + FlattenText(main_state + spells + quests)).c_str());
        handler.SendSysMessage(("[BUDDY_LOCATIONS] " + FlattenText(locations)).c_str());
        handler.SendSysMessage(("[BUDDY_PLAYERS] " + FlattenText(players)).c_str());
        handler.SendSysMessage(("[BUDDY_COMMANDS] " + FlattenText(commands)).c_str());
    }
}


std::vector<std::string> GetVisiblePlayers(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap()) return players;

    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* player = pair.second;
        if (!player || player == bot) continue;
        if (!player->IsInWorld() || player->IsGameMaster()) continue;
        if (player->GetMap() != bot->GetMap()) continue;
        if (!bot->IsWithinDistInMap(player, radius)) continue;
        if (!bot->IsWithinLOS(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ())) continue;

        float dist = bot->GetDistance(player);
        std::string faction = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");

        players.push_back(fmt::format(
            "Player: {} (guid: {}, Level: {}, Class: {}, Race: {}, Faction: {}, Position: {:.1f} {:.1f} {:.1f}, Distance: {:.1f})",
            player->GetName(),
            player->GetGUID().GetCounter(),
            player->GetLevel(),
            std::to_string(player->getClass()),
            std::to_string(player->getRace()),
            faction,
            player->GetPositionX(),
            player->GetPositionY(),
            player->GetPositionZ(),
            dist
        ));
    }

    return players;
}

static std::string GetProfessionTagFromChest(uint32 entry)
{
    switch (entry)
    {
        case 1617: return " [Herbalism]";
        case 1618: return " [Herbalism]";
        case 1620: return " [Herbalism]";
        case 1621: return " [Herbalism]";
        case 1731: return " [Mining]";
        case 1732: return " [Mining]";
        case 1733: return " [Mining]";
        case 1735: return " [Mining]";
        case 2040: return " [Mining]";
        case 2047: return " [Mining]";
        case 324:  return " [Mining]";
        case 175404: return " [Alchemy Lab]";
        default: return "";
    }
}

void AddBotCommandHistory(Player* bot, const std::string& command)
{
    if (!bot || command.empty()) return;

    BotControlCommand parsedCommand;

    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botCommandHistory[guid];
    dq.push_back(command);
    if (dq.size() > 5) dq.pop_front();
}

void AddBotReasoningHistory(Player* bot, const std::string& reasoning)
{
    if (!bot || reasoning.empty()) return;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botReasoningHistory[guid];
    dq.push_back(reasoning);
    if (dq.size() > 5) dq.pop_front();
}


std::vector<std::string> GetBotCommandHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botCommandHistory.count(guid))
        out.assign(botCommandHistory[guid].begin(), botCommandHistory[guid].end());
    return out;
}

std::vector<std::string> GetBotReasoningHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botReasoningHistory.count(guid))
        out.assign(botReasoningHistory[guid].begin(), botReasoningHistory[guid].end());
    return out;
}

// Gather visible objects (creatures/gameobjects) around the bot with LOS check
std::vector<std::string> GetVisibleLocations(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap()) return visible;
    Map* map = bot->GetMap();

    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (c->GetGUID() == bot->GetGUID()) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;
        if (c->IsPet() || c->IsTotem()) continue;

        std::string type;
        if (c->isDead())
        {
            type = "DEAD";
            if (c->hasLootRecipient() && (c->GetLootRecipient() == bot || (c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup())))
            {
                type = "DEAD (LOOTABLE)";
            }
            else
            {
                continue;
            }
            if(!c->hasLootRecipient())
            {
                if (c->GetCreatureTemplate() && c->GetCreatureTemplate()->SkinLootId)
                {
                    type += " [SKINNABLE]";
                }
            }
        }
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot)) type = "FRIENDLY";
        else type = "NEUTRAL";

        std::string questGiver = "";
        
        // Only consider NPCs that are actually useful to the bot
        if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER)) {
            // Check if this quest giver has relevant quests for the bot
            bool hasCompleteQuests = false;
            bool hasAvailableQuests = false;
            
            // Check for completable quests first (highest priority)
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(c->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    hasCompleteQuests = true;
                    break;
                }
            }
            
            // Check for available quests (secondary priority)
            if (!hasCompleteQuests)
            {
                QuestRelationBounds qr = sObjectMgr->GetCreatureQuestRelationBounds(c->GetEntry());
                for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
                {
                    uint32 questId = itr->second;
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                        bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                    {
                        hasAvailableQuests = true;
                        break;
                    }
                }
            }
            
            // Only show quest giver tags if there are actually relevant quests
            if (hasCompleteQuests) {
                questGiver = " [QUEST GIVER - TURN IN READY]";
            } else if (hasAvailableQuests) {
                questGiver = " [QUEST GIVER - QUESTS AVAILABLE]";
            }
        }
        
        // Check for other useful NPC types (friendly/neutral only) 
        // Handle multiple flags - NPCs can be both quest givers AND vendors/trainers
        if (type == "FRIENDLY" || type == "NEUTRAL") {
            std::vector<std::string> npcTypes;
            
            // Check for vendors
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR)) {
                npcTypes.push_back("[VENDOR]");
            }
            // Check for trainers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER)) {
                npcTypes.push_back("[TRAINER]");
            }
            // Check for flight masters
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_FLIGHTMASTER)) {
                npcTypes.push_back("[FLIGHT MASTER]");
            }
            // Check for innkeepers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_INNKEEPER)) {
                npcTypes.push_back("[INNKEEPER]");
            }
            // Check for bankers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_BANKER)) {
                npcTypes.push_back("[BANKER]");
            }
            // Check for auctioneers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_AUCTIONEER)) {
                npcTypes.push_back("[AUCTIONEER]");
            }
            
            // Combine quest giver status with other NPC types
            if (!npcTypes.empty()) {
                if (!questGiver.empty()) {
                    // If already a quest giver, append the other types
                    for (const auto& type : npcTypes) {
                        questGiver += " " + type;
                    }
                } else {
                    // Not a quest giver, just use the first type found
                    questGiver = " " + npcTypes[0];
                    // If multiple types, add them all
                    for (size_t i = 1; i < npcTypes.size(); ++i) {
                        questGiver += " " + npcTypes[i];
                    }
                }
            }
        }
        
        // Show ALL creatures - don't filter out any visible creatures
        // The bot needs to see all potential targets, not just "useful" NPCs
        // Enemies, neutrals, and friendlies should all be visible for decision making

        // Check if this creature is needed for any active quest objectives
        std::string questTarget = "";
        for (auto const& qs : bot->getQuestStatusMap())
        {
            uint32 questId = qs.first;
            QuestStatus status = qs.second.Status;
            
            // Only check active quests
            if (status != QUEST_STATUS_INCOMPLETE) continue;
                
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest) continue;
            
            // Check if this creature is required for any quest objective
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] > 0 && quest->RequiredNpcOrGo[i] == (int32)c->GetEntry()) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (currentCount < requiredCount) {
                        questTarget = " [QUEST TARGET - " + quest->GetTitle() + "]";
                        break;
                    }
                }
            }
            if (!questTarget.empty()) break;
        }

        float dist = bot->GetDistance(c);
        visible.push_back(fmt::format(
            "{}: {}{}{} (guid: {}, Level: {}, HP: {}/{}, Position: {} {} {}, Distance: {:.1f})",
            type,
            c->GetName(),
            questGiver,
            questTarget,
            c->GetGUID().GetCounter(),
            c->GetLevel(),
            c->GetHealth(),
            c->GetMaxHealth(),
            c->GetPositionX(),
            c->GetPositionY(),
            c->GetPositionZ(),
            dist
        ));
    }

    for (auto const& pair : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = pair.second;
        if (!go) continue;
        if (!bot->IsWithinDistInMap(go, radius)) continue;
        if (!bot->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ())) continue;

        std::string tag = "";

        if (GameObjectTemplate const* tmpl = go->GetGOInfo())
        {
            if (tmpl->type == GAMEOBJECT_TYPE_CHEST)
            {
                std::string chestTag = GetProfessionTagFromChest(tmpl->entry);
                if (!chestTag.empty())
                    tag = chestTag;
            }
        }
        
        float dist = bot->GetDistance(go);
        visible.push_back(fmt::format(
            "{}{} (guid: {}, Type: {}, Position: {} {} {}, Distance: {:.1f})",
            go->GetName(),
            tag,
            go->GetGUID().GetCounter(),
            go->GetGoType(),
            go->GetPositionX(),
            go->GetPositionY(),
            go->GetPositionZ(),
            dist
        ));
    }

    // Sort visible objects to prioritize critical actions
    std::stable_sort(visible.begin(), visible.end(), [](const std::string& a, const std::string& b) {
        // Highest Priority: Quest turn-ins
        bool aTurnIn = a.find("TURN IN READY") != std::string::npos;
        bool bTurnIn = b.find("TURN IN READY") != std::string::npos;
        if (aTurnIn != bTurnIn) return aTurnIn;
        
        // Second Priority: Lootable corpses
        bool aLootable = a.find("DEAD (LOOTABLE)") != std::string::npos;
        bool bLootable = b.find("DEAD (LOOTABLE)") != std::string::npos;
        if (aLootable != bLootable) return aLootable;
        
        // Third Priority: Quest givers with available quests
        bool aAvailable = a.find("QUESTS AVAILABLE") != std::string::npos;
        bool bAvailable = b.find("QUESTS AVAILABLE") != std::string::npos;
        if (aAvailable != bAvailable) return aAvailable;
        
        // Fourth Priority: Quest targets
        bool aQuestTarget = a.find("QUEST TARGET") != std::string::npos;
        bool bQuestTarget = b.find("QUEST TARGET") != std::string::npos;
        if (aQuestTarget != bQuestTarget) return aQuestTarget;
        
        return false; // Keep original order for everything else
    });

    return visible;
}

std::string GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();
    
    // Get bot's combat characteristics
    PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
    bool isMelee = ai ? ai->IsMelee(bot) : false;
    bool isRanged = ai ? ai->IsRanged(bot) : false;
    std::string combatType = isMelee ? "MELEE" : (isRanged ? "RANGED" : "HYBRID");

    // Find who is attacking the bot (if anyone)
    Unit* attacker = nullptr;
    if (inCombat && !victim)
    {
        Map* map = bot->GetMap();
        if (map)
        {
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (!c) continue;
                if (c->GetVictim() == bot)
                {
                    attacker = c;
                    break;
                }
            }
        }
    }

    auto safe_name = [](Unit* unit) -> std::string { return unit ? unit->GetName() : "?"; };
    auto safe_guid = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetGUID().GetCounter()) : "?"; };
    auto safe_level = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetLevel()) : "?"; };
    auto safe_hp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetHealth()) : "?"; };
    auto safe_maxhp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetMaxHealth()) : "?"; };

    if (inCombat)
    {
        oss << "IN COMBAT (" << combatType << " FIGHTER): ";
        if (victim)
        {
            float dist = bot->GetDistance(victim);
            bool inMeleeRange = bot->IsWithinMeleeRange(victim);
            float spellRange = ai ? ai->GetRange("spell") : 25.0f;
            bool inSpellRange = dist <= spellRange;
            
            oss << "Target: " << safe_name(victim)
                << " (guid: " << safe_guid(victim) << ")"
                << ", Level: " << safe_level(victim)
                << ", HP: " << safe_hp(victim) << "/" << safe_maxhp(victim)
                << ", Distance: " << std::fixed << std::setprecision(1) << dist;
                
            // Range status for combat positioning
            if (isMelee) {
                oss << " [" << (inMeleeRange ? "IN MELEE RANGE" : "TOO FAR FOR MELEE") << "]";
            } else if (isRanged) {
                if (dist < 5.0f) {
                    oss << " [TOO CLOSE - NEED TO BACK AWAY]";
                } else if (inSpellRange) {
                    oss << " [GOOD RANGED POSITION]";
                } else {
                    oss << " [TOO FAR FOR SPELLS]";
                }
            }
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";

        if (attacker)
        {
            float dist = bot && attacker ? bot->GetDistance(attacker) : -1.0f;

            Creature* c = dynamic_cast<Creature*>(attacker);
            Player* p = dynamic_cast<Player*>(attacker);

            oss << "DEFEND YOURSELF, YOU ARE UNDER ATTACK BY: ";
            if (c)
            {
                // Creature-specific info
                oss << "Creature '" << safe_name(c)
                    << "' (guid: " << safe_guid(c) << ")"
                    << ", Level: " << safe_level(c)
                    << ", HP: " << safe_hp(c) << "/" << safe_maxhp(c)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Elite: " << (c->isElite() ? "Yes" : "No");

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : c->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else if (p)
            {
                // Player-specific info
                std::string pFaction = (p->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
                oss << "Player '" << safe_name(p)
                    << "' (guid: " << safe_guid(p) << ")"
                    << ", Level: " << safe_level(p)
                    << ", HP: " << safe_hp(p) << "/" << safe_maxhp(p)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Faction: " << pFaction
                    << ", Class: " << std::to_string(p->getClass())
                    << ", Race: " << std::to_string(p->getRace());

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : p->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else
            {
                // Unknown Unit type
                oss << safe_name(attacker)
                    << " (guid: " << safe_guid(attacker) << ")"
                    << ", Level: " << safe_level(attacker)
                    << ", HP: " << safe_hp(attacker) << "/" << safe_maxhp(attacker)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?");
            }

            oss << ". ";
        }

        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    else
    {
        oss << "NOT IN COMBAT (" << combatType << " FIGHTER). ";
        
        // Check for health issues that might indicate environmental damage
        if (bot) {
            float healthPercent = (float)bot->GetHealth() / (float)bot->GetMaxHealth() * 100.0f;
            if (healthPercent < 90.0f) {
                oss << "WARNING: Your health is at " << (int)healthPercent << "% - you may be taking environmental damage! ";
            }
        }
        
        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    return oss.str();
}


std::string GetDetailedQuestInfo(Player* bot)
{
    std::ostringstream oss;
    
    bool hasActiveQuests = false;
    
    for (auto const& qs : bot->getQuestStatusMap())
    {
        uint32 questId = qs.first;
        QuestStatus status = qs.second.Status;
        
        // Skip abandoned, failed, or already rewarded quests
        if (status == QUEST_STATUS_NONE || status == QUEST_STATUS_FAILED || status == QUEST_STATUS_REWARDED)
            continue;
            
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) continue;
        
        if (!hasActiveQuests) {
            oss << "Active quests:\n";
            hasActiveQuests = true;
        }
        
        std::string statusText;
        switch (status) {
            case QUEST_STATUS_INCOMPLETE: statusText = "IN PROGRESS"; break;
            case QUEST_STATUS_COMPLETE: statusText = "READY TO TURN IN"; break;
            default: statusText = "UNKNOWN"; break;
        }
        
        oss << "\n**QUEST: " << quest->GetTitle() << "** (ID: " << questId << ") - " << statusText << "\n";
        oss << "Level: " << quest->GetQuestLevel() << " | XP Reward: " << quest->XPValue(bot->GetLevel()) << "\n";
        
        if (status == QUEST_STATUS_COMPLETE) {
            oss << "*** PRIORITY: FIND QUEST GIVER TO TURN IN THIS QUEST ***\n";
            
            // Find who can accept this quest turn-in
            std::vector<std::string> turnInNPCs;
            
            // Check creatures that can accept this quest
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr) {
                CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(itr->first);
                if (cTemplate) {
                    turnInNPCs.push_back(std::string("NPC: ") + cTemplate->Name);
                }
            }
            
            // Check game objects that can accept this quest
            QuestRelationBounds goQir = sObjectMgr->GetGOQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = goQir.first; itr != goQir.second; ++itr) {
                GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(itr->first);
                if (goTemplate) {
                    turnInNPCs.push_back(std::string("Object: ") + goTemplate->name);
                }
            }
            
            if (!turnInNPCs.empty()) {
                oss << "Turn in to: ";
                for (size_t i = 0; i < turnInNPCs.size(); ++i) {
                    oss << turnInNPCs[i];
                    if (i < turnInNPCs.size() - 1) oss << " OR ";
                }
                oss << "\n";
            }
        } else {
            // Quest is incomplete - show objectives
            oss << "Objectives to complete:\n";
            
            // Check kill objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] != 0) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        std::string targetName = "Unknown Target";
                        
                        if (quest->RequiredNpcOrGo[i] > 0) {
                            // It's a creature
                            CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(quest->RequiredNpcOrGo[i]);
                            if (cTemplate) {
                                targetName = std::string("Kill ") + cTemplate->Name;
                            }
                        } else {
                            // It's a game object (negative value)
                            GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(-quest->RequiredNpcOrGo[i]);
                            if (goTemplate) {
                                targetName = std::string("Use/Click ") + goTemplate->name;
                            }
                        }
                        
                        oss << " - " << targetName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check item objectives
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredItemId[i] != 0) {
                    uint32 currentCount = bot->GetItemCount(quest->RequiredItemId[i], true);
                    uint32 requiredCount = quest->RequiredItemCount[i];
                    
                    if (requiredCount > 0) {
                        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]);
                        std::string itemName = itemTemplate ? itemTemplate->Name1 : "Unknown Item";
                        
                        oss << " - Collect " << itemName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check exploration objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] == 0 && quest->RequiredNpcOrGoCount[i] > 0) {
                    // This might be an exploration or spell cast objective
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        oss << " - Exploration/Event objective: " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " INCOMPLETE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Show quest description for context
            if (!quest->GetObjectives().empty()) {
                oss << "Description: " << quest->GetObjectives() << "\n";
            }
        }
    }
    
    if (!hasActiveQuests) {
        oss << "No active quests. Look for quest givers with available quests or turn-ins ready!\n";
    }
    
    return oss.str();
}

std::vector<std::string> GetNearbyWaypoints(Player* bot, float radius = 200.0f)
{
    std::vector<std::string> wps;
    if (!bot) return wps;
    uint32 bot_map = bot->GetMapId();
    float bot_x = bot->GetPositionX();
    float bot_y = bot->GetPositionY();
    float bot_z = bot->GetPositionZ();

    auto nodes = sTravelNodeMap->getNodes();
    int idx = 0;
    for (TravelNode* node : nodes)
    {
        if (!node) continue;
        WorldPosition* pos = node->getPosition();
        if (!pos) continue;
        if (pos->getMapId() != bot_map) continue;
        float dx = pos->getX() - bot_x;
        float dy = pos->getY() - bot_y;
        float dz = pos->getZ() - bot_z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > radius) continue;
        wps.push_back(fmt::format("Node #{} '{}' ({:.1f}, {:.1f}, {:.1f}), distance: {:.1f}", idx, node->getName(), pos->getX(), pos->getY(), pos->getZ(), dist));        
        ++idx;
    }
    return wps;
}

OllamaBotControlLoop::OllamaBotControlLoop() : WorldScript("OllamaBotControlLoop") {}

static std::unordered_map<uint64_t, time_t> nextTick;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    std::string* responseBuffer = static_cast<std::string*>(userp);
    size_t totalSize = size * nmemb;
    responseBuffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

static std::string QueryOllamaLLM(const std::string& prompt)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Failed to initialize cURL.");
        return "";
    }

    nlohmann::json requestData = {
        {"model",  g_OllamaBotControlModel},
        {"prompt", prompt}
    };
    std::string requestDataStr = requestData.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string responseBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, g_OllamaBotControlUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Failed to reach Ollama AI. cURL error: {}", curl_easy_strerror(res));
        return "";
    }

    std::stringstream ss(responseBuffer);
    std::string line, extracted;
    while (std::getline(ss, line))
    {
        try
        {
            nlohmann::json jsonResponse = nlohmann::json::parse(line);
            if (jsonResponse.contains("response"))
                extracted += jsonResponse["response"].get<std::string>();
        }
        catch (...) {}
    }
    return extracted;
}

static std::string BuildBotPrompt(Player* bot)
{
    PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(bot);
    if (!botAI) return "";

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    std::vector<std::string> groupInfo = GetGroupStatus(bot);

    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = bot->GetMap() ? bot->GetMap()->GetMapName() : "UnknownMap";
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;
    

    std::ostringstream oss;
    oss << "Bot state summary:\n";
    oss << "Name: " << botName << "\n";
    oss << "Level: " << botLevel << "\n";
    oss << "Class: " << botClass << "\n";
    oss << "Race: " << botRace << "\n";
    oss << "Gender: " << botGender << "\n";
    oss << "Faction: " << botFaction << "\n";
    oss << "Gold: " << botGold << "\n";
    oss << "Area: " << botAreaName << "\n";
    oss << "Zone: " << botZoneName << "\n";
    oss << "Map: " << botMapName << "\n";
    oss << "Position: " << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << "\n";

    oss << GetCombatSummary(bot) << "\n\n";

    oss << "Your known spells:\n" << GetBotSpellInfo(bot) << "\n\n";

    oss << "Group status: " << botGroupStatus << "\n";
    if (!groupInfo.empty()) {
        oss << "Group members:\n";
        for (const auto& entry : groupInfo) oss << " - " << entry << "\n";
    }

    oss << GetDetailedQuestInfo(bot) << "\n";

    std::vector<std::string> losLocs = GetVisibleLocations(bot);
    std::vector<std::string> wps = GetNearbyWaypoints(bot);

    if (!losLocs.empty()) {
        oss << "Visible locations/objects in line of sight:\n";
        for (const auto& entry : losLocs) oss << " - " << entry << "\n";
        
        // Check for critical priorities and add warnings
        bool hasEnemies = false;
        bool hasNeutrals = false;
        bool hasQuestTargets = false;
        bool hasQuestTurnIns = false;
        bool hasLootableCorpses = false;
        
        for (const auto& entry : losLocs) {
            if (entry.find("ENEMY:") != std::string::npos) {
                hasEnemies = true;
            }
            if (entry.find("NEUTRAL:") != std::string::npos) {
                hasNeutrals = true;
            }
            if (entry.find("[QUEST TARGET") != std::string::npos) {
                hasQuestTargets = true;
            }
            if (entry.find("[QUEST GIVER - TURN IN READY]") != std::string::npos) {
                hasQuestTurnIns = true;
            }
            if (entry.find("DEAD (LOOTABLE)") != std::string::npos) {
                hasLootableCorpses = true;
            }
        }
        
        // Priority warnings in order of importance
        if (hasQuestTurnIns) {
            oss << "*** HIGHEST PRIORITY: QUEST TURN-INS AVAILABLE! Find NPCs marked with [QUEST GIVER - TURN IN READY] immediately! ***\n";
        }
        if (hasLootableCorpses) {
            oss << "*** LOOTABLE CORPSES AVAILABLE! Use 'loot' command on creatures marked 'DEAD (LOOTABLE)' for XP and items! ***\n";
        }
        if (hasQuestTargets) {
            oss << "*** QUEST TARGETS AVAILABLE! Attack the creatures marked with [QUEST TARGET] to complete your objectives! ***\n";
        }
        if (hasEnemies) {
            oss << "*** WARNING: ENEMIES ARE VISIBLE! You should attack them for XP and to defend yourself! ***\n";
        }
        if (hasNeutrals && !hasQuestTargets) {
            oss << "*** NEUTRAL CREATURES VISIBLE: These may be needed for quest objectives! Check your quest targets and attack if needed! ***\n";
        }
    }

    if (!wps.empty()) {
        oss << "Nearby navigation waypoints:\n";
        for (const auto& entry : wps) oss << " - " << entry << "\n";
    }

    std::vector<std::string> nearbyPlayers = GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        oss << "Visible players in area:\n";
        for (const auto& entry : nearbyPlayers) oss << " - " << entry << "\n";
    }

    if (!losLocs.empty() || !wps.empty()) {
        oss << "You must select one of these locations or waypoints to move to, interact with, accept or turn in quests, attack, loot, or any other action or choose a new unexplored spot.\n";
        oss << "IMPORTANT: You can ONLY attack creatures/NPCs that are listed above in the visible locations. If your quest requires creatures that are NOT visible, you must move to find them using waypoints or exploration.\n";
    }

    oss << FormatPlayerMessagesPromptSegment(bot);

    std::vector<std::string> cmdHist = GetBotCommandHistory(bot);

    std::vector<std::string> reasoningHist = GetBotReasoningHistory(bot);


    if (!cmdHist.empty() && !reasoningHist.empty())
    {
        oss << "Last 5 commands and their reasoning (most recent at the bottom):\n";
        for (size_t i = 0; i < cmdHist.size() && i < reasoningHist.size(); ++i)
        {
            oss << " - Command: " << cmdHist[i] << "\n";
            oss << "   Reasoning: " << reasoningHist[i] << "\n";
        }
        oss << "\nIMPORTANT: Look at your command history above! If you keep using move_to commands to the same location, switch to interact commands instead. If you keep trying to interact with the same NPC unsuccessfully, move away to find enemies or other NPCs.\n";
    }

    if (g_EnableOllamaBotBuddyDebug)
    {
        std::string safeSnapshot = EscapeBracesForFmt(oss.str());
        LOG_INFO("server.loading", "[OllamaBotBuddy] Bot Snapshot for '{}': {}", botName, safeSnapshot);
    }

    oss << R"(You are an AI-controlled bot in World of Warcraft. Your task is to follow these strict rules and reply only with the listed acceptable commands:

    Primary goal: Level to 80 and equip the best gear. Prioritize combat, questing and quest givers that have available quests, talking to other players and efficient progression. If no available quests or viable enemies are nearby, turn in quests, explore for new quests, dungeons, raids, professions, or gold opportunities.

    SURVIVAL AND IMMEDIATE THREATS (HIGHEST PRIORITY):
    - If you are taking damage and not in combat with a target, IMMEDIATELY move away from your current position
    - If you see "ENEMY" creatures in your visible list and you're not fighting anything, ATTACK the nearest enemy immediately
    - DO NOT STAND ON CAMP FIRES or other environmental hazards - they cause damage
    - If your HP is dropping and you're not in combat, move to a safe location immediately
    - If you're under attack by enemies, prioritize combat over everything else

    QUEST PRIORITIZATION (HIGH PRIORITY):
    - If you have any quests marked "READY TO TURN IN", that is your TOP PRIORITY - find the quest giver immediately
    - For incomplete quests, read the objectives carefully and focus on completing them:
      * If you need to kill creatures, prioritize those specific creatures over random enemies
      * If you need to collect items, look for the sources of those items
      * If you need to interact with objects, find and use those objects
      * If objectives show "COMPLETE", that part is done - focus on incomplete objectives
    - When you see quest objectives that need specific creatures or items, prioritize those targets over random combat
    - Quest completion gives significant XP - completing quests is more efficient than random grinding

    CRITICAL QUEST BEHAVIOR:
    - NEVER waste time sitting at NPCs that have no available quests for you
    - If an NPC doesn't have "[QUEST GIVER - TURN IN READY]" or "[QUEST GIVER - QUESTS AVAILABLE]" tags, DO NOT prioritize them unless you have no other options
    - If you tried to interact with an NPC and nothing happened, that means they have no quests - MOVE AWAY IMMEDIATELY and find something else to do
    - Look at your command history - if you keep trying the same quest giver repeatedly, STOP and go elsewhere

    NPC INTERACTION DECISION LOGIC:
    - If you see an NPC within 15 yards with "[QUEST GIVER - TURN IN READY]" or "[QUEST GIVER - QUESTS AVAILABLE]" tags: USE INTERACT COMMAND
    - If you see such an NPC beyond 15 yards: USE MOVE_TO COMMAND to get closer first
    - ONLY interact with NPCs that have useful tags: [QUEST GIVER - TURN IN READY], [QUEST GIVER - QUESTS AVAILABLE], [VENDOR], [TRAINER], [FLIGHT MASTER], [INNKEEPER], [BANKER], [AUCTIONEER]
    - NEVER interact with generic friendly NPCs that have no useful tags - they are a waste of time
    - If you see a friendly NPC with no useful tags, IGNORE IT completely and focus on combat or exploration
    - If your last action was to interact with an NPC but you're still in the same position, that NPC was useless - find enemies to fight or new areas to explore

    COMBAT TARGETING AND POSITIONING:
    - ALWAYS select your target properly before attacking using the attack command
    - If you're too far from your target, MOVE CLOSER first before trying to attack
    - MELEE fighters must get within 5 yards of the target before attacking
    - RANGED fighters should maintain 6-25 yard distance from targets
    - If you're a MELEE fighter and the target is far away, use move_to command to get closer first
    - If you're a RANGED fighter and too close (distance < 6), move away before attacking

    QUEST TARGET HUNTING:
    - Look at your quest objectives and identify what creatures/items you need
    - Check your "Visible locations/objects" list to see if those creatures are currently visible
    - If quest target creatures ARE visible: attack them immediately (use their GUID from the visible list)
    - If quest target creatures are NOT visible: move to a waypoint or new area to search for them
    - NEVER try to attack creatures that aren't in your current visible list - move to find them first
    - If no quest targets are available, attack any hostile creatures visible for XP while searching

    COMBAT RULES:
    - NEVER ATTACK DEAD CREATURES: If a creature is marked as DEAD or DEAD (LOOTABLE), use the loot command instead of attack
    - If you or a player in your group are under attack, IMMEDIATELY prioritize defense. Attack the enemy targeting you or your group, or escape if the enemy is much higher level.
    - During combat, do NOT disengage or move away unless your HP is low or the enemy is significantly stronger.
    - POSITIONING IS CRITICAL: Read your combat summary carefully to understand your role:
      * MELEE FIGHTERS: Must be within melee range (distance < 5). If you see TOO FAR FOR MELEE, move closer before attacking.
      * RANGED FIGHTERS: Maintain optimal distance (5-25 yards). If you see TOO CLOSE - NEED TO BACK AWAY, move away first. If you see TOO FAR FOR SPELLS, move closer.
      * Pay attention to range indicators: IN MELEE RANGE, GOOD RANGED POSITION, etc.
    - When choosing a target, move toward them if not in range. Use 'attack' only once you're within proper combat distance.
    - If you're too close to your target (distance <= 0.15) then move away before attacking again.
    - DO NOT TRY TO ATTACK OR DEFEND FROM CREATURES TAGGED AS DEAD - USE LOOT COMMAND INSTEAD.
    - BE AGGRESSIVE, killing things around your level grants you XP to level up. Attack monsters nearby to help level up.
    - QUEST CREATURES PRIORITY: Always attack creatures needed for your quest objectives, regardless of their faction (hostile, neutral, or friendly)
    - If no quest target creatures are visible, prioritize attacking hostile creatures for XP and safety
    - NEUTRAL CREATURES: Attack neutral creatures if they are needed for quest objectives or if they're aggressive toward you
    - Make sure you're using your spells, if you have the resource cost and the spell sounds like it would help in combat, use a spell command picking a logical target guid!
    - COMBAT TYPE AWARENESS: Your combat summary shows if you're a MELEE, RANGED, or HYBRID fighter. Use this to determine proper positioning and tactics.

    DECISION RULE (ABSOLUTE PRIORITY ORDER):
    1. SURVIVAL FIRST: If you're taking damage and not in combat, move away from environmental hazards immediately
    2. QUEST TURN-INS (HIGHEST PRIORITY): If you have quests READY TO TURN IN, find those quest givers immediately - this takes priority over EVERYTHING else
    3. LOOTING DEAD CREATURES: If you see DEAD (LOOTABLE) creatures in your visible list, use the loot command immediately - this gives XP and items
    4. QUEST OBJECTIVES: Always check your active quest details and prioritize completing quest objectives over random combat
    5. VISIBLE ENEMIES: If you see any ENEMY creatures in your visible list, attack them for XP - but only after checking for quest turn-ins and loot
    - For incomplete quests, target the specific creatures or objects needed for quest objectives rather than random enemies
    - CRITICAL: You can ONLY interact with, attack, or move to objects/creatures that are listed in your Visible locations/objects section - NEVER try to attack or interact with creatures/NPCs that aren't currently visible
    - **GUID USAGE CRITICAL**: When using attack, interact, or spell commands, you MUST copy the exact GUID number from the visible locations list. DO NOT make up or guess GUID numbers!
    - EXAMPLE: If you see ENEMY: Kobold Vermin (guid: 604, Level: 1...), use exactly 604 as the GUID in your attack command
    - INVALID: Using made-up GUIDs like 1234, 5678, or any number not explicitly shown in your visible locations
    - VALID: Only use GUIDs that appear in parentheses after guid: in your visible locations list
    - If quest objectives require specific creatures that are NOT in your visible list, you must move to find them - use waypoints or explore new areas
    - Always choose the most effective single action to level up, complete quests, gain gear, or respond to threats.
    - ANY other format or additional text reply is INVALID.
    - Base your decisions on the current game state, visible objects, group status, and your last 5 commands along with their reasoning. For example, if your previous command was to move and attack a target, and that target is still present and within range, your next action should likely be to execute an attack command.
    - DEAD CREATURE LOOTING: If you see a creature marked as DEAD (LOOTABLE) in your visible list, ALWAYS use the loot command to loot its body for XP and items - NEVER try to attack dead creatures
    - QUEST TARGET LOGIC:
      * First, check if the creatures you need for quest objectives are in your visible list - if yes, attack them (but only if they are ALIVE, not dead)
      * If quest target creatures are NOT visible, move to a waypoint or new area to search for them
      * If no quest targets are visible and no useful NPCs are available, attack any hostile creature in your visible list for XP
      * NEVER try to attack creatures that aren't in your current visible list - they don't exist in your current area
    - QUEST GIVER INTERACTION LOGIC: 
      * If you see an NPC within 15 yards with [QUEST GIVER - TURN IN READY] or [QUEST GIVER - QUESTS AVAILABLE] tags: USE INTERACT COMMAND immediately
      * If you see such an NPC beyond 15 yards: USE MOVE_TO COMMAND to get closer first
      * NEVER keep moving to the same quest giver if you're already close - switch to interact command
      * COMPLETELY IGNORE all other NPCs unless they have useful tags like [VENDOR], [TRAINER], [FLIGHT MASTER], [INNKEEPER], [BANKER], [AUCTIONEER]
      * NEVER interact with NPCs that have no quest tags, no useful service tags, or are just generic friendly NPCs
      * If you see an NPC with no available quests, IMMEDIATELY move away and find a different target
      * If your last command was to interact with a quest giver but you're still at the same location, that means the NPC had no quests - MOVE ELSEWHERE IMMEDIATELY
      * Do NOT repeatedly try to interact with the same quest giver - if it didn't work the first time, that NPC has no available quests for you
      * PRIORITIZE ENEMIES TO KILL over useless friendly NPCs - combat gives XP, talking to random NPCs does not
      * IF YOUR LAST COMMAND WAS move_to TO A QUEST GIVER AND YOU'RE NOW CLOSE TO THEM, YOUR NEXT COMMAND SHOULD BE interact
    - CRITICAL ENVIRONMENTAL SAFETY: If you are taking damage from environmental sources (like standing on campfires), IMMEDIATELY move to safety before doing anything else
    
    NAVIGATION:
    - Use ONLY GUIDs or coordinates listed in visible objects or navigation options.
    - NEVER make up IDs, GUIDs, or coordinates.
    - If nothing useful is visible, choose a waypoint or unexplored coordinate and move there.
    - If you're in a group, try to stay within 5-10 distance of another group member if you're not engaged in combat.
    - Do not move DIRECTLY on top of other players, creatures or objects, always maintain a distance to avoid collision issues.

    COMMUNICATION:
    - Be chatty only in the say field! Talk to other players, comment on things or people around you or your intentions and goals.
    - To make your character say something to players, put the message as a string in the top-level say field.
    - Make yourself seem as human as possible, ask players for help if you don't understand something or need help finding something or killing something or completing a quest. Ask a nearby real player and use their response in your reasoning.

    CRITICALLY IMPORTANT: Reply with EXACTLY and ONLY a single valid JSON object, no extra text, no comments, no code block formatting. Your JSON must be:
    {
    "command": { "type": <string>, "params": { ... } },
    "reasoning": <string>,
    "say": <string>
    }

    Allowed type values and required params:

    - move_to: params = { x: float, y: float, z: float }
    - attack: params = { guid: int }
    - interact: params = { guid: int }
    - spell: params = { spellid: int, guid: int (omit if self-cast) }
    - loot: params = { }
    - accept_quest: params = { id: int }
    - turn_in_quest: params = { id: int }
    - follow: params = { }
    - stop: params = { }

    reasoning must be a short natural-language explanation for why you chose this command.
    say must be what your character would say in-game to players, or empty string if nothing is to be said. You can use this to communicate with players, but do not use it for commands.

    **CRITICAL GUID REQUIREMENT**: For attack, interact, and spell commands, you MUST use the exact GUID numbers from your visible locations list. DO NOT make up numbers!

    EXAMPLES:
    {
    command: { type: move_to, params: { x: -9347.02, y: 256.48, z: 65.10 } },
    reasoning: Moving to the quest NPC as ordered by the player.,
    say: On my way.
    }
    {
    command: { type: attack, params: { guid: 604 } },
    reasoning: Attacking Kobold Vermin with GUID 604 from my visible list for quest objective.,
    say: Attacking the Kobold!
    }
    {
    command: { type: interact, params: { guid: 617 } },
    reasoning: Interacting with Kobold Worker GUID 617 from my visible list.,
    say: Let me talk to this worker.
    }
    {
    command: { type: loot, params: { } },
    reasoning: Looting the corpse.,
    say: Looting now.
    }

    REMEMBER: NEVER REPLY WITH ANYTHING OTHER THAN A VALID JSON OBJECT!!!
    )";


    return oss.str();
}

namespace
{
    struct OllamaBotState
    {
        std::atomic<bool> busy { false };
        time_t lastRequest { 0 };
    };
    std::unordered_map<uint64_t, OllamaBotState> ollamaBotStates;
}

std::string EscapeBracesForFmt(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2); // Avoid lots of reallocs

    for (char c : input) {
        if (c == '{' || c == '}') {
            output.push_back(c); // first brace
            output.push_back(c); // second brace
        } else {
            output.push_back(c);
        }
    }
    return output;
}

void OllamaBotControlLoop::OnUpdate(uint32 /*diff*/)
{
    if (!g_EnableOllamaBotControl) return;

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot->IsInWorld()) continue;
        std::string botName = bot->GetName();

        // Temporary marker for testing
        if (botName != "Ollamatest") continue;

        // Clear the normal Playerbot AI
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (ai)
        {
            ai->ClearStrategies(BOT_STATE_COMBAT);
            ai->ClearStrategies(BOT_STATE_NON_COMBAT);
            ai->ClearStrategies(BOT_STATE_DEAD);
        } else {
            continue;
        }

        uint64_t guid = bot->GetGUID().GetRawValue();
        OllamaBotState& state = ollamaBotStates[guid];

        // Only process if not already waiting for LLM
        if (!state.busy)
        {
            state.busy = true;
            state.lastRequest = time(nullptr);

            std::string prompt = BuildBotPrompt(bot);

            if (g_EnableOllamaBotBuddyDebug)
            {
                //LOG_INFO("server.loading", "[OllamaBotBuddy] Sending prompt for bot '{}': {}", botName, prompt);
            }

            std::thread([bot, guid, prompt]() {
                std::string llmReply = QueryOllamaLLM(prompt);

                if (g_EnableOllamaBotBuddyDebug)
                {
                    std::string safeJson = EscapeBracesForFmt(llmReply);
                    LOG_INFO("server.loading", "[OllamaBotBuddy] LLM reply for '{}':\n{}", bot->GetName(), safeJson);

                }

                if (!llmReply.empty())
                {
                    std::string jsonOnly = ExtractFirstJsonObject(llmReply);
                    if (!jsonOnly.empty()) {
                        ParseAndExecuteBotJson(bot, jsonOnly);
                        
                        // Rebuild the prompt to include the latest command in history
                        std::string updatedPrompt = BuildBotPrompt(bot);
                        SendBuddyBotStateToPlayer(bot, bot, updatedPrompt);

                    } else {
                        LOG_ERROR("server.loading", "[OllamaBotBuddy] No valid JSON object found in LLM reply: {}", llmReply);
                    }
                }

                // Mark ready for the next request
                ollamaBotStates[guid].busy = false;
            }).detach();
        }
    }
}
