#include "stdafx.h"
#include "AutoTestController.h"

#ifndef _WIN32

#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "Network/Server/WSclient.h"        // SocketClient, protocol states
#include "Network/Server/ServerListManager.h"
#include "Scenes/SceneCore.h"               // SceneFlag
#include "Scenes/SceneCommon.h"             // SelectedHero, EnableMainRender
#include "Scenes/CharacterScene.h"          // StartGame
#include "Engine/Object/ZzzCharacter.h"     // CharactersClient, Hero
#include "Engine/Object/ZzzInterface.h"     // LoadingWorld
#include "UI/Legacy/UIMng.h"                // CUIMng windows
#include "UI/NewUI/NewUISystem.h"           // g_pNewUISystem, g_pInGameShop
#include "GameShop/InGameShopSystem.h"      // g_InGameShopSystem

extern double WorldTime;
extern int LogIn;
extern wchar_t LogInID[MAX_USERNAME_SIZE + 1];
extern BYTE Version[SIZE_PROTOCOLVERSION];
extern BYTE Serial[SIZE_PROTOCOLSERIAL + 1];

// Defined in App/Platform/Windows/Winmain.cpp (compiled on every platform).
void RequestFrameCapture(const char* path);

namespace
{
    // Generous by design: CI machines are slow, the asset load alone can take
    // tens of seconds, and a false failure costs more than a slow pass. The
    // watchdog exists to end a hung run with a verdict, not to measure speed.
    constexpr double STEP_TIMEOUT_MS = 90000.0;
    // The hero spawns and then walks a step or two; the shop refuses to open
    // while moving, so give the world a moment to settle before asking.
    constexpr double SETTLE_MS = 4000.0;

    const wchar_t* EnvW(const char* name, wchar_t* buffer, size_t count)
    {
        const char* value = std::getenv(name);
        if (!value || !*value) return nullptr;
        const size_t written = std::mbstowcs(buffer, value, count - 1);
        if (written == static_cast<size_t>(-1)) return nullptr;
        buffer[written] = L'\0';
        return buffer;
    }
}

AutoTestController& AutoTestController::Instance()
{
    static AutoTestController instance;
    return instance;
}

bool AutoTestController::IsEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("MU_AUTOTEST");
        return value && *value == '1';
    }();
    return enabled;
}

double AutoTestController::ElapsedMs() const
{
    return WorldTime - m_stepStartedMs;
}

void AutoTestController::EnterStep(Step step, const char* label)
{
    m_step = step;
    m_stepStartedMs = WorldTime;
    std::fprintf(stderr, "[autotest] -> %s\n", label);
    std::fflush(stderr);
}

void AutoTestController::Capture(const char* label)
{
    char path[256];
    std::snprintf(path, sizeof(path), "autotest-%d-%s.ppm", ++m_shotIndex, label);
    RequestFrameCapture(path);
}

void AutoTestController::Fail(const char* why)
{
    std::fprintf(stderr, "[autotest] FAIL: %s\n", why);
    std::fflush(stderr);
    Capture("failure");
    m_step = Step::Failed;
    // One more frame is presented before the exit so the failure screenshot
    // actually gets written; the caller sees the message either way.
}

void AutoTestController::Pass()
{
    std::fprintf(stderr, "[autotest] PASS: logged in, entered the world, cash shop opened\n");
    std::fflush(stderr);
    m_step = Step::Done;
}

void AutoTestController::Update()
{
    if (!IsEnabled() || m_step == Step::Done || m_step == Step::Failed)
    {
        return;
    }

    if (m_step != Step::Idle && ElapsedMs() > STEP_TIMEOUT_MS)
    {
        Fail("step timed out");
        return;
    }

    switch (m_step)
    {
    case Step::Idle:
        // CreateLogInScene has opened the socket to the connect server by the
        // time the login scene is up; nothing to inject until the list lands.
        if (SceneFlag == LOG_IN_SCENE)
        {
            EnterStep(Step::PickServer, "waiting for the server list");
        }
        break;

    case Step::PickServer:
    {
        if (g_ServerListManager->GetServerGroupSize() <= 0)
        {
            break;
        }
        g_ServerListManager->SetFirst();
        CServerGroup* group = nullptr;
        if (!g_ServerListManager->GetNext(group) || group == nullptr)
        {
            Fail("server list arrived but the first group is missing");
            break;
        }
        CServerInfo* server = group->GetServerInfo(0);
        if (server == nullptr)
        {
            Fail("server group has no servers");
            break;
        }
        Capture("serverlist");
        CUIMng& uiMng = CUIMng::Instance();
        uiMng.HideWin(&uiMng.m_ServerSelWin);
        SocketClient->ToConnectServer()->SendConnectionInfoRequest(
            static_cast<uint16_t>(server->m_iConnectIndex));
        g_ServerListManager->SetSelectServerInfo(group->m_szName, server->m_iIndex,
                                                 server->m_byNonPvP);
        EnterStep(Step::Login, "connecting to the game server");
        break;
    }

    case Step::Login:
    {
        // SendLogin is only accepted once the game server's hello has been
        // processed - the same gate the login window enforces.
        if (CurrentProtocolState != RECEIVE_JOIN_SERVER_SUCCESS)
        {
            break;
        }
        wchar_t user[MAX_USERNAME_SIZE + 1] = { 0 };
        wchar_t pass[MAX_PASSWORD_SIZE + 1] = { 0 };
        if (!EnvW("MU_AUTOTEST_USER", user, _countof(user)) ||
            !EnvW("MU_AUTOTEST_PASS", pass, _countof(pass)))
        {
            Fail("MU_AUTOTEST_USER / MU_AUTOTEST_PASS are not set");
            break;
        }
        CUIMng& uiMng = CUIMng::Instance();
        uiMng.HideWin(&uiMng.m_LoginWin);
        LogIn = 1;
        wcscpy_s(LogInID, _countof(LogInID), user);
        CurrentProtocolState = REQUEST_LOG_IN;
        SocketClient->ToGameServer()->SendLogin(user, pass, Version, Serial);
        EnterStep(Step::PickCharacter, "logging in");
        break;
    }

    case Step::PickCharacter:
    {
        // Both states are valid entry points here: RECEIVE_CHARACTERS_LIST for
        // an account that already has characters, and
        // RECEIVE_CREATE_CHARACTER_SUCCESS for the one we just created - the
        // first run of this test hung precisely because it only accepted the
        // former, so a freshly created character was never selected.
        if (SceneFlag != CHARACTER_SCENE ||
            (CurrentProtocolState != RECEIVE_CHARACTERS_LIST &&
             CurrentProtocolState != RECEIVE_CREATE_CHARACTER_SUCCESS))
        {
            break;
        }
        // The character scene resets SelectedHero when it initialises, so the
        // selection has to happen after that, never before.
        int live = -1;
        for (int i = 0; i < MAX_CHARACTERS_PER_ACCOUNT; ++i)
        {
            if (CharactersClient[i].Object.Live)
            {
                live = i;
                break;
            }
        }
        if (live < 0)
        {
            // A fresh test account has no characters: create one, then come
            // back through this same step when the server confirms it.
            if (!m_createRequested)
            {
                Capture("charcreate");
                const auto classByte = static_cast<CharacterClassNumber>(
                    (CharacterView.Class << 2) + CharacterView.Skin);
                CurrentProtocolState = REQUEST_CREATE_CHARACTER;
                SocketClient->ToGameServer()->SendCreateCharacter(L"MacTest", classByte);
                m_createRequested = true;
                m_stepStartedMs = WorldTime;   // restart the watchdog for the create
            }
            break;
        }
        Capture("charlist");
        // The creation window auto-opens on an empty account and stays up
        // afterwards; close it so the world screenshots are not taken through
        // a dialog.
        CUIMng& uiMng = CUIMng::Instance();
        uiMng.HideWin(&uiMng.m_CharMakeWin);
        SelectedHero = live;
        StartGame();
        EnterStep(Step::EnterWorld, "entering the world");
        break;
    }

    case Step::EnterWorld:
        if (SceneFlag == MAIN_SCENE && EnableMainRender && LoadingWorld < 30)
        {
            Capture("world");
            EnterStep(Step::Settle, "waiting for the hero to settle");
        }
        break;

    case Step::Settle:
        if (ElapsedMs() >= SETTLE_MS)
        {
            EnterStep(Step::OpenShop, "opening the cash shop");
        }
        break;

    case Step::OpenShop:
    {
        if (g_pNewUISystem->IsVisible(SEASON3B::INTERFACE_INGAMESHOP))
        {
            Capture("shop");
            Pass();
            break;
        }
        // IsInGameShopOpen gates on standing still, being in a safe zone and
        // the catalog having downloaded.
        if (!g_pInGameShop->IsInGameShopOpen())
        {
            break;
        }

        // The catalog and banners are fetched lazily, by the X-key handler, on
        // the first open - so a controller that only sends the open request
        // waits forever for a shop that was never downloaded. This is the same
        // sequence CNewUIHotKey performs, and it is the part that actually
        // exercises the HTTP catalog download over the network.
        if (g_InGameShopSystem->IsScriptDownload())
        {
            if (!g_InGameShopSystem->ScriptDownload())
            {
                Fail("cash shop catalog download failed");
                break;
            }
        }
        if (g_InGameShopSystem->IsBannerDownload())
        {
            if (g_InGameShopSystem->BannerDownload())
            {
                g_pInGameShop->InitBanner(g_InGameShopSystem->GetBannerFileName(),
                                          g_InGameShopSystem->GetBannerURL());
            }
        }

        // The window is opened by the server's reply, not locally.
        if (!g_InGameShopSystem->GetIsRequestShopOpenning())
        {
            SocketClient->ToGameServer()->SendCashShopOpenState(0);
            g_InGameShopSystem->SetIsRequestShopOpenning(true);
        }
        break;
    }

    default:
        break;
    }
}

#else  // _WIN32

AutoTestController& AutoTestController::Instance()
{
    static AutoTestController instance;
    return instance;
}

// The autotest exists for the headless CI builds; on Windows it is inert so the
// shipped client carries no self-driving code path at all.
bool AutoTestController::IsEnabled() { return false; }
void AutoTestController::Update() {}

#endif // _WIN32
