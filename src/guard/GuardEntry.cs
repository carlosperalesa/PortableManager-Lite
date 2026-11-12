using System;

namespace PortableManager.Guard
{
    public static class GuardEntry
    {
        /// <summary>
        /// Punto de entrada del modo guardia: pml.exe --guard &lt;SessionID&gt; [PmProcessHandleHex]
        /// </summary>
        public static int RunGuardMode(string sessionId, string pmHandleArg)
        {
            if (string.IsNullOrWhiteSpace(sessionId))
            {
                Console.Error.WriteLine("[PMGuard] Error: SessionID vacio en modo --guard.");
                return 2;
            }

            Console.WriteLine(
                "[PMGuard] Fase 0 OK | SessionID={0} | HandleArg={1}",
                sessionId,
                string.IsNullOrEmpty(pmHandleArg) ? "NONE" : pmHandleArg);
            return 0;
        }
    }
}
