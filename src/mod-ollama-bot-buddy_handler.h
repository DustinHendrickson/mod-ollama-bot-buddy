#include "ScriptMgr.h"
#include <string>
#include <Group.h>
#include <Channel.h>

extern std::unordered_map<uint64_t, std::deque<std::pair<std::string, std::string>>> botPlayerMessages;
extern std::mutex botPlayerMessagesMutex;

class BotBuddyChatHandler : public PlayerScript
{
public:
    BotBuddyChatHandler() : PlayerScript("BotBuddyChatHandler") {}

    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* group) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;

private:
    void ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel = nullptr, Player* receiver = nullptr);
};
