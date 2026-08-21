// TextDirection.h: which way the active UI language reads.
//
// Windows that were laid out for Hebrew align their text to the right and grow
// their progress bars leftwards. That is correct for Hebrew and wrong for every
// other language the client ships, so the alignment has to follow the locale
// rather than be baked into the layout code.
//
// Locale-only on purpose: this header knows nothing about the render layer, so
// it can be included from UI, HUD and dialog code alike. Callers map the answer
// onto whatever their own alignment constants are (RT3_SORT_RIGHT, ...).

#pragma once

namespace Core::Localization
{
    // True when the active I18N locale is written right-to-left.
    //
    // Reads I18N::GetCurrentLocale(), so it follows a live language switch
    // without any cached state of its own.
    bool IsRightToLeft() noexcept;
}
