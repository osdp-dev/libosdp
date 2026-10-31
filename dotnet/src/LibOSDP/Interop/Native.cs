// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

internal static unsafe partial class Native
{
    private const string Library = "osdp";

    [LibraryImport(Library, EntryPoint = "osdp_get_abi_info")]
    internal static partial void GetAbiInfo(AbiInfo* info);

    [LibraryImport(Library, EntryPoint = "osdp_get_version")]
    internal static partial byte* GetVersion();

    [LibraryImport(Library, EntryPoint = "osdp_get_source_info")]
    internal static partial byte* GetSourceInfo();

    [LibraryImport(Library, EntryPoint = "osdp_logger_init")]
    internal static partial void LoggerInit(byte* name, int logLevel, nint putsFn);

    [LibraryImport(Library, EntryPoint = "osdp_set_log_callback")]
    internal static partial void SetLogCallback(delegate* unmanaged<int, int, byte*, byte*, CULong, void> callback);

    [LibraryImport(Library, EntryPoint = "osdp_get_status_mask")]
    internal static partial void GetStatusMask(nint ctx, byte* bitmask);

    [LibraryImport(Library, EntryPoint = "osdp_get_sc_status_mask")]
    internal static partial void GetScStatusMask(nint ctx, byte* bitmask);

    [LibraryImport(Library, EntryPoint = "osdp_get_metrics")]
    internal static partial int GetMetrics(nint ctx, int pd, NativeMetrics* metrics);

    [LibraryImport(Library, EntryPoint = "osdp_file_register_ops")]
    internal static partial int FileRegisterOps(nint ctx, int pd, NativeFileOps* ops);

    [LibraryImport(Library, EntryPoint = "osdp_get_file_tx_status")]
    internal static partial int GetFileTxStatus(nint ctx, int pd, uint* size, uint* offset);

    [LibraryImport(Library, EntryPoint = "osdp_cp_setup")]
    internal static partial nint CpSetup(NativeChannel* channel, int numPd, NativePdInfo* info);

    [LibraryImport(Library, EntryPoint = "osdp_cp_refresh")]
    internal static partial void CpRefresh(nint ctx);

    [LibraryImport(Library, EntryPoint = "osdp_cp_teardown")]
    internal static partial void CpTeardown(nint ctx);

    [LibraryImport(Library, EntryPoint = "osdp_cp_submit_command")]
    internal static partial int CpSubmitCommand(nint ctx, int pd, NativeCmd* cmd);

    [LibraryImport(Library, EntryPoint = "osdp_cp_cancel")]
    internal static partial int CpCancel(nint ctx, int pd, int what);

    [LibraryImport(Library, EntryPoint = "osdp_cp_flush_commands")]
    internal static partial int CpFlushCommands(nint ctx, int pd);

    [LibraryImport(Library, EntryPoint = "osdp_cp_get_pd_id")]
    internal static partial int CpGetPdId(nint ctx, int pd, NativePdId* id);

    [LibraryImport(Library, EntryPoint = "osdp_cp_get_capability")]
    internal static partial int CpGetCapability(nint ctx, int pd, NativePdCap* cap);

    [LibraryImport(Library, EntryPoint = "osdp_cp_set_event_callback")]
    internal static partial int CpSetEventCallback(nint ctx, delegate* unmanaged<nint, int, NativeEvent*, int> callback, nint arg);

    [LibraryImport(Library, EntryPoint = "osdp_cp_set_command_completion_callback")]
    internal static partial int CpSetCommandCompletionCallback(nint ctx, delegate* unmanaged<nint, int, NativeCmd*, int, void> callback, nint arg);

    [LibraryImport(Library, EntryPoint = "osdp_cp_modify_flag")]
    internal static partial int CpModifyFlag(nint ctx, int pd, uint flags, byte doSet);

    [LibraryImport(Library, EntryPoint = "osdp_cp_disable_pd")]
    internal static partial int CpDisablePd(nint ctx, int pd);

    [LibraryImport(Library, EntryPoint = "osdp_cp_enable_pd")]
    internal static partial int CpEnablePd(nint ctx, int pd);

    [LibraryImport(Library, EntryPoint = "osdp_cp_is_pd_enabled")]
    internal static partial byte CpIsPdEnabled(nint ctx, int pd);

    [LibraryImport(Library, EntryPoint = "osdp_cp_set_scbk")]
    internal static partial int CpSetScbk(nint ctx, int pd, byte* scbk);

    [LibraryImport(Library, EntryPoint = "osdp_cp_trs_scan_enable")]
    internal static partial int CpTrsScanEnable(nint ctx, int pd, NativeTrsScanParams* parameters);

    [LibraryImport(Library, EntryPoint = "osdp_cp_trs_scan_disable")]
    internal static partial int CpTrsScanDisable(nint ctx, int pd);

    [LibraryImport(Library, EntryPoint = "osdp_cp_trs_get_max_apdu_len")]
    internal static partial int CpTrsGetMaxApduLen(nint ctx, int pd);

    [LibraryImport(Library, EntryPoint = "osdp_pd_trs_get_max_apdu_len")]
    internal static partial int PdTrsGetMaxApduLen(nint ctx);

    [LibraryImport(Library, EntryPoint = "osdp_pd_setup")]
    internal static partial nint PdSetup(NativeChannel* channel, NativePdInfo* info);

    [LibraryImport(Library, EntryPoint = "osdp_pd_refresh")]
    internal static partial void PdRefresh(nint ctx);

    [LibraryImport(Library, EntryPoint = "osdp_pd_teardown")]
    internal static partial void PdTeardown(nint ctx);

    [LibraryImport(Library, EntryPoint = "osdp_pd_set_command_callback")]
    internal static partial int PdSetCommandCallback(nint ctx, delegate* unmanaged<nint, NativeCmd*, int> callback, nint arg);

    [LibraryImport(Library, EntryPoint = "osdp_pd_set_event_completion_callback")]
    internal static partial int PdSetEventCompletionCallback(nint ctx, delegate* unmanaged<nint, NativeEvent*, int, void> callback, nint arg);

    [LibraryImport(Library, EntryPoint = "osdp_pd_submit_event")]
    internal static partial int PdSubmitEvent(nint ctx, NativeEvent* ev);

    [LibraryImport(Library, EntryPoint = "osdp_pd_flush_events")]
    internal static partial int PdFlushEvents(nint ctx);
}
