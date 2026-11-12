# Fase 08 — Integración Nativa

**Estado:** `not_run` (pendiente de autorización tras el cierre de la fase anterior).
**Evidencia JSON objetivo:** `reports/rebuild_integration.json`

El directorio de instalación validado contiene el `wrapper.ini` de la app.
El flujo de actualización deberá preservar ese archivo durante la copia y
detectar y excluir cualquier `wrapper.ini` del paquete, también con variantes
de mayúsculas. El paquete no debe sustituir las reglas aprendidas.
