#include "core/atomic_file.h"

#include <atomic>
#include <cwchar>

namespace pm::core {

namespace {

std::atomic<std::uint64_t> g_temp_sequence{1};

bool CreateDirectoriesRecursive(const std::wstring& dir_path, DWORD& out_error) noexcept {
    if (dir_path.empty()) {
        out_error = ERROR_INVALID_NAME;
        return false;
    }

    const DWORD attrs = ::GetFileAttributesW(dir_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            out_error = ERROR_SUCCESS;
            return true;
        }
        out_error = ERROR_ALREADY_EXISTS;
        return false;
    }

    // No intentar crear la raíz de unidad ("C:" o "C:\")
    if (dir_path.size() <= 3 && dir_path.find(L':') != std::wstring::npos) {
        out_error = ERROR_PATH_NOT_FOUND;
        return false;
    }

    const std::size_t sep = dir_path.find_last_of(L"\\/");
    if (sep != std::wstring::npos && sep > 0) {
        const std::wstring parent = dir_path.substr(0, sep);
        if (!parent.empty() && !(parent.size() == 2 && parent[1] == L':')) {
            if (!CreateDirectoriesRecursive(parent, out_error)) {
                return false;
            }
        }
    }

    if (::CreateDirectoryW(dir_path.c_str(), nullptr)) {
        out_error = ERROR_SUCCESS;
        return true;
    }

    const DWORD err = ::GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        const DWORD recheck = ::GetFileAttributesW(dir_path.c_str());
        if (recheck != INVALID_FILE_ATTRIBUTES && (recheck & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            out_error = ERROR_SUCCESS;
            return true;
        }
    }
    out_error = err;
    return false;
}

std::wstring BuildUniqueTempCandidate(std::wstring_view target_path, std::uint32_t attempt) {
    const DWORD pid = ::GetCurrentProcessId();
    const ULONGLONG ticks = ::GetTickCount64();
    const std::uint64_t seq = g_temp_sequence.fetch_add(1, std::memory_order_relaxed);

    wchar_t suffix[128]{};
    std::swprintf(suffix, sizeof(suffix) / sizeof(wchar_t),
                  L".pmtmp.%lu.%llu.%llu.%u",
                  pid, ticks, static_cast<unsigned long long>(seq), attempt);

    std::wstring temp_path(target_path);
    temp_path.append(suffix);
    return temp_path;
}

} // namespace

bool AtomicFile::EnsureParentDirectories(std::wstring_view target_path, DWORD& out_error) noexcept {
    const std::size_t last_sep = target_path.find_last_of(L"\\/");
    if (last_sep == std::wstring_view::npos) {
        out_error = ERROR_SUCCESS;
        return true;
    }
    const std::wstring parent_dir(target_path.substr(0, last_sep));
    if (parent_dir.empty() || (parent_dir.size() == 2 && parent_dir[1] == L':')) {
        out_error = ERROR_SUCCESS;
        return true;
    }
    return CreateDirectoriesRecursive(parent_dir, out_error);
}

AtomicWriteResult AtomicFile::WriteBytes(
    std::wstring_view target_path,
    const void* data,
    std::size_t size_bytes,
    bool simulate_interruption) noexcept {

    AtomicWriteResult result{};
    if (target_path.empty()) {
        result.win32_error = ERROR_INVALID_PARAMETER;
        result.failed_stage = L"ValidateTarget";
        return result;
    }

    DWORD dir_err = ERROR_SUCCESS;
    if (!EnsureParentDirectories(target_path, dir_err)) {
        result.win32_error = dir_err;
        result.failed_stage = L"EnsureParentDirectories";
        return result;
    }

    // 1. Crear archivo temporal único junto al destino usando CREATE_NEW
    // para no sobrescribir jamás archivos temporales preexistentes o ajenos.
    HANDLE hTemp = INVALID_HANDLE_VALUE;
    std::wstring temp_path;
    DWORD create_err = ERROR_SUCCESS;

    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        temp_path = BuildUniqueTempCandidate(target_path, attempt);
        hTemp = ::CreateFileW(
            temp_path.c_str(),
            GENERIC_WRITE,
            0, // acceso exclusivo durante la escritura temporal
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (hTemp != INVALID_HANDLE_VALUE) {
            break;
        }
        create_err = ::GetLastError();
        if (create_err != ERROR_FILE_EXISTS && create_err != ERROR_ALREADY_EXISTS) {
            break;
        }
    }

    result.temp_file_path = temp_path;

    if (hTemp == INVALID_HANDLE_VALUE) {
        result.win32_error = create_err;
        result.failed_stage = L"CreateTempFile";
        return result;
    }

    auto cleanup_temp = [&]() noexcept {
        if (hTemp != INVALID_HANDLE_VALUE) {
            ::CloseHandle(hTemp);
            hTemp = INVALID_HANDLE_VALUE;
        }
        if (!temp_path.empty()) {
            if (::DeleteFileW(temp_path.c_str())) {
                result.temp_cleaned_up = true;
            }
        }
    };

    // 2. Escribir todo el contenido comprobando errores y escrituras incompletas
    const auto* byte_ptr = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size_bytes;

    if (simulate_interruption) {
        // Escribir únicamente la mitad para simular fallo/interrupción a mitad de escritura
        const DWORD partial_len = static_cast<DWORD>(remaining / 2);
        if (partial_len > 0 && byte_ptr != nullptr) {
            DWORD written = 0;
            ::WriteFile(hTemp, byte_ptr, partial_len, &written, nullptr);
        }
        result.win32_error = ERROR_WRITE_FAULT;
        result.failed_stage = L"WriteFile_SimulatedInterruption";
        cleanup_temp();
        return result;
    }

    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x1000000U) ? 0x1000000U : static_cast<DWORD>(remaining);
        DWORD written = 0;
        const BOOL write_ok = ::WriteFile(hTemp, byte_ptr, chunk, &written, nullptr);
        if (!write_ok) {
            result.win32_error = ::GetLastError();
            result.failed_stage = L"WriteFile";
            cleanup_temp();
            return result;
        }
        if (written == 0 || written > chunk) {
            result.win32_error = ERROR_WRITE_FAULT;
            result.failed_stage = L"WriteFile_Incomplete";
            cleanup_temp();
            return result;
        }
        byte_ptr += written;
        remaining -= written;
    }

    // 3. Ejecutar FlushFileBuffers antes de cerrar el handle
    if (!::FlushFileBuffers(hTemp)) {
        result.win32_error = ::GetLastError();
        result.failed_stage = L"FlushFileBuffers";
        cleanup_temp();
        return result;
    }

    if (!::CloseHandle(hTemp)) {
        result.win32_error = ::GetLastError();
        hTemp = INVALID_HANDLE_VALUE;
        result.failed_stage = L"CloseHandle";
        cleanup_temp();
        return result;
    }
    hTemp = INVALID_HANDLE_VALUE;

    // 4. Reemplazar el destino mediante MoveFileExW con MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    const std::wstring target_str(target_path);
    if (!::MoveFileExW(
            temp_path.c_str(),
            target_str.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.win32_error = ::GetLastError();
        result.failed_stage = L"MoveFileExW";
        cleanup_temp();
        return result;
    }

    result.ok = true;
    result.win32_error = ERROR_SUCCESS;
    result.temp_cleaned_up = true; // movido exitosamente al destino
    return result;
}

AtomicWriteResult AtomicFile::WriteUtf8Text(
    std::wstring_view target_path,
    std::string_view utf8_text,
    bool simulate_interruption) noexcept {
    return WriteBytes(target_path, utf8_text.data(), utf8_text.size(), simulate_interruption);
}

AtomicReadResult AtomicFile::ReadUtf8Text(std::wstring_view target_path,
                                          bool require_plain_file) noexcept {
    AtomicReadResult res{};
    if (target_path.empty()) {
        res.win32_error = ERROR_INVALID_PARAMETER;
        return res;
    }

    const std::wstring path_str(target_path);
    HANDLE hFile = ::CreateFileW(
        path_str.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        require_plain_file ? FILE_FLAG_OPEN_REPARSE_POINT : FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        res.win32_error = ::GetLastError();
        return res;
    }

    if (require_plain_file) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(hFile, &info)) {
            res.win32_error = ::GetLastError();
        } else if (info.nNumberOfLinks != 1 ||
                   (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            res.win32_error = ERROR_INVALID_DATA;
        }
        if (res.win32_error != ERROR_SUCCESS) {
            ::CloseHandle(hFile);
            return res;
        }
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(hFile, &file_size)) {
        res.win32_error = ::GetLastError();
        ::CloseHandle(hFile);
        return res;
    }

    if (file_size.QuadPart < 0 || file_size.QuadPart > 64LL * 1024LL * 1024LL) {
        res.win32_error = ERROR_FILE_TOO_LARGE;
        ::CloseHandle(hFile);
        return res;
    }

    const std::size_t total_size = static_cast<std::size_t>(file_size.QuadPart);
    std::string buffer(total_size, '\0');
    std::size_t offset = 0;

    while (offset < total_size) {
        const DWORD to_read = static_cast<DWORD>(total_size - offset);
        DWORD bytes_read = 0;
        if (!::ReadFile(hFile, buffer.data() + offset, to_read, &bytes_read, nullptr)) {
            res.win32_error = ::GetLastError();
            ::CloseHandle(hFile);
            return res;
        }
        if (bytes_read == 0) {
            break;
        }
        offset += bytes_read;
    }
    ::CloseHandle(hFile);
    buffer.resize(offset);

    // Retirar BOM UTF-8 (EF BB BF) si está presente al inicio
    if (buffer.size() >= 3 &&
        static_cast<unsigned char>(buffer[0]) == 0xEF &&
        static_cast<unsigned char>(buffer[1]) == 0xBB &&
        static_cast<unsigned char>(buffer[2]) == 0xBF) {
        buffer.erase(0, 3);
    }

    res.ok = true;
    res.win32_error = ERROR_SUCCESS;
    res.content = std::move(buffer);
    return res;
}

} // namespace pm::core
