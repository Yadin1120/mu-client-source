// Unattended end-to-end self-test: log in, enter the world, open the cash shop.
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
        CloseShop,      // put it away before opening the next window
        OpenMuPass,     // open the MU Pass window
        WindowTour,     // walk the rest of the player-facing windows
        Done,
        Failed,
    };

    // One entry per window the tour opens; m_tourIndex walks this table.
    struct TourEntry
    {
        int interfaceId;
        const char* label;
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
};
