// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;
using LibOSDP.Interop;
using Microsoft.Extensions.Logging;

namespace LibOSDP;

/// <summary>Library-wide information and settings.</summary>
public static unsafe class Osdp
{
    private static readonly object s_initGate = new();
    private static volatile bool s_initialized;
    private static ILogger? s_logger;
    private static int s_minimumLevel = (int)LogLevel.None;

    /// <summary>
    /// How often <see cref="ControlPanel.RunAsync"/> and
    /// <see cref="PeripheralDevice.RunAsync"/> service the bus. OSDP requires
    /// at least once every 50 ms.
    /// </summary>
    public static TimeSpan RefreshInterval { get; } = TimeSpan.FromMilliseconds(20);

    /// <summary>Version of the native libosdp in use, e.g. <c>4.0.0</c>.</summary>
    public static string Version
    {
        get
        {
            EnsureInitialized();
            return Marshal.PtrToStringUTF8((nint)Native.GetVersion()) ?? "";
        }
    }

    /// <summary>Where the native libosdp was built from: git branch, revision and tag.</summary>
    public static string SourceInfo
    {
        get
        {
            EnsureInitialized();
            return Marshal.PtrToStringUTF8((nint)Native.GetSourceInfo()) ?? "";
        }
    }

    internal static void EnsureInitialized()
    {
        if (s_initialized)
            return;
        lock (s_initGate)
        {
            if (s_initialized)
                return;
            AbiGuard.Verify(AbiGuard.Query(), AbiGuard.Expected());
            // LibOSDP prints to stdout until a callback is installed, and it
            // snapshots the callback into each context at setup, so install
            // it before any CP or PD exists. osdp_set_log_callback replaces
            // the whole logger config (osdp_common.c), and with a callback
            // LibOSDP does no level filtering, so SetLogger filters here.
            Native.SetLogCallback(&OnLog);
            s_initialized = true;
        }
    }

    /// <summary>
    /// Route LibOSDP's log output to <paramref name="logger"/>. Process-wide:
    /// LibOSDP has one logger for all devices, and a control panel or
    /// peripheral device created earlier keeps logging to whatever logger is
    /// current. Pass null to silence it.
    /// </summary>
    /// <param name="logger">Destination, or null.</param>
    /// <param name="minimumLevel">Least severe level to forward.</param>
    public static void SetLogger(ILogger? logger, LogLevel minimumLevel = LogLevel.Information)
    {
        EnsureInitialized();
        Volatile.Write(ref s_minimumLevel, (int)minimumLevel);
        Volatile.Write(ref s_logger, logger);
    }

    [UnmanagedCallersOnly]
    private static void OnLog(int pd, int level, byte* message, byte* file, CULong line)
    {
        ILogger? logger = Volatile.Read(ref s_logger);
        if (logger is null)
            return;
        try
        {
            LogLevel mapped = level switch
            {
                <= 2 => LogLevel.Critical,
                3 => LogLevel.Error,
                4 => LogLevel.Warning,
                5 or 6 => LogLevel.Information,
                _ => LogLevel.Debug,
            };
            if (mapped < (LogLevel)Volatile.Read(ref s_minimumLevel) || !logger.IsEnabled(mapped))
                return;
            string text = (Marshal.PtrToStringUTF8((nint)message) ?? "").TrimEnd();
            logger.Log(mapped, "{Message}", text);
        }
        catch (Exception)
        {
            // A broken logger must not take the bus down with it.
        }
    }
}
