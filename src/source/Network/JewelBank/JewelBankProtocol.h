// JewelBankProtocol.h: parsing of the jewel bank state packet (0xD2, 0x30)
// and the matching client requests. Parsed data is written into
// GameLogic::JewelBank::GetState(); the bank window only reads that state.
//////////////////////////////////////////////////////////////////////

#pragma once

namespace Network::JewelBank
{
    // The amount value which asks the server to fill every free inventory slot.
    inline constexpr int WITHDRAW_FILL = 255;

    // Server -> client. ReceiveBuffer points at the packet start (C2 byte).
    void ReceiveState(const BYTE* ReceiveBuffer);

    // Client -> server.
    void SendStatusRequest();
    void SendDepositRequest(int iInventorySlot);
    void SendWithdrawRequest(int iJewelType, int iAmount);
}
