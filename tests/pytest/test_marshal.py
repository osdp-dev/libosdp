#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Round-trip and validation checks for the typed command/event layer.

These need no channel, no threads and no peer: they exercise the conversion
between the dataclasses and the dicts the C extension speaks. That makes them
the fastest place to catch a schema drift, long before an integration test
would notice.
"""

import pytest

from osdp import commands, events
from osdp._marshal import (
    decode_command,
    decode_event,
    encode_command,
    encode_event,
)
from osdp.enums import (
    BioFormat,
    BioStatus,
    BioType,
    BuzzerControlCode,
    CardFormat,
    CommandId,
    EventId,
    LEDColor,
    NotificationType,
    OutputControlCode,
    PermanentLEDControlCode,
    StatusReportType,
    TemporaryLEDControlCode,
    TextControlCode,
    TrsCardProtocol,
    TrsCardStatus,
    TrsPinComplete,
    TrsPinFormat,
    TrsSessionStatus,
)
from osdp.errors import MarshalError

ALL_COMMANDS = [
    commands.Output(
        output_no=1,
        control_code=OutputControlCode.TemporaryOn,
        timer_count=300,
    ),
    # Temporary block only.
    commands.LED(
        led_number=0,
        temporary=commands.TemporaryLEDParams(
            on_color=LEDColor.Red,
            off_color=LEDColor.Black,
            on_count=5,
            off_count=5,
            timer_count=1000,
        ),
    ),
    # Permanent block only.
    commands.LED(
        led_number=1,
        permanent=commands.PermanentLEDParams(
            on_color=LEDColor.Green, on_count=1
        ),
    ),
    # Both blocks: cancel the temporary and set a new permanent. This could not
    # be expressed at all under the old flattened dict.
    commands.LED(
        led_number=0,
        temporary=commands.TemporaryLEDParams(
            control_code=TemporaryLEDControlCode.Cancel
        ),
        permanent=commands.PermanentLEDParams(
            on_color=LEDColor.Amber, on_count=1
        ),
    ),
    commands.Buzzer(
        control_code=BuzzerControlCode.DefaultTone,
        on_count=2,
        off_count=2,
        rep_count=3,
    ),
    commands.Text(
        control_code=TextControlCode.PermanentNoWrap,
        offset_row=1,
        offset_col=1,
        data="HELLO",
    ),
    commands.TDSet(year=2026, month=7, day=17, hour=12, minute=30, second=45),
    commands.Keyset(type=1, data=bytes(range(16))),
    commands.Comset(address=42, baud_rate=115200),
    commands.ComsetDone(address=42, baud_rate=115200),
    commands.Manufacturer(vendor_code=0x00030201, data=b"\x01\x02\x03"),
    commands.BioRead(
        type=BioType.RightThumbPrint,
        format=BioFormat.AnsiIncits378,
        quality=200,
    ),
    commands.BioMatch(
        type=BioType.LeftIrisScan,
        format=BioFormat.RawPGM,
        quality=100,
        data=b"template-bytes",
    ),
    commands.FileTransfer(id=1),
    commands.Status(type=StatusReportType.Input),
    commands.Status(type=StatusReportType.Input, report=bytes([0, 1, 0, 1])),
    commands.Notification(
        type=NotificationType.MultipartDone, mp_type=1, object_id=1,
        total=3200, offset=3200, outcome=0,
    ),
    commands.PivData(oid=b"\x5f\xc1\x02", element=7, offset=1),
    commands.GenAuth(algorithm=0xA7, key=0x9E, data=b"challenge-bytes"),
    commands.GenAuth(
        algorithm=1, key=2, data=bytes(range(256))[:commands.MAX_PIV_DATA_LEN]
    ),
    commands.CrAuth(algorithm=0x11, key=0x22, data=b"\x00\x01\x02"),
    commands.Notification(
        type=NotificationType.TrsStatus,
        status=int(TrsSessionStatus.CardReleased),
    ),
    commands.TrsStart(),
    commands.TrsStop(),
    commands.TrsCardScan(),
    commands.TrsSendApdu(apdu=bytes([0x00, 0xA4, 0x04, 0x00, 0x0E])),
    commands.TrsSendApdu(apdu=bytes(commands.MAX_TRS_APDU_LEN)),
    commands.TrsEnterPin(
        timeout_initial=30,
        timeout_digit=5,
        pin_block=commands.TrsPinBlock(
            format=TrsPinFormat.BCD, right_justify=True, offset_bits=40,
            size_bytes=8,
        ),
        pin_length_field=commands.TrsPinLengthField(
            size_bits=4, offset_bits=36
        ),
        min_digits=4,
        max_digits=12,
        complete_on=TrsPinComplete.Key | TrsPinComplete.Timeout,
        num_messages=1,
        language_id=0x0409,
        msg_index=2,
        teo_prologue=b"\x00\x40\x0d",
        apdu=bytes([0x00, 0x20, 0x00, 0x80, 0x08]) + bytes(8),
    ),
]

ALL_EVENTS = [
    events.CardRead(
        format=CardFormat.Wiegand,
        direction=0,
        data=bytes([0x01, 0x02, 0x03, 0x40]),
        bits=26,
    ),
    events.CardRead(format=CardFormat.Unspecified, data=b"\x01\x02"),
    events.CardRead(format=CardFormat.ASCII, data=b"1234567890"),
    events.KeyPress(data=b"1234"),
    events.ManufacturerReply(vendor_code=0x00030201, data=b"\x01\x02"),
    events.ManufacturerStatus(data=b"\x01\x02"),
    events.ManufacturerStatus(data=b""),
    events.ManufacturerError(data=b"\xff"),
    events.ManufacturerError(data=b""),
    events.BioRead(
        status=BioStatus.Success,
        type=BioType.RightThumbPrint,
        quality=200,
        data=b"template",
    ),
    events.BioRead(status=BioStatus.Timeout),
    events.BioMatch(status=BioStatus.Success, score=250),
    events.Status(type=StatusReportType.Output, report=bytes([1, 0, 1])),
    events.Notification(
        type=NotificationType.PeripheralDeviceStatus, online=True
    ),
    events.Notification(
        type=NotificationType.MultipartStart, mp_type=1, object_id=1,
        total=3200,
    ),
    events.Notification(
        type=NotificationType.MultipartDone, mp_type=1, object_id=1,
        total=3200, offset=3200, outcome=0,
    ),
    events.PivData(data=b"piv-object-contents"),
    events.GenAuth(data=b"auth-response"),
    events.CrAuth(data=b"challenge-response"),
    events.Notification(
        type=NotificationType.TrsStatus,
        status=int(TrsSessionStatus.Opened),
    ),
    events.TrsCardInfo(
        reader=1,
        protocol=TrsCardProtocol.Contact,
        csn=bytes([0x04, 0xA2, 0x2B, 0x7C]),
        protocol_data=bytes([0x3B, 0x8F, 0x80, 0x01]),
    ),
    events.TrsCardInfo(),
    events.TrsCardPresent(status=TrsCardStatus.PresentContactless),
    events.TrsCardPresent(status=TrsCardStatus.NotPresent),
    events.TrsCardData(status=1, apdu=bytes([0x6F, 0x1A, 0x90, 0x00])),
    events.TrsCardData(),
    events.TrsPinComplete(reader=1, status=2, tries=3),
    events.TrsError(code=0x6A),
]


@pytest.mark.parametrize(
    "cmd", ALL_COMMANDS, ids=lambda c: type(c).__name__
)
def test_command_round_trip(cmd):
    assert decode_command(encode_command(cmd)) == cmd


@pytest.mark.parametrize(
    "event", ALL_EVENTS, ids=lambda e: type(e).__name__
)
def test_event_round_trip(event):
    assert decode_event(encode_event(event)) == event


def test_every_command_type_is_covered():
    covered = {type(c).ID for c in ALL_COMMANDS}
    assert covered == set(CommandId)


def test_every_event_type_is_covered():
    covered = {type(e).ID for e in ALL_EVENTS}
    assert covered == set(EventId)


def test_encoded_command_carries_its_discriminator():
    payload = encode_command(commands.Buzzer())
    assert payload["command"] == CommandId.Buzzer


def test_encoded_event_carries_its_discriminator():
    payload = encode_event(events.KeyPress(data=b"1"))
    assert payload["event"] == EventId.KeyPress


# -- The card length is in bits, and only for the raw formats ---------------


def test_cardread_bits_default_to_whole_data():
    assert events.CardRead(format=CardFormat.Wiegand, data=b"\x01\x02").bits == 16


def test_cardread_bits_are_not_bytes():
    # 26 bits needs 4 bytes; 26 bytes of data would be wrong.
    event = events.CardRead(
        format=CardFormat.Wiegand, data=bytes(4), bits=26
    )
    assert encode_event(event)["length"] == 26


def test_cardread_rejects_bits_that_do_not_fit_the_data():
    with pytest.raises(ValueError, match="needs 4 bytes"):
        events.CardRead(format=CardFormat.Wiegand, data=b"\x01", bits=26)


def test_cardread_rejects_bits_on_ascii():
    with pytest.raises(ValueError, match="does not apply"):
        events.CardRead(format=CardFormat.ASCII, data=b"a", bits=8)


def test_ascii_cardread_omits_length():
    assert "length" not in encode_event(
        events.CardRead(format=CardFormat.ASCII, data=b"abc")
    )


# -- Validation catches what the C layer would silently truncate ------------


def test_text_longer_than_the_buffer_is_rejected():
    with pytest.raises(ValueError, match="at most 32 bytes"):
        commands.Text(data="x" * 33)


def test_text_limit_counts_encoded_bytes_not_characters():
    # Each of these is 3 bytes in UTF-8, so 11 of them fit and 12 do not.
    commands.Text(data="é" * 16)
    with pytest.raises(ValueError):
        commands.Text(data="€" * 11)


def test_manufacturer_payload_limit():
    with pytest.raises(ValueError, match="at most 64 bytes"):
        commands.Manufacturer(vendor_code=1, data=b"x" * 65)


def test_bio_template_limit():
    with pytest.raises(ValueError, match="at most 128 bytes"):
        commands.BioMatch(data=b"x" * 129)


def test_status_report_limit():
    with pytest.raises(ValueError, match="at most 64 bytes"):
        commands.Status(type=StatusReportType.Input, report=bytes(65))


def test_keyset_needs_a_key():
    with pytest.raises(ValueError, match="needs a key"):
        commands.Keyset(data=b"")


# -- LED: the reshaped command ----------------------------------------------


def test_led_needs_at_least_one_block():
    with pytest.raises(ValueError, match="temporary or a permanent"):
        commands.LED(led_number=0)


def test_led_nop_block_is_the_same_as_no_block():
    cmd = commands.LED(
        led_number=0,
        temporary=commands.TemporaryLEDParams(
            control_code=TemporaryLEDControlCode.Nop
        ),
        permanent=commands.PermanentLEDParams(
            on_color=LEDColor.Red, on_count=1
        ),
    )
    assert cmd.temporary is None


def test_led_set_block_must_light_up():
    # The spec forbids both durations being zero; a PD would reject it on the
    # wire, so catch it here instead.
    with pytest.raises(ValueError, match="cannot both be zero"):
        commands.PermanentLEDParams(
            control_code=PermanentLEDControlCode.Set, on_color=LEDColor.Red
        )
    with pytest.raises(ValueError, match="cannot both be zero"):
        commands.TemporaryLEDParams(
            control_code=TemporaryLEDControlCode.Set, on_color=LEDColor.Red
        )


def test_led_cancel_block_need_not_light_up():
    # Cancel is not Set, so the both-zero rule does not apply to it.
    commands.TemporaryLEDParams(control_code=TemporaryLEDControlCode.Cancel)


def test_led_timer_count_is_16_bit():
    cmd = commands.LED(
        led_number=0,
        temporary=commands.TemporaryLEDParams(
            on_color=LEDColor.Red, on_count=1, timer_count=1000
        ),
    )
    assert decode_command(encode_command(cmd)).temporary.timer_count == 1000


def test_led_omits_the_block_it_was_not_given():
    payload = encode_command(
        commands.LED(
            led_number=0,
            permanent=commands.PermanentLEDParams(
                on_color=LEDColor.Red, on_count=1
            ),
        )
    )
    assert "permanent" in payload
    assert "temporary" not in payload


# -- Bad input ---------------------------------------------------------------


def test_encoding_a_non_command_is_an_error():
    with pytest.raises(MarshalError, match="Not an osdp command"):
        encode_command(events.KeyPress(data=b"1"))


def test_encoding_a_non_event_is_an_error():
    with pytest.raises(MarshalError, match="Not an osdp event"):
        encode_event(commands.Buzzer())


def test_decoding_an_unknown_command_is_an_error():
    with pytest.raises(MarshalError, match="Cannot decode command"):
        decode_command({"command": 999})


def test_decoding_an_unknown_event_is_an_error():
    with pytest.raises(MarshalError, match="Cannot decode event"):
        decode_event({"event": 999})


# -- Values from the wire we do not name should not crash a callback ---------


def test_unknown_wire_enum_value_is_tolerated():
    # A vendor shipping an out-of-spec biometric status must not blow up the
    # decode, which runs inside a callback invoked from C.
    status = BioStatus(0x42)
    assert status == 0x42
    assert "UNKNOWN" in status.name


def test_an_out_of_spec_control_code_decodes_instead_of_raising():
    # Control codes arrive from the peer, so a CP that sends one we do not name
    # must not take the PD's command callback down with it.
    cmd = decode_command(
        {
            "command": CommandId.Output,
            "output_no": 0,
            "control_code": 200,
            "timer_count": 0,
        }
    )
    assert cmd.control_code == 200
