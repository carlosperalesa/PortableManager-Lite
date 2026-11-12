#include "pmd/pmd_module.h"
#include "applications/app_paths.h"
#include "applications/session_host.h"
#include "core/config_manager.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <thread>

namespace pm::pmd {
namespace {
namespace fs = std::filesystem;
using applications::App;
using applications::Equal;
using cleanup::Mode;
using core::PathResolver;

constexpr wchar_t kWindowClass[] = L"PML_PortableManagerDesktop";
constexpr UINT kFinished = WM_APP + 1;
constexpr UINT kOperation = WM_APP + 2;
constexpr UINT kOpen = WM_APP + 3;
constexpr UINT kGuardChanged = WM_APP + 4;
enum class View { Pill, Search, Files };
enum class Kind { App, File, Folder };
struct Item {
    Kind kind;
    std::wstring label;
    std::wstring value;
    std::wstring args;
};
struct Worker {
    App app;
    applications::StopRequest stop;
    applications::Handle event;
    std::thread thread;
    applications::RunResult result;
    bool uninstall_after_close = false;
};

std::wstring Text(HWND control) {
    std::wstring text(static_cast<std::size_t>(::GetWindowTextLengthW(control)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(::GetWindowTextW(control, text.data(), static_cast<int>(text.size()))));
    return text;
}

bool Prefix(std::wstring_view name, std::wstring_view query) {
    return name.size() >= query.size() && Equal(name.substr(0, query.size()), query);
}

std::vector<std::wstring> Split(std::wstring_view text, wchar_t delimiter) {
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

bool IsPackage(const fs::path& path) {
    const auto name = path.filename().native();
    const auto ext = path.extension().native();
    return (name.size() > 8 && Equal(name.substr(name.size() - 8), L".paf.exe")) ||
        Equal(ext, L".zip") || Equal(ext, L".rar") || Equal(ext, L".7z") || Equal(ext, L".tar");
}

std::wstring SuggestedId(const fs::path& path) {
    std::wstring result;
    const auto stem = path.stem().native();
    for (auto ch : stem) {
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')) result.push_back(ch);
        else if (!result.empty()) break;
        if (result.size() == 50) break;
    }
    return applications::SafeId(result) ? result : L"portable";
}

class Calculator {
public:
    explicit Calculator(std::wstring_view input) : input_(input) {}

    bool Evaluate(double& result) {
        if (input_.size() > 128) return false;
        result = Expression();
        Trim();
        return valid_ && input_.empty() && std::isfinite(result);
    }

private:
    void Trim() {
        while (!input_.empty() && input_.front() == L' ') input_.remove_prefix(1);
    }

    bool Take(wchar_t token) {
        Trim();
        if (input_.empty() || input_.front() != token) return false;
        input_.remove_prefix(1);
        return true;
    }

    double Expression() {
        double value = Term();
        for (;;) {
            if (Take(L'+')) value += Term();
            else if (Take(L'-')) value -= Term();
            else return value;
        }
    }

    double Term() {
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

    double Atom() {
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

    std::wstring_view input_;
    bool valid_ = true;
};

class Desktop {
public:
    explicit Desktop(const PathResolver& resolver) : resolver_(resolver), config_(resolver) {}
    int Run();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Message(UINT msg, WPARAM wp, LPARAM lp);
    void Show(View view);
    void Layout();
    void Populate();
    void Open(int index);
    void Back();
    void BrowseDocument(const Item& item);
    void SearchWeb(bool fallback);
    void Launch(const App& app, std::wstring arguments);
    void Close(Mode mode);
    void CloseWhenReady();
    void Finish(Worker* worker);
    void UpdateClipboard();
    void PickPackage(std::wstring id = {});
    void InstallPackage(bool choose_destination);
    void StartInstall(std::wstring id, std::wstring package, std::wstring executable = {});
    bool ChooseExecutable();
    void Menu(bool application, int index = -1);
    void StartOperation(std::function<DWORD()> operation);
    void Report(DWORD error);

    const PathResolver& resolver_;
    core::ConfigManager config_;
    applications::SessionHost session_;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND list_ = nullptr;
    HWND install_ = nullptr;
    HWND web_ = nullptr;
    WNDPROC edit_proc_ = nullptr;
    WNDPROC list_proc_ = nullptr;
    View view_ = View::Pill;
    std::wstring folder_ = L"Other";
    std::wstring origin_;
    std::wstring package_;
    std::vector<App> apps_;
    std::vector<Item> items_;
    std::vector<std::unique_ptr<Worker>> workers_;
    struct InstallSelection {
        std::wstring id;
        std::wstring package;
        std::vector<std::wstring> choices;
    };
    std::thread operation_;
    std::shared_ptr<InstallSelection> selection_;
    bool busy_ = false;
    bool closing_ = false;
    Mode close_mode_ = Mode::Normal;
    bool modal_ = false;
    bool error_ = false;
    int status_ = 0;
};

int Desktop::Run() {
    origin_ = PathResolver::QueryExecutablePath();
    const auto temp = runtime::SessionManager::QuerySystemTempDir();
    if (temp.empty() || !::SetCurrentDirectoryW(temp.c_str())) return 1;
    config_.LoadConfigIni(false);
    const auto instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassW(&wc)) return 1;
    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kWindowClass, L"PortableManager Desktop", WS_POPUP, 0, 0, 96, 28,
        nullptr, nullptr, instance, this);
    if (!hwnd_) return 1;
    const DWORD started = session_.Start(origin_, [window = hwnd_] {
        ::PostMessageW(window, kGuardChanged, 0, 0);
    });
    if (started) {
        session_.Close(Mode::Critical);
        ::DestroyWindow(hwnd_);
        return 1;
    }
    const auto key = config_.config_doc().GetValue(L"Desktop", L"Hotkey", L"Ctrl+Space");
    const auto tokens = Split(key, L'+');
    UINT modifiers = MOD_NOREPEAT;
    for (const auto& token : tokens) {
        if (Equal(token, L"Ctrl")) modifiers |= MOD_CONTROL;
        if (Equal(token, L"Alt")) modifiers |= MOD_ALT;
        if (Equal(token, L"Shift")) modifiers |= MOD_SHIFT;
        if (Equal(token, L"Win")) modifiers |= MOD_WIN;
    }
    const auto last = tokens.empty() ? std::wstring(L"Space") : tokens.back();
    const auto vk = last.size() == 1 ? static_cast<UINT>(::VkKeyScanW(last.front()) & 0xff) : VK_SPACE;
    ::RegisterHotKey(hwnd_, 1, modifiers, vk);
    ::AddClipboardFormatListener(hwnd_);
    ::SetTimer(hwnd_, 1, 500, nullptr);
    UpdateClipboard();
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return session_.Close(close_mode_) ? 1 : status_;
}

LRESULT CALLBACK Desktop::WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = static_cast<Desktop*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Desktop*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Message(msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Desktop::InputProc(HWND control, UINT msg, WPARAM wp, LPARAM lp) {
    const auto parent = ::GetParent(control);
    auto* self = reinterpret_cast<Desktop*>(::GetWindowLongPtrW(parent, GWLP_USERDATA));
    const bool edit = control == self->edit_;
    const auto base = edit ? self->edit_proc_ : self->list_proc_;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { self->Back(); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        const auto value = Text(self->edit_);
        if (edit && Equal(value, L"exit")) self->Close(Mode::Normal);
        else if (edit && value.starts_with(L"=")) {
            double result = 0;
            if (!Calculator(std::wstring_view(value).substr(1)).Evaluate(result)) self->Report(ERROR_INVALID_DATA);
            else {
                wchar_t display[64]{};
                std::swprintf(display, std::size(display), L"%.15g", result);
                ::SetWindowTextW(control, display);
                ::SendMessageW(control, EM_SETSEL, 0, -1);
            }
        } else {
            const auto index = static_cast<int>(::SendMessageW(self->list_, LB_GETCURSEL, 0, 0));
            if (index == LB_ERR && edit) self->SearchWeb(true);
            else self->Open(index);
        }
        return 0;
    }
    if (edit && msg == WM_CHAR && wp == L' ' && Text(control).empty()) {
        self->Show(View::Files);
        return 0;
    }
    if (edit && msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN)) return 0;
    if (edit && msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN) && !self->items_.empty()) {
        int selection = static_cast<int>(::SendMessageW(self->list_, LB_GETCURSEL, 0, 0));
        selection = std::clamp(selection + (wp == VK_UP ? -1 : 1), 0, static_cast<int>(self->items_.size() - 1));
        ::SendMessageW(self->list_, LB_SETCURSEL, selection, 0);
        return 0;
    }
    const auto result = ::CallWindowProcW(base, control, msg, wp, lp);
    if (!edit && (msg == WM_RBUTTONUP || msg == WM_LBUTTONUP)) {
        const auto item = ::SendMessageW(control, LB_ITEMFROMPOINT, 0, lp);
        if (!HIWORD(item)) {
            if (msg == WM_RBUTTONUP) self->Menu(true, LOWORD(item));
            else ::PostMessageW(parent, kOpen, LOWORD(item), 0);
        }
    }
    return result;
}

LRESULT Desktop::Message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const auto make = [&](const wchar_t* type, const wchar_t* title, DWORD style, INT_PTR id) {
                const auto child = ::CreateWindowExW(0, type, title, WS_CHILD | style, 0, 0, 0, 0, hwnd_,
                    reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr), nullptr);
                if (child) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);
                return child;
            };
            edit_ = make(L"EDIT", L"", ES_AUTOHSCROLL, 1);
            list_ = make(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 2);
            install_ = make(L"BUTTON", L"", BS_PUSHBUTTON, 3);
            web_ = make(L"BUTTON", L"\u2197", BS_PUSHBUTTON, 4);
            if (!edit_ || !list_ || !install_ || !web_) return -1;
            edit_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            list_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(list_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const auto dc = ::BeginPaint(hwnd_, &ps);
            RECT rect{};
            ::GetClientRect(hwnd_, &rect);
            ::FillRect(dc, &rect, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
            if (view_ == View::Pill) {
                const auto color = (error_ || !session_.protected_by_guard()) ? RGB(238, 81, 84) : closing_ || busy_ ? RGB(247, 185, 65) : RGB(92, 223, 164);
                const auto led = ::CreateSolidBrush(color);
                const auto brush = ::SelectObject(dc, led);
                const auto pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
                ::Ellipse(dc, rect.right - 24, 9, rect.right - 14, 19);
                ::SelectObject(dc, pen);
                ::SelectObject(dc, brush);
                ::DeleteObject(led);
            } else {
                const int x = rect.right - 19;
                const POINT shield[] = {{x - 8, 7}, {x, 4}, {x + 8, 7}, {x + 7, 16},
                                        {x + 4, 21}, {x, 24}, {x - 4, 21}, {x - 7, 16}};
                const auto fill = ::CreateSolidBrush(session_.protected_by_guard() ?
                    RGB(92, 223, 164) : RGB(238, 81, 84));
                const auto brush = ::SelectObject(dc, fill);
                const auto pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
                ::Polygon(dc, shield, static_cast<int>(std::size(shield)));
                ::SelectObject(dc, pen);
                ::SelectObject(dc, brush);
                ::DeleteObject(fill);
            }
            ::EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = 26;
            return TRUE;
        case WM_DRAWITEM: {
            const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (draw.itemID >= items_.size()) return TRUE;
            const auto& item = items_[draw.itemID];
            const auto bg = ::CreateSolidBrush(draw.itemState & ODS_SELECTED ? RGB(57, 86, 121) : RGB(33, 40, 51));
            ::FillRect(draw.hDC, &draw.rcItem, bg);
            ::DeleteObject(bg);
            ::SetBkMode(draw.hDC, TRANSPARENT);
            ::SetTextColor(draw.hDC, RGB(239, 243, 248));
            auto label = draw.rcItem;
            label.left += 31;
            SHFILEINFOW icon{};
            auto path = item.value;
            if (item.kind == Kind::App) {
                const auto app = std::find_if(apps_.begin(), apps_.end(), [&](const App& a) { return a.id == item.value; });
                path = app == apps_.end() ? L"" : app->executable;
                if (app != apps_.end()) {
                    const auto color = Equal(app->color, L"Green") ? RGB(107, 224, 166) :
                        Equal(app->color, L"Yellow") ? RGB(244, 203, 86) :
                        Equal(app->color, L"Red") ? RGB(245, 115, 121) : RGB(196, 201, 209);
                    ::SetTextColor(draw.hDC, color);
                }
            }
            const auto full = resolver_.ResolvePmRelativePath(path);
            if (full.ok && item.kind == Kind::App)
                ::ExtractIconExW(full.resolved_path.c_str(), 0, nullptr, &icon.hIcon, 1);
            else if (full.ok)
                ::SHGetFileInfoW(full.resolved_path.c_str(), item.kind == Kind::Folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                    &icon, sizeof(icon), SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
            if (icon.hIcon) {
                ::DrawIconEx(draw.hDC, label.left - 23, label.top + 5, icon.hIcon, 16, 16, 0, nullptr, DI_NORMAL);
                ::DestroyIcon(icon.hIcon);
            }
            ::DrawTextW(draw.hDC, item.label.c_str(), -1, &label, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == 1 && HIWORD(wp) == EN_CHANGE && view_ == View::Search) Populate();
            if (LOWORD(wp) == 3 && HIWORD(wp) == BN_CLICKED) InstallPackage(false);
            if (LOWORD(wp) == 4 && HIWORD(wp) == BN_CLICKED) SearchWeb(false);
            return 0;
        case WM_HOTKEY: Show(View::Search); return 0;
        case WM_LBUTTONUP: if (view_ == View::Pill) Show(View::Search); return 0;
        case WM_RBUTTONUP: Menu(false); return 0;
        case WM_KEYDOWN: if (wp == VK_SPACE) Show(View::Files); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !modal_ && view_ != View::Pill) Show(View::Pill);
            return 0;
        case WM_DISPLAYCHANGE: Layout(); return 0;
        case WM_CLIPBOARDUPDATE: UpdateClipboard(); return 0;
        case WM_TIMER:
            if (!closing_ && !guard::IsOriginAccessible(origin_)) Close(Mode::Critical);
            return 0;
        case WM_CLOSE: Close(Mode::Normal); return 0;
        case WM_QUERYENDSESSION: Close(Mode::Critical); return TRUE;
        case kGuardChanged: ::InvalidateRect(hwnd_, nullptr, TRUE); return 0;
        case kOpen: Open(static_cast<int>(wp)); return 0;
        case kFinished: Finish(reinterpret_cast<Worker*>(lp)); return 0;
        case kOperation:
            operation_.join();
            busy_ = false;
            if (wp == ERROR_MORE_DATA && !closing_ && ChooseExecutable()) return 0;
            selection_.reset();
            Report(static_cast<DWORD>(wp));
            if (!closing_) Populate();
            CloseWhenReady();
            return 0;
        case WM_DESTROY:
            ::UnregisterHotKey(hwnd_, 1);
            ::RemoveClipboardFormatListener(hwnd_);
            ::KillTimer(hwnd_, 1);
            ::PostQuitMessage(status_);
            return 0;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

void Desktop::Layout() {
    POINT cursor{};
    ::GetCursorPos(&cursor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
    const auto& area = monitor.rcWork;
    const int rows = std::clamp(static_cast<int>(items_.size()), 1, std::max(1, (static_cast<int>(area.bottom - area.top) - 65) / 26));
    const bool offer = view_ == View::Pill && !package_.empty() && !closing_ && !busy_;
    const int width = std::min(view_ == View::Pill ? (offer ? 238 : 92) : 326, static_cast<int>(area.right - area.left));
    const int top = view_ == View::Search ? 34 : view_ == View::Files ? 27 : 0;
    const int height = view_ == View::Pill ? 28 : top + rows * 26 + 8;
    const auto position = [&](const wchar_t* key, LONG fallback, LONG low, LONG high) {
        const auto value = config_.config_doc().GetValue(L"Desktop", key);
        wchar_t* end = nullptr;
        const LONG parsed = std::wcstol(value.c_str(), &end, 10);
        return std::clamp(!value.empty() && end == value.c_str() + value.size() ? parsed : fallback,
                          low, std::max(low, high));
    };
    const auto x = position(L"X", area.right - width - 16, area.left, area.right - width);
    const auto y = position(L"Y", area.top + 12, area.top, area.bottom - height);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    const auto region = ::CreateRoundRectRgn(0, 0, width + 1, height + 1, view_ == View::Pill ? 28 : 12, view_ == View::Pill ? 28 : 12);
    if (region && !::SetWindowRgn(hwnd_, region, TRUE)) ::DeleteObject(region);
    ::SetLayeredWindowAttributes(hwnd_, 0, view_ == View::Files ? 224 : 255, LWA_ALPHA);
    ::MoveWindow(edit_, 7, 5, width - 79, 24, TRUE);
    ::MoveWindow(web_, width - 67, 5, 31, 24, TRUE);
    ::MoveWindow(list_, 5, top + 4, width - 10, rows * 26, TRUE);
    ::MoveWindow(install_, 10, 3, width - 45, 22, TRUE);
    ::ShowWindow(edit_, view_ == View::Search ? SW_SHOW : SW_HIDE);
    ::ShowWindow(web_, view_ == View::Search && !Text(edit_).empty() ? SW_SHOW : SW_HIDE);
    ::ShowWindow(list_, view_ == View::Pill ? SW_HIDE : SW_SHOW);
    ::ShowWindow(install_, offer ? SW_SHOW : SW_HIDE);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void Desktop::Show(View view) {
    if (closing_) return;
    view_ = view;
    if (view == View::Pill) ::SetWindowTextW(edit_, L"");
    if (view == View::Files) folder_ = L"Other";
    if (view != View::Pill) ::SetForegroundWindow(hwnd_);
    Populate();
    if (view == View::Search) ::SetFocus(edit_);
    if (view == View::Files) ::SetFocus(list_);
}

void Desktop::Populate() {
    items_.clear();
    ::SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    if (view_ == View::Pill) { Layout(); return; }
    applications::List(resolver_, apps_);
    const auto typed = view_ == View::Search ? Text(edit_) : L"";
    if (view_ == View::Search || folder_ == L"Other") {
        std::vector<Item> partial;
        const auto& settings = config_.config_doc();
        for (const auto& app : apps_) {
            if (app.archived && view_ == View::Files) continue;
            auto aliases = Split(app.aliases, L',');
            aliases.push_back(app.id);
            aliases.push_back(app.name);
            for (const auto& line : settings.lines()) {
                if (line.kind != core::IniDocument::LineKind::Section ||
                    !Prefix(line.section_name, L"MagicWord.") ||
                    Equal(line.section_name, L"MagicWord._DEFAULT") ||
                    !Equal(settings.GetValue(line.section_name, L"App"), app.id)) continue;
                const auto magic = Split(settings.GetValue(line.section_name, L"Names"), L',');
                aliases.insert(aliases.end(), magic.begin(), magic.end());
            }
            Item item{Kind::App, app.name + (app.archived ? L" (archivado)" : L""), app.id, {}};
            bool match = false, prefix = false;
            for (const auto& name : aliases) {
                if (Equal(typed, name) || (Prefix(typed, name) && typed.size() > name.size() && typed[name.size()] == L' ')) {
                    if (typed.size() > name.size()) item.args = typed.substr(name.size() + 1);
                    items_.push_back(item);
                    match = true;
                    break;
                }
                prefix = prefix || Prefix(name, typed);
            }
            if (!match && prefix) partial.push_back(std::move(item));
        }
        items_.insert(items_.end(), partial.begin(), partial.end());
    }
    const auto folder = view_ == View::Files ? folder_ : L"Other";
    const auto full = resolver_.ResolvePmRelativePath(folder);
    applications::Pins pins;
    if (full.ok && !applications::PinDirectory(full.resolved_path, pins)) {
        std::error_code ec;
        std::vector<Item> files;
        for (fs::directory_iterator it(full.resolved_path, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().native();
            const auto normalized = PathResolver::NormalizePmRelativePath(name);
            if (!normalized.ok || normalized.resolved_path != name || !Prefix(name, typed)) continue;
            const auto attrs = ::GetFileAttributesW(it->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || attrs & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            files.push_back({attrs & FILE_ATTRIBUTE_DIRECTORY ? Kind::Folder : Kind::File, name, folder + L"\\" + name, {}});
            if (files.size() == 1000) break;
        }
        std::sort(files.begin(), files.end(), [](const Item& a, const Item& b) { return a.label < b.label; });
        items_.insert(items_.end(), files.begin(), files.end());
    }
    for (const auto& item : items_)
        ::SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
    if (!items_.empty()) ::SendMessageW(list_, LB_SETCURSEL, 0, 0);
    Layout();
}

void Desktop::Back() {
    if (view_ != View::Files || folder_ == L"Other") { Show(View::Pill); return; }
    folder_.resize(folder_.rfind(L'\\'));
    Populate();
}


void Desktop::Open(int index) {
    if (closing_ || index < 0 || static_cast<std::size_t>(index) >= items_.size()) return;
    const auto item = items_[static_cast<std::size_t>(index)];
    if (item.kind == Kind::Folder) {
        view_ = View::Files;
        folder_ = item.value;
        Populate();
        ::SetFocus(list_);
    } else if (item.kind == Kind::File) {
        BrowseDocument(item);
    } else {
        App app;
        const auto error = applications::Find(resolver_, item.value, app);
        if (error) Report(error);
        else if (app.archived) PickPackage(app.id);
        else Launch(app, item.args);
    }
}

void Desktop::BrowseDocument(const Item& item) {
    const auto path = resolver_.ResolvePmRelativePath(item.value);
    if (!path.ok || !applications::Within(item.value, L"Other")) return;
    applications::Pins pins;
    auto error = applications::PinDirectory(fs::path(path.resolved_path).parent_path().native(), pins);
    applications::Handle document;
    if (!error) error = applications::CheckFile(path.resolved_path, document);
    if (error) { Report(error); return; }
    auto ext = fs::path(item.value).extension().native();
    if (!ext.empty()) ext.erase(0, 1);
    std::wstring id;
    App app;
    if (!ext.empty() && !applications::Association(resolver_, ext, id) && !id.empty() &&
        !applications::Find(resolver_, id, app) && !app.archived && !applications::ValidateApp(resolver_, app, true)) {
        Launch(app, jobs::QuoteArgument(path.resolved_path));
        return;
    }
    const auto menu = ::CreatePopupMenu();
    std::vector<App> available;
    for (auto a : apps_) {
        if (a.archived || applications::ValidateApp(resolver_, a, true)) continue;
        available.push_back(a);
        ::AppendMenuW(menu, MF_STRING, available.size(), a.name.c_str());
    }
    constexpr int kHost = 32000;
    ::AppendMenuW(menu, MF_STRING, kHost, L"Abrir con Windows\u2026");
    POINT cursor{};
    ::GetCursorPos(&cursor);
    modal_ = true;
    const auto choice = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, hwnd_, nullptr);
    ::DestroyMenu(menu);
    if (choice == kHost) {
        if (::MessageBoxW(hwnd_, L"\u00bfAbrir con la aplicaci\u00f3n de Windows? PML no controla sus residuos.",
                          L"PortableManager Desktop", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            const auto code = reinterpret_cast<INT_PTR>(::ShellExecuteW(hwnd_, L"open", path.resolved_path.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL));
            if (code <= 32) Report(static_cast<DWORD>(code));
            else Show(View::Pill);
        }
    } else if (choice > 0 && static_cast<std::size_t>(choice) <= available.size()) {
        const auto selected = available[static_cast<std::size_t>(choice - 1)];
        if (!ext.empty() && ::MessageBoxW(hwnd_, L"\u00bfRecordar para esta extensi\u00f3n?",
                L"PortableManager Desktop", MB_YESNO | MB_ICONQUESTION) == IDYES)
            Report(applications::SetAssociation(resolver_, ext, selected.id));
        Launch(selected, jobs::QuoteArgument(path.resolved_path));
    }
    modal_ = false;
}

std::wstring ExpandWeb(std::wstring_view pattern, const std::wstring& input) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring url_value;
    for (const unsigned char byte : core::IniDocument::WideToUtf8(input)) {
        const bool simple = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~';
        if (simple) url_value.push_back(static_cast<wchar_t>(byte));
        else {
            url_value.push_back(L'%');
            url_value.push_back(hex[byte >> 4]);
            url_value.push_back(hex[byte & 15]);
        }
    }
    std::wstring result;
    while (!pattern.empty()) {
        if (pattern.starts_with(L"$W$")) {
            result += url_value;
            pattern.remove_prefix(3);
        } else if (pattern.starts_with(L"$I$")) {
            result += input;
            pattern.remove_prefix(3);
        } else {
            result.push_back(pattern.front());
            pattern.remove_prefix(1);
        }
    }
    return result;
}

void Desktop::SearchWeb(bool fallback) {
    const auto query = Text(edit_);
    if (query.empty() || closing_) return;
    const auto& settings = config_.config_doc();
    const auto section = fallback ? L"MagicWord._DEFAULT" : L"Web";
    const auto id = settings.GetValue(section, L"App");
    const auto url = ExpandWeb(settings.GetValue(section, L"URL", L"https://www.google.com/search?q=$W$"), query);
    const auto argument = fallback ? settings.GetValue(section, L"Argument") : L"";
    App app;
    if (!id.empty()) {
        const auto error = applications::Find(resolver_, id, app);
        if (error || app.archived) { Report(error ? error : ERROR_FILE_NOT_FOUND); return; }
        Launch(app, jobs::QuoteArgument(argument.empty() ? url : ExpandWeb(argument, query)));
        return;
    }
    const auto browser = std::find_if(apps_.begin(), apps_.end(), [](const App& a) { return a.browser && !a.archived; });
    if (browser != apps_.end()) { Launch(*browser, jobs::QuoteArgument(url)); return; }
    if (!url.starts_with(L"https://") && !url.starts_with(L"http://")) { Report(ERROR_INVALID_NAME); return; }
    modal_ = true;
    if (::MessageBoxW(hwnd_, L"\u00bfUsar el navegador del anfitri\u00f3n para esta b\u00fasqueda?",
                      L"PortableManager Desktop", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) Report(static_cast<DWORD>(result));
        else Show(View::Pill);
    }
    modal_ = false;
}

void Desktop::Launch(const App& app, std::wstring arguments) {
    if (closing_) return;
    if (Equal(app.color, L"Red")) {
        const bool old_modal = modal_;
        modal_ = true;
        const auto confirmed = ::MessageBoxW(hwnd_, L"Esta aplicaci\u00f3n puede dejar residuos. \u00bfAbrir de todas formas?",
                                              app.name.c_str(), MB_YESNO | MB_ICONWARNING);
        modal_ = old_modal;
        if (confirmed != IDYES) return;
    }
    if (std::any_of(workers_.begin(), workers_.end(), [&](const auto& w) { return w->app.id == app.id; })) {
        Report(ERROR_BUSY);
        return;
    }
    auto entry = std::make_unique<Worker>();
    entry->app = app;
    entry->event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!entry->event) { Report(::GetLastError()); return; }
    entry->stop.event = entry->event.get();
    auto* worker = entry.get();
    workers_.push_back(std::move(entry));
    try {
        worker->thread = std::thread([this, worker, args = std::move(arguments)] {
            try { worker->result = applications::Run(resolver_, worker->app, args, worker->stop, session_); }
            catch (...) { worker->result.error = ERROR_UNHANDLED_EXCEPTION; }
            ::PostMessageW(hwnd_, kFinished, 0, reinterpret_cast<LPARAM>(worker));
        });
    } catch (...) {
        workers_.pop_back();
        Report(ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    Show(View::Pill);
}

void Desktop::Finish(Worker* worker) {
    const auto found = std::find_if(workers_.begin(), workers_.end(), [&](const auto& w) { return w.get() == worker; });
    if (found == workers_.end()) return;
    worker->thread.join();
    const auto result = worker->result;
    const auto uninstall = worker->uninstall_after_close;
    const auto app = worker->app;
    workers_.erase(found);
    if (result.error == ERROR_DEVICE_NOT_CONNECTED) {
        if (result.rollback_error) status_ = 1;
        Close(Mode::Critical);
        return;
    }
    if (result.error || result.rollback_error) Report(result.rollback_error ? result.rollback_error : result.error);
    else if (uninstall && !closing_)
        StartOperation([this, app] { return applications::Maintain(resolver_, app, applications::Maintenance::Uninstall); });
    CloseWhenReady();
}

void Desktop::Close(Mode mode) {
    if (!closing_ || mode == Mode::Critical) close_mode_ = mode;
    closing_ = true;
    session_.BeginShutdown();
    view_ = View::Pill;
    for (auto& worker : workers_) {
        worker->stop.mode.store(mode);
        ::SetEvent(worker->event.get());
    }
    Layout();
    CloseWhenReady();
}

void Desktop::CloseWhenReady() {
    if (closing_ && workers_.empty() && !busy_) ::DestroyWindow(hwnd_);
}

void Desktop::Report(DWORD error) {
    error_ = error != ERROR_SUCCESS;
    if (error && closing_) status_ = 1;
    if (error && !closing_) {
        const bool was_modal = modal_;
        modal_ = true;
        const auto message = L"No se complet\u00f3 la operaci\u00f3n (Win32 " + std::to_wstring(error) + L").";
        ::MessageBoxW(hwnd_, message.c_str(), L"PortableManager Desktop", MB_OK | MB_ICONWARNING);
        modal_ = was_modal;
    }
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void Desktop::StartOperation(std::function<DWORD()> operation) {
    if (busy_ || closing_) return;
    busy_ = true;
    Show(View::Pill);
    try {
        operation_ = std::thread([this, action = std::move(operation)] {
            DWORD error = ERROR_UNHANDLED_EXCEPTION;
            try { error = action(); } catch (...) {}
            ::PostMessageW(hwnd_, kOperation, error, 0);
        });
    } catch (...) {
        busy_ = false;
        Report(ERROR_NOT_ENOUGH_MEMORY);
        Layout();
    }
}

void Desktop::UpdateClipboard() {
    if (!::OpenClipboard(hwnd_)) return;
    const auto drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP));
    std::wstring candidate;
    if (drop && ::DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1) {
        const auto size = ::DragQueryFileW(drop, 0, nullptr, 0);
        candidate.resize(static_cast<std::size_t>(size) + 1);
        ::DragQueryFileW(drop, 0, candidate.data(), size + 1);
        candidate.resize(size);
    }
    ::CloseClipboard();
    const auto attrs = candidate.empty() ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(candidate.c_str());
    package_ = attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        IsPackage(candidate) ? std::move(candidate) : L"";
    ::SetWindowTextW(install_, (L"Instalar " + SuggestedId(package_)).c_str());
    Layout();
}

void Desktop::PickPackage(std::wstring id) {
    wchar_t path[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = hwnd_;
    picker.lpstrFile = path;
    picker.nMaxFile = static_cast<DWORD>(std::size(path));
    picker.lpstrFilter = L"Paquetes portables\0*.paf.exe;*.zip;*.7z;*.rar;*.tar\0\0";
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    modal_ = true;
    const bool selected = ::GetOpenFileNameW(&picker) != FALSE;
    modal_ = false;
    if (!selected) return;
    if (id.empty()) {
        package_ = path;
        InstallPackage(false);
    } else {
        StartInstall(std::move(id), path);
    }
}

void Desktop::InstallPackage(bool choose_destination) {
    if (package_.empty() || busy_ || closing_) return;
    auto id = SuggestedId(package_);
    App existing;
    if (!applications::Find(resolver_, id, existing)) choose_destination = true;
    if (choose_destination) {
        applications::List(resolver_, apps_);
        const auto menu = ::CreatePopupMenu();
        ::AppendMenuW(menu, MF_STRING, 1, L"Nueva instalaci\u00f3n");
        for (std::size_t i = 0; i < apps_.size(); ++i)
            ::AppendMenuW(menu, MF_STRING, i + 2, apps_[i].name.c_str());
        POINT cursor{};
        ::GetCursorPos(&cursor);
        modal_ = true;
        const auto choice = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, hwnd_, nullptr);
        modal_ = false;
        ::DestroyMenu(menu);
        if (!choice) return;
        if (choice > 1 && static_cast<std::size_t>(choice - 2) < apps_.size())
            id = apps_[static_cast<std::size_t>(choice - 2)].id;
        else {
            const auto base = id;
            for (unsigned n = 2; !applications::Find(resolver_, id, existing); ++n)
                id = base + L"-" + std::to_wstring(n);
        }
    }
    StartInstall(std::move(id), package_);
}

void Desktop::StartInstall(std::wstring id, std::wstring package, std::wstring executable) {
    if (busy_ || closing_) return;
    selection_ = std::make_shared<InstallSelection>();
    selection_->id = std::move(id);
    selection_->package = std::move(package);
    StartOperation([this, selection = selection_, executable = std::move(executable)] {
        return applications::Install(resolver_, selection->id, selection->package, executable, &selection->choices);
    });
}

bool Desktop::ChooseExecutable() {
    if (!selection_ || selection_->choices.empty()) return false;
    const auto current = std::move(selection_);
    const auto menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Seleccionar ejecutable principal");
    for (std::size_t i = 0; i < current->choices.size(); ++i)
        ::AppendMenuW(menu, MF_STRING, i + 1, current->choices[i].c_str());
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(hwnd_);
    modal_ = true;
    const auto choice = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, hwnd_, nullptr);
    modal_ = false;
    ::DestroyMenu(menu);
    if (choice > 0 && static_cast<std::size_t>(choice) <= current->choices.size())
        StartInstall(current->id, current->package, current->choices[static_cast<std::size_t>(choice - 1)]);
    else Layout();
    return true;
}

void Desktop::Menu(bool application, int index) {
    if (closing_) return;
    const auto menu = ::CreatePopupMenu();
    App app;
    Worker* worker = nullptr;
    if (application) {
        if (index < 0 || static_cast<std::size_t>(index) >= items_.size() ||
            items_[static_cast<std::size_t>(index)].kind != Kind::App ||
            applications::Find(resolver_, items_[static_cast<std::size_t>(index)].value, app)) {
            ::DestroyMenu(menu);
            return;
        }
        for (auto& entry : workers_) if (entry->app.id == app.id) worker = entry.get();
        ::AppendMenuW(menu, MF_STRING, 1, app.archived ? L"Reinstalar\u2026" : L"Abrir");
        ::AppendMenuW(menu, MF_STRING | (worker ? 0 : MF_GRAYED), 2, L"Cerrar");
        ::AppendMenuW(menu, MF_STRING, 3, L"Configurar\u2026");
        const UINT available = MF_STRING | (worker || busy_ ? MF_GRAYED : 0);
        ::AppendMenuW(menu, available, 4, L"Actualizar\u2026");
        ::AppendMenuW(menu, available, 5, L"Restablecer\u2026");
        ::AppendMenuW(menu, available, 6, L"Archivar\u2026");
        ::AppendMenuW(menu, MF_STRING | (busy_ ? MF_GRAYED : 0), 7, L"Desinstalar\u2026");
    } else {
        ::AppendMenuW(menu, MF_STRING, 1, L"Escritorio");
        ::AppendMenuW(menu, MF_STRING, 2, L"Buscar");
        ::AppendMenuW(menu, MF_STRING, 3, L"Instalar paquete\u2026");
        if (!package_.empty()) ::AppendMenuW(menu, MF_STRING, 4, L"Elegir destino del portapapeles\u2026");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, 5, L"Cierre normal");
        ::AppendMenuW(menu, MF_STRING, 6, L"Cierre r\u00e1pido");
        ::AppendMenuW(menu, MF_STRING, 7, L"Cierre cr\u00edtico");
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(hwnd_);
    modal_ = true;
    const auto choice = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, hwnd_, nullptr);
    ::DestroyMenu(menu);
    if (!application) {
        if (choice == 1) Show(View::Files);
        if (choice == 2) Show(View::Search);
        if (choice == 3) PickPackage();
        if (choice == 4) InstallPackage(true);
        if (choice >= 5 && choice <= 7)
            Close(choice == 5 ? Mode::Normal : choice == 6 ? Mode::Fast : Mode::Critical);
    } else {
        if (choice == 1) Open(index);
        if (choice == 2) {
            // A worker may finish while the popup menu pumps messages.
            for (auto& entry : workers_)
                if (entry->app.id == app.id) ::SetEvent(entry->event.get());
        }
        if (choice == 3 && ::MessageBoxW(hwnd_, L"\u00bfAbrir apps.ini en el editor de Windows?",
                L"PortableManager Desktop", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            const auto file = resolver_.ResolvePmRelativePath(L"Data\\apps.ini");
            const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(hwnd_, L"open", file.resolved_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) Report(static_cast<DWORD>(result));
        }
        if (choice == 4) PickPackage(app.id);
        if (choice >= 5 && choice <= 7) {
            const auto action = choice == 5 ? applications::Maintenance::Reset :
                choice == 6 ? applications::Maintenance::Archive : applications::Maintenance::Uninstall;
            const auto text = choice == 6 ? L"\u00bfArchivar esta aplicaci\u00f3n y conservar sus datos?"
                : L"\u00bfConfirmar la eliminaci\u00f3n de los datos portables de esta aplicaci\u00f3n?";
            if (::MessageBoxW(hwnd_, text, app.name.c_str(), MB_YESNO | MB_ICONWARNING) == IDYES) {
                const auto running = std::find_if(workers_.begin(), workers_.end(),
                    [&](const auto& active) { return active->app.id == app.id; });
                if (action == applications::Maintenance::Uninstall && running != workers_.end()) {
                    (*running)->uninstall_after_close = true;
                    (*running)->stop.mode.store(Mode::Normal);
                    ::SetEvent((*running)->event.get());
                } else {
                    StartOperation([this, app, action] { return applications::Maintain(resolver_, app, action); });
                }
            }
        }
    }
    modal_ = false;
}

} // namespace

int Run(const PathResolver& resolver) {
    return Desktop(resolver).Run();
}

} // namespace pm::pmd
