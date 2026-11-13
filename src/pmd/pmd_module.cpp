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
enum class DesktopViewMode { LargeIcons, SmallIcons, List, Details };
enum class DesktopSortBy { Name, Modified };
enum class DesktopGroupBy { AppsFirst, FoldersFirst };
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
    std::uint64_t modified_time = 0;
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
    void OpenTab(Tab tab);
    void DesktopContextMenu();
    void HandleHotkeyCapture(WPARAM vk);
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
    void OpenTargetUrl(const std::wstring& target);
    void SearchWeb(bool fallback);
    void ConfigureAppModal(const App& app);
    void CommitAppFieldEdit();
    void RegisterManualApp(const std::wstring& rel_exe);
    void ToggleConfigRow(int idx);
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

    enum class UiNode {
        Pill,
        Search,
        ModuleBar,
        DesktopGrid,
        NotesTabs,
        NoteEditor,
        ConfigList,
        ConfigFieldEdit,
    };

    void FocusNode(UiNode node);
    HitZone HitTest(POINT client, int& item_index) const;
    void UpdateHover(POINT client);
    void EnsureVisibleSelection();
    void MoveSelection(int dx, int dy);
    static LRESULT CALLBACK NoteEditProc(HWND control, UINT msg, WPARAM wp, LPARAM lp);

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
    HWND note_find_edit_ = nullptr;
    WNDPROC note_find_proc_ = nullptr;
    WNDPROC note_edit_proc_ = nullptr;

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
    int translucent_pct_ = 10;
    bool remember_pos_ = true;
    bool clipboard_offers_ = true;
    bool enable_kill_ = true;
    bool show_all_close_modes_ = false;
    UiNode focus_node_ = UiNode::Pill;
    int config_selected_ = 0;
    bool app_cfg_editing_ = false;
    int app_cfg_field_ = 0; // 1: Name/Aliases, 2: Extensions, 3: Arguments
    App app_cfg_target_{};
    std::wstring config_app_id_;
    DesktopViewMode desk_view_ = DesktopViewMode::LargeIcons;
    DesktopSortBy desk_sort_ = DesktopSortBy::Name;
    DesktopGroupBy desk_group_ = DesktopGroupBy::AppsFirst;
    bool hotkey_capturing_ = false;
    bool hotkey_status_ok_ = false;
    bool hotkey_status_err_ = false;
    std::wstring hotkey_error_text_;
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
    RECT rect_note_find_{};
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
    wc.style = CS_DBLCLKS;
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

LRESULT CALLBACK Desktop::NoteEditProc(HWND control, UINT msg, WPARAM wp, LPARAM lp) {
    HWND host = ::GetParent(control);
    auto* self = reinterpret_cast<Desktop*>(::GetWindowLongPtrW(host, GWLP_USERDATA));
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && self) {
        self->SaveActiveNote();
        self->Show(View::Pill);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_UP && self) {
        DWORD start = 0;
        ::SendMessageW(control, EM_GETSEL, reinterpret_cast<WPARAM>(&start), 0);
        const LRESULT line = ::SendMessageW(control, EM_LINEFROMCHAR, static_cast<WPARAM>(start), 0);
        if (line == 0) {
            self->SaveActiveNote();
            self->FocusNode(self->notes_.empty() ? UiNode::ModuleBar : UiNode::NotesTabs);
            return 0;
        }
    }
    if (msg == WM_CHAR && wp == VK_ESCAPE) return 0;
    return self && self->note_edit_proc_
        ? ::CallWindowProcW(self->note_edit_proc_, control, msg, wp, lp)
        : ::DefWindowProcW(control, msg, wp, lp);
}

LRESULT CALLBACK Desktop::InputProc(HWND control, UINT msg, WPARAM wp, LPARAM lp) {
    const auto parent = ::GetParent(control);
    auto* self = reinterpret_cast<Desktop*>(::GetWindowLongPtrW(parent, GWLP_USERDATA));
    const bool edit = control == self->edit_;
    const bool note_find = control == self->note_find_edit_;
    const auto base = edit ? self->edit_proc_ : (note_find ? self->note_find_proc_ : self->list_proc_);

    if (control == self->edit_ || control == self->list_) {
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            ::BeginPaint(control, &ps);
            ::EndPaint(control, &ps);
            return 0;
        }
    }

    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (edit && self->app_cfg_editing_) {
            self->app_cfg_editing_ = false;
            ::SetWindowTextW(self->edit_, L"");
            self->FocusNode(UiNode::ConfigList);
            return 0;
        }
        if (edit && self->note_renaming_) {
            self->note_renaming_ = false;
            ::SetWindowTextW(self->edit_, L"");
            if (self->note_edit_) ::SetFocus(self->note_edit_);
            self->Render();
            return 0;
        }
        if (note_find) {
            if (!Text(control).empty()) {
                ::SetWindowTextW(control, L"");
                self->Render();
            } else {
                self->SaveActiveNote();
                self->Show(View::Pill);
            }
            return 0;
        }
        self->Back();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (edit && self->app_cfg_editing_) {
            self->CommitAppFieldEdit();
            return 0;
        }
        if (edit && self->url_naming_) {
            self->SaveUrlAlias();
            return 0;
        }
        if (edit && self->note_renaming_) {
            self->CommitNoteRename();
            return 0;
        }
        if (note_find) {
            self->FindNextInNote();
            return 0;
        }
        const auto value = Text(self->edit_);
        if (edit && (Equal(value, L"exit") || Equal(value, L"salir") || Equal(value, L"quit"))) self->Close(Mode::Normal);
        else if (edit && self->enable_kill_ && Equal(value, L"kill")) self->StartKillPick();
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
            if (edit && self->items_.empty() && !value.empty()) {
                wchar_t expanded[2048]{};
                const DWORD n = ::ExpandEnvironmentStringsW(value.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
                const std::wstring target = (n > 0 && n <= std::size(expanded)) ? std::wstring(expanded) : value;
                std::error_code ec;
                if (IsHttpUrl(target)) {
                    self->OpenTargetUrl(target);
                    return 0;
                }
                if (fs::exists(target, ec)) {
                    const auto res = reinterpret_cast<INT_PTR>(::ShellExecuteW(
                        self->hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                    if (res <= 32) self->Report(static_cast<DWORD>(res));
                    else self->Show(View::Pill);
                    return 0;
                }
            }
            if (self->selected_ < 0 && edit) self->SearchWeb(true);
            else if (self->items_.empty() && edit) self->SearchWeb(true);
            else self->Open(self->selected_);
        }
        return 0;
    }
    if (edit && msg == WM_KEYDOWN && wp == VK_F5 && self->view_ == View::Files && self->tab_ == Tab::Desktop) {
        self->Populate();
        return 0;
    }
    if ((edit || note_find) && msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN)) return 0;
    if (edit && msg == WM_KEYDOWN && !self->app_cfg_editing_ && !self->url_naming_ && !self->note_renaming_) {
        if (self->view_ == View::Search) {
            if (wp == VK_DOWN) {
                if (!self->items_.empty()) self->MoveSelection(0, 1);
                else if (Text(control).empty()) {
                    self->Show(View::Files);
                    self->FocusNode(UiNode::ModuleBar);
                }
                return 0;
            }
            if (wp == VK_UP) {
                if (self->items_.empty() || self->selected_ <= 0) self->Show(View::Pill);
                else self->MoveSelection(0, -1);
                return 0;
            }
        } else if (self->view_ == View::Files && self->tab_ == Tab::Desktop) {
            if (wp == VK_DOWN) {
                if (Text(control).empty()) {
                    self->FocusNode(UiNode::ModuleBar);
                } else if (!self->items_.empty()) {
                    self->MoveSelection(0, 1);
                }
                return 0;
            }
            if (wp == VK_UP) {
                if (Text(control).empty()) {
                    self->Show(View::Search);
                } else if (!self->items_.empty()) {
                    self->MoveSelection(0, -1);
                }
                return 0;
            }
            if (Text(control).empty() && !self->items_.empty() && (wp == VK_LEFT || wp == VK_RIGHT)) {
                self->MoveSelection(wp == VK_LEFT ? -1 : 1, 0);
                return 0;
            }
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
    if ((edit || note_find) && (msg == WM_KEYUP || msg == WM_LBUTTONUP || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
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
            note_find_edit_ = make(L"EDIT", L"", ES_AUTOHSCROLL, 3);
            if (!edit_ || !list_ || !note_find_edit_) return -1;
            edit_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            list_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(list_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
            note_find_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(note_find_edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));
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
                OpenTab(Tab::Desktop);
            } else if (zone == HitZone::TopTabNotes) {
                OpenTab(Tab::Notes);
            } else if (zone == HitZone::TopTabConfig) {
                config_app_id_.clear();
                app_cfg_editing_ = false;
                OpenTab(Tab::Config);
            } else if (zone == HitZone::NoteTab && idx >= 0) {
                if (idx == active_note_) StartNoteRename();
                else SwitchNote(idx);
            } else if (zone == HitZone::NoteClose && idx >= 0) {
                DeleteNote(idx);
            } else if (zone == HitZone::NoteAdd) {
                CreateNote();
            } else if (zone == HitZone::ConfigAction && idx >= 0) {
                config_selected_ = idx;
                focus_node_ = UiNode::ConfigList;
                ToggleConfigRow(idx);
            } else if (view_ == View::Files && tab_ == Tab::Notes && ::PtInRect(&rect_note_find_, client)) {
                ::SetFocus(note_find_edit_);
                Render();
            } else if (zone == HitZone::WebSearch) SearchWeb(false);
            else if (zone == HitZone::BackButton) Back();
            else if (zone == HitZone::Item && idx >= 0) {
                selected_ = idx;
                ::PostMessageW(hwnd_, kOpen, static_cast<WPARAM>(idx), 0);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (view_ == View::Pill) return 0;
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            const auto zone = HitTest(client, idx);
            if (zone == HitZone::Item && idx >= 0 && static_cast<std::size_t>(idx) < items_.size()) {
                const auto& item = items_[static_cast<std::size_t>(idx)];
                if (item.kind == Kind::File && Prefix(item.value, L"Apps\\") &&
                    Equal(fs::path(item.value).extension().native(), L".exe")) {
                    RegisterManualApp(item.value);
                    return 0;
                }
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            const auto zone = HitTest(client, idx);
            if (zone == HitZone::Item && idx >= 0) Menu(true, idx);
            else if (view_ == View::Files && tab_ == Tab::Desktop) DesktopContextMenu();
            else Menu(false);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (view_ == View::Files && tab_ == Tab::Desktop && !items_.empty()) {
                const int per_row = (desk_view_ == DesktopViewMode::List || desk_view_ == DesktopViewMode::Details) ? 1 : cols_;
                const int total_rows = static_cast<int>((items_.size() + static_cast<std::size_t>(per_row) - 1) / static_cast<std::size_t>(per_row));
                const int max_scroll = std::max(0, total_rows - visible_rows_);
                const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                scroll_row_ = std::clamp(scroll_row_ + (delta > 0 ? -1 : 1), 0, max_scroll);
                Render();
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == 1 && HIWORD(wp) == EN_CHANGE) {
                if (view_ == View::Search) Populate();
                else if (view_ == View::Files && !note_renaming_ && !app_cfg_editing_) {
                    if (tab_ != Tab::Desktop && !Text(edit_).empty()) tab_ = Tab::Desktop;
                    if (tab_ == Tab::Desktop) Populate();
                } else if (url_naming_ || note_renaming_ || app_cfg_editing_) {
                    Render();
                }
            } else if (LOWORD(wp) == 3 && HIWORD(wp) == EN_CHANGE) {
                Render();
            }
            return 0;
        case WM_HOTKEY:
            if (config_.LoadConfigIni(false).ok) ApplyConfigSettings(true);
            ::ShowWindow(hwnd_, SW_SHOW);
            Show(View::Search);
            return 0;
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
            if (view_ == View::Files && tab_ == Tab::Config && hotkey_capturing_) {
                if (wp == VK_ESCAPE || wp == VK_LEFT) {
                    hotkey_capturing_ = false;
                    FocusNode(UiNode::ConfigList);
                } else {
                    HandleHotkeyCapture(wp);
                }
                return 0;
            }
            if (view_ == View::Pill) {
                if (wp == VK_DOWN) Show(View::Search);
                else if (wp == VK_ESCAPE || wp == VK_UP) Back();
                return 0;
            }
            if (view_ == View::Files && tab_ == Tab::Desktop && wp == VK_F5) {
                Populate();
                return 0;
            }
            if (wp == VK_ESCAPE) {
                if (view_ == View::Files && tab_ == Tab::Config && !config_app_id_.empty()) {
                    config_app_id_.clear();
                    app_cfg_editing_ = false;
                    OpenTab(Tab::Desktop);
                    return 0;
                }
                Back();
                return 0;
            }
            if (view_ == View::Files) {
                if (focus_node_ == UiNode::ModuleBar) {
                    if (wp == VK_LEFT) {
                        const Tab prev = (tab_ == Tab::Desktop) ? Tab::Config
                            : (tab_ == Tab::Notes ? Tab::Desktop : Tab::Notes);
                        OpenTab(prev);
                        FocusNode(UiNode::ModuleBar);
                        return 0;
                    }
                    if (wp == VK_RIGHT) {
                        const Tab next = (tab_ == Tab::Desktop) ? Tab::Notes
                            : (tab_ == Tab::Notes ? Tab::Config : Tab::Desktop);
                        OpenTab(next);
                        FocusNode(UiNode::ModuleBar);
                        return 0;
                    }
                    if (wp == VK_DOWN) {
                        if (tab_ == Tab::Desktop) FocusNode(UiNode::DesktopGrid);
                        else if (tab_ == Tab::Notes) FocusNode(notes_.empty() ? UiNode::NoteEditor : UiNode::NotesTabs);
                        else FocusNode(UiNode::ConfigList);
                        return 0;
                    }
                    if (wp == VK_UP) {
                        Show(View::Search);
                        return 0;
                    }
                } else if (focus_node_ == UiNode::DesktopGrid && tab_ == Tab::Desktop) {
                    if (wp == VK_UP) { MoveSelection(0, -1); return 0; }
                    if (wp == VK_DOWN) { MoveSelection(0, 1); return 0; }
                    if (wp == VK_LEFT) { MoveSelection(-1, 0); return 0; }
                    if (wp == VK_RIGHT) { MoveSelection(1, 0); return 0; }
                    if (wp == VK_RETURN) { Open(selected_); return 0; }
                } else if (focus_node_ == UiNode::NotesTabs && tab_ == Tab::Notes) {
                    const int n = static_cast<int>(notes_.size());
                    if (wp == VK_LEFT && n > 0) {
                        SwitchNote((active_note_ - 1 + n) % n);
                        FocusNode(UiNode::NotesTabs);
                        return 0;
                    }
                    if (wp == VK_RIGHT && n > 0) {
                        SwitchNote((active_note_ + 1) % n);
                        FocusNode(UiNode::NotesTabs);
                        return 0;
                    }
                    if (wp == VK_DOWN) {
                        FocusNode(UiNode::NoteEditor);
                        return 0;
                    }
                    if (wp == VK_UP) {
                        FocusNode(UiNode::ModuleBar);
                        return 0;
                    }
                } else if (focus_node_ == UiNode::ConfigList && tab_ == Tab::Config) {
                    const int max_cfg = config_app_id_.empty() ? 6 : 3;
                    if (wp == VK_UP) {
                        if (config_selected_ <= 0) {
                            FocusNode(UiNode::ModuleBar);
                        } else {
                            --config_selected_;
                            Render();
                        }
                        return 0;
                    }
                    if (wp == VK_DOWN) {
                        config_selected_ = std::min(max_cfg, config_selected_ + 1);
                        Render();
                        return 0;
                    }
                    if (wp == VK_LEFT) {
                        FocusNode(UiNode::ModuleBar);
                        return 0;
                    }
                    if (wp == VK_RIGHT || wp == VK_RETURN || wp == VK_SPACE) {
                        ToggleConfigRow(config_selected_);
                        return 0;
                    }
                }
            }
            return 0;
        case WM_CHAR:
            if (view_ == View::Files && tab_ == Tab::Desktop && !hotkey_capturing_ && wp >= 32) {
                FocusNode(UiNode::Search);
                ::SendMessageW(edit_, WM_CHAR, wp, lp);
                return 0;
            }
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
                if (view_ == View::Search || url_naming_ || note_renaming_ || app_cfg_editing_) {
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
                ::ShowWindow(hwnd_, SW_SHOW);
                Show(View::Search);
            } else if (tray_msg == WM_LBUTTONDBLCLK) {
                ::ShowWindow(hwnd_, SW_SHOW);
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
        doc.SetValue(L"Desktop", L"Translucent", L"10%", error);
    if (doc.GetValue(L"Desktop", L"RememberPosition").empty())
        doc.SetValue(L"Desktop", L"RememberPosition", L"true", error);
    if (doc.GetValue(L"Desktop", L"ClipboardOffers").empty())
        doc.SetValue(L"Desktop", L"ClipboardOffers", L"true", error);
    if (doc.GetValue(L"Desktop", L"EnableKill").empty())
        doc.SetValue(L"Desktop", L"EnableKill", L"true", error);
}

void Desktop::ApplyConfigSettings(bool register_hotkey) {
    const auto& doc = config_.config_doc();
    const auto bool_opt = [&](const wchar_t* key, const wchar_t* def) {
        const auto v = doc.GetValue(L"Desktop", key, def);
        return !Equal(v, L"false") && v != L"0" && !Equal(v, L"no");
    };
    always_on_top_ = bool_opt(L"AlwaysOnTop", L"true");
    remember_pos_ = bool_opt(L"RememberPosition", L"true");
    clipboard_offers_ = bool_opt(L"ClipboardOffers", L"true");
    enable_kill_ = bool_opt(L"EnableKill", L"true");
    show_all_close_modes_ = bool_opt(L"ShowAllCloseModes", L"false");

    auto trans_val = doc.GetValue(L"Desktop", L"Translucent", L"10%");
    if (!trans_val.empty() && trans_val.back() == L'%') trans_val.pop_back();
    wchar_t* end = nullptr;
    const long parsed_pct = trans_val.empty() ? 10 : std::wcstol(trans_val.c_str(), &end, 10);
    translucent_pct_ = (end != trans_val.c_str() && *end == L'\0')
        ? std::clamp(static_cast<int>(parsed_pct), 0, 90) : 10;

    const auto vm = doc.GetValue(L"Desktop", L"ViewMode", L"LargeIcons");
    if (Equal(vm, L"SmallIcons")) desk_view_ = DesktopViewMode::SmallIcons;
    else if (Equal(vm, L"List")) desk_view_ = DesktopViewMode::List;
    else if (Equal(vm, L"Details")) desk_view_ = DesktopViewMode::Details;
    else desk_view_ = DesktopViewMode::LargeIcons;

    const auto sb = doc.GetValue(L"Desktop", L"SortBy", L"Name");
    desk_sort_ = Equal(sb, L"Modified") ? DesktopSortBy::Modified : DesktopSortBy::Name;

    const auto gb = doc.GetValue(L"Desktop", L"GroupBy", L"AppsFirst");
    desk_group_ = Equal(gb, L"FoldersFirst") ? DesktopGroupBy::FoldersFirst : DesktopGroupBy::AppsFirst;

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
            UINT vk = VK_SPACE;
            if (last.size() == 1) vk = static_cast<UINT>(::VkKeyScanW(last.front()) & 0xff);
            else if (Equal(last, L"Tab")) vk = VK_TAB;
            else if (Equal(last, L"Enter")) vk = VK_RETURN;
            else if (Equal(last, L"Esc")) vk = VK_ESCAPE;
            else if (Equal(last, L"Supr")) vk = VK_DELETE;
            else if ((last[0] == L'F' || last[0] == L'f') && last.size() >= 2) {
                const int fn = std::wcstol(last.c_str() + 1, nullptr, 10);
                if (fn >= 1 && fn <= 12) vk = static_cast<UINT>(VK_F1 + fn - 1);
            }
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
        target_w_ = std::clamp(S(notes_w_), S(340), avail_w);
        target_h_ = std::clamp(S(notes_h_), S(270), avail_h);
    } else if (tab_ == Tab::Config) {
        target_w_ = std::min(S(410), avail_w);
        target_h_ = S(40) + S(38) + S(276);
    } else {
        const int search_h = S(40);
        const int header_h = S(38);
        const int count = std::max(1, static_cast<int>(items_.size()));
        if (desk_view_ == DesktopViewMode::List || desk_view_ == DesktopViewMode::Details) {
            cols_ = 1;
            const int row_h = S(30);
            const int det_extra = (desk_view_ == DesktopViewMode::Details) ? S(24) : 0;
            const int max_rows = std::max(1, (avail_h - search_h - header_h - det_extra - S(16)) / row_h);
            visible_rows_ = std::clamp(count, 1, max_rows);
            scroll_row_ = std::clamp(scroll_row_, 0, std::max(0, count - visible_rows_));
            target_w_ = std::clamp(S(desk_view_ == DesktopViewMode::Details ? 480 : 380), S(320), avail_w);
            target_h_ = search_h + header_h + det_extra + visible_rows_ * row_h + S(12);
        } else {
            const bool small_ic = (desk_view_ == DesktopViewMode::SmallIcons);
            const int cell_w = small_ic ? S(72) : S(86);
            const int cell_h = small_ic ? S(64) : S(84);
            const auto col_str = config_.config_doc().GetValue(L"Desktop", L"Columns");
            wchar_t* end = nullptr;
            const long parsed_cols = col_str.empty() ? 0 : std::wcstol(col_str.c_str(), &end, 10);
            const int max_cols_fit = std::max(1, (avail_w - S(24)) / (cell_w + S(2)));
            const int desired_cols = (!col_str.empty() && end == col_str.c_str() + col_str.size() && parsed_cols > 0)
                ? static_cast<int>(parsed_cols) : (small_ic ? 5 : 4);
            cols_ = std::clamp(desired_cols, 1, max_cols_fit);
            if (col_str.empty()) cols_ = std::clamp(count, 3, cols_);
            const int total_rows = (count + cols_ - 1) / cols_;
            const int max_rows = std::max(1, (avail_h - search_h - header_h - S(16)) / cell_h);
            visible_rows_ = std::clamp(total_rows, 1, max_rows);
            scroll_row_ = std::clamp(scroll_row_, 0, std::max(0, total_rows - visible_rows_));
            target_w_ = std::clamp(cols_ * cell_w + S(24), S(320), avail_w);
            target_h_ = search_h + header_h + visible_rows_ * cell_h + S(12);
        }
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
    const int header_h = S(38);
    if (view_ == View::Pill && url_naming_) {
        ::MoveWindow(edit_, pad + S(34), pad + S(5), std::max(S(40), target_w_ - S(64)), S(24), FALSE);
    } else {
        ::MoveWindow(edit_, pad + S(36), pad + S(8), std::max(S(40), target_w_ - S(72)), S(24), FALSE);
    }
    if (view_ == View::Files && tab_ == Tab::Notes) {
        ::MoveWindow(note_find_edit_, pad + S(36), pad + target_h_ - S(34), std::max(S(40), target_w_ - S(66)), S(22), FALSE);
    } else {
        ::MoveWindow(note_find_edit_, 0, 0, 0, 0, FALSE);
    }
    if (view_ == View::Search) {
        ::MoveWindow(list_, pad, pad + search_h, target_w_, std::max(0, target_h_ - search_h), FALSE);
    } else if (view_ == View::Files && tab_ == Tab::Desktop) {
        ::MoveWindow(list_, pad, pad + search_h + header_h, target_w_, std::max(0, target_h_ - search_h - header_h), FALSE);
    } else {
        ::MoveWindow(list_, 0, 0, 0, 0, FALSE);
    }

    const bool show_edit = (view_ == View::Search) || url_naming_ || (view_ == View::Files);
    ::ShowWindow(edit_, show_edit ? SW_SHOW : SW_HIDE);
    ::ShowWindow(note_find_edit_, (view_ == View::Files && tab_ == Tab::Notes) ? SW_SHOW : SW_HIDE);
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
    const std::uint8_t base_alpha = static_cast<std::uint8_t>(std::clamp(((100 - translucent_pct_) * 255 + 50) / 100, 38, 255));
    const std::uint8_t fill_alpha = view_ == View::Files ? static_cast<std::uint8_t>(std::max(38, base_alpha - 6)) : base_alpha;

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
        rect_note_find_ = {};
        rect_note_tabs_.clear();
        rect_note_closes_.clear();
        rect_config_rows_.clear();

        // 1. Top unified search bar (invariable main interface)
        const int search_h = S(40);
        const float bar_cy = static_cast<float>(capsule.top) + static_cast<float>(search_h) * 0.5f;
        gfx::DrawSearchIcon(hdc, static_cast<float>(capsule.left) + Sf(20.0f), bar_cy, Sf(15.0f), {148, 163, 184, 220});

        const auto typed = note_renaming_ ? std::wstring{} : Text(edit_);
        const bool show_web = !typed.empty();
        const int right_margin = show_web ? S(42) : S(16);
        rect_edit_draw_ = {capsule.left + S(36), capsule.top + S(4), capsule.right - right_margin, capsule.top + search_h - S(4)};
        if (!note_renaming_) {
            DWORD sel_a = 0, sel_b = 0;
            ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_a), reinterpret_cast<LPARAM>(&sel_b));
            const bool focused = ::GetFocus() == edit_;
            gfx::DrawSearchText(hdc, typed, GhostSuffix(typed), static_cast<int>(sel_a), static_cast<int>(sel_b),
                                caret_visible_, focused, rect_edit_draw_, Sf(14.0f), dpi_);
        }
        if (show_web) {
            rect_web_ = {capsule.right - S(38), capsule.top + S(4), capsule.right - S(8), capsule.top + search_h - S(4)};
            const float wcx = static_cast<float>(rect_web_.left + rect_web_.right) * 0.5f;
            gfx::DrawWebIcon(hdc, wcx, bar_cy, Sf(15.0f), {148, 163, 184, 230}, hover_zone_ == HitZone::WebSearch);
        }

        gfx::DrawDivider(hdc, static_cast<float>(capsule.left + S(12)), static_cast<float>(capsule.right - S(12)),
                         static_cast<float>(capsule.top + search_h), 24);

        // 2. Environment bar (PMD)
        const int env_y = capsule.top + search_h;
        const int header_h = S(38);
        const bool sub = (tab_ == Tab::Desktop && folder_ != L"Other");
        int tab_x = capsule.left + S(12);
        if (sub) {
            rect_back_ = {capsule.left + S(8), env_y + S(5), capsule.left + S(34), env_y + header_h - S(5)};
            gfx::DrawBackIcon(hdc, static_cast<float>(rect_back_.left + rect_back_.right) * 0.5f,
                              static_cast<float>(env_y) + static_cast<float>(header_h) * 0.5f,
                              Sf(14.0f), {148, 163, 184, 230}, hover_zone_ == HitZone::BackButton);
            tab_x = capsule.left + S(36);
        }

        auto draw_top_tab = [&](const wchar_t* label, Tab t, HitZone hz, RECT& out_rect) {
            const int tw = static_cast<int>(gfx::MeasureTextWidth(hdc, label, Sf(13.0f), true)) + S(20);
            out_rect = {tab_x, env_y + S(5), tab_x + tw, env_y + header_h - S(5)};
            const bool active = (tab_ == t);
            const bool kbd_tab = active && (focus_node_ == UiNode::ModuleBar);
            const bool hov = (hover_zone_ == hz);
            if (active || hov) {
                const gfx::Color4 fill = active ? gfx::Color4{59, 130, 246, static_cast<std::uint8_t>(kbd_tab ? 85 : 55)} : gfx::Color4{255, 255, 255, 18};
                const gfx::Color4 border = kbd_tab ? gfx::Color4{147, 197, 253, 220}
                    : (active ? gfx::Color4{96, 165, 250, 140} : gfx::Color4{0, 0, 0, 0});
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

        gfx::DrawStatusShield(hdc, static_cast<float>(capsule.right) - Sf(22.0f),
                              static_cast<float>(env_y) + static_cast<float>(header_h) * 0.5f, Sf(18.0f), led_color);

        gfx::DrawDivider(hdc, static_cast<float>(capsule.left + S(12)), static_cast<float>(capsule.right - S(12)),
                         static_cast<float>(env_y + header_h), 22);

        const int content_y = env_y + header_h;

        if (tab_ == Tab::Desktop) {
            if (items_.empty()) {
                const RECT empty_rect{capsule.left + S(16), content_y + S(4), capsule.right - S(16), capsule.bottom - S(12)};
                gfx::DrawTextLine(hdc, typed.empty() ? L"Carpeta vac\u00eda" : L"Sin resultados",
                                  empty_rect, Sf(12.5f), false, {148, 163, 184, 180}, 0);
            } else if (desk_view_ == DesktopViewMode::List || desk_view_ == DesktopViewMode::Details) {
                int list_y = content_y + S(4);
                if (desk_view_ == DesktopViewMode::Details) {
                    const RECT hdr_n{capsule.left + S(44), list_y, capsule.right - S(190), list_y + S(22)};
                    const RECT hdr_t{capsule.right - S(186), list_y, capsule.right - S(104), list_y + S(22)};
                    const RECT hdr_d{capsule.right - S(100), list_y, capsule.right - S(14), list_y + S(22)};
                    gfx::DrawTextLine(hdc, L"Nombre", hdr_n, Sf(11.0f), true, {148, 163, 184, 210}, -1);
                    gfx::DrawTextLine(hdc, L"Tipo", hdr_t, Sf(11.0f), true, {148, 163, 184, 210}, -1);
                    gfx::DrawTextLine(hdc, L"Modificado", hdr_d, Sf(11.0f), true, {148, 163, 184, 210}, 1);
                    list_y += S(24);
                }
                const int row_h = S(30);
                for (int offset = 0; offset < visible_rows_ && scroll_row_ + offset < static_cast<int>(items_.size()); ++offset) {
                    const int idx = scroll_row_ + offset;
                    const int ry = list_y + offset * row_h;
                    if (ry + row_h > capsule.bottom) break;
                    const bool is_sel = (idx == selected_);
                    const bool is_hov = (hover_zone_ == HitZone::Item && hover_index_ == idx);
                    if (is_sel || is_hov) {
                        const gfx::Color4 fill = is_sel ? gfx::Color4{30, 41, 59, 210} : gfx::Color4{30, 41, 59, 115};
                        const gfx::Color4 border = is_sel ? gfx::Color4{59, 130, 246, 130} : gfx::Color4{0, 0, 0, 0};
                        gfx::DrawRoundedHighlight(hdc, static_cast<float>(capsule.left + S(8)), static_cast<float>(ry + S(1)),
                                                  static_cast<float>(cur_w_ - S(16)), static_cast<float>(row_h - S(2)),
                                                  Sf(7.0f), fill, border);
                    }
                    const auto& item = items_[static_cast<std::size_t>(idx)];
                    const int icon_sz = S(18);
                    const int ix = capsule.left + S(16);
                    const int iy = ry + (row_h - icon_sz) / 2;
                    if (item.kind == Kind::Url) {
                        gfx::DrawWebIcon(hdc, static_cast<float>(ix + icon_sz / 2), static_cast<float>(iy + icon_sz / 2),
                                         Sf(14.0f), {52, 211, 153, 235}, is_sel || is_hov);
                    } else if (item.kind == Kind::Kill) {
                        gfx::DrawGlowingLed(hdc, static_cast<float>(ix + icon_sz / 2), static_cast<float>(iy + icon_sz / 2),
                                            Sf(5.0f), {248, 113, 113, 255}, is_sel || is_hov);
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
                            } else rel.clear();
                        }
                        const auto resolved = resolver_.ResolvePmRelativePath(rel);
                        gfx::DrawItemIcon(hdc, resolved.ok ? resolved.resolved_path : L"",
                                          item.kind == Kind::App, item.kind == Kind::Folder,
                                          ix, iy, icon_sz, has_dot, dot_color, running, dpi_);
                    }
                    if (desk_view_ == DesktopViewMode::Details) {
                        const RECT nr{capsule.left + S(44), ry, capsule.right - S(190), ry + row_h};
                        const RECT tr{capsule.right - S(186), ry, capsule.right - S(104), ry + row_h};
                        const RECT dr{capsule.right - S(100), ry, capsule.right - S(14), ry + row_h};
                        gfx::DrawTextLine(hdc, item.label, nr, Sf(12.5f), is_sel, {241, 245, 249, 255}, -1);
                        const wchar_t* tipo = item.kind == Kind::App ? L"App" : item.kind == Kind::Folder ? L"Carpeta" : item.kind == Kind::Url ? L"Link" : item.kind == Kind::Kill ? L"Acci\u00f3n" : L"Archivo";
                        gfx::DrawTextLine(hdc, tipo, tr, Sf(11.5f), false, {148, 163, 184, 220}, -1);
                        std::wstring date_str = L"-";
                        if (item.modified_time != 0) {
                            FILETIME ft{static_cast<DWORD>(item.modified_time & 0xFFFFFFFFULL), static_cast<DWORD>(item.modified_time >> 32)};
                            FILETIME lft{};
                            SYSTEMTIME st{};
                            if (::FileTimeToLocalFileTime(&ft, &lft) && ::FileTimeToSystemTime(&lft, &st)) {
                                wchar_t buf[32]{};
                                std::swprintf(buf, std::size(buf), L"%02u/%02u/%04u", st.wDay, st.wMonth, st.wYear);
                                date_str = buf;
                            }
                        }
                        gfx::DrawTextLine(hdc, date_str, dr, Sf(11.5f), false, {148, 163, 184, 210}, 1);
                    } else {
                        const RECT nr{capsule.left + S(44), ry, capsule.right - S(16), ry + row_h};
                        gfx::DrawTextLine(hdc, item.label, nr, Sf(12.5f), is_sel, {241, 245, 249, 255}, -1);
                    }
                }
            } else {
                const bool small_ic = (desk_view_ == DesktopViewMode::SmallIcons);
                const int cell_w = small_ic ? S(72) : S(86);
                const int cell_h = small_ic ? S(64) : S(84);
                const int icon_sz = small_ic ? S(24) : S(36);
                const int grid_x = capsule.left + S(12);
                const int grid_y = content_y + S(4);
                const int first_idx = scroll_row_ * cols_;
                const int max_visible = visible_rows_ * cols_;

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
                    const int ix = cx + (cell_w - icon_sz) / 2;
                    const int iy = cy + (small_ic ? S(8) : S(12));
                    if (item.kind == Kind::Url) {
                        gfx::DrawWebIcon(hdc, static_cast<float>(ix) + static_cast<float>(icon_sz) * 0.5f,
                                         static_cast<float>(iy) + static_cast<float>(icon_sz) * 0.5f,
                                         small_ic ? Sf(18.0f) : Sf(24.0f), {52, 211, 153, 235}, is_sel || is_hov);
                    } else if (item.kind == Kind::Kill) {
                        gfx::DrawRoundedHighlight(hdc, static_cast<float>(ix + S(2)), static_cast<float>(iy + S(2)),
                                                  static_cast<float>(icon_sz - S(4)), static_cast<float>(icon_sz - S(4)),
                                                  Sf(12.0f), {220, 38, 38, 190}, {248, 113, 113, 255});
                        const RECT xr{ix, iy, ix + icon_sz, iy + icon_sz};
                        gfx::DrawTextLine(hdc, L"\u00d7", xr, small_ic ? Sf(14.0f) : Sf(18.0f), true, {255, 255, 255, 255}, 0);
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
                            } else rel.clear();
                        }
                        const auto resolved = resolver_.ResolvePmRelativePath(rel);
                        gfx::DrawItemIcon(hdc, resolved.ok ? resolved.resolved_path : L"",
                                          item.kind == Kind::App, item.kind == Kind::Folder,
                                          ix, iy, icon_sz, has_dot, dot_color, running, dpi_);
                    }

                    const RECT lbl_rect{cx + S(4), iy + icon_sz + S(4), cx + cell_w - S(4), cy + cell_h - S(2)};
                    gfx::DrawGridLabel(hdc, item.label, lbl_rect, small_ic ? Sf(10.5f) : Sf(11.5f),
                                       is_sel ? gfx::Color4{255, 255, 255, 255} : gfx::Color4{226, 232, 240, 245});
                }
            }
        } else if (tab_ == Tab::Notes) {
            // Top of Notes environment: note tabs
            const int note_tab_h = S(32);
            const int find_h = S(34);
            const int bottom_pad = S(10);
            const int tabs_y = content_y + S(4);
            const int find_y = capsule.bottom - bottom_pad - find_h;

            int nx = capsule.left + S(12);
            const int max_tab_x = capsule.right - S(44);
            for (int i = 0; i < static_cast<int>(notes_.size()); ++i) {
                const bool active = (i == active_note_);
                const bool hov = (hover_zone_ == HitZone::NoteTab && hover_index_ == i);
                const int text_w = static_cast<int>(gfx::MeasureTextWidth(
                    hdc, notes_[static_cast<std::size_t>(i)].name, Sf(12.0f), active));
                const int tw = std::clamp(text_w + S(38), S(74), S(140));
                if (nx + tw > max_tab_x) break;
                const RECT tr{nx, tabs_y + S(2), nx + tw, tabs_y + note_tab_h - S(2)};
                rect_note_tabs_.push_back(tr);
                const RECT cr{tr.right - S(22), tr.top + S(2), tr.right - S(4), tr.bottom - S(2)};
                rect_note_closes_.push_back(cr);

                if (active || hov) {
                    const bool kbd_note = active && (focus_node_ == UiNode::NotesTabs);
                    const gfx::Color4 fill = active ? gfx::Color4{30, 41, 59, static_cast<std::uint8_t>(kbd_note ? 245 : 220)} : gfx::Color4{255, 255, 255, 18};
                    const gfx::Color4 border = kbd_note ? gfx::Color4{147, 197, 253, 220}
                        : (active ? gfx::Color4{59, 130, 246, 150} : gfx::Color4{0, 0, 0, 0});
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
                rect_note_add_ = {nx, tabs_y + S(2), nx + S(26), tabs_y + note_tab_h - S(2)};
                const bool add_hov = (hover_zone_ == HitZone::NoteAdd);
                if (add_hov) {
                    gfx::DrawRoundedHighlight(hdc, static_cast<float>(rect_note_add_.left), static_cast<float>(rect_note_add_.top),
                                              static_cast<float>(S(26)), static_cast<float>(rect_note_add_.bottom - rect_note_add_.top),
                                              Sf(7.0f), {255, 255, 255, 22}, {0, 0, 0, 0});
                }
                gfx::DrawTextLine(hdc, L"+", rect_note_add_, Sf(15.0f), true, {203, 213, 225, 255}, 0);
            }

            // Bottom of Notes environment: local find bar + resize grip
            rect_note_find_ = {capsule.left + S(12), find_y + S(3), capsule.right - S(28), find_y + find_h - S(2)};
            gfx::DrawRoundedHighlight(hdc, static_cast<float>(rect_note_find_.left), static_cast<float>(rect_note_find_.top),
                                      static_cast<float>(rect_note_find_.right - rect_note_find_.left),
                                      static_cast<float>(rect_note_find_.bottom - rect_note_find_.top),
                                      Sf(8.0f), {15, 23, 42, 135}, {148, 163, 184, 55});
            const float fcy = static_cast<float>(rect_note_find_.top + rect_note_find_.bottom) * 0.5f;
            gfx::DrawSearchIcon(hdc, static_cast<float>(rect_note_find_.left + S(16)), fcy, Sf(13.0f), {148, 163, 184, 220});

            const RECT find_draw{rect_note_find_.left + S(30), rect_note_find_.top + S(2),
                                 rect_note_find_.right - S(8), rect_note_find_.bottom - S(2)};
            DWORD fa = 0, fb = 0;
            ::SendMessageW(note_find_edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&fa), reinterpret_cast<LPARAM>(&fb));
            gfx::DrawSearchText(hdc, Text(note_find_edit_), L"", static_cast<int>(fa), static_cast<int>(fb),
                                caret_visible_, ::GetFocus() == note_find_edit_, find_draw, Sf(12.5f), dpi_);

            rect_resize_grip_ = {capsule.right - S(24), capsule.bottom - S(24), capsule.right - S(2), capsule.bottom - S(2)};
            const bool grip_hov = (hover_zone_ == HitZone::ResizeGrip || resizing_notes_);
            gfx::DrawTextLine(hdc, L"\u25e2", rect_resize_grip_, Sf(12.0f), false,
                              grip_hov ? gfx::Color4{148, 163, 184, 255} : gfx::Color4{148, 163, 184, 140}, 0);
        } else {
            // Tab::Config
            struct CfgRow { const wchar_t* label; std::wstring value; gfx::Color4 color; };
            std::vector<CfgRow> rows;
            App cfg_app;
            const bool is_app_cfg = !config_app_id_.empty() &&
                applications::Find(resolver_, config_app_id_, cfg_app) == ERROR_SUCCESS;
            if (is_app_cfg) {
                const std::wstring name_disp = cfg_app.aliases.empty() ? cfg_app.name : (cfg_app.name + L", " + cfg_app.aliases);
                const std::wstring edit_tag = L"[Editando arriba\u2026]";
                rows = {
                    {L"Nombre y alias (Name)", (app_cfg_editing_ && app_cfg_field_ == 1) ? edit_tag : name_disp, {96, 165, 250, 255}},
                    {L"Extensiones (Extensions)", (app_cfg_editing_ && app_cfg_field_ == 2) ? edit_tag : (cfg_app.extensions.empty() ? L"(ninguna)" : cfg_app.extensions), {96, 165, 250, 255}},
                    {L"Argumentos (Arguments)", (app_cfg_editing_ && app_cfg_field_ == 3) ? edit_tag : (cfg_app.arguments.empty() ? L"(ninguno)" : cfg_app.arguments), {96, 165, 250, 255}},
                    {L"Navegador predeterminado", cfg_app.browser ? L"Activado" : L"Desactivado", {96, 165, 250, 255}},
                };
            } else {
                const auto& cfg = config_.config_doc();
                const std::wstring hotkey_val = cfg.GetValue(L"Desktop", L"Hotkey", L"Ctrl+Space");
                gfx::Color4 hk_color{96, 165, 250, 255};
                std::wstring hk_text = hotkey_val;
                if (hotkey_capturing_) {
                    hk_text = L"Presiona teclas\u2026";
                    hk_color = {147, 197, 253, 255};
                } else if (hotkey_status_err_) {
                    hk_color = {248, 113, 113, 255};
                } else if (hotkey_status_ok_) {
                    hk_color = {52, 211, 153, 255};
                }
                rows = {
                    {L"Atajo de teclado (Hotkey)", hk_text, hk_color},
                    {L"Siempre visible (AlwaysOnTop)", always_on_top_ ? L"Activado" : L"Desactivado", {96, 165, 250, 255}},
                    {L"Translucidez (Translucent)", std::to_wstring(translucent_pct_) + L"%", {96, 165, 250, 255}},
                    {L"Recordar posici\u00f3n (RememberPosition)", remember_pos_ ? L"Activado" : L"Desactivado", {96, 165, 250, 255}},
                    {L"Detectar portapapeles (ClipboardOffers)", clipboard_offers_ ? L"Activado" : L"Desactivado", {96, 165, 250, 255}},
                    {L"Comando kill (EnableKill)", enable_kill_ ? L"Activado" : L"Desactivado", {96, 165, 250, 255}},
                    {L"Mostrar todos los tipos de cierre", show_all_close_modes_ ? L"Activado" : L"Desactivado", {96, 165, 250, 255}},
                };
            }
            const int row_h = S(36);
            int ry = content_y + S(8);
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                const RECT rr{capsule.left + S(16), ry, capsule.right - S(16), ry + row_h - S(4)};
                rect_config_rows_.push_back(rr);
                const bool kbd_sel = (focus_node_ == UiNode::ConfigList && config_selected_ == i);
                const bool hov = (hover_zone_ == HitZone::ConfigAction && hover_index_ == i) || kbd_sel;
                gfx::Color4 border = hov ? gfx::Color4{59, 130, 246, 150} : gfx::Color4{148, 163, 184, 35};
                if (!is_app_cfg && i == 0 && hotkey_status_err_) border = {248, 113, 113, 180};
                else if (!is_app_cfg && i == 0 && hotkey_status_ok_) border = {52, 211, 153, 180};
                gfx::DrawRoundedHighlight(hdc, static_cast<float>(rr.left), static_cast<float>(rr.top),
                                          static_cast<float>(rr.right - rr.left), static_cast<float>(rr.bottom - rr.top),
                                          Sf(9.0f), hov ? gfx::Color4{30, 41, 59, 190} : gfx::Color4{15, 23, 42, 110}, border);
                const RECT lr{rr.left + S(12), rr.top, rr.right - S(160), rr.bottom};
                const RECT vr{rr.right - S(160), rr.top, rr.right - S(12), rr.bottom};
                gfx::DrawTextLine(hdc, rows[static_cast<std::size_t>(i)].label, lr, Sf(12.0f), kbd_sel, {226, 232, 240, 250}, -1);
                gfx::DrawTextLine(hdc, rows[static_cast<std::size_t>(i)].value, vr, Sf(12.0f), true, rows[static_cast<std::size_t>(i)].color, 1);
                ry += row_h;
            }
            if (!is_app_cfg && hotkey_status_err_ && !hotkey_error_text_.empty()) {
                const RECT err_r{capsule.left + S(20), ry + S(2), capsule.right - S(20), ry + S(22)};
                gfx::DrawTextLine(hdc, hotkey_error_text_, err_r, Sf(11.5f), true, {248, 113, 113, 255}, 1);
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

    if (!Text(edit_).empty() && in(rect_web_)) return HitZone::WebSearch;
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
    const int search_h = S(40);
    const int header_h = S(38);
    const int content_y = pad + search_h + header_h;
    if (desk_view_ == DesktopViewMode::List || desk_view_ == DesktopViewMode::Details) {
        const int list_y = content_y + S(4) + (desk_view_ == DesktopViewMode::Details ? S(24) : 0);
        const int row_h = S(30);
        if (client.x >= pad && client.x <= pad + cur_w_ && client.y >= list_y) {
            const int row = (client.y - list_y) / row_h;
            const int idx = scroll_row_ + row;
            if (row >= 0 && row < visible_rows_ && idx >= 0 && idx < static_cast<int>(items_.size())) {
                item_index = idx;
                return HitZone::Item;
            }
        }
        return HitZone::None;
    }

    const bool small_ic = (desk_view_ == DesktopViewMode::SmallIcons);
    const int cell_w = small_ic ? S(72) : S(86);
    const int cell_h = small_ic ? S(64) : S(84);
    const int grid_x = pad + S(12);
    const int grid_y = content_y + S(4);
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
    if (view_ != View::Files || items_.empty()) return;
    const int per_row = (desk_view_ == DesktopViewMode::List || desk_view_ == DesktopViewMode::Details) ? 1 : std::max(1, cols_);
    const int row = selected_ / per_row;
    if (row < scroll_row_) scroll_row_ = row;
    else if (row >= scroll_row_ + visible_rows_) scroll_row_ = row - visible_rows_ + 1;
}

void Desktop::FocusNode(UiNode node) {
    focus_node_ = node;
    switch (node) {
        case UiNode::Pill:
        case UiNode::ModuleBar:
            ::SetFocus(hwnd_);
            break;
        case UiNode::Search:
        case UiNode::ConfigFieldEdit:
            if (node == UiNode::ConfigFieldEdit && hotkey_capturing_) ::SetFocus(hwnd_);
            else ::SetFocus(edit_);
            break;
        case UiNode::DesktopGrid:
            if (!items_.empty()) selected_ = std::clamp(selected_, 0, static_cast<int>(items_.size()) - 1);
            ::SetFocus(hwnd_);
            break;
        case UiNode::NotesTabs:
            ::SetFocus(hwnd_);
            break;
        case UiNode::NoteEditor:
            if (note_edit_) ::SetFocus(note_edit_);
            else ::SetFocus(hwnd_);
            break;
        case UiNode::ConfigList: {
            const int max_cfg = config_app_id_.empty() ? 6 : 3;
            config_selected_ = std::clamp(config_selected_, 0, max_cfg);
            ::SetFocus(hwnd_);
            break;
        }
    }
    Render();
}

void Desktop::MoveSelection(int dx, int dy) {
    if (items_.empty()) return;
    const int count = static_cast<int>(items_.size());
    if (view_ == View::Search) {
        selected_ = std::clamp(selected_ + dy, 0, count - 1);
    } else {
        const int per_row = (desk_view_ == DesktopViewMode::List || desk_view_ == DesktopViewMode::Details) ? 1 : cols_;
        if (dy < 0 && selected_ < per_row) {
            FocusNode(UiNode::ModuleBar);
            return;
        }
        int next = selected_ + dx + dy * per_row;
        next = std::clamp(next, 0, count - 1);
        selected_ = next;
        EnsureVisibleSelection();
    }
    Render();
}

void Desktop::OpenTab(Tab tab) {
    if (closing_) return;
    if (view_ == View::Files && tab_ == Tab::Notes && tab != Tab::Notes) {
        SaveActiveNote();
    }
    note_renaming_ = false;
    hotkey_capturing_ = false;
    hotkey_status_ok_ = false;
    hotkey_status_err_ = false;
    hotkey_error_text_.clear();
    view_ = View::Files;
    tab_ = tab;
    url_naming_ = false;
    ::SetWindowTextW(edit_, L"");
    if (tab == Tab::Desktop) {
        focus_node_ = UiNode::Search;
        folder_ = L"Other";
        scroll_row_ = 0;
        selected_ = 0;
        ::SetForegroundWindow(hwnd_);
        Populate();
        caret_visible_ = true;
        ::SetFocus(edit_);
    } else if (tab == Tab::Notes) {
        focus_node_ = UiNode::NoteEditor;
        ::SetForegroundWindow(hwnd_);
        LoadNotes();
        Layout();
        if (note_edit_) ::SetFocus(note_edit_);
    } else {
        focus_node_ = UiNode::ConfigList;
        ::SetForegroundWindow(hwnd_);
        Layout();
    }
}

void Desktop::Show(View view) {
    if (closing_) return;
    if (view_ == View::Files && tab_ == Tab::Notes && view != View::Files) {
        SaveActiveNote();
        note_renaming_ = false;
    }
    hotkey_capturing_ = false;
    hotkey_status_ok_ = false;
    hotkey_status_err_ = false;
    hotkey_error_text_.clear();
    view_ = view;
    url_naming_ = false;
    if (view == View::Pill) ::SetWindowTextW(edit_, L"");
    if (view == View::Files) {
        focus_node_ = UiNode::Search;
        tab_ = Tab::Desktop;
        note_renaming_ = false;
        folder_ = L"Other";
    }
    if (view == View::Pill) focus_node_ = UiNode::Pill;
    else if (view == View::Search) focus_node_ = UiNode::Search;
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
    if (enable_kill_ && !typed.empty() && Prefix(L"kill", typed)) {
        items_.push_back({Kind::Kill, L"kill \u2014 Matar ventana con un clic", L"kill", {}, 0});
    }
    applications::List(resolver_, apps_);
    std::vector<Item> app_items;
    if (view_ == View::Search || folder_ == L"Other") {
        std::vector<Item> partial;
        for (const auto& app : apps_) {
            if (app.archived && view_ == View::Files) continue;
            auto aliases = Split(app.aliases, L',');
            aliases.push_back(app.id);
            aliases.push_back(app.name);
            std::uint64_t mtime = 0;
            if (const auto exe = resolver_.ResolvePmRelativePath(app.executable); exe.ok) {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (::GetFileAttributesExW(exe.resolved_path.c_str(), GetFileExInfoStandard, &fad)) {
                    mtime = (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                             static_cast<std::uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
                }
            }
            Item item{Kind::App, app.name + (app.archived ? L" (archivado)" : L""), app.id, {}, mtime};
            bool match = false, prefix = false;
            for (const auto& name : aliases) {
                if (Equal(typed, name) || (Prefix(typed, name) && typed.size() > name.size() && typed[name.size()] == L' ')) {
                    if (typed.size() > name.size()) item.args = typed.substr(name.size() + 1);
                    app_items.push_back(item);
                    match = true;
                    break;
                }
                prefix = prefix || Prefix(name, typed);
            }
            if (!match && prefix) partial.push_back(std::move(item));
        }
        app_items.insert(app_items.end(), partial.begin(), partial.end());
    }
    std::vector<Item> url_items;
    if (view_ == View::Search || (view_ == View::Files && folder_ == L"Other" && !typed.empty())) {
        config_.LoadAppsIni(false);
        const auto& apps_doc = config_.apps_doc();
        std::vector<Item> url_exact, url_partial;
        for (const auto& line : apps_doc.lines()) {
            if (line.kind != core::IniDocument::LineKind::Section) continue;
            const bool is_url = Prefix(line.section_name, L"Url.");
            const bool is_link = Prefix(line.section_name, L"Link.");
            if (!is_url && !is_link) continue;
            const auto name = line.section_name.substr(is_url ? 4 : 5);
            auto url = apps_doc.GetValue(line.section_name, L"URL");
            if (url.empty()) url = apps_doc.GetValue(line.section_name, L"Path");
            if (url.empty()) url = apps_doc.GetValue(line.section_name, L"Target");
            if (name.empty() || url.empty()) continue;
            Item item{Kind::Url, name, url, {}, 0};
            if (Equal(typed, name) || (Prefix(typed, name) && typed.size() > name.size() && typed[name.size()] == L' ')) {
                if (typed.size() > name.size()) item.args = typed.substr(name.size() + 1);
                url_exact.push_back(std::move(item));
            } else if (Prefix(name, typed)) {
                url_partial.push_back(std::move(item));
            }
        }
        url_items.insert(url_items.end(), url_exact.begin(), url_exact.end());
        url_items.insert(url_items.end(), url_partial.begin(), url_partial.end());
    }
    std::vector<Item> file_items;
    const auto folder = view_ == View::Files ? folder_ : L"Other";
    const auto full = resolver_.ResolvePmRelativePath(folder);
    applications::Pins pins;
    if (full.ok && !applications::PinDirectory(full.resolved_path, pins)) {
        std::error_code ec;
        for (fs::directory_iterator it(full.resolved_path, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().native();
            const auto normalized = PathResolver::NormalizePmRelativePath(name);
            if (!normalized.ok || normalized.resolved_path != name || !Prefix(name, typed)) continue;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &fad)) continue;
            if (fad.dwFileAttributes == INVALID_FILE_ATTRIBUTES || (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const std::uint64_t mtime = (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                        static_cast<std::uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
            file_items.push_back({fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? Kind::Folder : Kind::File,
                                  name, folder + L"\\" + name, {}, mtime});
            if (file_items.size() == 1000) break;
        }
    }
    if (view_ == View::Files && folder_ == L"Other" && typed.empty()) {
        const auto apps_root = resolver_.ResolvePmRelativePath(L"Apps");
        applications::Pins apps_pins;
        if (apps_root.ok && !applications::PinDirectory(apps_root.resolved_path, apps_pins)) {
            std::error_code ec;
            for (fs::directory_iterator it(apps_root.resolved_path, ec), end; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().native();
                if (Equal(name, L".temp")) continue;
                const auto rel_dir = L"Apps\\" + name;
                const bool registered = std::any_of(apps_.begin(), apps_.end(), [&](const App& a) {
                    return Equal(a.directory, rel_dir);
                });
                if (registered) continue;
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (!::GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &fad)) continue;
                if (fad.dwFileAttributes == INVALID_FILE_ATTRIBUTES ||
                    (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const std::uint64_t mtime = (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                            static_cast<std::uint64_t>(fad.ftLastWriteTime.dwLowDateTime);
                file_items.push_back({Kind::Folder, name, rel_dir, {}, mtime});
                if (file_items.size() >= 1000) break;
            }
        }
    }

    if (view_ == View::Files && typed.empty()) {
        auto sort_vec = [&](std::vector<Item>& v) {
            std::sort(v.begin(), v.end(), [&](const Item& a, const Item& b) {
                if (desk_sort_ == DesktopSortBy::Modified && a.modified_time != b.modified_time) {
                    return a.modified_time > b.modified_time;
                }
                return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
            });
        };
        sort_vec(app_items);
        sort_vec(file_items);
        if (desk_group_ == DesktopGroupBy::FoldersFirst) {
            items_.insert(items_.end(), file_items.begin(), file_items.end());
            items_.insert(items_.end(), app_items.begin(), app_items.end());
        } else {
            items_.insert(items_.end(), app_items.begin(), app_items.end());
            items_.insert(items_.end(), file_items.begin(), file_items.end());
        }
    } else {
        std::sort(file_items.begin(), file_items.end(), [](const Item& a, const Item& b) { return a.label < b.label; });
        items_.insert(items_.end(), app_items.begin(), app_items.end());
        items_.insert(items_.end(), url_items.begin(), url_items.end());
        items_.insert(items_.end(), file_items.begin(), file_items.end());
    }
    selected_ = items_.empty() ? -1 : 0;
    Layout();
}

void Desktop::Back() {
    if (view_ == View::Pill) {
        bool dismissed = false;
        if (url_naming_) {
            url_naming_ = false;
            ::SetWindowTextW(edit_, L"");
            dismissed = true;
        }
        if (!clipboard_url_.empty()) {
            dismissed_url_ = clipboard_url_;
            clipboard_url_.clear();
            Layout();
            dismissed = true;
        }
        if (!package_.empty()) {
            dismissed_package_ = package_;
            package_.clear();
            Layout();
            dismissed = true;
        }
        if (!dismissed && !always_on_top_ && tray_added_) {
            ::ShowWindow(hwnd_, SW_HIDE);
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
        if (note_find_edit_ && !Text(note_find_edit_).empty()) {
            ::SetWindowTextW(note_find_edit_, L"");
            Render();
            return;
        }
        SaveActiveNote();
        Show(View::Pill);
        return;
    }
    if (tab_ == Tab::Config) {
        if (hotkey_capturing_) {
            hotkey_capturing_ = false;
            Render();
            return;
        }
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
        if (Prefix(item.value, L"Apps\\")) {
            if (Equal(fs::path(item.value).extension().native(), L".exe")) {
                RegisterManualApp(item.value);
                return;
            }
            const auto path = resolver_.ResolvePmRelativePath(item.value);
            if (!path.ok) { Report(ERROR_INVALID_NAME); return; }
            const auto code = reinterpret_cast<INT_PTR>(::ShellExecuteW(
                hwnd_, L"open", path.resolved_path.c_str(), nullptr,
                fs::path(path.resolved_path).parent_path().c_str(), SW_SHOWNORMAL));
            if (code <= 32) Report(static_cast<DWORD>(code));
            else Show(View::Pill);
        } else {
            BrowseDocument(item);
        }
    } else if (item.kind == Kind::Url) {
        auto expanded = (item.value.find(L"$W$") != std::wstring::npos || item.value.find(L"$I$") != std::wstring::npos)
            ? ExpandWeb(item.value, item.args) : item.value;
        wchar_t env_buf[2048]{};
        const DWORD env_n = ::ExpandEnvironmentStringsW(expanded.c_str(), env_buf, static_cast<DWORD>(std::size(env_buf)));
        const std::wstring final_url = (env_n > 0 && env_n <= std::size(env_buf)) ? std::wstring(env_buf) : expanded;
        std::error_code ec;
        const bool is_local = fs::exists(final_url, ec) || final_url.starts_with(L"file://");
        if (!IsHttpUrl(final_url) && !is_local) { Report(ERROR_INVALID_NAME); return; }
        if (IsHttpUrl(final_url)) {
            OpenTargetUrl(final_url);
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
    if (!ext.empty() && !applications::Association(resolver_, ext, id) && !id.empty()) {
        if (Equal(id, L"HOST")) {
            const auto code = reinterpret_cast<INT_PTR>(::ShellExecuteW(
                hwnd_, L"open", path.resolved_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (code <= 32) Report(static_cast<DWORD>(code));
            else Show(View::Pill);
            return;
        }
        if (!applications::Find(resolver_, id, app) && !app.archived && !applications::ValidateApp(resolver_, app, true)) {
            Launch(app, jobs::QuoteArgument(path.resolved_path));
            return;
        }
    }

    const bool old_modal = modal_;
    modal_ = true;
    const std::wstring prompt = ext.empty()
        ? L"\u00bfC\u00f3mo deseas abrir este archivo?"
        : (L"No hay asociaci\u00f3n configurada para ." + ext + L".\n\u00bfC\u00f3mo deseas abrirlo?");
    const int mode_choice = gfx::ShowGlassDialog(
        hwnd_, L"Abrir archivo", prompt,
        {L"App de PML", L"Anfitri\u00f3n (Windows)", L"Cancelar"}, dpi_, 0);
    if (mode_choice == 1) {
        if (!ext.empty()) Report(applications::SetAssociation(resolver_, ext, L"HOST"));
        const auto code = reinterpret_cast<INT_PTR>(::ShellExecuteW(
            hwnd_, L"open", path.resolved_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (code <= 32) Report(static_cast<DWORD>(code));
        else Show(View::Pill);
        modal_ = old_modal;
        return;
    }
    if (mode_choice != 0) {
        modal_ = old_modal;
        return;
    }

    std::vector<gfx::MenuEntry> menu_items;
    std::vector<App> available;
    for (auto a : apps_) {
        if (a.archived || applications::ValidateApp(resolver_, a, true)) continue;
        available.push_back(a);
        menu_items.push_back({static_cast<int>(available.size()), a.name, true, false});
    }
    if (menu_items.empty()) {
        gfx::ShowGlassDialog(hwnd_, L"Abrir archivo", L"No hay aplicaciones de PML disponibles.", {L"Aceptar"}, dpi_, 0);
        modal_ = old_modal;
        return;
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
    modal_ = old_modal;
    if (choice > 0 && static_cast<std::size_t>(choice) <= available.size()) {
        const auto selected = available[static_cast<std::size_t>(choice - 1)];
        if (!ext.empty()) Report(applications::SetAssociation(resolver_, ext, selected.id));
        Launch(selected, jobs::QuoteArgument(path.resolved_path));
    }
}

void Desktop::OpenTargetUrl(const std::wstring& target) {
    if (closing_) return;
    if (!IsHttpUrl(target)) {
        const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) Report(static_cast<DWORD>(result));
        else Show(View::Pill);
        return;
    }
    applications::List(resolver_, apps_);
    const auto& settings = config_.config_doc();
    const auto pref = settings.GetValue(L"Web", L"App");
    if (Equal(pref, L"HOST")) {
        const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) Report(static_cast<DWORD>(result));
        else Show(View::Pill);
        return;
    }
    if (!pref.empty()) {
        App app;
        if (!applications::Find(resolver_, pref, app) && !app.archived) {
            Launch(app, jobs::QuoteArgument(target));
            return;
        }
    }
    const auto browser = std::find_if(apps_.begin(), apps_.end(), [](const App& a) { return a.browser && !a.archived; });
    if (browser != apps_.end()) {
        Launch(*browser, jobs::QuoteArgument(target));
        return;
    }

    const bool old_modal = modal_;
    modal_ = true;
    const int mode_choice = gfx::ShowGlassDialog(
        hwnd_, L"Abrir enlace",
        L"No hay un navegador predeterminado configurado.\n\u00bfC\u00f3mo deseas abrir este enlace?",
        {L"App de PML", L"Anfitri\u00f3n (Windows)", L"Cancelar"}, dpi_, 0);
    if (mode_choice == 1) {
        std::wstring err;
        config_.config_doc().SetValue(L"Web", L"App", L"HOST", err);
        config_.SaveConfigIniIfModified(false);
        const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) Report(static_cast<DWORD>(result));
        else Show(View::Pill);
        modal_ = old_modal;
        return;
    }
    if (mode_choice != 0) {
        modal_ = old_modal;
        return;
    }

    std::vector<gfx::MenuEntry> menu_items;
    std::vector<App> available;
    for (auto a : apps_) {
        if (a.archived || applications::ValidateApp(resolver_, a, true)) continue;
        available.push_back(a);
        menu_items.push_back({static_cast<int>(available.size()), a.name, true, false});
    }
    if (menu_items.empty()) {
        gfx::ShowGlassDialog(hwnd_, L"Abrir enlace", L"No hay aplicaciones de PML disponibles.", {L"Aceptar"}, dpi_, 0);
        modal_ = old_modal;
        return;
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
    modal_ = old_modal;
    if (choice > 0 && static_cast<std::size_t>(choice) <= available.size()) {
        auto selected = available[static_cast<std::size_t>(choice - 1)];
        std::wstring err;
        config_.config_doc().SetValue(L"Web", L"App", selected.id, err);
        config_.SaveConfigIniIfModified(false);
        selected.browser = true;
        applications::SaveApp(resolver_, selected, false);
        Launch(selected, jobs::QuoteArgument(target));
    }
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

void Desktop::SearchWeb(bool /*fallback*/) {
    const auto query = Text(edit_);
    if (query.empty() || closing_) return;
    const auto& settings = config_.config_doc();
    const auto url = ExpandWeb(settings.GetValue(L"Web", L"URL", L"https://www.google.com/search?q=$W$"), query);
    OpenTargetUrl(url);
}

void Desktop::Launch(const App& app, std::wstring arguments, bool force_evaluate) {
    if (closing_) return;
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
        gfx::ShowGlassDialog(hwnd_, L"PortableManager Lite", message, {L"Aceptar"}, dpi_, 0);
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
    if (!clipboard_offers_) {
        package_.clear();
        clipboard_url_.clear();
        url_naming_ = false;
        Layout();
        return;
    }
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
        if (index < 0 || static_cast<std::size_t>(index) >= items_.size()) return;
        const auto& clicked = items_[static_cast<std::size_t>(index)];
        if (clicked.kind == Kind::File && Prefix(clicked.value, L"Apps\\") &&
            Equal(fs::path(clicked.value).extension().native(), L".exe")) {
            menu_items.push_back({1, L"Abrir", true, false});
            menu_items.push_back({2, L"Registrar en PML\u2026", !busy_, false});
            POINT cursor{};
            ::GetCursorPos(&cursor);
            ::SetForegroundWindow(hwnd_);
            modal_ = true;
            const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
            modal_ = false;
            if (choice == 1) Open(index);
            else if (choice == 2) RegisterManualApp(clicked.value);
            return;
        }
        if (clicked.kind != Kind::App || applications::Find(resolver_, clicked.value, app)) {
            return;
        }
        for (auto& entry : workers_) if (entry->app.id == app.id) worker = entry.get();
        menu_items.push_back({1, app.archived ? L"Reinstalar\u2026" : L"Abrir", true, false});
        menu_items.push_back({2, L"Cerrar", worker != nullptr, false});
        const bool can_maintain = !worker && !busy_;
        menu_items.push_back({8, L"Evaluar", can_maintain && !app.archived && session_.protected_by_guard() && workers_.empty(), false});
        menu_items.push_back({3, L"Configurar\u2026", true, false});
        menu_items.push_back({5, L"Restablecer\u2026", can_maintain, false});
        menu_items.push_back({6, L"Archivar\u2026", can_maintain, false});
        menu_items.push_back({7, L"Desinstalar\u2026", !busy_, false});
    } else {
        menu_items.push_back({1, L"Escritorio", true, false});
        menu_items.push_back({11, L"Notas", true, false});
        menu_items.push_back({8, L"Configuraci\u00f3n", true, false});
        menu_items.push_back({2, L"Buscar", true, false});
        if (!package_.empty())
            menu_items.push_back({4, L"Elegir destino del portapapeles\u2026", true, false});
        if (!orphan_sessions_.empty()) {
            menu_items.push_back({0, L"", false, true});
            menu_items.push_back({9, L"Recuperar sesiones hu\u00e9rfanas\u2026", !busy_, false});
            menu_items.push_back({10, L"Desechar sesiones hu\u00e9rfanas\u2026", !busy_, false});
        }
        menu_items.push_back({0, L"", false, true});
        menu_items.push_back({5, L"Cerrar", true, false});
        if (show_all_close_modes_) {
            menu_items.push_back({6, L"Cierre r\u00e1pido", true, false});
            menu_items.push_back({7, L"Cierre cr\u00edtico", true, false});
        }
        menu_items.push_back({0, L"", false, true});
        menu_items.push_back({0, std::wstring(L"v") + core::kVersionString, false, false});
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(hwnd_);
    modal_ = true;
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
    if (!application) {
        if (choice == 1) OpenTab(Tab::Desktop);
        if (choice == 11) OpenTab(Tab::Notes);
        if (choice == 8) {
            config_app_id_.clear();
            app_cfg_editing_ = false;
            OpenTab(Tab::Config);
        }
        if (choice == 2) Show(View::Search);
        if (choice == 4) InstallPackage(true);
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
        if (choice == 3) {
            modal_ = false;
            ConfigureAppModal(app);
            return;
        }
        if (choice >= 5 && choice <= 7) {
            const auto action = choice == 5 ? applications::Maintenance::Reset :
                choice == 6 ? applications::Maintenance::Archive : applications::Maintenance::Uninstall;
            const auto text = choice == 6 ? L"\u00bfArchivar esta aplicaci\u00f3n y conservar sus datos?"
                : L"\u00bfConfirmar la eliminaci\u00f3n de los datos portables de esta aplicaci\u00f3n?";
            if (gfx::ShowGlassDialog(hwnd_, app.name, text, {L"S\u00ed", L"No"}, dpi_, 1) == 0) {
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
    const int answer = gfx::ShowGlassDialog(hwnd_, L"Sesiones hu\u00e9rfanas", msg, {L"Restaurar", L"Cancelar"}, dpi_, 0);
    modal_ = was_modal;
    if (answer != 0) return;

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
    const int answer = gfx::ShowGlassDialog(hwnd_, L"Sesiones hu\u00e9rfanas", msg, {L"Desechar", L"Cancelar"}, dpi_, 1);
    modal_ = was_modal;
    if (answer != 0) return;

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
        if (note_edit_) {
            if (note_font_) ::SendMessageW(note_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(note_font_), TRUE);
            note_edit_proc_ = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(note_edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NoteEditProc)));
        }
    }
}

void Desktop::UpdateNoteHostBounds() {
    if (view_ == View::Files && tab_ == Tab::Notes) {
        EnsureNoteHost();
        if (!note_host_ || !note_edit_) return;
        const int pad = ShadowPad();
        const int search_h = S(40);
        const int header_h = S(38);
        const int note_tab_h = S(32);
        const int find_h = S(34);
        const int bottom_pad = S(10);
        const int top_off = search_h + header_h + note_tab_h + S(8);
        const int nx = win_x_ + pad + S(14);
        const int ny = win_y_ + pad + top_off;
        const int nw = std::max(S(120), cur_w_ - S(28));
        const int nh = std::max(S(60), cur_h_ - top_off - find_h - bottom_pad - S(8));
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
        const int ans = gfx::ShowGlassDialog(hwnd_, L"Notas", msg, {L"Eliminar", L"Cancelar"}, dpi_, 1);
        modal_ = old_modal;
        if (ans != 0) return;
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
    if (!note_edit_ || !note_find_edit_) return;
    const auto needle = Text(note_find_edit_);
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

void Desktop::ToggleConfigRow(int idx) {
    if (!config_app_id_.empty()) {
        App cfg_app;
        if (applications::Find(resolver_, config_app_id_, cfg_app) != ERROR_SUCCESS) {
            config_app_id_.clear();
            Render();
            return;
        }
        if (idx >= 0 && idx <= 2) {
            const std::wstring name_disp = cfg_app.aliases.empty() ? cfg_app.name : (cfg_app.name + L", " + cfg_app.aliases);
            app_cfg_target_ = cfg_app;
            app_cfg_field_ = idx + 1;
            app_cfg_editing_ = true;
            const std::wstring initial = (idx == 0) ? name_disp
                : (idx == 1 ? cfg_app.extensions : cfg_app.arguments);
            ::SetWindowTextW(edit_, initial.c_str());
            ::SendMessageW(edit_, EM_SETSEL, 0, -1);
            FocusNode(UiNode::ConfigFieldEdit);
            return;
        }
        if (idx == 3) {
            cfg_app.browser = !cfg_app.browser;
            Report(applications::SaveApp(resolver_, cfg_app, false));
            std::wstring err;
            if (cfg_app.browser) {
                config_.config_doc().SetValue(L"Web", L"App", cfg_app.id, err);
                config_.SaveConfigIniIfModified(false);
            }
            Render();
            return;
        }
        return;
    }

    auto& doc = config_.config_doc();
    std::wstring err;
    if (idx == 0) {
        hotkey_capturing_ = !hotkey_capturing_;
        hotkey_status_ok_ = false;
        hotkey_status_err_ = false;
        hotkey_error_text_.clear();
        FocusNode(hotkey_capturing_ ? UiNode::ConfigFieldEdit : UiNode::ConfigList);
        return;
    }
    hotkey_capturing_ = false;
    if (idx == 1) doc.SetValue(L"Desktop", L"AlwaysOnTop", always_on_top_ ? L"false" : L"true", err);
    else if (idx == 2) {
        const int next_pct = (translucent_pct_ >= 90) ? 0 : ((translucent_pct_ / 10) * 10 + 10);
        doc.SetValue(L"Desktop", L"Translucent", std::to_wstring(next_pct) + L"%", err);
    } else if (idx == 3) doc.SetValue(L"Desktop", L"RememberPosition", remember_pos_ ? L"false" : L"true", err);
    else if (idx == 4) doc.SetValue(L"Desktop", L"ClipboardOffers", clipboard_offers_ ? L"false" : L"true", err);
    else if (idx == 5) doc.SetValue(L"Desktop", L"EnableKill", enable_kill_ ? L"false" : L"true", err);
    else if (idx == 6) doc.SetValue(L"Desktop", L"ShowAllCloseModes", show_all_close_modes_ ? L"false" : L"true", err);
    config_.SaveConfigIniIfModified(false);
    ApplyConfigSettings(true);
    ApplyWindowGeometry();
    Render();
}

void Desktop::ConfigureAppModal(const App& app) {
    if (closing_) return;
    config_app_id_ = app.id;
    app_cfg_editing_ = false;
    OpenTab(Tab::Config);
    config_selected_ = 0;
    FocusNode(UiNode::ConfigList);
}

void Desktop::CommitAppFieldEdit() {
    if (!app_cfg_editing_) return;
    const auto text = Text(edit_);
    App updated = app_cfg_target_;
    if (app_cfg_field_ == 1) {
        const auto parts = Split(text, L',');
        if (!parts.empty()) {
            updated.name = parts.front();
            updated.aliases.clear();
            for (std::size_t i = 1; i < parts.size(); ++i) {
                if (!updated.aliases.empty()) updated.aliases += L", ";
                updated.aliases += parts[i];
            }
        }
    } else if (app_cfg_field_ == 2) {
        updated.extensions = text;
    } else if (app_cfg_field_ == 3) {
        updated.arguments = text;
    }
    app_cfg_editing_ = false;
    ::SetWindowTextW(edit_, L"");
    Report(applications::SaveApp(resolver_, updated, false));
    applications::List(resolver_, apps_);
    FocusNode(UiNode::ConfigList);
}

void Desktop::RegisterManualApp(const std::wstring& rel_exe) {
    if (closing_ || !Prefix(rel_exe, L"Apps\\")) return;
    const fs::path p(rel_exe);
    auto it = p.begin();
    if (it == p.end()) return;
    ++it; // skip "Apps"
    if (it == p.end()) return;
    const std::wstring dir_name = it->native();
    App app;
    app.id = SanitizeUrlName(dir_name);
    if (app.id.empty()) app.id = L"app";
    app.name = p.stem().native();
    app.directory = L"Apps\\" + dir_name;
    app.executable = rel_exe;
    app.paf = false;
    const DWORD err = applications::Register(resolver_, app);
    if (err) {
        Report(err);
        return;
    }
    Populate();
    Launch(app, {});
}

void Desktop::DesktopContextMenu() {
    if (closing_) return;
    auto mark = [](bool on, const wchar_t* label) -> std::wstring {
        return (on ? L"\u2713 " : L"   ") + std::wstring(label);
    };
    std::vector<gfx::MenuEntry> menu_items;
    menu_items.push_back({1, mark(desk_view_ == DesktopViewMode::LargeIcons, L"Ver: Iconos grandes"), true, false});
    menu_items.push_back({2, mark(desk_view_ == DesktopViewMode::SmallIcons, L"Ver: Iconos peque\u00f1os"), true, false});
    menu_items.push_back({3, mark(desk_view_ == DesktopViewMode::List, L"Ver: Lista"), true, false});
    menu_items.push_back({4, mark(desk_view_ == DesktopViewMode::Details, L"Ver: Detalles"), true, false});
    menu_items.push_back({0, L"", false, true});
    menu_items.push_back({5, mark(desk_sort_ == DesktopSortBy::Name, L"Ordenar por: Nombre"), true, false});
    menu_items.push_back({6, mark(desk_sort_ == DesktopSortBy::Modified, L"Ordenar por: Fecha de modificaci\u00f3n"), true, false});
    menu_items.push_back({0, L"", false, true});
    menu_items.push_back({7, mark(desk_group_ == DesktopGroupBy::AppsFirst, L"Agrupar: Apps primero"), true, false});
    menu_items.push_back({8, mark(desk_group_ == DesktopGroupBy::FoldersFirst, L"Agrupar: Carpetas primero"), true, false});
    menu_items.push_back({0, L"", false, true});
    menu_items.push_back({10, L"   Actualizar (F5)", true, false});

    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(hwnd_);
    modal_ = true;
    const auto choice = gfx::ShowGlassMenu(hwnd_, cursor, menu_items, dpi_);
    modal_ = false;
    if (!choice) return;
    if (choice == 10) {
        Populate();
        return;
    }
    std::wstring err;
    auto& doc = config_.config_doc();
    if (choice == 1) doc.SetValue(L"Desktop", L"ViewMode", L"LargeIcons", err);
    else if (choice == 2) doc.SetValue(L"Desktop", L"ViewMode", L"SmallIcons", err);
    else if (choice == 3) doc.SetValue(L"Desktop", L"ViewMode", L"List", err);
    else if (choice == 4) doc.SetValue(L"Desktop", L"ViewMode", L"Details", err);
    else if (choice == 5) doc.SetValue(L"Desktop", L"SortBy", L"Name", err);
    else if (choice == 6) doc.SetValue(L"Desktop", L"SortBy", L"Modified", err);
    else if (choice == 7) doc.SetValue(L"Desktop", L"GroupBy", L"AppsFirst", err);
    else if (choice == 8) doc.SetValue(L"Desktop", L"GroupBy", L"FoldersFirst", err);
    config_.SaveConfigIniIfModified(false);
    ApplyConfigSettings(false);
    Populate();
}

void Desktop::HandleHotkeyCapture(WPARAM vk) {
    if (!hotkey_capturing_) return;
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LWIN || vk == VK_RWIN) {
        return;
    }
    if (vk == VK_ESCAPE && (::GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
        (::GetKeyState(VK_MENU) & 0x8000) == 0 && (::GetKeyState(VK_SHIFT) & 0x8000) == 0) {
        hotkey_capturing_ = false;
        FocusNode(UiNode::ConfigList);
        return;
    }
    const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool win = ((::GetKeyState(VK_LWIN) & 0x8000) != 0) || ((::GetKeyState(VK_RWIN) & 0x8000) != 0);

    std::wstring key_name;
    if (vk == VK_SPACE) key_name = L"Space";
    else if (vk == VK_ESCAPE) key_name = L"Esc";
    else if (vk == VK_TAB) key_name = L"Tab";
    else if (vk == VK_RETURN) key_name = L"Enter";
    else if (vk == VK_DELETE) key_name = L"Supr";
    else if (vk >= VK_F1 && vk <= VK_F12) key_name = L"F" + std::to_wstring(vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) key_name.push_back(static_cast<wchar_t>(vk));
    else return;

    std::wstring combo;
    UINT mods = MOD_NOREPEAT;
    if (ctrl) { combo += L"Ctrl+"; mods |= MOD_CONTROL; }
    if (alt) { combo += L"Alt+"; mods |= MOD_ALT; }
    if (shift) { combo += L"Shift+"; mods |= MOD_SHIFT; }
    if (win) { combo += L"Win+"; mods |= MOD_WIN; }
    combo += key_name;

    hotkey_capturing_ = false;
    hotkey_status_ok_ = false;
    hotkey_status_err_ = false;
    hotkey_error_text_.clear();

    const wchar_t* known_owner = nullptr;
    if (ctrl && !alt && !shift && !win && vk == VK_ESCAPE) known_owner = L"Men\u00fa Inicio";
    else if (ctrl && shift && !alt && !win && vk == VK_ESCAPE) known_owner = L"Administrador de tareas";
    else if (win && !ctrl && !alt && !shift && (vk == 'D' || vk == 'E' || vk == 'R' || vk == 'L')) {
        known_owner = vk == 'D' ? L"Mostrar escritorio" : vk == 'E' ? L"Explorador de archivos" : vk == 'R' ? L"Ejecutar" : L"Bloquear equipo";
    } else if (alt && !ctrl && !shift && !win && (vk == VK_TAB || vk == VK_F4 || vk == VK_SPACE)) {
        known_owner = vk == VK_TAB ? L"Cambiar de ventana" : vk == VK_F4 ? L"Cerrar ventana" : L"Men\u00fa de ventana";
    }
    if (known_owner) {
        hotkey_status_err_ = true;
        hotkey_error_text_ = known_owner;
        FocusNode(UiNode::ConfigList);
        return;
    }

    const bool same_as_active = Equal(combo, registered_hotkey_);
    if (!same_as_active) {
        if (!::RegisterHotKey(hwnd_, 2, mods, static_cast<UINT>(vk))) {
            hotkey_status_err_ = true;
            FocusNode(UiNode::ConfigList);
            return;
        }
        ::UnregisterHotKey(hwnd_, 2);
    }

    std::wstring err;
    config_.config_doc().SetValue(L"Desktop", L"Hotkey", combo, err);
    config_.SaveConfigIniIfModified(false);
    ApplyConfigSettings(true);
    hotkey_status_ok_ = true;
    FocusNode(UiNode::ConfigList);
}

} // namespace

int Run(const PathResolver& resolver) {
    return Desktop(resolver).Run();
}

} // namespace pm::pmd
