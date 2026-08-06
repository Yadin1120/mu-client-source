// MuPassState.h: client-side snapshot of the MU Pass state, filled from
// server packets (see Network::MuPass) and read by the MU Pass window.
//////////////////////////////////////////////////////////////////////

#pragma once

#include <vector>

namespace GameLogic::MuPass
{
    inline constexpr int MAX_PASS_LEVEL = 30;

    // These wide-char buffers must be at least as large as the UTF-8 byte length of
    // the matching packet string field (96 and 48 bytes). ConvertFromUtf8 writes up to
    // one wchar_t per source byte into the target, so a smaller buffer overruns the
    // stack (the packet pads unused bytes with NULs, each decoding to one wide NUL).
    inline constexpr int MISSION_TEXT_LEN = 96;
    inline constexpr int REWARD_LABEL_LEN = 48;

    // The reward item, serialized by the server in the very same format every other item
    // arrives in, so the reward track can build a real ITEM and show the normal item
    // tooltip for it. Raw bytes, so unlike the text fields above there is no conversion
    // involved and no buffer to overrun. All-zero means "not a concrete item".
    inline constexpr int REWARD_ITEM_DATA_BYTES = 15;

    enum class RewardState : unsigned char
    {
        Locked = 0,
        Ready = 1,
        Collected = 2,
        None = 3,
    };

    struct Mission
    {
        bool bIsPro;
        bool bCompleted;
        int iDaysAgo;
        int iProgress;
        int iTarget;
        int iPoints;
        wchar_t szText[MISSION_TEXT_LEN];
    };

    struct TrackReward
    {
        int iItemType;      // group*512+number, -1 when the track has no reward
        int iItemLevel;
        int iAmount;
        RewardState eState;
        wchar_t szLabel[REWARD_LABEL_LEN];

        // Serialized reward item; bHasItemData is false for credits and random-excellent
        // rewards, which have no concrete item to show a tooltip for.
        bool bHasItemData;
        unsigned char ItemData[REWARD_ITEM_DATA_BYTES];
    };

    struct TrackLevel
    {
        int iLevel;
        TrackReward Free;
        TrackReward Pro;
    };

    struct State
    {
        bool bReceived;

        int iLevel;
        bool bIsPro;
        int iCompletedTodayFree;
        int iTotalTodayFree;
        int iTotalPoints;
        int iPointsIntoLevel;
        int iPointsForNextLevel;
        int iSecondsUntilReset;
        int iSeasonDaysLeft;
        bool bDailyBonusGranted;
        int iReadyRewardCount;
        int iProPriceCredits;
        int iDailyBonusPoints;
        unsigned long dwStatusReceivedTick;     // GetTickCount() at receive, for the live reset countdown

        std::vector<Mission> Missions;
        std::vector<TrackLevel> Track;
    };

    State& GetState();

    // Remaining seconds until the daily mission reset, counted down locally
    // from the last status packet.
    int GetSecondsUntilResetNow();

    // The first level whose free (or pro, when unlocked) reward is ready to
    // collect; 0 when nothing is ready.
    int GetFirstReadyLevel(bool* pbOutPro);
}
