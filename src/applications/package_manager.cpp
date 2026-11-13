#include "applications/app_paths.h"
#include "core/config_manager.h"
#include "jobs/job_manager.h"

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <set>
#include <sstream>

namespace pm::applications {
namespace {
namespace fs = std::filesystem;
using core::PathResolver;

constexpr unsigned long long kMaxPackage = 8ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMaxEntries = 100000;

struct PathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

bool Absent(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

DWORD PlainTree(const fs::path& dir, std::size_t& entries, unsigned depth = 0) {
    if (depth >= 32) return ERROR_BUFFER_OVERFLOW;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        const auto name = path.filename().native();
        const auto normalized = PathResolver::NormalizePmRelativePath(name);
        if (!normalized.ok || normalized.resolved_path != name || ++entries > kMaxEntries)
            return ERROR_INVALID_NAME;
        const DWORD attrs = ::GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return ::GetLastError();
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_INVALID_DATA;
        Handle file;
        const DWORD error = attrs & FILE_ATTRIBUTE_DIRECTORY
            ? PlainTree(path, entries, depth + 1) : CheckFile(path.native(), file);
        if (error) return error;
    }
    return ec ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}

// Never descend through a reparse point, including when rejecting a package.
void RemoveTree(const fs::path& root) {
    const DWORD attrs = ::GetFileAttributesW(root.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        ::DeleteFileW(root.c_str());
        return;
    }
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            RemoveTree(it->path());
    }
    ::RemoveDirectoryW(root.c_str());
}

struct Stage {
    fs::path path;
    bool preserve = false;
    Pins parents;
    ~Stage() { if (!preserve && !path.empty()) RemoveTree(path); }

    DWORD Create(const PathResolver& resolver) {
        const DWORD error = EnsureDirectory(resolver, L"Apps\\.temp", parents);
        if (error) return error;
        const auto id = runtime::SessionManager::GenerateSessionGuid();
        if (id.empty()) return ERROR_GEN_FAILURE;
        auto directory = resolver.ResolvePmRelativePath(L"Apps\\.temp\\" + id);
        if (!directory.ok) return ERROR_INVALID_NAME;
        if (!::CreateDirectoryW(directory.resolved_path.c_str(), nullptr)) return ::GetLastError();
        path = directory.resolved_path;
        return ERROR_SUCCESS;
    }
};

DWORD Command(const std::wstring& executable, const std::wstring& args, const fs::path& cwd,
              const fs::path& output = {}) {
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = output.empty() ? sizeof(STARTUPINFOW) : sizeof(startup);
    std::vector<unsigned char> attributes;
    Handle destination, reader, writer, input;
    HANDLE allowed[2]{};
    if (!output.empty()) {
        const auto file = ::CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return ::GetLastError();
        destination.reset(file);
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!::CreatePipe(&read, &write, &inheritable, 0)) return ::GetLastError();
        reader.reset(read);
        writer.reset(write);
        if (!::SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0)) return ::GetLastError();
        if (!::CreatePipe(&read, &write, &inheritable, 0)) return ::GetLastError();
        input.reset(read);
        ::CloseHandle(write); // End of input for tar, which always reads from the named archive.
        SIZE_T bytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return ::GetLastError();
        attributes.resize(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return ::GetLastError();
        allowed[0] = writer.get();
        allowed[1] = input.get();
        if (!::UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         allowed, sizeof(allowed), nullptr, nullptr)) {
            const auto error = ::GetLastError();
            ::DeleteProcThreadAttributeList(startup.lpAttributeList);
            return error;
        }
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = writer.get();
        startup.StartupInfo.hStdError = writer.get();
        startup.StartupInfo.hStdInput = input.get();
    }
    auto command = jobs::QuoteArgument(executable) + L" " + args;
    PROCESS_INFORMATION process{};
    const DWORD flags = destination ? CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT : 0;
    const bool started = ::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                                          destination != nullptr, flags, nullptr, cwd.c_str(),
                                          &startup.StartupInfo, &process) != FALSE;
    DWORD error = started ? ERROR_SUCCESS : ::GetLastError();
    if (startup.lpAttributeList) ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!started) return error;
    ::CloseHandle(process.hThread);
    const Handle child(process.hProcess);
    writer.reset();
    input.reset();
    if (destination) {
        constexpr DWORD kMaxOutput = 32 * 1024 * 1024;
        const auto deadline = ::GetTickCount64() + 300000;
        DWORD total = 0;
        for (;;) {
            DWORD available = 0;
            if (!::PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr)) {
                const auto cause = ::GetLastError();
                if (cause != ERROR_BROKEN_PIPE) error = cause;
                break;
            }
            if (available) {
                char buffer[4096];
                DWORD size = 0, written = 0;
                if (!::ReadFile(reader.get(), buffer, std::min(available, static_cast<DWORD>(sizeof(buffer))),
                                &size, nullptr) ||
                    !::WriteFile(destination.get(), buffer, size, &written, nullptr)) {
                    error = ::GetLastError();
                    break;
                }
                total += written;
                if (written != size || total > kMaxOutput) { error = ERROR_BUFFER_OVERFLOW; break; }
            } else {
                // A child can write and exit between PeekNamedPipe and this
                // wait. Drain its pipe until EOF even after it exits.
                ::Sleep(15);
            }
            if (::GetTickCount64() > deadline) { error = ERROR_TIMEOUT; break; }
        }
    }
    if (!error && ::WaitForSingleObject(child.get(), destination ? 5000 : INFINITE) != WAIT_OBJECT_0)
        error = ERROR_TIMEOUT;
    if (error) {
        ::TerminateProcess(child.get(), error);
        ::WaitForSingleObject(child.get(), INFINITE);
        return error;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(child.get(), &code)) return ::GetLastError();
    if (!code) return ERROR_SUCCESS;
    if (destination) {
        destination.reset();
        const auto diagnostic = core::AtomicFile::ReadUtf8Text(output.native(), true);
        if (diagnostic.ok && diagnostic.content.size() < 4096)
            std::fprintf(stderr, "[PML] tar.exe (%lu): %s\n", code, diagnostic.content.c_str());
    }
    return ERROR_INSTALL_FAILURE;
}


DWORD ValidateListing(const fs::path& paths, const fs::path& verbose, unsigned long long available) {
    const auto names = core::AtomicFile::ReadUtf8Text(paths.native(), true);
    const auto details = core::AtomicFile::ReadUtf8Text(verbose.native(), true);
    if (!names.ok) return names.win32_error;
    if (!details.ok) return details.win32_error;
    std::istringstream file_names(names.content), file_details(details.content);
    std::set<std::wstring, PathLess> unique;
    std::string name, line;
    unsigned long long total = 0;
    while (std::getline(file_names, name)) {
        if (!std::getline(file_details, line) || line.empty() || (line.front() != '-' && line.front() != 'd'))
            return ERROR_INVALID_DATA;
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.starts_with("./")) name.erase(0, 2);
        if (!name.empty() && name.back() == '/') name.pop_back();
        if (name == "." || name.empty()) {
            if (line.front() != 'd') return ERROR_INVALID_DATA;
            continue;
        }
        if (name.find('\\') != std::string::npos || name.size() > 220)
            return ERROR_INVALID_NAME;
        auto relative = core::IniDocument::Utf8ToWide(name);
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        const auto norm = PathResolver::NormalizePmRelativePath(relative);
        if (!norm.ok || norm.resolved_path != relative || !unique.insert(relative).second ||
            unique.size() > kMaxEntries) return ERROR_INVALID_NAME;
        std::istringstream fields(line);
        std::string permissions, count, owner, group;
        unsigned long long size = 0;
        if (!(fields >> permissions >> count >> owner >> group >> size) || size > kMaxPackage - total)
            return ERROR_FILE_TOO_LARGE;
        total += size;
    }
    if (unique.empty() || std::getline(file_details, line)) return ERROR_INVALID_DATA;
    return total > available ? ERROR_DISK_FULL : ERROR_SUCCESS;
}

DWORD Extract(const fs::path& stage, const fs::path& archive, const fs::path& target) {
    wchar_t system[MAX_PATH]{};
    if (!::GetSystemDirectoryW(system, MAX_PATH)) return ::GetLastError();
    const auto tar = (fs::path(system) / L"tar.exe").native();
    const auto input = jobs::QuoteArgument(archive.native());
    const auto names = stage / L"paths.txt";
    const auto details = stage / L"entries.txt";
    DWORD error = Command(tar, L"-tf " + input, stage, names);
    if (error) return error;
    error = Command(tar, L"-tvf " + input, stage, details);
    if (error) return error;
    ULARGE_INTEGER available{};
    if (!::GetDiskFreeSpaceExW(stage.c_str(), &available, nullptr, nullptr)) return ::GetLastError();
    error = ValidateListing(names, details, available.QuadPart);
    if (error) {
        std::fprintf(stderr, "[PML] Validacion del paquete (Win32 %lu).\\n", error);
        return error;
    }
    return Command(tar, L"-xf " + input + L" -C " + jobs::QuoteArgument(target.native()) +
                        L" --no-same-owner --no-same-permissions", stage, stage / L"extract.txt");
}

DWORD ChooseApp(bool paf, std::wstring_view chosen, fs::path& root,
                std::wstring& executable, std::vector<std::wstring>* choices) {
    std::size_t count = 0;
    DWORD error = PlainTree(root, count);
    if (error) return error;
    const auto scan = [](const fs::path& dir, std::vector<fs::path>& exes,
                         std::vector<fs::path>& directories) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) directories.push_back(it->path());
            else if (Equal(it->path().extension().native(), L".exe")) exes.push_back(it->path());
        }
        return ec ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
    };
    std::vector<fs::path> exes, folders;
    error = scan(root, exes, folders);
    if (error) return error;
    if (exes.empty() && folders.size() == 1) {
        root = folders.front();
        folders.clear();
        error = scan(root, exes, folders);
        if (error) return error;
    }
    if (paf) {
        const auto info_path = root / L"App" / L"AppInfo" / L"appinfo.ini";
        const auto info = core::AtomicFile::ReadUtf8Text(info_path.native(), true);
        if (!info.ok) return info.win32_error;
        core::IniDocument appinfo;
        std::wstring message;
        if (!appinfo.ParseUtf8(info.content, message)) return ERROR_INVALID_DATA;
        executable = appinfo.GetValue(L"Control", L"Start");
        const auto norm = PathResolver::NormalizePmRelativePath(executable);
        if (!norm.ok || norm.resolved_path != executable || executable.find(L'\\') != std::wstring::npos ||
            (!chosen.empty() && !Equal(chosen, executable))) return ERROR_INVALID_DATA;
        Handle file;
        return CheckFile((root / executable).native(), file);
    }
    if (!chosen.empty()) {
        executable = chosen;
        std::replace(executable.begin(), executable.end(), L'/', L'\\');
        const auto norm = PathResolver::NormalizePmRelativePath(executable);
        if (!norm.ok || norm.resolved_path != executable) return ERROR_INVALID_NAME;
        Handle file;
        return CheckFile((root / executable).native(), file);
    }
    if (exes.size() == 1) {
        executable = exes.front().filename().native();
        return ERROR_SUCCESS;
    }
    if (exes.empty()) return ERROR_FILE_NOT_FOUND;
    if (choices)
        for (const auto& exe : exes) choices->push_back(exe.filename().native());
    return ERROR_MORE_DATA;
}

struct Move {
    fs::path from;
    fs::path to;
};

class Transaction {
public:
    Transaction(const fs::path& stage, const fs::path& portable)
        : journal_(stage / L"session.journal"), portable_(portable) {}

    DWORD Rename(const fs::path& from, const fs::path& to) {
        auto next = text_ + L"MOVE\t" + Relative(from) + L"\t" + Relative(to) + L"\r\n";
        const DWORD error = Persist(next);
        if (error) return error;
        if (!::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return ::GetLastError();
        text_ = std::move(next);
        moves_.push_back({from, to});
        return ERROR_SUCCESS;
    }

    DWORD Directory(const fs::path& dir) {
        auto next = text_ + L"MKDIR\t" + Relative(dir) + L"\r\n";
        const DWORD error = Persist(next);
        if (error) return error;
        if (!::CreateDirectoryW(dir.c_str(), nullptr)) return ::GetLastError();
        text_ = std::move(next);
        directories_.push_back(dir);
        return ERROR_SUCCESS;
    }

    bool Undo() {
        bool ok = true;
        for (auto it = moves_.rbegin(); it != moves_.rend(); ++it)
            if (!::MoveFileExW(it->to.c_str(), it->from.c_str(), MOVEFILE_WRITE_THROUGH)) ok = false;
        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it)
            if (!::RemoveDirectoryW(it->c_str())) ok = false;
        return ok;
    }

private:
    std::wstring Relative(const fs::path& path) const {
        return path.lexically_relative(portable_).native();
    }

    DWORD Persist(const std::wstring& text) {
        return core::AtomicFile::WriteUtf8Text(journal_.native(), core::IniDocument::WideToUtf8(text)).win32_error;
    }

    fs::path journal_;
    fs::path portable_;
    std::wstring text_;
    std::vector<Move> moves_;
    std::vector<fs::path> directories_;
};

DWORD Overlay(const fs::path& source, const fs::path& dest, const fs::path& backup,
              bool paf, Transaction& txn, bool root = true) {
    std::error_code ec;
    // Snapshot iteration because successful moves remove the current entries.
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec))
        entries.push_back(it->path());
    if (ec) return ERROR_ACCESS_DENIED;
    for (const auto& path : entries) {
        const auto name = path.filename();
        if (root && Equal(name.native(), L"wrapper.ini")) continue;
        const auto target = dest / name;
        const DWORD attrs = ::GetFileAttributesW(target.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES && !Absent(::GetLastError())) return ::GetLastError();
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return ERROR_INVALID_DATA;
        if (paf && root && Equal(name.native(), L"Data") && attrs != INVALID_FILE_ATTRIBUTES) continue;
        const DWORD incoming = ::GetFileAttributesW(path.c_str());
        if (incoming == INVALID_FILE_ATTRIBUTES) return ::GetLastError();
        if (incoming & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_INVALID_DATA;
        if (incoming & FILE_ATTRIBUTE_DIRECTORY) {
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = txn.Directory(target);
                if (error) return error;
            } else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                return ERROR_DIRECTORY;
            }
            const DWORD error = Overlay(path, target, backup, paf, txn, false);
            if (error) return error;
        } else {
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                Handle existing;
                const DWORD validated = CheckFile(target.native(), existing);
                if (validated) return validated;
                existing.reset();
                const auto displaced = backup / runtime::SessionManager::GenerateSessionGuid();
                const DWORD error = txn.Rename(target, displaced);
                if (error) return error;
            }
            const DWORD error = txn.Rename(path, target);
            if (error) return error;
        }
    }
    return ERROR_SUCCESS;
}

} // namespace

DWORD Install(const PathResolver& resolver, std::wstring_view id,
              const std::wstring& package, std::wstring_view selected_executable,
              std::vector<std::wstring>* executable_choices) {
    if (executable_choices) executable_choices->clear();
    if (!SafeId(id)) return ERROR_INVALID_NAME;
    if (package.size() < 4 || package[1] != L':' || package[2] != L'\\') return ERROR_INVALID_NAME;
    Pins source_pins;
    DWORD error = PinDirectory(package.substr(0, package.rfind(L'\\')), source_pins);
    if (error) return error;
    Handle source;
    error = CheckFile(package, source);
    if (error) return error;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(source.get(), &size)) return ::GetLastError();
    if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) > kMaxPackage)
        return ERROR_FILE_TOO_LARGE;
    const auto suffix = fs::path(package).extension().native();
    const auto filename = fs::path(package).filename().native();
    const auto normalized_name = PathResolver::NormalizePmRelativePath(filename);
    if (!normalized_name.ok || normalized_name.resolved_path != filename) return ERROR_INVALID_NAME;
    const bool paf = filename.size() > 8 && Equal(filename.substr(filename.size() - 8), L".paf.exe");
    if (!paf && !Equal(suffix, L".zip") && !Equal(suffix, L".7z") &&
        !Equal(suffix, L".rar") && !Equal(suffix, L".tar")) return ERROR_NOT_SUPPORTED;

    App app;
    const DWORD found = Find(resolver, id, app);
    if (found == ERROR_NOT_FOUND) {
        app.id = id;
        app.name = id;
        app.directory = L"Apps\\" + std::wstring(id);
        app.paf = paf;
    } else if (found) return found;
    if (app.paf != paf) return ERROR_INVALID_PARAMETER;
    const auto install = resolver.ResolvePmRelativePath(app.directory);
    if (!install.ok) return ERROR_INVALID_NAME;
    Handle lease;
    error = AcquireApp(resolver, app.directory, lease);
    if (error) return error;
    const fs::path target = install.resolved_path;
    const DWORD attrs = ::GetFileAttributesW(target.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (found == ERROR_NOT_FOUND || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                                             (attrs & FILE_ATTRIBUTE_REPARSE_POINT))) return ERROR_ALREADY_EXISTS;
    if (attrs == INVALID_FILE_ATTRIBUTES && !Absent(::GetLastError())) return ::GetLastError();
    Stage stage;
    error = stage.Create(resolver);
    if (error) return error;
    const auto snapshot = stage.path / (L"package" + suffix);
    if (!::CopyFileW(package.c_str(), snapshot.c_str(), TRUE)) return ::GetLastError();
    source.reset();
    source_pins.clear();
    auto payload = stage.path / L"payload";
    if (!::CreateDirectoryW(payload.c_str(), nullptr)) return ::GetLastError();
    if (paf) {
        const auto destination = jobs::QuoteArgument(payload.native() + L"\\");
        error = Command(snapshot.native(), L"/DESTINATION=" + destination, stage.path);
    } else {
        error = Extract(stage.path, snapshot, payload);
    }
    if (error) return error;
    std::wstring executable;
    error = ChooseApp(paf, selected_executable, payload, executable, executable_choices);
    if (error) return error;
    app.executable = app.directory + L"\\" + executable;
    app.archived = false;
    error = ValidateApp(resolver, app, false);
    if (error) return error;
    const auto backup = stage.path / L"replaced";
    if (!::CreateDirectoryW(backup.c_str(), nullptr)) return ::GetLastError();
    const auto archived = fs::path(resolver.ResolvePmRelativePath(L"Data\\archived\\" + app.id).resolved_path);
    Transaction txn(stage.path, fs::path(resolver.ResolvePmRelativePath(L"Apps").resolved_path).parent_path());
    if (attrs == INVALID_FILE_ATTRIBUTES) error = txn.Directory(target);
    Pins target_pins;
    if (!error) error = PinDirectory(target.native(), target_pins);
    if (!error) error = Overlay(payload, target, backup, paf, txn);
    const DWORD archived_attrs = ::GetFileAttributesW(archived.c_str());
    if (!error && archived_attrs != INVALID_FILE_ATTRIBUTES) {
        Pins archive_pins;
        error = PinDirectory(archived.native(), archive_pins);
        for (const auto* name : {L"Data", L"wrapper.ini"}) {
            if (error) break;
            const auto saved = archived / name;
            const DWORD saved_attrs = ::GetFileAttributesW(saved.c_str());
            if (saved_attrs == INVALID_FILE_ATTRIBUTES) continue;
            if (saved_attrs & FILE_ATTRIBUTE_REPARSE_POINT) { error = ERROR_INVALID_DATA; break; }
            const auto dest = target / name;
            if (::GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
                error = txn.Rename(dest, backup / name);
                if (error) break;
            }
            error = txn.Rename(saved, dest);
        }
    }
    if (!error) error = SaveApp(resolver, app, false);
    target_pins.clear();
    if (error) {
        stage.preserve = !txn.Undo();
        return stage.preserve ? ERROR_RECOVERY_FAILURE : error;
    }
    if (archived_attrs != INVALID_FILE_ATTRIBUTES) ::RemoveDirectoryW(archived.c_str());
    return ERROR_SUCCESS;
}

DWORD Maintain(const PathResolver& resolver, const App& selected, Maintenance action) {
    App app = selected;
    DWORD error = ValidateApp(resolver, app, false);
    if (error) return error;
    Handle lease;
    error = AcquireApp(resolver, app.directory, lease);
    if (error) return error;
    if (action == Maintenance::Archive && app.archived) return ERROR_SUCCESS;
    Stage stage;
    error = stage.Create(resolver);
    if (error) return error;
    Transaction txn(stage.path, fs::path(resolver.ResolvePmRelativePath(L"Apps").resolved_path).parent_path());
    const auto install = fs::path(resolver.ResolvePmRelativePath(app.directory).resolved_path);
    const auto data = fs::path(resolver.ResolvePmRelativePath(L"Data\\apps\\" + app.id).resolved_path);
    const auto archive_rel = L"Data\\archived\\" + app.id;
    const auto archive = fs::path(resolver.ResolvePmRelativePath(archive_rel).resolved_path);
    const auto move = [&](const fs::path& path, const fs::path& dest) -> DWORD {
        const DWORD attrs = ::GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD code = ::GetLastError();
            return Absent(code) ? ERROR_SUCCESS : code;
        }
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_INVALID_DATA;
        Pins pins;
        const DWORD pin = PinDirectory(path.parent_path().native(), pins);
        if (pin) return pin;
        return txn.Rename(path, dest);
    };
    if (action == Maintenance::Archive) {
        Pins archive_pins;
        error = EnsureDirectory(resolver, archive_rel, archive_pins);
        if (!error) error = move(install / L"wrapper.ini", archive / L"wrapper.ini");
        if (!error && app.paf) error = move(install / L"Data", archive / L"Data");
        if (!error) error = move(install, stage.path / L"application");
        app.archived = true;
        if (!error) error = SaveApp(resolver, app, false);
    } else {
        error = move(data, stage.path / L"data");
        if (!error && action == Maintenance::Reset && app.paf)
            error = move((app.archived ? archive : install) / L"Data", stage.path / L"paf-data");
        if (!error && action == Maintenance::Uninstall) {
            error = move(install, stage.path / L"application");
            if (!error) error = move(archive, stage.path / L"archived");
            if (!error) error = SaveApp(resolver, app, true);
        }
    }
    if (error) {
        stage.preserve = !txn.Undo();
        return stage.preserve ? ERROR_RECOVERY_FAILURE : error;
    }
    return ERROR_SUCCESS;
}

} // namespace pm::applications
