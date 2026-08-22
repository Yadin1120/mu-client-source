// JewelBankState.cpp: client-side snapshot of the jewel bank balances.
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "JewelBankState.h"

namespace GameLogic::JewelBank
{
    State& GetState()
    {
        static State s_state{};
        return s_state;
    }
}
