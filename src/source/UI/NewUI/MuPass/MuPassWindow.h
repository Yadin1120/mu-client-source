// MuPassWindow.h: the MU Pass window, opened with the T key.
// Shows the goblin point meter, daily and pro missions, the vertical
// 30-level reward track and the collect / buy-pro buttons.
//////////////////////////////////////////////////////////////////////

#pragma once

#include "UI/NewUI/NewUIBase.h"
#include "UI/NewUI/NewUIManager.h"
#include "GameLogic/MuPass/MuPassState.h"

namespace SEASON3B
{
    class CMuPassWindow : public CNewUIObj
    {
    public:
        enum LAYOUT
        {
            // The window must end well above y=440 in the 640x480 UI space, otherwise
            // the game's bottom frame (skill bar) draws over the button row.
            WND_WIDTH = 560,
            WND_HEIGHT = 406,
            WND_POS_X = 40,
            WND_POS_Y = 14,

            HEADER_HEIGHT = 34,
            METER_TOP = 40,
            METER_HEIGHT = 56,
            BODY_TOP = 100,
            BODY_BOTTOM = 352,
            BOTTOM_BAR_TOP = 358,

            TRACK_COLUMN_X = 8,
            TRACK_COLUMN_WIDTH = 130,
            TRACK_ROW_HEIGHT = 44,
            TRACK_VISIBLE_ROWS = 5,
            TRACK_SLOT_SIZE = 38,

            MISSIONS_COLUMN_X = 146,
            MISSIONS_COLUMN_WIDTH = 406,
            MISSION_ROW_HEIGHT = 24,

            BUTTON_WIDTH = 264,
            BUTTON_HEIGHT = 34,
        };

    public:
        CMuPassWindow();
        virtual ~CMuPassWindow();

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
        // Input.
        bool HandleHeaderClicks();
        bool HandleTrackScroll();
        bool HandleMissionScroll();
        bool HandleBottomButtons();

        // Rendering, split per area.
        void RenderFrame();
        void RenderHeader();
        void RenderPointsMeter();
        void RenderMissions();
        void RenderMissionRow(const GameLogic::MuPass::Mission& mission, int iRowY);
        void RenderRewardTrack();
        void RenderTrackSlot(const GameLogic::MuPass::TrackReward& reward, int iSlotX, int iSlotY, bool bProSlot);
        void RenderBottomButtons();
        void RenderInfoOverlay();

        // Item icons are 3D models that need the camera projection set up, so they
        // can't be drawn inline with the 2D fills. RenderTrackSlot queues them here
        // and RenderPendingItems draws them all in one 3D pass (like the item shop).
        void QueueItemIcon(int iSlotX, int iSlotY, int iItemType, int iItemLevel);
        void RenderPendingItems();

        // Helpers.
        int GetMaxTrackScroll() const;
        int GetMaxMissionScroll() const;
        void ScrollTrackToCurrentLevel();
        bool IsProConfirmPending() const;
        float GetPulseAlpha() const;

    private:
        CNewUIManager* m_pNewUIMng;
        POINT m_Pos;

        bool m_bWasVisible;             // for open-transition detection in Update()
        bool m_bInfoOverlayVisible;
        int m_iTrackScroll;             // first visible reward-track row
        int m_iMissionScroll;           // first visible mission row - missions accumulate past what fits
        DWORD m_dwProConfirmTick;       // GetTickCount() of the first buy-pro click, 0 when idle

        struct PendingItemIcon
        {
            int iX;
            int iY;
            int iSize;
            int iType;
            int iLevel;
        };

        PendingItemIcon m_pendingItems[TRACK_VISIBLE_ROWS * 2];
        int m_pendingItemCount;

        // The reward the mouse is over this frame, if any. Collected while the track is
        // drawn and rendered as a normal item tooltip at the very end of Render(), so it
        // draws above the 3D reward icons.
        void RenderRewardToolTip();

        bool m_bHoveredReward;
        bool m_bHoveredHasItem;         // false for credits / random excellent - those get a text box
        int m_iHoveredTipX;
        int m_iHoveredTipY;
        unsigned char m_HoveredItemData[GameLogic::MuPass::REWARD_ITEM_DATA_BYTES];
        wchar_t m_szHoveredLabel[GameLogic::MuPass::REWARD_LABEL_LEN];
    };
}

extern SEASON3B::CMuPassWindow* g_pMuPassWindow;
