// MuPassState.cpp: client-side snapshot of the MU Pass state.
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MuPassState.h"

namespace GameLogic::MuPass
{
    State& GetState()
    {
        static State s_state{};
        return s_state;
    }

    int GetSecondsUntilResetNow()
    {
        const State& state = GetState();
        if (!state.bReceived)
        {
            return 0;
        }

        const int iElapsedSeconds = static_cast<int>((GetTickCount() - state.dwStatusReceivedTick) / 1000);
        const int iRemaining = state.iSecondsUntilReset - iElapsedSeconds;
        return iRemaining > 0 ? iRemaining : 0;
    }

    int GetFirstReadyLevel(bool* pbOutPro)
    {
        const State& state = GetState();
        for (const TrackLevel& level : state.Track)
        {
            if (level.Free.eState == RewardState::Ready)
            {
                if (pbOutPro != nullptr)
                {
                    *pbOutPro = false;
                }

                return level.iLevel;
            }

            if (level.Pro.eState == RewardState::Ready)
            {
                if (pbOutPro != nullptr)
                {
                    *pbOutPro = true;
                }

                return level.iLevel;
            }
        }

        return 0;
    }
}
