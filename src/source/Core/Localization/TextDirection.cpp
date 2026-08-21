#include "stdafx.h"
#include "Core/Localization/TextDirection.h"

#include "I18N/All.h"

#include <cstring>

namespace
{
    // The right-to-left languages the client can actually be switched to.
    // Hebrew is the only one today; Arabic and Persian are listed so that
    // adding their .resx files is the whole job, not a hunt for this array.
    const char* const k_RightToLeftLocales[] = { "he", "ar", "fa" };

    // Locale codes may carry a region ("he-IL"), so compare the language
    // subtag only - the part before the first hyphen.
    bool LanguageMatches(const char* locale, const char* language) noexcept
    {
        const size_t length = std::strlen(language);
        if (std::strncmp(locale, language, length) != 0)
            return false;

        return locale[length] == '\0' || locale[length] == '-';
    }
}

namespace Core::Localization
{
    bool IsRightToLeft() noexcept
    {
        const char* locale = I18N::GetCurrentLocale();
        if (locale == nullptr)
            return false;

        for (const char* rtl : k_RightToLeftLocales)
        {
            if (LanguageMatches(locale, rtl))
                return true;
        }

        return false;
    }
}
