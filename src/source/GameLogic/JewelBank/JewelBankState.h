// JewelBankState.h: client-side snapshot of the jewel bank balances, filled
// from the server packet (see Network::JewelBank) and read by the bank window.
//////////////////////////////////////////////////////////////////////

#pragma once

#include <vector>

namespace GameLogic::JewelBank
{
    // The server never sends more than the configured jewel types (ten today), but the
    // count arrives as a byte, so the window is sized from the vector and not the other
    // way around.
    struct Entry
    {
        int iJewelType;     // the mix number, sent back in the withdraw request
        int iItemType;      // group*512+number of the single jewel, for icon and name
        int iAmount;
    };

    struct State
    {
        bool bReceived;
        unsigned long dwReceivedTick;
        std::vector<Entry> Entries;
    };

    State& GetState();
}
