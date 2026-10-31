// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

[StructLayout(LayoutKind.Sequential)]
internal record struct AbiInfo
{
    public uint Size;
    public uint Options;
    public uint SizeofCmd;
    public uint SizeofEvent;
    public uint SizeofChannel;
    public uint SizeofPdInfo;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeQueueNode
{
    public nint Next;
    public nint Prev;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeChannel
{
    public nint Data;
    public delegate* unmanaged<nint, byte*, int, int> Recv;
    public delegate* unmanaged<nint, byte*, int, int> Send;
    public delegate* unmanaged<nint, void> Flush;
    public delegate* unmanaged<nint, void> Close;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativePdId
{
    public int Version;
    public int Model;
    public uint VendorCode;
    public uint SerialNumber;
    public uint FirmwareVersion;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativePdCap
{
    public int FunctionCode;
    public byte ComplianceLevel;
    public byte NumItems;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativePdInfo
{
    public byte* Name;
    public uint BaudRate;
    public int Address;
    public uint Flags;
    public NativePdId Id;
    public NativePdCap* Cap;
    public byte* Scbk;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeMetrics
{
    public uint PacketsSent;
    public uint PacketsReceived;
    public uint PacketCheckErrors;
    public uint NakCount;
    public uint ScHandshakeCount;
    public uint ScFailureCount;
    public uint CommandCount;
    public uint EventCount;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeFileOps
{
    public nint Arg;
    public delegate* unmanaged<nint, int, uint*, int> Open;
    public delegate* unmanaged<nint, void*, uint, uint, int> Read;
    public delegate* unmanaged<nint, void*, uint, uint, int> Write;
    public delegate* unmanaged<nint, int> Close;
    public delegate* unmanaged<nint, int*, ushort*, byte> Finalize;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeScStatus
{
    public byte Active;
    public byte ScbkD;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeMpNotification
{
    public int MpType;
    public int ObjectId;
    public uint Total;
    public uint Offset;
    public int Outcome;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTrsStatus
{
    public int Status;
    public byte Reader;
    public int Reason;
    public byte NakCode;
}

[StructLayout(LayoutKind.Explicit)]
internal struct NativeNotificationData
{
    [FieldOffset(0)] public NativeScStatus ScStatus;
    [FieldOffset(0)] public byte PdOnline;
    [FieldOffset(0)] public NativeMpNotification Mp;
    [FieldOffset(0)] public NativePdId PdId;
    [FieldOffset(0)] public NativeTrsStatus TrsStatus;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTrsScanParams
{
    public ushort Mode0DwellMs;
    public ushort Mode1DwellMs;
    public ushort HoldMs;
    public byte Reader;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeNotification
{
    public int Type;
    public NativeNotificationData Data;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeStatusReport
{
    public int Type;
    public int NrEntries;
    public fixed byte Report[NativeLimits.StatusReportMaxLen];
}
