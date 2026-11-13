#include "applications/app_paths.h"

#include <algorithm>

namespace pm::applications {

bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && (a.empty() ||
        ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                               b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL);
}

bool Within(std::wstring_view path, std::wstring_view parent) {
    return path.size() > parent.size() && path[parent.size()] == L'\\' &&
           Equal(path.substr(0, parent.size()), parent);
}

bool SafeId(std::wstring_view id) {
    constexpr std::wstring_view allowed = L"abcdefghijklmnopqrstuvwxyz0123456789-_";
    return !id.empty() && id.size() <= 60 && id.find_first_not_of(allowed) == std::wstring_view::npos &&
           core::PathResolver::NormalizePmRelativePath(id).ok;
}

DWORD PinDirectory(const std::wstring& path, Pins& pins) {
    if (path.size() <= 3 || path[1] != L':' || path[2] != L'\\') return ERROR_INVALID_NAME;
    const auto tail = std::wstring_view(path).substr(3);
    const auto normalized = core::PathResolver::NormalizePmRelativePath(tail);
    if (!normalized.ok || normalized.resolved_path != tail) return ERROR_INVALID_NAME;
    for (auto end = path.find(L'\\', 3);; end = path.find(L'\\', end + 1)) {
        const auto prefix = path.substr(0, end);
        HANDLE raw = ::CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return ::GetLastError();
        Handle handle(raw);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(raw, &info)) return ::GetLastError();
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_DIRECTORY;
        pins.push_back(std::move(handle));
        if (end == std::wstring::npos) return ERROR_SUCCESS;
    }
}

DWORD EnsureDirectory(const core::PathResolver& resolver, std::wstring_view relative, Pins& pins) {
    const auto target = resolver.ResolvePmRelativePath(relative);
    if (!target.ok) return ERROR_INVALID_NAME;
    const auto& path = target.resolved_path;
    const auto start = resolver.root_dir().size();
    for (auto end = path.find(L'\\', start + 1);; end = path.find(L'\\', end + 1)) {
        const auto part = path.substr(0, end);
        const auto parent = part.substr(0, part.rfind(L'\\'));
        Pins checked_parent;
        if (parent.size() > 2) {
            const DWORD error = PinDirectory(parent, checked_parent);
            if (error) return error;
        }
        if (!::CreateDirectoryW(part.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return ::GetLastError();
        if (end == std::wstring::npos) return PinDirectory(part, pins);
    }
}

DWORD CheckFile(const std::wstring& path, Handle& file) {
    HANDLE raw = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return ::GetLastError();
    file.reset(raw);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(raw, &info)) return ::GetLastError();
    if (info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

DWORD AcquireApp(const core::PathResolver& resolver, std::wstring_view directory, Handle& lease) {
    // Canonical portable root identity avoids separate locks for drive-letter
    // or short-name aliases of the same root. The installation is a direct child of App.
    Pins pins;
    BY_HANDLE_FILE_INFORMATION info{};
    auto root = resolver.root_dir();
    if (root.size() == 2 && root[1] == L':') root.push_back(L'\\');
    if (root.size() == 3 && root[1] == L':' && root[2] == L'\\') {
        HANDLE raw = ::CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return ::GetLastError();
        pins.emplace_back(raw);
    } else {
        const DWORD error = PinDirectory(root, pins);
        if (error) return error;
    }
    if (!::GetFileInformationByHandle(pins.back().get(), &info)) return ::GetLastError();
    const auto name = L"Global\\PML_App_" + std::to_wstring(info.dwVolumeSerialNumber) + L"_" +
        std::to_wstring(info.nFileIndexHigh) + L"_" + std::to_wstring(info.nFileIndexLow) + L"_" +
        std::wstring(directory.substr(directory.rfind(L'\\') + 1));
    auto folded = name;
    ::CharLowerBuffW(folded.data() + 7, static_cast<DWORD>(folded.size() - 7));
    ::SetLastError(ERROR_SUCCESS);
    lease.reset(::CreateEventW(nullptr, TRUE, FALSE, folded.c_str()));
    const DWORD error = ::GetLastError();
    if (!lease || error == ERROR_ALREADY_EXISTS) {
        lease.reset();
        return error == ERROR_ALREADY_EXISTS ? ERROR_BUSY : error;
    }
    return ERROR_SUCCESS;
}

DWORD ValidateApp(const core::PathResolver& resolver, App& app, bool check_on_disk) {
    if (!SafeId(app.id)) return ERROR_INVALID_NAME;
    std::replace(app.directory.begin(), app.directory.end(), L'/', L'\\');
    std::replace(app.executable.begin(), app.executable.end(), L'/', L'\\');
    // Older apps.ini entries may supply only Executable/Launcher. The layout
    // fixes the installation boundary at the first directory below Apps.
    if (app.directory.empty() && Within(app.executable, L"Apps")) {
        const auto end = app.executable.find(L'\\', 5);
        if (end != std::wstring::npos) app.directory = app.executable.substr(0, end);
    }
    const auto directory = core::PathResolver::NormalizePmRelativePath(app.directory);
    const auto exe = core::PathResolver::NormalizePmRelativePath(app.executable);
    if (!directory.ok || directory.resolved_path != app.directory ||
        !Within(app.directory, L"Apps") || app.directory.find(L'\\', 5) != std::wstring::npos ||
        app.directory[5] == L'.' || app.directory.find(L'~') != std::wstring::npos ||
        !exe.ok || exe.resolved_path != app.executable || !Within(app.executable, app.directory) ||
        app.executable.size() <= 4 || !Equal(std::wstring_view(app.executable).substr(app.executable.size() - 4), L".exe"))
        return ERROR_INVALID_NAME;
    const auto executable_suffix = app.executable.substr(app.directory.size());
    app.directory.replace(0, 4, L"Apps");
    app.executable = app.directory + executable_suffix;
    if (!check_on_disk || app.archived) return ERROR_SUCCESS;
    const auto resolved = resolver.ResolvePmRelativePath(app.executable);
    if (!resolved.ok) return ERROR_INVALID_NAME;
    Pins pins;
    const DWORD error = PinDirectory(resolved.resolved_path.substr(0, resolved.resolved_path.rfind(L'\\')), pins);
    Handle file;
    return error ? error : CheckFile(resolved.resolved_path, file);
}

} // namespace pm::applications
