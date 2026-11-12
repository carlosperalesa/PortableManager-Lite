#include "runtime/manifest_manager.h"

#include "core/atomic_file.h"
#include "core/config_manager.h"
#include "core/path_resolver.h"

#include <cwchar>
#include <cwctype>

namespace pm::runtime {

namespace {

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
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

bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    return EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool EndsWithIgnoreCase(std::wstring_view text, std::wstring_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    return EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

bool ContainsIgnoreCase(std::wstring_view haystack, std::wstring_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    const std::size_t limit = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= limit; ++i) {
        if (EqualsIgnoreCase(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

std::wstring NormalizeSlashes(std::wstring_view p) {
    std::wstring out(p);
    for (wchar_t& ch : out) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

bool ContainsDotDotSegment(std::wstring_view path) noexcept {
    std::size_t i = 0;
    const std::size_t n = path.size();
    while (i < n) {
        while (i < n && (path[i] == L'\\' || path[i] == L'/')) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        std::size_t start = i;
        while (i < n && path[i] != L'\\' && path[i] != L'/') {
            ++i;
        }
        if (path.substr(start, i - start) == L"..") {
            return true;
        }
    }
    return false;
}

std::wstring QueryEnvNormalized(const wchar_t* var_name) {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = ::GetEnvironmentVariableW(var_name, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"";
    }
    return NormalizeSlashes(std::wstring_view(buf, len));
}

bool HasInvalidPathChars(std::wstring_view path) noexcept {
    for (wchar_t ch : path) {
        if (ch <= 0x1F || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' || ch == L'?' || ch == L'*') {
            return true;
        }
    }
    return false;
}

bool IsSafeHostFilesystemTarget(std::wstring_view path) {
    if (path.size() < 4 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
        return false;
    }
    if (ContainsDotDotSegment(path) || HasInvalidPathChars(path)) {
        return false;
    }

    const std::wstring norm = NormalizeSlashes(path);
    const std::wstring user_profile = QueryEnvNormalized(L"USERPROFILE");
    const std::wstring temp_dir = QueryEnvNormalized(L"TEMP");

    const bool inside_user = !user_profile.empty() &&
                             StartsWithIgnoreCase(norm, user_profile + L"\\") &&
                             norm.size() > user_profile.size() + 1;
    const bool inside_temp = !temp_dir.empty() &&
                             StartsWithIgnoreCase(norm, temp_dir + L"\\") &&
                             norm.size() > temp_dir.size() + 1;

    return inside_user || inside_temp;
}

bool IsSafeTempFilesystemPath(std::wstring_view path) {
    if (path.size() < 4 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
        return false;
    }
    if (ContainsDotDotSegment(path) || HasInvalidPathChars(path)) {
        return false;
    }

    const std::wstring norm = NormalizeSlashes(path);
    const std::wstring temp_dir = QueryEnvNormalized(L"TEMP");
    return !temp_dir.empty() &&
           StartsWithIgnoreCase(norm, temp_dir + L"\\") &&
           norm.size() > temp_dir.size() + 1;
}

bool DirectoryOrFileExists(const std::wstring& path) noexcept {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DeleteDirectoryTreeSafe(const std::wstring& dir_path, DWORD& out_err) noexcept {
    const DWORD attrs = ::GetFileAttributesW(dir_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            out_err = ERROR_SUCCESS;
            return true;
        }
        out_err = err;
        return false;
    }

    // Si es un reparse point (Junction NTFS), remover únicamente el punto de unión en el anfitrión
    // sin entrar recursivamente ni necesitar acceso al destino USB.
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (::RemoveDirectoryW(dir_path.c_str())) {
            out_err = ERROR_SUCCESS;
            return true;
        }
        out_err = ::GetLastError();
        return false;
    }

    const std::wstring pattern = dir_path + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring_view name(fd.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = dir_path + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!DeleteDirectoryTreeSafe(child, out_err)) {
                    ::FindClose(hFind);
                    return false;
                }
            } else {
                if (!::DeleteFileW(child.c_str())) {
                    out_err = ::GetLastError();
                    ::FindClose(hFind);
                    return false;
                }
            }
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
    }

    if (!::RemoveDirectoryW(dir_path.c_str())) {
        out_err = ::GetLastError();
        return false;
    }
    out_err = ERROR_SUCCESS;
    return true;
}

} // namespace

ManifestManager::ManifestManager(std::wstring_view session_id, std::wstring_view manifest_file_path)
    : session_id_(session_id), manifest_path_(manifest_file_path) {}

void ManifestManager::Configure(std::wstring_view session_id, std::wstring_view manifest_file_path) {
    session_id_.assign(session_id);
    manifest_path_.assign(manifest_file_path);
}

bool ManifestManager::AreAllOperationsTerminalAcceptable() const noexcept {
    for (const auto& op : operations_) {
        if (!IsTerminalAcceptableState(op.state)) {
            return false;
        }
    }
    return true;
}

std::wstring ManifestManager::SessionStateToString(SessionState st) {
    switch (st) {
        case SessionState::Preparing: return L"PREPARING";
        case SessionState::Active:    return L"ACTIVE";
        case SessionState::Cleaning:  return L"CLEANING";
        case SessionState::Cleaned:   return L"CLEANED";
    }
    return L"PREPARING";
}

bool ManifestManager::ParseSessionState(std::wstring_view text, SessionState& out_st) noexcept {
    if (text == L"PREPARING") { out_st = SessionState::Preparing; return true; }
    if (text == L"ACTIVE")    { out_st = SessionState::Active;    return true; }
    if (text == L"CLEANING")  { out_st = SessionState::Cleaning;  return true; }
    if (text == L"CLEANED")   { out_st = SessionState::Cleaned;   return true; }
    return false;
}

std::wstring ManifestManager::OperationTypeToString(OperationType t) {
    switch (t) {
        case OperationType::KillJob:        return L"KILL_JOB";
        case OperationType::RemoveJunction: return L"REMOVE_JUNCTION";
        case OperationType::RestoreBkp:     return L"RESTORE_BKP";
        case OperationType::RestoreReg:     return L"RESTORE_REG";
        case OperationType::DeleteDir:      return L"DELETE_DIR";
    }
    return L"DELETE_DIR";
}

bool ManifestManager::ParseOperationType(std::wstring_view text, OperationType& out_t) noexcept {
    if (text == L"KILL_JOB")        { out_t = OperationType::KillJob;        return true; }
    if (text == L"REMOVE_JUNCTION") { out_t = OperationType::RemoveJunction; return true; }
    if (text == L"RESTORE_BKP")     { out_t = OperationType::RestoreBkp;     return true; }
    if (text == L"RESTORE_REG")     { out_t = OperationType::RestoreReg;     return true; }
    if (text == L"DELETE_DIR")      { out_t = OperationType::DeleteDir;      return true; }
    return false;
}

std::wstring ManifestManager::OperationStateToString(OperationState s) {
    switch (s) {
        case OperationState::IntentRecorded: return L"INTENT_RECORDED";
        case OperationState::Applied:        return L"APPLIED";
        case OperationState::RolledBack:     return L"ROLLED_BACK";
        case OperationState::Failed:         return L"FAILED";
        case OperationState::Skipped:        return L"SKIPPED";
    }
    return L"INTENT_RECORDED";
}

bool ManifestManager::ParseOperationState(std::wstring_view text, OperationState& out_s) noexcept {
    if (text == L"INTENT_RECORDED") { out_s = OperationState::IntentRecorded; return true; }
    if (text == L"APPLIED")         { out_s = OperationState::Applied;        return true; }
    if (text == L"ROLLED_BACK")     { out_s = OperationState::RolledBack;     return true; }
    if (text == L"RESTORED")        { out_s = OperationState::RolledBack;     return true; } // Compatibilidad de lectura
    if (text == L"FAILED")          { out_s = OperationState::Failed;         return true; }
    if (text == L"SKIPPED")         { out_s = OperationState::Skipped;        return true; }
    return false;
}

bool ManifestManager::ValidateRegBackupFile(const std::wstring& reg_file_path, std::wstring& out_error) {
    if (!IsSafeTempFilesystemPath(reg_file_path) || !EndsWithIgnoreCase(reg_file_path, L".reg")) {
        out_error = L"BackupPath de registro debe ser un archivo .reg dentro de %TEMP%.";
        return false;
    }

    HANDLE hFile = ::CreateFileW(
        reg_file_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        out_error = L"El archivo de respaldo .reg no existe o no se puede abrir.";
        return false;
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(hFile, &file_size) || file_size.QuadPart < 40 || file_size.QuadPart > (16LL * 1024 * 1024)) {
        ::CloseHandle(hFile);
        out_error = L"El archivo de respaldo .reg esta vacio, incompleto o excede el tamano permitido.";
        return false;
    }

    std::string raw_bytes(static_cast<std::size_t>(file_size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL rd_ok = ::ReadFile(hFile, raw_bytes.data(), static_cast<DWORD>(raw_bytes.size()), &bytes_read, nullptr);
    ::CloseHandle(hFile);
    if (!rd_ok || bytes_read != raw_bytes.size()) {
        out_error = L"Error al leer el contenido completo del archivo de respaldo .reg.";
        return false;
    }

    std::wstring decoded;
    // Detectar UTF-16LE con BOM (formato nativo de reg.exe export)
    if (raw_bytes.size() >= 2 &&
        static_cast<unsigned char>(raw_bytes[0]) == 0xFF &&
        static_cast<unsigned char>(raw_bytes[1]) == 0xFE) {
        if ((raw_bytes.size() % 2) != 0) {
            out_error = L"Archivo .reg UTF-16LE truncado (numero impar de bytes).";
            return false;
        }
        const std::size_t wchar_count = (raw_bytes.size() - 2) / sizeof(wchar_t);
        decoded.resize(wchar_count);
        std::memcpy(decoded.data(), raw_bytes.data() + 2, wchar_count * sizeof(wchar_t));
    } else {
        decoded = pm::core::IniDocument::Utf8ToWide(raw_bytes);
    }

    // Eliminar posibles caracteres nulos finales y comprobar salto de línea final (exportación completa)
    while (!decoded.empty() && decoded.back() == L'\0') {
        decoded.pop_back();
    }
    if (decoded.empty() || (decoded.back() != L'\n' && decoded.back() != L'\r')) {
        out_error = L"El archivo de respaldo .reg esta truncado (no finaliza en salto de linea completo).";
        return false;
    }

    const bool has_header =
        StartsWithIgnoreCase(decoded, L"Windows Registry Editor Version 5.00") ||
        StartsWithIgnoreCase(decoded, L"REGEDIT4");
    if (!has_header) {
        out_error = L"El archivo .reg no contiene una cabecera valida (Windows Registry Editor Version 5.00).";
        return false;
    }

    const bool has_hkcu_section =
        ContainsIgnoreCase(decoded, L"[HKEY_CURRENT_USER\\Software\\") ||
        ContainsIgnoreCase(decoded, L"[HKCU\\Software\\");
    if (!has_hkcu_section) {
        out_error = L"El archivo .reg no contiene ninguna clave exportada bajo HKEY_CURRENT_USER\\Software\\.";
        return false;
    }

    return true;
}

bool ManifestManager::ValidateOperationSecurity(const ManifestOperation& op, std::wstring& out_error) {
    if (op.id.empty()) {
        out_error = L"Identificador de operacion vacio.";
        return false;
    }
    for (wchar_t ch : op.id) {
        if (!(std::iswalnum(ch) || ch == L'_' || ch == L'-')) {
            out_error = L"Identificador de operacion contiene caracteres no permitidos.";
            return false;
        }
    }

    switch (op.type) {
        case OperationType::KillJob: {
            if (op.job_name.empty() || ContainsDotDotSegment(op.job_name) || HasInvalidPathChars(op.job_name)) {
                out_error = L"KILL_JOB requiere un JobName valido.";
                return false;
            }
            return true;
        }
        case OperationType::RemoveJunction:
        case OperationType::DeleteDir: {
            if (!IsSafeHostFilesystemTarget(op.target_host_path)) {
                out_error = L"TargetPath fuera de directorios de usuario permitidos o inseguro.";
                return false;
            }
            return true;
        }
        case OperationType::RestoreBkp: {
            if (!IsSafeHostFilesystemTarget(op.target_host_path)) {
                out_error = L"TargetPath invalido o inseguro en RESTORE_BKP.";
                return false;
            }
            const std::wstring norm_target = NormalizeSlashes(op.target_host_path);
            if (op.original_existed) {
                if (!IsSafeHostFilesystemTarget(op.backup_host_path)) {
                    out_error = L"BackupPath invalido en RESTORE_BKP.";
                    return false;
                }
                const std::wstring norm_bkp = NormalizeSlashes(op.backup_host_path);
                if (!EqualsIgnoreCase(norm_bkp, norm_target + L".pmbkp")) {
                    out_error = L"BackupPath en RESTORE_BKP debe ser exactamente <TargetPath>.pmbkp.";
                    return false;
                }
            }
            return true;
        }
        case OperationType::RestoreReg: {
            const std::wstring& key_path = !op.reg_key_path.empty() ? op.reg_key_path : op.target_host_path;
            if (key_path.empty() || ContainsDotDotSegment(key_path) || HasInvalidPathChars(key_path)) {
                out_error = L"RegKeyPath vacia o con caracteres/segmentos '..' invalidos.";
                return false;
            }
            if (!StartsWithIgnoreCase(key_path, L"HKCU\\Software\\") || key_path.size() <= 14) {
                out_error = L"RESTORE_REG solo permite claves bajo HKCU\\Software\\<SubClave>.";
                return false;
            }
            if (op.original_existed) {
                if (!IsSafeTempFilesystemPath(op.backup_host_path) ||
                    !EndsWithIgnoreCase(op.backup_host_path, L".reg")) {
                    out_error = L"BackupPath de RESTORE_REG debe ser un archivo .reg dentro de %TEMP%.";
                    return false;
                }
            }
            return true;
        }
    }

    out_error = L"Tipo de operacion desconocido.";
    return false;
}

const ManifestOperation* ManifestManager::FindOperation(std::wstring_view op_id) const noexcept {
    for (const auto& op : operations_) {
        if (op.id == op_id) {
            return &op;
        }
    }
    return nullptr;
}

ManifestStatusResult ManifestManager::InitializeNew() {
    if (session_id_.empty() || manifest_path_.empty()) {
        return {false, false, ERROR_INVALID_PARAMETER, L"SessionID o ruta de manifiesto vacios."};
    }
    session_state_ = SessionState::Preparing;
    operations_.clear();
    next_op_seq_ = 1;
    return SaveToDisk();
}

std::wstring ManifestManager::SessionHeader(std::wstring_view session_id) {
    std::wstring text;
    text.append(L"; PortableManager Lite — Transient Host Restoration Manifest\r\n");
    text.append(L"[Session]\r\n");
    text.append(L"FormatSignature=").append(kFormatSignature).append(L"\r\n");
    text.append(L"SessionId=").append(session_id).append(L"\r\n");
    return text;
}

ManifestStatusResult ManifestManager::SaveToDisk() const {
    if (session_id_.empty() || manifest_path_.empty()) {
        return {false, false, ERROR_INVALID_PARAMETER, L"Manifiesto no configurado."};
    }

    auto text = SessionHeader(session_id_);
    text.reserve(1024 + operations_.size() * 256);
    text.append(L"State=").append(SessionStateToString(session_state_)).append(L"\r\n");
    text.append(L"OperationCount=").append(std::to_wstring(operations_.size())).append(L"\r\n");

    for (std::size_t i = 0; i < operations_.size(); ++i) {
        const ManifestOperation& op = operations_[i];
        text.append(L"\r\n[Op.").append(op.id).append(L"]\r\n");
        text.append(L"Index=").append(std::to_wstring(i)).append(L"\r\n");
        text.append(L"OpId=").append(op.id).append(L"\r\n");
        text.append(L"OpType=").append(OperationTypeToString(op.type)).append(L"\r\n");
        text.append(L"OpState=").append(OperationStateToString(op.state)).append(L"\r\n");
        text.append(L"OriginalExisted=").append(op.original_existed ? L"1" : L"0").append(L"\r\n");
        text.append(L"AppId=").append(op.app_id).append(L"\r\n");
        text.append(L"TargetPath=").append(op.target_host_path).append(L"\r\n");
        if (!op.junction_target.empty()) {
            text.append(L"JunctionTarget=").append(op.junction_target).append(L"\r\n");
            text.append(L"JunctionFileId=").append(op.junction_file_id).append(L"\r\n");
            text.append(L"OriginalFileId=").append(op.original_file_id).append(L"\r\n");
            text.append(L"HostParentFileId=").append(op.host_parent_file_id).append(L"\r\n");
        }
        text.append(L"BackupPath=").append(op.backup_host_path).append(L"\r\n");
        text.append(L"RegKeyPath=").append(op.reg_key_path).append(L"\r\n");
        if (!op.registry_backup_sha256.empty())
            text.append(L"RegistryBackupSHA256=").append(op.registry_backup_sha256).append(L"\r\n");
        text.append(L"JobName=").append(op.job_name).append(L"\r\n");
        text.append(L"LastError=").append(std::to_wstring(op.last_error)).append(L"\r\n");
    }

    text.append(L"\r\n[Footer]\r\n");
    text.append(L"EndMarker=EOF_OK\r\n");

    const std::string utf8 = pm::core::IniDocument::WideToUtf8(text);
    const pm::core::AtomicWriteResult wr = pm::core::AtomicFile::WriteUtf8Text(manifest_path_, utf8, false);
    if (!wr.ok) {
        return {false, false, wr.win32_error, L"Error escribiendo manifiesto en etapa: " + wr.failed_stage};
    }
    return {true, false, ERROR_SUCCESS, L""};
}

ManifestStatusResult ManifestManager::LoadFromDisk() {
    if (manifest_path_.empty()) {
        return {false, false, ERROR_INVALID_PARAMETER, L"Ruta de manifiesto vacia."};
    }

    const pm::core::AtomicReadResult rd = pm::core::AtomicFile::ReadUtf8Text(manifest_path_);
    if (!rd.ok) {
        return {false, false, rd.win32_error, L"No se pudo leer session.ini desde disco."};
    }

    pm::core::IniDocument doc;
    std::wstring parse_err;
    if (!doc.ParseUtf8(rd.content, parse_err)) {
        return {false, true, ERROR_INVALID_DATA, L"Sintaxis INI corrupta en manifiesto: " + parse_err};
    }

    for (const auto& line : doc.lines()) {
        if (line.kind == pm::core::IniDocument::LineKind::UnknownRaw) {
            return {false, true, ERROR_INVALID_DATA, L"Linea corrupta o desconocida en manifiesto."};
        }
    }

    const std::wstring sig = doc.GetValue(L"Session", L"FormatSignature");
    if (sig != kFormatSignature) {
        return {false, true, ERROR_INVALID_DATA, L"Firma FormatSignature ausente o invalida en manifiesto."};
    }

    const std::wstring end_marker = doc.GetValue(L"Footer", L"EndMarker");
    if (end_marker != L"EOF_OK") {
        return {false, true, ERROR_INVALID_DATA, L"Manifiesto incompleto o truncado (falta EndMarker=EOF_OK)."};
    }

    const std::wstring file_session_id = doc.GetValue(L"Session", L"SessionId");
    if (file_session_id.empty() || (!session_id_.empty() && file_session_id != session_id_)) {
        return {false, true, ERROR_INVALID_DATA, L"SessionId del manifiesto vacio o no coincide con la sesion esperada."};
    }

    SessionState parsed_state = SessionState::Preparing;
    if (!ParseSessionState(doc.GetValue(L"Session", L"State"), parsed_state)) {
        return {false, true, ERROR_INVALID_DATA, L"Estado de sesion invalido en manifiesto."};
    }

    const std::wstring count_str = doc.GetValue(L"Session", L"OperationCount");
    if (count_str.empty()) {
        return {false, true, ERROR_INVALID_DATA, L"OperationCount ausente en manifiesto."};
    }
    wchar_t* end_ptr = nullptr;
    const unsigned long expected_count = std::wcstoul(count_str.c_str(), &end_ptr, 10);
    if (end_ptr == count_str.c_str() || *end_ptr != L'\0') {
        return {false, true, ERROR_INVALID_DATA, L"OperationCount no numerico en manifiesto."};
    }

    std::vector<std::wstring> op_sections;
    for (const auto& line : doc.lines()) {
        if (line.kind == pm::core::IniDocument::LineKind::Section) {
            if (line.section_name == L"Session" || line.section_name == L"Footer") {
                continue;
            }
            if (line.section_name.rfind(L"Op.", 0) == 0 && line.section_name.size() > 3) {
                op_sections.push_back(line.section_name);
            } else {
                return {false, true, ERROR_INVALID_DATA, L"Seccion desconocida no permitida en manifiesto: " + line.section_name};
            }
        }
    }

    if (op_sections.size() != expected_count) {
        return {false, true, ERROR_INVALID_DATA, L"El numero de secciones [Op.*] no coincide con OperationCount."};
    }

    std::vector<ManifestOperation> loaded_ops;
    loaded_ops.reserve(op_sections.size());

    for (const std::wstring& sec : op_sections) {
        ManifestOperation op{};
        op.id = doc.GetValue(sec, L"OpId");
        const std::wstring sec_id = sec.substr(3);
        if (op.id.empty() || op.id != sec_id) {
            return {false, true, ERROR_INVALID_DATA, L"OpId inconsistente con la cabecera de seccion."};
        }

        if (!ParseOperationType(doc.GetValue(sec, L"OpType"), op.type)) {
            return {false, true, ERROR_INVALID_DATA, L"Tipo de operacion desconocido o prohibido en manifiesto."};
        }
        if (!ParseOperationState(doc.GetValue(sec, L"OpState"), op.state)) {
            return {false, true, ERROR_INVALID_DATA, L"Estado de operacion invalido en manifiesto."};
        }

        const std::wstring orig_str = doc.GetValue(sec, L"OriginalExisted");
        if (orig_str != L"0" && orig_str != L"1") {
            return {false, true, ERROR_INVALID_DATA, L"OriginalExisted invalido en manifiesto."};
        }
        op.original_existed = (orig_str == L"1");
        op.app_id = doc.GetValue(sec, L"AppId");

        op.target_host_path = doc.GetValue(sec, L"TargetPath");
        op.junction_target = doc.GetValue(sec, L"JunctionTarget");
        op.junction_file_id = doc.GetValue(sec, L"JunctionFileId");
        op.original_file_id = doc.GetValue(sec, L"OriginalFileId");
        op.host_parent_file_id = doc.GetValue(sec, L"HostParentFileId");
        if (op.target_host_path.empty()) {
            op.target_host_path = doc.GetValue(sec, L"TargetHostPath");
        }

        op.backup_host_path = doc.GetValue(sec, L"BackupPath");
        if (op.backup_host_path.empty()) {
            op.backup_host_path = doc.GetValue(sec, L"BackupHostPath");
        }

        op.reg_key_path = doc.GetValue(sec, L"RegKeyPath");
        op.registry_backup_sha256 = doc.GetValue(sec, L"RegistryBackupSHA256");
        if (op.type == OperationType::RestoreReg && op.reg_key_path.empty()) {
            op.reg_key_path = op.target_host_path;
        }

        op.job_name = doc.GetValue(sec, L"JobName");

        const std::wstring err_str = doc.GetValue(sec, L"LastError", L"0");
        op.last_error = static_cast<DWORD>(std::wcstoul(err_str.c_str(), nullptr, 10));

        std::wstring sec_err;
        if (!ValidateOperationSecurity(op, sec_err)) {
            return {false, true, ERROR_INVALID_DATA, L"Validacion de seguridad del manifiesto rechazada: " + sec_err};
        }

        for (const auto& existing : loaded_ops) {
            if (existing.id == op.id) {
                return {false, true, ERROR_INVALID_DATA, L"OpId duplicado en manifiesto: " + op.id};
            }
        }
        loaded_ops.push_back(std::move(op));
    }

    session_id_ = file_session_id;
    session_state_ = parsed_state;
    operations_ = std::move(loaded_ops);
    next_op_seq_ = operations_.size() + 1;

    return {true, false, ERROR_SUCCESS, L""};
}

ManifestStatusResult ManifestManager::SetSessionStateAndPersist(SessionState new_state) {
    if (new_state == SessionState::Cleaned && !AreAllOperationsTerminalAcceptable()) {
        return {false, false, ERROR_DS_BUSY,
                L"No se puede cambiar la sesion a CLEANED mientras existan operaciones en INTENT_RECORDED, APPLIED o FAILED."};
    }
    session_state_ = new_state;
    return SaveToDisk();
}

ManifestStatusResult ManifestManager::RecordOperationIntent(
    ManifestOperation op,
    std::wstring& out_assigned_id) {

    // Normalizar RegKeyPath si se suministró en target_host_path
    if (op.type == OperationType::RestoreReg && op.reg_key_path.empty() && !op.target_host_path.empty()) {
        op.reg_key_path = op.target_host_path;
    }

    // Directiva obligatoria Sección 1 (Orden de limpieza — KILL_JOB):
    // KILL_JOB debe registrarse como la última operación de preparación inmediatamente antes de iniciar
    // la aplicación, para que en el rollback LIFO sea la primera operación efectiva.
    for (const auto& existing : operations_) {
        if (existing.type == OperationType::KillJob && op.type != OperationType::KillJob) {
            return {false, false, ERROR_INVALID_PARAMETER,
                    L"Directiva KILL_JOB: KILL_JOB debe ser la ultima operacion de preparacion; no se pueden registrar operaciones adicionales despues de KILL_JOB."};
        }
    }

    if (op.id.empty()) {
        wchar_t buf[32]{};
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"op_%04zu", next_op_seq_++);
        op.id = buf;
    } else {
        ++next_op_seq_;
    }

    for (const auto& existing : operations_) {
        if (existing.id == op.id) {
            return {false, false, ERROR_ALREADY_EXISTS, L"El identificador de operacion ya existe en el manifiesto."};
        }
    }

    std::wstring sec_err;
    if (!ValidateOperationSecurity(op, sec_err)) {
        return {false, false, ERROR_INVALID_PARAMETER, sec_err};
    }

    // Regla 3.1: No sobrescribir un respaldo .pmbkp existente al registrar una operación RESTORE_BKP
    if (op.type == OperationType::RestoreBkp && op.original_existed && !op.backup_host_path.empty()) {
        if (DirectoryOrFileExists(op.backup_host_path)) {
            return {false, false, ERROR_ALREADY_EXISTS,
                    L"Ya existe un respaldo .pmbkp previo en el anfitrion; se rechaza sobrescribirlo."};
        }
    }

    // Directiva Sección 2 (Respaldo del registro — archivos .reg):
    // Cuando la clave original existía, el archivo .reg debe haberse exportado al directorio de sesión en %TEMP%
    // y verificarse completo y válido ANTES de aceptar el registro de la operación y modificar el registro.
    if (op.type == OperationType::RestoreReg && op.original_existed) {
        std::wstring reg_err;
        if (!ValidateRegBackupFile(op.backup_host_path, reg_err)) {
            return {false, false, ERROR_INVALID_DATA,
                    L"Respaldo .reg invalido o incompleto en RESTORE_REG: " + reg_err};
        }
    }

    op.state = OperationState::IntentRecorded;
    op.last_error = ERROR_SUCCESS;
    out_assigned_id = op.id;

    operations_.push_back(std::move(op));
    return SaveToDisk();
}

ManifestStatusResult ManifestManager::RecordJunctionFileId(
    std::wstring_view op_id, std::wstring_view file_id) {
    for (auto& op : operations_) {
        if (op.id != op_id) continue;
        if (op.type != OperationType::RemoveJunction || op.junction_target.empty() ||
            op.state != OperationState::IntentRecorded || !op.junction_file_id.empty() ||
            file_id.empty()) {
            return {false, false, ERROR_INVALID_PARAMETER, L"Intencion de Junction invalida."};
        }
        op.junction_file_id = file_id;
        return SaveToDisk();
    }
    return {false, false, ERROR_NOT_FOUND, L"No se encontro la intencion de Junction."};
}

ManifestStatusResult ManifestManager::UpdateOperationState(
    std::wstring_view op_id,
    OperationState new_state,
    DWORD last_error) {

    for (auto& op : operations_) {
        if (op.id == op_id) {
            op.state = new_state;
            op.last_error = last_error;
            return SaveToDisk();
        }
    }
    return {false, false, ERROR_NOT_FOUND, L"No se encontro la operacion en el manifiesto."};
}

bool ManifestManager::DefaultRestoreBkpDir(
    const ManifestOperation& op,
    bool& out_was_skipped,
    DWORD& out_err) noexcept {

    out_was_skipped = false;
    out_err = ERROR_SUCCESS;
    const std::wstring& target = op.target_host_path;
    const std::wstring& backup = op.backup_host_path;

    const bool target_exists = DirectoryOrFileExists(target);
    const bool backup_exists = !backup.empty() && DirectoryOrFileExists(backup);

    if (op.original_existed) {
        if (backup_exists) {
            // Si hay una carpeta o reparse point activo en target, retirarlo primero sin tocar el respaldo
            if (target_exists) {
                if (!DeleteDirectoryTreeSafe(target, out_err)) {
                    // Conservar el respaldo .pmbkp intacto si no se puede limpiar target
                    return false;
                }
            }
            // Restaurar el respaldo .pmbkp hacia target
            if (!::MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                out_err = ::GetLastError();
                return false;
            }
            if (!DirectoryOrFileExists(target) || DirectoryOrFileExists(backup)) {
                out_err = ERROR_INTERNAL_ERROR;
                return false;
            }
            return true;
        } else {
            // El respaldo .pmbkp no existe en disco:
            // - Si la operación estaba en INTENT_RECORDED y target sigue existiendo, la caída ocurrió antes de renombrar -> SKIPPED.
            // - Si ya estaba en ROLLED_BACK o SKIPPED y target existe, es una repetición idempotente.
            if (target_exists) {
                out_was_skipped = (op.state == OperationState::IntentRecorded || op.state == OperationState::Skipped);
                return true;
            }
            out_err = ERROR_FILE_NOT_FOUND;
            return false;
        }
    } else {
        // Originalmente NO existía: después de la restauración debe quedar ausente
        if (!target_exists) {
            out_was_skipped = (op.state == OperationState::IntentRecorded || op.state == OperationState::Skipped);
            return true;
        }
        return DeleteDirectoryTreeSafe(target, out_err);
    }
}

bool ManifestManager::DefaultDeleteCreatedDir(
    const ManifestOperation& op,
    bool& out_was_skipped,
    DWORD& out_err) noexcept {

    out_was_skipped = false;
    out_err = ERROR_SUCCESS;
    if (op.original_existed) {
        out_was_skipped = true;
        return true;
    }
    if (!DirectoryOrFileExists(op.target_host_path)) {
        out_was_skipped = (op.state == OperationState::IntentRecorded || op.state == OperationState::Skipped);
        return true;
    }
    return DeleteDirectoryTreeSafe(op.target_host_path, out_err);
}

RollbackSummary ManifestManager::ExecutePendingRollbacks(const RollbackHandlers& handlers) {
    RollbackSummary summary{};
    session_state_ = SessionState::Cleaning;
    if (const auto saved = SaveToDisk(); !saved.ok) {
        summary.persistence_error = saved.win32_error;
        summary.failed_count = 1;
        return summary;
    }

    // Construir el orden de índices en LIFO garantizando que cualquier operación KILL_JOB
    // se ejecute siempre en primer lugar antes de restaurar carpetas, Junctions o Registro.
    std::vector<std::size_t> lifo_indices;
    lifo_indices.reserve(operations_.size());

    for (std::size_t idx = operations_.size(); idx > 0; --idx) {
        if (operations_[idx - 1].type == OperationType::KillJob) {
            lifo_indices.push_back(idx - 1);
        }
    }
    for (std::size_t idx = operations_.size(); idx > 0; --idx) {
        if (operations_[idx - 1].type != OperationType::KillJob) {
            lifo_indices.push_back(idx - 1);
        }
    }

    for (std::size_t op_index : lifo_indices) {
        ManifestOperation& op = operations_[op_index];
        ++summary.total_considered;
        summary.execution_order_ids.push_back(op.id);
        summary.execution_order_types.push_back(op.type);

        bool step_ok = false;
        bool was_skipped = false;
        DWORD step_err = ERROR_SUCCESS;

        switch (op.type) {
            case OperationType::KillJob: {
                if (handlers.on_kill_job) {
                    step_ok = handlers.on_kill_job(op, step_err);
                } else {
                    step_ok = true;
                }
                break;
            }
            case OperationType::RemoveJunction: {
                if (IsTerminalAcceptableState(op.state)) {
                    step_ok = true;
                    was_skipped = op.state == OperationState::Skipped;
                } else if (handlers.on_remove_junction) {
                    step_ok = handlers.on_remove_junction(op, step_err);
                } else {
                    step_err = ERROR_NOT_SUPPORTED;
                }
                break;
            }
            case OperationType::RestoreBkp: {
                if (handlers.on_restore_bkp) {
                    step_ok = handlers.on_restore_bkp(op, step_err);
                } else {
                    step_ok = DefaultRestoreBkpDir(op, was_skipped, step_err);
                }
                break;
            }
            case OperationType::RestoreReg: {
                if (op.state == OperationState::RolledBack || op.state == OperationState::Skipped) {
                    // Ya fue restaurada o saltada previamente (idempotencia)
                    step_ok = true;
                    was_skipped = (op.state == OperationState::Skipped);
                } else {
                    // Si existía originalmente, comprobar que el archivo .reg sigue íntegro antes de restaurar
                    bool reg_bkp_valid = true;
                    if (op.original_existed) {
                        std::wstring reg_err;
                        if (!ValidateRegBackupFile(op.backup_host_path, reg_err)) {
                            reg_bkp_valid = false;
                            step_err = ERROR_INVALID_DATA;
                        }
                    }

                    if (!reg_bkp_valid) {
                        step_ok = false;
                    } else if (handlers.on_restore_reg) {
                        step_ok = handlers.on_restore_reg(op, step_err);
                    } else {
                        step_err = ERROR_NOT_SUPPORTED;
                    }
                }
                break;
            }
            case OperationType::DeleteDir: {
                if (handlers.on_delete_dir) {
                    step_ok = handlers.on_delete_dir(op, step_err);
                } else {
                    step_ok = DefaultDeleteCreatedDir(op, was_skipped, step_err);
                }
                break;
            }
        }

        if (step_ok) {
            if (was_skipped) {
                op.state = OperationState::Skipped;
                ++summary.skipped_count;
            } else {
                op.state = OperationState::RolledBack;
                ++summary.rolled_back_count;
            }
            op.last_error = ERROR_SUCCESS;
        } else {
            op.state = OperationState::Failed;
            op.last_error = (step_err != ERROR_SUCCESS) ? step_err : ERROR_GEN_FAILURE;
            ++summary.failed_count;
            summary.failed_op_ids.push_back(op.id);
        }

        if (const auto saved = SaveToDisk(); !saved.ok) {
            summary.persistence_error = saved.win32_error;
            ++summary.failed_count;
            summary.failed_op_ids.push_back(op.id);
            return summary;
        }
        // A crash before saving ROLLED_BACK must leave the original available
        // for a retry. A crash afterwards only needs to release the backup.
        if (step_ok && !was_skipped && op.type == OperationType::RestoreReg && op.original_existed) {
            DWORD release_error = ERROR_SUCCESS;
            const bool released = handlers.on_release_reg_backup
                ? handlers.on_release_reg_backup(op, release_error)
                : (::DeleteFileW(op.backup_host_path.c_str()) ||
                   (release_error = ::GetLastError()) == ERROR_FILE_NOT_FOUND);
            if (!released) {
                ++summary.failed_count;
                summary.failed_op_ids.push_back(op.id);
            }
        }
        // Never restore resources while a controlled process might still use them.
        if (!step_ok && op.type == OperationType::KillJob) return summary;
    }

    summary.all_succeeded = (summary.failed_count == 0) && AreAllOperationsTerminalAcceptable();
    if (summary.all_succeeded) {
        session_state_ = SessionState::Cleaned;
        if (const auto saved = SaveToDisk(); !saved.ok) {
            summary.persistence_error = saved.win32_error;
            session_state_ = SessionState::Cleaning;
            summary.all_succeeded = false;
            ++summary.failed_count;
        }
    }
    return summary;
}

} // namespace pm::runtime
