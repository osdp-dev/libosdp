// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

using System.Text;

namespace LibOSDP.Interop;

internal static unsafe class CommandMarshal
{
    /// Writes Id and the union only. Node belongs to LibOSDP's queue and Flags
    /// to whoever allocated the struct; a PD answer is written over a command
    /// LibOSDP still holds.
    internal static void ToNative(OsdpCommand command, NativeCmd* dst)
    {
        NativeCmd staged = default;
        Stage(command, &staged);
        dst->Id = staged.Id;
        dst->U = staged.U;
    }

    private static void Stage(OsdpCommand command, NativeCmd* dst)
    {
        ArgumentNullException.ThrowIfNull(command);
        NativeCmdUnion* u = &dst->U;
        switch (command)
        {
        case OutputCommand c:
            dst->Id = NativeCmdId.Output;
            u->Output.OutputNo = c.OutputNo;
            u->Output.ControlCode = (int)c.ControlCode;
            u->Output.TimerCount = c.TimerCount;
            break;
        case LedCommand c:
            dst->Id = NativeCmdId.Led;
            u->Led.Reader = c.Reader;
            u->Led.LedNumber = c.LedNumber;
            if (c.Temporary is { } t)
            {
                RequireDefined(t.ControlCode);
                u->Led.Temporary = new NativeCmdLedParams
                {
                    ControlCode = (byte)t.ControlCode, OnCount = t.OnCount, OffCount = t.OffCount,
                    OnColor = (int)t.OnColor, OffColor = (int)t.OffColor, TimerCount = t.TimerCount,
                };
            }
            if (c.Permanent is { } p)
            {
                RequireDefined(p.ControlCode);
                u->Led.Permanent = new NativeCmdLedParams
                {
                    ControlCode = (byte)p.ControlCode, OnCount = p.OnCount, OffCount = p.OffCount,
                    OnColor = (int)p.OnColor, OffColor = (int)p.OffColor,
                };
            }
            break;
        case BuzzerCommand c:
            dst->Id = NativeCmdId.Buzzer;
            u->Buzzer.Reader = c.Reader;
            u->Buzzer.ControlCode = (int)c.ControlCode;
            u->Buzzer.OnCount = c.OnCount;
            u->Buzzer.OffCount = c.OffCount;
            u->Buzzer.RepCount = c.RepCount;
            break;
        case TextCommand c:
            dst->Id = NativeCmdId.Text;
            u->Text.Reader = c.Reader;
            u->Text.ControlCode = (int)c.ControlCode;
            u->Text.TempTime = c.TempTime;
            u->Text.OffsetRow = c.OffsetRow;
            u->Text.OffsetCol = c.OffsetCol;
            u->Text.Length = (byte)Bytes.Fill(Bytes.Ascii(c.Data, nameof(c.Data)), u->Text.Data,
                                              NativeLimits.CmdTextMaxLen, nameof(c.Data));
            break;
        case KeysetCommand c:
            dst->Id = NativeCmdId.Keyset;
            u->Keyset.Type = c.Type;
            u->Keyset.Length = (byte)Bytes.Fill(c.Data, u->Keyset.Data, NativeLimits.CmdKeysetKeyMaxLen, nameof(c.Data));
            break;
        case ComsetCommand c:
            dst->Id = NativeCmdId.Comset;
            u->Comset.Address = c.Address;
            u->Comset.BaudRate = c.BaudRate;
            break;
        case ComsetDoneCommand c:
            dst->Id = NativeCmdId.ComsetDone;
            u->Comset.Address = c.Address;
            u->Comset.BaudRate = c.BaudRate;
            break;
        case ManufacturerCommand c:
            dst->Id = NativeCmdId.Mfg;
            u->Mfg.VendorCode = c.VendorCode;
            u->Mfg.Length = (byte)Bytes.Fill(c.Data, u->Mfg.Data, NativeLimits.CmdMfgMaxDataLen, nameof(c.Data));
            break;
        case FileTransferCommand c:
            dst->Id = NativeCmdId.FileTx;
            u->FileTx.Id = c.FileId;
            u->FileTx.Flags = c.Flags;
            break;
        case StatusCommand c:
            dst->Id = NativeCmdId.Status;
            u->Status.Type = (int)c.Type;
            u->Status.NrEntries = Bytes.Fill(c.Report, u->Status.Report, NativeLimits.StatusReportMaxLen, nameof(c.Report));
            break;
        case BioReadCommand c:
            dst->Id = NativeCmdId.BioRead;
            u->BioRead.Reader = c.Reader;
            u->BioRead.Type = (int)c.Type;
            u->BioRead.Format = (int)c.Format;
            u->BioRead.Quality = c.Quality;
            break;
        case BioMatchCommand c:
            dst->Id = NativeCmdId.BioMatch;
            u->BioMatch.Reader = c.Reader;
            u->BioMatch.Type = (int)c.Type;
            u->BioMatch.Format = (int)c.Format;
            u->BioMatch.Quality = c.Quality;
            u->BioMatch.Length = (ushort)Bytes.Fill(c.Template, u->BioMatch.Data,
                                                    NativeLimits.CmdBiomatchMaxTemplateLen, nameof(c.Template));
            break;
        case TdSetCommand c:
            dst->Id = NativeCmdId.TdSet;
            u->TdSet = new NativeCmdTdSet
            {
                Year = c.Year, Month = c.Month, Day = c.Day, Hour = c.Hour, Minute = c.Minute, Second = c.Second,
            };
            break;
        case PivDataCommand c:
            dst->Id = NativeCmdId.PivData;
            if (c.ObjectId is not { Length: 3 })
                throw new ArgumentException("ObjectId must be exactly 3 bytes.", nameof(c.ObjectId));
            Bytes.Fill(c.ObjectId, u->PivData.Oid, 3, nameof(c.ObjectId));
            u->PivData.Element = c.Element;
            u->PivData.Offset = c.Offset;
            break;
        case GenAuthCommand c:
            dst->Id = NativeCmdId.GenAuth;
            FillAuth(&u->Auth, c.Algorithm, c.Key, c.Data);
            break;
        case CrAuthCommand c:
            dst->Id = NativeCmdId.CrAuth;
            FillAuth(&u->Auth, c.Algorithm, c.Key, c.Data);
            break;
        case TrsStartCommand c:
            StageTrs(dst, NativeTrsCmdId.Start, c.Reader);
            break;
        case TrsStopCommand c:
            StageTrs(dst, NativeTrsCmdId.Stop, c.Reader);
            break;
        case TrsSendApduCommand c:
            StageTrs(dst, NativeTrsCmdId.SendApdu, c.Reader);
            FillApdu(&u->Trs.U.Apdu, c.Apdu, nameof(c.Apdu));
            break;
        case TrsEnterPinCommand c:
            StageTrs(dst, NativeTrsCmdId.EnterPin, c.Reader);
            FillPinEntry(&u->Trs.U.PinEntry, c);
            break;
        case TrsCardScanCommand c:
            StageTrs(dst, NativeTrsCmdId.CardScan, c.Reader);
            break;
        case NotificationCommand:
            throw new ArgumentException("Notifications are generated by LibOSDP and cannot be submitted.", nameof(command));
        default:
            throw new ArgumentException($"{command.GetType().Name} is not supported.", nameof(command));
        }
    }

    /// Null for a command kind this binding does not know.
    internal static OsdpCommand? FromNative(NativeCmd* src)
    {
        NativeCmdUnion* u = &src->U;
        return src->Id switch
        {
            NativeCmdId.Output => new OutputCommand(u->Output.OutputNo, (OutputControlCode)u->Output.ControlCode,
                                                    u->Output.TimerCount),
            NativeCmdId.Led => new LedCommand(u->Led.Reader, u->Led.LedNumber,
                u->Led.Temporary.ControlCode == 0 ? null : new TemporaryLedParams(
                    (TemporaryLedControlCode)u->Led.Temporary.ControlCode, u->Led.Temporary.OnCount,
                    u->Led.Temporary.OffCount, (LedColor)u->Led.Temporary.OnColor,
                    (LedColor)u->Led.Temporary.OffColor, u->Led.Temporary.TimerCount),
                u->Led.Permanent.ControlCode == 0 ? null : new PermanentLedParams(
                    (PermanentLedControlCode)u->Led.Permanent.ControlCode, u->Led.Permanent.OnCount,
                    u->Led.Permanent.OffCount, (LedColor)u->Led.Permanent.OnColor,
                    (LedColor)u->Led.Permanent.OffColor)),
            NativeCmdId.Buzzer => new BuzzerCommand(u->Buzzer.Reader, (BuzzerControlCode)u->Buzzer.ControlCode,
                                                    u->Buzzer.OnCount, u->Buzzer.OffCount, u->Buzzer.RepCount),
            NativeCmdId.Text => new TextCommand(u->Text.Reader, (TextControlCode)u->Text.ControlCode,
                u->Text.TempTime, u->Text.OffsetRow, u->Text.OffsetCol,
                Encoding.ASCII.GetString(Bytes.Read(u->Text.Data, u->Text.Length, NativeLimits.CmdTextMaxLen))),
            NativeCmdId.Keyset => new KeysetCommand(u->Keyset.Type,
                Bytes.Read(u->Keyset.Data, u->Keyset.Length, NativeLimits.CmdKeysetKeyMaxLen)),
            NativeCmdId.Comset => new ComsetCommand(u->Comset.Address, u->Comset.BaudRate),
            NativeCmdId.ComsetDone => new ComsetDoneCommand(u->Comset.Address, u->Comset.BaudRate),
            NativeCmdId.Mfg => new ManufacturerCommand(u->Mfg.VendorCode,
                Bytes.Read(u->Mfg.Data, u->Mfg.Length, NativeLimits.CmdMfgMaxDataLen)),
            NativeCmdId.FileTx => new FileTransferCommand(u->FileTx.Id, u->FileTx.Flags),
            NativeCmdId.Status => new StatusCommand((StatusReportType)u->Status.Type,
                Bytes.Read(u->Status.Report, u->Status.NrEntries, NativeLimits.StatusReportMaxLen)),
            NativeCmdId.Notification => NotificationMarshal.FromNative(&u->Notif) is { } n
                ? new NotificationCommand(n) : null,
            NativeCmdId.BioRead => new BioReadCommand(u->BioRead.Reader, (BioType)u->BioRead.Type,
                                                      (BioFormat)u->BioRead.Format, u->BioRead.Quality),
            NativeCmdId.BioMatch => new BioMatchCommand(u->BioMatch.Reader, (BioType)u->BioMatch.Type,
                (BioFormat)u->BioMatch.Format, u->BioMatch.Quality,
                Bytes.Read(u->BioMatch.Data, u->BioMatch.Length, NativeLimits.CmdBiomatchMaxTemplateLen)),
            NativeCmdId.TdSet => new TdSetCommand(u->TdSet.Year, u->TdSet.Month, u->TdSet.Day,
                                                  u->TdSet.Hour, u->TdSet.Minute, u->TdSet.Second),
            NativeCmdId.PivData => new PivDataCommand(Bytes.Read(u->PivData.Oid, 3, 3),
                                                      u->PivData.Element, u->PivData.Offset),
            NativeCmdId.GenAuth => new GenAuthCommand(u->Auth.Algorithm, u->Auth.Key,
                Bytes.Read(u->Auth.Data, u->Auth.Length, NativeLimits.PivDataMaxLen)),
            NativeCmdId.CrAuth => new CrAuthCommand(u->Auth.Algorithm, u->Auth.Key,
                Bytes.Read(u->Auth.Data, u->Auth.Length, NativeLimits.PivDataMaxLen)),
            NativeCmdId.Xwr => TrsFromNative(&u->Trs),
            _ => null,
        };
    }

    private static OsdpCommand? TrsFromNative(NativeCmdTrs* t) => t->Command switch
    {
        NativeTrsCmdId.Start => new TrsStartCommand(t->Reader),
        NativeTrsCmdId.Stop => new TrsStopCommand(t->Reader),
        NativeTrsCmdId.SendApdu => new TrsSendApduCommand(t->Reader, ReadApdu(&t->U.Apdu)),
        NativeTrsCmdId.EnterPin => PinEntryFromNative(t->Reader, &t->U.PinEntry),
        NativeTrsCmdId.CardScan => new TrsCardScanCommand(t->Reader),
        _ => null,
    };

    private static TrsEnterPinCommand PinEntryFromNative(byte reader, NativeTrsPinEntry* p) =>
        new(reader, p->TimeoutInitial, p->TimeoutDigit,
            new TrsPinBlock((TrsPinFormat)p->PinBlock.Format, p->PinBlock.RightJustify != 0,
                            p->PinBlock.OffsetBits, p->PinBlock.SizeBytes),
            new TrsPinLengthField(p->PinLengthField.SizeBits, p->PinLengthField.OffsetBits),
            p->MinDigits, p->MaxDigits, (TrsPinComplete)p->CompleteOn, p->NumMessages, p->LanguageId,
            p->MsgIndex, Bytes.Read(p->TeoPrologue, 3, 3), ReadApdu(&p->Apdu));

    private static void StageTrs(NativeCmd* dst, int command, byte reader)
    {
        dst->Id = NativeCmdId.Xwr;
        dst->U.Trs.Command = command;
        dst->U.Trs.Reader = reader;
    }

    private static void FillPinEntry(NativeTrsPinEntry* p, TrsEnterPinCommand c)
    {
        ArgumentNullException.ThrowIfNull(c.PinBlock, nameof(c.PinBlock));
        ArgumentNullException.ThrowIfNull(c.PinLengthField, nameof(c.PinLengthField));
        if (c.TeoPrologue is not { Length: 3 })
            throw new ArgumentException("TeoPrologue must be exactly 3 bytes.", nameof(c.TeoPrologue));
        p->TimeoutInitial = c.TimeoutInitial;
        p->TimeoutDigit = c.TimeoutDigit;
        p->PinBlock = new NativeTrsPinBlock
        {
            Format = (int)c.PinBlock.Format, RightJustify = (byte)(c.PinBlock.RightJustify ? 1 : 0),
            OffsetBits = c.PinBlock.OffsetBits, SizeBytes = c.PinBlock.SizeBytes,
        };
        p->PinLengthField = new NativeTrsPinLengthField
        {
            SizeBits = c.PinLengthField.SizeBits, OffsetBits = c.PinLengthField.OffsetBits,
        };
        p->MinDigits = c.MinDigits;
        p->MaxDigits = c.MaxDigits;
        p->CompleteOn = (uint)c.CompleteOn;
        p->NumMessages = c.NumMessages;
        p->LanguageId = c.LanguageId;
        p->MsgIndex = c.MsgIndex;
        Bytes.Fill(c.TeoPrologue, p->TeoPrologue, 3, nameof(c.TeoPrologue));
        FillApdu(&p->Apdu, c.Apdu, nameof(c.Apdu));
    }

    internal static void FillApdu(NativeTrsApdu* apdu, byte[] data, string paramName) =>
        apdu->Length = (ushort)Bytes.Fill(data, apdu->Data, NativeLimits.TrsApduMaxLen, paramName);

    internal static byte[] ReadApdu(NativeTrsApdu* apdu) =>
        Bytes.Read(apdu->Data, apdu->Length, NativeLimits.TrsApduMaxLen);

    private static void RequireDefined<T>(T controlCode) where T : struct, Enum
    {
        if (!Enum.IsDefined(controlCode))
            throw new ArgumentException($"{controlCode} is not a valid control code.", "ControlCode");
    }

    private static void FillAuth(NativeCmdAuth* auth, byte algorithm, byte key, byte[] data)
    {
        auth->Algorithm = algorithm;
        auth->Key = key;
        auth->Length = (ushort)Bytes.Fill(data, auth->Data, NativeLimits.PivDataMaxLen, "Data");
    }
}
