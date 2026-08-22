// JewelBankWindow.cpp: implementation of the jewel bank window (J key).
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "JewelBankWindow.h"
#include "UI/NewUI/NewUISystem.h"
#include "UI/NewUI/NewUICommon.h"
#include "UI/NewUI/Inventory/NewUIMyInventory.h"
#include "UI/NewUI/Inventory/NewUIInventoryCtrl.h"
#include "UI/Legacy/UIControls.h"
#include "Render/Textures/ZzzOpenglUtil.h"
#include "Engine/Object/ZzzInventory.h"
#include "Network/JewelBank/JewelBankProtocol.h"
#include "Audio/DSPlaySound.h"
#include "App/Platform/Windows/Winmain.h"
#include "Camera/CameraProjection.h"
#include "Camera/CameraState.h"
#include "Core/Localization/TextDirection.h"
#include "I18N/All.h"

SEASON3B::CJewelBankWindow* g_pJewelBankWindow = nullptr;

namespace
{
    // The five buttons of every row. 255 asks the server to fill the inventory.
    constexpr int WITHDRAW_AMOUNTS[5] = { 1, 10, 20, 30, Network::JewelBank::WITHDRAW_FILL };
    constexpr int BUTTON_COUNT = 5;

    // The height one line of the standard font occupies, used to centre text in a row.
    constexpr int TEXT_LINE_HEIGHT = 12;

    // Same palette as the MU Pass window, so the two custom windows read as one game.
    constexpr float COL_BG[3] = { 20.f / 255.f, 16.f / 255.f, 12.f / 255.f };
    constexpr float COL_PANEL[3] = { 36.f / 255.f, 28.f / 255.f, 18.f / 255.f };
    constexpr float COL_ROW[3] = { 30.f / 255.f, 24.f / 255.f, 16.f / 255.f };
    constexpr float COL_GOLD[3] = { 232.f / 255.f, 182.f / 255.f, 76.f / 255.f };
    constexpr float COL_GOLD_DARK[3] = { 138.f / 255.f, 109.f / 255.f, 59.f / 255.f };

    int SortLeading() noexcept
    {
        return Core::Localization::IsRightToLeft() ? RT3_SORT_RIGHT : RT3_SORT_LEFT;
    }

    void FillRect(int x, int y, int w, int h, const float col[3], float alpha = 1.f)
    {
        glColor4f(col[0], col[1], col[2], alpha);
        RenderColor(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));

        // RenderColor leaves texturing disabled; EnableAlphaTest turns it back on and
        // keeps the engine's texture flag in sync, so the next text draw renders glyphs
        // instead of solid blocks. Same note as in the MU Pass window.
        EnableAlphaTest();

        // 🔴 And it also leaves its own colour current. The font bitmap is drawn
        // modulated by that colour, so text right after a fill came out brown-on-brown
        // and looked like it had not been drawn at all - only the rows that got no
        // background band showed their name. Restore white; SetTextColor decides the
        // glyph colour by itself.
        glColor4f(1.f, 1.f, 1.f, 1.f);
    }

    void OutlineRect(int x, int y, int w, int h, const float col[3], float alpha = 1.f)
    {
        FillRect(x, y, w, 1, col, alpha);
        FillRect(x, y + h - 1, w, 1, col, alpha);
        FillRect(x, y, 1, h, col, alpha);
        FillRect(x + w - 1, y, 1, h, col, alpha);
    }

    void SetTextGold() { g_pRenderText->SetTextColor(232, 182, 76, 255); }
    void SetTextIvory() { g_pRenderText->SetTextColor(240, 223, 174, 255); }
    void SetTextMuted() { g_pRenderText->SetTextColor(178, 164, 129, 255); }
    void SetTextDim() { g_pRenderText->SetTextColor(120, 110, 92, 255); }

    bool WasClicked(int x, int y, int w, int h)
    {
        if (MouseLButtonPush && SEASON3B::CheckMouseIn(x, y, w, h))
        {
            MouseLButtonPush = false;
            return true;
        }

        return false;
    }
}

SEASON3B::CJewelBankWindow::CJewelBankWindow()
    : m_pNewUIMng(nullptr)
    , m_Pos{ 0, 0 }
    , m_bWasVisible(false)
    , m_pendingItems{}
    , m_pendingItemCount(0)
{
}

SEASON3B::CJewelBankWindow::~CJewelBankWindow()
{
    Release();
}

bool SEASON3B::CJewelBankWindow::Create(CNewUIManager* pNewUIMng, int x, int y)
{
    if (pNewUIMng == nullptr)
    {
        return false;
    }

    m_pNewUIMng = pNewUIMng;
    m_pNewUIMng->AddUIObj(SEASON3B::INTERFACE_JEWELBANK, this);
    SetPos(x, y);
    Show(false);
    return true;
}

void SEASON3B::CJewelBankWindow::Release()
{
    if (m_pNewUIMng != nullptr)
    {
        m_pNewUIMng->RemoveUIObj(this);
        m_pNewUIMng = nullptr;
    }
}

void SEASON3B::CJewelBankWindow::SetPos(int x, int y)
{
    m_Pos.x = x;
    m_Pos.y = y;
}

float SEASON3B::CJewelBankWindow::GetLayerDepth()
{
    return 5.2f;
}

void SEASON3B::CJewelBankWindow::OpeningProcess()
{
    // The balances are never guessed client side: every open asks the server.
    Network::JewelBank::SendStatusRequest();
}

void SEASON3B::CJewelBankWindow::ClosingProcess()
{
}

bool SEASON3B::CJewelBankWindow::Update()
{
    const bool bVisible = IsVisible();
    if (bVisible && !m_bWasVisible)
    {
        OpeningProcess();
    }
    else if (!bVisible && m_bWasVisible)
    {
        ClosingProcess();
    }

    m_bWasVisible = bVisible;
    return true;
}

bool SEASON3B::CJewelBankWindow::UpdateKeyEvent()
{
    if (!IsVisible())
    {
        return true;
    }

    if (SEASON3B::IsPress(VK_ESCAPE))
    {
        g_pNewUISystem->Hide(SEASON3B::INTERFACE_JEWELBANK);
        PlayBuffer(SOUND_CLICK01);
        return false;
    }

    return true;
}

bool SEASON3B::CJewelBankWindow::UpdateMouseEvent()
{
    if (!IsVisible())
    {
        return true;
    }

    // Close button.
    if (WasClicked(m_Pos.x + WND_WIDTH - 28, m_Pos.y + 4, 20, 18))
    {
        g_pNewUISystem->Hide(SEASON3B::INTERFACE_JEWELBANK);
        PlayBuffer(SOUND_CLICK01);
        return false;
    }

    const GameLogic::JewelBank::State& state = GameLogic::JewelBank::GetState();
    const int iRowCount = static_cast<int>(state.Entries.size());
    const int iVisibleRows = (iRowCount < MAX_VISIBLE_ROWS) ? iRowCount : static_cast<int>(MAX_VISIBLE_ROWS);

    for (int iRow = 0; iRow < iVisibleRows; ++iRow)
    {
        const GameLogic::JewelBank::Entry& entry = state.Entries[iRow];
        const int iRowY = m_Pos.y + LIST_TOP + (iRow * ROW_HEIGHT);
        const int iButtonY = iRowY + ((ROW_HEIGHT - BUTTON_HEIGHT) / 2);

        for (int iButton = 0; iButton < BUTTON_COUNT; ++iButton)
        {
            const int iWidth = (iButton == BUTTON_COUNT - 1) ? BUTTON_WIDE : BUTTON_WIDTH;
            if (!WasClicked(GetButtonX(iButton), iButtonY, iWidth, BUTTON_HEIGHT))
            {
                continue;
            }

            // An empty row has nothing to give; the server would refuse anyway, but a
            // silent click feels broken, so it just doesn't react.
            if (entry.iAmount <= 0)
            {
                return false;
            }

            Network::JewelBank::SendWithdrawRequest(entry.iJewelType, GetWithdrawAmount(iButton));
            PlayBuffer(SOUND_CLICK01);
            return false;
        }
    }

    // Swallow every event inside the window so the world doesn't react to it.
    if (CheckMouseIn(m_Pos.x, m_Pos.y, WND_WIDTH, WND_HEIGHT))
    {
        return false;
    }

    return true;
}

bool SEASON3B::CJewelBankWindow::Render()
{
    if (!IsVisible())
    {
        return true;
    }

    EnableAlphaTest();

    m_pendingItemCount = 0;
    RenderFrame();
    RenderHeader();
    RenderRows();
    RenderFooter();

    // The queued jewel icons, in one 3D pass.
    RenderPendingItems();
    EnableAlphaTest();

    return true;
}

void SEASON3B::CJewelBankWindow::RenderFrame()
{
    FillRect(m_Pos.x, m_Pos.y, WND_WIDTH, WND_HEIGHT, COL_BG, 1.0f);
    OutlineRect(m_Pos.x - 2, m_Pos.y - 2, WND_WIDTH + 4, WND_HEIGHT + 4, COL_GOLD_DARK);
    OutlineRect(m_Pos.x, m_Pos.y, WND_WIDTH, WND_HEIGHT, COL_GOLD_DARK, 0.6f);
}

void SEASON3B::CJewelBankWindow::RenderHeader()
{
    FillRect(m_Pos.x, m_Pos.y, WND_WIDTH, HEADER_HEIGHT, COL_PANEL);
    FillRect(m_Pos.x, m_Pos.y + HEADER_HEIGHT - 2, WND_WIDTH, 2, COL_GOLD_DARK);

    g_pRenderText->SetBgColor(0, 0, 0, 0);
    g_pRenderText->SetFont(g_hFontBold);
    SetTextGold();
    g_pRenderText->RenderText(m_Pos.x + 10, m_Pos.y + 6, I18N::Game::JewelBank, WND_WIDTH - 46, 0, SortLeading());

    OutlineRect(m_Pos.x + WND_WIDTH - 28, m_Pos.y + 4, 20, 18, COL_GOLD_DARK);
    g_pRenderText->SetFont(g_hFont);
    SetTextGold();
    g_pRenderText->RenderText(m_Pos.x + WND_WIDTH - 28, m_Pos.y + 7, L"X", 20, 0, RT3_SORT_CENTER);
}

void SEASON3B::CJewelBankWindow::RenderRows()
{
    const GameLogic::JewelBank::State& state = GameLogic::JewelBank::GetState();

    if (state.Entries.empty())
    {
        g_pRenderText->SetFont(g_hFont);
        SetTextMuted();
        g_pRenderText->RenderText(m_Pos.x + 10, m_Pos.y + LIST_TOP + 10, I18N::Game::Loading, WND_WIDTH - 20, 0, RT3_SORT_CENTER);
        return;
    }

    const int iRowCount = static_cast<int>(state.Entries.size());
    const int iVisibleRows = (iRowCount < MAX_VISIBLE_ROWS) ? iRowCount : static_cast<int>(MAX_VISIBLE_ROWS);

    for (int iRow = 0; iRow < iVisibleRows; ++iRow)
    {
        RenderRow(state.Entries[iRow], m_Pos.y + LIST_TOP + (iRow * ROW_HEIGHT), (iRow % 2) == 1);
    }
}

void SEASON3B::CJewelBankWindow::RenderRow(const GameLogic::JewelBank::Entry& entry, int iRowY, bool bOddRow)
{
    const bool bEmpty = entry.iAmount <= 0;

    // Alternating bands, so ten rows of small text stay countable at a glance.
    if (bOddRow)
    {
        FillRect(m_Pos.x + 2, iRowY, WND_WIDTH - 4, ROW_HEIGHT - 1, COL_ROW, 0.7f);
    }

    QueueItemIcon(m_Pos.x + ICON_X, iRowY + 2, entry.iItemType);

    // ⚠️ RT3_SORT_LEFT / RT3_SORT_CENTER on purpose, and not the leading-edge helper
    // the header uses: the first build drew nothing at all in the rows, and these are
    // the two flags the button labels in this very window already prove to work.
    wchar_t szName[64] = { 0 };
    ::GetItemName(entry.iItemType, 0, szName);
    if (szName[0] == L'\0')
    {
        // A type the client has no name for would otherwise leave a blank row -
        // better to show the number than to look broken.
        swprintf_s(szName, L"#%d", entry.iItemType);
    }

    g_pRenderText->SetBgColor(0, 0, 0, 0);
    g_pRenderText->SetFont(g_hFont);

    const int iTextY = iRowY + ((ROW_HEIGHT - TEXT_LINE_HEIGHT) / 2);
    const int iNameWidth = GetAmountX() - (m_Pos.x + NAME_X) - 4;

    if (bEmpty)
    {
        SetTextMuted();
    }
    else
    {
        SetTextIvory();
    }

    g_pRenderText->RenderText(m_Pos.x + NAME_X, iTextY, szName, iNameWidth, 0, RT3_SORT_LEFT);

    wchar_t szAmount[32];
    swprintf_s(szAmount, L"%d", entry.iAmount);
    if (bEmpty)
    {
        SetTextDim();
    }
    else
    {
        SetTextGold();
    }

    g_pRenderText->RenderText(GetAmountX(), iTextY, szAmount, AMOUNT_WIDTH, 0, RT3_SORT_CENTER);

    const int iButtonY = iRowY + ((ROW_HEIGHT - BUTTON_HEIGHT) / 2);
    for (int iButton = 0; iButton < BUTTON_COUNT; ++iButton)
    {
        const int iX = GetButtonX(iButton);
        const int iWidth = (iButton == BUTTON_COUNT - 1) ? BUTTON_WIDE : BUTTON_WIDTH;
        const bool bHovered = !bEmpty && CheckMouseIn(iX, iButtonY, iWidth, BUTTON_HEIGHT);

        FillRect(iX, iButtonY, iWidth, BUTTON_HEIGHT, COL_PANEL, bEmpty ? 0.3f : 1.0f);
        OutlineRect(iX, iButtonY, iWidth, BUTTON_HEIGHT, COL_GOLD_DARK, bHovered ? 1.0f : 0.55f);

        wchar_t szLabel[16];
        if (iButton == BUTTON_COUNT - 1)
        {
            wcscpy_s(szLabel, I18N::Game::Fill);
        }
        else
        {
            swprintf_s(szLabel, L"%d", WITHDRAW_AMOUNTS[iButton]);
        }

        if (bEmpty)
        {
            SetTextDim();
        }
        else if (bHovered)
        {
            SetTextGold();
        }
        else
        {
            SetTextMuted();
        }

        g_pRenderText->RenderText(iX, iButtonY + 2, szLabel, iWidth, 0, RT3_SORT_CENTER);
    }
}

void SEASON3B::CJewelBankWindow::RenderFooter()
{
    const int iFooterY = m_Pos.y + WND_HEIGHT - FOOTER_HEIGHT;
    FillRect(m_Pos.x, iFooterY, WND_WIDTH, FOOTER_HEIGHT, COL_PANEL);
    FillRect(m_Pos.x, iFooterY, WND_WIDTH, 2, COL_GOLD_DARK);

    g_pRenderText->SetFont(g_hFont);
    SetTextMuted();
    g_pRenderText->RenderText(m_Pos.x + 8, iFooterY + 5, I18N::Game::CtrlRightClickToDeposit, WND_WIDTH - 16, 0, SortLeading());

    // The free slot count decides whether a withdrawal can succeed at all, so it
    // belongs on screen and not only in the error message.
    wchar_t szSlots[64];
    swprintf_s(szSlots, I18N::Game::DFreeSlots, GetFreeInventorySlotCount());
    SetTextGold();
    g_pRenderText->RenderText(m_Pos.x + 8, iFooterY + 15, szSlots, WND_WIDTH - 16, 0, SortLeading());
}

void SEASON3B::CJewelBankWindow::QueueItemIcon(int iX, int iY, int iItemType)
{
    if (m_pendingItemCount >= MAX_VISIBLE_ROWS)
    {
        return;
    }

    PendingItemIcon& icon = m_pendingItems[m_pendingItemCount++];
    icon.iX = iX;
    icon.iY = iY;
    icon.iType = iItemType;
}

void SEASON3B::CJewelBankWindow::RenderPendingItems()
{
    if (m_pendingItemCount == 0)
    {
        return;
    }

    EndBitmap();

    glMatrixMode(GL_PROJECTION);
    SaveCameraPerspective();
    glPushMatrix();
    glLoadIdentity();
    glViewport2(0, 0, WindowWidth, WindowHeight);
    gluPerspective2(2.0f, static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight), RENDER_ITEMVIEW_NEAR, RENDER_ITEMVIEW_FAR);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    CameraProjection::GetOpenGLMatrix(g_Camera.Matrix);
    EnableDepthTest();
    EnableDepthMask();

    glClear(GL_DEPTH_BUFFER_BIT);

    for (int i = 0; i < m_pendingItemCount; ++i)
    {
        const PendingItemIcon& icon = m_pendingItems[i];
        RenderItem3D(
            static_cast<float>(icon.iX),
            static_cast<float>(icon.iY),
            static_cast<float>(ICON_SIZE),
            static_cast<float>(ICON_SIZE),
            icon.iType,
            0,
            0,
            0,
            true);
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    RestoreCameraPerspective();
    BeginBitmap();
}

int SEASON3B::CJewelBankWindow::GetButtonX(int iButtonIndex) const
{
    // The row of buttons is anchored to the right edge, so the name column keeps
    // whatever is left - long names in either language have somewhere to go.
    const int iTotalWidth = ((BUTTON_COUNT - 1) * (BUTTON_WIDTH + BUTTON_GAP)) + BUTTON_WIDE;
    const int iFirstX = m_Pos.x + WND_WIDTH - 6 - iTotalWidth;
    return iFirstX + (iButtonIndex * (BUTTON_WIDTH + BUTTON_GAP));
}

int SEASON3B::CJewelBankWindow::GetAmountX() const
{
    return GetButtonX(0) - 6 - AMOUNT_WIDTH;
}

int SEASON3B::CJewelBankWindow::GetWithdrawAmount(int iButtonIndex) const
{
    if (iButtonIndex < 0 || iButtonIndex >= BUTTON_COUNT)
    {
        return 1;
    }

    return WITHDRAW_AMOUNTS[iButtonIndex];
}

int SEASON3B::CJewelBankWindow::GetFreeInventorySlotCount() const
{
    if (g_pMyInventory == nullptr)
    {
        return 0;
    }

    SEASON3B::CNewUIInventoryCtrl* pInventoryCtrl = g_pMyInventory->GetInventoryCtrl();
    return (pInventoryCtrl != nullptr) ? pInventoryCtrl->GetEmptySlotCount() : 0;
}
