using System;
using System.Diagnostics;

// Build-time helper only: starts the Windows GUI application and returns its exit code.
// It is never copied into the user-facing portable archive or installer.
public static class SmokeLauncher {
    public static int Main(string[] args) {
        if (args.Length < 1) return 2;
        try {
            var start = new ProcessStartInfo {
                FileName = args[0], UseShellExecute = false, CreateNoWindow = true,
                WorkingDirectory = System.IO.Path.GetDirectoryName(args[0])
            };
            for (int i = 1; i < args.Length; ++i) {
                start.Arguments += (i == 1 ? "" : " ") + "\"" + args[i].Replace("\"", "\\\"") + "\"";
            }
            using (var process = Process.Start(start)) {
                if (!process.WaitForExit(30000)) { process.Kill(); return 124; }
                return process.ExitCode;
            }
        } catch (Exception exception) {
            Console.Error.WriteLine(exception.Message);
            return 1;
        }
    }
}
