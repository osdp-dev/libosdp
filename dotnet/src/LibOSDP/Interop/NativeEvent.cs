// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeEvtCardRead
{
    public int ReaderNo;
    public int Format;
    public int Direction;
    public ushort Length;
    public fixed byte Data[NativeLimits.EventCardreadMaxDataLen];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeEvtKeyPress
{
    public int ReaderNo;
    public int Length;
    public fixed byte Data[NativeLimits.EventKeypressMaxDataLen];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeEvtMfgRep
{
    public uint VendorCode;
    public byte Length;
    public fixed byte Data[NativeLimits.EventMfgrepMaxDataLen];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeEvtMfgStat
{
    public byte Length;
    public fixed byte Data[NativeLimits.EventMfgstatMaxDataLen];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeEvtBioReadR
{
    public byte Reader;
    public int Status;
    public int Type;
    public byte Quality;
    public ushort Length;
    public fixed byte Data[NativeLimits.EventBioreadrMaxTemplateLen];
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeEvtBioMatchR
{
    public byte Reader;
    public int Status;
    public byte Score;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeEvtPivReply
{
    public ushort Length;
    public fixed byte Data[NativeLimits.PivDataMaxLen];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeTrsCardInfo
{
    public byte Reader;
    public int Protocol;
    public byte CsnLength;
    public fixed byte Csn[NativeLimits.TrsCsnMaxLen];
    public byte ProtocolDataLength;
    public fixed byte ProtocolData[NativeLimits.TrsProtocolDataMaxLen];
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTrsCardPresent
{
    public byte Reader;
    public int Status;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTrsCardData
{
    public byte Reader;
    public byte ReaderStatus;
    public NativeTrsApdu Apdu;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTrsPinComplete
{
    public byte Reader;
    public byte ReaderStatus;
    public byte Tries;
}

[StructLayout(LayoutKind.Explicit)]
internal struct NativeTrsReplyUnion
{
    [FieldOffset(0)] public NativeTrsCardInfo CardInfo;
    [FieldOffset(0)] public NativeTrsCardPresent CardPresent;
    [FieldOffset(0)] public NativeTrsCardData CardData;
    [FieldOffset(0)] public NativeTrsPinComplete PinComplete;
    [FieldOffset(0)] public byte ErrorCode;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeTrsReply
{
    public int Reply;
    public NativeTrsReplyUnion U;
}

[StructLayout(LayoutKind.Explicit, Size = NativeLimits.EventUnionSize)]
internal struct NativeEventUnion
{
    [FieldOffset(0)] public NativeEvtKeyPress KeyPress;
    [FieldOffset(0)] public NativeEvtCardRead CardRead;
    [FieldOffset(0)] public NativeEvtMfgRep MfgRep;
    [FieldOffset(0)] public NativeEvtMfgStat MfgStat;
    [FieldOffset(0)] public NativeEvtBioReadR BioReadR;
    [FieldOffset(0)] public NativeEvtBioMatchR BioMatchR;
    [FieldOffset(0)] public NativeEvtPivReply PivReply;
    [FieldOffset(0)] public NativeStatusReport Status;
    [FieldOffset(0)] public NativeNotification Notif;
    [FieldOffset(0)] public NativeTrsReply Trs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeEvent
{
    public NativeQueueNode Node;
    public int Type;
    public uint Flags;
    public NativeEventUnion U;
}
