#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winioctl.h>
#include <winreg.h>
#include <shellapi.h>

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Verificación de característica C++20 (concepts + consteval)
template <typename T>
concept IntegralHandleCode = std::integral<T>;

consteval std::uint32_t CompileTimeTagCheck(std::uint32_t tag) {
    return tag;
}

static_assert(CompileTimeTagCheck(IO_REPARSE_TAG_MOUNT_POINT) == 0xA0000003U,
              "IO_REPARSE_TAG_MOUNT_POINT debe ser 0xA0000003");

#pragma pack(push, 1)
struct MountPointReparseBuffer {
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    WCHAR PathBuffer[1024];
};
#pragma pack(pop)

namespace {

std::wstring GetModulePath() {
    std::wstring buf(MAX_PATH, L'\0');
    DWORD copied = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(copied);
    return buf;
}

bool TestJobObjectAndChildProcess(const std::wstring& self_exe) {
    const std::wstring job_name = L"Local\\PM_ToolchainProbe_Job_" + std::to_wstring(::GetCurrentProcessId());
    HANDLE hJob = ::CreateJobObjectW(nullptr, job_name.c_str());
    if (!hJob) {
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        ::CloseHandle(hJob);
        return false;
    }

    HANDLE hOpenedJob = ::OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS | JOB_OBJECT_TERMINATE | SYNCHRONIZE, FALSE, job_name.c_str());
    if (!hOpenedJob) {
        ::CloseHandle(hJob);
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + self_exe + L"\" --child";
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(hOpenedJob);
        ::CloseHandle(hJob);
        return false;
    }

    const BOOL assigned = ::AssignProcessToJobObject(hOpenedJob, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, 5000);
    DWORD exit_code = 999;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::TerminateJobObject(hOpenedJob, 0);

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(hOpenedJob);
    ::CloseHandle(hJob);
    return assigned != FALSE && exit_code == 0;
}

bool TestDirectoryJunctionInTemp() {
    wchar_t temp_path[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp_path);
    const std::wstring base_dir = std::wstring(temp_path) + L"PM_ToolchainProbe_" + std::to_wstring(::GetCurrentProcessId());
    const std::wstring target_dir = base_dir + L"\\target";
    const std::wstring link_dir = base_dir + L"\\junction";

    ::CreateDirectoryW(base_dir.c_str(), nullptr);
    ::CreateDirectoryW(target_dir.c_str(), nullptr);
    ::CreateDirectoryW(link_dir.c_str(), nullptr);

    HANDLE hDir = ::CreateFileW(
        link_dir.c_str(),
        GENERIC_WRITE | GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (hDir == INVALID_HANDLE_VALUE) {
        ::RemoveDirectoryW(link_dir.c_str());
        ::RemoveDirectoryW(target_dir.c_str());
        ::RemoveDirectoryW(base_dir.c_str());
        return false;
    }

    const std::wstring subst_name = L"\\??\\" + target_dir;
    const std::wstring print_name = target_dir;

    MountPointReparseBuffer rdb{};
    rdb.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rdb.SubstituteNameOffset = 0;
    rdb.SubstituteNameLength = static_cast<WORD>(subst_name.size() * sizeof(WCHAR));
    rdb.PrintNameOffset = static_cast<WORD>(rdb.SubstituteNameLength + sizeof(WCHAR));
    rdb.PrintNameLength = static_cast<WORD>(print_name.size() * sizeof(WCHAR));
    rdb.ReparseDataLength = static_cast<WORD>(8 + rdb.SubstituteNameLength + sizeof(WCHAR) + rdb.PrintNameLength + sizeof(WCHAR));

    std::memcpy(rdb.PathBuffer, subst_name.c_str(), rdb.SubstituteNameLength);
    std::memcpy(reinterpret_cast<char*>(rdb.PathBuffer) + rdb.PrintNameOffset, print_name.c_str(), rdb.PrintNameLength);

    DWORD bytes_returned = 0;
    const DWORD total_size = 8 + rdb.ReparseDataLength;
    const BOOL set_ok = ::DeviceIoControl(
        hDir, FSCTL_SET_REPARSE_POINT, &rdb, total_size, nullptr, 0, &bytes_returned, nullptr);

    MountPointReparseBuffer query_rdb{};
    const BOOL get_ok = ::DeviceIoControl(
        hDir, FSCTL_GET_REPARSE_POINT, nullptr, 0, &query_rdb, sizeof(query_rdb), &bytes_returned, nullptr);

    ::CloseHandle(hDir);

    const DWORD attrs = ::GetFileAttributesW(link_dir.c_str());
    const bool is_reparse = (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0);

    const BOOL removed_link = ::RemoveDirectoryW(link_dir.c_str());
    const BOOL removed_target = ::RemoveDirectoryW(target_dir.c_str());
    const BOOL removed_base = ::RemoveDirectoryW(base_dir.c_str());

    return set_ok && get_ok && (query_rdb.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) &&
           is_reparse && removed_link && removed_target && removed_base;
}

bool TestRegistryAndVersionAndShellApisReadOnly() {
    // 1. Verificar enlace real a RegCopyTreeW y RegDeleteTreeW en advapi32.dll
    // sin modificar ninguna clave del registro de Windows (Restricción Sección 6).
    auto pfn_copy_tree = &::RegCopyTreeW;
    auto pfn_delete_tree = &::RegDeleteTreeW;
    if (pfn_copy_tree == nullptr || pfn_delete_tree == nullptr) {
        return false;
    }

    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD subkeys = 0;
    const LSTATUS qrc = ::RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &subkeys,
                                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ::RegCloseKey(hKey);
    if (qrc != ERROR_SUCCESS) {
        return false;
    }

    // 2. Verificar version.lib (GetFileVersionInfoSizeW / GetFileVersionInfoW / VerQueryValueW)
    DWORD dummy = 0;
    const DWORD ver_size = ::GetFileVersionInfoSizeW(L"kernel32.dll", &dummy);
    if (ver_size == 0) {
        return false;
    }
    std::vector<std::uint8_t> ver_buf(ver_size);
    if (!::GetFileVersionInfoW(L"kernel32.dll", 0, ver_size, ver_buf.data())) {
        return false;
    }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffi_len = 0;
    if (!::VerQueryValueW(ver_buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffi_len) || !ffi) {
        return false;
    }

    // 3. Verificar shell32.lib (CommandLineToArgvW)
    int num_args = 0;
    LPWSTR* parsed = ::CommandLineToArgvW(::GetCommandLineW(), &num_args);
    if (!parsed || num_args < 1) {
        return false;
    }
    ::LocalFree(parsed);
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--child") {
        return 0;
    }

    const std::wstring self_exe = GetModulePath();
    if (self_exe.empty()) {
        std::fwprintf(stderr, L"[PROBE] FAIL: GetModuleFileNameW\n");
        return 1;
    }

    if (!TestJobObjectAndChildProcess(self_exe)) {
        std::fwprintf(stderr, L"[PROBE] FAIL: JobObject / CreateProcessW\n");
        return 2;
    }

    if (!TestDirectoryJunctionInTemp()) {
        std::fwprintf(stderr, L"[PROBE] FAIL: DirectoryJunction / DeviceIoControl\n");
        return 3;
    }

    if (!TestRegistryAndVersionAndShellApisReadOnly()) {
        std::fwprintf(stderr, L"[PROBE] FAIL: Advapi32 / Version / Shell32\n");
        return 4;
    }

    std::wprintf(L"[PROBE] ALL_CHECKS_PASSED | Module=%ls | C++=%ld\n", self_exe.c_str(), __cplusplus);
    return 0;
}
