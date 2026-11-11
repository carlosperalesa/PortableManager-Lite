# Resultados finales de validación D1 y D3

11 de noviembre de 2026. Entorno independiente en D:\Data\PM_Lite_FinalValidation. No reconstruye PM Core. Ningún informe antiguo fue recuperado ni recreado como original.

## Estado y evidencia

| Comprobación | Estado | Evidencia nueva |
|---|---|---|
| Integridad original FirefoxPortable y PuTTY | Aprobada; SHA-256 exactos | reports/package_integrity.json |
| Authenticode FirefoxPortable | Valid; RARE IDEAS, LLC | reports/package_integrity.json |
| ZIP PuTTY y versión | CRC válido; Release 0.85 | reports/package_integrity.json |
| Pruebas unitarias fase 1 | 8 aprobadas | reports/unit_tests.json |
| Regresión final tras ampliar auditoría | 9 aprobadas; clave sintética ausente | reports/unit_tests_final.json |
| D1 perfil USB, marcador y preferencia | Funcionalidad aprobada en dos sesiones | reports/d1_processes_first_session.json, d1_first_close_verification.json, d1_second_close_verification.json |
| D1 resultado global | AMARILLO; residuos y límites de auditoría | reports/d1_firefoxportable.json, d1_registry_diff.json, d1_residue_recipe.md |
| D3 persistencia y transacción genérica | Aprobadas; clave originalmente ausente y finalmente ausente | reports/d3_putty.json, final_scoped_checks.json |
| D3 resultado global | AMARILLO; archivo Windows Recent conservado | reports/d3_recent_file_attribution.json, d3_host_diff.json |

## Ejecución real D1

El usuario instaló FirefoxPortable 160.0 con el PAF oficial validado a E:\PM_POC\App\FirefoxPortable. No había instalación previa. Se lanzó exclusivamente FirefoxPortable.exe, con página HTML local en Data. El proceso Firefox recibió explícitamente -profile E:\PM_POC\App\FirefoxPortable\Data\profile. El Firefox de D: estaba cerrado antes de empezar y no se lanzó como sustituto.

El usuario creó el marcador y cambió desde la GUI browser.ctrlTab.sortByRecentlyUsed, de desmarcado a marcado. Tras el primer cierre, SELECT de solo lectura encontró PM_POC_TEST. (con punto final), y prefs.js contenía true. En la segunda sesión el usuario corrigió el nombre y confirmó la preferencia y el cierre. La lectura final verificó PM_POC_TEST exacto y true. No se editaron places.sqlite ni prefs.js mediante scripts. Ambos cierres terminaron sin procesos Firefox.

## Residuos D1

Se detectaron 23 nuevos valores con nombres que incluyen la ruta USB bajo HKCU\Software\Mozilla\Firefox, en Default Browser Agent, DllPrefetchExperiment, Launcher y PreXULSkeletonUISettings. Son atribuibles a la instalación de prueba y permanecen intactos. La receta propuesta está en reports/d1_residue_recipe.md; no se ejecutó limpieza.

Entre 14.461 archivos legibles del inventario inicial y posterior no aparecieron archivos nuevos, modificados ni eliminados. Los archivos inventariados de D:\App\FirefoxPortable\Data conservaron hashes y metadatos. No se detectaron archivos nuevos Crash Reports-BackupByFirefoxPortable en los alcances registrados. LocalAppData\Mozilla pasó de presente a ausente y no tenía archivos inventariados; no se reconstruyó ni se atribuye con certeza la eliminación de esa carpeta vacía.

Limitaciones: tres temporales preexistentes de VSCode bloqueados; ausencia de .mozilla registrada como error de recorrido; directorios vacíos no inventariados salvo raíces. El registro inicial conserva nombres y tipos, no contenidos de valores, por lo que no demuestra que valores existentes no hayan cambiado. Las evidencias prueban el perfil USB explícito y la integridad de los archivos legibles del perfil D:, pero no excluyen toda utilización transitoria del anfitrión.

## Ejecución real D3

Se verificó de nuevo el ZIP y se extrajo sin sobrescribir a E:\PM_POC\App\PuTTY. HKCU\Software\SimonTatham\PuTTY estaba ausente. Una transacción configurable guardó esa ausencia en RAM durante ambas sesiones. El usuario guardó PM_POC_TEST con host 127.0.0.1, cerró, volvió a abrir, cargó y verificó la sesión y cerró. Las lecturas del registro verificaron persistencia antes de la restauración; no se inició una conexión según las instrucciones y confirmaciones del usuario. No se capturó tráfico para demostrar ausencia de conexiones de todo el sistema.

El mismo proceso vivo de la transacción restauró la ausencia original y la verificó. No utilizó -cleanup. La clave PuTTY y la clave de pruebas sintéticas están ausentes al final. No se afirma restauración de ACL. La sesión ficticia se eliminó del registro durante la restauración; no se implementó exportación persistente de sesiones al USB.

## Residuos D3

Archivo nuevo conservado:

`%APPDATA%\Microsoft\Windows\Recent\CustomDestinations\9390ee5b658e96e.customDestinations-ms`

Contiene PuTTY y la ruta E:\PM_POC\App\PuTTY; se almacenaron únicamente metadatos, SHA-256 y resultados booleanos de búsqueda, no su contenido. No se limpió. Receta propuesta: autorización expresa, cierre de PuTTY, nueva comprobación de procedencia y ausencia original, y eliminación exclusiva de este archivo si sigue siendo exclusivamente atribuible; no borrar carpetas Recent ni destinos de otras aplicaciones.

HKCU\Software\SimonTatham quedó vacía, pero no se capturó su presencia original: se conservó por procedencia incierta. La auditoría amplia registró cinco archivos nuevos y 40 modificados, sin archivos eliminados; cuatro nuevos son cachés Edge. El resto de cambios corresponde a rutas de Codex, Edge y otros componentes del anfitrión, cuya atribución causal no se demuestra por metadatos. No se tocaron. No se hallaron rutas nuevas con nombres PuTTY/SimonTatham, pero el archivo Recent demuestra que una búsqueda solo por nombre sería insuficiente.

Alcance: 165.543 archivos y 29.291 directorios iniciales en USERPROFILE (incluidos AppData y LocalAppData) y TEMP, solo metadatos, cero errores. TEMP: cuatro archivos legibles con SHA-256 sin diferencias, y tres bloqueados en ambos inventarios. No se prueba ausencia absoluta de cambios de contenido conservando metadatos ni limpieza integral del anfitrión.

## Auditoría final y USB

El USB fue identificado por marcador autorizado, GUID aec5fbfe-d7ae-11f1-a523-00090faa0001 y serial 048CCF2C. Permaneció conectado. NTFS Healthy/OK. Inventario final: 2.126 archivos de PM_POC con hashes, sin errores; conserva FirefoxPortable, su perfil/marcador/preferencia, PuTTY y HTML local. No se formateó ni se modificaron particiones.

No hay procesos Firefox/FirefoxPortable/PuTTY ni proceso de transacción pendiente. No se encontraron PM_POC_TestApp, .pmbk o candidatos de runtime temporal PM en USERPROFILE/TEMP/PM_POC inventariados; no constituye barrido de todos los discos. No hay snapshots RAM ni restauraciones pendientes. No se eliminaron residuos desconocidos ni se forzó cierre de procesos.

Restart Manager no informó aplicaciones usando los 2.127 archivos registrados (PM_POC y marcador). Consulta cerrada mediante RmEndSession. El inventario y las conexiones SQLite finalizaron y cerraron sus identificadores. No se informó ningún trabajo de almacenamiento y la muestra de E: mostró cola 0, escrituras/s 0 y bytes escritos/s 0. Es una comprobación puntual; Restart Manager no enumera todos los identificadores ni directorios del sistema. Windows debe realizar la expulsión segura y sus comprobaciones finales; si la rechaza, no retirar el dispositivo.

Evidencias: reports/usb_open_files.json, usb_final_inventory.json, usb_final_state.json y final_scoped_checks.json. Los paquetes originales siguen íntegros; la firma Firefox continúa válida. applications_executed=false en package_integrity.json describe únicamente esa comprobación de integridad, que no ejecuta aplicaciones.

## Antecedentes y decisiones pendientes

Comunicados previamente por el usuario, no verificables con informes originales actuales: 3A/3B/3B.6/A/B/C aprobadas, D1 amarillo, D2 verde y D3 individual verde. A/B/C/D2 no se repitieron, incluida la desconexión física.

No se declara PM Lite listo para producción ni anfitrión restaurado íntegramente. Se puede planificar la implementación con estas evidencias; antes de cerrar el diseño faltan política de residuos Firefox/Windows Recent, baseline de valores y carpetas vacías, recuperación ante caídas y persistencia portable de sesiones PuTTY. La integración de raíces genéricas al futuro PM Core requiere autorización; el módulo de prueba no modifica código de producción. No existe repositorio ni commit.

Documentos finales: README.md, CONTINUIDAD_DESARROLLO.md, RESULTADOS_D1_D3.md y reports/d1_firefoxportable.json, d3_putty.json, package_integrity.json; evidencia auxiliar en reports/.

PRUEBAS TERMINADAS. YA PUEDES EXPULSAR EL USB DE FORMA SEGURA MEDIANTE WINDOWS.

No retirarlo físicamente antes de que Windows confirme la expulsión.
