using System;
using System.Runtime.InteropServices;
using System.Text;

namespace PortableManager.Core
{
    public static class PmVersion
    {
        public const string ProductName = "PortableManager Lite";
        public const string VersionString = "0.2.2";
        public const int VersionMajor = 0;
        public const int VersionMinor = 2;
        public const int VersionPatch = 2;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetModuleFileNameW(IntPtr hModule, StringBuilder lpFilename, uint nSize);

        /// <summary>
        /// Obtiene la ruta absoluta del ejecutable actual mediante la API Win32 GetModuleFileNameW.
        /// Cumple la Directiva 3.1: nunca depender del directorio de trabajo actual (CWD).
        /// </summary>
        public static string GetExecutablePath()
        {
            int capacity = 260;
            while (capacity <= 32768)
            {
                StringBuilder sb = new StringBuilder(capacity);
                uint copied = GetModuleFileNameW(IntPtr.Zero, sb, (uint)capacity);
                if (copied == 0)
                {
                    throw new InvalidOperationException("GetModuleFileNameW failed with error " + Marshal.GetLastWin32Error());
                }
                if (copied < capacity - 1)
                {
                    return sb.ToString(0, (int)copied);
                }
                capacity *= 2;
            }
            throw new InvalidOperationException("Executable path exceeds maximum Win32 path length.");
        }

        /// <summary>
        /// Obtiene el directorio raíz donde reside pm.exe a partir de GetModuleFileNameW.
        /// </summary>
        public static string GetExecutableDirectory()
        {
            string exePath = GetExecutablePath();
            int lastSep = exePath.LastIndexOfAny(new char[] { '\\', '/' });
            if (lastSep <= 0)
            {
                throw new InvalidOperationException("Cannot resolve parent directory from module path: " + exePath);
            }
            return exePath.Substring(0, lastSep);
        }
    }
}
