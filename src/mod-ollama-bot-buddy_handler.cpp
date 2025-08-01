#include "mod-ollama-bot-buddy_handler.h"
#include "Log.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <deque>
#include <chrono>

// Stores the last messages: [bot GUID][playerName] => pair<text, timestamp>
std::unordered_map<uint64_t, std::deque<std::pair<std::string, std::string>>> botPlayerMessages;
std::mutex botPlayerMessagesMutex;

void BotBuddyChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    ProcessChat(player, type, lang, msg, nullptr, nullptr);
}
void BotBuddyChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* /*group*/)
{
    ProcessChat(player, type, lang, msg, nullptr, nullptr);
}
void BotBuddyChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    ProcessChat(player, type, lang, msg, channel, nullptr);
}

void BotBuddyChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver)
{
    // For whispers, don't process in bot buddy - let ollama chat handle them
    return;
}

void BotBuddyChatHandler::ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel, Player* receiver)
{
    LOG_INFO("server.loading", "ProcessChat: sender={} type={} lang={} msg='{}' channel={} receiver={}", 
    player ? player->GetName() : "NULL", type, lang, msg, 
    channel ? channel->GetName() : "nullptr",
    receiver ? receiver->GetName() : "nullptr");

    if (!player || msg.empty()) return;
    PlayerbotAI* senderAI = sPlayerbotsMgr->GetPlayerbotAI(player);
    if (senderAI && senderAI->IsBotAI()) return;

    // For whispers, don't process - let ollama chat handle them
    if (receiver) return;

    std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);

    auto const& allPlayers = ObjectAccessor::GetPlayers();
    for (auto const& itr : allPlayers)
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsAlive()) continue;

        PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!botAI || !botAI->IsBotAI()) continue;

        std::string messageLower = msg;
        std::string botNameLower = bot->GetName();
        std::transform(messageLower.begin(), messageLower.end(), messageLower.begin(), ::tolower);
        std::transform(botNameLower.begin(), botNameLower.end(), botNameLower.begin(), ::tolower);

        // If the player mentions the bot in the message
        if (messageLower.find(botNameLower) != std::string::npos)
        {
            botPlayerMessages[bot->GetGUID().GetRawValue()].emplace_back(player->GetName(), msg);
        }
    }
}