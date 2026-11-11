#include "core/config_manager.h"

#include <windows.h>
#include <cwctype>

namespace pm::core {

std::string IniDocument::WideToUtf8(std::wstring_view wide_str) {
    if (wide_str.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide_str.data(), static_cast<int>(wide_str.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide_str.data(), static_cast<int>(wide_str.size()),
        utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::wstring IniDocument::Utf8ToWide(std::string_view utf8_str) {
    if (utf8_str.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8_str.data(), static_cast<int>(utf8_str.size()),
        nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8_str.data(), static_cast<int>(utf8_str.size()),
        wide.data(), needed);
    return wide;
}

bool IniDocument::EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towupper(a[i]) != std::towupper(b[i])) {
            return false;
        }
    }
    return true;
}

bool IniDocument::EndsWithIgnoreCase(std::wstring_view text, std::wstring_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    return EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

std::wstring IniDocument::Trim(std::wstring_view text) {
    std::size_t start = 0;
    while (start < text.size() && (text[start] == L' ' || text[start] == L'\t')) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
        --end;
    }
    return std::wstring(text.substr(start, end - start));
}

bool IniDocument::IsValidIdentifier(std::wstring_view token, bool allow_dots_and_dashes) noexcept {
    if (token.empty()) {
        return false;
    }
    for (wchar_t ch : token) {
        if (ch <= 0x1F || ch == L'=' || ch == L'[' || ch == L']' || ch == L'\r' || ch == L'\n') {
            return false;
        }
        if (!allow_dots_and_dashes && (ch == L'.' || ch == L'-')) {
            return false;
        }
    }
    return true;
}

bool IniDocument::IsValidValueText(std::wstring_view val) noexcept {
    for (wchar_t ch : val) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\0') {
            return false;
        }
    }
    return true;
}

bool IniDocument::IsPersistentPathKeyName(std::wstring_view key) noexcept {
    return EqualsIgnoreCase(key, L"Path") ||
           EqualsIgnoreCase(key, L"Executable") ||
           EqualsIgnoreCase(key, L"Launcher") ||
           EqualsIgnoreCase(key, L"Root") ||
           EqualsIgnoreCase(key, L"Icon") ||
           EqualsIgnoreCase(key, L"Dir") ||
           EndsWithIgnoreCase(key, L"Path") ||
           EndsWithIgnoreCase(key, L"Dir");
}

bool IniDocument::ParseUtf8(std::string_view utf8_content, std::wstring& out_error) {
    lines_.clear();
    dirty_ = false;
    out_error.clear();

    const std::wstring wide_text = Utf8ToWide(utf8_content);
    std::wstring current_section;

    std::size_t pos = 0;
    const std::size_t total = wide_text.size();

    while (pos <= total) {
        std::size_t line_end = wide_text.find(L'\n', pos);
        bool has_more = true;
        if (line_end == std::wstring::npos) {
            line_end = total;
            has_more = false;
        }

        std::wstring_view raw_line(wide_text.data() + pos, line_end - pos);
        if (!raw_line.empty() && raw_line.back() == L'\r') {
            raw_line.remove_suffix(1);
        }

        if (raw_line.empty() && !has_more && pos == total) {
            break;
        }

        const std::wstring trimmed = Trim(raw_line);
        EntryLine entry{};
        entry.raw_text = std::wstring(raw_line);

        if (trimmed.empty()) {
            entry.kind = LineKind::Blank;
        } else if (trimmed.front() == L';' || trimmed.front() == L'#') {
            entry.kind = LineKind::Comment;
        } else if (trimmed.front() == L'[' && trimmed.back() == L']' && trimmed.size() >= 3) {
            const std::wstring sec_name = Trim(std::wstring_view(trimmed).substr(1, trimmed.size() - 2));
            if (!IsValidIdentifier(sec_name, true)) {
                out_error = L"Nombre de seccion INI invalido: " + sec_name;
                return false;
            }
            current_section = sec_name;
            entry.kind = LineKind::Section;
            entry.section_name = current_section;
        } else {
            const std::size_t eq_pos = raw_line.find(L'=');
            if (eq_pos != std::wstring_view::npos) {
                const std::wstring k = Trim(raw_line.substr(0, eq_pos));
                const std::wstring v = Trim(raw_line.substr(eq_pos + 1));
                if (!IsValidIdentifier(k, true)) {
                    out_error = L"Clave INI invalida: " + k;
                    return false;
                }
                entry.kind = LineKind::KeyValue;
                entry.section_name = current_section;
                entry.key = k;
                entry.value = v;
            } else {
                // Línea desconocida sin '=': se preserva intacta sin eliminarla
                entry.kind = LineKind::UnknownRaw;
                entry.section_name = current_section;
            }
        }

        lines_.push_back(std::move(entry));

        if (!has_more) {
            break;
        }
        pos = line_end + 1;
    }

    return true;
}

std::string IniDocument::SerializeUtf8() const {
    std::wstring buffer;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const EntryLine& e = lines_[i];
        switch (e.kind) {
            case LineKind::Blank:
            case LineKind::Comment:
            case LineKind::UnknownRaw:
                buffer.append(e.raw_text);
                break;
            case LineKind::Section:
                buffer.push_back(L'[');
                buffer.append(e.section_name);
                buffer.push_back(L']');
                break;
            case LineKind::KeyValue:
                buffer.append(e.key);
                buffer.push_back(L'=');
                buffer.append(e.value);
                break;
        }
        buffer.append(L"\r\n");
    }
    return WideToUtf8(buffer);
}

bool IniDocument::HasSection(std::wstring_view section) const noexcept {
    for (const EntryLine& e : lines_) {
        if (e.kind == LineKind::Section && EqualsIgnoreCase(e.section_name, section)) {
            return true;
        }
    }
    return false;
}

bool IniDocument::HasKey(std::wstring_view section, std::wstring_view key) const noexcept {
    for (const EntryLine& e : lines_) {
        if (e.kind == LineKind::KeyValue &&
            EqualsIgnoreCase(e.section_name, section) &&
            EqualsIgnoreCase(e.key, key)) {
            return true;
        }
    }
    return false;
}

std::wstring IniDocument::GetValue(
    std::wstring_view section,
    std::wstring_view key,
    std::wstring_view default_value) const {
    for (const EntryLine& e : lines_) {
        if (e.kind == LineKind::KeyValue &&
            EqualsIgnoreCase(e.section_name, section) &&
            EqualsIgnoreCase(e.key, key)) {
            return e.value;
        }
    }
    return std::wstring(default_value);
}

bool IniDocument::SetValue(
    std::wstring_view section,
    std::wstring_view key,
    std::wstring_view value,
    std::wstring& out_error,
    bool enforce_relative_path) {

    const std::wstring clean_section = Trim(section);
    const std::wstring clean_key = Trim(key);
    std::wstring final_value(value);

    if (!IsValidIdentifier(clean_section, true)) {
        out_error = L"Nombre de seccion invalido.";
        return false;
    }
    if (!IsValidIdentifier(clean_key, true)) {
        out_error = L"Nombre de clave invalido.";
        return false;
    }
    if (!IsValidValueText(final_value)) {
        out_error = L"El valor contiene saltos de linea o caracteres de control no permitidos.";
        return false;
    }

    const bool must_be_relative_path = enforce_relative_path || IsPersistentPathKeyName(clean_key);
    if (must_be_relative_path) {
        if (!PathResolver::IsValidPersistentPmPath(final_value)) {
            out_error = L"Directiva de rutas violada: no se permiten letras de unidad, rutas absolutas, tokens de entorno ni '..' en una ruta persistente de PM.";
            return false;
        }
        const PathResolutionResult norm = PathResolver::NormalizePmRelativePath(final_value);
        if (!norm.ok) {
            out_error = norm.error_message;
            return false;
        }
        final_value = norm.resolved_path;
        for (wchar_t& ch : final_value) {
            if (ch == L'\\') {
                ch = L'/';
            }
        }
    }

    // Buscar si la clave ya existe en la sección
    int last_index_in_section = -1;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        EntryLine& e = lines_[i];
        if ((e.kind == LineKind::Section || e.kind == LineKind::KeyValue || e.kind == LineKind::UnknownRaw) &&
            EqualsIgnoreCase(e.section_name, clean_section)) {
            last_index_in_section = static_cast<int>(i);
            if (e.kind == LineKind::KeyValue && EqualsIgnoreCase(e.key, clean_key)) {
                if (e.value == final_value) {
                    // Sin cambio real: no marcar como dirty
                    return true;
                }
                e.value = final_value;
                e.raw_text = e.key + L"=" + e.value;
                dirty_ = true;
                return true;
            }
        }
    }

    // Si la sección existe pero la clave no, insertar justo al final de la sección
    if (last_index_in_section >= 0) {
        EntryLine kv{};
        kv.kind = LineKind::KeyValue;
        kv.section_name = clean_section;
        kv.key = clean_key;
        kv.value = final_value;
        kv.raw_text = clean_key + L"=" + final_value;
        lines_.insert(lines_.begin() + (last_index_in_section + 1), std::move(kv));
        dirty_ = true;
        return true;
    }

    // Si la sección tampoco existe, añadir sección al final (con línea en blanco previa si había contenido)
    if (!lines_.empty() && lines_.back().kind != LineKind::Blank) {
        EntryLine blank{};
        blank.kind = LineKind::Blank;
        lines_.push_back(std::move(blank));
    }

    EntryLine sec{};
    sec.kind = LineKind::Section;
    sec.section_name = clean_section;
    sec.raw_text = L"[" + clean_section + L"]";
    lines_.push_back(std::move(sec));

    EntryLine kv{};
    kv.kind = LineKind::KeyValue;
    kv.section_name = clean_section;
    kv.key = clean_key;
    kv.value = final_value;
    kv.raw_text = clean_key + L"=" + final_value;
    lines_.push_back(std::move(kv));

    dirty_ = true;
    return true;
}

bool IniDocument::SetRelativePathValue(
    std::wstring_view section,
    std::wstring_view key,
    std::wstring_view relative_path,
    std::wstring& out_error) {
    return SetValue(section, key, relative_path, out_error, true);
}

ConfigManager::ConfigManager(const PathResolver& resolver)
    : resolver_(resolver) {
    const auto cfg_res = resolver_.ResolvePmRelativePath(kDefaultConfigRelPath);
    if (cfg_res.ok) {
        config_ini_path_ = cfg_res.resolved_path;
    }
    const auto apps_res = resolver_.ResolvePmRelativePath(kDefaultAppsRelPath);
    if (apps_res.ok) {
        apps_ini_path_ = apps_res.resolved_path;
    }
}

ConfigOperationResult ConfigManager::LoadIniFileInternal(
    std::wstring_view full_path,
    IniDocument& out_doc,
    bool create_if_missing) {

    ConfigOperationResult res{};
    if (full_path.empty()) {
        res.win32_error = ERROR_INVALID_PARAMETER;
        res.error_message = L"Ruta de archivo INI vacia.";
        return res;
    }

    const std::wstring path_str(full_path);
    const DWORD attrs = ::GetFileAttributesW(path_str.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = ::GetLastError();
        if ((err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) && create_if_missing) {
            std::wstring parse_err;
            out_doc.ParseUtf8("", parse_err);
            res.ok = true;
            return res;
        }
        res.win32_error = err;
        res.error_message = L"No se pudo encontrar el archivo INI.";
        return res;
    }

    const AtomicReadResult read_res = AtomicFile::ReadUtf8Text(full_path);
    if (!read_res.ok) {
        res.win32_error = read_res.win32_error;
        res.error_message = L"Error al leer archivo INI desde disco.";
        return res;
    }

    std::wstring parse_err;
    if (!out_doc.ParseUtf8(read_res.content, parse_err)) {
        res.win32_error = ERROR_INVALID_DATA;
        res.error_message = parse_err;
        return res;
    }

    res.ok = true;
    return res;
}

ConfigOperationResult ConfigManager::SaveIniFileInternal(
    std::wstring_view full_path,
    IniDocument& doc,
    bool force_if_missing) {

    ConfigOperationResult res{};
    if (full_path.empty()) {
        res.win32_error = ERROR_INVALID_PARAMETER;
        res.error_message = L"Ruta de archivo INI vacia.";
        return res;
    }

    const std::wstring path_str(full_path);
    const bool file_exists = (::GetFileAttributesW(path_str.c_str()) != INVALID_FILE_ATTRIBUTES);

    if (!doc.is_dirty() && (file_exists || !force_if_missing)) {
        res.ok = true;
        res.wrote_to_disk = false;
        return res;
    }

    const std::string serialized = doc.SerializeUtf8();
    const AtomicWriteResult write_res = AtomicFile::WriteUtf8Text(full_path, serialized);
    if (!write_res.ok) {
        res.win32_error = write_res.win32_error;
        res.error_message = L"Fallo escritura atomica de INI en etapa: " + write_res.failed_stage;
        return res;
    }

    doc.ClearDirty();
    res.ok = true;
    res.wrote_to_disk = true;
    return res;
}

ConfigOperationResult ConfigManager::LoadConfigIni(bool create_if_missing) {
    return LoadIniFileInternal(config_ini_path_, config_doc_, create_if_missing);
}

ConfigOperationResult ConfigManager::LoadAppsIni(bool create_if_missing) {
    return LoadIniFileInternal(apps_ini_path_, apps_doc_, create_if_missing);
}

ConfigOperationResult ConfigManager::SaveConfigIniIfModified(bool force_if_missing) {
    return SaveIniFileInternal(config_ini_path_, config_doc_, force_if_missing);
}

ConfigOperationResult ConfigManager::SaveAppsIniIfModified(bool force_if_missing) {
    return SaveIniFileInternal(apps_ini_path_, apps_doc_, force_if_missing);
}

ConfigOperationResult ConfigManager::LoadRelativeIni(
    std::wstring_view relative_ini_path,
    IniDocument& out_doc,
    bool create_if_missing) const {

    const PathResolutionResult resolved = resolver_.ResolvePmRelativePath(relative_ini_path);
    if (!resolved.ok) {
        return {false, false, ERROR_INVALID_NAME, resolved.error_message};
    }
    return LoadIniFileInternal(resolved.resolved_path, out_doc, create_if_missing);
}

ConfigOperationResult ConfigManager::SaveRelativeIniIfModified(
    std::wstring_view relative_ini_path,
    IniDocument& doc,
    bool force_if_missing) const {

    const PathResolutionResult resolved = resolver_.ResolvePmRelativePath(relative_ini_path);
    if (!resolved.ok) {
        return {false, false, ERROR_INVALID_NAME, resolved.error_message};
    }
    return SaveIniFileInternal(resolved.resolved_path, doc, force_if_missing);
}

} // namespace pm::core
