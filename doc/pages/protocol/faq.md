---
lastUpdated: 2026-11-27
description: "Frequently asked questions about the OSDP protocol — secure channel, interoperability, RS-485 wiring, addressing, and common integration questions."
---
# Frequently Asked Questions

The following FAQ has been put together by the community. In addition to the
primary authors, the following people have contributed:

- David Eriksson

## Is OSDP standardized?

Yes. OSDP has become an IEC standard — IEC 60839-11-5.

## Who owns this protocol?

OSDP is wholly owned by SIA (Security Industry Association).

## Can I use OSDP/libosdp in my product?

Yes, you are free to use OSDP and/or LibOSDP in your product. You do not have
to pay any royalties to SIA or goToMain.

## Where can I find the specification document?

The protocol is free, but the specification is developed and maintained by the
Security Industry Association and must be purchased from their website.

## Is OSDP free to consume?

Yes. OSDP is open to consumption. You can procure the specification document
and implement it yourself.

That includes smart cards. Transparent Mode, where the ACU exchanges APDUs with
the card through the PD using `osdp_XWR` and `osdp_XRD`, is part of the
standard (v2.2 section 5.11), and LibOSDP implements it fully; see
[Smartcard Commands](/libosdp/smartcard). The specification also names an Extended
Packet Mode in its smart card capability, but defines no messages for it, and
LibOSDP does not implement it.

## I'm contributing to LibOSDP — should I buy the specification?

Please send an email to support@osdp.dev. Contributors of this project do
not need to purchase the specification.

**Update (2021-06-29):** This needs to be confirmed with SIA due to the new
terms under which the specification is shared.

## How can I test OSDP when I only have one side of the link?

When you have a CP but no PD (or the other way around), tools built on LibOSDP
can stand in for the missing side. [Osprio Emulator](https://osdp.dev/products/osprio-view/emulator-workspace/)
emulates a CP or PD on a real bus so you can exercise the device you do have,
and [Osprio Capture](https://osdp.dev/products/osprio-view/capture-workspace/)
sniffs and decodes live OSDP traffic for inspection.
