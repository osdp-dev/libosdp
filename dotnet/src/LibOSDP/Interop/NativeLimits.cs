// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Interop;

/// Mirrors the *_MAX_LEN defaults in osdp.h. AbiGuard catches a library built
/// with different values through the struct sizes they produce.
internal static class NativeLimits
{
    internal const int StatusReportMaxLen = 64;
    internal const int CmdTextMaxLen = 32;
    internal const int CmdKeysetKeyMaxLen = 32;
    internal const int CmdMfgMaxDataLen = 64;
    internal const int CmdBiomatchMaxTemplateLen = 128;
    internal const int PivDataMaxLen = 256;
    internal const int EventCardreadMaxDataLen = 64;
    internal const int EventKeypressMaxDataLen = 64;
    internal const int EventMfgrepMaxDataLen = 128;
    internal const int EventMfgstatMaxDataLen = 128;
    internal const int EventBioreadrMaxTemplateLen = 256;

    // The unions hold no pointers and nothing wider than 4-byte alignment, so
    // their size is the same on every target. Measured on armv7, where struct
    // osdp_cmd has no tail padding: sizeof (320) minus the 16-byte header. On
    // 64-bit targets only the header differs (24 bytes), giving 328.
    internal const int CmdUnionSize = 304;
    internal const int EventUnionSize = 272;

    internal const int CapSentinel = 17; // OSDP_PD_CAP_SENTINEL
}

internal static class NativeCmdId
{
    internal const int Output = 1, Led = 2, Buzzer = 3, Text = 4, Keyset = 5,
        Comset = 6, Mfg = 7, FileTx = 8, Status = 9, ComsetDone = 10,
        Notification = 11, BioRead = 12, BioMatch = 13, TdSet = 14,
        PivData = 15, GenAuth = 16, CrAuth = 17;
}

internal static class NativeEventId
{
    internal const int CardRead = 1, KeyPress = 2, MfgRep = 3, Status = 4,
        Notification = 5, MfgStatR = 6, MfgErrR = 7, BioReadR = 8,
        BioMatchR = 9, PivDataR = 10, GenAuthR = 11, CrAuthR = 12;
}
