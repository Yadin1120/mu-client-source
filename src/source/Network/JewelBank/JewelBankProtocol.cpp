// JewelBankProtocol.cpp: parsing of the jewel bank state packet and the
// matching client requests.
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "JewelBankProtocol.h"
#include "GameLogic/JewelBank/JewelBankState.h"
#include "Network/Server/WSclient.h"
#include "Dotnet/Connection.h"

namespace Network::JewelBank
{
    namespace
    {
        constexpr int C2_PAYLOAD_OFFSET = 5;    // C2, size(2), 0xD2, subcode

        // The send functions are custom exports of MUnique.Client.Library
        // (ConnectionManager.ClientToServer.Custom.cs). They aren't part of the
        // generated packet bindings, so they're resolved from the managed library.
        using SendSimpleFn = void(CORECLR_DELEGATE_CALLTYPE*)(int32_t);
        using SendByteFn = void(CORECLR_DELEGATE_CALLTYPE*)(int32_t, uint8_t);
        using SendTwoBytesFn = void(CORECLR_DELEGATE_CALLTYPE*)(int32_t, uint8_t, uint8_t);

        int GetConnectionHandle()
        {
            return SocketClient != nullptr ? SocketClient->GetHandle() : -1;
        }

#pragma pack(push, 1)
        struct EntryData
        {
            BYTE JewelType;
            BYTE ItemGroup;
            WORD ItemNumber;
            INT Amount;
        };
#pragma pack(pop)
    }

    void ReceiveState(const BYTE* ReceiveBuffer)
    {
        const int iCount = ReceiveBuffer[C2_PAYLOAD_OFFSET];
        const auto* pEntries = reinterpret_cast<const EntryData*>(ReceiveBuffer + C2_PAYLOAD_OFFSET + 1);

        GameLogic::JewelBank::State& state = GameLogic::JewelBank::GetState();
        state.Entries.clear();
        state.Entries.reserve(iCount);

        for (int i = 0; i < iCount; ++i)
        {
            const EntryData& entry = pEntries[i];
            GameLogic::JewelBank::Entry row{};
            row.iJewelType = entry.JewelType;
            row.iItemType = (entry.ItemGroup * MAX_ITEM_INDEX) + entry.ItemNumber;
            row.iAmount = entry.Amount;
            state.Entries.push_back(row);
        }

        state.dwReceivedTick = GetTickCount();
        state.bReceived = true;
    }

    void SendStatusRequest()
    {
        const int iHandle = GetConnectionHandle();
        if (iHandle < 0)
        {
            return;
        }

        static SendSimpleFn s_pfn = LoadManagedSymbol<SendSimpleFn>("SendJewelBankStatusRequest");
        if (s_pfn != nullptr)
        {
            s_pfn(iHandle);
        }
    }

    void SendDepositRequest(int iInventorySlot)
    {
        const int iHandle = GetConnectionHandle();
        if (iHandle < 0)
        {
            return;
        }

        static SendByteFn s_pfn = LoadManagedSymbol<SendByteFn>("SendJewelBankDepositRequest");
        if (s_pfn != nullptr)
        {
            s_pfn(iHandle, static_cast<uint8_t>(iInventorySlot));
        }
    }

    void SendWithdrawRequest(int iJewelType, int iAmount)
    {
        const int iHandle = GetConnectionHandle();
        if (iHandle < 0)
        {
            return;
        }

        static SendTwoBytesFn s_pfn = LoadManagedSymbol<SendTwoBytesFn>("SendJewelBankWithdrawRequest");
        if (s_pfn != nullptr)
        {
            s_pfn(iHandle, static_cast<uint8_t>(iJewelType), static_cast<uint8_t>(iAmount));
        }
    }
}
