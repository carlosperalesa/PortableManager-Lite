#include "pmd/pmd_module.h"
#include "pmd/pmd_graphics.h"
#include "applications/app_paths.h"
#include "applications/session_host.h"
#include "cleanup/cleanup_module.h"
#include "core/config_manager.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "learning/learning_engine.h"

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
constexpr UINT kTrayCallback = WM_APP + 10;
constexpr UINT kTrayId = 1;

constexpr UINT_PTR kOriginTimerId = 1;
constexpr UINT_PTR kAnimTimerId = 2;
constexpr UINT_PTR kCaretTimerId = 3;
constexpr UINT_PTR kBusyTimerId = 4;

enum class View { Pill, Search, Files };
enum class Kind { App, File, Folder };
enum class HitZone { None, InstallIcon, InstallDest, OrphanBadge, WebSearch, BackButton, Item };

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
    learning::LearningResult learning_result;
    bool learning_run = false;
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

gfx::Color4 PrivacyColor(std::wstring_view color_name) {
    if (Equal(color_name, L"Green")) return {52, 211, 153, 255};
    if (Equal(color_name, L"Yellow")) return {251, 191, 36, 255};
    if (Equal(color_name, L"Red")) return {248, 113, 113, 255};
    return {148, 163, 184, 255};
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

    int S(int px) const { return gfx::Scale(px, dpi_); }
    float Sf(float px) const { return gfx::ScaleF(px, dpi_); }
    int ShadowPad() const { return S(14); }
    bool IsOfferVisible() const { return view_ == View::Pill && !package_.empty() && !closing_ && !busy_; }
    bool IsAppRunning(std::wstring_view id) const;
    const App* FindLoadedApp(std::wstring_view id) const;
    std::wstring GhostSuffix(const std::wstring& typed) const;

    void Show(View view);
    void InitTray();
    void UpdateTrayIcon();
    void RemoveTray();
    void RefreshOrphans();
    void ChooseOrphanAction();
    void RestoreOrphansInteractive();
    void DiscardOrphansInteractive();
    void EnsureConfigDefaults();
    void ApplyConfigSettings(bool register_hotkey);
    void SavePosition();
    void Layout();
    void ApplyWindowGeometry();
    void Render();
    void Populate();
    void Open(int index);
    void Back();
    void BrowseDocument(const Item& item);
    void SearchWeb(bool fallback);
    void Launch(const App& app, std::wstring arguments);
    void Close(Mode mode);
    void CloseWhenReady();
    void LoseOrigin();
    void Finish(Worker* worker);
    void UpdateClipboard();
    void PickPackage(std::wstring id = {});
    void InstallPackage(bool choose_destination);
    void StartInstall(std::wstring id, std::wstring package, std::wstring executable = {});
    bool ChooseExecutable();
    void Menu(bool application, int index = -1);
    void StartOperation(std::function<DWORD()> operation);
    void Report(DWORD error);

    HitZone HitTest(POINT client, int& item_index) const;
    void UpdateHover(POINT client);
    void EnsureVisibleSelection();
    void MoveSelection(int dx, int dy);

    const PathResolver& resolver_;
    core::ConfigManager config_;
    applications::SessionHost session_;
    gfx::GdiPlusSession gdiplus_;
    gfx::Surface surface_;

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND list_ = nullptr;
    WNDPROC edit_proc_ = nullptr;
    WNDPROC list_proc_ = nullptr;

    View view_ = View::Pill;
    std::wstring folder_ = L"Other";
    std::wstring origin_;
    std::wstring package_;
    std::wstring dismissed_package_;
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
    bool origin_lost_ = false;
    int status_ = 0;

    // Visual & layout state (all logical coordinates scaled by dpi_)
    UINT dpi_ = 96;
    int target_w_ = 96;
    int target_h_ = 32;
    int cur_w_ = 96;
    int cur_h_ = 32;
    int win_x_ = 0;
    int win_y_ = 0;
    bool shown_ = false;
    bool has_pill_anchor_ = false;
    int pill_center_x_ = 0;
    int pill_top_y_ = 0;
    bool always_on_top_ = true;
    int translucent_pct_ = 90;
    bool remember_pos_ = true;
    std::wstring registered_hotkey_;
    bool dragging_ = false;
    POINT drag_start_screen_{};
    int drag_start_x_ = 0;
    int drag_start_y_ = 0;

    int cols_ = 4;
    int visible_rows_ = 1;
    int scroll_row_ = 0;
    int selected_ = 0;
    HitZone hover_zone_ = HitZone::None;
    int hover_index_ = -1;
    bool tracking_mouse_ = false;
    bool caret_visible_ = true;
    int busy_tick_ = 0;

    RECT rect_install_{};
    RECT rect_dest_{};
    RECT rect_orphan_{};
    RECT rect_web_{};
    RECT rect_back_{};
    RECT rect_edit_draw_{};

    HICON tray_icon_green_ = nullptr;
    HICON tray_icon_red_ = nullptr;
    bool tray_added_ = false;
    bool tray_is_red_ = false;
    std::vector<std::wstring> orphan_sessions_;
};

bool Desktop::IsAppRunning(std::wstring_view id) const {
    return std::any_of(workers_.begin(), workers_.end(), [&](const auto& w) { return w->app.id == id; });
}

const App* Desktop::FindLoadedApp(std::wstring_view id) const {
    const auto it = std::find_if(apps_.begin(), apps_.end(), [&](const App& a) { return a.id == id; });
    return it == apps_.end() ? nullptr : &(*it);
}

std::wstring Desktop::GhostSuffix(const std::wstring& typed) const {
    if (typed.empty() || items_.empty() || typed.starts_with(L"=")) return {};
    const auto& first = items_.front().label;
    if (first.size() <= typed.size() || !Prefix(first, typed)) return {};
    return first.substr(typed.size());
}

int Desktop::Run() {
    if (!gdiplus_.ok()) return 1;
    gfx::EnablePerMonitorDpiAwareness();
    origin_ = PathResolver::QueryExecutablePath();
    const auto temp = runtime::SessionManager::QuerySystemTempDir();
    if (temp.empty() || !::SetCurrentDirectoryW(temp.c_str())) return 1;
    config_.LoadConfigIni(false);
    const auto instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassW(&wc)) return 1;
    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kWindowClass, L"PortableManager Desktop", WS_POPUP, 0, 0, 124, 60,
        nullptr, nullptr, instance, this);
    if (!hwnd_) return 1;
    dpi_ = gfx::WindowDpi(hwnd_);

    const DWORD started = session_.Start(origin_, [window = hwnd_] {
        ::PostMessageW(window, kGuardChanged, 0, 0);
    });
    if (started) {
        session_.Close(Mode::Critical);
        ::DestroyWindow(hwnd_);
        return 1;
    }

    ApplyConfigSettings(true);
    const int icon_w = ::GetSystemMetrics(SM_CXSMICON);
    const int icon_h = ::GetSystemMetrics(SM_CYSMICON);
    tray_icon_green_ = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(101), IMAGE_ICON, icon_w, icon_h, 0));
    tray_icon_red_ = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(102), IMAGE_ICON, icon_w, icon_h, 0));
    RefreshOrphans();
    InitTray();
    ::AddClipboardFormatListener(hwnd_);
    ::SetTimer(hwnd_, kOriginTimerId, 500, nullptr);
    ::SetTimer(hwnd_, kCaretTimerId, 530, nullptr);
    UpdateClipboard();
    Layout();
    shown_ = true;
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
                self->Render();
            }
        } else {
            if (self->selected_ < 0 && edit) self->SearchWeb(true);
            else if (self->items_.empty() && edit) self->SearchWeb(true);
            else self->Open(self->selected_);
        }
        return 0;
    }
    if (edit && msg == WM_CHAR && wp == L' ' && Text(control).empty()) {
        self->Show(View::Files);
        return 0;
    }
    if (edit && msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN)) return 0;
    if (edit && msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN) && !self->items_.empty()) {
        self->MoveSelection(0, wp == VK_UP ? -1 : 1);
        return 0;
    }
    if (!edit && msg == WM_KEYDOWN) {
        if (wp == VK_UP) { self->MoveSelection(0, -1); return 0; }
        if (wp == VK_DOWN) { self->MoveSelection(0, 1); return 0; }
        if (wp == VK_LEFT) { self->MoveSelection(-1, 0); return 0; }
        if (wp == VK_RIGHT) { self->MoveSelection(1, 0); return 0; }
    }
    if (!edit && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
                  msg == WM_RBUTTONUP || msg == WM_MOUSEWHEEL)) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (msg != WM_MOUSEWHEEL) ::MapWindowPoints(control, parent, &pt, 1);
        return ::SendMessageW(parent, msg, wp, msg == WM_MOUSEWHEEL ? lp : MAKELPARAM(pt.x, pt.y));
    }

    const auto result = ::CallWindowProcW(base, control, msg, wp, lp);
    if (edit && (msg == WM_KEYUP || msg == WM_LBUTTONUP || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                 msg == WM_CHAR || msg == WM_SETTEXT)) {
        self->caret_visible_ = true;
        self->Render();
    }
    return result;
}

LRESULT Desktop::Message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const auto make = [&](const wchar_t* type, const wchar_t* title, DWORD style, INT_PTR id) {
                return ::CreateWindowExW(0, type, title, WS_CHILD | style, 0, 0, 0, 0, hwnd_,
                    reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr), nullptr);
            };
            edit_ = make(L"EDIT", L"", ES_AUTOHSCROLL, 1);
            list_ = make(L"STATIC", L"", SS_NOTIFY, 2);
            if (!edit_ || !list_) return -1;
            edit_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            list_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(list_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd_, &ps);
            ::EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            Layout();
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                const bool clickable = hover_zone_ != HitZone::None || view_ == View::Pill;
                ::SetCursor(::LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (dragging_) {
                POINT cursor{};
                ::GetCursorPos(&cursor);
                win_x_ = drag_start_x_ + (cursor.x - drag_start_screen_.x);
                win_y_ = drag_start_y_ + (cursor.y - drag_start_screen_.y);
                const int pad = ShadowPad();
                pill_center_x_ = win_x_ + pad + cur_w_ / 2;
                pill_top_y_ = win_y_ + pad;
                has_pill_anchor_ = true;
                ::SetWindowPos(hwnd_, always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST, win_x_, win_y_, 0, 0,
                               SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;
            }
            if (!tracking_mouse_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                ::TrackMouseEvent(&tme);
                tracking_mouse_ = true;
            }
            UpdateHover({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (hover_zone_ != HitZone::None || hover_index_ != -1) {
                hover_zone_ = HitZone::None;
                hover_index_ = -1;
                Render();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            const auto zone = HitTest(client, idx);
            if (view_ == View::Pill) {
                POINT screen = client;
                ::ClientToScreen(hwnd_, &screen);
                if (::DragDetect(hwnd_, screen)) {
                    dragging_ = true;
                    has_pill_anchor_ = true;
                    drag_start_screen_ = screen;
                    drag_start_x_ = win_x_;
                    drag_start_y_ = win_y_;
                    const int pad = ShadowPad();
                    pill_center_x_ = win_x_ + pad + cur_w_ / 2;
                    pill_top_y_ = win_y_ + pad;
                    ::SetCapture(hwnd_);
                    return 0;
                }
                if (zone == HitZone::InstallIcon) InstallPackage(false);
                else if (zone == HitZone::InstallDest) InstallPackage(true);
                else if (zone == HitZone::OrphanBadge) ChooseOrphanAction();
                else Show(View::Search);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (dragging_) {
                dragging_ = false;
                ::ReleaseCapture();
                return 0;
            }
            if (view_ == View::Pill) return 0;
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            const auto zone = HitTest(client, idx);
            if (zone == HitZone::WebSearch) SearchWeb(false);
            else if (zone == HitZone::BackButton) Back();
            else if (zone == HitZone::Item && idx >= 0) {
                selected_ = idx;
                ::PostMessageW(hwnd_, kOpen, static_cast<WPARAM>(idx), 0);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            const auto zone = HitTest(client, idx);
            if (zone == HitZone::Item && idx >= 0) Menu(true, idx);
            else Menu(false);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (view_ == View::Files && !items_.empty()) {
                const int total_rows = static_cast<int>((items_.size() + static_cast<std::size_t>(cols_) - 1) / static_cast<std::size_t>(cols_));
                const int max_scroll = std::max(0, total_rows - visible_rows_);
                const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                scroll_row_ = std::clamp(scroll_row_ + (delta > 0 ? -1 : 1), 0, max_scroll);
                Render();
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == 1 && HIWORD(wp) == EN_CHANGE && view_ == View::Search) Populate();
            return 0;
        case WM_HOTKEY:
            if (config_.LoadConfigIni(false).ok) ApplyConfigSettings(true);
            Show(View::Search);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_SPACE && view_ == View::Pill) Show(View::Files);
            else if (wp == VK_ESCAPE) Back();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (!modal_ && view_ != View::Pill) Show(View::Pill);
            } else {
                RefreshOrphans();
                if (config_.LoadConfigIni(false).ok) {
                    ApplyConfigSettings(true);
                    ApplyWindowGeometry();
                }
                Layout();
            }
            return 0;
        case WM_DISPLAYCHANGE: Layout(); return 0;
        case WM_CLIPBOARDUPDATE: UpdateClipboard(); return 0;
        case WM_TIMER:
            if (wp == kOriginTimerId) {
                if (!closing_ && !guard::IsOriginAccessible(origin_)) LoseOrigin();
            } else if (wp == kAnimTimerId) {
                const auto step = [](int cur, int target) {
                    const int diff = target - cur;
                    if (std::abs(diff) <= 2) return target;
                    const int delta = diff / 3;
                    return cur + (delta != 0 ? delta : (diff > 0 ? 1 : -1));
                };
                cur_w_ = step(cur_w_, target_w_);
                cur_h_ = step(cur_h_, target_h_);
                if (has_pill_anchor_) {
                    MONITORINFO monitor{};
                    monitor.cbSize = sizeof(monitor);
                    if (::GetMonitorInfoW(::MonitorFromPoint(POINT{pill_center_x_, pill_top_y_}, MONITOR_DEFAULTTONEAREST), &monitor)) {
                        const auto& area = monitor.rcWork;
                        const int pad = ShadowPad();
                        const int x_capsule = std::clamp(pill_center_x_ - cur_w_ / 2,
                            static_cast<int>(area.left), std::max(static_cast<int>(area.left), static_cast<int>(area.right) - cur_w_));
                        const int y_capsule = std::clamp(pill_top_y_,
                            static_cast<int>(area.top), std::max(static_cast<int>(area.top), static_cast<int>(area.bottom) - cur_h_));
                        win_x_ = x_capsule - pad;
                        win_y_ = y_capsule - pad;
                    }
                }
                ApplyWindowGeometry();
                Render();
                if (cur_w_ == target_w_ && cur_h_ == target_h_) ::KillTimer(hwnd_, kAnimTimerId);
            } else if (wp == kCaretTimerId) {
                if (view_ == View::Search) {
                    caret_visible_ = !caret_visible_;
                    Render();
                }
            } else if (wp == kBusyTimerId) {
                if (busy_ || closing_) {
                    busy_tick_ = (busy_tick_ + 1) % 32;
                    Render();
                } else {
                    ::KillTimer(hwnd_, kBusyTimerId);
                }
            }
            return 0;
        case WM_CLOSE: Close(Mode::Normal); return 0;
        case WM_QUERYENDSESSION: Close(Mode::Critical); return TRUE;
        case kGuardChanged: UpdateTrayIcon(); Render(); return 0;
        case kTrayCallback: {
            const UINT tray_msg = LOWORD(lp);
            if (tray_msg == WM_LBUTTONUP) {
                Show(View::Search);
            } else if (tray_msg == WM_LBUTTONDBLCLK) {
                Show(View::Files);
            } else if (tray_msg == WM_RBUTTONUP) {
                ::SetForegroundWindow(hwnd_);
                Menu(false);
                ::PostMessageW(hwnd_, WM_NULL, 0, 0);
            }
            return 0;
        }
        case kOpen: Open(static_cast<int>(wp)); return 0;
        case kFinished: Finish(reinterpret_cast<Worker*>(lp)); return 0;
        case kOperation:
            operation_.join();
            busy_ = false;
            if (wp == ERROR_MORE_DATA && !closing_ && ChooseExecutable()) return 0;
            if (wp == 0 && selection_ && !package_.empty() && selection_->package == package_) {
                dismissed_package_ = package_;
                package_.clear();
            }
            selection_.reset();
            Report(static_cast<DWORD>(wp));
            if (!closing_) {
                RefreshOrphans();
                Populate();
            }
            CloseWhenReady();
            return 0;
        case WM_DESTROY:
            RemoveTray();
            if (tray_icon_green_) { ::DestroyIcon(tray_icon_green_); tray_icon_green_ = nullptr; }
            if (tray_icon_red_) { ::DestroyIcon(tray_icon_red_); tray_icon_red_ = nullptr; }
            ::UnregisterHotKey(hwnd_, 1);
            ::RemoveClipboardFormatListener(hwnd_);
            ::KillTimer(hwnd_, kOriginTimerId);
            ::KillTimer(hwnd_, kAnimTimerId);
            ::KillTimer(hwnd_, kCaretTimerId);
            ::KillTimer(hwnd_, kBusyTimerId);
            ::PostQuitMessage(status_);
            return 0;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

void Desktop::EnsureConfigDefaults() {
    auto& doc = config_.config_doc();
    std::wstring error;
    if (doc.GetValue(L"Desktop", L"Hotkey").empty())
        doc.SetValue(L"Desktop", L"Hotkey", L"Ctrl+Space", error);
    if (doc.GetValue(L"Desktop", L"AlwaysOnTop").empty())
        doc.SetValue(L"Desktop", L"AlwaysOnTop", L"true", error);
    if (doc.GetValue(L"Desktop", L"Translucent").empty())
        doc.SetValue(L"Desktop", L"Translucent", L"90%", error);
    if (doc.GetValue(L"Desktop", L"RememberPosition").empty())
        doc.SetValue(L"Desktop", L"RememberPosition", L"true", error);
}

void Desktop::ApplyConfigSettings(bool register_hotkey) {
    const auto& doc = config_.config_doc();
    const auto top_val = doc.GetValue(L"Desktop", L"AlwaysOnTop", L"true");
    always_on_top_ = !Equal(top_val, L"false") && top_val != L"0" && !Equal(top_val, L"no");

    const auto rem_val = doc.GetValue(L"Desktop", L"RememberPosition", L"true");
    remember_pos_ = !Equal(rem_val, L"false") && rem_val != L"0" && !Equal(rem_val, L"no");

    auto trans_val = doc.GetValue(L"Desktop", L"Translucent", L"90%");
    if (!trans_val.empty() && trans_val.back() == L'%') trans_val.pop_back();
    wchar_t* end = nullptr;
    const long parsed_pct = trans_val.empty() ? 90 : std::wcstol(trans_val.c_str(), &end, 10);
    translucent_pct_ = (end != trans_val.c_str() && *end == L'\0')
        ? std::clamp(static_cast<int>(parsed_pct), 15, 100) : 90;

    if (register_hotkey && hwnd_) {
        const auto key = doc.GetValue(L"Desktop", L"Hotkey", L"Ctrl+Space");
        if (!Equal(key, registered_hotkey_)) {
            ::UnregisterHotKey(hwnd_, 1);
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
            registered_hotkey_ = key;
        }
    }
}

void Desktop::SavePosition() {
    if (origin_lost_ || !remember_pos_ || !has_pill_anchor_) return;
    if (!config_.LoadConfigIni(true).ok) return;
    EnsureConfigDefaults();
    const int pill_w = IsOfferVisible() ? target_w_ : S(96);
    const int save_x = pill_center_x_ - pill_w / 2;
    const int save_y = pill_top_y_;
    std::wstring error;
    config_.config_doc().SetValue(L"Desktop", L"X", std::to_wstring(save_x), error);
    config_.config_doc().SetValue(L"Desktop", L"Y", std::to_wstring(save_y), error);
    config_.SaveConfigIniIfModified(true);
}

void Desktop::Layout() {
    POINT anchor{};
    if (has_pill_anchor_) {
        anchor = {pill_center_x_, pill_top_y_};
    } else {
        ::GetCursorPos(&anchor);
    }
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!::GetMonitorInfoW(::MonitorFromPoint(anchor, has_pill_anchor_ ? MONITOR_DEFAULTTONEAREST : MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
    const auto& area = monitor.rcWork;
    const int avail_w = std::max(S(160), static_cast<int>(area.right - area.left) - S(24));
    const int avail_h = std::max(S(120), static_cast<int>(area.bottom - area.top) - S(24));

    dpi_ = gfx::WindowDpi(hwnd_);
    const int pad = ShadowPad();

    if (view_ == View::Pill) {
        if (IsOfferVisible()) {
            const auto dest = SuggestedId(package_) + L" \u25be";
            const int text_w = static_cast<int>(std::ceil(gfx::MeasureTextWidth(surface_.hdc(), dest, Sf(13.0f), true)));
            target_w_ = std::clamp(S(68) + text_w, S(188), std::min(S(320), avail_w));
            target_h_ = S(34);
        } else {
            target_w_ = orphan_sessions_.empty() ? S(96) : S(116);
            target_h_ = S(32);
        }
    } else if (view_ == View::Search) {
        target_w_ = std::min(S(372), avail_w);
        const int search_h = S(40);
        const int row_h = S(32);
        const int max_rows = std::max(1, (avail_h - search_h - S(18)) / row_h);
        visible_rows_ = std::min(static_cast<int>(items_.size()), max_rows);
        target_h_ = visible_rows_ > 0 ? (search_h + S(8) + visible_rows_ * row_h + S(8)) : search_h;
    } else {
        const auto col_str = config_.config_doc().GetValue(L"Desktop", L"Columns");
        wchar_t* end = nullptr;
        const long parsed_cols = col_str.empty() ? 0 : std::wcstol(col_str.c_str(), &end, 10);
        const int max_cols_fit = std::max(1, (avail_w - S(24)) / S(88));
        const int desired_cols = (!col_str.empty() && end == col_str.c_str() + col_str.size() && parsed_cols > 0)
            ? static_cast<int>(parsed_cols) : 4;
        cols_ = std::clamp(desired_cols, 1, max_cols_fit);
        const int count = std::max(1, static_cast<int>(items_.size()));
        if (col_str.empty()) cols_ = std::clamp(count, 2, cols_);
        const int header_h = S(40);
        const int cell_w = S(86);
        const int cell_h = S(84);
        const int total_rows = (count + cols_ - 1) / cols_;
        const int max_rows = std::max(1, (avail_h - header_h - S(16)) / cell_h);
        visible_rows_ = std::clamp(total_rows, 1, max_rows);
        scroll_row_ = std::clamp(scroll_row_, 0, std::max(0, total_rows - visible_rows_));
        target_w_ = std::clamp(cols_ * cell_w + S(24), S(220), avail_w);
        target_h_ = header_h + visible_rows_ * cell_h + S(12);
    }

    if (!has_pill_anchor_) {
        const auto position = [&](const wchar_t* key, LONG fallback, LONG low, LONG high) {
            if (!remember_pos_) return std::clamp(fallback, low, std::max(low, high));
            const auto value = config_.config_doc().GetValue(L"Desktop", key);
            wchar_t* end = nullptr;
            const LONG parsed = std::wcstol(value.c_str(), &end, 10);
            return std::clamp(!value.empty() && end == value.c_str() + value.size() ? parsed : fallback,
                              low, std::max(low, high));
        };
        const int init_x = position(L"X", area.right - target_w_ - S(18), area.left, area.right - target_w_);
        const int init_y = position(L"Y", area.top + S(14), area.top, area.bottom - target_h_);
        pill_center_x_ = init_x + target_w_ / 2;
        pill_top_y_ = init_y;
        has_pill_anchor_ = true;
    }

    const int draw_w = shown_ ? cur_w_ : target_w_;
    const int draw_h = shown_ ? cur_h_ : target_h_;
    const int x_capsule = std::clamp(pill_center_x_ - draw_w / 2,
        static_cast<int>(area.left), std::max(static_cast<int>(area.left), static_cast<int>(area.right) - draw_w));
    const int y_capsule = std::clamp(pill_top_y_,
        static_cast<int>(area.top), std::max(static_cast<int>(area.top), static_cast<int>(area.bottom) - draw_h));

    win_x_ = x_capsule - pad;
    win_y_ = y_capsule - pad;

    if (!shown_) {
        cur_w_ = target_w_;
        cur_h_ = target_h_;
    }

    ApplyWindowGeometry();

    // Update child control positions and visibility immediately for tests and input handling
    const int search_h = S(40);
    const int header_h = S(40);
    ::MoveWindow(edit_, pad + S(36), pad + S(8), std::max(S(40), target_w_ - S(72)), S(24), FALSE);
    if (view_ == View::Search) {
        ::MoveWindow(list_, pad, pad + search_h, target_w_, std::max(0, target_h_ - search_h), FALSE);
    } else if (view_ == View::Files) {
        ::MoveWindow(list_, pad, pad + header_h, target_w_, std::max(0, target_h_ - header_h), FALSE);
    } else {
        ::MoveWindow(list_, 0, 0, 0, 0, FALSE);
    }

    ::ShowWindow(edit_, view_ == View::Search ? SW_SHOW : SW_HIDE);
    ::ShowWindow(list_, view_ != View::Pill ? SW_SHOW : SW_HIDE);

    Render();

    if (shown_ && (cur_w_ != target_w_ || cur_h_ != target_h_)) {
        ::SetTimer(hwnd_, kAnimTimerId, 16, nullptr);
    }
}

void Desktop::ApplyWindowGeometry() {
    const int pad = ShadowPad();
    ::SetWindowPos(hwnd_, always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                   win_x_, win_y_, cur_w_ + pad * 2, cur_h_ + pad * 2, SWP_NOACTIVATE);
}

void Desktop::Render() {
    if (!hwnd_) return;
    UpdateTrayIcon();
    const int pad = ShadowPad();
    const int total_w = cur_w_ + pad * 2;
    const int total_h = cur_h_ + pad * 2;
    if (!surface_.Resize(total_w, total_h)) return;
    surface_.Clear();

    const HDC hdc = surface_.hdc();
    const RECT capsule{pad, pad, pad + cur_w_, pad + cur_h_};
    const float radius = view_ == View::Pill
        ? static_cast<float>(cur_h_) * 0.5f
        : (view_ == View::Search ? Sf(19.0f) : Sf(18.0f));
    const std::uint8_t base_alpha = static_cast<std::uint8_t>(std::clamp((translucent_pct_ * 255 + 50) / 100, 38, 255));
    const std::uint8_t fill_alpha = view_ == View::Files ? static_cast<std::uint8_t>(std::max(38, base_alpha - 10)) : base_alpha;

    gfx::DrawGlassCapsule(hdc, capsule, radius, fill_alpha, dpi_);

    const bool guard_ok = session_.protected_by_guard();
    const gfx::Color4 led_color = (origin_lost_ || !guard_ok)
        ? gfx::Color4{248, 113, 113, 255}
        : gfx::Color4{52, 211, 153, 255};

    // Reset hit rects
    rect_install_ = {};
    rect_dest_ = {};
    rect_web_ = {};
    rect_back_ = {};
    rect_edit_draw_ = {};
    rect_orphan_ = {};

    if (view_ == View::Pill) {
        if (busy_ || closing_) {
            const bool red_bar = origin_lost_ || !guard_ok || close_mode_ != Mode::Normal;
            const gfx::Color4 bar_color = red_bar ? gfx::Color4{239, 68, 68, 130} : gfx::Color4{52, 211, 153, 125};
            const float progress = closing_ ? 0.85f : (0.25f + 0.65f * (0.5f + 0.5f * std::sin(static_cast<float>(busy_tick_) * 0.25f)));
            gfx::DrawCapsuleProgress(hdc, capsule, radius, progress, bar_color);
        }

        const float led_cx = static_cast<float>(capsule.right) - Sf(18.0f);
        const float led_cy = static_cast<float>(capsule.top + capsule.bottom) * 0.5f;

        if (IsOfferVisible()) {
            const auto id = SuggestedId(package_);
            const bool target_busy = IsAppRunning(id);
            const gfx::Color4 icon_color = target_busy ? gfx::Color4{248, 113, 113, 255} : gfx::Color4{52, 211, 153, 255};

            rect_install_ = {capsule.left + S(6), capsule.top, capsule.left + S(36), capsule.bottom};
            const float icx = static_cast<float>(rect_install_.left + rect_install_.right) * 0.5f;
            const float icy = static_cast<float>(rect_install_.top + rect_install_.bottom) * 0.5f;
            gfx::DrawInstallIcon(hdc, icx, icy, Sf(16.0f), icon_color, hover_zone_ == HitZone::InstallIcon);

            rect_dest_ = {capsule.left + S(36), capsule.top, capsule.right - S(30), capsule.bottom};
            const auto dest_label = id + L" \u25be";
            const gfx::Color4 text_color = hover_zone_ == HitZone::InstallDest
                ? gfx::Color4{255, 255, 255, 255} : gfx::Color4{226, 232, 240, 255};
            gfx::DrawTextLine(hdc, dest_label, rect_dest_, Sf(13.0f), true, text_color, 0);
        } else if (!orphan_sessions_.empty()) {
            const float bx = led_cx - Sf(22.0f);
            rect_orphan_ = {static_cast<LONG>(bx - Sf(10.0f)), capsule.top, static_cast<LONG>(bx + Sf(10.0f)), capsule.bottom};
            gfx::DrawOrphanBadge(hdc, bx, led_cy, Sf(14.0f), hover_zone_ == HitZone::OrphanBadge);
        }

        gfx::DrawGlowingLed(hdc, led_cx, led_cy, Sf(5.2f), led_color, tracking_mouse_ && !IsOfferVisible());
    } else if (view_ == View::Search) {
        const int search_h = S(40);
        const float bar_cy = static_cast<float>(capsule.top) + static_cast<float>(search_h) * 0.5f;
        gfx::DrawSearchIcon(hdc, static_cast<float>(capsule.left) + Sf(20.0f), bar_cy, Sf(15.0f), {148, 163, 184, 220});

        const auto typed = Text(edit_);
        const bool show_web = !typed.empty();
        const int right_margin = show_web ? S(42) : S(16);
        rect_edit_draw_ = {capsule.left + S(36), capsule.top + S(4), capsule.right - right_margin, capsule.top + search_h - S(4)};

        DWORD sel_a = 0, sel_b = 0;
        ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_a), reinterpret_cast<LPARAM>(&sel_b));
        const bool focused = ::GetFocus() == edit_;
        gfx::DrawSearchText(hdc, typed, GhostSuffix(typed), static_cast<int>(sel_a), static_cast<int>(sel_b),
                            caret_visible_, focused, rect_edit_draw_, Sf(14.0f), dpi_);

        if (show_web) {
            rect_web_ = {capsule.right - S(38), capsule.top + S(4), capsule.right - S(8), capsule.top + search_h - S(4)};
            const float wcx = static_cast<float>(rect_web_.left + rect_web_.right) * 0.5f;
            gfx::DrawWebIcon(hdc, wcx, bar_cy, Sf(15.0f), {148, 163, 184, 230}, hover_zone_ == HitZone::WebSearch);
        }

        if (!items_.empty() && cur_h_ > search_h + S(12)) {
            gfx::DrawDivider(hdc, static_cast<float>(capsule.left + S(12)), static_cast<float>(capsule.right - S(12)),
                             static_cast<float>(capsule.top + search_h), 24);
            const int row_h = S(32);
            const int start_y = capsule.top + search_h + S(6);
            const int max_draw = std::min(static_cast<int>(items_.size()),
                                          std::max(0, static_cast<int>(capsule.bottom - start_y - S(6)) / row_h));

            for (int i = 0; i < max_draw; ++i) {
                const int ry = start_y + i * row_h;
                const bool is_sel = (i == selected_);
                const bool is_hov = (hover_zone_ == HitZone::Item && hover_index_ == i);
                if (is_sel || is_hov) {
                    const gfx::Color4 fill = is_sel ? gfx::Color4{30, 41, 59, 210} : gfx::Color4{30, 41, 59, 115};
                    const gfx::Color4 border = is_sel ? gfx::Color4{59, 130, 246, 130} : gfx::Color4{0, 0, 0, 0};
                    gfx::DrawRoundedHighlight(hdc, static_cast<float>(capsule.left + S(8)), static_cast<float>(ry + S(1)),
                                              static_cast<float>(cur_w_ - S(16)), static_cast<float>(row_h - S(2)),
                                              Sf(8.0f), fill, border);
                    if (is_sel) {
                        gfx::DrawAccentBar(hdc, static_cast<float>(capsule.left + S(10)), static_cast<float>(ry + S(7)),
                                           Sf(3.0f), static_cast<float>(row_h - S(14)), {59, 130, 246, 255});
                    }
                }

                const auto& item = items_[static_cast<std::size_t>(i)];
                std::wstring rel = item.value;
                bool has_dot = false;
                gfx::Color4 dot_color{148, 163, 184, 255};
                bool running = false;
                if (item.kind == Kind::App) {
                    has_dot = true;
                    running = IsAppRunning(item.value);
                    if (const auto* app = FindLoadedApp(item.value)) {
                        rel = app->executable;
                        dot_color = PrivacyColor(app->color);
                    } else {
                        rel.clear();
                    }
                }
                const auto resolved = resolver_.ResolvePmRelativePath(rel);
                const int icon_sz = S(20);
                gfx::DrawItemIcon(hdc, resolved.ok ? resolved.resolved_path : L"",
                                  item.kind == Kind::App, item.kind == Kind::Folder,
                                  capsule.left + S(20), ry + (row_h - icon_sz) / 2, icon_sz,
                                  has_dot, dot_color, running, dpi_);

                const RECT text_rect{capsule.left + S(48), ry, capsule.right - S(16), ry + row_h};
                gfx::DrawTextLine(hdc, item.label, text_rect, Sf(13.5f), is_sel,
                                  is_sel ? gfx::Color4{248, 250, 252, 255} : gfx::Color4{226, 232, 240, 255}, -1);
            }
        }
    } else {
        // View::Files (PM-Desktop)
        const int header_h = S(40);
        const bool sub = folder_ != L"Other";
        int title_left = capsule.left + S(16);
        if (sub) {
            rect_back_ = {capsule.left + S(8), capsule.top + S(6), capsule.left + S(34), capsule.top + header_h - S(6)};
            gfx::DrawBackIcon(hdc, static_cast<float>(rect_back_.left + rect_back_.right) * 0.5f,
                              static_cast<float>(capsule.top) + static_cast<float>(header_h) * 0.5f,
                              Sf(14.0f), {148, 163, 184, 230}, hover_zone_ == HitZone::BackButton);
            title_left = capsule.left + S(36);
        }

        const std::wstring title = sub ? folder_ : L"Escritorio";
        const RECT title_rect{title_left, capsule.top, capsule.right - S(42), capsule.top + header_h};
        gfx::DrawTextLine(hdc, title, title_rect, Sf(13.5f), true, {241, 245, 249, 255}, -1);

        // Shield exclusively in PMD top-right
        gfx::DrawStatusShield(hdc, static_cast<float>(capsule.right) - Sf(22.0f),
                              static_cast<float>(capsule.top) + Sf(20.0f), Sf(17.0f), led_color);

        gfx::DrawDivider(hdc, static_cast<float>(capsule.left + S(12)), static_cast<float>(capsule.right - S(12)),
                         static_cast<float>(capsule.top + header_h), 22);

        const int cell_w = S(86);
        const int cell_h = S(84);
        const int grid_x = capsule.left + S(12);
        const int grid_y = capsule.top + header_h + S(4);
        const int first_idx = scroll_row_ * cols_;
        const int max_visible = visible_rows_ * cols_;

        if (items_.empty()) {
            const RECT empty_rect{capsule.left + S(16), grid_y, capsule.right - S(16), capsule.bottom - S(12)};
            gfx::DrawTextLine(hdc, L"Carpeta vac\u00eda", empty_rect, Sf(12.5f), false, {148, 163, 184, 180}, 0);
        } else {
            for (int offset = 0; offset < max_visible && first_idx + offset < static_cast<int>(items_.size()); ++offset) {
                const int idx = first_idx + offset;
                const int row = offset / cols_;
                const int col = offset % cols_;
                const int cx = grid_x + col * cell_w;
                const int cy = grid_y + row * cell_h;
                if (cy + cell_h > capsule.bottom) break;

                const bool is_sel = (idx == selected_);
                const bool is_hov = (hover_zone_ == HitZone::Item && hover_index_ == idx);
                // No permanent card borders around icons; subtle glow only on active hover/focus
                if (is_sel || is_hov) {
                    const gfx::Color4 fill = is_sel ? gfx::Color4{59, 130, 246, 38} : gfx::Color4{255, 255, 255, 18};
                    const gfx::Color4 border = is_sel ? gfx::Color4{96, 165, 250, 120} : gfx::Color4{0, 0, 0, 0};
                    gfx::DrawRoundedHighlight(hdc, static_cast<float>(cx + S(4)), static_cast<float>(cy + S(4)),
                                              static_cast<float>(cell_w - S(8)), static_cast<float>(cell_h - S(8)),
                                              Sf(10.0f), fill, border);
                }

                const auto& item = items_[static_cast<std::size_t>(idx)];
                std::wstring rel = item.value;
                bool has_dot = false;
                gfx::Color4 dot_color{148, 163, 184, 255};
                bool running = false;
                if (item.kind == Kind::App) {
                    has_dot = true;
                    running = IsAppRunning(item.value);
                    if (const auto* app = FindLoadedApp(item.value)) {
                        rel = app->executable;
                        dot_color = PrivacyColor(app->color);
                    } else {
                        rel.clear();
                    }
                }
                const auto resolved = resolver_.ResolvePmRelativePath(rel);
                const int icon_sz = S(36);
                const int ix = cx + (cell_w - icon_sz) / 2;
                const int iy = cy + S(12);
                gfx::DrawItemIcon(hdc, resolved.ok ? resolved.resolved_path : L"",
                                  item.kind == Kind::App, item.kind == Kind::Folder,
                                  ix, iy, icon_sz, has_dot, dot_color, running, dpi_);

                const RECT lbl_rect{cx + S(4), iy + icon_sz + S(5), cx + cell_w - S(4), cy + cell_h - S(2)};
                gfx::DrawGridLabel(hdc, item.label, lbl_rect, Sf(11.5f),
                                   is_sel ? gfx::Color4{255, 255, 255, 255} : gfx::Color4{226, 232, 240, 245});
            }
        }
    }

    POINT dst{win_x_, win_y_};
    SIZE sz{total_w, total_h};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(hwnd_, nullptr, &dst, &sz, hdc, &src, 0, &blend, ULW_ALPHA);
}

HitZone Desktop::HitTest(POINT client, int& item_index) const {
    item_index = -1;
    const auto in = [&](const RECT& r) { return ::PtInRect(&r, client) != FALSE; };
    const int pad = ShadowPad();

    if (view_ == View::Pill) {
        if (IsOfferVisible()) {
            if (in(rect_install_)) return HitZone::InstallIcon;
            if (in(rect_dest_)) return HitZone::InstallDest;
        } else if (!orphan_sessions_.empty() && in(rect_orphan_)) {
            return HitZone::OrphanBadge;
        }
        return HitZone::None;
    }

    if (view_ == View::Search) {
        if (!Text(edit_).empty() && in(rect_web_)) return HitZone::WebSearch;
        const int search_h = S(40);
        const int row_h = S(32);
        const int start_y = pad + search_h + S(6);
        if (client.x >= pad && client.x <= pad + cur_w_ && client.y >= start_y) {
            const int idx = (client.y - start_y) / row_h;
            if (idx >= 0 && idx < visible_rows_ && idx < static_cast<int>(items_.size())) {
                item_index = idx;
                return HitZone::Item;
            }
        }
        return HitZone::None;
    }

    if (folder_ != L"Other" && in(rect_back_)) return HitZone::BackButton;
    const int header_h = S(40);
    const int cell_w = S(86);
    const int cell_h = S(84);
    const int grid_x = pad + S(12);
    const int grid_y = pad + header_h + S(4);
    if (client.x >= grid_x && client.x < grid_x + cols_ * cell_w &&
        client.y >= grid_y && client.y < grid_y + visible_rows_ * cell_h) {
        const int col = (client.x - grid_x) / cell_w;
        const int row = (client.y - grid_y) / cell_h;
        const int idx = (scroll_row_ + row) * cols_ + col;
        if (idx >= 0 && idx < static_cast<int>(items_.size())) {
            item_index = idx;
            return HitZone::Item;
        }
    }
    return HitZone::None;
}

void Desktop::UpdateHover(POINT client) {
    int idx = -1;
    const auto zone = HitTest(client, idx);
    if (zone != hover_zone_ || idx != hover_index_) {
        hover_zone_ = zone;
        hover_index_ = idx;
        if (view_ == View::Search && zone == HitZone::Item && idx >= 0) selected_ = idx;
        Render();
    }
}

void Desktop::EnsureVisibleSelection() {
    if (view_ != View::Files || items_.empty() || cols_ <= 0) return;
    const int row = selected_ / cols_;
    if (row < scroll_row_) scroll_row_ = row;
    else if (row >= scroll_row_ + visible_rows_) scroll_row_ = row - visible_rows_ + 1;
}

void Desktop::MoveSelection(int dx, int dy) {
    if (items_.empty()) return;
    const int count = static_cast<int>(items_.size());
    if (view_ == View::Search) {
        selected_ = std::clamp(selected_ + dy, 0, count - 1);
    } else {
        int next = selected_ + dx + dy * cols_;
        next = std::clamp(next, 0, count - 1);
        selected_ = next;
        EnsureVisibleSelection();
    }
    Render();
}

void Desktop::Show(View view) {
    if (closing_) return;
    view_ = view;
    if (view == View::Pill) ::SetWindowTextW(edit_, L"");
    if (view == View::Files) folder_ = L"Other";
    scroll_row_ = 0;
    selected_ = 0;
    hover_zone_ = HitZone::None;
    hover_index_ = -1;
    if (view != View::Pill) ::SetForegroundWindow(hwnd_);
    Populate();
    if (view == View::Search) {
        caret_visible_ = true;
        ::SetFocus(edit_);
    }
    if (view == View::Files) ::SetFocus(list_);
}

void Desktop::Populate() {
    items_.clear();
    scroll_row_ = 0;
    if (view_ == View::Pill) { Layout(); return; }
    const auto typed = view_ == View::Search ? Text(edit_) : L"";
    if (view_ == View::Search && typed.empty()) {
        selected_ = -1;
        Layout();
        return;
    }
    applications::List(resolver_, apps_);
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
    selected_ = items_.empty() ? -1 : 0;
    Layout();
}

void Desktop::Back() {
    if (view_ == View::Pill) {
        if (!package_.empty()) {
            dismissed_package_ = package_;
            package_.clear();
            Layout();
        }
        return;
    }
    if (view_ == View::Search) {
        if (!Text(edit_).empty()) {
            ::SetWindowTextW(edit_, L"");
            Populate();
        } else {
            Show(View::Pill);
        }
        return;
    }
    if (folder_ == L"Other") { Show(View::Pill); return; }
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
    std::vector<gfx::MenuEntry> menu_items;
    std::vector<App> available;
    for (auto a : apps_) {
        if (a.archived || applications::ValidateApp(resolver_, a, true)) continue;
        available.push_back(a);
        menu_items.push_back({static_cast<int>(available.size()), a.name, true, false});
    }
    constexpr int kHost = 32000;
    menu_items.push_back({kHost, L"Abrir con Windows\u2026", true, false});
    POINT cursor{};
    ::GetCursorPos(&cursor);
    modal_ = true;
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
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
    if (IsAppRunning(app.id)) {
        Report(ERROR_BUSY);
        return;
    }
    auto entry = std::make_unique<Worker>();
    entry->app = app;
    const auto resolved_dir = resolver_.ResolvePmRelativePath(app.directory);
    const auto executable = resolver_.ResolvePmRelativePath(app.executable).resolved_path;
    std::error_code ec;
    const bool no_wrapper = resolved_dir.ok &&
        !fs::exists(fs::path(resolved_dir.resolved_path) / L"wrapper.ini", ec);
    const auto candidates = (no_wrapper && workers_.empty() && session_.protected_by_guard())
        ? learning::LearningEngine::Inspect(executable)
        : std::nullopt;
    entry->learning_run = candidates.has_value();
    entry->event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!entry->event) { Report(::GetLastError()); return; }
    entry->stop.event = entry->event.get();
    auto* worker = entry.get();
    workers_.push_back(std::move(entry));
    try {
        worker->thread = std::thread([this, worker, args = std::move(arguments), executable, candidates] {
            try {
                if (worker->learning_run) {
                    std::wstring full_args = worker->app.arguments;
                    if (!args.empty()) {
                        if (!full_args.empty()) full_args += L" ";
                        full_args += args;
                    }
                    learning::LearningRequest req{
                        worker->app.directory,
                        executable,
                        full_args,
                        worker->app.id,
                        {candidates->roaming, candidates->local, candidates->registry}
                    };
                    worker->learning_result = learning::LearningEngine::Run(resolver_, req);
                } else {
                    worker->result = applications::Run(resolver_, worker->app, args, worker->stop, session_);
                }
            } catch (...) {
                if (worker->learning_run) worker->learning_result.error = ERROR_UNHANDLED_EXCEPTION;
                else worker->result.error = ERROR_UNHANDLED_EXCEPTION;
            }
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
    const bool is_learning = worker->learning_run;
    const auto result = worker->result;
    const auto lr = worker->learning_result;
    const auto uninstall = worker->uninstall_after_close;
    const auto app = worker->app;
    workers_.erase(found);

    if (is_learning) {
        if (lr.error == ERROR_DEVICE_NOT_CONNECTED) {
            if (lr.rollback_error) status_ = 1;
            LoseOrigin();
            return;
        }
        if (!origin_lost_) {
            std::wstring new_color = L"Red";
            if (lr.host_restored && lr.rollback_error == 0 && lr.error == 0) {
                bool has_learned_resources = false;
                core::IniDocument wrapper_doc;
                const auto wrapper_path = lr.wrapper_path.empty()
                    ? (fs::path(resolver_.ResolvePmRelativePath(app.directory).resolved_path) / L"wrapper.ini").native()
                    : lr.wrapper_path;
                const auto read_res = core::AtomicFile::ReadUtf8Text(wrapper_path);
                std::wstring parse_err;
                if (!lr.session_id.empty() && read_res.ok && wrapper_doc.ParseUtf8(read_res.content, parse_err)) {
                    const std::wstring sec = L"Learning." + lr.session_id;
                    bool in_sec = false;
                    for (const auto& line : wrapper_doc.lines()) {
                        if (line.kind == core::IniDocument::LineKind::Section) {
                            in_sec = Equal(line.section_name, sec);
                        } else if (in_sec && line.kind == core::IniDocument::LineKind::KeyValue && !line.value.empty()) {
                            if (Equal(line.key, L"RegistryKey") ||
                                (line.key.size() >= 4 && Equal(line.key.substr(line.key.size() - 4), L"Host"))) {
                                has_learned_resources = true;
                                break;
                            }
                        }
                    }
                }
                new_color = has_learned_resources ? L"Yellow" : L"Green";
            }
            if (config_.LoadAppsIni(false).ok) {
                std::wstring err;
                config_.apps_doc().SetValue(L"App." + app.id, L"Color", new_color, err);
                config_.SaveAppsIniIfModified(false);
            }
        }
        if (lr.error || lr.rollback_error) Report(lr.rollback_error ? lr.rollback_error : lr.error);
        else if (uninstall && !closing_)
            StartOperation([this, app] { return applications::Maintain(resolver_, app, applications::Maintenance::Uninstall); });
        Render();
        CloseWhenReady();
        return;
    }

    if (result.error == ERROR_DEVICE_NOT_CONNECTED) {
        if (result.rollback_error) status_ = 1;
        LoseOrigin();
        return;
    }
    if (result.error || result.rollback_error) Report(result.rollback_error ? result.rollback_error : result.error);
    else if (uninstall && !closing_)
        StartOperation([this, app] { return applications::Maintain(resolver_, app, applications::Maintenance::Uninstall); });
    Render();
    CloseWhenReady();
}

void Desktop::Close(Mode mode) {
    if (!closing_) SavePosition();
    if (!closing_ || mode == Mode::Critical) close_mode_ = mode;
    closing_ = true;
    session_.BeginShutdown();
    view_ = View::Pill;
    for (auto& worker : workers_) {
        worker->stop.mode.store(mode);
        ::SetEvent(worker->event.get());
    }
    ::SetTimer(hwnd_, kBusyTimerId, 30, nullptr);
    Layout();
    CloseWhenReady();
}

void Desktop::CloseWhenReady() {
    if (closing_ && workers_.empty() && !busy_) ::DestroyWindow(hwnd_);
}

void Desktop::LoseOrigin() {
    origin_lost_ = true;
    UpdateTrayIcon();
    Close(Mode::Critical);
    Render();
}

void Desktop::Report(DWORD error) {
    if (error && closing_) status_ = 1;
    if (error && !closing_) {
        const bool was_modal = modal_;
        modal_ = true;
        const auto message = L"No se complet\u00f3 la operaci\u00f3n (Win32 " + std::to_wstring(error) + L").";
        ::MessageBoxW(hwnd_, message.c_str(), L"PortableManager Desktop", MB_OK | MB_ICONWARNING);
        modal_ = was_modal;
    }
    Render();
}

void Desktop::StartOperation(std::function<DWORD()> operation) {
    if (busy_ || closing_) return;
    busy_ = true;
    busy_tick_ = 0;
    ::SetTimer(hwnd_, kBusyTimerId, 30, nullptr);
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
    const bool valid_pkg = attrs != INVALID_FILE_ATTRIBUTES &&
        !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        IsPackage(candidate);
    if (!valid_pkg) {
        package_.clear();
        dismissed_package_.clear();
    } else if (candidate == dismissed_package_) {
        package_.clear();
    } else {
        package_ = std::move(candidate);
    }
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
        std::vector<gfx::MenuEntry> menu_items;
        menu_items.push_back({1, L"+ Nueva instalaci\u00f3n", true, false});
        for (std::size_t i = 0; i < apps_.size(); ++i)
            menu_items.push_back({static_cast<int>(i + 2), apps_[i].name, true, false});
        POINT cursor{};
        ::GetCursorPos(&cursor);
        modal_ = true;
        const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
        modal_ = false;
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
    std::vector<gfx::MenuEntry> menu_items;
    menu_items.push_back({0, L"Seleccionar ejecutable principal", false, false});
    menu_items.push_back({0, L"", false, true});
    for (std::size_t i = 0; i < current->choices.size(); ++i)
        menu_items.push_back({static_cast<int>(i + 1), current->choices[i], true, false});
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(hwnd_);
    modal_ = true;
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
    modal_ = false;
    if (choice > 0 && static_cast<std::size_t>(choice) <= current->choices.size())
        StartInstall(current->id, current->package, current->choices[static_cast<std::size_t>(choice - 1)]);
    else Layout();
    return true;
}

void Desktop::Menu(bool application, int index) {
    if (closing_) return;
    std::vector<gfx::MenuEntry> menu_items;
    App app;
    Worker* worker = nullptr;
    if (application) {
        if (index < 0 || static_cast<std::size_t>(index) >= items_.size() ||
            items_[static_cast<std::size_t>(index)].kind != Kind::App ||
            applications::Find(resolver_, items_[static_cast<std::size_t>(index)].value, app)) {
            return;
        }
        for (auto& entry : workers_) if (entry->app.id == app.id) worker = entry.get();
        menu_items.push_back({1, app.archived ? L"Reinstalar\u2026" : L"Abrir", true, false});
        menu_items.push_back({2, L"Cerrar", worker != nullptr, false});
        menu_items.push_back({3, L"Configurar\u2026", true, false});
        const bool can_maintain = !worker && !busy_;
        menu_items.push_back({4, L"Actualizar\u2026", can_maintain, false});
        menu_items.push_back({5, L"Restablecer\u2026", can_maintain, false});
        menu_items.push_back({6, L"Archivar\u2026", can_maintain, false});
        menu_items.push_back({7, L"Desinstalar\u2026", !busy_, false});
    } else {
        menu_items.push_back({1, L"Escritorio", true, false});
        menu_items.push_back({2, L"Buscar", true, false});
        menu_items.push_back({3, L"Instalar paquete\u2026", true, false});
        if (!package_.empty())
            menu_items.push_back({4, L"Elegir destino del portapapeles\u2026", true, false});
        menu_items.push_back({0, L"", false, true});
        menu_items.push_back({8, L"Configuraci\u00f3n\u2026", !origin_lost_, false});
        if (!orphan_sessions_.empty()) {
            menu_items.push_back({9, L"Recuperar sesiones hu\u00e9rfanas\u2026", !busy_, false});
            menu_items.push_back({10, L"Desechar sesiones hu\u00e9rfanas\u2026", !busy_, false});
        }
        menu_items.push_back({0, L"", false, true});
        menu_items.push_back({5, L"Cierre normal", true, false});
        menu_items.push_back({6, L"Cierre r\u00e1pido", true, false});
        menu_items.push_back({7, L"Cierre cr\u00edtico", true, false});
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(hwnd_);
    modal_ = true;
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
    if (!application) {
        if (choice == 1) Show(View::Files);
        if (choice == 2) Show(View::Search);
        if (choice == 3) PickPackage();
        if (choice == 4) InstallPackage(true);
        if (choice == 8 && !origin_lost_) {
            config_.LoadConfigIni(true);
            EnsureConfigDefaults();
            config_.SaveConfigIniIfModified(true);
            const auto file = resolver_.ResolvePmRelativePath(L"Data\\config.ini");
            const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(
                hwnd_, L"open", file.resolved_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) Report(static_cast<DWORD>(result));
        }
        if (choice == 9) {
            modal_ = false;
            RestoreOrphansInteractive();
            return;
        }
        if (choice == 10) {
            modal_ = false;
            DiscardOrphansInteractive();
            return;
        }
        if (choice >= 5 && choice <= 7)
            Close(choice == 5 ? Mode::Normal : choice == 6 ? Mode::Fast : Mode::Critical);
    } else {
        if (choice == 1) Open(index);
        if (choice == 2) {
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

void Desktop::InitTray() {
    if (!hwnd_ || tray_added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallback;
    tray_is_red_ = origin_lost_ || !session_.protected_by_guard();
    nid.hIcon = tray_is_red_ ? tray_icon_red_ : tray_icon_green_;
    wcscpy_s(nid.szTip, L"PortableManager Lite");
    tray_added_ = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void Desktop::UpdateTrayIcon() {
    if (!tray_added_) return;
    const bool is_red = origin_lost_ || !session_.protected_by_guard();
    if (is_red == tray_is_red_) return;
    tray_is_red_ = is_red;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON;
    nid.hIcon = is_red ? tray_icon_red_ : tray_icon_green_;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Desktop::RemoveTray() {
    if (!tray_added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    tray_added_ = false;
}

void Desktop::RefreshOrphans() {
    orphan_sessions_.clear();
    const auto active_id = session_.session().session_id();
    for (const auto& id : session_.session().DiscoverOrphanSessions()) {
        if (id != active_id) orphan_sessions_.push_back(id);
    }
}

void Desktop::ChooseOrphanAction() {
    if (orphan_sessions_.empty() || busy_ || closing_) return;
    std::vector<gfx::MenuEntry> menu_items;
    menu_items.push_back({1, L"Recuperar sesiones hu\u00e9rfanas\u2026", true, false});
    menu_items.push_back({2, L"Desechar sesiones hu\u00e9rfanas\u2026", true, false});
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(hwnd_);
    modal_ = true;
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
    modal_ = false;
    if (choice == 1) RestoreOrphansInteractive();
    else if (choice == 2) DiscardOrphansInteractive();
}

void Desktop::RestoreOrphansInteractive() {
    if (orphan_sessions_.empty() || busy_ || closing_) return;
    const bool was_modal = modal_;
    modal_ = true;
    const auto msg = L"Se detectaron " + std::to_wstring(orphan_sessions_.size()) +
        L" sesi\u00f3n(es) hu\u00e9rfana(s) sin restaurar en el sistema.\n\n\u00bfDeseas restaurar el anfitri\u00f3n ahora?";
    const int answer = ::MessageBoxW(hwnd_, msg.c_str(), L"PortableManager Lite", MB_YESNO | MB_ICONWARNING);
    modal_ = was_modal;
    if (answer != IDYES) return;

    const auto ids = orphan_sessions_;
    StartOperation([ids]() -> DWORD {
        DWORD err = 0;
        for (const auto& id : ids) {
            runtime::SessionManager s;
            if (!s.OpenExistingSession(id).ok) {
                if (!err) err = ERROR_INVALID_DATA;
                continue;
            }
            const auto r = cleanup::CleanupEngine::Recover(s, true);
            if (!r.ok && !err) err = r.error ? r.error : ERROR_CAN_NOT_COMPLETE;
        }
        return err;
    });
}

void Desktop::DiscardOrphansInteractive() {
    if (orphan_sessions_.empty() || busy_ || closing_) return;
    const bool was_modal = modal_;
    modal_ = true;
    const auto msg = L"Se detectaron " + std::to_wstring(orphan_sessions_.size()) +
        L" sesi\u00f3n(es) hu\u00e9rfana(s) en %TEMP%.\n\nEsta acci\u00f3n eliminar\u00e1 \u00fanicamente los registros temporales sin modificar archivos ni el Registro del equipo.\n\n\u00bfDeseas desecharlas ahora?";
    const int answer = ::MessageBoxW(hwnd_, msg.c_str(), L"PortableManager Lite", MB_YESNO | MB_ICONWARNING);
    modal_ = was_modal;
    if (answer != IDYES) return;

    const auto ids = orphan_sessions_;
    const auto sessions_root = fs::path(runtime::SessionManager::QuerySystemTempDir()) /
        L"PortableManager" / L"Session";
    StartOperation([ids, sessions_root]() -> DWORD {
        DWORD err = 0;
        for (const auto& id : ids) {
            std::error_code ec;
            fs::remove_all(sessions_root / id, ec);
            if (ec && !err) err = static_cast<DWORD>(ec.value() ? ec.value() : ERROR_ACCESS_DENIED);
        }
        return err;
    });
}

} // namespace

int Run(const PathResolver& resolver) {
    return Desktop(resolver).Run();
}

} // namespace pm::pmd
