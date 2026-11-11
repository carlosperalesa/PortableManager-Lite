#pragma once

#include <string>
#include <string_view>

namespace pm::core {

struct PathResolutionResult {
    bool ok = false;
    std::wstring resolved_path;
    std::wstring error_message;
};

class PathResolver {
public:
    // Inicializa PathResolver determinando la raíz desde GetModuleFileNameW(nullptr).
    // Nunca utiliza el directorio de trabajo actual (CWD).
    PathResolver();

    // Constructor explícito para pruebas sintéticas sobre una raíz controlada.
    explicit PathResolver(std::wstring_view explicit_root_dir);

    // Obtiene la ruta completa del ejecutable actual mediante GetModuleFileNameW.
    static std::wstring QueryExecutablePath();

    // Obtiene el directorio padre del ejecutable actual mediante GetModuleFileNameW.
    static std::wstring QueryExecutableDirectory();

    // Devuelve la raíz activa de PM (sin barra final salvo raíz de volumen).
    const std::wstring& root_dir() const noexcept { return root_dir_; }

    // Normaliza una ruta relativa dentro de PM (usa '\' como separador canónico Win32),
    // resolviendo '.' y '..' léxicamente y rechazando cualquier intento de escapar de la raíz,
    // rutas absolutas, letras de unidad, UNC, tokens '%...%' o caracteres ilegales.
    static PathResolutionResult NormalizePmRelativePath(std::wstring_view relative_input);

    // Resuelve una ruta relativa a la raíz de PM devolviendo la ruta absoluta temporal.
    PathResolutionResult ResolvePmRelativePath(std::wstring_view relative_input) const;

    // Convierte una ruta (relativa o absoluta dentro de root_dir_) a su representación
    // relativa persistente (con '/' para portabilidad en INI). Rechaza rutas fuera de root_dir_.
    PathResolutionResult ToPersistentRelativePath(std::wstring_view path_input) const;

    // Comprueba si una cadena es válida como ruta persistente en config.ini / apps.ini:
    // estrictamente relativa a PM, sin letras de unidad, sin UNC, sin tokens %...% y sin escape '..'.
    static bool IsValidPersistentPmPath(std::wstring_view path_input) noexcept;

    // Comprueba si una ruta contiene letra de unidad ('C:'), prefijo UNC ('\\') o barra inicial.
    static bool IsAbsoluteOrRootedPath(std::wstring_view path_input) noexcept;

    // Comprueba si una ruta contiene algún token de entorno '%TOKEN%'.
    static bool ContainsEnvironmentToken(std::wstring_view path_input) noexcept;

    // Resuelve una ruta externa del anfitrión basada en tokens permitidos para recetas
    // (%APPDATA%, %LOCALAPPDATA%, %TEMP%, %USERPROFILE%).
    // Rechaza escapes '..' que intenten salir del directorio base del token.
    static PathResolutionResult ResolveHostTokenPath(std::wstring_view tokenized_path);

private:
    std::wstring root_dir_;

    static std::wstring TrimTrailingSeparators(std::wstring_view path);
    static bool IsInvalidPathChar(wchar_t ch) noexcept;
    static bool IsReservedDosDeviceName(std::wstring_view segment) noexcept;
};

} // namespace pm::core
