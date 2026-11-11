using System;
using System.Text;
using PortableManager.Guard;

namespace PortableManager.Core
{
    public static class Program
    {
        public static int Main(string[] args)
        {
            Console.OutputEncoding = new UTF8Encoding(false);

            string exeDir;
            try
            {
                exeDir = PmVersion.GetExecutableDirectory();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("[PM] Error critico en GetModuleFileNameW: " + ex.Message);
                return 1;
            }

            if (args != null && args.Length >= 1)
            {
                string arg0 = args[0];

                if (string.Equals(arg0, "--guard", StringComparison.Ordinal))
                {
                    string sessionId = args.Length >= 2 ? args[1] : string.Empty;
                    string handleArg = args.Length >= 3 ? args[2] : string.Empty;
                    return GuardEntry.RunGuardMode(sessionId, handleArg);
                }

                if (string.Equals(arg0, "--version", StringComparison.Ordinal))
                {
                    Console.WriteLine("{0} v{1} (x64 Native Win32)", PmVersion.ProductName, PmVersion.VersionString);
                    return 0;
                }

                if (string.Equals(arg0, "--smoke-test", StringComparison.Ordinal))
                {
                    Console.WriteLine("[PM] SMOKE_OK | Version={0} | Root={1}", PmVersion.VersionString, exeDir);
                    return 0;
                }
            }

            Console.WriteLine("{0} v{1} | Root={2} | Ready.", PmVersion.ProductName, PmVersion.VersionString, exeDir);
            return 0;
        }
    }
}
