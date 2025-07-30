#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_loop.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Log.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "Map.h"
#include "Event.h"
#include "QuestDef.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GossipDef.h"
#include <sstream>

namespace BotBuddyAI
{
    bool MoveTo(Player* bot, float x, float y, float z)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle movement using coordinate format
        std::ostringstream coords;
        coords << x << ";" << y << ";" << z;
        
        Event event = Event("", coords.str());
        return ai->DoSpecificAction("go", event);
    }

    bool Attack(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;

        Unit* target = ObjectAccessor::GetUnit(*bot, guid);
        if (!target || !bot->IsWithinLOSInMap(target)) return false;

        // Set the target and use the AI system to handle attacking
        bot->SetTarget(guid);
        Event event = Event("", target->GetName());
        return ai->DoSpecificAction("attack my target", event);
    }

    bool Interact(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;

        if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
        {
            // Check if this is a quest giver and handle quest interaction properly
            if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            {
                return InteractWithQuestGiver(bot, creature);
            }
            else
            {
                // For non-quest NPCs, use gossip hello action
                Event event = Event("", std::to_string(guid.GetCounter()));
                return ai->DoSpecificAction("gossip hello", event);
            }
        }
        else if (GameObject* go = ObjectAccessor::GetGameObject(*bot, guid))
        {
            // Check if this is a quest giver game object
            if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
            {
                return InteractWithQuestGiver(bot, go);
            }
            else
            {
                // Use the bot's AI system to handle interaction with game objects
                Event event = Event("", go->GetGOInfo()->name);
                return ai->DoSpecificAction("use", event);
            }
        }
        return false;
    }

    bool InteractWithQuestGiver(Player* bot, WorldObject* questGiver)
    {
        if (!bot || !questGiver) return false;

        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;

        // Check interaction distance
        if (bot->GetDistance(questGiver) > INTERACTION_DISTANCE)
        {
            return false;
        }

        // Face the quest giver
        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, questGiver, sPlayerbotAIConfig->sightDistance))
            bot->SetFacingToObject(questGiver);

        ObjectGuid guid = questGiver->GetGUID();
        
        // Prepare the quest menu for this quest giver
        bot->PrepareQuestMenu(guid);
        QuestMenu& questMenu = bot->PlayerTalkClass->GetQuestMenu();

        bool foundQuestAction = false;

        // Process all available quest menu items
        for (uint32 i = 0; i < questMenu.GetMenuItemCount(); ++i)
        {
            QuestMenuItem const& menuItem = questMenu.GetItem(i);
            Quest const* quest = sObjectMgr->GetQuestTemplate(menuItem.QuestId);
            if (!quest) continue;

            QuestStatus status = bot->GetQuestStatus(menuItem.QuestId);
            
            // Handle completed quests first (highest priority)
            if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, false))
            {
                // Turn in the quest using the playerbot action system
                TurnInQuest(bot, menuItem.QuestId);
                foundQuestAction = true;
                
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} turning in quest {}: {}", 
                        bot->GetName(), menuItem.QuestId, quest->GetTitle());
                }
            }
            // Handle new quests that can be accepted
            else if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                // Accept the quest using the playerbot action system
                AcceptQuest(bot, menuItem.QuestId);
                foundQuestAction = true;
                
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} accepting quest {}: {}", 
                        bot->GetName(), menuItem.QuestId, quest->GetTitle());
                }
            }
        }

        // If we found quest actions, return success
        if (foundQuestAction)
        {
            return true;
        }

        // If no direct quest actions were available, try automatic gossip navigation
        if (Creature* creature = questGiver->ToCreature())
        {
            // Try to automatically navigate gossip menus for quest options
            if (AutoNavigateGossipForQuests(bot, creature))
            {
                return true;
            }
            
            // Fallback to basic gossip hello action
            Event event = Event("", std::to_string(guid.GetCounter()));
            return ai->DoSpecificAction("gossip hello", event);
        }
        else if (GameObject* go = questGiver->ToGameObject())
        {
            // Use game object interaction
            Event event = Event("", go->GetGOInfo()->name);
            return ai->DoSpecificAction("use", event);
        }

        return false;
    }

    bool AutoNavigateGossipForQuests(Player* bot, Creature* creature)
    {
        if (!bot || !creature) return false;

        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;

        // Start gossip interaction
        WorldPacket packet(CMSG_GOSSIP_HELLO);
        packet << creature->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(packet);

        // Wait a moment for the server to process
        if (!bot->PlayerTalkClass) return false;

        GossipMenu& gossipMenu = bot->PlayerTalkClass->GetGossipMenu();
        QuestMenu& questMenu = bot->PlayerTalkClass->GetQuestMenu();

        // First priority: Handle direct quest menus
        for (uint32 i = 0; i < questMenu.GetMenuItemCount(); ++i)
        {
            QuestMenuItem const& menuItem = questMenu.GetItem(i);
            Quest const* quest = sObjectMgr->GetQuestTemplate(menuItem.QuestId);
            if (!quest) continue;

            QuestStatus status = bot->GetQuestStatus(menuItem.QuestId);
            
            if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, false))
            {
                TurnInQuest(bot, menuItem.QuestId);
                return true;
            }
            else if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                AcceptQuest(bot, menuItem.QuestId);
                return true;
            }
        }

        // Second priority: Navigate gossip menu for quest-related options
        GossipMenuItemContainer const& gossipItems = gossipMenu.GetMenuItems();
        for (auto const& item : gossipItems)
        {
            GossipMenuItem const* gossipItem = &item.second;
            std::string message = gossipItem->Message;
            
            // Look for quest-related gossip options
            if (message.find("quest") != std::string::npos || 
                message.find("Quest") != std::string::npos ||
                message.find("mission") != std::string::npos ||
                message.find("task") != std::string::npos)
            {
                // Select this gossip option
                WorldPacket selectPacket(CMSG_GOSSIP_SELECT_OPTION);
                selectPacket << creature->GetGUID();
                selectPacket << gossipMenu.GetMenuId();
                selectPacket << item.first;
                selectPacket << std::string("");
                bot->GetSession()->HandleGossipSelectOptionOpcode(selectPacket);
                
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} selected gossip option: {}", 
                        bot->GetName(), message);
                }
                return true;
            }
        }

        return false;
    }

    bool HasQuestsAvailable(Player* bot, WorldObject* questGiver)
    {
        if (!bot || !questGiver) return false;

        // For creatures, check their quest relations directly (more efficient)
        if (Creature* creature = questGiver->ToCreature())
        {
            // Check for completable quests first (highest priority)
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(creature->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    return true; // Has quest ready to turn in
                }
            }
            
            // Check for available quests (secondary priority)
            QuestRelationBounds qr = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
            for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
            {
                uint32 questId = itr->second;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                    bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                {
                    return true; // Has quest available to accept
                }
            }
        }
        // For game objects, check quest relations
        else if (GameObject* go = questGiver->ToGameObject())
        {
            // Check for completable quests
            QuestRelationBounds qir = sObjectMgr->GetGOQuestInvolvedRelationBounds(go->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    return true;
                }
            }
            
            // Check for available quests
            QuestRelationBounds qr = sObjectMgr->GetGOQuestRelationBounds(go->GetEntry());
            for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
            {
                uint32 questId = itr->second;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                    bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                {
                    return true;
                }
            }
        }
        
        return false;
    }

    bool CastSpell(Player* bot, uint32 spellId, Unit* target)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo) return false;
        
        // Use the bot's AI system to handle spell casting
        std::ostringstream cmd;
        cmd << spellInfo->SpellName[0];
        if (target && target != bot)
        {
            cmd << " on " << target->GetName();
        }
        
        Event event = Event("", cmd.str());
        return ai->DoSpecificAction("cast custom spell", event);
    }

    bool Say(Player* bot, const std::string& msg)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle saying
        Event event = Event("", msg);
        return ai->DoSpecificAction("say", event);
    }

    bool FollowMaster(Player* bot)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle following
        Event event = Event("", "");
        return ai->DoSpecificAction("follow", event);
    }

    bool StopMoving(Player* bot)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle stopping
        Event event = Event("", "");
        return ai->DoSpecificAction("stay", event);
    }

    bool AcceptQuest(Player* bot, uint32 questId)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) return false;

        // Use the playerbot AI system to handle quest acceptance
        Event event = Event("", std::to_string(questId));
        return ai->DoSpecificAction("accept quest", event);
    }

    bool TurnInQuest(Player* bot, uint32 questId)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) return false;

        // Check if quest is complete
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            return false;

        // Use the playerbot AI system to handle quest turn-in
        Event event = Event("", std::to_string(questId));
        return ai->DoSpecificAction("turn in query quest", event);
    }

    bool LootNearby(Player* bot)
    {
        if (!bot) return false;

        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;

        // Use the bot's AI system to handle looting
        Event event = Event("", "");
        return ai->DoSpecificAction("loot", event);
    }

} // namespace BotBuddyAI

bool HandleBotControlCommand(Player* bot, const BotControlCommand& command)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] HandleBotControlCommand for '{}', type {}", bot->GetName(), int(command.type));
        LOG_INFO("server.loading", "[OllamaBotBuddy] ================================================================================================");
    }
    if (!bot) return false;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            if (command.args.size() >= 3)
            {
                float x = std::stof(command.args[0]);
                float y = std::stof(command.args[1]);
                float z = std::stof(command.args[2]);
                return BotBuddyAI::MoveTo(bot, x, y, z);
            }
            break;
        case BotControlCommandType::Attack:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
                try {
                    lowGuid = std::stoul(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[0]);
                    return false;
                }

                // Try to find the Creature by LowGuid first
                Creature* creatureTarget = nullptr;
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    // Use the actual GUID from the creature, never reconstruct!
                    return BotBuddyAI::Attack(bot, creatureTarget->GetGUID());
                }

                // If not found, try Player
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                if (playerTarget)
                {
                    return BotBuddyAI::Attack(bot, playerTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::Interact:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
                try {
                    lowGuid = std::stoul(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[0]);
                    return false;
                }
                Creature* creatureTarget = nullptr;
                GameObject* goTarget = nullptr;

                // Find creature by LowGuid
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    return BotBuddyAI::Interact(bot, creatureTarget->GetGUID());
                }

                // Find gameobject by LowGuid
                for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
                {
                    GameObject* go = pair.second;
                    if (!go) continue;
                    if (go->GetGUID().GetCounter() == lowGuid)
                    {
                        goTarget = go;
                        break;
                    }
                }

                if (goTarget)
                {
                    return BotBuddyAI::Interact(bot, goTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find interact target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::CastSpell:
            if (!command.args.empty())
            {
                uint32 spellId = 0;
                try {
                    spellId = std::stoi(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for spellId '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for spellId '{}'", command.args[0]);
                    return false;
                }
                Unit* target = nullptr;
                if (command.args.size() > 1)
                {
                    uint32 lowGuid = 0;
        try {
            lowGuid = std::stoul(command.args[1]);
        } catch (const std::invalid_argument& e) {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[1]);
            return false;
        } catch (const std::out_of_range& e) {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[1]);
            return false;
        }
                    // Try to find creature by lowGuid
                    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                    {
                        Creature* c = pair.second;
                        if (c && c->GetGUID().GetCounter() == lowGuid)
                        {
                            target = c;
                            break;
                        }
                    }
                    // Try to find player by lowGuid if not found
                    if (!target)
                    {
                        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                        Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                        if (playerTarget) target = playerTarget;
                    }
                }
                else
                {
                    target = bot; // Use bot itself as the target if no guid provided
                }
                return BotBuddyAI::CastSpell(bot, spellId, target);
            }
            break;
        case BotControlCommandType::Say:
            if (!command.args.empty())
            {
                return BotBuddyAI::Say(bot, command.args[0]);
            }
            break;
        case BotControlCommandType::Follow:
            return BotBuddyAI::FollowMaster(bot);
        case BotControlCommandType::Stop:
            return BotBuddyAI::StopMoving(bot);
        case BotControlCommandType::AcceptQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::AcceptQuest(bot, questId);
            }
            break;
        case BotControlCommandType::TurnInQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::TurnInQuest(bot, questId);
            }
            break;
        case BotControlCommandType::Loot:
            return BotBuddyAI::LootNearby(bot);
        default:
            break;
    }
    return false;
}


bool ParseBotControlCommand(Player* bot, const std::string& commandStr)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] ParseBotControlCommand for '{}': {}", bot->GetName(), commandStr);
    }
    std::istringstream iss(commandStr);
    std::string cmd;
    iss >> cmd;
    if (cmd == "move")
    {
        std::string to;
        iss >> to;
        if (to != "to") return false;
        float x, y, z;
        iss >> x >> y >> z;
        BotControlCommand command = {BotControlCommandType::MoveTo, {std::to_string(x), std::to_string(y), std::to_string(z)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "attack")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Attack, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "interact")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Interact, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "say")
    {
        std::string msg;
        std::getline(iss, msg);
        BotControlCommand command = {BotControlCommandType::Say, {msg}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "loot")
    {
        BotControlCommand command = {BotControlCommandType::Loot, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "follow")
    {
        BotControlCommand command = {BotControlCommandType::Follow, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "stop")
    {
        BotControlCommand command = {BotControlCommandType::Stop, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "acceptquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::AcceptQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "turninquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::TurnInQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "spell")
    {
        uint32 spellId;
        iss >> spellId;
        std::string targetGuid;
        iss >> targetGuid;
        BotControlCommand command;
        if (!targetGuid.empty())
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId), targetGuid}};
        }
        else
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId)}};
        }
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    return false;
}

std::string FormatCommandString(const BotControlCommand& command)
{
    std::ostringstream ss;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            ss << "move to";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Attack:
            ss << "attack";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Interact:
            ss << "interact";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::CastSpell:
            ss << "cast";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Loot:
            ss << "loot";
            break;
        case BotControlCommandType::Follow:
            ss << "follow";
            break;
        case BotControlCommandType::Say:
            ss << "say";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::AcceptQuest:
            ss << "acceptquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::TurnInQuest:
            ss << "turninquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Stop:
            ss << "stop";
            break;
        default:
            ss << "unknown command";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
    }
    return ss.str();
}


