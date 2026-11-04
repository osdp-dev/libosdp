#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
# Several PDs daisy-chained onto one RS-485 bus behind a single CP.
#
# Everything else in the suite gives each PD its own point-to-point channel, so
# none of it exercises the CP's round-robin across PDs, nor the PD-side address
# filtering that a shared wire depends on. MultidropBus puts every PD on the
# same pair of streams: each PD sees the traffic addressed to its neighbours and
# has to ignore it.

import pytest

from osdp import Capability, KeyStore, PDCapabilities, PDInfo, commands, events
from conftest import MultidropBus, assert_command_received

ADDRESSES = [101, 102, 103]


@pytest.fixture
def bus_of_three(utils):
    """One CP, three secure PDs, all on a single bus."""
    bus = MultidropBus(len(ADDRESSES))
    keys = {address: utils.ks.gen_key() for address in ADDRESSES}
    capabilities = PDCapabilities(
        [
            (Capability.OutputControl, 1, 8),
            (Capability.LEDControl, 1, 1),
            (Capability.AudibleControl, 1, 1),
        ]
    )

    pds = {
        address: utils.create_pd(
            PDInfo(address, bus.pd_channel(i), scbk=keys[address]),
            capabilities=capabilities,
        )
        for i, address in enumerate(ADDRESSES)
    }

    channel = bus.multidrop_channel()
    cp = utils.create_cp(
        [PDInfo(address, channel, scbk=keys[address]) for address in ADDRESSES],
        sc_wait=True,
    )

    yield cp, pds

    cp.teardown()
    for pd in pds.values():
        # A test may have deliberately stopped one of them; teardown() goes
        # through stop(), which insists on a running thread.
        if pd.thread:
            pd.teardown()


def test_every_pd_on_the_bus_comes_up(bus_of_three):
    cp, pds = bus_of_three
    for address in ADDRESSES:
        assert cp.is_online(address), f"PD-{address} never came online"
        assert cp.is_sc_active(address), f"PD-{address} has no secure channel"
    assert cp.get_num_online() == len(ADDRESSES)
    assert cp.get_num_sc_active() == len(ADDRESSES)


def test_each_pd_is_addressed_independently(bus_of_three):
    cp, pds = bus_of_three

    # A distinct command per PD, submitted back to back, so the CP has to keep
    # three conversations straight as it round-robins the bus.
    sent = {
        address: commands.Output(output_no=i, control_code=1, timer_count=10)
        for i, address in enumerate(ADDRESSES)
    }
    for address, command in sent.items():
        assert cp.submit_command(address, command)

    for address, command in sent.items():
        assert_command_received(pds[address], command, timeout=5)

    # Every PD saw all three go by on the wire; each may act on its own only.
    for address in ADDRESSES:
        assert pds[address].get_command(timeout=0.1) is None, (
            f"PD-{address} acted on a command addressed to another PD"
        )


def test_an_event_arrives_tagged_with_the_pd_that_sent_it(bus_of_three):
    cp, pds = bus_of_three

    for address in ADDRESSES:
        event = events.KeyPress(data=bytes([address & 0xFF]))
        assert pds[address].submit_event(event)

        received = None
        for _ in range(20):
            candidate = cp.get_event(address, timeout=0.5)
            if candidate is not None and not isinstance(
                candidate, events.Notification
            ):
                received = candidate
                break
        assert received == event, f"PD-{address}'s event did not arrive intact"


@pytest.mark.slow
def test_one_pd_going_quiet_leaves_the_others_alone(bus_of_three):
    cp, pds = bus_of_three

    # Stop servicing PD-102. It should drop off the bus without taking its
    # neighbours with it -- the whole point of not sharing a state machine.
    #
    # This takes longer than a point-to-point link would: the CP round-robins,
    # so PD-102's polling slot only comes up every third turn and the response
    # timeouts accrue that much more slowly. Measured at ~9s for three PDs.
    pds[102].stop()
    assert cp.offline_wait(102, timeout=20), "PD-102 never went offline"

    for address in (101, 103):
        assert cp.is_online(address), f"PD-{address} went down with PD-102"

    # ...and the survivors still answer.
    command = commands.Buzzer(on_count=1, off_count=1, rep_count=1)
    assert cp.submit_command(101, command)
    assert_command_received(pds[101], command, timeout=5)
