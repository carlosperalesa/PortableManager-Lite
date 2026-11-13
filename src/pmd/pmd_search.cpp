#include "pmd/pmd_search.h"
#include "applications/app_paths.h"

#include <cmath>
#include <cwchar>
#include <cwctype>

namespace pm::pmd {

bool Calculator::Evaluate(double& result) {
    if (input_.size() > 128) return false;
    result = Expression();
    Trim();
    return valid_ && input_.empty() && std::isfinite(result);
}

void Calculator::Trim() {
    while (!input_.empty() && input_.front() == L' ') input_.remove_prefix(1);
}

bool Calculator::Take(wchar_t token) {
    Trim();
    if (input_.empty() || input_.front() != token) return false;
    input_.remove_prefix(1);
    return true;
}

double Calculator::Expression() {
    double value = Term();
    for (;;) {
        if (Take(L'+')) value += Term();
        else if (Take(L'-')) value -= Term();
        else return value;
    }
}

double Calculator::Term() {
    double value = Atom();
    for (;;) {
        if (Take(L'*')) value *= Atom();
        else if (Take(L'/')) {
            const double divisor = Atom();
            if (divisor == 0) valid_ = false;
            else value /= divisor;
        } else return value;
    }
}

double Calculator::Atom() {
    if (Take(L'-')) return -Atom();
    if (Take(L'+')) return Atom();
    if (Take(L'(')) {
        const auto value = Expression();
        if (!Take(L')')) valid_ = false;
        return value;
    }
    Trim();
    const std::wstring literal(input_.substr(0, input_.find_first_not_of(L"0123456789.")));
    wchar_t* end = nullptr;
    const double result = std::wcstod(literal.c_str(), &end);
    if (literal.empty() || end != literal.c_str() + literal.size()) valid_ = false;
    input_.remove_prefix(literal.size());
    return result;
}

bool PrefixNoCase(std::wstring_view name, std::wstring_view query) {
    return name.size() >= query.size() && applications::Equal(name.substr(0, query.size()), query);
}

std::vector<std::wstring> SplitTrimmed(std::wstring_view text, wchar_t delimiter) {
    std::vector<std::wstring> result;
    while (!text.empty()) {
        const auto end = text.find(delimiter);
        auto word = text.substr(0, end);
        const auto start = word.find_first_not_of(L" \t");
        if (start != std::wstring_view::npos)
            result.emplace_back(word.substr(start, word.find_last_not_of(L" \t") - start + 1));
        if (end == std::wstring_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return result;
}

bool IsHttpUrl(std::wstring_view text) {
    if (text.size() <= 8) return false;
    if (!text.starts_with(L"https://") && !text.starts_with(L"http://")) return false;
    for (const wchar_t ch : text) {
        if (ch <= L' ' || ch == L'"' || ch == L'\r' || ch == L'\n') return false;
    }
    const auto sep = text.find(L"://");
    return sep != std::wstring_view::npos && sep + 3 < text.size();
}

std::wstring SanitizeUrlName(std::wstring_view raw) {
    std::wstring out;
    for (const wchar_t ch : raw) {
        if (ch == L'[' || ch == L']' || ch == L'=' || ch == L'\r' || ch == L'\n') continue;
        if (ch == L' ' || ch == L'\t') {
            if (!out.empty() && out.back() != L'-') out.push_back(L'-');
        } else {
            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
        }
    }
    while (!out.empty() && out.back() == L'-') out.pop_back();
    return out;
}

std::wstring SuggestedUrlName(std::wstring_view url) {
    const auto sep = url.find(L"://");
    if (sep == std::wstring_view::npos) return L"web";
    auto host = url.substr(sep + 3);
    const auto slash = host.find_first_of(L"/?#:");
    if (slash != std::wstring_view::npos) host = host.substr(0, slash);
    if (host.starts_with(L"www.")) host.remove_prefix(4);
    const auto dot = host.find(L'.');
    if (dot != std::wstring_view::npos && dot > 0) host = host.substr(0, dot);
    auto cleaned = SanitizeUrlName(host);
    return cleaned.empty() ? L"web" : cleaned;
}

} // namespace pm::pmd
