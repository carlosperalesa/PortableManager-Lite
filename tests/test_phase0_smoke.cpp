#include <windows.h>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

bool RunProcessCaptureOutput(
    const std::wstring& cmd_line,
    const wchar_t* working_dir,
    DWORD& exit_code,
    std::string& stdout_text) {

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!::CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return false;
    }
    ::SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    const BOOL created = ::CreateProcessW(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        working_dir,
        &si,
        &pi);

    ::CloseHandle(hWritePipe);

    if (!created) {
        ::CloseHandle(hReadPipe);
        return false;
    }

    stdout_text.clear();
    char buffer[512];
    DWORD bytes_read = 0;
    while (::ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        stdout_text.append(buffer, bytes_read);
    }

    ::WaitForSingleObject(pi.hProcess, 10000);
    ::GetExitCodeProcess(pi.hProcess, &exit_code);

    ::CloseHandle(hReadPipe);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::fwprintf(stderr, L"Uso: test_phase0_smoke.exe <ruta_a_pml.exe>\n");
        return 2;
    }

    const std::wstring pm_path = argv[1];
    const std::size_t last_sep = pm_path.find_last_of(L"\\/");
    const std::wstring expected_root = (last_sep != std::wstring::npos) ? pm_path.substr(0, last_sep) : L"";
    const std::string expected_root_utf8 = WideToUtf8(expected_root);

    wchar_t temp_dir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp_dir);

    // 1. Arranque CLI (--ready) desde directorio de trabajo distinto (%TEMP%)
    DWORD exit_code = 999;
    std::string out_normal;
    if (!RunProcessCaptureOutput(L"\"" + pm_path + L"\" --ready", temp_dir, exit_code, out_normal) ||
        exit_code != 0 ||
        out_normal.find("Ready.") == std::string::npos ||
        out_normal.find("Root=" + expected_root_utf8) == std::string::npos) {
        std::fwprintf(stderr, L"[FAIL] 1. Arranque normal fallo (rc=%lu)\n", exit_code);
        return 1;
    }

    // 2. Ejecución de --smoke-test desde otro directorio de trabajo (%TEMP%) verificando GetModuleFileNameW
    exit_code = 999;
    std::string out_smoke;
    if (!RunProcessCaptureOutput(L"\"" + pm_path + L"\" --smoke-test", temp_dir, exit_code, out_smoke) ||
        exit_code != 0 ||
        out_smoke.find("[PM] SMOKE_OK") == std::string::npos ||
        out_smoke.find("Root=" + expected_root_utf8) == std::string::npos) {
        std::fwprintf(stderr, L"[FAIL] 2. --smoke-test fallo (rc=%lu)\n", exit_code);
        return 1;
    }

    // 3. Fase 3: el protocolo real exige handles heredados; ya no acepta el stub.
    exit_code = 999;
    std::string out_guard_ok;
    if (!RunProcessCaptureOutput(L"\"" + pm_path + L"\" --guard fase0b_session_test", temp_dir, exit_code, out_guard_ok) ||
        exit_code != 2) {
        std::fwprintf(stderr, L"[FAIL] 3. --guard <SessionID> fallo (rc=%lu)\n", exit_code);
        return 1;
    }

    // 4. Invocación de --guard sin SessionID (debe rechazar con código 2)
    exit_code = 0;
    std::string out_guard_err;
    if (!RunProcessCaptureOutput(L"\"" + pm_path + L"\" --guard", temp_dir, exit_code, out_guard_err) ||
        exit_code != 2) {
        std::fwprintf(stderr, L"[FAIL] 4. --guard sin SessionID debia devolver 2, devolvio %lu\n", exit_code);
        return 1;
    }

    std::wprintf(L"[PASS] test_phase0_smoke (C++20 Native) superado exitosamente.\n");
    return 0;
}
