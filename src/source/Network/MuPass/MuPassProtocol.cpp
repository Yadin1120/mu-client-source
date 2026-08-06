// MuPassProtocol.cpp: parsing of the MU Pass server packets and the
// matching client requests.
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MuPassProtocol.h"
#include "GameLogic/MuPass/MuPassState.h"
#include "Network/Server/WSclient.h"
#include "Dotnet/Connection.h"
#include "Data/Translation/MultiLanguage.h"

namespace Network::MuPass
{
    namespace
    {
        constexpr int C1_PAYLOAD_OFFSET = 4;    // C1, size, 0xD2, subcode
        constexpr int C2_PAYLOAD_OFFSET = 5;    // C2, size(2), 0xD2, subcode

        // The MU Pass send functions are custom exports of MUnique.Client.Library
        // (see ConnectionManager.ClientToServer.Custom.cs). They aren't part of the
        // generated packet bindings, so we resolve them from the managed library
        // directly rather than through PacketFunctions_ClientToServer.
        using SendSimpleFn = void(CORECLR_DELEGATE_CALLTYPE*)(int32_t);
        using SendCollectFn = void(CORECLR_DELEGATE_CALLTYPE*)(int32_t, uint8_t, uint8_t);

        int GetConnectionHandle()
        {
            return SocketClient != nullptr ? SocketClient->GetHandle() : -1;
        }
        constexpr int MISSION_TEXT_BYTES = 96;
        constexpr int REWARD_LABEL_BYTES = 48;

#pragma pack(push, 1)
        struct StatusPayload
        {
            BYTE Level;
            BYTE IsPro;
            BYTE CompletedTodayFree;
            BYTE TotalTodayFree;
            INT TotalPoints;
            INT PointsIntoLevel;
            INT PointsForNextLevel;
            INT SecondsUntilReset;
            WORD SeasonDaysLeft;
            BYTE DailyBonusGranted;
            BYTE ReadyRewardCount;
            INT ProPriceCredits;
            INT DailyBonusPoints;
        };

        struct MissionEntry
        {
            BYTE IsPro;
            BYTE Completed;
            BYTE DaysAgo;
            BYTE Padding;
            INT Progress;
            INT Target;
            INT Points;
            char Text[MISSION_TEXT_BYTES];
        };

        struct RewardLevelEntry
        {
            BYTE Level;
            BYTE FreeGroup;
            WORD FreeNumber;
            BYTE FreeAmount;
            BYTE FreeItemLevel;
            BYTE FreeState;
            BYTE ProGroup;
            WORD ProNumber;
            BYTE ProAmount;
            BYTE ProItemLevel;
            BYTE ProState;
            BYTE Padding;
            char FreeLabel[REWARD_LABEL_BYTES];
            char ProLabel[REWARD_LABEL_BYTES];

            // Appended after the labels, so the offsets above stay as they were.
            BYTE FreeItemData[GameLogic::MuPass::REWARD_ITEM_DATA_BYTES];
            BYTE ProItemData[GameLogic::MuPass::REWARD_ITEM_DATA_BYTES];
        };
#pragma pack(pop)

        void ParseTrackReward(
            GameLogic::MuPass::TrackReward& reward,
            BYTE byGroup,
            WORD wNumber,
            BYTE byAmount,
            BYTE byItemLevel,
            BYTE byState,
            const char* szLabelUtf8,
            const BYTE* pItemData)
        {
            using GameLogic::MuPass::RewardState;

            reward.eState = static_cast<RewardState>(byState);
            reward.iAmount = byAmount;
            reward.iItemLevel = byItemLevel;
            reward.iItemType = reward.eState == RewardState::None
                ? -1
                : (byGroup * MAX_ITEM_INDEX) + wNumber;
            CMultiLanguage::ConvertFromUtf8(reward.szLabel, szLabelUtf8, REWARD_LABEL_BYTES);
            reward.szLabel[GameLogic::MuPass::REWARD_LABEL_LEN - 1] = L'\0';

            // The server leaves the field all-zero when the reward is not a concrete item
            // (credits, random excellent); a zero group and number can never be a real item.
            memcpy(reward.ItemData, pItemData, GameLogic::MuPass::REWARD_ITEM_DATA_BYTES);
            reward.bHasItemData = (pItemData[0] != 0) || (pItemData[1] != 0);
        }
    }

    void ReceiveStatus(const BYTE* ReceiveBuffer)
    {
        const auto* pPayload = reinterpret_cast<const StatusPayload*>(ReceiveBuffer + C1_PAYLOAD_OFFSET);
        GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();

        state.iLevel = pPayload->Level;
        state.bIsPro = pPayload->IsPro != 0;
        state.iCompletedTodayFree = pPayload->CompletedTodayFree;
        state.iTotalTodayFree = pPayload->TotalTodayFree;
        state.iTotalPoints = pPayload->TotalPoints;
        state.iPointsIntoLevel = pPayload->PointsIntoLevel;
        state.iPointsForNextLevel = pPayload->PointsForNextLevel;
        state.iSecondsUntilReset = pPayload->SecondsUntilReset;
        state.iSeasonDaysLeft = pPayload->SeasonDaysLeft;
        state.bDailyBonusGranted = pPayload->DailyBonusGranted != 0;
        state.iReadyRewardCount = pPayload->ReadyRewardCount;
        state.iProPriceCredits = pPayload->ProPriceCredits;
        state.iDailyBonusPoints = pPayload->DailyBonusPoints;
        state.dwStatusReceivedTick = GetTickCount();
        state.bReceived = true;
    }

    void ReceiveMissionList(const BYTE* ReceiveBuffer)
    {
        const int iCount = ReceiveBuffer[C2_PAYLOAD_OFFSET];
        const auto* pEntries = reinterpret_cast<const MissionEntry*>(ReceiveBuffer + C2_PAYLOAD_OFFSET + 1);

        GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
        state.Missions.clear();
        state.Missions.reserve(iCount);

        for (int i = 0; i < iCount; ++i)
        {
            const MissionEntry& entry = pEntries[i];
            GameLogic::MuPass::Mission mission{};
            mission.bIsPro = entry.IsPro != 0;
            mission.bCompleted = entry.Completed != 0;
            mission.iDaysAgo = entry.DaysAgo;
            mission.iProgress = entry.Progress;
            mission.iTarget = entry.Target;
            mission.iPoints = entry.Points;
            CMultiLanguage::ConvertFromUtf8(mission.szText, entry.Text, MISSION_TEXT_BYTES);
            mission.szText[GameLogic::MuPass::MISSION_TEXT_LEN - 1] = L'\0';
            state.Missions.push_back(mission);
        }
    }

    void ReceiveRewardTrack(const BYTE* ReceiveBuffer)
    {
        const int iCount = ReceiveBuffer[C2_PAYLOAD_OFFSET];
        const auto* pEntries = reinterpret_cast<const RewardLevelEntry*>(ReceiveBuffer + C2_PAYLOAD_OFFSET + 1);

        GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
        state.Track.clear();
        state.Track.reserve(iCount);

        for (int i = 0; i < iCount; ++i)
        {
            const RewardLevelEntry& entry = pEntries[i];
            GameLogic::MuPass::TrackLevel level{};
            level.iLevel = entry.Level;
            ParseTrackReward(level.Free, entry.FreeGroup, entry.FreeNumber, entry.FreeAmount, entry.FreeItemLevel, entry.FreeState, entry.FreeLabel, entry.FreeItemData);
            ParseTrackReward(level.Pro, entry.ProGroup, entry.ProNumber, entry.ProAmount, entry.ProItemLevel, entry.ProState, entry.ProLabel, entry.ProItemData);
            state.Track.push_back(level);
        }
    }

    void SendStatusRequest()
    {
        const int iHandle = GetConnectionHandle();
        if (iHandle < 0)
        {
            return;
        }

        static SendSimpleFn s_pfn = LoadManagedSymbol<SendSimpleFn>("SendMuPassStatusRequest");
        if (s_pfn != nullptr)
        {
            s_pfn(iHandle);
        }
    }

    void SendCollectRequest(int iLevel, bool bProTrack)
    {
        const int iHandle = GetConnectionHandle();
        if (iHandle < 0)
        {
            return;
        }

        static SendCollectFn s_pfn = LoadManagedSymbol<SendCollectFn>("SendMuPassCollectRequest");
        if (s_pfn != nullptr)
        {
            s_pfn(iHandle, static_cast<uint8_t>(iLevel), bProTrack ? 1 : 0);
        }
    }

    void SendProUpgradeRequest()
    {
        const int iHandle = GetConnectionHandle();
        if (iHandle < 0)
        {
            return;
        }

        static SendSimpleFn s_pfn = LoadManagedSymbol<SendSimpleFn>("SendMuPassProUpgradeRequest");
        if (s_pfn != nullptr)
        {
            s_pfn(iHandle);
        }
    }
}
