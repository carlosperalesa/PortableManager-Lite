#include "pmd/pmd_module.h"
#include "pmd/pmd_graphics.h"
#include "applications/app_paths.h"
#include "applications/session_host.h"
#include "cleanup/cleanup_module.h"
#include "core/config_manager.h"
#include "core/pm_version.h"
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
constexpr wchar_t kKillOverlayClass[] = L"PML_KillOverlay";
constexpr wchar_t kNoteHostClass[] = L"PML_NoteHost";
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
enum class Tab { Desktop, Notes, Config };
enum class Kind { App, File, Folder, Url, Kill };
enum class HitZone {
    None, InstallIcon, InstallDest, OrphanBadge, WebSearch, BackButton, Item,
    TopTabDesktop, TopTabNotes, TopTabConfig, NoteTab, NoteClose, NoteAdd, ResizeGrip, ConfigAction
};

struct NoteEntry {
    std::wstring name;
    std::wstring path;
    std::wstring content;
    bool dirty = false;
};

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

std::wstring ExpandWeb(std::wstring_view pattern, const std::wstring& input);

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
    bool IsOfferVisible() const { return view_ == View::Pill && (!package_.empty() || !clipboard_url_.empty()) && !closing_ && !busy_; }
    bool IsUrlOfferVisible() const { return view_ == View::Pill && package_.empty() && !clipboard_url_.empty() && !closing_ && !busy_; }
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
    void Close(Mode mode);
    void CloseWhenReady();
    void LoseOrigin();
    void Finish(Worker* worker);
    void UpdateClipboard();
    void StartUrlNaming();
    void SaveUrlAlias();
    void StartKillPick();
    void LoadNotes();
    void SaveActiveNote();
    void SwitchNote(int index);
    void CreateNote();
    void DeleteNote(int index);
    void StartNoteRename();
    void CommitNoteRename();
    void FindNextInNote();
    void EnsureNoteHost();
    void UpdateNoteHostBounds();
    void PickPackage(std::wstring id = {});
    void InstallPackage(bool choose_destination);
    void StartInstall(std::wstring id, std::wstring package, std::wstring executable = {});
    bool ChooseExecutable();
    void Menu(bool application, int index = -1);
    void Launch(const App& app, std::wstring arguments, bool force_evaluate = false);
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
    Tab tab_ = Tab::Desktop;
    std::wstring folder_ = L"Other";
    std::wstring origin_;
    std::wstring package_;
    std::wstring dismissed_package_;
    std::wstring clipboard_url_;
    std::wstring dismissed_url_;
    bool url_naming_ = false;
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
    RECT rect_tab_desktop_{};
    RECT rect_tab_notes_{};
    RECT rect_tab_config_{};
    RECT rect_note_add_{};
    RECT rect_resize_grip_{};
    std::vector<RECT> rect_note_tabs_;
    std::vector<RECT> rect_note_closes_;
    std::vector<RECT> rect_config_rows_;

    HWND note_host_ = nullptr;
    HWND note_edit_ = nullptr;
    HBRUSH note_bg_brush_ = nullptr;
    HFONT note_font_ = nullptr;
    std::vector<NoteEntry> notes_;
    int active_note_ = 0;
    bool note_renaming_ = false;
    int notes_w_ = 440;
    int notes_h_ = 360;
    bool resizing_notes_ = false;
    POINT resize_start_screen_{};
    int resize_start_w_ = 440;
    int resize_start_h_ = 360;
    bool kill_picking_ = false;

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

    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (edit && self->note_renaming_) {
            self->note_renaming_ = false;
            ::SetWindowTextW(self->edit_, L"");
            self->Layout();
            return 0;
        }
        self->Back();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (edit && self->url_naming_) {
            self->SaveUrlAlias();
            return 0;
        }
        if (edit && self->note_renaming_) {
            self->CommitNoteRename();
            return 0;
        }
        if (edit && self->view_ == View::Files && self->tab_ == Tab::Notes) {
            self->FindNextInNote();
            return 0;
        }
        const auto value = Text(self->edit_);
        if (edit && Equal(value, L"exit")) self->Close(Mode::Normal);
        else if (edit && Equal(value, L"kill")) self->StartKillPick();
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
    if (edit && msg == WM_CHAR && wp == L' ' && Text(control).empty() &&
        self->view_ == View::Search && !self->url_naming_ && !self->note_renaming_) {
        self->Show(View::Files);
        return 0;
    }
    if (edit && msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN)) return 0;
    if (edit && msg == WM_KEYDOWN && !self->items_.empty() &&
        (self->view_ == View::Search || (self->view_ == View::Files && self->tab_ == Tab::Desktop))) {
        if (wp == VK_UP || wp == VK_DOWN) {
            self->MoveSelection(0, wp == VK_UP ? -1 : 1);
            return 0;
        }
        if (self->view_ == View::Files && Text(control).empty() && (wp == VK_LEFT || wp == VK_RIGHT)) {
            self->MoveSelection(wp == VK_LEFT ? -1 : 1, 0);
            return 0;
        }
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
                if (hover_zone_ == HitZone::ResizeGrip) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENWSE));
                    return TRUE;
                }
                const bool clickable = hover_zone_ != HitZone::None || view_ == View::Pill;
                ::SetCursor(::LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (resizing_notes_) {
                POINT cursor{};
                ::GetCursorPos(&cursor);
                const int dx = static_cast<int>(std::lround((cursor.x - resize_start_screen_.x) * 96.0 / dpi_));
                const int dy = static_cast<int>(std::lround((cursor.y - resize_start_screen_.y) * 96.0 / dpi_));
                notes_w_ = std::clamp(resize_start_w_ + dx, 320, 920);
                notes_h_ = std::clamp(resize_start_h_ + dy, 250, 740);
                Layout();
                return 0;
            }
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
                if (zone == HitZone::InstallIcon) {
                    if (IsUrlOfferVisible()) StartUrlNaming();
                    else InstallPackage(false);
                } else if (zone == HitZone::InstallDest) {
                    if (IsUrlOfferVisible()) StartUrlNaming();
                    else InstallPackage(true);
                } else if (zone == HitZone::OrphanBadge) ChooseOrphanAction();
                else Show(View::Search);
                return 0;
            }
            if (view_ == View::Files && tab_ == Tab::Notes && zone == HitZone::ResizeGrip) {
                POINT screen = client;
                ::ClientToScreen(hwnd_, &screen);
                resizing_notes_ = true;
                resize_start_screen_ = screen;
                resize_start_w_ = notes_w_;
                resize_start_h_ = notes_h_;
                ::SetCapture(hwnd_);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (resizing_notes_) {
                resizing_notes_ = false;
                ::ReleaseCapture();
                std::wstring err;
                config_.config_doc().SetValue(L"Desktop", L"NotesWidth", std::to_wstring(notes_w_), err);
                config_.config_doc().SetValue(L"Desktop", L"NotesHeight", std::to_wstring(notes_h_), err);
                config_.SaveConfigIniIfModified(false);
                return 0;
            }
            if (dragging_) {
                dragging_ = false;
                ::ReleaseCapture();
                return 0;
            }
            if (view_ == View::Pill) return 0;
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            const auto zone = HitTest(client, idx);
            if (zone == HitZone::TopTabDesktop) {
                SaveActiveNote();
                note_renaming_ = false;
                tab_ = Tab::Desktop;
                ::SetWindowTextW(edit_, L"");
                Populate();
                ::SetFocus(edit_);
            } else if (zone == HitZone::TopTabNotes) {
                note_renaming_ = false;
                tab_ = Tab::Notes;
                ::SetWindowTextW(edit_, L"");
                LoadNotes();
                Layout();
            } else if (zone == HitZone::TopTabConfig) {
                SaveActiveNote();
                note_renaming_ = false;
                tab_ = Tab::Config;
                ::SetWindowTextW(edit_, L"");
                Layout();
            } else if (zone == HitZone::NoteTab && idx >= 0) {
                if (idx == active_note_) StartNoteRename();
                else SwitchNote(idx);
            } else if (zone == HitZone::NoteClose && idx >= 0) {
                DeleteNote(idx);
            } else if (zone == HitZone::NoteAdd) {
                CreateNote();
            } else if (zone == HitZone::ConfigAction && idx >= 0) {
                auto& doc = config_.config_doc();
                std::wstring err;
                if (idx == 0) {
                    const std::vector<std::wstring> presets{L"Ctrl+Space", L"Alt+Space", L"Ctrl+Shift+Space", L"Ctrl+Alt+Space"};
                    const auto cur = doc.GetValue(L"Desktop", L"Hotkey", L"Ctrl+Space");
                    std::size_t next = 0;
                    for (std::size_t i = 0; i < presets.size(); ++i) {
                        if (Equal(cur, presets[i])) { next = (i + 1) % presets.size(); break; }
                    }
                    doc.SetValue(L"Desktop", L"Hotkey", presets[next], err);
                } else if (idx == 1) {
                    doc.SetValue(L"Desktop", L"AlwaysOnTop", always_on_top_ ? L"false" : L"true", err);
                } else if (idx == 2) {
                    const int next_pct = translucent_pct_ <= 50 ? 100 : (translucent_pct_ - 10);
                    doc.SetValue(L"Desktop", L"Translucent", std::to_wstring(next_pct) + L"%", err);
                } else if (idx == 3) {
                    doc.SetValue(L"Desktop", L"RememberPosition", remember_pos_ ? L"false" : L"true", err);
                } else if (idx == 4) {
                    config_.SaveConfigIniIfModified(true);
                    const auto file = resolver_.ResolvePmRelativePath(L"Data\\config.ini");
                    const auto res = reinterpret_cast<INT_PTR>(::ShellExecuteW(
                        hwnd_, L"open", file.resolved_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                    if (res <= 32) Report(static_cast<DWORD>(res));
                }
                if (idx >= 0 && idx <= 3) {
                    config_.SaveConfigIniIfModified(false);
                    ApplyConfigSettings(true);
                    ApplyWindowGeometry();
                    Render();
                }
            } else if (zone == HitZone::WebSearch) SearchWeb(false);
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
            if (LOWORD(wp) == 1 && HIWORD(wp) == EN_CHANGE) {
                if (view_ == View::Search || (view_ == View::Files && tab_ == Tab::Desktop)) Populate();
                else if (url_naming_ || note_renaming_ || (view_ == View::Files && tab_ == Tab::Notes)) Render();
            }
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
                if (!modal_ && !kill_picking_) {
                    const HWND new_fg = reinterpret_cast<HWND>(lp);
                    if (note_host_ && (new_fg == note_host_ || new_fg == note_edit_)) return 0;
                    SaveActiveNote();
                    if (view_ != View::Pill) Show(View::Pill);
                    else if (url_naming_) {
                        url_naming_ = false;
                        ::SetWindowTextW(edit_, L"");
                        Layout();
                    }
                }
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
            SaveActiveNote();
            if (note_host_) { ::DestroyWindow(note_host_); note_host_ = nullptr; note_edit_ = nullptr; }
            if (note_bg_brush_) { ::DeleteObject(note_bg_brush_); note_bg_brush_ = nullptr; }
            if (note_font_) { ::DeleteObject(note_font_); note_font_ = nullptr; }
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

    const auto parse_dim = [&](const wchar_t* key, int fallback, int low, int high) {
        const auto v = doc.GetValue(L"Desktop", key);
        wchar_t* e = nullptr;
        const long n = v.empty() ? 0 : std::wcstol(v.c_str(), &e, 10);
        return (!v.empty() && e == v.c_str() + v.size()) ? std::clamp(static_cast<int>(n), low, high) : fallback;
    };
    notes_w_ = parse_dim(L"NotesWidth", 440, 320, 920);
    notes_h_ = parse_dim(L"NotesHeight", 360, 250, 740);

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
        if (url_naming_) {
            target_w_ = std::min(S(248), avail_w);
            target_h_ = S(34);
        } else if (IsOfferVisible()) {
            const auto dest = IsUrlOfferVisible()
                ? (SuggestedUrlName(clipboard_url_) + L" (URL)")
                : (SuggestedId(package_) + L" \u25be");
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
    } else if (tab_ == Tab::Notes) {
        target_w_ = std::clamp(S(notes_w_), S(320), avail_w);
        target_h_ = std::clamp(S(notes_h_), S(250), avail_h);
    } else if (tab_ == Tab::Config) {
        target_w_ = std::min(S(388), avail_w);
        target_h_ = S(40) + S(246);
    } else {
        const auto col_str = config_.config_doc().GetValue(L"Desktop", L"Columns");
        wchar_t* end = nullptr;
        const long parsed_cols = col_str.empty() ? 0 : std::wcstol(col_str.c_str(), &end, 10);
        const int max_cols_fit = std::max(1, (avail_w - S(24)) / S(88));
        const int desired_cols = (!col_str.empty() && end == col_str.c_str() + col_str.size() && parsed_cols > 0)
            ? static_cast<int>(parsed_cols) : 4;
        cols_ = std::clamp(desired_cols, 1, max_cols_fit);
        const int count = std::max(1, static_cast<int>(items_.size()));
        if (col_str.empty()) cols_ = std::clamp(count, 3, cols_);
        const int header_h = S(40);
        const int search_h = S(36);
        const int cell_w = S(86);
        const int cell_h = S(84);
        const int total_rows = (count + cols_ - 1) / cols_;
        const int max_rows = std::max(1, (avail_h - header_h - search_h - S(16)) / cell_h);
        visible_rows_ = std::clamp(total_rows, 1, max_rows);
        scroll_row_ = std::clamp(scroll_row_, 0, std::max(0, total_rows - visible_rows_));
        target_w_ = std::clamp(cols_ * cell_w + S(24), S(300), avail_w);
        target_h_ = header_h + search_h + visible_rows_ * cell_h + S(12);
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
    const int desk_search_h = S(36);
    if (view_ == View::Pill && url_naming_) {
        ::MoveWindow(edit_, pad + S(34), pad + S(5), std::max(S(40), target_w_ - S(64)), S(24), FALSE);
    } else if (view_ == View::Files && tab_ == Tab::Desktop) {
        ::MoveWindow(edit_, pad + S(36), pad + header_h + S(6), std::max(S(40), target_w_ - S(72)), S(24), FALSE);
    } else if (view_ == View::Files && tab_ == Tab::Notes) {
        ::MoveWindow(edit_, pad + S(36), pad + target_h_ - S(30), std::max(S(40), target_w_ - S(66)), S(22), FALSE);
    } else {
        ::MoveWindow(edit_, pad + S(36), pad + S(8), std::max(S(40), target_w_ - S(72)), S(24), FALSE);
    }
    if (view_ == View::Search) {
        ::MoveWindow(list_, pad, pad + search_h, target_w_, std::max(0, target_h_ - search_h), FALSE);
    } else if (view_ == View::Files && tab_ == Tab::Desktop) {
        ::MoveWindow(list_, pad, pad + header_h + desk_search_h, target_w_, std::max(0, target_h_ - header_h - desk_search_h), FALSE);
    } else {
        ::MoveWindow(list_, 0, 0, 0, 0, FALSE);
    }

    const bool show_edit = (view_ == View::Search) || url_naming_ ||
        (view_ == View::Files && (tab_ == Tab::Desktop || tab_ == Tab::Notes));
    ::ShowWindow(edit_, show_edit ? SW_SHOW : SW_HIDE);
    ::ShowWindow(list_, (view_ == View::Search || (view_ == View::Files && tab_ == Tab::Desktop)) ? SW_SHOW : SW_HIDE);
    UpdateNoteHostBounds();

    Render();

    if (shown_ && (cur_w_ != target_w_ || cur_h_ != target_h_)) {
        ::SetTimer(hwnd_, kAnimTimerId, 16, nullptr);
    }
}

void Desktop::ApplyWindowGeometry() {
    const int pad = ShadowPad();
    ::SetWindowPos(hwnd_, always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                   win_x_, win_y_, cur_w_ + pad * 2, cur_h_ + pad * 2, SWP_NOACTIVATE);
    UpdateNoteHostBounds();
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

        if (url_naming_) {
            const float wcx = static_cast<float>(capsule.left + S(20));
            gfx::DrawWebIcon(hdc, wcx, led_cy, Sf(15.0f), {52, 211, 153, 255}, false);
            rect_edit_draw_ = {capsule.left + S(34), capsule.top + S(3), capsule.right - S(30), capsule.bottom - S(3)};
            DWORD sel_a = 0, sel_b = 0;
            ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_a), reinterpret_cast<LPARAM>(&sel_b));
            const bool focused = ::GetFocus() == edit_;
            gfx::DrawSearchText(hdc, Text(edit_), L"", static_cast<int>(sel_a), static_cast<int>(sel_b),
                                caret_visible_, focused, rect_edit_draw_, Sf(13.0f), dpi_);
        } else if (IsUrlOfferVisible()) {
            rect_install_ = {capsule.left + S(6), capsule.top, capsule.left + S(36), capsule.bottom};
            const float icx = static_cast<float>(rect_install_.left + rect_install_.right) * 0.5f;
            const float icy = static_cast<float>(rect_install_.top + rect_install_.bottom) * 0.5f;
            gfx::DrawWebIcon(hdc, icx, icy, Sf(15.5f), {52, 211, 153, 255}, hover_zone_ == HitZone::InstallIcon);

            rect_dest_ = {capsule.left + S(36), capsule.top, capsule.right - S(30), capsule.bottom};
            const auto dest_label = SuggestedUrlName(clipboard_url_) + L" (URL)";
            const gfx::Color4 text_color = hover_zone_ == HitZone::InstallDest
                ? gfx::Color4{255, 255, 255, 255} : gfx::Color4{226, 232, 240, 255};
            gfx::DrawTextLine(hdc, dest_label, rect_dest_, Sf(13.0f), true, text_color, 0);
        } else if (IsOfferVisible()) {
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
                if (item.kind == Kind::Url) {
                    const float wcx = static_cast<float>(capsule.left + S(30));
                    const float wcy = static_cast<float>(ry) + static_cast<float>(row_h) * 0.5f;
                    gfx::DrawWebIcon(hdc, wcx, wcy, Sf(15.0f), {52, 211, 153, 235}, is_sel || is_hov);
                } else if (item.kind == Kind::Kill) {
                    const float kcx = static_cast<float>(capsule.left + S(30));
                    const float kcy = static_cast<float>(ry) + static_cast<float>(row_h) * 0.5f;
                    gfx::DrawGlowingLed(hdc, kcx, kcy, Sf(5.5f), {248, 113, 113, 255}, is_sel || is_hov);
                } else {
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
                }

                const RECT text_rect{capsule.left + S(48), ry, capsule.right - S(16), ry + row_h};
                gfx::DrawTextLine(hdc, item.label, text_rect, Sf(13.5f), is_sel,
                                  is_sel ? gfx::Color4{248, 250, 252, 255} : gfx::Color4{226, 232, 240, 255}, -1);
            }
        }
    } else {
        // View::Files (PM-Desktop)
        rect_back_ = {};
        rect_tab_desktop_ = {};
        rect_tab_notes_ = {};
        rect_tab_config_ = {};
        rect_note_add_ = {};
        rect_resize_grip_ = {};
        rect_note_tabs_.clear();
        rect_note_closes_.clear();
        rect_config_rows_.clear();

        const int header_h = S(40);
        const bool sub = (tab_ == Tab::Desktop && folder_ != L"Other");
        int tab_x = capsule.left + S(12);
        if (sub) {
            rect_back_ = {capsule.left + S(8), capsule.top + S(6), capsule.left + S(34), capsule.top + header_h - S(6)};
            gfx::DrawBackIcon(hdc, static_cast<float>(rect_back_.left + rect_back_.right) * 0.5f,
                              static_cast<float>(capsule.top) + static_cast<float>(header_h) * 0.5f,
                              Sf(14.0f), {148, 163, 184, 230}, hover_zone_ == HitZone::BackButton);
            tab_x = capsule.left + S(36);
        }

        auto draw_top_tab = [&](const wchar_t* label, Tab t, HitZone hz, RECT& out_rect) {
            const int tw = static_cast<int>(gfx::MeasureTextWidth(hdc, label, Sf(13.0f), true)) + S(22);
            out_rect = {tab_x, capsule.top + S(6), tab_x + tw, capsule.top + header_h - S(6)};
            const bool active = (tab_ == t);
            const bool hov = (hover_zone_ == hz);
            if (active || hov) {
                const gfx::Color4 fill = active ? gfx::Color4{59, 130, 246, 55} : gfx::Color4{255, 255, 255, 18};
                const gfx::Color4 border = active ? gfx::Color4{96, 165, 250, 140} : gfx::Color4{0, 0, 0, 0};
                gfx::DrawRoundedHighlight(hdc, static_cast<float>(out_rect.left), static_cast<float>(out_rect.top),
                                          static_cast<float>(tw), static_cast<float>(out_rect.bottom - out_rect.top),
                                          Sf(8.0f), fill, border);
            }
            gfx::DrawTextLine(hdc, label, out_rect, Sf(13.0f), active,
                              active ? gfx::Color4{248, 250, 252, 255} : gfx::Color4{148, 163, 184, 235}, 0);
            tab_x += tw + S(4);
        };
        draw_top_tab(sub ? folder_.c_str() : L"Escritorio", Tab::Desktop, HitZone::TopTabDesktop, rect_tab_desktop_);
        draw_top_tab(L"Notas", Tab::Notes, HitZone::TopTabNotes, rect_tab_notes_);
        draw_top_tab(L"Config", Tab::Config, HitZone::TopTabConfig, rect_tab_config_);

        // Shield exclusively in PMD top-right
        gfx::DrawStatusShield(hdc, static_cast<float>(capsule.right) - Sf(22.0f),
                              static_cast<float>(capsule.top) + Sf(20.0f), Sf(17.0f), led_color);

        gfx::DrawDivider(hdc, static_cast<float>(capsule.left + S(12)), static_cast<float>(capsule.right - S(12)),
                         static_cast<float>(capsule.top + header_h), 22);

        if (tab_ == Tab::Desktop) {
            const int desk_search_h = S(36);
            const RECT sb{capsule.left + S(12), capsule.top + header_h + S(4),
                          capsule.right - S(12), capsule.top + header_h + desk_search_h - S(2)};
            gfx::DrawRoundedHighlight(hdc, static_cast<float>(sb.left), static_cast<float>(sb.top),
                                      static_cast<float>(sb.right - sb.left), static_cast<float>(sb.bottom - sb.top),
                                      Sf(8.0f), {15, 23, 42, 135}, {148, 163, 184, 55});
            const float scy = static_cast<float>(sb.top + sb.bottom) * 0.5f;
            gfx::DrawSearchIcon(hdc, static_cast<float>(sb.left + S(16)), scy, Sf(13.5f), {148, 163, 184, 220});

            const auto typed = Text(edit_);
            rect_edit_draw_ = {sb.left + S(30), sb.top + S(2), sb.right - S(8), sb.bottom - S(2)};
            DWORD sel_a = 0, sel_b = 0;
            ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_a), reinterpret_cast<LPARAM>(&sel_b));
            const bool focused = ::GetFocus() == edit_;
            gfx::DrawSearchText(hdc, typed, GhostSuffix(typed), static_cast<int>(sel_a), static_cast<int>(sel_b),
                                caret_visible_, focused, rect_edit_draw_, Sf(13.0f), dpi_);

            const int cell_w = S(86);
            const int cell_h = S(84);
            const int grid_x = capsule.left + S(12);
            const int grid_y = capsule.top + header_h + desk_search_h + S(4);
            const int first_idx = scroll_row_ * cols_;
            const int max_visible = visible_rows_ * cols_;

            if (items_.empty()) {
                const RECT empty_rect{capsule.left + S(16), grid_y, capsule.right - S(16), capsule.bottom - S(12)};
                gfx::DrawTextLine(hdc, typed.empty() ? L"Carpeta vac\u00eda" : L"Sin resultados",
                                  empty_rect, Sf(12.5f), false, {148, 163, 184, 180}, 0);
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
                    if (is_sel || is_hov) {
                        const gfx::Color4 fill = is_sel ? gfx::Color4{59, 130, 246, 38} : gfx::Color4{255, 255, 255, 18};
                        const gfx::Color4 border = is_sel ? gfx::Color4{96, 165, 250, 120} : gfx::Color4{0, 0, 0, 0};
                        gfx::DrawRoundedHighlight(hdc, static_cast<float>(cx + S(4)), static_cast<float>(cy + S(4)),
                                                  static_cast<float>(cell_w - S(8)), static_cast<float>(cell_h - S(8)),
                                                  Sf(10.0f), fill, border);
                    }

                    const auto& item = items_[static_cast<std::size_t>(idx)];
                    const int icon_sz = S(36);
                    const int ix = cx + (cell_w - icon_sz) / 2;
                    const int iy = cy + S(12);
                    if (item.kind == Kind::Url) {
                        gfx::DrawWebIcon(hdc, static_cast<float>(ix) + static_cast<float>(icon_sz) * 0.5f,
                                         static_cast<float>(iy) + static_cast<float>(icon_sz) * 0.5f,
                                         Sf(24.0f), {52, 211, 153, 235}, is_sel || is_hov);
                    } else if (item.kind == Kind::Kill) {
                        gfx::DrawRoundedHighlight(hdc, static_cast<float>(ix + S(4)), static_cast<float>(iy + S(4)),
                                                  static_cast<float>(icon_sz - S(8)), static_cast<float>(icon_sz - S(8)),
                                                  Sf(14.0f), {220, 38, 38, 190}, {248, 113, 113, 255});
                        const RECT xr{ix, iy, ix + icon_sz, iy + icon_sz};
                        gfx::DrawTextLine(hdc, L"\u00d7", xr, Sf(18.0f), true, {255, 255, 255, 255}, 0);
                    } else {
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
                        gfx::DrawItemIcon(hdc, resolved.ok ? resolved.resolved_path : L"",
                                          item.kind == Kind::App, item.kind == Kind::Folder,
                                          ix, iy, icon_sz, has_dot, dot_color, running, dpi_);
                    }

                    const RECT lbl_rect{cx + S(4), iy + icon_sz + S(5), cx + cell_w - S(4), cy + cell_h - S(2)};
                    gfx::DrawGridLabel(hdc, item.label, lbl_rect, Sf(11.5f),
                                       is_sel ? gfx::Color4{255, 255, 255, 255} : gfx::Color4{226, 232, 240, 245});
                }
            }
        } else if (tab_ == Tab::Notes) {
            const int note_tab_h = S(32);
            const int find_h = S(34);
            const int bottom_pad = S(10);
            const int tabs_y = capsule.bottom - bottom_pad - find_h - note_tab_h;
            const int find_y = capsule.bottom - bottom_pad - find_h;

            gfx::DrawDivider(hdc, static_cast<float>(capsule.left + S(12)), static_cast<float>(capsule.right - S(12)),
                             static_cast<float>(tabs_y - S(2)), 20);

            int nx = capsule.left + S(12);
            const int max_tab_x = capsule.right - S(44);
            for (int i = 0; i < static_cast<int>(notes_.size()); ++i) {
                const bool active = (i == active_note_);
                const bool hov = (hover_zone_ == HitZone::NoteTab && hover_index_ == i);
                const int text_w = static_cast<int>(gfx::MeasureTextWidth(
                    hdc, notes_[static_cast<std::size_t>(i)].name, Sf(12.0f), active));
                const int tw = std::clamp(text_w + S(38), S(74), S(140));
                if (nx + tw > max_tab_x) break;
                const RECT tr{nx, tabs_y + S(3), nx + tw, tabs_y + note_tab_h - S(3)};
                rect_note_tabs_.push_back(tr);
                const RECT cr{tr.right - S(22), tr.top + S(2), tr.right - S(4), tr.bottom - S(2)};
                rect_note_closes_.push_back(cr);

                if (active || hov) {
                    const gfx::Color4 fill = active ? gfx::Color4{30, 41, 59, 220} : gfx::Color4{255, 255, 255, 18};
                    const gfx::Color4 border = active ? gfx::Color4{59, 130, 246, 150} : gfx::Color4{0, 0, 0, 0};
                    gfx::DrawRoundedHighlight(hdc, static_cast<float>(tr.left), static_cast<float>(tr.top),
                                              static_cast<float>(tw), static_cast<float>(tr.bottom - tr.top),
                                              Sf(7.0f), fill, border);
                }

                if (active && note_renaming_) {
                    const RECT er{tr.left + S(6), tr.top + S(2), cr.left - S(2), tr.bottom - S(2)};
                    DWORD sel_a = 0, sel_b = 0;
                    ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_a), reinterpret_cast<LPARAM>(&sel_b));
                    gfx::DrawSearchText(hdc, Text(edit_), L"", static_cast<int>(sel_a), static_cast<int>(sel_b),
                                        caret_visible_, ::GetFocus() == edit_, er, Sf(12.0f), dpi_);
                } else {
                    const RECT lr{tr.left + S(8), tr.top, cr.left, tr.bottom};
                    gfx::DrawTextLine(hdc, notes_[static_cast<std::size_t>(i)].name, lr, Sf(12.0f), active,
                                      active ? gfx::Color4{248, 250, 252, 255} : gfx::Color4{148, 163, 184, 235}, -1);
                }

                const bool c_hov = (hover_zone_ == HitZone::NoteClose && hover_index_ == i);
                gfx::DrawTextLine(hdc, L"\u00d7", cr, Sf(12.5f), c_hov,
                                  c_hov ? gfx::Color4{248, 113, 113, 255} : gfx::Color4{148, 163, 184, 210}, 0);
                nx += tw + S(4);
            }

            if (nx + S(28) <= capsule.right - S(12)) {
                rect_note_add_ = {nx, tabs_y + S(3), nx + S(26), tabs_y + note_tab_h - S(3)};
                const bool add_hov = (hover_zone_ == HitZone::NoteAdd);
                if (add_hov) {
                    gfx::DrawRoundedHighlight(hdc, static_cast<float>(rect_note_add_.left), static_cast<float>(rect_note_add_.top),
                                              static_cast<float>(S(26)), static_cast<float>(rect_note_add_.bottom - rect_note_add_.top),
                                              Sf(7.0f), {255, 255, 255, 22}, {0, 0, 0, 0});
                }
                gfx::DrawTextLine(hdc, L"+", rect_note_add_, Sf(15.0f), true, {203, 213, 225, 255}, 0);
            }

            const RECT fb{capsule.left + S(12), find_y + S(3), capsule.right - S(28), find_y + find_h - S(2)};
            gfx::DrawRoundedHighlight(hdc, static_cast<float>(fb.left), static_cast<float>(fb.top),
                                      static_cast<float>(fb.right - fb.left), static_cast<float>(fb.bottom - fb.top),
                                      Sf(8.0f), {15, 23, 42, 135}, {148, 163, 184, 55});
            const float fcy = static_cast<float>(fb.top + fb.bottom) * 0.5f;
            gfx::DrawSearchIcon(hdc, static_cast<float>(fb.left + S(16)), fcy, Sf(13.0f), {148, 163, 184, 220});

            if (!note_renaming_) {
                rect_edit_draw_ = {fb.left + S(30), fb.top + S(2), fb.right - S(8), fb.bottom - S(2)};
                DWORD sel_a = 0, sel_b = 0;
                ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_a), reinterpret_cast<LPARAM>(&sel_b));
                gfx::DrawSearchText(hdc, Text(edit_), L"", static_cast<int>(sel_a), static_cast<int>(sel_b),
                                    caret_visible_, ::GetFocus() == edit_, rect_edit_draw_, Sf(12.5f), dpi_);
            }

            rect_resize_grip_ = {capsule.right - S(24), capsule.bottom - S(24), capsule.right - S(2), capsule.bottom - S(2)};
            const bool grip_hov = (hover_zone_ == HitZone::ResizeGrip || resizing_notes_);
            gfx::DrawTextLine(hdc, L"\u25e2", rect_resize_grip_, Sf(12.0f), false,
                              grip_hov ? gfx::Color4{148, 163, 184, 255} : gfx::Color4{148, 163, 184, 140}, 0);
        } else {
            // Tab::Config
            const auto& cfg = config_.config_doc();
            const std::wstring hotkey_val = cfg.GetValue(L"PMD", L"Hotkey", L"Ctrl+Space");
            const auto bool_str = [&](const wchar_t* key, const wchar_t* def) -> std::wstring {
                const auto v = cfg.GetValue(L"PMD", key, def);
                return (Equal(v, L"true") || v == L"1") ? L"Activado" : L"Desactivado";
            };
            struct CfgRow { const wchar_t* label; std::wstring value; };
            const CfgRow rows[] = {
                {L"Atajo de teclado (Hotkey)", hotkey_val + L"  (clic para cambiar)"},
                {L"Siempre visible (AlwaysOnTop)", bool_str(L"AlwaysOnTop", L"true")},
                {L"Translucidez (Translucent)", bool_str(L"Translucent", L"true")},
                {L"Recordar posici\u00f3n (RememberPosition)", bool_str(L"RememberPosition", L"false")},
                {L"Editar archivo Data\\config.ini", L"Abrir\u2026"},
            };
            const int row_h = S(42);
            int ry = capsule.top + header_h + S(12);
            for (int i = 0; i < 5; ++i) {
                const RECT rr{capsule.left + S(16), ry, capsule.right - S(16), ry + row_h - S(4)};
                rect_config_rows_.push_back(rr);
                const bool hov = (hover_zone_ == HitZone::ConfigAction && hover_index_ == i);
                gfx::DrawRoundedHighlight(hdc, static_cast<float>(rr.left), static_cast<float>(rr.top),
                                          static_cast<float>(rr.right - rr.left), static_cast<float>(rr.bottom - rr.top),
                                          Sf(9.0f), hov ? gfx::Color4{30, 41, 59, 190} : gfx::Color4{15, 23, 42, 110},
                                          hov ? gfx::Color4{59, 130, 246, 130} : gfx::Color4{148, 163, 184, 35});
                const RECT lr{rr.left + S(12), rr.top, rr.right - S(150), rr.bottom};
                const RECT vr{rr.right - S(150), rr.top, rr.right - S(12), rr.bottom};
                gfx::DrawTextLine(hdc, rows[i].label, lr, Sf(12.5f), false, {226, 232, 240, 250}, -1);
                gfx::DrawTextLine(hdc, rows[i].value, vr, Sf(12.0f), true, {96, 165, 250, 255}, 1);
                ry += row_h;
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
        if (url_naming_) return HitZone::None;
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

    if (in(rect_tab_desktop_)) return HitZone::TopTabDesktop;
    if (in(rect_tab_notes_)) return HitZone::TopTabNotes;
    if (in(rect_tab_config_)) return HitZone::TopTabConfig;

    if (tab_ == Tab::Notes) {
        if (in(rect_resize_grip_)) return HitZone::ResizeGrip;
        if (in(rect_note_add_)) return HitZone::NoteAdd;
        for (int i = 0; i < static_cast<int>(rect_note_closes_.size()); ++i) {
            if (in(rect_note_closes_[static_cast<std::size_t>(i)])) {
                item_index = i;
                return HitZone::NoteClose;
            }
        }
        for (int i = 0; i < static_cast<int>(rect_note_tabs_.size()); ++i) {
            if (in(rect_note_tabs_[static_cast<std::size_t>(i)])) {
                item_index = i;
                return HitZone::NoteTab;
            }
        }
        return HitZone::None;
    }

    if (tab_ == Tab::Config) {
        for (int i = 0; i < static_cast<int>(rect_config_rows_.size()); ++i) {
            if (in(rect_config_rows_[static_cast<std::size_t>(i)])) {
                item_index = i;
                return HitZone::ConfigAction;
            }
        }
        return HitZone::None;
    }

    if (folder_ != L"Other" && in(rect_back_)) return HitZone::BackButton;
    const int header_h = S(40);
    const int desk_search_h = S(36);
    const int cell_w = S(86);
    const int cell_h = S(84);
    const int grid_x = pad + S(12);
    const int grid_y = pad + header_h + desk_search_h + S(4);
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
    if (view_ == View::Files && tab_ == Tab::Notes && view != View::Files) {
        SaveActiveNote();
        note_renaming_ = false;
    }
    view_ = view;
    url_naming_ = false;
    if (view == View::Pill) ::SetWindowTextW(edit_, L"");
    if (view == View::Files) {
        tab_ = Tab::Desktop;
        note_renaming_ = false;
        folder_ = L"Other";
    }
    scroll_row_ = 0;
    selected_ = 0;
    hover_zone_ = HitZone::None;
    hover_index_ = -1;
    if (view != View::Pill) ::SetForegroundWindow(hwnd_);
    Populate();
    if (view == View::Search || (view == View::Files && tab_ == Tab::Desktop)) {
        caret_visible_ = true;
        ::SetFocus(edit_);
    }
}

void Desktop::Populate() {
    items_.clear();
    scroll_row_ = 0;
    if (view_ == View::Pill) { Layout(); return; }
    if (view_ == View::Files && tab_ != Tab::Desktop) { Layout(); return; }
    const auto typed = Text(edit_);
    if (view_ == View::Search && typed.empty()) {
        selected_ = -1;
        Layout();
        return;
    }
    if (!typed.empty() && Prefix(L"kill", typed)) {
        items_.push_back({Kind::Kill, L"kill \u2014 Matar ventana con un clic", L"kill", {}});
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
    if (view_ == View::Search || (view_ == View::Files && folder_ == L"Other" && !typed.empty())) {
        config_.LoadAppsIni(false);
        const auto& apps_doc = config_.apps_doc();
        std::vector<Item> url_exact, url_partial;
        for (const auto& line : apps_doc.lines()) {
            if (line.kind != core::IniDocument::LineKind::Section || !Prefix(line.section_name, L"Url.")) continue;
            const auto name = line.section_name.substr(4);
            const auto url = apps_doc.GetValue(line.section_name, L"URL");
            if (name.empty() || url.empty()) continue;
            Item item{Kind::Url, name, url, {}};
            if (Equal(typed, name) || (Prefix(typed, name) && typed.size() > name.size() && typed[name.size()] == L' ')) {
                if (typed.size() > name.size()) item.args = typed.substr(name.size() + 1);
                url_exact.push_back(std::move(item));
            } else if (Prefix(name, typed)) {
                url_partial.push_back(std::move(item));
            }
        }
        items_.insert(items_.end(), url_exact.begin(), url_exact.end());
        items_.insert(items_.end(), url_partial.begin(), url_partial.end());
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
        if (url_naming_) {
            url_naming_ = false;
            ::SetWindowTextW(edit_, L"");
        }
        if (!clipboard_url_.empty()) {
            dismissed_url_ = clipboard_url_;
            clipboard_url_.clear();
            Layout();
        }
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
    if (tab_ == Tab::Notes) {
        if (note_renaming_) {
            note_renaming_ = false;
            ::SetWindowTextW(edit_, L"");
            if (note_edit_) ::SetFocus(note_edit_);
            Render();
            return;
        }
        if (!Text(edit_).empty()) {
            ::SetWindowTextW(edit_, L"");
            Render();
            return;
        }
        SaveActiveNote();
        Show(View::Pill);
        return;
    }
    if (tab_ == Tab::Config) {
        Show(View::Pill);
        return;
    }
    if (!Text(edit_).empty()) {
        ::SetWindowTextW(edit_, L"");
        Populate();
        return;
    }
    if (folder_ == L"Other") { Show(View::Pill); return; }
    folder_.resize(folder_.rfind(L'\\'));
    Populate();
}

void Desktop::Open(int index) {
    if (closing_ || index < 0 || static_cast<std::size_t>(index) >= items_.size()) return;
    const auto item = items_[static_cast<std::size_t>(index)];
    if (item.kind == Kind::Kill) {
        StartKillPick();
    } else if (item.kind == Kind::Folder) {
        view_ = View::Files;
        tab_ = Tab::Desktop;
        folder_ = item.value;
        ::SetWindowTextW(edit_, L"");
        Populate();
        ::SetFocus(edit_);
    } else if (item.kind == Kind::File) {
        BrowseDocument(item);
    } else if (item.kind == Kind::Url) {
        const auto final_url = item.value.find(L"$W$") != std::wstring::npos
            ? ExpandWeb(item.value, item.args) : item.value;
        if (!IsHttpUrl(final_url)) { Report(ERROR_INVALID_NAME); return; }
        applications::List(resolver_, apps_);
        const auto browser = std::find_if(apps_.begin(), apps_.end(), [](const App& a) { return a.browser && !a.archived; });
        if (browser != apps_.end()) {
            Launch(*browser, jobs::QuoteArgument(final_url));
        } else {
            const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(
                hwnd_, L"open", final_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) Report(static_cast<DWORD>(result));
            else Show(View::Pill);
        }
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

void Desktop::Launch(const App& app, std::wstring arguments, bool force_evaluate) {
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
    const auto candidates = ((no_wrapper || force_evaluate) && workers_.empty() && session_.protected_by_guard())
        ? std::make_optional(learning::LearningEngine::InspectWithFallback(executable, app.id))
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
                        {candidates->roaming, candidates->local, candidates->registry},
                        true
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
                const auto resolved_wrapper = lr.wrapper_path.empty()
                    ? resolver_.ResolvePmRelativePath(app.directory + L"\\wrapper.ini")
                    : resolver_.ResolvePmRelativePath(lr.wrapper_path);
                const auto wrapper_path = resolved_wrapper.ok
                    ? resolved_wrapper.resolved_path
                    : (fs::path(resolver_.ResolvePmRelativePath(app.directory).resolved_path) / L"wrapper.ini").native();
                const auto read_res = core::AtomicFile::ReadUtf8Text(wrapper_path);
                std::wstring parse_err;
                if (read_res.ok && wrapper_doc.ParseUtf8(read_res.content, parse_err)) {
                    bool in_learning_sec = false;
                    for (const auto& line : wrapper_doc.lines()) {
                        if (line.kind == core::IniDocument::LineKind::Section) {
                            in_learning_sec = Prefix(line.section_name, L"Learning.");
                        } else if (in_learning_sec && line.kind == core::IniDocument::LineKind::KeyValue && !line.value.empty()) {
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
    std::wstring text_candidate;
    if (candidate.empty()) {
        if (const auto text_mem = static_cast<HGLOBAL>(::GetClipboardData(CF_UNICODETEXT))) {
            if (const auto* raw = static_cast<const wchar_t*>(::GlobalLock(text_mem))) {
                text_candidate = raw;
                ::GlobalUnlock(text_mem);
            }
        }
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
        clipboard_url_.clear();
        url_naming_ = false;
    }

    while (!text_candidate.empty() && (text_candidate.front() == L' ' || text_candidate.front() == L'\t' ||
           text_candidate.front() == L'\r' || text_candidate.front() == L'\n'))
        text_candidate.erase(text_candidate.begin());
    while (!text_candidate.empty() && (text_candidate.back() == L' ' || text_candidate.back() == L'\t' ||
           text_candidate.back() == L'\r' || text_candidate.back() == L'\n'))
        text_candidate.pop_back();

    const bool valid_url = !valid_pkg && IsHttpUrl(text_candidate);
    if (!valid_url) {
        clipboard_url_.clear();
        dismissed_url_.clear();
        if (url_naming_) {
            url_naming_ = false;
            ::SetWindowTextW(edit_, L"");
        }
    } else if (text_candidate == dismissed_url_) {
        clipboard_url_.clear();
    } else {
        if (clipboard_url_ != text_candidate) url_naming_ = false;
        clipboard_url_ = std::move(text_candidate);
    }
    Layout();
}

void Desktop::StartUrlNaming() {
    if (!IsUrlOfferVisible()) return;
    url_naming_ = true;
    const auto suggested = SuggestedUrlName(clipboard_url_);
    ::SetWindowTextW(edit_, suggested.c_str());
    ::SendMessageW(edit_, EM_SETSEL, 0, -1);
    Layout();
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(edit_);
}

void Desktop::SaveUrlAlias() {
    if (!url_naming_ || clipboard_url_.empty() || origin_lost_) return;
    const auto name = SanitizeUrlName(Text(edit_));
    if (name.empty()) {
        Report(ERROR_INVALID_NAME);
        return;
    }
    config_.LoadAppsIni(true);
    std::wstring error;
    const auto section = L"Url." + name;
    if (!config_.apps_doc().SetValue(section, L"URL", clipboard_url_, error) ||
        !config_.SaveAppsIniIfModified(true).ok) {
        Report(ERROR_WRITE_FAULT);
        return;
    }
    dismissed_url_ = clipboard_url_;
    clipboard_url_.clear();
    url_naming_ = false;
    ::SetWindowTextW(edit_, L"");
    Populate();
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
        const bool can_maintain = !worker && !busy_;
        menu_items.push_back({8, L"Evaluar", can_maintain && !app.archived && session_.protected_by_guard(), false});
        menu_items.push_back({3, L"Configurar\u2026", true, false});
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
        menu_items.push_back({0, L"", false, true});
        menu_items.push_back({0, std::wstring(L"v") + core::kVersionString, false, false});
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
        if (choice == 8) {
            modal_ = false;
            Launch(app, {}, true);
            return;
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

void Desktop::StartKillPick() {
    if (closing_) return;
    Show(View::Pill);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kKillOverlayClass;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            if (m == WM_KEYDOWN && w == VK_ESCAPE) {
                ::DestroyWindow(h);
                return 0;
            }
            if (m == WM_RBUTTONUP) {
                ::DestroyWindow(h);
                return 0;
            }
            if (m == WM_LBUTTONUP) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ::SetWindowLongPtrW(h, GWLP_USERDATA, 1);
                ::DestroyWindow(h);
                if (HWND hit = ::WindowFromPoint(pt)) {
                    HWND root = ::GetAncestor(hit, GA_ROOT);
                    if (!root) root = hit;
                    wchar_t cls[64]{};
                    ::GetClassNameW(root, cls, 64);
                    const std::wstring_view c(cls);
                    if (c != L"Shell_TrayWnd" && c != L"Progman" && c != L"WorkerW" && c != L"DV2ControlHost") {
                        DWORD pid = 0;
                        ::GetWindowThreadProcessId(root, &pid);
                        if (pid && pid != ::GetCurrentProcessId()) {
                            if (HANDLE proc = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
                                ::TerminateProcess(proc, 1);
                                ::CloseHandle(proc);
                            }
                        }
                    }
                }
                return 0;
            }
            return ::DefWindowProcW(h, m, w, l);
        };
        ::RegisterClassExW(&wc);
        registered = true;
    }

    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    ::ShowWindow(hwnd_, SW_HIDE);
    HWND overlay = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kKillOverlayClass, L"", WS_POPUP | WS_VISIBLE,
        vx, vy, vw, vh, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (overlay) {
        ::SetLayeredWindowAttributes(overlay, 0, 1, LWA_ALPHA);
        ::SetForegroundWindow(overlay);
        ::SetCapture(overlay);
        MSG msg{};
        while (::IsWindow(overlay) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::ReleaseCapture();
    }
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Render();
}

void Desktop::EnsureNoteHost() {
    if (note_host_) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kNoteHostClass;
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            auto* self = reinterpret_cast<Desktop*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
            if (m == WM_CTLCOLOREDIT && self && self->note_bg_brush_) {
                HDC hdc = reinterpret_cast<HDC>(w);
                ::SetTextColor(hdc, RGB(241, 245, 249));
                ::SetBkColor(hdc, RGB(15, 23, 42));
                return reinterpret_cast<LRESULT>(self->note_bg_brush_);
            }
            if (m == WM_KEYDOWN && w == VK_ESCAPE && self) {
                self->Back();
                return 0;
            }
            return ::DefWindowProcW(h, m, w, l);
        };
        ::RegisterClassExW(&wc);
        registered = true;
    }

    if (!note_bg_brush_) note_bg_brush_ = ::CreateSolidBrush(RGB(15, 23, 42));
    if (!note_font_) {
        note_font_ = ::CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    const DWORD ex = WS_EX_TOOLWINDOW | (always_on_top_ ? WS_EX_TOPMOST : 0);
    note_host_ = ::CreateWindowExW(ex, kNoteHostClass, L"", WS_POPUP | WS_CLIPCHILDREN,
                                   0, 0, 100, 100, hwnd_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (note_host_) {
        ::SetWindowLongPtrW(note_host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        note_edit_ = ::CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
            0, 0, 100, 100, note_host_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (note_edit_ && note_font_) {
            ::SendMessageW(note_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(note_font_), TRUE);
        }
    }
}

void Desktop::UpdateNoteHostBounds() {
    if (view_ == View::Files && tab_ == Tab::Notes) {
        EnsureNoteHost();
        if (!note_host_ || !note_edit_) return;
        const int pad = ShadowPad();
        const int header_h = S(40);
        const int note_tab_h = S(32);
        const int find_h = S(34);
        const int bottom_pad = S(10);
        const int nx = win_x_ + pad + S(14);
        const int ny = win_y_ + pad + header_h + S(8);
        const int nw = std::max(S(120), cur_w_ - S(28));
        const int nh = std::max(S(60), cur_h_ - header_h - note_tab_h - find_h - bottom_pad - S(16));
        ::SetWindowPos(note_host_, always_on_top_ ? HWND_TOPMOST : HWND_TOP,
                       nx, ny, nw, nh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::MoveWindow(note_edit_, 0, 0, nw, nh, TRUE);
    } else if (note_host_) {
        ::ShowWindow(note_host_, SW_HIDE);
    }
}

void Desktop::LoadNotes() {
    notes_.clear();
    const auto dir_res = resolver_.ResolvePmRelativePath(L"Data\\notes");
    if (!dir_res.ok) return;
    std::error_code ec;
    fs::create_directories(dir_res.resolved_path, ec);
    for (fs::directory_iterator it(dir_res.resolved_path, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (Equal(it->path().extension().native(), L".txt")) {
            notes_.push_back({it->path().stem().native(), it->path().native(), {}, false});
        }
    }
    std::sort(notes_.begin(), notes_.end(), [](const NoteEntry& a, const NoteEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    if (notes_.empty()) {
        const auto p = (fs::path(dir_res.resolved_path) / L"Nota 1.txt").native();
        core::AtomicFile::WriteUtf8Text(p, "");
        notes_.push_back({L"Nota 1", p, {}, false});
    }
    active_note_ = std::clamp(active_note_, 0, static_cast<int>(notes_.size()) - 1);
    SwitchNote(active_note_);
}

void Desktop::SaveActiveNote() {
    if (!note_edit_ || active_note_ < 0 || static_cast<std::size_t>(active_note_) >= notes_.size()) return;
    const auto raw = Text(note_edit_);
    std::wstring norm;
    norm.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r' && i + 1 < raw.size() && raw[i + 1] == L'\n') continue;
        norm.push_back(raw[i]);
    }
    core::AtomicFile::WriteUtf8Text(notes_[static_cast<std::size_t>(active_note_)].path,
                                    core::IniDocument::WideToUtf8(norm));
}

void Desktop::SwitchNote(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size()) return;
    if (note_edit_ && active_note_ >= 0 && static_cast<std::size_t>(active_note_) < notes_.size() && index != active_note_) {
        SaveActiveNote();
    }
    active_note_ = index;
    EnsureNoteHost();
    if (!note_edit_) return;
    const auto read_res = core::AtomicFile::ReadUtf8Text(notes_[static_cast<std::size_t>(index)].path);
    const auto wide = read_res.ok ? core::IniDocument::Utf8ToWide(read_res.content) : L"";
    std::wstring crlf;
    crlf.reserve(wide.size() + 16);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r')) crlf.push_back(L'\r');
        crlf.push_back(wide[i]);
    }
    ::SetWindowTextW(note_edit_, crlf.c_str());
    ::SendMessageW(note_edit_, EM_SETSEL, 0, 0);
    Render();
}

void Desktop::CreateNote() {
    SaveActiveNote();
    const auto dir_res = resolver_.ResolvePmRelativePath(L"Data\\notes");
    if (!dir_res.ok) return;
    std::error_code ec;
    fs::create_directories(dir_res.resolved_path, ec);
    int n = static_cast<int>(notes_.size()) + 1;
    fs::path p;
    std::wstring title;
    do {
        title = L"Nota " + std::to_wstring(n++);
        p = fs::path(dir_res.resolved_path) / (title + L".txt");
    } while (fs::exists(p, ec));
    core::AtomicFile::WriteUtf8Text(p.native(), "");
    notes_.push_back({title, p.native(), {}, false});
    SwitchNote(static_cast<int>(notes_.size()) - 1);
    if (note_edit_) ::SetFocus(note_edit_);
}

void Desktop::DeleteNote(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= notes_.size()) return;
    bool has_text = false;
    if (index == active_note_ && note_edit_) {
        has_text = !Text(note_edit_).empty();
    } else {
        const auto r = core::AtomicFile::ReadUtf8Text(notes_[static_cast<std::size_t>(index)].path);
        has_text = r.ok && !r.content.empty();
    }
    if (has_text) {
        const bool old_modal = modal_;
        modal_ = true;
        const auto msg = L"\u00bfEliminar la nota \"" + notes_[static_cast<std::size_t>(index)].name + L"\"?";
        const int ans = ::MessageBoxW(hwnd_, msg.c_str(), L"Notas", MB_YESNO | MB_ICONQUESTION);
        modal_ = old_modal;
        if (ans != IDYES) return;
    }
    std::error_code ec;
    fs::remove(notes_[static_cast<std::size_t>(index)].path, ec);
    notes_.erase(notes_.begin() + index);
    if (notes_.empty()) {
        const auto dir_res = resolver_.ResolvePmRelativePath(L"Data\\notes");
        const auto p = (fs::path(dir_res.resolved_path) / L"Nota 1.txt").native();
        core::AtomicFile::WriteUtf8Text(p, "");
        notes_.push_back({L"Nota 1", p, {}, false});
        active_note_ = 0;
    } else if (active_note_ >= static_cast<int>(notes_.size())) {
        active_note_ = static_cast<int>(notes_.size()) - 1;
    }
    SwitchNote(active_note_);
}

void Desktop::StartNoteRename() {
    if (active_note_ < 0 || static_cast<std::size_t>(active_note_) >= notes_.size()) return;
    note_renaming_ = true;
    ::SetWindowTextW(edit_, notes_[static_cast<std::size_t>(active_note_)].name.c_str());
    ::SendMessageW(edit_, EM_SETSEL, 0, -1);
    ::SetFocus(edit_);
    Render();
}

void Desktop::CommitNoteRename() {
    if (!note_renaming_) return;
    note_renaming_ = false;
    std::wstring next = Text(edit_);
    ::SetWindowTextW(edit_, L"");
    if (!next.empty() && active_note_ >= 0 && static_cast<std::size_t>(active_note_) < notes_.size()) {
        for (wchar_t& ch : next) {
            if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
                ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') ch = L'_';
        }
        auto& note = notes_[static_cast<std::size_t>(active_note_)];
        if (next != note.name) {
            SaveActiveNote();
            const fs::path old_p(note.path);
            const fs::path new_p = old_p.parent_path() / (next + L".txt");
            std::error_code ec;
            if (!fs::exists(new_p, ec)) {
                fs::rename(old_p, new_p, ec);
                if (!ec) {
                    note.name = next;
                    note.path = new_p.native();
                }
            }
        }
    }
    if (note_edit_) ::SetFocus(note_edit_);
    Render();
}

void Desktop::FindNextInNote() {
    if (!note_edit_) return;
    const auto needle = Text(edit_);
    if (needle.empty()) return;
    const auto hay = Text(note_edit_);
    if (hay.empty()) return;
    auto lower = [](std::wstring s) {
        ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
        return s;
    };
    const auto h_low = lower(hay);
    const auto n_low = lower(needle);
    DWORD sel_a = 0, sel_b = 0;
    ::SendMessageW(note_edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_a), reinterpret_cast<LPARAM>(&sel_b));
    std::size_t pos = h_low.find(n_low, sel_b);
    if (pos == std::wstring::npos) pos = h_low.find(n_low, 0);
    if (pos != std::wstring::npos) {
        ::SendMessageW(note_edit_, EM_SETSEL, static_cast<WPARAM>(pos), static_cast<LPARAM>(pos + n_low.size()));
        ::SendMessageW(note_edit_, EM_SCROLLCARET, 0, 0);
    }
}

} // namespace

int Run(const PathResolver& resolver) {
    return Desktop(resolver).Run();
}

} // namespace pm::pmd
