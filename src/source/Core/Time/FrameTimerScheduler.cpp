#include "stdafx.h"
#include "Core/Time/FrameTimerScheduler.h"

#include <chrono>
#include <vector>

namespace Core::Time
{
    FrameTimerScheduler& FrameTimerScheduler::Instance()
    {
        // בכוונה דולף ולעולם לא נהרס: הרסני גלובלים (למשל ~CSlideHelpMgr)
        // קוראים ל-Kill() בזמן הרס הסטטיים, ומופע סטטי רגיל עלול להיהרס
        // לפניהם — וכל יציאה מהמשחק קרסה ב-0xc0000005 בתוך מפת הטיימרים
        // (נמדד 21/08/2026, אותו offset בשלוש בניות שונות). דליפה של אובייקט
        // אחד ברגע היציאה היא המחיר של סדר הרס שאי אפשר להבטיח.
        static FrameTimerScheduler* instance = new FrameTimerScheduler();
        return *instance;
    }

    std::uint64_t FrameTimerScheduler::NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    void FrameTimerScheduler::SetRepeating(TimerId id, unsigned intervalMs, Callback callback)
    {
        m_timers[id] = Timer{ intervalMs, NowMs() + intervalMs, std::move(callback) };
    }

    void FrameTimerScheduler::Kill(TimerId id)
    {
        m_timers.erase(id);
    }

    void FrameTimerScheduler::Tick()
    {
        const std::uint64_t now = NowMs();

        // Collect the due ids first, then fire. A callback may register or kill
        // timers (e.g. a buff timer kills itself on expiry), so we must not hold
        // an iterator into m_timers across a callback.
        std::vector<TimerId> due;
        for (const auto& [id, timer] : m_timers)
        {
            if (now >= timer.nextDueMs)
            {
                due.push_back(id);
            }
        }

        for (TimerId id : due)
        {
            auto it = m_timers.find(id);
            if (it == m_timers.end())
            {
                continue; // killed by an earlier callback this tick
            }

            // Reschedule before firing so a callback that re-registers or kills
            // this id wins over the reschedule. No catch-up: the next due time is
            // measured from now, matching WM_TIMER coalescing.
            it->second.nextDueMs = now + it->second.intervalMs;

            Callback callback = it->second.callback;
            callback();
        }
    }
}
