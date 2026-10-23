#pragma once
#include <string>
#include <string_view>

namespace desktopball {
enum class Language { chinese, english };
Language parseLanguage(std::wstring_view code);
const wchar_t* languageCode(Language language);
const wchar_t* menuFontFamily(Language language);
std::wstring menuText(Language language, std::wstring_view text);
}
