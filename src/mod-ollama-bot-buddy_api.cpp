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

// Constants for interaction and combat ranges
#define INTERACTION_DISTANCE 5.5f
#define ATTACK_DISTANCE 5.0f

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

        // CRITICAL: Validate target before attacking to prevent friendly fire
        if (!bot->IsValidAttackTarget(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Invalid attack target: {} - not attackable", target->GetName());
            }
            return false;
        }

        // Check if target is friendly - absolutely prevent attacking friendlies
        if (bot->IsFriendlyTo(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Refusing to attack friendly target: {}", target->GetName());
            }
            return false;
        }

        // Additional safety: check if target is in same group or guild
        if (Player* targetPlayer = target->ToPlayer()) {
            if (bot->IsInSameGroupWith(targetPlayer) || bot->IsInSameGuildWith(targetPlayer)) {
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Refusing to attack group/guild member: {}", target->GetName());
                }
                return false;
            }
        }

        // Check if target is dead or GM
        if (!target->IsAlive() || (target->ToPlayer() && target->ToPlayer()->IsGameMaster())) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Target is dead or GM: {}", target->GetName());
            }
            return false;
        }

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} attacking target {} (guid: {})", 
                bot->GetName(), target->GetName(), guid.GetCounter());
        }

        // CRITICAL: Set selection and target properly FIRST
        bot->SetSelection(guid);
        bot->SetTarget(guid);
        
        // Set the target in the AI context - this is critical for combat
        ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
        ai->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(guid);
        
        // Change to combat engine to enable combat actions
        ai->ChangeEngine(BOT_STATE_COMBAT);
        
        // Face the target properly
        bot->SetFacingToObject(target);
        
        // Determine if bot is melee or ranged for proper positioning
        bool isMelee = ai->IsMelee(bot);
        bool isRanged = ai->IsRanged(bot);
        float currentDistance = bot->GetDistance(target);
        float meleeRange = ai->GetRange("melee");
        float spellRange = ai->GetRange("spell");
        float shootRange = ai->GetRange("shoot");
        
        Event event = Event("", "");
        bool result = false;
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Combat analysis - Melee: {}, Ranged: {}, Distance: {:.1f}", 
                isMelee ? "YES" : "NO", isRanged ? "YES" : "NO", currentDistance);
            LOG_INFO("server.loading", "[OllamaBotBuddy] Ranges - Melee: {:.1f}, Spell: {:.1f}, Shoot: {:.1f}", 
                meleeRange, spellRange, shootRange);
        }
        
        // Handle positioning and attacking based on combat type
        if (isMelee) {
            // For melee bots, ensure they're in melee range before attacking
            if (currentDistance > meleeRange) {
                // Move to melee range first
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MoveChase(target);
                
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Melee bot moving to target, distance: {:.1f}", currentDistance);
                }
                
                // Try reach melee action as well
                result = ai->DoSpecificAction("reach melee", event);
                return true; // Movement initiated, attack will happen next cycle
            }
            
            // In melee range - attack now
            if (ai->IsTank(bot)) {
                result = ai->DoSpecificAction("tank assist", event);
            } else {
                result = ai->DoSpecificAction("dps assist", event);
            }
            
            // Fallback to melee action if assist actions fail
            if (!result) {
                result = ai->DoSpecificAction("melee", event);
            }
            
            // Manual attack initiation if playerbot actions fail
            if (!result) {
                bot->Attack(target, true); // true = melee attack
                result = true;
            }
            
        } else if (isRanged) {
            // For ranged bots, maintain appropriate distance
            float optimalRange = std::min(spellRange, shootRange);
            float minRange = 6.0f; // Minimum range for ranged combat
            
            if (currentDistance < minRange) {
                // Too close for ranged - back away
                bot->GetMotionMaster()->Clear();
                float angle = target->GetAngle(bot);
                float destX = bot->GetPositionX() + cos(angle) * 12.0f;
                float destY = bot->GetPositionY() + sin(angle) * 12.0f;
                float destZ = bot->GetPositionZ();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);
                
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Ranged bot backing away, distance: {:.1f}", currentDistance);
                }
                return true; // Movement initiated
                
            } else if (currentDistance > optimalRange) {
                // Too far - move closer to optimal range
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MoveChase(target, optimalRange * 0.7f);
                
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Ranged bot moving to optimal range, distance: {:.1f}", currentDistance);
                }
                return true; // Movement initiated
            }
            
            // In good range - attack now
            if (ai->IsTank(bot)) {
                result = ai->DoSpecificAction("tank assist", event);
            } else {
                result = ai->DoSpecificAction("dps assist", event);
            }
            
            // Fallback to specific ranged actions if assist fails
            if (!result) {
                result = ai->DoSpecificAction("shoot", event);
                if (!result) {
                    result = ai->DoSpecificAction("cast combat spell", event);
                }
            }
            
            // Manual attack initiation if playerbot actions fail
            if (!result) {
                bot->Attack(target, false); // false = ranged attack
                result = true;
            }
            
        } else {
            // Hybrid or unknown type - use basic positioning and attack
            if (currentDistance > ATTACK_DISTANCE) {
                // Move closer
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MoveChase(target);
                return true; // Movement initiated
            }
            
            // Try basic attack actions
            result = ai->DoSpecificAction("dps assist", event);
            if (!result) {
                bot->Attack(target, currentDistance <= ATTACK_DISTANCE);
                result = true;
            }
        }
        
        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Combat type - Melee: {}, Ranged: {}, Distance: {:.1f}, Attack result: {}", 
                isMelee ? "YES" : "NO", isRanged ? "YES" : "NO", currentDistance, result ? "SUCCESS" : "FAILED");
        }
        
        return result;
    }

    bool Interact(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;

        if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
        {
            // Check interaction distance FIRST - move closer if needed
            float distance = bot->GetDistance(creature);
            if (distance > INTERACTION_DISTANCE)
            {
                // Too far - move closer first
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} moving to interact with {} at distance {:.1f}", 
                        bot->GetName(), creature->GetName(), distance);
                }
                
                // Calculate a position close to the creature but not directly on top
                float angle = creature->GetAngle(bot);
                float destX = creature->GetPositionX() + cos(angle + M_PI) * 3.0f; // 3 yards away
                float destY = creature->GetPositionY() + sin(angle + M_PI) * 3.0f;
                float destZ = creature->GetPositionZ();
                
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);
                return true; // Movement initiated, interaction will happen next cycle
            }
            
            // Check if this is a quest giver and handle quest interaction properly
            if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            {
                return InteractWithQuestGiver(bot, creature);
            }
            else
            {
                // For non-quest NPCs, use gossip hello action
                bot->SetFacingToObject(creature);
                Event event = Event("", std::to_string(guid.GetCounter()));
                return ai->DoSpecificAction("gossip hello", event);
            }
        }
        else if (GameObject* go = ObjectAccessor::GetGameObject(*bot, guid))
        {
            // Check interaction distance FIRST - move closer if needed  
            float distance = bot->GetDistance(go);
            float interactionDist = go->GetInteractionDistance();
            if (distance > interactionDist)
            {
                // Too far - move closer first
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} moving to interact with {} at distance {:.1f}", 
                        bot->GetName(), go->GetName(), distance);
                }
                
                // Calculate a position close to the object but not directly on top
                float angle = go->GetAngle(bot);
                float destX = go->GetPositionX() + cos(angle + M_PI) * 2.0f; // 2 yards away
                float destY = go->GetPositionY() + sin(angle + M_PI) * 2.0f;
                float destZ = go->GetPositionZ();
                
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);
                return true; // Movement initiated, interaction will happen next cycle
            }
            
            // Check if this is a quest giver game object
            if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
            {
                return InteractWithQuestGiver(bot, go);
            }
            else
            {
                // Use the bot's AI system to handle interaction with game objects
                bot->SetFacingToObject(go);
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
            
            // Look for quest-related gossip options with enhanced keyword matching
            if (message.find("quest") != std::string::npos || 
                message.find("Quest") != std::string::npos ||
                message.find("mission") != std::string::npos ||
                message.find("task") != std::string::npos ||
                message.find("reward") != std::string::npos ||
                message.find("complete") != std::string::npos ||
                message.find("turn in") != std::string::npos ||
                message.find("finish") != std::string::npos)
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
        
        // Set the target in the AI context if provided
        if (target) {
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
            
            // Check range requirements for the spell
            float spellRange = spellInfo->GetMaxRange(false);
            float currentDistance = bot->GetDistance(target);
            bool isMeleeSpell = spellRange <= ATTACK_DISTANCE;
            
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Casting spell {} on target at distance {:.1f}, spell range: {:.1f}", 
                    spellInfo->SpellName[0], currentDistance, spellRange);
            }
            
            // Handle positioning for spell casting
            Event moveEvent = Event("", "");
            if (isMeleeSpell && !bot->IsWithinMeleeRange(target)) {
                // Need to get into melee range for melee spells
                ai->DoSpecificAction("reach melee", moveEvent);
            } else if (!isMeleeSpell && currentDistance > spellRange) {
                // Need to get into spell range for ranged spells
                ai->DoSpecificAction("reach spell", moveEvent);
            } else if (!isMeleeSpell && currentDistance < 5.0f && ai->IsRanged(bot)) {
                // Ranged character too close - back away for better positioning
                ai->DoSpecificAction("flee", moveEvent);
            }
        }
        
        // Use the spell name directly as the action
        const char* spellName = spellInfo->SpellName[0];
        if (!spellName || !*spellName) return false;
        
        Event event = Event("", "");
        bool result = ai->DoSpecificAction(spellName, event);
        
        // If spell casting by name fails, try using spell ID
        if (!result && target) {
            // Try alternative approaches
            std::string spellIdStr = std::to_string(spellId);
            event = Event("", spellIdStr);
            result = ai->DoSpecificAction("cast", event);
        }
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Spell cast result for {}: {}", 
                spellName, result ? "SUCCESS" : "FAILED");
        }
        
        return result;
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

        // Check if quest is ready to turn in
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE || bot->GetQuestRewardStatus(questId))
        {
            if (g_EnableOllamaBotBuddyDebug)
            {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} cannot turn in quest {}: status={}, already rewarded={}", 
                    bot->GetName(), questId, bot->GetQuestStatus(questId), bot->GetQuestRewardStatus(questId));
            }
            return false;
        }
        
        if (!bot->CanRewardQuest(quest, false))
        {
            if (g_EnableOllamaBotBuddyDebug)
            {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} cannot reward quest {}: requirements not met", 
                    bot->GetName(), questId);
            }
            return false;
        }
        
        // Find quest giver in range
        ObjectGuid questGiverGuid;
        Map* map = bot->GetMap();
        if (map)
        {
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* creature = pair.second;
                if (!creature || !bot->IsWithinDistInMap(creature, INTERACTION_DISTANCE)) continue;
                if (!creature->hasInvolvedQuest(questId)) continue;
                
                questGiverGuid = creature->GetGUID();
                break;
            }
            
            // Also check game objects
            if (!questGiverGuid)
            {
                for (auto const& pair : map->GetGameObjectBySpawnIdStore())
                {
                    GameObject* go = pair.second;
                    if (!go || !bot->IsWithinDistInMap(go, INTERACTION_DISTANCE)) continue;
                    if (!go->hasInvolvedQuest(questId)) continue;
                    
                    questGiverGuid = go->GetGUID();
                    break;
                }
            }
        }
        
        if (!questGiverGuid)
        {
            if (g_EnableOllamaBotBuddyDebug)
            {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} cannot find quest giver for quest {}", 
                    bot->GetName(), questId);
            }
            return false;
        }
        
        // First, initiate quest completion dialog
        WorldPacket completePacket(CMSG_QUESTGIVER_COMPLETE_QUEST);
        completePacket << questGiverGuid << questId;
        completePacket.rpos(0);
        bot->GetSession()->HandleQuestgiverCompleteQuest(completePacket);
        
        // Handle quest rewards
        uint32 rewardIndex = 0;
        if (quest->GetRewChoiceItemsCount() > 1)
        {
            // Find the best reward using simple logic
            for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
            {
                if (quest->RewardChoiceItemId[i])
                {
                    ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                    if (item && bot->CanUseItem(item) == EQUIP_ERR_OK)
                    {
                        rewardIndex = i;
                        break; // Use first usable reward
                    }
                }
            }
        }
        
        // Complete the reward selection
        WorldPacket rewardPacket(CMSG_QUESTGIVER_CHOOSE_REWARD);
        rewardPacket << questGiverGuid << questId << rewardIndex;
        rewardPacket.rpos(0);
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(rewardPacket);
        
        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} turned in quest {}: {} with reward index {}", 
                bot->GetName(), questId, quest->GetTitle(), rewardIndex);
        }
        
        return true;
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


