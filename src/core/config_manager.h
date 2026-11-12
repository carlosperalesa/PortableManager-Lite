#pragma once

#include "core/atomic_file.h"
#include "core/path_resolver.h"

#include <string>
#include <string_view>
#include <vector>

namespace pm::core {

struct ConfigOperationResult {
    bool ok = false;
    bool wrote_to_disk = false;
    DWORD win32_error = ERROR_SUCCESS;
    std::wstring error_message;
};

class IniDocument {
public:
    enum class LineKind {
        Blank,
        Comment,
        Section,
        KeyValue,
        UnknownRaw
    };

    struct EntryLine {
        LineKind kind = LineKind::Blank;
        std::wstring raw_text;      // Para Blank, Comment, UnknownRaw o preservación de formato
        std::wstring section_name;  // Para Section y KeyValue
        std::wstring key;           // Para KeyValue
        std::wstring value;         // Para KeyValue
        std::wstring inline_suffix; // No modifica comentarios de línea independientes
    };

    // Parsea texto UTF-8 preservando comentarios, orden de líneas y claves desconocidas.
    bool ParseUtf8(std::string_view utf8_content, std::wstring& out_error);

    // Serializa el documento completo a UTF-8 sin BOM.
    std::string SerializeUtf8() const;

    bool HasSection(std::wstring_view section) const noexcept;
    bool HasKey(std::wstring_view section, std::wstring_view key) const noexcept;

    std::wstring GetValue(
        std::wstring_view section,
        std::wstring_view key,
        std::wstring_view default_value = L"") const;

    // Valida y establece un valor. Si el valor coincide exactamente con el actual,
    // no marca el documento como modificado (dirty == false).
    // Si la clave corresponde a una ruta persistente de PM (o enforce_relative_path == true),
    // exige que sea una ruta relativa segura dentro de PM (sin unidad, sin UNC, sin '..', sin '%...%').
    bool SetValue(
        std::wstring_view section,
        std::wstring_view key,
        std::wstring_view value,
        std::wstring& out_error,
        bool enforce_relative_path = false);

    bool SetRelativePathValue(
        std::wstring_view section,
        std::wstring_view key,
        std::wstring_view relative_path,
        std::wstring& out_error);

    void RemoveSection(std::wstring_view section);
    void RemoveKey(std::wstring_view section, std::wstring_view key);

    bool is_dirty() const noexcept { return dirty_; }
    void ClearDirty() noexcept { dirty_ = false; }
    void MarkDirty() noexcept { dirty_ = true; }

    const std::vector<EntryLine>& lines() const noexcept { return lines_; }

    static std::string WideToUtf8(std::wstring_view wide_str);
    static std::wstring Utf8ToWide(std::string_view utf8_str);

private:
    std::vector<EntryLine> lines_;
    bool dirty_ = false;

    static bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept;
    static bool EndsWithIgnoreCase(std::wstring_view text, std::wstring_view suffix) noexcept;
    static std::wstring Trim(std::wstring_view text);
    static bool IsValidIdentifier(std::wstring_view token, bool allow_dots_and_dashes) noexcept;
    static bool IsValidValueText(std::wstring_view val) noexcept;
    static bool IsPersistentPathKeyName(std::wstring_view key) noexcept;
};

class ConfigManager {
public:
    static inline constexpr wchar_t kDefaultConfigRelPath[] = L"Data\\config.ini";
    static inline constexpr wchar_t kDefaultAppsRelPath[]   = L"Data\\apps.ini";

    explicit ConfigManager(const PathResolver& resolver);

    const PathResolver& resolver() const noexcept { return resolver_; }

    std::wstring config_ini_path() const { return config_ini_path_; }
    std::wstring apps_ini_path() const { return apps_ini_path_; }

    IniDocument& config_doc() noexcept { return config_doc_; }
    const IniDocument& config_doc() const noexcept { return config_doc_; }

    IniDocument& apps_doc() noexcept { return apps_doc_; }
    const IniDocument& apps_doc() const noexcept { return apps_doc_; }

    // Carga Data/config.ini (si no existe y create_if_missing es true, prepara documento nuevo en memoria).
    ConfigOperationResult LoadConfigIni(bool create_if_missing = true);

    // Carga Data/apps.ini (si no existe y create_if_missing es true, prepara documento nuevo en memoria).
    ConfigOperationResult LoadAppsIni(bool create_if_missing = true);

    // Guarda Data/config.ini mediante AtomicFile únicamente si existen cambios pendientes (dirty)
    // o si el archivo aún no existe en disco y force_if_missing == true.
    ConfigOperationResult SaveConfigIniIfModified(bool force_if_missing = false);

    // Guarda Data/apps.ini mediante AtomicFile únicamente si existen cambios pendientes (dirty)
    // o si el archivo aún no existe en disco y force_if_missing == true.
    ConfigOperationResult SaveAppsIniIfModified(bool force_if_missing = false);

    // Métodos genéricos para cargar/guardar cualquier INI relativo a PM (ej. App/<app>/wrapper.ini).
    ConfigOperationResult LoadRelativeIni(
        std::wstring_view relative_ini_path,
        IniDocument& out_doc,
        bool create_if_missing = true) const;

    ConfigOperationResult SaveRelativeIniIfModified(
        std::wstring_view relative_ini_path,
        IniDocument& doc,
        bool force_if_missing = false) const;

private:
    PathResolver resolver_;
    std::wstring config_ini_path_;
    std::wstring apps_ini_path_;
    IniDocument config_doc_;
    IniDocument apps_doc_;

    static ConfigOperationResult LoadIniFileInternal(
        std::wstring_view full_path,
        IniDocument& out_doc,
        bool create_if_missing);

    static ConfigOperationResult SaveIniFileInternal(
        std::wstring_view full_path,
        IniDocument& doc,
        bool force_if_missing);
};

} // namespace pm::core
