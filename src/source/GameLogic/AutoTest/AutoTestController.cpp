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
extern int MouseX;
extern int MouseY;
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
    // Back-to-back CI runs share one test account, and the server holds the
    // previous session for a short while ("your account is already connected").
    constexpr double LOGIN_RETRY_MS = 20000.0;
    constexpr int MAX_LOGIN_ATTEMPTS = 4;
    // Let a freshly shown window lay itself out before photographing it.
    constexpr double WINDOW_SETTLE_MS = 1200.0;
    constexpr double HOVER_SETTLE_MS = 1200.0;
    // The storage list is a round trip to the server (request on shop open, a
    // count packet, then one packet per item), so it gets its own window rather
    // than being judged on the first frame it is looked at.
    constexpr double STORAGE_SETTLE_MS = 5000.0;
    // A capture is a request, not an action: RequestFrameCapture only marks a
    // path, and the framebuffer is written when the frame is presented. Hiding
    // the window in the same Update() therefore photographed the frame AFTER it
    // was gone - every window-tour screenshot up to 13/08/2026 was of an empty
    // world, which is exactly how a broken tour looks like a working one. Hold
    // the window open a few frames past the request before putting it away.
    constexpr double CAPTURE_HOLD_MS = 400.0;
    // First product tile in the cash shop, in the 640x480 reference space the
    // UI is authored in (the renderer scales it to the real window).
    constexpr int SHOP_FIRST_ITEM_X = 372;
    constexpr int SHOP_FIRST_ITEM_Y = 180;

    // The player-facing windows worth a screenshot, in the order a person would
    // reasonably open them. Anything needing an NPC, a party or a guild is
    // included too - it simply gets skipped when it declines to open.
    const AutoTestController::TourEntry kTour[] = {
        { SEASON3B::INTERFACE_INVENTORY,        "inventory" },
        { SEASON3B::INTERFACE_CHARACTER,        "character" },
        { SEASON3B::INTERFACE_SKILL_LIST,       "skills" },
        { SEASON3B::INTERFACE_MASTER_LEVEL,     "masterlevel" },
        { SEASON3B::INTERFACE_MYQUEST,          "quests" },
        { SEASON3B::INTERFACE_PARTY,            "party" },
        { SEASON3B::INTERFACE_GUILDINFO,        "guild" },
        { SEASON3B::INTERFACE_OPTION,           "options" },
        { SEASON3B::INTERFACE_HELP,             "help" },
        { SEASON3B::INTERFACE_MUHELPER,         "muhelper" },
        { SEASON3B::INTERFACE_CHATLOGWINDOW,    "chatlog" },
        { SEASON3B::INTERFACE_JEWELBANK,       "jewelbank" },
        { SEASON3B::INTERFACE_MINI_MAP,         "minimap" },
    };

    // The town NPCs worth hailing, by the server's NPC number. Their dialogue is
    // the longest prose in the game and the only place a player reads full
    // sentences, which is exactly where Hebrew line breaking shows up broken -
    // and no window in kTour above reaches it, because these need an NPC.
    //
    // Whoever is out of view is skipped rather than walked to: every one of
    // these stands inside or beside the Lorencia spawn square, so a tour that
    // only hails what it can already see costs nothing and needs no pathfinding.
    // If a future entry sits further out, that is when walking earns its keep.
    const AutoTestController::NpcEntry kNpcTour[] = {
        { 371, "npc-leo" },             // Leo The Helper - the tutorial guide
        { 253, "npc-potion-girl" },     // Amy
        { 250, "npc-merchant" },        // Wandering Merchant Harold
        { 246, "npc-weapons" },         // Zienna
        { 257, "npc-elf-soldier" },     // the blessing NPC
    };
    // A hail is a round trip; give the server time to answer before deciding
    // this NPC had nothing to say.
    constexpr double NPC_ANSWER_MS = 4000.0;

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
    std::fprintf(stderr, "[autotest] PASS: logged in, entered the world, cash shop (with its storage list), MU Pass and the town NPCs\n");
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
        ++m_loginAttempts;
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
            // A back-to-back run hits "your account is already connected":
            // the previous session is still held server-side and the login is
            // refused, leaving us waiting on a character list that will never
            // arrive. The hold clears on its own, so retry the login a few
            // times before giving up - the same reasoning ReconnectManager
            // applies after a crash.
            if (ElapsedMs() > LOGIN_RETRY_MS && m_loginAttempts < MAX_LOGIN_ATTEMPTS)
            {
                std::fprintf(stderr, "[autotest] no character list yet - retrying the login "
                                     "(attempt %d)\n", m_loginAttempts + 1);
                std::fflush(stderr);
                CUIMng& retryUi = CUIMng::Instance();
                retryUi.HideWin(&retryUi.m_MsgWin);
                CurrentProtocolState = RECEIVE_JOIN_SERVER_SUCCESS;
                EnterStep(Step::Login, "retrying the login");
            }
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
            EnterStep(Step::ShopItemHover, "hovering a shop product");
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

    case Step::ShopItemHover:
        // The tooltip follows the cursor, and the cursor is just a pair of
        // globals - park it over the first product tile and let a few frames
        // render so the tooltip is actually on screen when the shot is taken.
        // This is the one capture that proves item names and descriptions
        // (the longest Hebrew strings the game has) render correctly.
        MouseX = SHOP_FIRST_ITEM_X;
        MouseY = SHOP_FIRST_ITEM_Y;
        if (ElapsedMs() > HOVER_SETTLE_MS)
        {
            Capture("shop-item-tooltip");
            EnterStep(Step::CheckStorage, "checking the cash shop storage list");
        }
        break;

    case Step::CheckStorage:
    {
        // The one check here that a screenshot cannot make, and the reason this
        // step exists: on 13/08/2026 a Mac player bought an item, was charged,
        // and found an empty storage. Nothing was lost - the client decoded the
        // per-item packet at the wrong offsets, because its struct used `long`
        // (4 bytes on Windows, 8 on macOS) and came out 53 bytes against a
        // 33-byte packet. Every screenshot of that build looks perfectly fine.
        //
        // What makes it catchable without knowing the account's contents: the
        // count packet (0x06) and the item packets (0x0D) are decoded through
        // completely separate paths, so the server's own "this page holds N
        // items" is a free oracle for "N rows should exist". Any gap is a
        // decoding bug, on any platform.
        //
        // The list is requested by the client itself on shop open, so by now it
        // has usually landed; wait out the settle window before judging, and
        // let the step watchdog end a run where it never arrives at all.
        const int expected = g_pInGameShop->GetStorageExpectedRowCount();
        const int actual = g_pInGameShop->GetStorageActualRowCount();
        if (expected > 0 && actual >= expected)
        {
            std::fprintf(stderr, "[autotest] storage: %d announced, %d decoded - ok\n", expected, actual);
            std::fflush(stderr);
            Capture("shop-storage");
            EnterStep(Step::CloseShop, "closing the cash shop");
            break;
        }
        if (ElapsedMs() <= STORAGE_SETTLE_MS)
        {
            break;
        }
        if (expected <= 0)
        {
            // Not a failure, but the check proved nothing - say so loudly rather
            // than let a green run imply the storage was verified. Fix by giving
            // the test account a cash shop item.
            std::fprintf(stderr, "[autotest] storage: the test account has no stored items - NOTHING VERIFIED\n");
            std::fflush(stderr);
            Capture("shop-storage-empty");
            EnterStep(Step::CloseShop, "closing the cash shop");
            break;
        }
        char why[160];
        std::snprintf(why, sizeof(why),
                      "storage list: server announced %d items on this page, the client decoded %d rows",
                      expected, actual);
        Fail(why);
        break;
    }

    case Step::CloseShop:
        // Tell the server the shop is closed, the way the X key does, so the
        // next window opens on a clean screen instead of on top of the shop.
        if (g_pNewUISystem->IsVisible(SEASON3B::INTERFACE_INGAMESHOP))
        {
            SocketClient->ToGameServer()->SendCashShopOpenState(1);
            g_pNewUISystem->Hide(SEASON3B::INTERFACE_INGAMESHOP);
            break;
        }
        EnterStep(Step::OpenMuPass, "opening MU Pass");
        break;

    case Step::OpenMuPass:
        // Purely local, unlike the shop: the T key just toggles the window.
        if (g_pNewUISystem->IsVisible(SEASON3B::INTERFACE_MUPASS))
        {
            if (m_capturedMs == 0.0)
            {
                Capture("mupass");
                m_capturedMs = WorldTime;
                break;
            }
            if (WorldTime - m_capturedMs < CAPTURE_HOLD_MS)
                break;

            m_capturedMs = 0.0;
            g_pNewUISystem->Hide(SEASON3B::INTERFACE_MUPASS);
            m_tourOpenedMs = WorldTime;
            EnterStep(Step::WindowTour, "touring the remaining windows");
            break;
        }
        g_pNewUISystem->Toggle(SEASON3B::INTERFACE_MUPASS);
        break;

    case Step::WindowTour:
    {
        // One window per pass: show it, give it a few frames to lay itself out,
        // photograph it, hide it, move on. Windows that refuse to open (some
        // need an NPC, a party or a guild) are skipped rather than failing the
        // run - the point is a visual sweep of the Hebrew UI, not a feature
        // audit.
        if (m_tourIndex >= static_cast<int>(_countof(kTour)))
        {
            EnterStep(Step::TalkToNpc, "hailing the town NPCs");
            break;
        }
        const TourEntry& entry = kTour[m_tourIndex];
        if (WorldTime - m_tourOpenedMs < WINDOW_SETTLE_MS)
        {
            g_pNewUISystem->Show(entry.interfaceId);
            break;
        }
        if (g_pNewUISystem->IsVisible(entry.interfaceId))
        {
            if (m_capturedMs == 0.0)
            {
                Capture(entry.label);
                m_capturedMs = WorldTime;
                break;
            }
            if (WorldTime - m_capturedMs < CAPTURE_HOLD_MS)
                break;

            g_pNewUISystem->Hide(entry.interfaceId);
        }
        else
        {
            std::fprintf(stderr, "[autotest] window '%s' did not open - skipping\n", entry.label);
            std::fflush(stderr);
        }
        m_capturedMs = 0.0;
        ++m_tourIndex;
        m_tourOpenedMs = WorldTime;
        m_stepStartedMs = WorldTime;   // each window gets its own watchdog
        break;
    }

    case Step::TalkToNpc:
    {
        // NPC prose is the longest running text in the game, and the only place
        // a player reads whole sentences - so it is where a broken line-breaker
        // shows itself. A player reported exactly that on macOS: the welcome
        // speech came out shredded into two- and three-letter lines while every
        // other window looked right. None of the windows above can reach that
        // text, because it only arrives in answer to hailing an NPC.
        if (m_npcIndex >= static_cast<int>(_countof(kNpcTour)))
        {
            Pass();
            break;
        }

        const NpcEntry& npc = kNpcTour[m_npcIndex];
        const int index = FindCharacterIndexByMonsterIndex(npc.monsterIndex);
        if (index >= MAX_CHARACTERS_CLIENT)
        {
            // Out of view. Say which one, so a tour that quietly covers less
            // than it appears to is visible in the log rather than implied.
            std::fprintf(stderr, "[autotest] NPC '%s' is not in view - skipping\n", npc.label);
            std::fflush(stderr);
            ++m_npcIndex;
            m_stepStartedMs = WorldTime;
            break;
        }

        SocketClient->ToGameServer()->SendTalkToNpcRequest(
            static_cast<uint16_t>(CharactersClient[index].Key));
        EnterStep(Step::NpcDialogue, npc.label);
        break;
    }

    case Step::NpcDialogue:
    {
        const NpcEntry& npc = kNpcTour[m_npcIndex];
        // Merchants answer with their shop, quest givers with the dialogue box.
        // Photograph whichever came up - both are full of Hebrew, and the shop
        // is a useful shot in its own right.
        const bool dialogue = g_pNewUISystem->IsVisible(SEASON3B::INTERFACE_NPC_DIALOGUE);
        const bool shop = g_pNewUISystem->IsVisible(SEASON3B::INTERFACE_NPCSHOP);
        if (dialogue || shop)
        {
            if (m_capturedMs == 0.0)
            {
                Capture(npc.label);
                m_capturedMs = WorldTime;
                break;
            }
            if (WorldTime - m_capturedMs < CAPTURE_HOLD_MS)
                break;

            g_pNewUISystem->Hide(dialogue ? SEASON3B::INTERFACE_NPC_DIALOGUE
                                          : SEASON3B::INTERFACE_NPCSHOP);
        }
        else if (ElapsedMs() <= NPC_ANSWER_MS)
        {
            break;   // still waiting on the server
        }
        else
        {
            std::fprintf(stderr, "[autotest] NPC '%s' answered with no window\n", npc.label);
            std::fflush(stderr);
        }

        m_capturedMs = 0.0;
        ++m_npcIndex;
        EnterStep(Step::TalkToNpc, "hailing the town NPCs");
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
