// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// Cadence of the smart-card presence scan (<see cref="ControlPanel.TrsScanEnable"/>).
/// A zero selects that parameter's default.
/// </summary>
/// <param name="Mode0DwellMs">
/// Time in the reader's default mode between probes, where ordinary card reads
/// work (default 200 ms). Shorter makes the scan more aggressive: the reader
/// spends less of each cycle where ordinary credentials can be read.
/// </param>
/// <param name="Mode1DwellMs">Time per probe in transparent mode watching for a card (default 200 ms).</param>
/// <param name="HoldMs">
/// How long transparent mode is held after a sighting, for a
/// <see cref="TrsStartCommand"/> to adopt it (default 500 ms).
/// </param>
/// <param name="Reader">Reader the scan asks, from 0.</param>
public sealed record TrsScanParams(ushort Mode0DwellMs = 0, ushort Mode1DwellMs = 0, ushort HoldMs = 0,
                                   byte Reader = 0);
