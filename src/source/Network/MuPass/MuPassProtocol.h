// MuPassProtocol.h: parsing of the MU Pass server packets (0xD2, 0x20-0x22)
// and the matching client requests. Parsed data is written into
// GameLogic::MuPass::GetState(); the MU Pass window only reads that state.
//////////////////////////////////////////////////////////////////////

#pragma once

namespace Network::MuPass
{
    // Server -> client. ReceiveBuffer points at the packet start (C1/C2 byte).
    void ReceiveStatus(const BYTE* ReceiveBuffer);
    void ReceiveMissionList(const BYTE* ReceiveBuffer);
    void ReceiveRewardTrack(const BYTE* ReceiveBuffer);

    // Client -> server.
    void SendStatusRequest();
    void SendCollectRequest(int iLevel, bool bProTrack);
    void SendProUpgradeRequest();
}
