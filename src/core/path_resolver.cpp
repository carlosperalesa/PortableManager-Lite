#include "core/path_resolver.h"

#include <windows.h>
#include <cwctype>
#include <vector>

namespace pm::core {

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

std::wstring QueryEnvVar(const wchar_t* name) {
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return L"";
    }
    std::wstring value(needed, L'\0');
    DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return L"";
    }
    value.resize(written);
    return value;
}

} // namespace

PathResolver::PathResolver()
    : root_dir_(TrimTrailingSeparators(QueryExecutableDirectory())) {}

PathResolver::PathResolver(std::wstring_view explicit_root_dir)
    : root_dir_(TrimTrailingSeparators(explicit_root_dir)) {}

std::wstring PathResolver::QueryExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD copied = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return L"";
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return buffer;
        }
        if (buffer.size() >= 32768) {
            return L"";
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring PathResolver::QueryExecutableDirectory() {
    const std::wstring exe_path = QueryExecutablePath();
    if (exe_path.empty()) {
        return L"";
    }
    const std::size_t last_sep = exe_path.find_last_of(L"\\/");
    if (last_sep == std::wstring::npos) {
        return L"";
    }
    return exe_path.substr(0, last_sep);
}

std::wstring PathResolver::TrimTrailingSeparators(std::wstring_view path) {
    std::wstring result(path);
    for (wchar_t& ch : result) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    return result;
}

bool PathResolver::IsInvalidPathChar(wchar_t ch) noexcept {
    if (ch <= 0x1F) {
        return true;
    }
    switch (ch) {
        case L'<':
        case L'>':
        case L':':
        case L'"':
        case L'|':
        case L'?':
        case L'*':
            return true;
        default:
            return false;
    }
}

bool PathResolver::IsReservedDosDeviceName(std::wstring_view segment) noexcept {
    const std::size_t dot_pos = segment.find(L'.');
    const std::wstring_view stem = (dot_pos == std::wstring_view::npos) ? segment : segment.substr(0, dot_pos);
    if (stem.empty()) {
        return false;
    }
    if (EqualsIgnoreCase(stem, L"CON") ||
        EqualsIgnoreCase(stem, L"PRN") ||
        EqualsIgnoreCase(stem, L"AUX") ||
        EqualsIgnoreCase(stem, L"NUL")) {
        return true;
    }
    if (stem.size() == 4 &&
        (StartsWithIgnoreCase(stem, L"COM") || StartsWithIgnoreCase(stem, L"LPT")) &&
        stem[3] >= L'1' && stem[3] <= L'9') {
        return true;
    }
    return false;
}

bool PathResolver::IsAbsoluteOrRootedPath(std::wstring_view path_input) noexcept {
    if (path_input.empty()) {
        return false;
    }
    if (path_input.front() == L'\\' || path_input.front() == L'/') {
        return true;
    }
    if (path_input.size() >= 2 && path_input[1] == L':') {
        return true;
    }
    if (path_input.find(L':') != std::wstring_view::npos) {
        return true;
    }
    return false;
}

bool PathResolver::ContainsEnvironmentToken(std::wstring_view path_input) noexcept {
    const std::size_t first_pct = path_input.find(L'%');
    if (first_pct == std::wstring_view::npos) {
        return false;
    }
    const std::size_t second_pct = path_input.find(L'%', first_pct + 1);
    return second_pct != std::wstring_view::npos;
}

PathResolutionResult PathResolver::NormalizePmRelativePath(std::wstring_view relative_input) {
    if (relative_input.empty()) {
        return {false, L"", L"La ruta relativa no puede estar vacia."};
    }
    if (IsAbsoluteOrRootedPath(relative_input)) {
        return {false, L"", L"Se rechazo una ruta absoluta o con letra de unidad donde se requeria ruta relativa."};
    }
    if (ContainsEnvironmentToken(relative_input)) {
        return {false, L"", L"Las rutas internas de PM no pueden contener tokens de entorno (%...%)."};
    }

    std::vector<std::wstring> stack;
    std::size_t i = 0;
    const std::size_t n = relative_input.size();

    while (i < n) {
        while (i < n && (relative_input[i] == L'\\' || relative_input[i] == L'/')) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        std::size_t start = i;
        while (i < n && relative_input[i] != L'\\' && relative_input[i] != L'/') {
            ++i;
        }
        std::wstring_view segment = relative_input.substr(start, i - start);
        if (segment == L".") {
            continue;
        }
        if (segment == L"..") {
            if (stack.empty()) {
                return {false, L"", L"Intento de escapar de la raiz de PM mediante '..' rechazado."};
            }
            stack.pop_back();
            continue;
        }

        // Rechazar puntos o espacios finales y secuencias de solo puntos ('...')
        if (segment.back() == L'.' || segment.back() == L' ') {
            return {false, L"", L"Segmento de ruta invalido en Windows (termina en punto o espacio)."};
        }
        for (wchar_t ch : segment) {
            if (IsInvalidPathChar(ch) || ch == L'%') {
                return {false, L"", L"Caracter ilegal detectado en segmento de ruta relativa."};
            }
        }
        if (IsReservedDosDeviceName(segment)) {
            return {false, L"", L"Nombre de dispositivo DOS reservado rechazado."};
        }

        stack.emplace_back(segment);
    }

    if (stack.empty()) {
        return {false, L"", L"La ruta relativa normalizada quedo vacia."};
    }

    std::wstring normalized;
    for (std::size_t idx = 0; idx < stack.size(); ++idx) {
        if (idx > 0) {
            normalized.push_back(L'\\');
        }
        normalized.append(stack[idx]);
    }
    return {true, normalized, L""};
}

PathResolutionResult PathResolver::ResolvePmRelativePath(std::wstring_view relative_input) const {
    if (root_dir_.empty()) {
        return {false, L"", L"La raiz de PM no esta inicializada."};
    }
    const PathResolutionResult norm = NormalizePmRelativePath(relative_input);
    if (!norm.ok) {
        return norm;
    }
    std::wstring full = root_dir_;
    if (full.back() != L'\\') {
        full.push_back(L'\\');
    }
    full.append(norm.resolved_path);
    return {true, full, L""};
}

PathResolutionResult PathResolver::ToPersistentRelativePath(std::wstring_view path_input) const {
    if (path_input.empty()) {
        return {false, L"", L"Ruta vacia."};
    }

    std::wstring candidate(path_input);
    for (wchar_t& ch : candidate) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }

    // Si comienza con root_dir_, extraer la parte relativa
    if (!root_dir_.empty() && StartsWithIgnoreCase(candidate, root_dir_)) {
        if (candidate.size() == root_dir_.size()) {
            return {false, L"", L"La ruta apunta a la raiz misma y no a un recurso relativo."};
        }
        const wchar_t sep = candidate[root_dir_.size()];
        if (sep == L'\\' || sep == L'/') {
            candidate = candidate.substr(root_dir_.size() + 1);
        } else {
            return {false, L"", L"Ruta absoluta externa a la raiz de PM rechazada."};
        }
    }

    const PathResolutionResult norm = NormalizePmRelativePath(candidate);
    if (!norm.ok) {
        return norm;
    }

    std::wstring portable = norm.resolved_path;
    for (wchar_t& ch : portable) {
        if (ch == L'\\') {
            ch = L'/';
        }
    }
    return {true, portable, L""};
}

bool PathResolver::IsValidPersistentPmPath(std::wstring_view path_input) noexcept {
    if (IsAbsoluteOrRootedPath(path_input) || ContainsEnvironmentToken(path_input)) {
        return false;
    }
    const PathResolutionResult norm = NormalizePmRelativePath(path_input);
    return norm.ok;
}

PathResolutionResult PathResolver::ResolveHostTokenPath(std::wstring_view tokenized_path) {
    if (tokenized_path.empty() || tokenized_path.front() != L'%') {
        return {false, L"", L"La ruta de anfitrion debe comenzar con un token autorizado (%APPDATA%, %LOCALAPPDATA%, etc.)."};
    }
    const std::size_t closing_pct = tokenized_path.find(L'%', 1);
    if (closing_pct == std::wstring_view::npos || closing_pct <= 1) {
        return {false, L"", L"Token de entorno mal formado."};
    }

    const std::wstring_view token_name = tokenized_path.substr(1, closing_pct - 1);
    if (!EqualsIgnoreCase(token_name, L"APPDATA") &&
        !EqualsIgnoreCase(token_name, L"LOCALAPPDATA") &&
        !EqualsIgnoreCase(token_name, L"TEMP") &&
        !EqualsIgnoreCase(token_name, L"USERPROFILE")) {
        return {false, L"", L"Token de entorno no autorizado para recetas de PM."};
    }

    const std::wstring token_str(token_name);
    const std::wstring base_dir = TrimTrailingSeparators(QueryEnvVar(token_str.c_str()));
    if (base_dir.empty()) {
        return {false, L"", L"No se pudo resolver la variable de entorno del anfitrion."};
    }

    std::wstring_view remainder = tokenized_path.substr(closing_pct + 1);
    while (!remainder.empty() && (remainder.front() == L'\\' || remainder.front() == L'/')) {
        remainder.remove_prefix(1);
    }

    if (remainder.empty()) {
        return {true, base_dir, L""};
    }

    const PathResolutionResult norm_sub = NormalizePmRelativePath(remainder);
    if (!norm_sub.ok) {
        return {false, L"", L"Subruta posterior al token insegura o invalida: " + norm_sub.error_message};
    }

    std::wstring full = base_dir;
    if (full.back() != L'\\') {
        full.push_back(L'\\');
    }
    full.append(norm_sub.resolved_path);
    return {true, full, L""};
}

} // namespace pm::core
