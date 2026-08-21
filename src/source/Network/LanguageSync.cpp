#include "stdafx.h"
#include "Network/LanguageSync.h"

#include <string>

#include "Data/GameConfig/GameConfig.h"
#include "Dotnet/Connection.h"
#include "Dotnet/PacketFunctions_ClientToServer.h"
#include "Engine/Object/ZzzCharacter.h"

extern Connection* SocketClient;

namespace
{
// The server speaks Hebrew and English. Every other UI locale (the options
// window offers 13) gets English - a half-translated game is worse than a
// consistent English one, and the server would answer "unknown language"
// to anything else, which reads like an error the player caused.
std::wstring ServerCodeForLocale(const std::wstring& locale)
{
    return locale == L"he" ? L"he" : L"en";
}
}  // namespace

void SyncLanguageWithServer()
{
    GameConfig& config = GameConfig::GetInstance();
    const std::wstring code = ServerCodeForLocale(config.GetUILocale());

    // Nothing new to announce. This is the common case for the whole Hebrew
    // player base (the default marker is "he"), so nobody pays for this
    // feature with a chat message they never asked for.
    if (config.GetLanguageSynced() == code)
    {
        return;
    }

    // Not in game yet (login/character screens) - the world-entry call will
    // pick it up. The marker is intentionally NOT updated here, so the
    // change is never lost between scenes.
    if (SocketClient == nullptr || !SocketClient->IsConnected() || Hero == nullptr || Hero->ID[0] == L'\0')
    {
        return;
    }

    // The existing chat channel is the whole trick: "/language <code>" is a
    // regular chat message the server already understands and never
    // broadcasts (commands stay between the player and the server). The
    // server persists the choice on the account since 21/08/2026, so this
    // one message survives logout.
    const std::wstring message = L"/language " + code;
    SocketClient->ToGameServer()->SendPublicChatMessage(Hero->ID, message.c_str());

    config.SetLanguageSynced(code);
    config.Save();
}
