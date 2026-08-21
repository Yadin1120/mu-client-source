#include "stdafx.h"
#include "GameLogic/Combat/SkillExecution.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>

#include "Engine/AI/ZzzAI.h"
#include "Engine/Object/ZzzCharacter.h"
#include "Engine/Object/ZzzInterface.h"
#include "Engine/Object/PlayerActionState.h"
#include "UI/NewUI/NewUISystem.h"
#include "Core/Utilities/Log/muConsoleDebug.h"
#include "Character/CharacterManager.h"
#include "GameLogic/Skills/SkillManager.h"
#include "GameLogic/Social/PartyManager.h"
#include "World/MapInfra/MapManager.h"
#include "Network/Server/WSclient.h"

#include "MuHelper.h"

constexpr int MAX_ACTIONABLE_DISTANCE = 10;
constexpr int DEFAULT_DURABILITY_THRESHOLD = 50;

// כמה צעדים מותר לצעוד בפעימה אחת כשניגשים למטרה. הקצב של המנוע המקורי.
constexpr int MAX_STEPS_PER_TICK = 2;

// כמה פעימות נכנסות לשנייה. חייב להתאים ל-MUHELPER_TIMER ב-Winmain.cpp
// (‏100ms), אחרת מוני השניות משקרים: מרווחי הכישופים לפי טיימר וזמן
// ההתרחקות מהעוגן נמדדים בשניות ונגזרים מכאן.
constexpr int TICKS_PER_SECOND = 10;

// כל כמה פעימות נסרק המסך מחדש אחרי מפלצות דוממות. פקטות הלידה
// והתנועה מכסות את הרוב, אבל מפלצת שנטענה בלי פקטה — אחרי ורפ, אחרי
// התחברות מחדש, או כשהעוזר נדלק מרחוק — לא מגיעה מהן לעולם.
constexpr int RESEED_INTERVAL_TICKS = 2 * TICKS_PER_SECOND;

// מרחק שנחשב "בעוגן". משבצת אחת, כמו במונה ההתרחקות של WorkLoop.
constexpr int ANCHOR_TOLERANCE = 1;

// שומר התקיעות: כמה פעימות מותר להיות "בתנועה" בלי שהמיקום בפועל
// משתנה. 3 שניות הן הרבה מעבר למסלול של שני צעדים.
constexpr int STALL_LIMIT_TICKS = 3 * TICKS_PER_SECOND;

// כמה זמן מותר לשלב תמיכה (באף/ריפוי) לעצור את הלולאה לפני שממשיכים
// לתקוף בלעדיו. מעל זה מדובר בחסם קבוע ולא בהמתנה לנפנוף.
constexpr int SUPPORT_BLOCK_LIMIT_TICKS = 2 * TICKS_PER_SECOND;

SpinLock _targetsLock;
SpinLock _itemsLock;

// Movement/target globals are defined in ZzzInterface.cpp.
extern MovementSkill g_MovementSkill;
extern int SelectedCharacter;
extern int TargetX;
extern int TargetY;

namespace MUHelper
{
	MovementSkill& g_MovementSkill = ::g_MovementSkill;
	int& SelectedCharacter = ::SelectedCharacter;
	int& TargetX = ::TargetX;
	int& TargetY = ::TargetY;

    CMuHelper g_MuHelper;

    void CALLBACK CMuHelper::TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
    {
        g_MuHelper.WorkLoop(hwnd, uMsg, idEvent, dwTime);
    }

    void CMuHelper::Save(const ConfigData& config)
    {
        m_config = config;

        PRECEIVE_MUHELPER_DATA netData;
        ConfigDataSerDe::Serialize(m_config, netData);

        SocketClient->ToGameServer()->SendMuHelperSaveDataRequest(reinterpret_cast<BYTE*>(&netData), sizeof(netData));
    }

    void CMuHelper::Load(const ConfigData& config)
    {
        m_config = config;
    }

    ConfigData CMuHelper::GetConfig() const {
        return m_config;
    }

    void CMuHelper::Toggle()
    {
        if (m_bActive)
        {
            TriggerStop();

            // Stop the client-driven bot immediately instead of waiting for the
            // server's status reply. After an auto-reconnect the server's new
            // session doesn't have the helper marked active, so it never replies
            // and the bot would otherwise keep running with no way to stop it.
            Stop();
        }
        else
        {
            TriggerStart();
        }
    }

    void CMuHelper::TriggerStart()
    {
        if (!Hero->SafeZone)
            SocketClient->ToGameServer()->SendMuHelperStatusChangeRequest(0);
    }

    void CMuHelper::TriggerStop()
    {
        SocketClient->ToGameServer()->SendMuHelperStatusChangeRequest(1);
    }

    void CMuHelper::Start()
    {
        if (m_bActive)
        {
            return;
        }

        m_iTotalCost = 0;
        m_iComboState = 0;
        m_iCurrentBuffIndex = 0;
        m_iCurrentBuffPartyIndex = 0;
        m_iCurrentHealPartyIndex = 0;
        m_iCurrentTarget = -1;
        m_iCurrentSkill = (ActionSkillType)m_config.aiSkill[0];
        m_iCurrentItem = MAX_ITEMS;
        m_posOriginal = { Hero->PositionX, Hero->PositionY };

        // הגדרות שמורות בשרת יכולות להגיע עם אפס (חשבון שנשמר לפני שהיו
        // ברירות מחדל, או שמעולם לא כוון בחלון Z). טווח אפס = עוזר עיוור,
        // ולכן אפס מיושר לברירת המחדל ולא נלקח כפשוטו.
        m_iHuntingDistance = ComputeDistanceByRange(
            m_config.iHuntingRange > 0 ? m_config.iHuntingRange : DEFAULT_HELPER_RANGE);
        m_iObtainingDistance = ComputeDistanceByRange(
            m_config.iObtainingRange > 0 ? m_config.iObtainingRange : DEFAULT_HELPER_RANGE);

        // רצועת העיגון = טווח הציד שהשחקן כבר בחר. אין הגדרה חדשה, אין
        // שינוי בפרוטוקול: "טווח ציד 6" פירושו גם "עד כמה מותר להתרחק".
        m_iLeashDistance = m_iHuntingDistance;
        m_iReseedCounter = 0;

        m_iSecondsElapsed = 0;
        m_iSecondsAway = 0;

        m_iStallTicks = 0;
        m_posLastSeen = m_posOriginal;
        m_iSupportBlockTicks = 0;

        m_bTimerActivatedBuffOngoing = false;
        m_bPetActivated = false;

        m_iLoopCounter = 0;

        m_bActive = true;

        SeedTargetsFromViewport();

        g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Started");
    }

    // מפלצות שכבר עומדות במסך ברגע ההדלקה. המזינים הרגילים של AddTarget הם
    // פקטות תנועה/לידה/תקיפה — מפלצת דוממת שנטענה לפני ההדלקה לא מגיעה מהם
    // לעולם, ולכן העוזר "לא תקף בכלל" ליד מפלצות עומדות (16/08/2026).
    // חייב לרוץ אחרי m_bActive = true, כי AddTarget דוחה קריאות כשהעוזר כבוי.
    void CMuHelper::SeedTargetsFromViewport()
    {
        for (int i = 0; i < MAX_CHARACTERS_CLIENT; i++)
        {
            CHARACTER* pChar = &CharactersClient[i];
            if (!pChar->Object.Live || pChar->Dead != 0 || !IsMonster(pChar))
            {
                continue;
            }

            AddTarget(pChar->Key, false);
        }
    }

    void CMuHelper::Stop()
    {
        m_bActive = false;
        g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Stopped");
    }

    void CMuHelper::WorkLoop(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
    {
        if (!m_bActive)
        {
            return;
        }

        if (Hero->SafeZone)
        {
            g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Entered safezone. Stopping.");
            TriggerStop();
            return;
        }

        // סריקה חוזרת של המסך: ב-16/08 היא נוספה רק להדלקה, ולכן מפלצת
        // דוממת שהופיעה אחר כך בלי פקטת לידה נשארה בלתי־נראית לעוזר עד
        // שזזה מעצמה. בסריקה מחזורית אין חלון עיוור כזה.
        if (++m_iReseedCounter >= RESEED_INTERVAL_TICKS)
        {
            m_iReseedCounter = 0;
            SeedTargetsFromViewport();
        }

        BreakStallIfStuck();

        Work();

        if (++m_iLoopCounter >= TICKS_PER_SECOND)
        {
            m_iSecondsElapsed++;

            if (ComputeDistanceBetween({ Hero->PositionX, Hero->PositionY }, m_posOriginal) > 1)
            {
                m_iSecondsAway++;
            }
            else
            {
                m_iSecondsAway = 0;
            }

            m_iLoopCounter = 0;
        }
    }

    void CMuHelper::Work()
    {
        try
        {
            if (!ActivatePet())
            {
                return;
            }

            // 🔴 שלבי התמיכה עצרו את הלולאה לנצח. `Buff` ו-`Heal` מחזירים
            // 0 בכל כישלון של הכישוף — ואין מאנה, כישוף בקירור או מטרה שלא
            // ניתן להגיע אליה הם כישלון קבוע, לא המתנה. התוצאה: הדמות עומדת
            // בלי לתקוף כלום, לפעמים עם חיים נמוכים ובלי מאנה לרפא, עד
            // שנהרגת. עכשיו נותנים לשלב זמן קצוב לחסום — מספיק לנפנוף אחד
            // להסתיים — ואחריו ממשיכים לתקוף בלעדיו. המונה מתאפס רק כששלב
            // התמיכה מצליח שוב, כך שחוסר מאנה מתמשך לא חוזר לחנוק אותנו.
            if (!Buff() || !RecoverHealth())
            {
                if (++m_iSupportBlockTicks < SUPPORT_BLOCK_LIMIT_TICKS)
                {
                    return;
                }
            }
            else
            {
                m_iSupportBlockTicks = 0;
            }

            // תקיפה לפני איסוף (20/08/2026). בסדר ההפוך, כל פעימה שבה הדמות
            // הייתה בדרך לפריט עצרה את הלולאה לפני Attack — ועם רצפה מלאה
            // בזן זה כמעט כל פעימה, כלומר הדמות רצה בין ערימות ובקושי תוקפת.
            // השלל לא בורח (‏20 שניות בעלות, דקה עד היעלמות); המפלצת כן.
            Attack();

            // אוספים רק כשאין מטרה בהישג. כל עוד יש במי להילחם, הדמות לא
            // עוזבת את הקרב בשביל ערימה על הרצפה.
            if (m_iCurrentTarget == -1)
            {
                if (!ObtainItem())
                {
                    return;
                }

                if (!Regroup())
                {
                    return;
                }
            }

            RepairEquipments();
        }
        catch (...)
        {
            g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Exception occurred. Ignoring...");
        }
    }

    void CMuHelper::AddTarget(int iTargetId, bool bIsAttacking)
    {
        if (!m_bActive)
        {
            return;
        }

        CHARACTER* pTarget = FindCharacterByKey(iTargetId);
        if (!pTarget || pTarget == Hero)
        {
            return;
        }

        int iDistance = ComputeDistanceFromTarget(pTarget);

        if ((iDistance <= m_iHuntingDistance)
            || (bIsAttacking && m_config.bLongRangeCounterAttack))
        {
            _targetsLock.lock();

            m_setTargets.insert(iTargetId);

            if (bIsAttacking)
            {
                m_setTargetsAttacking.insert(iTargetId);
            }

            _targetsLock.unlock();
        }

        if (m_config.bUseSelfDefense && IsMonster(pTarget))
        {
            m_iCurrentTarget = iTargetId;
        }
    }

    void CMuHelper::DeleteTarget(int iTargetId)
    {
        _targetsLock.lock();

        m_setTargets.erase(iTargetId);
        m_setTargetsAttacking.erase(iTargetId);

        _targetsLock.unlock();

        if (iTargetId == m_iCurrentTarget)
        {
            m_iCurrentTarget = -1;
        }
    }

    void CMuHelper::DeleteAllTargets()
    {
        _targetsLock.lock();

        m_setTargets.clear();
        m_setTargetsAttacking.clear();

        _targetsLock.unlock();
    }

    int CMuHelper::ComputeDistanceByRange(int iRange)
    {
        return ComputeDistanceBetween({ 0, 0 }, { iRange, iRange });
    }

    int CMuHelper::ComputeDistanceFromTarget(CHARACTER* pTarget)
    {
        const POINT posHero = { Hero->PositionX, Hero->PositionY };

        const POINT posCurrent = { pTarget->PositionX, pTarget->PositionY };
        const POINT posNext    = { pTarget->TargetX,   pTarget->TargetY };

        return std::min(
            ComputeDistanceBetween(posHero, posCurrent),
            ComputeDistanceBetween(posHero, posNext)
        );
    }

    int CMuHelper::ComputeDistanceBetween(POINT posA, POINT posB)
    {
        int iDx = posA.x - posB.x;
        int iDy = posA.y - posB.y;

        return static_cast<int>(std::ceil(std::sqrt(iDx * iDx + iDy * iDy)));
    }

    // ציד מעוגן (20/08/2026). הנקודה שבה נדלק העוזר היא עוגן, והדמות רשאית
    // לצעוד לעבר מטרות רק בתוך רצועה סביבו. עד 16/08 המנוע המקורי הלך שני
    // צעדים אל המטרה בלי שום גבול — זה ה"רודף אחרי מפלצות" שבוטל; הניטרול
    // שבא במקומו השאיר את הדמות עומדת גם כשמפלצת שני צעדים ממנה. הרצועה
    // נותנת את שניהם: ניגשת למה שקרוב, לא נודדת מהמקום.
    bool CMuHelper::IsWithinLeash(POINT pos)
    {
        return ComputeDistanceBetween(pos, m_posOriginal) <= m_iLeashDistance;
    }

    // האזור שהעוזר בכלל מתעניין בו: כל מה שאפשר לתקוף מאיזושהי נקודה בתוך
    // הרצועה. רחב מהרצועה עצמה בדיוק בטווח הציד.
    bool CMuHelper::IsWithinHuntingArea(POINT pos)
    {
        return ComputeDistanceBetween(pos, m_posOriginal)
            <= (m_iLeashDistance + m_iHuntingDistance);
    }

    // צועד לעבר המטרה לאורך מסלול שכבר חושב, ורק כל עוד הצעדים נשארים
    // בתוך הרצועה. ‏0 = לא זזנו כי הצעד הבא חורג — הקורא משחרר את התור
    // למטרה קרובה יותר במקום להיגרר אחרי זו.
    int CMuHelper::StepTowardTarget(const PATH_t& tempPath)
    {
        // "רק לתקוף": לא ניגשים לשום מטרה. מחזירים 0 כדי שהקורא ישחרר את
        // התור למטרה קרובה יותר שכבר בטווח — אחרת הדמות הייתה נועלת את
        // עצמה על מפלצת רחוקה ועומדת בלי לתקוף כלום.
        if (m_config.bStayInPlace)
        {
            return 0;
        }

        // 🔴 אין להוציא פקודת תנועה חדשה כשהקודמת עוד רצה. אחרי שהפעימה
        // ירדה ל-100ms זה קרה עשר פעמים בשנייה, וכל פקודה אתחלה מסלול של
        // שני צעדים מחדש לפני שהקודם הסתיים — הדמות מגמגמת במקום ללכת.
        // מחזירים 1 ולא 0: אנחנו בדרך, לא נכשלנו, ואסור לשחרר את המטרה.
        if (Hero->Movement)
        {
            return 1;
        }

        const int iPathNum = std::min<int>(tempPath.PathNum, MAX_STEPS_PER_TICK);

        int iSteps = 0;
        while (iSteps < iPathNum
            && IsWithinLeash({ tempPath.PathX[iSteps], tempPath.PathY[iSteps] }))
        {
            iSteps++;
        }

        if (iSteps == 0)
        {
            return 0;
        }

        Hero->Path.Lock.lock();

        for (int i = 0; i < iSteps; i++)
        {
            Hero->Path.PathX[i] = tempPath.PathX[i];
            Hero->Path.PathY[i] = tempPath.PathY[i];
        }
        Hero->Path.PathNum = iSteps;
        Hero->Path.CurrentPath = 0;
        Hero->Path.CurrentPathFloat = 0;

        Hero->Path.Lock.unlock();

        SendMove(Hero, &Hero->Object);
        return 1;
    }

    // iMaxDistance מגביל את החיפוש (למשל לטווח הכישוף);
    // ‏-1 = טווח הציד המלא, כמו תמיד.
    int CMuHelper::GetNearestTarget(int iMaxDistance)
    {
        int iClosestMonsterId = -1;
        int iMinDistance = (iMaxDistance >= 0)
            ? std::min(iMaxDistance, m_iHuntingDistance)
            : m_iHuntingDistance;
        std::set<int> setTargets;
        {
            _targetsLock.lock();
            setTargets = m_setTargets;
            _targetsLock.unlock();
        }

        for (const int& iMonsterId : setTargets)
        {
            int iIndex = FindCharacterIndex(iMonsterId);
            if (iIndex == MAX_CHARACTERS_CLIENT)
            {
                continue;
            }

            CHARACTER* pTarget = &CharactersClient[iIndex];

            if (!IsMonster(pTarget))
            {
                continue;
            }

            int iDistance = ComputeDistanceFromTarget(pTarget);
            if (iDistance <= iMinDistance)
            {
                iMinDistance = iDistance;
                iClosestMonsterId = iMonsterId;
            }
        }

        return iClosestMonsterId;
    }

    int CMuHelper::GetFarthestAttackingTarget()
    {
        int iFarthestMonsterId = -1;
        int iMaxDistance = -1;

        std::set<int> setTargets;
        {
            _targetsLock.lock();
            setTargets = m_setTargetsAttacking;
            _targetsLock.unlock();
        }

        for (const int& iMonsterId : setTargets)
        {
            int iIndex = FindCharacterIndex(iMonsterId);
            if (iIndex == MAX_CHARACTERS_CLIENT)
            {
                continue;
            }

            CHARACTER* pTarget = &CharactersClient[iIndex];

            if (!IsMonster(pTarget))
            {
                continue;
            }

            int iDistance = ComputeDistanceFromTarget(pTarget);
            if (iDistance > iMaxDistance)
            {
                iMaxDistance = iDistance;
                iFarthestMonsterId = iMonsterId;
            }
        }

        return iFarthestMonsterId;
    }

    void CMuHelper::CleanupTargets()
    {
        std::set<int> setTargets;
        std::set<int> setAttacking;
        {
            _targetsLock.lock();
            setTargets = m_setTargets;
            setAttacking = m_setTargetsAttacking;
            _targetsLock.unlock();
        }

        for (const int& iMonsterId : setTargets)
        {
            int iIndex = FindCharacterIndex(iMonsterId);
            if (iIndex == MAX_CHARACTERS_CLIENT)
            {
                DeleteTarget(iMonsterId);
                continue;
            }

            CHARACTER* pTarget = &CharactersClient[iIndex];
            if (pTarget->Dead > 0 || !pTarget->Object.Live)
            {
                DeleteTarget(iMonsterId);
                continue;
            }

            // מפלצת שנדדה אל מחוץ לאזור הציד כבר לא ברת־השגה. בלי הניקוי
            // הזה הרשימה נשארת מלאה במטרות רחוקות, והעוזר "עסוק" לכאורה
            // בזמן שאין לו מה לתקוף. מי שתוקף אותנו נשאר ברשימה בכל מקרה.
            if (!IsWithinHuntingArea({ pTarget->PositionX, pTarget->PositionY })
                && setAttacking.find(iMonsterId) == setAttacking.end())
            {
                DeleteTarget(iMonsterId);
            }
        }
    }

    int CMuHelper::ActivatePet()
    {
        if (!m_config.bUseDarkRaven)
        {
            return 1;
        }

        if (m_bPetActivated)
        {
            return 1;
        }

        if (m_config.iDarkRavenMode == PET_ATTACK_CEASE)
        {
            SocketClient->ToGameServer()->SendPetCommandRequest(PetType::DarkRaven, PetCommandMode::Normal, 0xFFFF);
        }
        else if (m_config.iDarkRavenMode == PET_ATTACK_AUTO)
        {
            SocketClient->ToGameServer()->SendPetCommandRequest(PetType::DarkRaven, PetCommandMode::AttackRandom, 0xFFFF);
        }
        else if (m_config.iDarkRavenMode == PET_ATTACK_TOGETHER)
        {
            SocketClient->ToGameServer()->SendPetCommandRequest(PetType::DarkRaven, PetCommandMode::AttackWithOwner, 0xFFFF);
        }

        m_bPetActivated = true;
        return 1;
    }

    int CMuHelper::Buff()
    {
        if (!HasAssignedBuffSkill())
        {
            return 1;
        }

        if (m_config.bSupportParty && g_pPartyManager->IsPartyActive())
        {
            PARTY_t* pMember = &Party[m_iCurrentBuffPartyIndex];
            CHARACTER* pChar = g_pPartyManager->GetPartyMemberChar(pMember);

            if (pChar != NULL
                && pMember->Map == gMapManager.WorldActive
                && ComputeDistanceFromTarget(pChar) <= MAX_ACTIONABLE_DISTANCE)
            {
                if (!m_config.bBuffDurationParty
                    && m_config.iBuffCastInterval != 0
                    && m_iSecondsElapsed % m_config.iBuffCastInterval == 0)
                {
                    m_bTimerActivatedBuffOngoing = true;
                }

                if (!BuffTarget(pChar, (ActionSkillType)m_config.aiBuff[m_iCurrentBuffIndex]))
                {
                    return 0;
                }
            }

            m_iCurrentBuffPartyIndex = (m_iCurrentBuffPartyIndex + 1) % (sizeof(Party) / sizeof(Party[0]));
        }
        else
        {
            if (!m_config.bBuffDuration
                && m_config.iBuffCastInterval != 0
                && m_iSecondsElapsed % m_config.iBuffCastInterval == 0)
            {
                m_bTimerActivatedBuffOngoing = true;
            }

            if (!BuffTarget(Hero, (ActionSkillType)m_config.aiBuff[m_iCurrentBuffIndex]))
            {
                return 0;
            }
        }

        if (m_iCurrentBuffPartyIndex == 0)
        {
            m_iCurrentBuffIndex = (m_iCurrentBuffIndex + 1) % m_config.aiBuff.size();

            // Reaching this branch means everyone's been buffed, 
            // so we're resetting the timer activated buff flag
            if (m_iCurrentBuffIndex == 0)
            {
                m_bTimerActivatedBuffOngoing = false;
            }
        }

        return 1;
    }

    int CMuHelper::BuffTarget(CHARACTER* pTargetChar, ActionSkillType iBuffSkill)
    {
        OBJECT* obj = &pTargetChar->Object;

        auto CastIfMissing = [&](bool bBuffActive, bool bTimerRespected, bool bNeedsTarget) -> int
        {
            if (!bBuffActive || (bTimerRespected && m_bTimerActivatedBuffOngoing))
                return SimulateSkill(iBuffSkill, bNeedsTarget, pTargetChar->Key);
            return 1;
        };

        switch (iBuffSkill)
        {
        case AT_SKILL_ATTACK:
        case AT_SKILL_ATTACK_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Attack), true, true);

        case AT_SKILL_DEFENSE:
        case AT_SKILL_DEFENSE_STR:
        case AT_SKILL_DEFENSE_MASTERY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Defense), true, true);

        case AT_SKILL_INFINITY_ARROW:
        case AT_SKILL_INFINITY_ARROW_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_InfinityArrow), false, false);

        case AT_SKILL_SOUL_BARRIER:
        case AT_SKILL_SOUL_BARRIER_STR:
        case AT_SKILL_SOUL_BARRIER_PROFICIENCY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_WizDefense), true, true);

        case AT_SKILL_SWELL_LIFE:
        case AT_SKILL_SWELL_LIFE_STR:
        case AT_SKILL_SWELL_LIFE_PROFICIENCY:
            if (m_iComboState == 2)
            {
                return 1;
            }
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Life), true, false);

        case AT_SKILL_EXPANSION_OF_WIZARDRY:
        case AT_SKILL_EXPANSION_OF_WIZARDRY_STR:
        case AT_SKILL_EXPANSION_OF_WIZARDRY_MASTERY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_SwellOfMagicPower), false, false);

        case AT_SKILL_ADD_CRITICAL:
        case AT_SKILL_ADD_CRITICAL_STR1:
        case AT_SKILL_ADD_CRITICAL_STR2:
        case AT_SKILL_ADD_CRITICAL_STR3:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_AddCriticalDamage), false, false);

        case AT_SKILL_ALICE_BERSERKER:
        case AT_SKILL_ALICE_BERSERKER_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Berserker), false, false);

        case AT_SKILL_ALICE_THORNS:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Thorns), false, false);

        // Rage Fighter party buffs — self/party AoE, no explicit target needed.
        case AT_SKILL_ATT_UP_OURFORCES:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Att_up_Ourforces), true, false);

        case AT_SKILL_HP_UP_OURFORCES:
        case AT_SKILL_HP_UP_OURFORCES_STR:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Hp_up_Ourforces), true, false);

        case AT_SKILL_DEF_UP_OURFORCES:
        case AT_SKILL_DEF_UP_OURFORCES_STR:
        case AT_SKILL_DEF_UP_OURFORCES_MASTERY:
            return CastIfMissing(g_isCharacterBuff(obj, eBuff_Def_up_Ourforces), true, false);

        default:
            return 1;
        }
    }


    int CMuHelper::ConsumePotion()
    {
        int64_t iLife = CharacterAttribute->Life;
        int64_t iLifeMax = CharacterAttribute->LifeMax;

        if (m_config.bUseHealPotion && iLifeMax > 0 && iLife > 0)
        {
            int64_t iRemaining = (iLife * 100 + iLifeMax - 1) / iLifeMax;
            if (iRemaining <= m_config.iPotionThreshold)
            {
                int iPotionIndex = g_pMyInventory->FindHealingItemIndex();
                if (iPotionIndex != -1)
                {
                    SendRequestUse(iPotionIndex, 0);
                }
            }
        }

        return 1;
    }

    int CMuHelper::RecoverHealth()
    {
        if (!Heal())
        {
            return 0;
        }
        
        if (!DrainLife())
        {
            return 0;
        }

        if (!ConsumePotion())
        {
            return 0;
        }

        return 1;
    }

    int CMuHelper::Heal()
    {
        if (!m_config.bAutoHeal)
        {
            return 1;
        }

        auto iHealingSkill = GetHealingSkill();
        if (iHealingSkill == AT_SKILL_UNDEFINED)
        {
            return 1;
        }

        if (m_config.bAutoHealParty && g_pPartyManager->IsPartyActive())
        {
            PARTY_t* pMember = &Party[m_iCurrentHealPartyIndex];
            CHARACTER* pChar = g_pPartyManager->GetPartyMemberChar(pMember);
            int iHealResult = 1;

            if (pChar != NULL)
            {
                if (pChar == Hero)
                {
                    iHealResult = HealSelf(iHealingSkill);
                }
                else if (pMember->Map == gMapManager.WorldActive
                    && pMember->stepHP * 10 <= m_config.iHealPartyThreshold
                    && ComputeDistanceFromTarget(pChar) <= MAX_ACTIONABLE_DISTANCE)
                {
                    iHealResult = SimulateSkill(iHealingSkill, true, pChar->Key);
                }
            }

            m_iCurrentHealPartyIndex = (m_iCurrentHealPartyIndex + 1) % (sizeof(Party) / sizeof(Party[0]));

            return iHealResult;
        }
        else
        {
            return HealSelf(iHealingSkill);
        }

        return 1;
    }

    int CMuHelper::HealSelf(ActionSkillType iHealingSkill)
    {
        int64_t iLife = CharacterAttribute->Life;
        int64_t iLifeMax = CharacterAttribute->LifeMax;
        int64_t iRemaining = (iLife * 100 + iLifeMax - 1) / iLifeMax;

        if (iRemaining <= m_config.iHealThreshold)
        {
            return SimulateSkill(iHealingSkill, true, HeroKey);
        }

        return 1;
    }

    int CMuHelper::DrainLife()
    {
        if (!m_config.bUseDrainLife)
        {
            return 1;
        }

        auto iDrainLife = GetDrainLifeSkill();
        if (iDrainLife == AT_SKILL_UNDEFINED)
        {
            return 1;
        }

        int64_t iLife = CharacterAttribute->Life;
        int64_t iLifeMax = CharacterAttribute->LifeMax;
        int64_t iRemaining = (iLife * 100 + iLifeMax - 1) / iLifeMax;

        if (iRemaining <= m_config.iHealThreshold)
        {
            m_iCurrentTarget = GetNearestTarget();
            if (m_iCurrentTarget != -1)
            {
                return SimulateSkill(iDrainLife, true, m_iCurrentTarget);
            }
        }

        return 1;
    }

    int CMuHelper::RepairEquipments()
    {
        if (m_config.bRepairItem)
        {
            for (int i = 0; i < MAX_EQUIPMENT; i++)
            {
                ITEM* pItem = &CharacterMachine->Equipment[i];
                if (!pItem || pItem->Type == -1)
                {
                    continue;
                }

                ITEM_ATTRIBUTE* pAttr = &ItemAttribute[pItem->Type];
                if (!pAttr)
                {
                    continue;
                }

                int iLevel = pItem->Level;
                int iDurability = pItem->Durability;
                int iMaxDurability = CalcMaxDurability(pItem, pAttr, iLevel);

                int64_t iHealth = (iDurability * 100 + iMaxDurability - 1) / iMaxDurability;

                if (iHealth <= DEFAULT_DURABILITY_THRESHOLD)
                {
                    int64_t iGoldCost = CalcSelfRepairCost(ItemValue(pItem, 2), iDurability, iMaxDurability, pItem->Type);
                    if (iGoldCost <= CharacterMachine->Gold)
                    {
                        SocketClient->ToGameServer()->SendRepairItemRequest(i, 1);
                    }
                }
            }
        }

        return 1;
    }

    int CMuHelper::Attack()
    {
        if (m_iCurrentTarget == -1)
        {
            CleanupTargets();

            if (m_config.bLongRangeCounterAttack)
            {
                m_iCurrentTarget = GetFarthestAttackingTarget();
            }

            if (m_iCurrentTarget == -1)
            {
                m_iCurrentTarget = GetNearestTarget();
            }

            // אין מטרה בהישג. לא רק "הרשימה ריקה" — גם רשימה שכולה מפלצות
            // שהתרחקו נראתה קודם כמו עבודה, והעוזר עמד בלי לעשות כלום ובלי
            // לחזור. עכשיו חוזרים לעוגן ומחכים שם לריספון.
            if (m_iCurrentTarget == -1)
            {
                m_iComboState = 0;

                // "רק לתקוף": ממשיכים לנפנף גם בלי מטרה, בדיוק כמו שחקן
                // שמחזיק את ההתקפה על קרקע ריקה. לכישופי אזור זה לא סתם
                // אנימציה — מה שנכנס לטווח חוטף בלי שנצטרך לבחור בו קודם.
                if (m_config.bStayInPlace)
                {
                    return AttackInPlace();
                }

                ReturnToAnchor();
                return 0;
            }
        }

        if (m_config.bUseCombo)
        {
            return SimulateComboAttack();
        }

        m_iCurrentSkill = SelectAttackSkill();
        if (m_iCurrentSkill > AT_SKILL_UNDEFINED)
        {
            const float fSkillDistance = gSkillManager.GetSkillDistance(m_iCurrentSkill, Hero);
            if (GameLogic::Combat::CanExecuteSkill(Hero, m_iCurrentSkill, fSkillDistance))
            {
                return SimulateAttack(m_iCurrentSkill);
            }

            // ‏CanExecuteSkill נכשל על מאנה או תנאי כישוף — לא על מרחק
            // (המרחק נבדק בתוך SimulateSkill). ניסיון חוזר של אותו כישוף על
            // מטרה אחרת ייכשל בדיוק אותו דבר, ולכן נופלים להתקפה הבסיסית
            // במקום לעמוד בלי לעשות כלום עד שהמאנה תחזור.
        }

        if (m_config.bFallbackBasicAttack)
        {
            if (!Hero->Movement)
            {
                return SimulateBasicAttack(m_iCurrentTarget);
            }
        }

        return 1;
    }

    ActionSkillType CMuHelper::SelectAttackSkill()
    {
        const size_t safeSize = std::min({m_config.aiSkill.size(), m_config.aiSkillCondition.size(), m_config.aiSkillInterval.size()});
        for (int i = 1; i < (int)safeSize; i++)
        {
            const int iSkillId = m_config.aiSkill[i];
            if (iSkillId <= 0 || iSkillId >= MAX_SKILLS)
            {
                continue;
            }

            if ((m_config.aiSkillCondition[i] & ON_TIMER)
                && m_config.aiSkillInterval[i] != 0
                && m_iSecondsElapsed > 0
                && m_iSecondsElapsed % m_config.aiSkillInterval[i] == 0)
            {
                return (ActionSkillType)iSkillId;
            }

            if (m_config.aiSkillCondition[i] & ON_CONDITION)
            {
                int iCount = 0;
                if (m_config.aiSkillCondition[i] & ON_MOBS_NEARBY)
                {
                    iCount = (int)m_setTargets.size();
                }
                else if (m_config.aiSkillCondition[i] & ON_MOBS_ATTACKING)
                {
                    iCount = (int)m_setTargetsAttacking.size();
                }
                else
                {
                    continue;
                }

                if (((m_config.aiSkillCondition[i] & ON_MORE_THAN_TWO_MOBS)   && iCount >= 2)
                    || ((m_config.aiSkillCondition[i] & ON_MORE_THAN_THREE_MOBS) && iCount >= 3)
                    || ((m_config.aiSkillCondition[i] & ON_MORE_THAN_FOUR_MOBS)  && iCount >= 4)
                    || ((m_config.aiSkillCondition[i] & ON_MORE_THAN_FIVE_MOBS)  && iCount >= 5))
                {
                    return (ActionSkillType)iSkillId;
                }
            }
        }

        if (m_config.aiSkill[0] > 0)
        {
            return (ActionSkillType)m_config.aiSkill[0];
        }

        return AT_SKILL_UNDEFINED;
    }

    int CMuHelper::SimulateComboAttack()
    {
        for (int i = 0; i < m_config.aiSkill.size(); i++)
        {
            if (m_config.aiSkill[i] == 0)
            {
                return 0;
            }
        }

        if (SimulateAttack((ActionSkillType)m_config.aiSkill[m_iComboState]))
        {
            m_iComboState = (m_iComboState + 1) % 3;
        }

        return 1;
    }

    // True while the hero is mid swing; gating helper actions on it makes the
    // bot's cadence follow AttackSpeed instead of the fixed helper timer, the
    // same way the manual click path gates in MoveHero (ZzzInterface.cpp).
    static bool IsHeroSwingInProgress()
    {
        const int iAction = Hero->Object.CurrentAction;

        // Outside the swing enum range entirely -> not a swing.
        if (!Engine::Object::IsAttackAction(iAction))
            return false;

        // Several non-swing *stance* animations (mounted idle/walk/run, two-hand-
        // sword stance, ride-horse, rage-fenrir) share the [PLAYER_ATTACK_FIST ..
        // PLAYER_RIDE_SKILL] enum range that IsAttackAction() spans. MoveHero
        // (ZzzInterface.cpp) OR-excludes exactly these four ranges when deciding
        // whether the hero may move; mirror that here. Otherwise a Fenrir-mounted
        // idle character (CurrentAction == PLAYER_FENRIR_STAND, inside the range)
        // reads as a perpetual swing, IsHeroSwingInProgress() never clears, and
        // SimulateSkill()/SimulateAttack() never fire -- the auto-helper is dead
        // for the whole session while Horn of Fenrir (or any mount) is equipped.
        if ((iAction >= PLAYER_STOP_TWO_HAND_SWORD_TWO && iAction <= PLAYER_RUN_TWO_HAND_SWORD_TWO)
            || (iAction >= PLAYER_DARKLORD_STAND && iAction <= PLAYER_RUN_RIDE_HORSE)
            || (iAction >= PLAYER_FENRIR_RUN && iAction <= PLAYER_FENRIR_WALK_ONE_LEFT)
            || (iAction >= PLAYER_RAGE_FENRIR_WALK && iAction <= PLAYER_RAGE_FENRIR_STAND_ONE_LEFT))
            return false;

        // Genuine attack/skill swing -> Fenrir attack/skill actions sit below
        // PLAYER_FENRIR_RUN, so they stay gated and cadence still tracks
        // AttackSpeed when mounted.
        return true;
    }

    int CMuHelper::SimulateAttack(ActionSkillType iSkill)
    {
        return SimulateSkill(iSkill, true, m_iCurrentTarget);
    }

    int CMuHelper::SimulateSkill(ActionSkillType iSkill, bool bTargetRequired, int iTarget)
    {
        // Let the current swing finish before issuing another action, so the
        // cadence tracks AttackSpeed instead of the fixed helper timer.
        if (IsHeroSwingInProgress())
        {
            return 0;
        }

        g_MovementSkill.m_iSkill = iSkill;
        g_MovementSkill.m_bMagic = true;

        const float fSkillDistance = gSkillManager.GetSkillDistance(iSkill, Hero);
        const bool bSelfPositionSkill = IsSelfPositionSkill(iSkill);

        if (bTargetRequired)
        {
            if (bSelfPositionSkill)
            {
                TargetX = Hero->PositionX;
                TargetY = Hero->PositionY;

                g_MovementSkill.m_iTarget = -1;

                // Check if current target is still valid (exists and alive)
                if (iTarget != -1)
                {
                    const int iCharIndex = FindCharacterIndex(iTarget);
                    if (iCharIndex != MAX_CHARACTERS_CLIENT)
                    {
                        CHARACTER* pCurrentTarget = &CharactersClient[iCharIndex];
                        if (pCurrentTarget->Dead > 0 || !IsMonster(pCurrentTarget))
                        {
                            DeleteTarget(iTarget);
                            return 0;
                        }
                    }
                    else
                    {
                        DeleteTarget(iTarget);
                        return 0;
                    }
                }
            }
            else
            {
                if (iTarget == -1)
                {
                    return 0;
                }

                const int iCharIndex = FindCharacterIndex(iTarget);
                if (iCharIndex == MAX_CHARACTERS_CLIENT)
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                SelectedCharacter = iCharIndex;

                CHARACTER* pTarget = &CharactersClient[iCharIndex];
                if (pTarget->Dead > 0)
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                g_MovementSkill.m_iTarget = iCharIndex;

                TargetX = (int)(pTarget->Object.Position[0] / TERRAIN_SCALE);
                TargetY = (int)(pTarget->Object.Position[1] / TERRAIN_SCALE);

                PATH_t tempPath;
                bool bHasPath = PathFinding2(Hero->PositionX, Hero->PositionY, TargetX, TargetY, &tempPath, m_iHuntingDistance + fSkillDistance);
                
                // Target not reachable, ignore it
                if (!bHasPath)
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                const bool bTargetNear = CheckTile(Hero, &Hero->Object, fSkillDistance);
                if (bTargetNear && !CheckWall(Hero->PositionX, Hero->PositionY, TargetX, TargetY))
                {
                    DeleteTarget(iTarget);
                    return 0;
                }

                // המטרה עוד לא בטווח הכישוף: ניגשים אליה בתוך רצועת העוגן.
                // חורגת מהרצועה — משחררים את התור למטרה קרובה יותר.
                if (!bTargetNear)
                {
                    if (!StepTowardTarget(tempPath))
                    {
                        m_iCurrentTarget = -1;
                    }
                    return 0;
                }
            }
        }
        else
        {
            TargetX = Hero->PositionX;
            TargetY = Hero->PositionY;
        }

        int iSkillResult = GameLogic::Combat::ExecuteSkill(Hero, iSkill, fSkillDistance);
        if (iSkillResult == -1 && iTarget != -1)
        {
            DeleteTarget(iTarget);
        }

        return (int)(iSkillResult == 1);
    }

    int CMuHelper::SimulateBasicAttack(int iTarget)
    {
        if (iTarget == -1)
        {
            return 0;
        }

        // Let the current swing finish before attacking again, so the cadence
        // tracks AttackSpeed instead of the fixed helper timer.
        if (IsHeroSwingInProgress())
        {
            return 0;
        }

        const int iCharIndex = FindCharacterIndex(iTarget);
        if (iCharIndex == MAX_CHARACTERS_CLIENT)
        {
            DeleteTarget(iTarget);
            return 0;
        }

        CHARACTER* pTarget = &CharactersClient[iCharIndex];
        if (pTarget->Dead > 0 || !IsMonster(pTarget))
        {
            DeleteTarget(iTarget);
            return 0;
        }

        constexpr float BASIC_RANGE_DEFAULT = 1.8f;
        constexpr float BASIC_RANGE_SPEAR = 2.2f;
        constexpr float BASIC_RANGE_BOW = 6.0f;

        float fRange = BASIC_RANGE_DEFAULT;
        const int iWeaponRight = CharacterMachine->Equipment[EQUIPMENT_WEAPON_RIGHT].Type;
        if (iWeaponRight >= ITEM_SPEAR && iWeaponRight < ITEM_SPEAR + MAX_ITEM_INDEX)
        {
            fRange = BASIC_RANGE_SPEAR;
        }
        if (gCharacterManager.GetEquipedBowType() != BOWTYPE_NONE)
        {
            fRange = BASIC_RANGE_BOW;
        }

        SelectedCharacter = iCharIndex;
        TargetX = (int)(pTarget->Object.Position[0] / TERRAIN_SCALE);
        TargetY = (int)(pTarget->Object.Position[1] / TERRAIN_SCALE);

        PATH_t tempPath;
        const bool bHasPath = PathFinding2(Hero->PositionX, Hero->PositionY, TargetX, TargetY, &tempPath, m_iHuntingDistance + fRange);
        if (!bHasPath)
        {
            DeleteTarget(iTarget);
            return 0;
        }

        const bool bTargetNear = CheckTile(Hero, &Hero->Object, fRange);
        if (bTargetNear && !CheckWall(Hero->PositionX, Hero->PositionY, TargetX, TargetY))
        {
            DeleteTarget(iTarget);
            return 0;
        }

        // גם ההתקפה הבסיסית ניגשת בתוך הרצועה — אותה התנהגות כמו ב-SimulateSkill.
        if (!bTargetNear)
        {
            if (!StepTowardTarget(tempPath))
            {
                m_iCurrentTarget = -1;
            }
            return 0;
        }

        if (gCharacterManager.GetEquipedBowType() != BOWTYPE_NONE && !CheckArrow())
        {
            return 0;
        }

        Hero->MovementType = MOVEMENT_ATTACK;
        ActionTarget = iCharIndex;
        Attacking = 1;
        Action(Hero, &Hero->Object, true);
        return 1;
    }

    int CMuHelper::Regroup()
    {
        if (m_config.bReturnToOriginalPosition && m_iSecondsAway > m_config.iMaxSecondsAway)
        {
            if (!SimulateMove(m_posOriginal))
            {
                return 0;
            }

            m_iSecondsAway = 0;
            m_iComboState = 0;
            m_iCurrentTarget = -1;
        }

        return 1;
    }

    // אין מה לתקוף: אם התרחקנו מהעוגן תוך כדי הציד, חוזרים אליו במקום
    // להישאר תקועים בקצה הרצועה. כך מרכז אזור הציד נשאר איפה שהשחקן
    // בחר להעמיד את הדמות, גם אחרי גל מפלצות שמשך אותה הצידה.
    // תקיפה במקום, בלי מטרה כלל. ‏SimulateSkill עם bTargetRequired=false
    // מכוון את הכישוף למיקום של הדמות עצמה — אותו מסלול שבו משתמשים
    // הבאפים וכישופי הנובה, ולכן הוא בדוק ואינו דורש מטרה נבחרת.
    // ההתקפה הבסיסית לא נכללת: היא מחייבת אינדקס מטרה ואי-אפשר לשחרר
    // אותה באוויר.
    int CMuHelper::AttackInPlace()
    {
        const ActionSkillType iSkill = SelectAttackSkill();
        if (iSkill <= AT_SKILL_UNDEFINED)
        {
            return 0;
        }

        m_iCurrentSkill = iSkill;
        return SimulateSkill(iSkill, false, -1);
    }

    // 🔴 שומר תקיעות. שלושה מקומות בעוזר ממתינים ל-`Hero->Movement`
    // שיתנקה, ואף אחד מהם לא מוותר על זה לעולם: StepTowardTarget מחזיר 1
    // ("בדרך") ומחזיק את המטרה, ReturnToAnchor מחזיר 0, והנפילה להתקפה
    // הבסיסית מותנית ב-`!Hero->Movement`. אם פקודת תנועה לא מסתיימת בפועל
    // — מסלול שנחסם, אי-התאמה מול השרת, או מפלצת שעומדת בדרך — הדמות
    // נשארת עומדת לנצח עם מטרה נעולה. ו-Regroup לא מציל, כי הוא רץ רק
    // כשאין מטרה. כאן המדידה היא תזוזה אמיתית על המפה, לא דגל.
    bool CMuHelper::BreakStallIfStuck()
    {
        const POINT posHero = { Hero->PositionX, Hero->PositionY };

        if (posHero.x != m_posLastSeen.x || posHero.y != m_posLastSeen.y)
        {
            m_posLastSeen = posHero;
            m_iStallTicks = 0;
            return false;
        }

        if (!Hero->Movement)
        {
            m_iStallTicks = 0;
            return false;
        }

        if (++m_iStallTicks < STALL_LIMIT_TICKS)
        {
            return false;
        }

        // משחררים את התנועה התקועה ואת המטרה שגררה אליה, כדי שהפעימה
        // הבאה תבחר מחדש — ואם אין מה לתקוף, תחזור לעוגן.
        Hero->Path.Lock.lock();
        Hero->Path.PathNum = 0;
        Hero->Path.CurrentPath = 0;
        Hero->Path.CurrentPathFloat = 0;
        Hero->Path.Lock.unlock();

        Hero->Movement = 0;
        SetPlayerStop(Hero);
        m_iCurrentTarget = -1;
        m_iComboState = 0;
        m_iStallTicks = 0;

        g_ConsoleDebug->Write(MCD_NORMAL, L"[MU Helper] Movement stalled. Releasing target.");
        return true;
    }

    int CMuHelper::ReturnToAnchor()
    {
        // "רק לתקוף": אם לא זזנו, אין לאן לחזור.
        if (m_config.bStayInPlace)
        {
            return 1;
        }

        const POINT posHero = { Hero->PositionX, Hero->PositionY };
        if (ComputeDistanceBetween(posHero, m_posOriginal) <= ANCHOR_TOLERANCE)
        {
            return 1;
        }

        if (Hero->Movement)
        {
            return 0;
        }

        return SimulateMove(m_posOriginal);
    }

    int CMuHelper::SimulateMove(POINT posMove)
    {
        Hero->MovementType = MOVEMENT_MOVE;
        TargetX = (int)posMove.x;
        TargetY = (int)posMove.y;

        if (!CheckTile(Hero, &Hero->Object, 1.5f))
        {
            if (PathFinding2((Hero->PositionX), (Hero->PositionY), TargetX, TargetY, &Hero->Path))
            {
                SendMove(Hero, &Hero->Object);
            }
            return 0;
        }

        return 1;
    }

    bool CMuHelper::HasAssignedBuffSkill()
    {
        for (int i = 0; i < m_config.aiBuff.size(); i++)
        {
            if (m_config.aiBuff[i] != 0)
            {
                return true;
            }
        }

        return false;
    }

    ActionSkillType CMuHelper::GetHealingSkill()
    {
        std::vector<ActionSkillType> aiHealingSkills =
        {
            AT_SKILL_HEALING,
            AT_SKILL_HEALING_STR,
        };

        for (int i = 0; i < aiHealingSkills.size(); i++)
        {
            int iSkillIndex = g_pSkillList->GetSkillIndex(aiHealingSkills[i]);
            if (iSkillIndex != -1)
            {
                return aiHealingSkills[i];
            }
        }

        return AT_SKILL_UNDEFINED;
    }

    // Matches AttackWizard() behavior in ZzzInterface.cpp for these skill IDs.
    bool CMuHelper::IsSelfPositionSkill(ActionSkillType iSkill)
    {
        return (
            iSkill == AT_SKILL_NOVA_BEGIN ||
            iSkill == AT_SKILL_NOVA ||
            iSkill == AT_SKILL_HELL_FIRE ||
            iSkill == AT_SKILL_HELL_FIRE_STR ||
            iSkill == AT_SKILL_INFERNO ||
            iSkill == AT_SKILL_INFERNO_STR ||
            iSkill == AT_SKILL_INFERNO_STR_MG
        );
    }

    ActionSkillType CMuHelper::GetDrainLifeSkill()
    {
        std::vector<ActionSkillType> aiDrainLifeSkills =
        {
            AT_SKILL_ALICE_DRAINLIFE,
            AT_SKILL_ALICE_DRAINLIFE_STR
        };

        for (int i = 0; i < aiDrainLifeSkills.size(); i++)
        {
            int iSkillIndex = g_pSkillList->GetSkillIndex(aiDrainLifeSkills[i]);
            if (iSkillIndex != -1)
            {
                return aiDrainLifeSkills[i];
            }
        }

        return AT_SKILL_UNDEFINED;
    }

    int CMuHelper::ObtainItem()
    {
        if (m_iCurrentItem == MAX_ITEMS)
        {
            m_iCurrentItem = SelectItemToObtain();
            if (m_iCurrentItem == MAX_ITEMS)
            {
                return 1;
            }
        }

        ITEM_t* pDrop = &Items[m_iCurrentItem];

        if (!pDrop->Object.Live)
        {
            DeleteItem(m_iCurrentItem);
            return 1;
        }

        TargetX = (int)(Items[m_iCurrentItem].Object.Position[0] / TERRAIN_SCALE);
        TargetY = (int)(Items[m_iCurrentItem].Object.Position[1] / TERRAIN_SCALE);

        int iDistance = ComputeDistanceBetween({ Hero->PositionX, Hero->PositionY }, { TargetX, TargetY });
        if (iDistance <= m_iObtainingDistance)
        {
            if (!CheckTile(Hero, &Hero->Object, 2.0f))
            {
                // "רק לתקוף": לא הולכים גם אל שלל. פריט שנפל מחוץ להישג יד
                // משוחרר כדי שלא יחסום את התור לפריט שכן צמוד לדמות.
                if (m_config.bStayInPlace)
                {
                    DeleteItem(m_iCurrentItem);
                    return 1;
                }

                // 🔴 ההליכה אל השלל הייתה הדבר היחיד בעוזר שלא כפוף
                // לרצועה. הבחירה כן מסוננת, אבל לפי אזור הציד (רצועה + טווח),
                // ולכן הדמות הלכה עד קצהו — ומשם ערימה חדשה נכנסה לטווח
                // האיסוף שלה ומשכה אותה הלאה. עם דרופ של 20% הרצפה מלאה תמיד,
                // וזה נראה כמו דמות שמסתובבת סתם במפה. פריט מעבר לרצועה
                // משוחרר במקום לגרור אליו.
                if (!IsWithinLeash({ TargetX, TargetY }))
                {
                    DeleteItem(m_iCurrentItem);
                    return 1;
                }

                if (PathFinding2((Hero->PositionX), (Hero->PositionY), TargetX, TargetY, &Hero->Path))
                {
                    SendMove(Hero, &Hero->Object);
                }

                return 0;
            }
            else
            {
                if (SendGetItem == -1)
                {
                    SendGetItem = m_iCurrentItem;
                    SocketClient->ToGameServer()->SendPickupItemRequest(m_iCurrentItem);
                    DeleteItem(m_iCurrentItem);
                }
            }
        }

        return 1;
    }

    bool CMuHelper::ShouldObtainItem(int iItemId)
    {
        // "לא סומן כלום" = לא לאסוף כלום, וזו עמידה-ותקיפה בלבד.
        //
        // ב-16/08/2026 זה התפרש כאן הפוך — "לא כיוונתי, אז תאסוף הכול" —
        // כדי לפתור חשבון שהגיע עם הגדרות ריקות מהשרת ולא אסף דבר. המחיר
        // התגלה ב-20/08: **אי-אפשר היה לבקש מהעוזר לא לאסוף**, והוא נטש
        // כל קרב בשביל ערימת זן. החלון מציע במפורש "לאסוף הכול" מול
        // "לאסוף נבחרים", ולכן לשחקן יש דרך ברורה להביע את הכוונה ההפוכה,
        // ואין סיבה לנחש במקומו.
        ITEM_t* pDrop = &Items[iItemId];
        ITEM* pItem = &pDrop->Item;

        if ((m_config.bPickZen && IsMoneyItem(pItem))
            || (m_config.bPickJewel && IsJewelItem(pItem))
            || (m_config.bPickAncient && IsAncientItem(pItem))
            || (m_config.bPickExcellent && IsExcellentItem(pItem)))
        {
            return true;
        }

        if (m_config.bPickExtraItems)
        {
            std::wstring strDisplayName = GetItemDisplayName(pItem);

            for (const auto& str : m_config.aExtraItems)
            {
                // Check if the search keyword is in the item's display name
                if (strDisplayName.find(str) != std::wstring::npos)
                {
                    return true;
                }
            }
        }

        return m_config.bPickAllItems;
    }

    void CMuHelper::AddItem(int iItemId, POINT posWhere)
    {
        _itemsLock.lock();
        m_setItems.insert(iItemId);
        _itemsLock.unlock();
    }

    void CMuHelper::DeleteItem(int iItemId)
    {
        _itemsLock.lock();
        m_setItems.erase(iItemId);
        _itemsLock.unlock();

        if (iItemId == m_iCurrentItem)
        {
            m_iCurrentItem = MAX_ITEMS;
        }
    }

    int CMuHelper::SelectItemToObtain()
    {
        int iClosestItemId = MAX_ITEMS;
        // m_iObtainingDistance (המרחק המחושב) ולא הערך הגולמי מההגדרות —
        // ObtainItem() משווה מול המחושב, וערך גולמי כאן צמצם את רדיוס
        // הבחירה בפועל (תיקון 4 מאבחון 16/08/2026).
        int iMinDistance = m_iObtainingDistance;

        std::set<int> setItems;
        {
            _itemsLock.lock();
            setItems = m_setItems;
            _itemsLock.unlock();
        }

        for (const int& iItemId : setItems)
        {
            if (!ShouldObtainItem(iItemId))
            {
                continue;
            }

            int iItemX = (int)(Items[iItemId].Object.Position[0] / TERRAIN_SCALE);
            int iItemY = (int)(Items[iItemId].Object.Position[1] / TERRAIN_SCALE);

            // פריט שנפל מחוץ לאזור הציד לא שווה נדידה מהעוגן. הסינון כאן
            // ולא ב-ObtainItem בכוונה: פריט שנבחר ואי־אפשר להגיע אליו חוסם
            // את כל האיסוף עד שהוא נעלם.
            if (!IsWithinHuntingArea({ iItemX, iItemY }))
            {
                continue;
            }

            int iDistance = ComputeDistanceBetween({ Hero->PositionX, Hero->PositionY }, { iItemX, iItemY });
            if (iDistance <= iMinDistance)
            {
                iMinDistance = iDistance;
                iClosestItemId = iItemId;
            }
        }

        return iClosestItemId;
    }
}
