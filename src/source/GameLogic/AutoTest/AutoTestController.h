// Unattended end-to-end self-test: log in, enter the world, open the cash shop
// and assert its storage list decoded correctly.
//
// Why this exists: the macOS client can only be judged by a human with a Mac,
// and that turns every fix into a day of waiting on someone else's screen. This
// drives the same sequence a player performs - server select, login, character
// select, world load, shop open - with no input, capturing a screenshot at each
// milestone and reporting PASS/FAIL on stdout. CI can then answer "does the Mac
// build actually play?" on every commit.
//
// Off by default and inert unless MU_AUTOTEST=1 is set in the environment, so a
// shipped binary can never wander into it.
//
// Modelled on ReconnectManager, which already performs the unattended half of
// this sequence after a dropped connection: same per-frame Update() from
// RenderScene, same WorldTime-based watchdogs, same rule of only injecting the
// steps the scene loops cannot take on their own.
#pragma once

class AutoTestController
{
public:
    static AutoTestController& Instance();

    // True when MU_AUTOTEST=1. Cheap; the result is cached.
    static bool IsEnabled();

    // Once per rendered frame, in every scene. No-op unless enabled.
    void Update();

    // One entry per window the window tour opens. Public because the table
    // itself lives in the .cpp, next to the tuning constants it belongs with.
    struct TourEntry
    {
        int interfaceId;
        const char* label;
    };

    // One entry per NPC the town tour hails. Same shape as TourEntry, and
    // public for the same reason: the table itself lives in the .cpp. Keyed by
    // the monster/NPC number the server assigns rather than a window id.
    struct NpcEntry
    {
        int monsterIndex;
        const char* label;
    };

private:
    AutoTestController() = default;

    enum class Step
    {
        Idle,           // waiting for the login scene to come up
        PickServer,     // server list received -> select the first server
        Login,          // game server hello received -> send credentials
        PickCharacter,  // character list received -> select or create
        EnterWorld,     // waiting for the world to finish loading
        Settle,         // let the hero come to rest inside the safe zone
        OpenShop,       // request the cash shop
        ShopItemHover,  // park the cursor on a product so its tooltip renders
        CheckStorage,   // the storage list decoded as many rows as were announced
        CloseShop,      // put it away before opening the next window
        OpenMuPass,     // open the MU Pass window
        WindowTour,     // walk the rest of the player-facing windows
        TalkToNpc,      // pick the next town NPC that is in view and hail it
        NpcDialogue,    // photograph whatever window it answered with
        Done,
        Failed,
    };

    void EnterStep(Step step, const char* label);
    double ElapsedMs() const;
    // Screenshot named "<n>-<label>.ppm" next to the executable, via the
    // capture hook in Winmain.cpp. Best effort - a failed capture never fails
    // the run, since the PASS/FAIL verdict comes from the state machine.
    void Capture(const char* label);
    void Fail(const char* why);
    void Pass();

    Step m_step = Step::Idle;
    double m_stepStartedMs = 0.0;
    int m_shotIndex = 0;
    bool m_createRequested = false;
    int m_loginAttempts = 0;
    int m_tourIndex = 0;
    double m_tourOpenedMs = 0.0;
    int m_npcIndex = 0;
    // WorldTime of the pending capture request, 0 when none is outstanding.
    // A window must stay open for a few frames after Capture() or the shot
    // lands on the frame after it closed - see CAPTURE_HOLD_MS.
    double m_capturedMs = 0.0;
};
