#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pm::core {

struct AtomicWriteResult {
    bool ok = false;
    DWORD win32_error = ERROR_SUCCESS;
    std::wstring failed_stage;
    std::wstring temp_file_path;
    bool temp_cleaned_up = false;
};

struct AtomicReadResult {
    bool ok = false;
    DWORD win32_error = ERROR_SUCCESS;
    std::string content;
};

class AtomicFile {
public:
    // Escribe contenido binario de manera atómica usando CreateFileW (CREATE_NEW),
    // WriteFile, FlushFileBuffers y MoveFileExW (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
    static AtomicWriteResult WriteBytes(
        std::wstring_view target_path,
        const void* data,
        std::size_t size_bytes,
        bool simulate_interruption = false) noexcept;

    // Escribe texto UTF-8 (sin BOM) de manera atómica.
    static AtomicWriteResult WriteUtf8Text(
        std::wstring_view target_path,
        std::string_view utf8_text,
        bool simulate_interruption = false) noexcept;

    // Lee el contenido completo de un archivo como texto UTF-8 (eliminando BOM UTF-8 si existiera).
    static AtomicReadResult ReadUtf8Text(std::wstring_view target_path) noexcept;

    // Garantiza que exista la cadena de directorios padre de target_path.
    static bool EnsureParentDirectories(std::wstring_view target_path, DWORD& out_error) noexcept;
};

} // namespace pm::core
