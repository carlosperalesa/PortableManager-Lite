#include "core/atomic_file.h"
#include "core/config_manager.h"
#include "core/path_resolver.h"
#include "core/pm_version.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool RunProcessCapture(
    const std::wstring& cmd_line,
    const wchar_t* working_dir,
    DWORD& exit_code,
    std::string& stdout_text) {

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    if (!::CreatePipe(&hRead, &hWrite, &sa, 0)) {
        return false;
    }
    ::SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    const BOOL ok = ::CreateProcessW(
        nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, working_dir, &si, &pi);
    ::CloseHandle(hWrite);

    if (!ok) {
        ::CloseHandle(hRead);
        return false;
    }

    stdout_text.clear();
    char buf[512];
    DWORD read_bytes = 0;
    while (::ReadFile(hRead, buf, sizeof(buf), &read_bytes, nullptr) && read_bytes > 0) {
        stdout_text.append(buf, read_bytes);
    }

    ::WaitForSingleObject(pi.hProcess, 10000);
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(hRead);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

void DeleteDirectoryRecursive(const std::wstring& dir_path) {
    const std::wstring pattern = dir_path + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring_view name(fd.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring full = dir_path + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                DeleteDirectoryRecursive(full);
            } else {
                ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(full.c_str());
            }
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
    }
    ::RemoveDirectoryW(dir_path.c_str());
}

int CountTempFilesInDir(const std::wstring& dir_path, const std::wstring& prefix_filter) {
    int count = 0;
    const std::wstring pattern = dir_path + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name(fd.cFileName);
            if (name.find(prefix_filter) != std::wstring::npos) {
                ++count;
            }
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
    }
    return count;
}

bool ContainsDriveLetterInLines(const std::string& ini_content) {
    // Busca cualquier 'X:' fuera de líneas de comentario
    std::size_t pos = 0;
    while (pos < ini_content.size()) {
        std::size_t end = ini_content.find('\n', pos);
        if (end == std::string::npos) {
            end = ini_content.size();
        }
        std::string_view line(ini_content.data() + pos, end - pos);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (!line.empty() && line.front() != ';' && line.front() != '#') {
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                const char c = line[i];
                if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && line[i + 1] == ':') {
                    return true;
                }
            }
        }
        pos = end + 1;
    }
    return false;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::fwprintf(stderr, L"Uso: test_phase1_core.exe <ruta_a_pml.exe>\n");
        return 2;
    }

    const std::wstring pm_exe_source = argv[1];
    wchar_t temp_buf[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp_buf);

    const std::wstring sandbox_root =
        std::wstring(temp_buf) + L"PM_Phase1_CoreTest_" + std::to_wstring(::GetCurrentProcessId());
    DeleteDirectoryRecursive(sandbox_root);
    ::CreateDirectoryW(sandbox_root.c_str(), nullptr);

    const std::wstring fake_cwd = sandbox_root + L"\\FakeWorkingDir";
    ::CreateDirectoryW(fake_cwd.c_str(), nullptr);

    // Cambiar deliberadamente el CWD del proceso de prueba a fake_cwd
    wchar_t original_cwd[MAX_PATH]{};
    ::GetCurrentDirectoryW(MAX_PATH, original_cwd);
    ::SetCurrentDirectoryW(fake_cwd.c_str());

    auto fail_and_cleanup = [&](const wchar_t* msg, int code) -> int {
        std::fwprintf(stderr, L"[FAIL] %ls\n", msg);
        ::SetCurrentDirectoryW(original_cwd);
        DeleteDirectoryRecursive(sandbox_root);
        return code;
    };

    // =========================================================================
    // SUBPRUEBA 1: PathResolver
    // =========================================================================
    {
        using pm::core::PathResolver;

        // 1.1 Raíz obtenida desde la ubicación real del ejecutable (no de fake_cwd)
        PathResolver default_resolver;
        if (default_resolver.root_dir().empty() || default_resolver.root_dir() == fake_cwd) {
            return fail_and_cleanup(L"1.1 PathResolver uso el CWD en lugar de GetModuleFileNameW.", 10);
        }

        // 1.2 Raíz sintética y resolución de rutas relativas y normalización
        const std::wstring synth_pm_root = sandbox_root + L"\\SynthPmRoot";
        PathResolver resolver(synth_pm_root);

        auto r1 = resolver.ResolvePmRelativePath(L"App/firefox/./sub/../wrapper.ini");
        if (!r1.ok || r1.resolved_path != synth_pm_root + L"\\App\\firefox\\wrapper.ini") {
            return fail_and_cleanup(L"1.2 Fallo normalizacion de ruta relativa valida.", 11);
        }

        // 1.3 Rechazo de escapes mediante '..'
        auto esc1 = resolver.ResolvePmRelativePath(L"..\\outside.ini");
        auto esc2 = resolver.ResolvePmRelativePath(L"Data/../../escape.ini");
        auto esc3 = resolver.ResolvePmRelativePath(L"App/a/b/../../../../secret.txt");
        if (esc1.ok || esc2.ok || esc3.ok) {
            return fail_and_cleanup(L"1.3 PathResolver no rechazo escape mediante '..'.", 12);
        }

        // 1.4 Rechazo de rutas absolutas, UNC, dispositivos DOS y caracteres inválidos
        auto bad_abs = resolver.ResolvePmRelativePath(L"C:\\Windows\\System32\\cmd.exe");
        auto bad_unc = resolver.ResolvePmRelativePath(L"\\\\server\\share\\file.ini");
        auto bad_dos = resolver.ResolvePmRelativePath(L"Data\\CON.ini");
        auto bad_chr = resolver.ResolvePmRelativePath(L"Data\\bad*name.ini");
        if (bad_abs.ok || bad_unc.ok || bad_dos.ok || bad_chr.ok) {
            return fail_and_cleanup(L"1.4 PathResolver acepto ruta insegura o absoluta.", 13);
        }

        // 1.5 Resolución de %APPDATA% y %LOCALAPPDATA% y bloqueo de escape desde token
        auto appdata_res = PathResolver::ResolveHostTokenPath(L"%APPDATA%/VendorLabs/Sub/..\\TestApp");
        auto localappdata_res = PathResolver::ResolveHostTokenPath(L"%LOCALAPPDATA%\\TestApp\\Cache");
        auto token_escape = PathResolver::ResolveHostTokenPath(L"%APPDATA%\\..\\..\\Windows");
        auto unauth_token = PathResolver::ResolveHostTokenPath(L"%SYSTEMROOT%\\System32");
        if (!appdata_res.ok || appdata_res.resolved_path.find(L"\\VendorLabs\\TestApp") == std::wstring::npos) {
            return fail_and_cleanup(L"1.5 Fallo resolucion de %APPDATA%.", 14);
        }
        if (!localappdata_res.ok || localappdata_res.resolved_path.find(L"\\TestApp\\Cache") == std::wstring::npos) {
            return fail_and_cleanup(L"1.5 Fallo resolucion de %LOCALAPPDATA%.", 15);
        }
        if (token_escape.ok || unauth_token.ok) {
            return fail_and_cleanup(L"1.5 No se rechazo escape o token no autorizado en ResolveHostTokenPath.", 16);
        }

        // 1.6 Separación estricta entre rutas persistentes de PM y rutas externas del anfitrión
        if (!PathResolver::IsValidPersistentPmPath(L"App/PuTTY/putty.exe") ||
            PathResolver::IsValidPersistentPmPath(L"%APPDATA%/PuTTY") ||
            PathResolver::IsValidPersistentPmPath(L"C:/App/PuTTY/putty.exe") ||
            PathResolver::IsValidPersistentPmPath(L"../App/PuTTY/putty.exe")) {
            return fail_and_cleanup(L"1.6 Fallo separacion entre rutas persistentes de PM y rutas externas.", 17);
        }
    }

    // =========================================================================
    // SUBPRUEBA 2: AtomicFile
    // =========================================================================
    {
        using pm::core::AtomicFile;

        const std::wstring atomic_dir = sandbox_root + L"\\AtomicTests";
        const std::wstring target_file = atomic_dir + L"\\sample.ini";
        const std::wstring foreign_temp = atomic_dir + L"\\sample.ini.foreign_tmp";

        // 2.1 Archivo nuevo + preservación de temporal ajeno preexistente
        const std::string initial_payload = "[Section]\r\nKey=ValorOriginal_12345\r\n";
        auto w1 = AtomicFile::WriteUtf8Text(target_file, initial_payload, false);
        if (!w1.ok) {
            return fail_and_cleanup(L"2.1 AtomicFile fallo al crear archivo nuevo.", 20);
        }

        // Crear un archivo temporal ajeno junto al destino
        auto wf = AtomicFile::WriteUtf8Text(foreign_temp, "FOREIGN_TEMP_DO_NOT_DELETE", false);
        if (!wf.ok) {
            return fail_and_cleanup(L"2.1 No se pudo crear archivo temporal ajeno de prueba.", 21);
        }

        // 2.2 Reemplazo de archivo existente
        const std::string replaced_payload = "[Section]\r\nKey=ValorReemplazado_67890\r\n";
        auto w2 = AtomicFile::WriteUtf8Text(target_file, replaced_payload, false);
        auto r2 = AtomicFile::ReadUtf8Text(target_file);
        if (!w2.ok || !r2.ok || r2.content != replaced_payload) {
            return fail_and_cleanup(L"2.2 AtomicFile fallo al reemplazar archivo existente.", 22);
        }

        // 2.3 Escritura interrumpida: debe fallar, conservar el original y borrar su temporal propio
        const std::string interrupted_payload = "[Section]\r\nKey=ValorCorruptoQueNoDebePersistir\r\n";
        auto w_int = AtomicFile::WriteUtf8Text(target_file, interrupted_payload, true);
        auto r_after_int = AtomicFile::ReadUtf8Text(target_file);
        const int own_temps_after_int = CountTempFilesInDir(atomic_dir, L".pmtmp.");
        if (w_int.ok || !w_int.temp_cleaned_up || own_temps_after_int != 0 ||
            !r_after_int.ok || r_after_int.content != replaced_payload) {
            return fail_and_cleanup(L"2.3 Escritura interrumpida altero el original o dejo temporales.", 23);
        }

        // 2.4 Error de acceso en reemplazo (destino marcado como solo lectura)
        ::SetFileAttributesW(target_file.c_str(), FILE_ATTRIBUTE_READONLY);
        auto w_denied = AtomicFile::WriteUtf8Text(target_file, "[Section]\r\nKey=NoPermitido\r\n", false);
        ::SetFileAttributesW(target_file.c_str(), FILE_ATTRIBUTE_NORMAL);

        auto r_after_denied = AtomicFile::ReadUtf8Text(target_file);
        const int own_temps_after_denied = CountTempFilesInDir(atomic_dir, L".pmtmp.");
        auto r_foreign = AtomicFile::ReadUtf8Text(foreign_temp);

        if (w_denied.ok || w_denied.win32_error == ERROR_SUCCESS ||
            !w_denied.temp_cleaned_up || own_temps_after_denied != 0 ||
            !r_after_denied.ok || r_after_denied.content != replaced_payload ||
            !r_foreign.ok || r_foreign.content != "FOREIGN_TEMP_DO_NOT_DELETE") {
            return fail_and_cleanup(L"2.4 Error de acceso no conservo el original o borro temporal ajeno.", 24);
        }
    }

    // =========================================================================
    // SUBPRUEBA 3: ConfigManager
    // =========================================================================
    {
        using pm::core::AtomicFile;
        using pm::core::ConfigManager;
        using pm::core::PathResolver;

        const std::wstring cfg_root = sandbox_root + L"\\ConfigTests";
        PathResolver resolver(cfg_root);

        // 3.1 Crear un config.ini existente con UTF-8 español, comentarios, secciones y claves desconocidas
        const std::wstring config_path = cfg_root + L"\\Data\\config.ini";
        const std::string seed_ini =
            "; Comentario inicial: Configuración general de PM Lite\r\n"
            "# Segundo comentario con eñe y tildes: camión, pingüino, ¡ok!\r\n"
            "\r\n"
            "[General]\r\n"
            "Language=es-ES\r\n"
            "Description=Entorno portátil — validación española (ñ, Ñ, á, é, í, ó, ú, ü)\r\n"
            "UnknownLegacyFlag=keep_me_intact_42\r\n"
            "AppsDir=App\r\n"
            "\r\n"
            "[FutureSection_v9]\r\n"
            "; Comentario interno de sección futura\r\n"
            "FutureSetting=experimental_value\r\n"
            "RawUnparsedLineWithoutEquals\r\n";

        if (!AtomicFile::WriteUtf8Text(config_path, seed_ini).ok) {
            return fail_and_cleanup(L"3.1 No se pudo sembrar config.ini inicial.", 30);
        }

        // 3.2 Cargar, verificar lectura UTF-8 y actualizar una sola clave preservando el resto
        ConfigManager mgr1(resolver);
        if (!mgr1.LoadConfigIni(false).ok) {
            return fail_and_cleanup(L"3.2 ConfigManager fallo al cargar config.ini existente.", 31);
        }

        const std::wstring desc = mgr1.config_doc().GetValue(L"General", L"Description");
        if (desc.find(L"validación española (ñ, Ñ, á, é, í, ó, ú, ü)") == std::wstring::npos) {
            return fail_and_cleanup(L"3.2 Lectura UTF-8 con caracteres españoles incorrecta.", 32);
        }

        // Comprobar que guardar sin modificar NO escribe en disco
        auto no_write = mgr1.SaveConfigIniIfModified(false);
        if (!no_write.ok || no_write.wrote_to_disk) {
            return fail_and_cleanup(L"3.2 SaveConfigIniIfModified escribio en disco sin haber cambios.", 33);
        }

        // Comprobar que rechaza rutas absolutas o con letra de unidad en claves de ruta
        std::wstring set_err;
        if (mgr1.config_doc().SetValue(L"General", L"AppsDir", L"C:\\Program Files\\Apps", set_err) ||
            mgr1.config_doc().SetValue(L"General", L"AppsDir", L"%APPDATA%\\Apps", set_err) ||
            mgr1.config_doc().SetValue(L"General", L"AppsDir", L"..\\OutsideApps", set_err)) {
            return fail_and_cleanup(L"3.2 ConfigManager permitio ruta absoluta, token o '..' en clave persistente.", 34);
        }

        // Actualizar una clave existente y añadir una clave nueva en apps.ini
        if (!mgr1.config_doc().SetValue(L"General", L"Language", L"es-CL", set_err) ||
            !mgr1.config_doc().SetRelativePathValue(L"General", L"ExampleDir", L"Data\\example", set_err)) {
            return fail_and_cleanup(L"3.2 Fallo al actualizar valores validos en config.ini.", 35);
        }

        auto save1 = mgr1.SaveConfigIniIfModified(false);
        if (!save1.ok || !save1.wrote_to_disk) {
            return fail_and_cleanup(L"3.2 SaveConfigIniIfModified no persistio los cambios.", 36);
        }

        // Configurar también apps.ini desde cero
        if (!mgr1.LoadAppsIni(true).ok) {
            return fail_and_cleanup(L"3.2 Fallo al inicializar apps.ini nuevo.", 37);
        }
        if (!mgr1.apps_doc().SetValue(L"App.putty", L"Name", L"PuTTY Portátil (Sesión Ñ)", set_err) ||
            !mgr1.apps_doc().SetRelativePathValue(L"App.putty", L"Executable", L"App\\PuTTY\\PUTTY.EXE", set_err) ||
            !mgr1.apps_doc().SetRelativePathValue(L"App.putty", L"PortableDataDir", L"Data\\apps\\putty", set_err)) {
            return fail_and_cleanup(L"3.2 Fallo al registrar app en apps.ini.", 38);
        }
        auto save_apps = mgr1.SaveAppsIniIfModified(true);
        if (!save_apps.ok || !save_apps.wrote_to_disk) {
            return fail_and_cleanup(L"3.2 Fallo al guardar apps.ini nuevo.", 39);
        }

        // 3.3 Simular reinicio con nueva instancia de ConfigManager y verificar preservación + ausencia de unidades
        ConfigManager mgr2(resolver);
        if (!mgr2.LoadConfigIni(false).ok || !mgr2.LoadAppsIni(false).ok) {
            return fail_and_cleanup(L"3.3 Fallo al recargar config.ini / apps.ini tras reinicio.", 40);
        }

        if (mgr2.config_doc().GetValue(L"General", L"Language") != L"es-CL" ||
            mgr2.config_doc().GetValue(L"General", L"UnknownLegacyFlag") != L"keep_me_intact_42" ||
            mgr2.config_doc().GetValue(L"General", L"ExampleDir") != L"Data/example" ||
            mgr2.config_doc().GetValue(L"FutureSection_v9", L"FutureSetting") != L"experimental_value" ||
            mgr2.apps_doc().GetValue(L"App.putty", L"Executable") != L"App/PuTTY/PUTTY.EXE") {
            return fail_and_cleanup(L"3.3 Valores o claves desconocidas no se preservaron tras reinicio.", 41);
        }

        const auto raw_cfg = AtomicFile::ReadUtf8Text(config_path);
        const auto raw_apps = AtomicFile::ReadUtf8Text(mgr2.apps_ini_path());
        if (!raw_cfg.ok || !raw_apps.ok ||
            raw_cfg.content.find("; Comentario inicial:") == std::string::npos ||
            raw_cfg.content.find("# Segundo comentario con eñe") == std::string::npos ||
            raw_cfg.content.find("RawUnparsedLineWithoutEquals") == std::string::npos ||
            ContainsDriveLetterInLines(raw_cfg.content) ||
            ContainsDriveLetterInLines(raw_apps.content)) {
            return fail_and_cleanup(L"3.3 Comentarios perdidos o letra de unidad detectada en INI persistente.", 42);
        }
    }

    // =========================================================================
    // SUBPRUEBA 4: Integración con pml.exe en dos ubicaciones sintéticas distintas
    // =========================================================================
    {
        using pm::core::AtomicFile;

        const std::wstring loc_a = sandbox_root + L"\\Location_A_PendriveSim";
        const std::wstring loc_b = sandbox_root + L"\\Location_B_DeepFolder\\Sub\\PM";
        DWORD err = 0;
        pm::core::AtomicFile::EnsureParentDirectories(loc_a + L"\\pml.exe", err);
        pm::core::AtomicFile::EnsureParentDirectories(loc_b + L"\\pml.exe", err);

        const std::wstring pm_a = loc_a + L"\\pml.exe";
        const std::wstring pm_b = loc_b + L"\\pml.exe";
        if (!::CopyFileW(pm_exe_source.c_str(), pm_a.c_str(), FALSE) ||
            !::CopyFileW(pm_exe_source.c_str(), pm_b.c_str(), FALSE)) {
            return fail_and_cleanup(L"4.1 No se pudo copiar pml.exe a las ubicaciones sinteticas.", 50);
        }

        DWORD rc_a1 = 999, rc_a2 = 999, rc_b1 = 999;
        std::string out_a1, out_a2, out_b1;
        if (!RunProcessCapture(L"\"" + pm_a + L"\" --init-config", fake_cwd.c_str(), rc_a1, out_a1) || rc_a1 != 0 ||
            out_a1.find("WroteConfig=1") == std::string::npos) {
            return fail_and_cleanup(L"4.2 Ejecucion de pml.exe --init-config en Location_A fallo.", 51);
        }
        // Segunda ejecución sin cambios no debe reescribir config.ini (WroteConfig=0)
        if (!RunProcessCapture(L"\"" + pm_a + L"\" --init-config", fake_cwd.c_str(), rc_a2, out_a2) || rc_a2 != 0 ||
            out_a2.find("WroteConfig=0") == std::string::npos) {
            return fail_and_cleanup(L"4.3 Segunda ejecucion de pml.exe --init-config reescribio innecesariamente.", 52);
        }
        if (!RunProcessCapture(L"\"" + pm_b + L"\" --init-config", fake_cwd.c_str(), rc_b1, out_b1) || rc_b1 != 0 ||
            out_b1.find("WroteConfig=1") == std::string::npos) {
            return fail_and_cleanup(L"4.4 Ejecucion de pml.exe --init-config en Location_B fallo.", 53);
        }

        const auto cfg_a = AtomicFile::ReadUtf8Text(loc_a + L"\\Data\\config.ini");
        const auto cfg_b = AtomicFile::ReadUtf8Text(loc_b + L"\\Data\\config.ini");
        if (!cfg_a.ok || !cfg_b.ok || cfg_a.content != cfg_b.content ||
            ContainsDriveLetterInLines(cfg_a.content)) {
            return fail_and_cleanup(L"4.5 Los config.ini generados en ubicaciones distintas no son identicos o contienen letras de unidad.", 54);
        }
    }

    ::SetCurrentDirectoryW(original_cwd);
    DeleteDirectoryRecursive(sandbox_root);

    std::wprintf(L"[PASS] test_phase1_core (PathResolver + AtomicFile + ConfigManager + Integracion) superado exitosamente.\n");
    return 0;
}
