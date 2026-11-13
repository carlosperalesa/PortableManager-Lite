#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pm::pmd {

class Calculator {
public:
    explicit Calculator(std::wstring_view input) : input_(input) {}
    bool Evaluate(double& result);

private:
    void Trim();
    bool Take(wchar_t token);
    double Expression();
    double Term();
    double Atom();

    std::wstring_view input_;
    bool valid_ = true;
};

bool PrefixNoCase(std::wstring_view name, std::wstring_view query);
std::vector<std::wstring> SplitTrimmed(std::wstring_view text, wchar_t delimiter);
bool IsHttpUrl(std::wstring_view text);
std::wstring SanitizeUrlName(std::wstring_view raw);
std::wstring SuggestedUrlName(std::wstring_view url);

} // namespace pm::pmd
