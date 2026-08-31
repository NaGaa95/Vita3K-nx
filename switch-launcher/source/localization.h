#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace LauncherLocalization
{
struct Language
{
  const char* code;
  const char* name;
};

void SetLanguage(std::string preference);
std::string_view Translate(std::string_view source);
const std::string& Preference();
std::string_view CurrentLanguage();
std::string DisplayName();
const std::vector<Language>& Languages();
int FindLanguage(std::string_view code);
}
