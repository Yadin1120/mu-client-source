// MuPassWindow.cpp: implementation of the MU Pass window (T key).
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MuPassWindow.h"
#include "UI/NewUI/NewUISystem.h"
#include "UI/NewUI/NewUICommon.h"
#include "UI/Legacy/UIControls.h"
#include "Render/Textures/ZzzOpenglUtil.h"
#include "Engine/Object/ZzzInventory.h"
#include "UI/NewUI/Inventory/NewUIItemMng.h"
#include "Network/MuPass/MuPassProtocol.h"
#include "Audio/DSPlaySound.h"
#include "App/Platform/Windows/Winmain.h"
#include "Camera/CameraProjection.h"
#include "Camera/CameraState.h"
#include "Core/Localization/TextDirection.h"
#include "I18N/All.h"
#include <cmath>

SEASON3B::CMuPassWindow* g_pMuPassWindow = nullptr;

namespace
{
    using GameLogic::MuPass::RewardState;

    constexpr DWORD PRO_CONFIRM_TIMEOUT_MS = 5000;

    // The window was laid out for Hebrew: titles hug the right edge and the
    // progress bar grows leftwards. That is the reading order in Hebrew and the
    // wrong one in English, so alignment follows the locale instead of being
    // baked in. Hebrew keeps exactly the layout it has today.
    //
    // "Leading" is where a line of text starts, "trailing" is the far edge.
    int SortLeading() noexcept
    {
        return Core::Localization::IsRightToLeft() ? RT3_SORT_RIGHT : RT3_SORT_LEFT;
    }

    int SortTrailing() noexcept
    {
        return Core::Localization::IsRightToLeft() ? RT3_SORT_LEFT : RT3_SORT_RIGHT;
    }
    // 8 rows + the pro section header is the most that fits between BODY_TOP and
    // BODY_BOTTOM without spilling into the bottom button bar.
    constexpr int MAX_VISIBLE_MISSIONS = 8;
    constexpr int SECONDS_PER_HOUR = 3600;
    constexpr int SECONDS_PER_MINUTE = 60;

    // Mission row geometry. The progress box sits on the left, the (RTL) mission name
    // fills the rest and stops short of the status marker on the right edge.
    constexpr int PROGRESS_BOX_WIDTH = 100;
    constexpr int MISSION_TEXT_WIDTH =
        SEASON3B::CMuPassWindow::MISSIONS_COLUMN_WIDTH - PROGRESS_BOX_WIDTH - 34;

    // Window palette - the gold-on-dark look of the approved mockup.
    constexpr float COL_BG[3] = { 20.f / 255.f, 16.f / 255.f, 12.f / 255.f };
    constexpr float COL_PANEL[3] = { 36.f / 255.f, 28.f / 255.f, 18.f / 255.f };
    constexpr float COL_GOLD[3] = { 232.f / 255.f, 182.f / 255.f, 76.f / 255.f };
    constexpr float COL_GOLD_DARK[3] = { 138.f / 255.f, 109.f / 255.f, 59.f / 255.f };
    constexpr float COL_BAR_FILL[3] = { 185.f / 255.f, 138.f / 255.f, 46.f / 255.f };
    constexpr float COL_GREEN[3] = { 127.f / 255.f, 197.f / 255.f, 127.f / 255.f };
    constexpr float COL_PURPLE[3] = { 138.f / 255.f, 95.f / 255.f, 176.f / 255.f };
    constexpr float COL_PURPLE_BG[3] = { 29.f / 255.f, 21.f / 255.f, 36.f / 255.f };

    void FillRect(int x, int y, int w, int h, const float col[3], float alpha = 1.f)
    {
        glColor4f(col[0], col[1], col[2], alpha);
        RenderColor(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));

        // 🔴 The fill also leaves its own colour current, and the font bitmap is drawn
        // modulated by it - so every line of text right after a fill came out darkened
        // towards the panel colour. Found while building the jewel bank window, where
        // the same helper made whole rows look empty. The muted grey below was lifted
        // once already to compensate for this; the real fix is here.
        glColor4f(1.f, 1.f, 1.f, 1.f);

        // RenderColor disables texturing (via DisableTexture) and doesn't restore it.
        // EnableAlphaTest turns GL_TEXTURE_2D back on AND keeps the engine's texture-
        // state flag in sync (EndRenderColor would desync it, making the next fill
        // sample the font texture). Text draws after a fill now render glyphs, not
        // solid blocks, and the next fill's DisableTexture still works correctly.
        EnableAlphaTest();
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
    // Secondary text (countdown, tier line, finished missions). The original 143/130/100
    // was barely readable against the dark panel; lifted just enough to read comfortably
    // while still stepping back from the ivory/gold primary text.
    void SetTextMuted() { g_pRenderText->SetTextColor(178, 164, 129, 255); }
    void SetTextGreen() { g_pRenderText->SetTextColor(127, 197, 127, 255); }
    void SetTextPurple() { g_pRenderText->SetTextColor(192, 138, 224, 255); }

    bool WasClicked(int x, int y, int w, int h)
    {
        if (MouseLButtonPush && SEASON3B::CheckMouseIn(x, y, w, h))
        {
            MouseLButtonPush = false;
            return true;
        }

        return false;
    }

    void FormatCountdown(int iTotalSeconds, wchar_t* pszOut, size_t nOutLen)
    {
        const int iHours = iTotalSeconds / SECONDS_PER_HOUR;
        const int iMinutes = (iTotalSeconds % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
        const int iSeconds = iTotalSeconds % SECONDS_PER_MINUTE;
        swprintf_s(pszOut, nOutLen, L"%02d:%02d:%02d", iHours, iMinutes, iSeconds);
    }
}

SEASON3B::CMuPassWindow::CMuPassWindow()
    : m_pNewUIMng(nullptr)
    , m_Pos{ 0, 0 }
    , m_bWasVisible(false)
    , m_bInfoOverlayVisible(false)
    , m_iTrackScroll(0)
    , m_iMissionScroll(0)
    , m_dwProConfirmTick(0)
    , m_pendingItems{}
    , m_pendingItemCount(0)
    , m_bHoveredReward(false)
    , m_bHoveredHasItem(false)
    , m_iHoveredTipX(0)
    , m_iHoveredTipY(0)
    , m_HoveredItemData{}
    , m_szHoveredLabel{}
{
}

SEASON3B::CMuPassWindow::~CMuPassWindow()
{
    Release();
}

bool SEASON3B::CMuPassWindow::Create(CNewUIManager* pNewUIMng, int x, int y)
{
    if (pNewUIMng == nullptr)
    {
        return false;
    }

    m_pNewUIMng = pNewUIMng;
    m_pNewUIMng->AddUIObj(SEASON3B::INTERFACE_MUPASS, this);
    SetPos(x, y);
    Show(false);
    return true;
}

void SEASON3B::CMuPassWindow::Release()
{
    if (m_pNewUIMng != nullptr)
    {
        m_pNewUIMng->RemoveUIObj(this);
        m_pNewUIMng = nullptr;
    }
}

void SEASON3B::CMuPassWindow::SetPos(int x, int y)
{
    m_Pos.x = x;
    m_Pos.y = y;
}

float SEASON3B::CMuPassWindow::GetLayerDepth()
{
    return 5.2f;
}

void SEASON3B::CMuPassWindow::OpeningProcess()
{
    m_bInfoOverlayVisible = false;
    m_dwProConfirmTick = 0;
    m_iMissionScroll = 0;
    Network::MuPass::SendStatusRequest();
    ScrollTrackToCurrentLevel();
}

void SEASON3B::CMuPassWindow::ClosingProcess()
{
    m_bInfoOverlayVisible = false;
    m_dwProConfirmTick = 0;
}

bool SEASON3B::CMuPassWindow::Update()
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

    if (IsProConfirmPending() && GetTickCount() - m_dwProConfirmTick > PRO_CONFIRM_TIMEOUT_MS)
    {
        m_dwProConfirmTick = 0;
    }

    return true;
}

bool SEASON3B::CMuPassWindow::UpdateKeyEvent()
{
    if (!IsVisible())
    {
        return true;
    }

    if (SEASON3B::IsPress(VK_ESCAPE))
    {
        g_pNewUISystem->Hide(SEASON3B::INTERFACE_MUPASS);
        PlayBuffer(SOUND_CLICK01);
        return false;
    }

    return true;
}

bool SEASON3B::CMuPassWindow::UpdateMouseEvent()
{
    if (!IsVisible())
    {
        return true;
    }

    if (HandleHeaderClicks() || HandleBottomButtons() || HandleTrackScroll() || HandleMissionScroll())
    {
        return false;
    }

    // Swallow every event inside the window so the world doesn't react.
    if (CheckMouseIn(m_Pos.x, m_Pos.y, WND_WIDTH, WND_HEIGHT))
    {
        return false;
    }

    return true;
}

bool SEASON3B::CMuPassWindow::HandleHeaderClicks()
{
    // Close "X" - top-left corner.
    if (WasClicked(m_Pos.x + 8, m_Pos.y + 7, 20, 20))
    {
        g_pNewUISystem->Hide(SEASON3B::INTERFACE_MUPASS);
        PlayBuffer(SOUND_CLICK01);
        return true;
    }

    // "What is MU Pass?" toggle - next to the close button.
    if (WasClicked(m_Pos.x + 34, m_Pos.y + 7, 120, 20))
    {
        m_bInfoOverlayVisible = !m_bInfoOverlayVisible;
        PlayBuffer(SOUND_CLICK01);
        return true;
    }

    return false;
}

bool SEASON3B::CMuPassWindow::HandleTrackScroll()
{
    if (MouseWheel != 0
        && CheckMouseIn(m_Pos.x + TRACK_COLUMN_X, m_Pos.y + BODY_TOP, TRACK_COLUMN_WIDTH, BODY_BOTTOM - BODY_TOP))
    {
        m_iTrackScroll -= MouseWheel;
        MouseWheel = 0;

        const int iMaxScroll = GetMaxTrackScroll();
        if (m_iTrackScroll < 0)
        {
            m_iTrackScroll = 0;
        }
        else if (m_iTrackScroll > iMaxScroll)
        {
            m_iTrackScroll = iMaxScroll;
        }

        return true;
    }

    return false;
}

bool SEASON3B::CMuPassWindow::HandleMissionScroll()
{
    // Uncompleted missions carry over to the next day, so the list regularly holds more
    // rows than fit. Without scrolling the extra ones - including the pro missions -
    // would simply be invisible.
    if (MouseWheel != 0
        && CheckMouseIn(m_Pos.x + MISSIONS_COLUMN_X, m_Pos.y + BODY_TOP, MISSIONS_COLUMN_WIDTH, BODY_BOTTOM - BODY_TOP))
    {
        m_iMissionScroll -= MouseWheel;
        MouseWheel = 0;

        const int iMaxScroll = GetMaxMissionScroll();
        if (m_iMissionScroll < 0)
        {
            m_iMissionScroll = 0;
        }
        else if (m_iMissionScroll > iMaxScroll)
        {
            m_iMissionScroll = iMaxScroll;
        }

        return true;
    }

    return false;
}

bool SEASON3B::CMuPassWindow::HandleBottomButtons()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
    const int iButtonY = m_Pos.y + BOTTOM_BAR_TOP + 6;

    // Collect button (right half - the RTL-primary action).
    bool bReadyIsPro = false;
    const int iReadyLevel = GameLogic::MuPass::GetFirstReadyLevel(&bReadyIsPro);
    if (iReadyLevel > 0
        && WasClicked(m_Pos.x + WND_WIDTH - 12 - BUTTON_WIDTH, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT))
    {
        Network::MuPass::SendCollectRequest(iReadyLevel, bReadyIsPro);
        PlayBuffer(SOUND_CLICK01);
        return true;
    }

    // Buy-pro button (left half), with a local double-confirmation step.
    if (!state.bIsPro
        && WasClicked(m_Pos.x + 12, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT))
    {
        PlayBuffer(SOUND_CLICK01);
        if (IsProConfirmPending())
        {
            m_dwProConfirmTick = 0;
            Network::MuPass::SendProUpgradeRequest();
        }
        else
        {
            m_dwProConfirmTick = GetTickCount();
        }

        return true;
    }

    return false;
}

int SEASON3B::CMuPassWindow::GetMaxTrackScroll() const
{
    const int iTotalRows = static_cast<int>(GameLogic::MuPass::GetState().Track.size());
    const int iMaxScroll = iTotalRows - TRACK_VISIBLE_ROWS;
    return iMaxScroll > 0 ? iMaxScroll : 0;
}

int SEASON3B::CMuPassWindow::GetMaxMissionScroll() const
{
    const int iTotal = static_cast<int>(GameLogic::MuPass::GetState().Missions.size());
    const int iMaxScroll = iTotal - MAX_VISIBLE_MISSIONS;
    return iMaxScroll > 0 ? iMaxScroll : 0;
}

void SEASON3B::CMuPassWindow::ScrollTrackToCurrentLevel()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();

    // Center the current level in the visible track rows.
    m_iTrackScroll = state.iLevel - (TRACK_VISIBLE_ROWS / 2);
    const int iMaxScroll = GetMaxTrackScroll();
    if (m_iTrackScroll < 0)
    {
        m_iTrackScroll = 0;
    }
    else if (m_iTrackScroll > iMaxScroll)
    {
        m_iTrackScroll = iMaxScroll;
    }
}

bool SEASON3B::CMuPassWindow::IsProConfirmPending() const
{
    return m_dwProConfirmTick != 0;
}

float SEASON3B::CMuPassWindow::GetPulseAlpha() const
{
    // Slow gold pulse used to highlight a ready reward (the "collect me" animation).
    const float fPhase = static_cast<float>(GetTickCount() % 1200) / 1200.f;
    return 0.65f + 0.35f * std::sin(fPhase * 2.f * 3.14159265f);
}

bool SEASON3B::CMuPassWindow::Render()
{
    if (!IsVisible())
    {
        return true;
    }

    // Normal alpha blending (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) so the dark panel
    // actually covers the world behind it. EnableAlphaBlend() is additive (GL_ONE,
    // GL_ONE) - a dark fill would only brighten the background, leaving it see-through.
    EnableAlphaTest();

    m_pendingItemCount = 0;
    m_bHoveredReward = false;
    RenderFrame();
    RenderHeader();
    RenderPointsMeter();
    RenderRewardTrack();
    RenderMissions();
    RenderBottomButtons();

    // Draw the queued reward item icons in one 3D pass (needs the camera projection).
    RenderPendingItems();

    // Re-establish 2D blending for any overlay drawn on top of the items.
    EnableAlphaTest();

    // After the 3D icon pass, so the tooltip box draws above the reward icons.
    RenderRewardToolTip();

    if (m_bInfoOverlayVisible)
    {
        RenderInfoOverlay();
    }

    EndRenderColor();
    DisableAlphaBlend();
    glColor4f(1.f, 1.f, 1.f, 1.f);
    return true;
}

void SEASON3B::CMuPassWindow::RenderFrame()
{
    FillRect(m_Pos.x, m_Pos.y, WND_WIDTH, WND_HEIGHT, COL_BG, 1.0f);
    OutlineRect(m_Pos.x - 2, m_Pos.y - 2, WND_WIDTH + 4, WND_HEIGHT + 4, COL_GOLD_DARK);
    OutlineRect(m_Pos.x, m_Pos.y, WND_WIDTH, WND_HEIGHT, COL_GOLD_DARK, 0.6f);
}

void SEASON3B::CMuPassWindow::RenderHeader()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();

    FillRect(m_Pos.x, m_Pos.y, WND_WIDTH, HEADER_HEIGHT, COL_PANEL);
    FillRect(m_Pos.x, m_Pos.y + HEADER_HEIGHT - 2, WND_WIDTH, 2, COL_GOLD_DARK);

    g_pRenderText->SetBgColor(0, 0, 0, 0);
    g_pRenderText->SetFont(g_hFontBold);
    SetTextGold();
    // No season name here on purpose: the season can be renamed in the admin panel and the
    // status packet doesn't carry the name, so a hardcoded one would eventually be a lie.
    g_pRenderText->RenderText(m_Pos.x + 12, m_Pos.y + 6, L"MU Pass", WND_WIDTH - 24, 0, SortLeading());

    g_pRenderText->SetFont(g_hFont);
    SetTextMuted();
    wchar_t szText[128];
    swprintf_s(szText, I18N::Game::EndsInDDays, state.iSeasonDaysLeft);
    g_pRenderText->RenderText(m_Pos.x + 12, m_Pos.y + 20, szText, WND_WIDTH - 24, 0, SortLeading());

    // Close button.
    OutlineRect(m_Pos.x + 8, m_Pos.y + 7, 20, 20, COL_GOLD_DARK);
    SetTextGold();
    g_pRenderText->RenderText(m_Pos.x + 8, m_Pos.y + 11, L"X", 20, 0, RT3_SORT_CENTER);

    // Info toggle.
    OutlineRect(m_Pos.x + 34, m_Pos.y + 7, 120, 20, COL_GOLD_DARK);
    g_pRenderText->RenderText(m_Pos.x + 36, m_Pos.y + 11, I18N::Game::WhatIsMUPass, 116, 0, RT3_SORT_CENTER);
}

void SEASON3B::CMuPassWindow::RenderPointsMeter()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
    const int iBaseY = m_Pos.y + METER_TOP;
    wchar_t szText[128];

    g_pRenderText->SetFont(g_hFontBold);
    SetTextGold();
    swprintf_s(szText, I18N::Game::LevelDOfD, state.iLevel, GameLogic::MuPass::MAX_PASS_LEVEL);
    g_pRenderText->RenderText(m_Pos.x + 12, iBaseY, szText, WND_WIDTH - 24, 0, SortLeading());

    g_pRenderText->SetFont(g_hFont);
    SetTextIvory();
    if (state.iPointsForNextLevel > 0)
    {
        swprintf_s(szText, I18N::Game::GoblinPointsDD, state.iPointsIntoLevel, state.iPointsForNextLevel);
    }
    else
    {
        swprintf_s(szText, I18N::Game::GoblinPointsDMaxLevel, state.iTotalPoints);
    }

    g_pRenderText->RenderText(m_Pos.x + 12, iBaseY + 2, szText, WND_WIDTH - 24, 0, SortTrailing());

    // Progress bar.
    const int iBarY = iBaseY + 20;
    const int iBarWidth = WND_WIDTH - 24;
    FillRect(m_Pos.x + 12, iBarY, iBarWidth, 12, COL_BG);
    OutlineRect(m_Pos.x + 12, iBarY, iBarWidth, 12, COL_GOLD_DARK);
    if (state.iPointsForNextLevel > 0)
    {
        const float fRatio = static_cast<float>(state.iPointsIntoLevel) / static_cast<float>(state.iPointsForNextLevel);
        const int iFillWidth = static_cast<int>((iBarWidth - 2) * (fRatio > 1.f ? 1.f : fRatio));
        // The bar grows from the reading edge: right-to-left in Hebrew,
        // left-to-right everywhere else.
        const int iFillX = Core::Localization::IsRightToLeft()
            ? m_Pos.x + 12 + iBarWidth - 1 - iFillWidth
            : m_Pos.x + 13;
        FillRect(iFillX, iBarY + 1, iFillWidth, 10, COL_BAR_FILL);
    }
    else
    {
        FillRect(m_Pos.x + 13, iBarY + 1, iBarWidth - 2, 10, COL_BAR_FILL);
    }

    // Tier line.
    SetTextMuted();
    g_pRenderText->RenderText(m_Pos.x + 12, iBarY + 16, I18N::Game::Levels1101000PtsLevels10201500PtsLevels20302000Pts, WND_WIDTH - 24, 0, RT3_SORT_CENTER);
}

void SEASON3B::CMuPassWindow::RenderMissions()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
    const int iColumnX = m_Pos.x + MISSIONS_COLUMN_X;
    int iRowY = m_Pos.y + BODY_TOP;
    wchar_t szText[160];

    g_pRenderText->SetFont(g_hFontBold);
    SetTextGold();
    swprintf_s(szText, I18N::Game::DailyMissionsDD, state.iCompletedTodayFree, state.iTotalTodayFree);
    g_pRenderText->RenderText(iColumnX, iRowY, szText, MISSIONS_COLUMN_WIDTH, 0, SortLeading());

    g_pRenderText->SetFont(g_hFont);
    SetTextMuted();
    wchar_t szCountdown[32];
    FormatCountdown(GameLogic::MuPass::GetSecondsUntilResetNow(), szCountdown, 32);
    swprintf_s(szText, I18N::Game::NewOnesInLs, szCountdown);
    g_pRenderText->RenderText(iColumnX, iRowY + 2, szText, MISSIONS_COLUMN_WIDTH, 0, SortTrailing());
    iRowY += 18;

    if (state.bDailyBonusGranted)
    {
        SetTextGreen();
        swprintf_s(szText, I18N::Game::BonusDForEveryMissionClaimed, state.iDailyBonusPoints);
    }
    else
    {
        SetTextMuted();
        swprintf_s(szText, I18N::Game::FinishAllDBonusDPts, state.iTotalTodayFree, state.iDailyBonusPoints);
    }

    g_pRenderText->RenderText(iColumnX, iRowY, szText, MISSIONS_COLUMN_WIDTH, 0, SortLeading());

    // Uncompleted missions carry over, so the list is regularly longer than the rows that
    // fit here. Clamp the scroll offset every frame - the list length changes underneath it.
    const int iTotal = static_cast<int>(state.Missions.size());
    const int iMaxScroll = GetMaxMissionScroll();
    if (m_iMissionScroll > iMaxScroll)
    {
        m_iMissionScroll = iMaxScroll;
    }

    if (m_iMissionScroll < 0)
    {
        m_iMissionScroll = 0;
    }

    if (iTotal > MAX_VISIBLE_MISSIONS)
    {
        SetTextMuted();
        swprintf_s(szText, I18N::Game::DDOfDScrollWithTheMouseWheel,
            m_iMissionScroll + 1,
            (m_iMissionScroll + MAX_VISIBLE_MISSIONS < iTotal) ? m_iMissionScroll + MAX_VISIBLE_MISSIONS : iTotal,
            iTotal);
        g_pRenderText->RenderText(iColumnX, iRowY, szText, MISSIONS_COLUMN_WIDTH, 0, SortTrailing());
    }

    iRowY += 16;

    // Free missions first, pro missions afterwards - both already ordered by the server.
    int iShown = 0;
    bool bProHeaderShown = false;
    for (int iIndex = m_iMissionScroll; iIndex < iTotal && iShown < MAX_VISIBLE_MISSIONS; ++iIndex)
    {
        const GameLogic::MuPass::Mission& mission = state.Missions[iIndex];

        if (mission.bIsPro && !bProHeaderShown)
        {
            bProHeaderShown = true;
            g_pRenderText->SetFont(g_hFontBold);
            SetTextPurple();
            const wchar_t* pszProTitle = state.bIsPro ? I18N::Game::ProMissions : I18N::Game::ProMissionsLocked;
            g_pRenderText->RenderText(iColumnX, iRowY + 4, pszProTitle, MISSIONS_COLUMN_WIDTH, 0, SortLeading());
            g_pRenderText->SetFont(g_hFont);
            iRowY += 20;
        }

        RenderMissionRow(mission, iRowY);
        iRowY += MISSION_ROW_HEIGHT;
        ++iShown;
    }

    if (state.Missions.empty())
    {
        SetTextMuted();
        g_pRenderText->RenderText(iColumnX, iRowY + 8, I18N::Game::LoadingMissions, MISSIONS_COLUMN_WIDTH, 0, RT3_SORT_CENTER);
    }
}

void SEASON3B::CMuPassWindow::RenderMissionRow(const GameLogic::MuPass::Mission& mission, int iRowY)
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
    const int iColumnX = m_Pos.x + MISSIONS_COLUMN_X;
    const bool bLockedPro = mission.bIsPro && !state.bIsPro;
    wchar_t szText[160];

    // Row background.
    if (mission.bIsPro)
    {
        FillRect(iColumnX, iRowY, MISSIONS_COLUMN_WIDTH, MISSION_ROW_HEIGHT - 3, COL_PURPLE_BG, bLockedPro ? 0.55f : 1.f);
        OutlineRect(iColumnX, iRowY, MISSIONS_COLUMN_WIDTH, MISSION_ROW_HEIGHT - 3, COL_PURPLE, 0.5f);
    }
    else
    {
        FillRect(iColumnX, iRowY, MISSIONS_COLUMN_WIDTH, MISSION_ROW_HEIGHT - 3, COL_PANEL, mission.bCompleted ? 0.5f : 1.f);
        OutlineRect(iColumnX, iRowY, MISSIONS_COLUMN_WIDTH, MISSION_ROW_HEIGHT - 3, COL_GOLD_DARK, mission.bCompleted ? 0.3f : 0.8f);
    }

    // Status marker square (right edge - RTL reading start).
    const int iMarkerX = iColumnX + MISSIONS_COLUMN_WIDTH - 16;
    const int iMarkerY = iRowY + 5;
    if (mission.bCompleted)
    {
        FillRect(iMarkerX, iMarkerY, 10, 10, COL_GREEN);
    }
    else if (bLockedPro)
    {
        FillRect(iMarkerX, iMarkerY, 10, 10, COL_PURPLE);
    }
    else
    {
        OutlineRect(iMarkerX, iMarkerY, 10, 10, COL_GOLD_DARK);
    }

    // Mission text. Bold while the mission is still open - it is the line the player
    // actually acts on; a finished one stays in the regular font and steps back.
    if (mission.bCompleted)
    {
        SetTextMuted();
    }
    else
    {
        g_pRenderText->SetFont(g_hFontBold);
        if (mission.bIsPro)
        {
            SetTextPurple();
        }
        else
        {
            SetTextIvory();
        }
    }

    // The text box starts to the left of the progress box instead of overlapping it -
    // a long mission name used to run into the points and both became unreadable.
    g_pRenderText->RenderText(iColumnX + PROGRESS_BOX_WIDTH + 10, iRowY + 5, mission.szText, MISSION_TEXT_WIDTH, 0, SortLeading());
    g_pRenderText->SetFont(g_hFont);

    // Progress / points (left side).
    if (mission.bCompleted)
    {
        SetTextGreen();
        swprintf_s(szText, L"+%d ✓", mission.iPoints);
    }
    else if (mission.iTarget > 1)
    {
        SetTextGold();
        swprintf_s(szText, L"%d/%d · +%d", mission.iProgress, mission.iTarget, mission.iPoints);
    }
    else
    {
        SetTextGold();
        swprintf_s(szText, L"+%d", mission.iPoints);
    }

    g_pRenderText->RenderText(iColumnX + 6, iRowY + 5, szText, PROGRESS_BOX_WIDTH, 0, RT3_SORT_LEFT);
}

void SEASON3B::CMuPassWindow::RenderRewardTrack()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
    const int iColumnX = m_Pos.x + TRACK_COLUMN_X;
    int iRowY = m_Pos.y + BODY_TOP;

    // Column geometry, laid out right-to-left: level number, free slot, pro slot.
    const int iLevelNumX = iColumnX + TRACK_COLUMN_WIDTH - 26;
    const int iFreeSlotX = iColumnX + TRACK_COLUMN_WIDTH - 30 - TRACK_SLOT_SIZE;
    const int iProSlotX = iColumnX + TRACK_COLUMN_WIDTH - 34 - (2 * TRACK_SLOT_SIZE);

    g_pRenderText->SetFont(g_hFontBold);
    SetTextGold();
    g_pRenderText->RenderText(iColumnX, iRowY, I18N::Game::RewardTrack, TRACK_COLUMN_WIDTH, 0, SortLeading());
    g_pRenderText->SetFont(g_hFont);
    iRowY += 16;

    if (state.Track.empty())
    {
        SetTextMuted();
        g_pRenderText->RenderText(iColumnX, iRowY + 8, I18N::Game::LoadingRewards, TRACK_COLUMN_WIDTH, 0, RT3_SORT_CENTER);
        return;
    }

    // Column captions sit directly above their slot columns, so it's obvious which
    // column is which track.
    SetTextMuted();
    g_pRenderText->RenderText(iFreeSlotX, iRowY, I18N::Game::Free, TRACK_SLOT_SIZE, 0, RT3_SORT_CENTER);
    SetTextPurple();
    g_pRenderText->RenderText(iProSlotX, iRowY, I18N::Game::Pro, TRACK_SLOT_SIZE, 0, RT3_SORT_CENTER);
    iRowY += 14;

    const int iRowCount = static_cast<int>(state.Track.size());
    for (int iRow = 0; iRow < TRACK_VISIBLE_ROWS; ++iRow)
    {
        const int iTrackIndex = m_iTrackScroll + iRow;
        if (iTrackIndex >= iRowCount)
        {
            break;
        }

        const GameLogic::MuPass::TrackLevel& level = state.Track[iTrackIndex];
        const bool bCurrentLevel = level.iLevel == state.iLevel;

        if (bCurrentLevel)
        {
            FillRect(iColumnX, iRowY - 2, TRACK_COLUMN_WIDTH, TRACK_ROW_HEIGHT, COL_PANEL);
            OutlineRect(iColumnX, iRowY - 2, TRACK_COLUMN_WIDTH, TRACK_ROW_HEIGHT, COL_GOLD);
        }

        // Level number - right edge.
        wchar_t szLevel[8];
        swprintf_s(szLevel, L"%d", level.iLevel);
        if (bCurrentLevel)
        {
            SetTextGold();
        }
        else
        {
            SetTextMuted();
        }

        g_pRenderText->RenderText(iLevelNumX, iRowY + 14, szLevel, 24, 0, RT3_SORT_CENTER);

        RenderTrackSlot(level.Free, iFreeSlotX, iRowY, false);
        RenderTrackSlot(level.Pro, iProSlotX, iRowY, true);

        iRowY += TRACK_ROW_HEIGHT;
    }
}

void SEASON3B::CMuPassWindow::RenderTrackSlot(const GameLogic::MuPass::TrackReward& reward, int iSlotX, int iSlotY, bool bProSlot)
{
    const float* pBorderCol = bProSlot ? COL_PURPLE : COL_GOLD_DARK;
    const bool bReady = reward.eState == RewardState::Ready;
    const bool bCollected = reward.eState == RewardState::Collected;
    const bool bLocked = reward.eState == RewardState::Locked;

    FillRect(iSlotX, iSlotY, TRACK_SLOT_SIZE, TRACK_SLOT_SIZE, bProSlot ? COL_PURPLE_BG : COL_BG, bLocked ? 0.55f : 1.f);

    if (bReady)
    {
        // Pulsing gold border - the "reward is waiting" animation.
        OutlineRect(iSlotX - 1, iSlotY - 1, TRACK_SLOT_SIZE + 2, TRACK_SLOT_SIZE + 2, COL_GOLD, GetPulseAlpha());
    }
    else
    {
        OutlineRect(iSlotX, iSlotY, TRACK_SLOT_SIZE, TRACK_SLOT_SIZE, pBorderCol, bLocked ? 0.5f : 0.9f);
    }

    if (reward.iItemType >= 0)
    {
        QueueItemIcon(iSlotX + 1, iSlotY + 1, reward.iItemType, reward.iItemLevel);

        // Remember what the mouse is over; the tooltip itself is drawn at the end of
        // Render(), because it must sit above the 3D icon pass. Rewards without a concrete
        // item (credits, random excellent) get a plain text box instead of an item tooltip -
        // without it the icon alone never says what the reward actually is.
        if (CheckMouseIn(iSlotX, iSlotY, TRACK_SLOT_SIZE, TRACK_SLOT_SIZE))
        {
            m_bHoveredReward = true;
            m_bHoveredHasItem = reward.bHasItemData;
            m_iHoveredTipX = iSlotX + (TRACK_SLOT_SIZE / 2);
            m_iHoveredTipY = iSlotY;
            memcpy(m_HoveredItemData, reward.ItemData, GameLogic::MuPass::REWARD_ITEM_DATA_BYTES);
            wcscpy_s(m_szHoveredLabel, reward.szLabel);
        }
    }

    if (bCollected)
    {
        // Green check overlay.
        FillRect(iSlotX, iSlotY, TRACK_SLOT_SIZE, TRACK_SLOT_SIZE, COL_BG, 0.45f);
        SetTextGreen();
        g_pRenderText->SetFont(g_hFontBold);
        g_pRenderText->RenderText(iSlotX, iSlotY + 13, L"✓", TRACK_SLOT_SIZE, 0, RT3_SORT_CENTER);
        g_pRenderText->SetFont(g_hFont);
    }

    if (reward.iAmount > 1)
    {
        wchar_t szAmount[8];
        swprintf_s(szAmount, L"x%d", reward.iAmount);
        SetTextIvory();
        g_pRenderText->RenderText(iSlotX + 2, iSlotY + TRACK_SLOT_SIZE - 12, szAmount, TRACK_SLOT_SIZE - 4, 0, RT3_SORT_LEFT);
    }
}

void SEASON3B::CMuPassWindow::RenderRewardToolTip()
{
    if (!m_bHoveredReward)
    {
        return;
    }

    if (!m_bHoveredHasItem)
    {
        // Credits and random-excellent rewards have no item to build, so all we can show is
        // what the reward is - which is exactly what was missing: the icon alone never said
        // "credits". Drawn in the same style as the info overlay.
        if (m_szHoveredLabel[0] == L'\0')
        {
            return;
        }

        const int iTextWidth = 150;
        const int iBoxHeight = 22;
        int iBoxX = m_iHoveredTipX - (iTextWidth / 2);
        int iBoxY = m_iHoveredTipY - iBoxHeight - 4;

        // Keep the box inside the 640x480 UI space, so a reward at the edge stays readable.
        if (iBoxX < 2) { iBoxX = 2; }
        if (iBoxX + iTextWidth > 638) { iBoxX = 638 - iTextWidth; }
        if (iBoxY < 2) { iBoxY = m_iHoveredTipY + TRACK_SLOT_SIZE + 4; }

        FillRect(iBoxX, iBoxY, iTextWidth, iBoxHeight, COL_PANEL, 0.97f);
        OutlineRect(iBoxX, iBoxY, iTextWidth, iBoxHeight, COL_GOLD);
        g_pRenderText->SetFont(g_hFontBold);
        SetTextGold();
        g_pRenderText->RenderText(iBoxX, iBoxY + 4, m_szHoveredLabel, iTextWidth, 0, RT3_SORT_CENTER);
        g_pRenderText->SetFont(g_hFont);
        return;
    }

    // The reward arrives serialized in the same format as every other item, so the normal
    // parser builds it and the normal tooltip renders it - excellent options, luck, skill,
    // the additional option and the requirements all come out exactly as on a real item.
    ItemCreationParams params = ParseItemData(
        std::span<const BYTE>(m_HoveredItemData, GameLogic::MuPass::REWARD_ITEM_DATA_BYTES));

    ITEM* pItem = g_pNewItemMng->CreateItemByParameters(&params);
    if (pItem == nullptr)
    {
        return;
    }

    // Pets take a different tooltip path which asks the server for the pet's level and
    // experience - and a reward that is not in any inventory has nothing to ask about.
    if (pItem->Type != ITEM_DARK_HORSE_ITEM && pItem->Type != ITEM_DARK_RAVEN_ITEM)
    {
        ::RenderItemInfo(m_iHoveredTipX, m_iHoveredTipY, pItem, false, 0, true);
    }

    // Built and released per frame: the tooltip needs it only while it is drawn, and
    // keeping it alive would mean tracking when the hovered reward changes.
    g_pNewItemMng->DeleteItem(pItem);
}

void SEASON3B::CMuPassWindow::QueueItemIcon(int iSlotX, int iSlotY, int iItemType, int iItemLevel)
{
    if (m_pendingItemCount >= TRACK_VISIBLE_ROWS * 2)
    {
        return;
    }

    PendingItemIcon& icon = m_pendingItems[m_pendingItemCount++];
    icon.iX = iSlotX;
    icon.iY = iSlotY;
    icon.iSize = TRACK_SLOT_SIZE - 2;
    icon.iType = iItemType;
    icon.iLevel = iItemLevel;
}

void SEASON3B::CMuPassWindow::RenderPendingItems()
{
    if (m_pendingItemCount == 0)
    {
        return;
    }

    // Same 3D setup the item shop uses before RenderItem3D: switch to a perspective
    // projection with the game camera, enable depth, then restore 2D bitmap mode.
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
            static_cast<float>(icon.iSize),
            static_cast<float>(icon.iSize),
            icon.iType,
            icon.iLevel,
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

void SEASON3B::CMuPassWindow::RenderBottomButtons()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
    const int iButtonY = m_Pos.y + BOTTOM_BAR_TOP + 6;
    wchar_t szText[96];

    FillRect(m_Pos.x, m_Pos.y + BOTTOM_BAR_TOP, WND_WIDTH, WND_HEIGHT - BOTTOM_BAR_TOP, COL_PANEL);
    FillRect(m_Pos.x, m_Pos.y + BOTTOM_BAR_TOP, WND_WIDTH, 2, COL_GOLD_DARK);

    // Collect button - right.
    bool bReadyIsPro = false;
    const int iReadyLevel = GameLogic::MuPass::GetFirstReadyLevel(&bReadyIsPro);
    const int iCollectX = m_Pos.x + WND_WIDTH - 12 - BUTTON_WIDTH;
    if (iReadyLevel > 0)
    {
        FillRect(iCollectX, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT, COL_BAR_FILL, GetPulseAlpha());
        OutlineRect(iCollectX, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT, COL_GOLD);
        g_pRenderText->SetFont(g_hFontBold);
        g_pRenderText->SetTextColor(20, 16, 12, 255);
        swprintf_s(szText, I18N::Game::ClaimRewardLevelD, iReadyLevel);
        g_pRenderText->RenderText(iCollectX, iButtonY + 10, szText, BUTTON_WIDTH, 0, RT3_SORT_CENTER);
    }
    else
    {
        FillRect(iCollectX, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT, COL_BG, 0.6f);
        OutlineRect(iCollectX, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT, COL_GOLD_DARK, 0.5f);
        g_pRenderText->SetFont(g_hFont);
        SetTextMuted();
        g_pRenderText->RenderText(iCollectX, iButtonY + 10, I18N::Game::NoRewardsToClaim, BUTTON_WIDTH, 0, RT3_SORT_CENTER);
    }

    // Buy-pro button - left.
    const int iProX = m_Pos.x + 12;
    if (state.bIsPro)
    {
        g_pRenderText->SetFont(g_hFontBold);
        SetTextPurple();
        g_pRenderText->RenderText(iProX, iButtonY + 10, I18N::Game::ProTrackActive, BUTTON_WIDTH, 0, RT3_SORT_CENTER);
    }
    else
    {
        FillRect(iProX, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT, COL_PURPLE_BG);
        OutlineRect(iProX, iButtonY, BUTTON_WIDTH, BUTTON_HEIGHT, COL_PURPLE);
        g_pRenderText->SetFont(g_hFontBold);
        SetTextPurple();
        if (IsProConfirmPending())
        {
            swprintf_s(szText, I18N::Game::ClickAgainToConfirmDCredits, state.iProPriceCredits);
        }
        else
        {
            swprintf_s(szText, I18N::Game::BuyProDCredits, state.iProPriceCredits);
        }

        g_pRenderText->RenderText(iProX, iButtonY + 10, szText, BUTTON_WIDTH, 0, RT3_SORT_CENTER);
    }

    g_pRenderText->SetFont(g_hFont);
}

void SEASON3B::CMuPassWindow::RenderInfoOverlay()
{
    const GameLogic::MuPass::State& state = GameLogic::MuPass::GetState();
    const int iPanelX = m_Pos.x + 40;
    const int iPanelY = m_Pos.y + 110;
    const int iPanelWidth = WND_WIDTH - 80;
    const int iPanelHeight = 150;
    wchar_t szText[160];

    FillRect(iPanelX, iPanelY, iPanelWidth, iPanelHeight, COL_PANEL, 0.97f);
    OutlineRect(iPanelX, iPanelY, iPanelWidth, iPanelHeight, COL_GOLD);

    g_pRenderText->SetFont(g_hFontBold);
    SetTextGold();
    g_pRenderText->RenderText(iPanelX + 10, iPanelY + 10, I18N::Game::WhatIsMUPass, iPanelWidth - 20, 0, SortLeading());

    g_pRenderText->SetFont(g_hFont);
    SetTextIvory();
    const wchar_t* apszLines[] =
    {
        I18N::Game::MUPassIsASeasonalProgressionTrack,
        I18N::Game::CompleteDailyMissionsCollectGoblinPointsAndGainLevels,
        I18N::Game::EveryLevelUnlocksARewardCollectItWithOneClick,
        I18N::Game::MissionsYouDidNotFinishCarryOverToTheNextDayUpTo10Open,
        I18N::Game::ProgressIsSharedByEveryCharacterOnYourAccount,
    };
    int iLineY = iPanelY + 34;
    for (const wchar_t* pszLine : apszLines)
    {
        g_pRenderText->RenderText(iPanelX + 10, iLineY, pszLine, iPanelWidth - 20, 0, SortLeading());
        iLineY += 18;
    }

    SetTextPurple();
    swprintf_s(szText, I18N::Game::ProTrackDExtraMissionsADayAndUpgradedRewardsDCredits, 3, state.iProPriceCredits);
    g_pRenderText->RenderText(iPanelX + 10, iLineY + 4, szText, iPanelWidth - 20, 0, SortLeading());
}
