// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Interop;

internal static unsafe class EventMarshal
{
    /// Writes Type and the union only; Node and Flags are not ours to touch.
    /// Nothing is written unless the whole event is valid.
    internal static void ToNative(OsdpEvent ev, NativeEvent* dst)
    {
        NativeEvent staged = default;
        Stage(ev, &staged);
        dst->Type = staged.Type;
        dst->U = staged.U;
    }

    private static void Stage(OsdpEvent ev, NativeEvent* dst)
    {
        ArgumentNullException.ThrowIfNull(ev);
        NativeEventUnion* u = &dst->U;
        switch (ev)
        {
        case CardReadEvent e:
            dst->Type = NativeEventId.CardRead;
            u->CardRead.ReaderNo = e.ReaderNo;
            u->CardRead.Format = (int)e.Format;
            u->CardRead.Length = CardLength(e);
            Bytes.Fill(e.Data, u->CardRead.Data, NativeLimits.EventCardreadMaxDataLen, nameof(e.Data));
            break;
        case KeyPressEvent e:
            dst->Type = NativeEventId.KeyPress;
            u->KeyPress.ReaderNo = e.ReaderNo;
            u->KeyPress.Length = Bytes.Fill(e.Data, u->KeyPress.Data, NativeLimits.EventKeypressMaxDataLen, nameof(e.Data));
            break;
        case ManufacturerReplyEvent e:
            dst->Type = NativeEventId.MfgRep;
            u->MfgRep.VendorCode = e.VendorCode;
            u->MfgRep.Length = (byte)Bytes.Fill(e.Data, u->MfgRep.Data, NativeLimits.EventMfgrepMaxDataLen, nameof(e.Data));
            break;
        case ManufacturerStatusEvent e:
            dst->Type = NativeEventId.MfgStatR;
            u->MfgStat.Length = (byte)Bytes.Fill(e.Data, u->MfgStat.Data, NativeLimits.EventMfgstatMaxDataLen, nameof(e.Data));
            break;
        case ManufacturerErrorEvent e:
            dst->Type = NativeEventId.MfgErrR;
            u->MfgStat.Length = (byte)Bytes.Fill(e.Data, u->MfgStat.Data, NativeLimits.EventMfgstatMaxDataLen, nameof(e.Data));
            break;
        case BioReadEvent e:
            dst->Type = NativeEventId.BioReadR;
            u->BioReadR.Reader = e.Reader;
            u->BioReadR.Status = (int)e.Status;
            u->BioReadR.Type = (int)e.Type;
            u->BioReadR.Quality = e.Quality;
            u->BioReadR.Length = (ushort)Bytes.Fill(e.Template, u->BioReadR.Data,
                                                    NativeLimits.EventBioreadrMaxTemplateLen, nameof(e.Template));
            break;
        case BioMatchEvent e:
            dst->Type = NativeEventId.BioMatchR;
            u->BioMatchR.Reader = e.Reader;
            u->BioMatchR.Status = (int)e.Status;
            u->BioMatchR.Score = e.Score;
            break;
        case StatusEvent e:
            dst->Type = NativeEventId.Status;
            u->Status.Type = (int)e.Type;
            u->Status.NrEntries = Bytes.Fill(e.Report, u->Status.Report, NativeLimits.StatusReportMaxLen, nameof(e.Report));
            break;
        case PivDataEvent e:
            dst->Type = NativeEventId.PivDataR;
            FillPiv(u, e.Data);
            break;
        case GenAuthEvent e:
            dst->Type = NativeEventId.GenAuthR;
            FillPiv(u, e.Data);
            break;
        case CrAuthEvent e:
            dst->Type = NativeEventId.CrAuthR;
            FillPiv(u, e.Data);
            break;
        case TrsCardInfoEvent e:
            dst->Type = NativeEventId.Trs;
            u->Trs.Reply = NativeTrsReplyId.CardInfo;
            u->Trs.U.CardInfo.Reader = e.Reader;
            u->Trs.U.CardInfo.Protocol = (int)e.Protocol;
            u->Trs.U.CardInfo.CsnLength = (byte)Bytes.Fill(e.Csn, u->Trs.U.CardInfo.Csn,
                                                           NativeLimits.TrsCsnMaxLen, nameof(e.Csn));
            u->Trs.U.CardInfo.ProtocolDataLength = (byte)Bytes.Fill(e.ProtocolData, u->Trs.U.CardInfo.ProtocolData,
                                                                    NativeLimits.TrsProtocolDataMaxLen,
                                                                    nameof(e.ProtocolData));
            break;
        case TrsCardPresentEvent e:
            dst->Type = NativeEventId.Trs;
            u->Trs.Reply = NativeTrsReplyId.CardPresent;
            u->Trs.U.CardPresent.Reader = e.Reader;
            u->Trs.U.CardPresent.Status = (int)e.Status;
            break;
        case TrsCardDataEvent e:
            dst->Type = NativeEventId.Trs;
            u->Trs.Reply = NativeTrsReplyId.CardData;
            u->Trs.U.CardData.Reader = e.Reader;
            u->Trs.U.CardData.ReaderStatus = e.ReaderStatus;
            CommandMarshal.FillApdu(&u->Trs.U.CardData.Apdu, e.Apdu, nameof(e.Apdu));
            break;
        case TrsPinCompleteEvent e:
            dst->Type = NativeEventId.Trs;
            u->Trs.Reply = NativeTrsReplyId.PinComplete;
            u->Trs.U.PinComplete = new NativeTrsPinComplete
            {
                Reader = e.Reader, ReaderStatus = e.ReaderStatus, Tries = e.Tries,
            };
            break;
        case TrsErrorEvent e:
            dst->Type = NativeEventId.Trs;
            u->Trs.Reply = NativeTrsReplyId.Error;
            u->Trs.U.ErrorCode = e.Code;
            break;
        case NotificationEvent:
            throw new ArgumentException("Notifications are generated by LibOSDP and cannot be submitted.", nameof(ev));
        default:
            throw new ArgumentException($"{ev.GetType().Name} is not supported.", nameof(ev));
        }
    }

    /// Null for an event kind this binding does not know.
    internal static OsdpEvent? FromNative(NativeEvent* src)
    {
        NativeEventUnion* u = &src->U;
        return src->Type switch
        {
            NativeEventId.CardRead => CardFromNative(&u->CardRead),
            NativeEventId.KeyPress => new KeyPressEvent(u->KeyPress.ReaderNo,
                Bytes.Read(u->KeyPress.Data, u->KeyPress.Length, NativeLimits.EventKeypressMaxDataLen)),
            NativeEventId.MfgRep => new ManufacturerReplyEvent(u->MfgRep.VendorCode,
                Bytes.Read(u->MfgRep.Data, u->MfgRep.Length, NativeLimits.EventMfgrepMaxDataLen)),
            NativeEventId.MfgStatR => new ManufacturerStatusEvent(
                Bytes.Read(u->MfgStat.Data, u->MfgStat.Length, NativeLimits.EventMfgstatMaxDataLen)),
            NativeEventId.MfgErrR => new ManufacturerErrorEvent(
                Bytes.Read(u->MfgStat.Data, u->MfgStat.Length, NativeLimits.EventMfgstatMaxDataLen)),
            NativeEventId.BioReadR => new BioReadEvent(u->BioReadR.Reader, (BioStatus)u->BioReadR.Status,
                (BioType)u->BioReadR.Type, u->BioReadR.Quality,
                Bytes.Read(u->BioReadR.Data, u->BioReadR.Length, NativeLimits.EventBioreadrMaxTemplateLen)),
            NativeEventId.BioMatchR => new BioMatchEvent(u->BioMatchR.Reader, (BioStatus)u->BioMatchR.Status,
                                                         u->BioMatchR.Score),
            NativeEventId.Status => new StatusEvent((StatusReportType)u->Status.Type,
                Bytes.Read(u->Status.Report, u->Status.NrEntries, NativeLimits.StatusReportMaxLen)),
            NativeEventId.Notification => NotificationMarshal.FromNative(&u->Notif) is { } n
                ? new NotificationEvent(n) : null,
            NativeEventId.PivDataR => new PivDataEvent(ReadPiv(u)),
            NativeEventId.GenAuthR => new GenAuthEvent(ReadPiv(u)),
            NativeEventId.CrAuthR => new CrAuthEvent(ReadPiv(u)),
            NativeEventId.Trs => TrsFromNative(&u->Trs),
            _ => null,
        };
    }

    private static OsdpEvent? TrsFromNative(NativeTrsReply* t)
    {
        NativeTrsReplyUnion* r = &t->U;
        return t->Reply switch
        {
            NativeTrsReplyId.CardInfo => new TrsCardInfoEvent(r->CardInfo.Reader,
                (TrsCardProtocol)r->CardInfo.Protocol,
                Bytes.Read(r->CardInfo.Csn, r->CardInfo.CsnLength, NativeLimits.TrsCsnMaxLen),
                Bytes.Read(r->CardInfo.ProtocolData, r->CardInfo.ProtocolDataLength,
                           NativeLimits.TrsProtocolDataMaxLen)),
            NativeTrsReplyId.CardPresent => new TrsCardPresentEvent(r->CardPresent.Reader,
                (TrsCardStatus)r->CardPresent.Status),
            NativeTrsReplyId.CardData => new TrsCardDataEvent(r->CardData.Reader, r->CardData.ReaderStatus,
                CommandMarshal.ReadApdu(&r->CardData.Apdu)),
            NativeTrsReplyId.PinComplete => new TrsPinCompleteEvent(r->PinComplete.Reader,
                r->PinComplete.ReaderStatus, r->PinComplete.Tries),
            NativeTrsReplyId.Error => new TrsErrorEvent(r->ErrorCode),
            _ => null,
        };
    }

    /// Raw formats carry a bit count. LibOSDP does not support osdp_FMT, so an
    /// ASCII read is refused here with the cause named.
    private static ushort CardLength(CardReadEvent e)
    {
        ArgumentNullException.ThrowIfNull(e.Data, nameof(e.Data));
        if (e.Format == CardFormat.Ascii)
            throw new ArgumentException(
                "ASCII card data (osdp_FMT) is not supported by LibOSDP.", nameof(e.Format));
        int bits = e.Bits ?? e.Data.Length * 8;
        if (bits < 0 || (bits + 7) / 8 != e.Data.Length)
            throw new ArgumentException($"{bits} bits need {(bits + 7) / 8} bytes of Data, not {e.Data.Length}.", nameof(e.Bits));
        return (ushort)bits;
    }

    private static CardReadEvent CardFromNative(NativeEvtCardRead* c)
    {
        var format = (CardFormat)c->Format;
        if (format == CardFormat.Ascii)
            return new CardReadEvent(c->ReaderNo, format,
                Bytes.Read(c->Data, c->Length, NativeLimits.EventCardreadMaxDataLen), null);
        byte[] data = Bytes.Read(c->Data, (c->Length + 7) / 8, NativeLimits.EventCardreadMaxDataLen);
        return new CardReadEvent(c->ReaderNo, format, data, Math.Min((int)c->Length, data.Length * 8));
    }

    private static void FillPiv(NativeEventUnion* u, byte[] data) =>
        u->PivReply.Length = (ushort)Bytes.Fill(data, u->PivReply.Data, NativeLimits.PivDataMaxLen, "Data");

    private static byte[] ReadPiv(NativeEventUnion* u) =>
        Bytes.Read(u->PivReply.Data, u->PivReply.Length, NativeLimits.PivDataMaxLen);
}
