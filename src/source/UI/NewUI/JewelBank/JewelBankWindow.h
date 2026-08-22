// JewelBankWindow.h: the jewel bank window, opened with the J key.
// One row per jewel type: icon, name, stored amount, and the withdrawal
// buttons (1 / 10 / 20 / 30 / fill). Depositing is Ctrl + right-click in
// the inventory, handled by CNewUIMyInventory.
//////////////////////////////////////////////////////////////////////

#pragma once

#include "UI/NewUI/NewUIBase.h"
#include "UI/NewUI/NewUIManager.h"
#include "GameLogic/JewelBank/JewelBankState.h"

namespace SEASON3B
{
    class CJewelBankWindow : public CNewUIObj
    {
    public:
        enum LAYOUT
        {
            // Sized for the 640x480 UI space. Wider than the 270 other servers use,
            // because the item name has to fit whole in both languages - theirs shows
            // "Jewel of Ble". Every row is one line: icon, name, amount, buttons.
            WND_WIDTH = 330,
            WND_HEIGHT = 300,
            // Left of the inventory panel (which starts at 450), so both are
            // readable side by side - depositing needs them both on screen.
            WND_POS_X = 112,
            WND_POS_Y = 40,

            HEADER_HEIGHT = 26,
            LIST_TOP = 30,
            ROW_HEIGHT = 24,
            MAX_VISIBLE_ROWS = 10,
            FOOTER_HEIGHT = 28,

            ICON_X = 8,
            ICON_SIZE = 20,
            NAME_X = 34,
            AMOUNT_WIDTH = 46,

            BUTTON_HEIGHT = 16,
            BUTTON_WIDTH = 22,
            BUTTON_WIDE = 30,       // the fill button carries a word, not a number
            BUTTON_GAP = 2,
        };

    public:
        CJewelBankWindow();
        virtual ~CJewelBankWindow();

        bool Create(CNewUIManager* pNewUIMng, int x, int y);
        void Release();

        void SetPos(int x, int y);

        bool UpdateMouseEvent() override;
        bool UpdateKeyEvent() override;
        bool Update() override;
        bool Render() override;

        float GetLayerDepth() override;    // 5.2f - above the standard side panels

        void OpeningProcess();
        void ClosingProcess();

    private:
        void RenderFrame();
        void RenderHeader();
        void RenderRows();
        void RenderRow(const GameLogic::JewelBank::Entry& entry, int iRowY, bool bOddRow);
        void RenderFooter();

        // Item icons are 3D models and need the camera projection, so they can't be
        // drawn inline with the 2D fills: the rows queue them and one 3D pass draws
        // them all, exactly like the MU Pass window does.
        void QueueItemIcon(int iX, int iY, int iItemType);
        void RenderPendingItems();

        int GetButtonX(int iButtonIndex) const;
        int GetAmountX() const;
        int GetWithdrawAmount(int iButtonIndex) const;
        int GetFreeInventorySlotCount() const;

    private:
        CNewUIManager* m_pNewUIMng;
        POINT m_Pos;

        bool m_bWasVisible;             // for open-transition detection in Update()

        struct PendingItemIcon
        {
            int iX;
            int iY;
            int iType;
        };

        PendingItemIcon m_pendingItems[MAX_VISIBLE_ROWS];
        int m_pendingItemCount;
    };
}

extern SEASON3B::CJewelBankWindow* g_pJewelBankWindow;
