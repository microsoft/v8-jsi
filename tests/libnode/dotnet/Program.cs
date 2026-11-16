// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// A .NET consumer of the Microsoft.JavaScript.LibNode package. The .NET host
// loads libnode from the package's runtimes/<rid>/native folder, and the app
// runs Node.js through the embedding API, like tests/libnode/smoke.c:
//   dotnet LibNodeSmoke.dll <script> [args...]

using System;
using System.Runtime.InteropServices;

internal static class Program
{
    // NODE_EMBEDDING_VERSION and node_embedding_status_error_exit_code in
    // node_embedding_api.h.
    private const int EmbeddingApiVersion = 1;
    private const int ErrorExitCode = 512;

    [DllImport("libnode", CallingConvention = CallingConvention.Cdecl)]
    private static extern int node_embedding_main_run(
        int embeddingApiVersion,
        int argc,
        IntPtr[] argv,
        IntPtr configurePlatform,
        IntPtr configurePlatformData,
        IntPtr configureRuntime,
        IntPtr configureRuntimeData);

    [DllImport("libnode", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr node_embedding_last_error_message_get();

    private static int Main(string[] args)
    {
        // Node.js takes UTF-8 arguments.
        IntPtr[] argv = new IntPtr[args.Length + 1];
        int status;
        try
        {
            argv[0] = Marshal.StringToCoTaskMemUTF8(Environment.ProcessPath ?? "LibNodeSmoke");
            for (int i = 0; i < args.Length; i++)
            {
                argv[i + 1] = Marshal.StringToCoTaskMemUTF8(args[i]);
            }

            status = node_embedding_main_run(
                EmbeddingApiVersion, argv.Length, argv, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        }
        finally
        {
            foreach (IntPtr arg in argv)
            {
                Marshal.FreeCoTaskMem(arg);
            }
        }

        if (status == 0)
        {
            return 0;
        }

        string? message = Marshal.PtrToStringUTF8(node_embedding_last_error_message_get());
        Console.Error.WriteLine($"LibNodeSmoke: {message}");
        return (status & ErrorExitCode) != 0 ? status & ~ErrorExitCode : 1;
    }
}
