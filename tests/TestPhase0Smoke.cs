using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace PortableManager.Tests
{
    public static class TestPhase0Smoke
    {
        private const ushort IMAGE_FILE_MACHINE_AMD64 = 0x8664;
        private const ushort IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP = 0x0400;

        public static int Main(string[] args)
        {
            Console.OutputEncoding = new UTF8Encoding(false);

            if (args == null || args.Length < 1)
            {
                Console.Error.WriteLine("Uso: test_phase0_smoke.exe <ruta_a_pm.exe> [ruta_json_salida]");
                return 2;
            }

            string pmExePath = Path.GetFullPath(args[0]);
            string jsonReportPath = args.Length >= 2 ? Path.GetFullPath(args[1]) : string.Empty;

            if (!File.Exists(pmExePath))
            {
                Console.Error.WriteLine("[FAIL] No existe el ejecutable: " + pmExePath);
                return 1;
            }

            string expectedRoot = Path.GetDirectoryName(pmExePath);
            string tempWorkingDir = Path.GetTempPath().TrimEnd('\\');

            // 1. Verificar arquitectura x64 y flag /SWAPRUN:CD (0x0400) en cabecera PE
            ushort machine = 0;
            ushort characteristics = 0;
            using (FileStream fs = new FileStream(pmExePath, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (BinaryReader br = new BinaryReader(fs))
            {
                fs.Seek(0x3C, SeekOrigin.Begin);
                int e_lfanew = br.ReadInt32();
                fs.Seek(e_lfanew + 4, SeekOrigin.Begin);
                machine = br.ReadUInt16();
                fs.Seek(e_lfanew + 22, SeekOrigin.Begin);
                characteristics = br.ReadUInt16();
            }

            bool isX64 = (machine == IMAGE_FILE_MACHINE_AMD64);
            bool hasSwaprunCd = ((characteristics & IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP) != 0);

            if (!isX64 || !hasSwaprunCd)
            {
                Console.Error.WriteLine(
                    "[FAIL] Cabecera PE invalida: Machine=0x{0:X4}, Characteristics=0x{1:X4}",
                    machine, characteristics);
                return 1;
            }

            // 2. Ejecutar pm.exe --smoke-test desde CWD = %TEMP% y comprobar GetModuleFileNameW
            int rcSmoke;
            string outSmoke;
            string errSmoke;
            RunProcess(pmExePath, "--smoke-test", tempWorkingDir, out rcSmoke, out outSmoke, out errSmoke);
            bool smokeOk = (rcSmoke == 0) &&
                           outSmoke.Contains("[PM] SMOKE_OK") &&
                           outSmoke.IndexOf("Root=" + expectedRoot, StringComparison.OrdinalIgnoreCase) >= 0;

            if (!smokeOk)
            {
                Console.Error.WriteLine("[FAIL] --smoke-test fallo: rc={0}, out={1}, err={2}", rcSmoke, outSmoke, errSmoke);
                return 1;
            }

            // 3. Ejecutar pm.exe --guard <SessionID> desde CWD = %TEMP%
            int rcGuardOk;
            string outGuardOk;
            string errGuardOk;
            RunProcess(pmExePath, "--guard phase0_session_9f1a", tempWorkingDir, out rcGuardOk, out outGuardOk, out errGuardOk);
            bool guardModeOk = (rcGuardOk == 0) &&
                               outGuardOk.Contains("[PMGuard] Fase 0 OK | SessionID=phase0_session_9f1a");

            if (!guardModeOk)
            {
                Console.Error.WriteLine("[FAIL] --guard <SessionID> fallo: rc={0}, out={1}, err={2}", rcGuardOk, outGuardOk, errGuardOk);
                return 1;
            }

            // 4. Ejecutar pm.exe --guard sin SessionID (debe rechazar con código != 0)
            int rcGuardEmpty;
            string outGuardEmpty;
            string errGuardEmpty;
            RunProcess(pmExePath, "--guard", tempWorkingDir, out rcGuardEmpty, out outGuardEmpty, out errGuardEmpty);
            bool guardRejectEmptyOk = (rcGuardEmpty == 2);

            if (!guardRejectEmptyOk)
            {
                Console.Error.WriteLine("[FAIL] --guard sin argumentos debia devolver 2, devolvio {0}", rcGuardEmpty);
                return 1;
            }

            string sha256Hex = ComputeSha256(pmExePath);
            long fileSize = new FileInfo(pmExePath).Length;

            if (!string.IsNullOrEmpty(jsonReportPath))
            {
                string dir = Path.GetDirectoryName(jsonReportPath);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                {
                    Directory.CreateDirectory(dir);
                }
                string json = string.Format(
                    "{{\n" +
                    "  \"phase\": \"Fase 0 — Preparacion del Proyecto y Smoke Test\",\n" +
                    "  \"timestamp_utc\": \"{0}\",\n" +
                    "  \"compiler\": \"C:\\\\Windows\\\\Microsoft.NET\\\\Framework64\\\\v4.0.30319\\\\csc.exe (x64 + Win32 P/Invoke)\",\n" +
                    "  \"binary\": {{\n" +
                    "    \"path\": \"{1}\",\n" +
                    "    \"size_bytes\": {2},\n" +
                    "    \"sha256\": \"{3}\",\n" +
                    "    \"machine\": \"0x{4:X4}\",\n" +
                    "    \"pe_characteristics\": \"0x{5:X4}\",\n" +
                    "    \"is_x64\": {6},\n" +
                    "    \"swaprun_cd_enabled\": {7}\n" +
                    "  }},\n" +
                    "  \"checks\": {{\n" +
                    "    \"smoke_test_from_different_cwd\": {8},\n" +
                    "    \"resolved_root_via_GetModuleFileNameW\": \"{9}\",\n" +
                    "    \"guard_mode_invocation_ok\": {10},\n" +
                    "    \"guard_mode_empty_session_rejected\": {11}\n" +
                    "  }},\n" +
                    "  \"status\": \"passed\"\n" +
                    "}}\n",
                    DateTime.UtcNow.ToString("o"),
                    EscapeJson(pmExePath),
                    fileSize,
                    sha256Hex,
                    machine,
                    characteristics,
                    isX64 ? "true" : "false",
                    hasSwaprunCd ? "true" : "false",
                    smokeOk ? "true" : "false",
                    EscapeJson(expectedRoot),
                    guardModeOk ? "true" : "false",
                    guardRejectEmptyOk ? "true" : "false");

                File.WriteAllText(jsonReportPath, json, new UTF8Encoding(false));
            }

            Console.WriteLine("[PASS] Fase 0 Smoke Test aprobado | SHA256={0} | Machine=0x{1:X4} | Characteristics=0x{2:X4}",
                sha256Hex, machine, characteristics);
            return 0;
        }

        private static void RunProcess(string exePath, string arguments, string workingDir, out int exitCode, out string stdout, out string stderr)
        {
            ProcessStartInfo psi = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = arguments,
                WorkingDirectory = workingDir,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };

            using (Process p = Process.Start(psi))
            {
                stdout = p.StandardOutput.ReadToEnd().Trim();
                stderr = p.StandardError.ReadToEnd().Trim();
                p.WaitForExit(10000);
                exitCode = p.ExitCode;
            }
        }

        private static string ComputeSha256(string filePath)
        {
            using (SHA256 sha = SHA256.Create())
            using (FileStream fs = File.OpenRead(filePath))
            {
                byte[] hash = sha.ComputeHash(fs);
                StringBuilder sb = new StringBuilder(hash.Length * 2);
                for (int i = 0; i < hash.Length; i++)
                {
                    sb.Append(hash[i].ToString("X2"));
                }
                return sb.ToString();
            }
        }

        private static string EscapeJson(string s)
        {
            if (s == null) return string.Empty;
            return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
        }
    }
}
