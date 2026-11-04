#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
# Start/stop/teardown guards on both device types. These need a context but no
# traffic, so they run against a channel that never carries anything.

import pytest

from osdp import Channel, ControlPanel, PDCapabilities, PDInfo, PeripheralDevice


class SilentChannel(Channel):
    """A channel that is always idle. Nothing ever comes online over it."""

    def read(self, max_bytes: int) -> bytes:
        return b""

    def write(self, buf: bytes) -> int:
        return len(buf)

    def flush(self) -> None:
        pass


@pytest.fixture
def cp():
    device = ControlPanel([PDInfo(address=101, channel=SilentChannel())])
    yield device
    if device.thread:
        device.stop()


@pytest.fixture
def pd():
    device = PeripheralDevice(
        PDInfo(address=101, channel=SilentChannel()), PDCapabilities()
    )
    yield device
    if device.thread:
        device.stop()


@pytest.fixture(params=["cp", "pd"])
def device(request):
    """Both device types, since they carry the same lifecycle guards."""
    return request.getfixturevalue(request.param)


def test_start_twice_is_refused(device):
    device.start()
    with pytest.raises(RuntimeError, match="Thread already running"):
        device.start()


def test_stop_without_start_is_refused(device):
    with pytest.raises(RuntimeError, match="Thread not running"):
        device.stop()


def test_start_stop_start_again(device):
    device.start()
    device.stop()
    assert device.thread is None
    device.start()
    assert device.thread is not None


def test_teardown_releases_the_context(device):
    device.start()
    device.teardown()
    # Every method reaches the library through the ctx property, which is what
    # refuses to hand back a context that has been released.
    with pytest.raises(RuntimeError, match="has been torn down"):
        device.ctx


def test_teardown_before_start_is_refused(device):
    with pytest.raises(RuntimeError, match="Thread not running"):
        device.teardown()


@pytest.mark.parametrize(
    "method, args",
    [
        ("is_online", (0x99,)),
        ("is_sc_active", (0x99,)),
        ("get_pd_id", (0x99,)),
        ("flush_commands", (0x99,)),
        ("enable_pd", (0x99,)),
        ("disable_pd", (0x99,)),
        ("is_pd_enabled", (0x99,)),
        ("get_metrics", (0x99,)),
        ("get_file_tx_status", (0x99,)),
    ],
)
def test_cp_rejects_an_address_it_does_not_know(cp, method, args):
    # Every address-taking method funnels through pd_addr.index().
    with pytest.raises(ValueError):
        getattr(cp, method)(*args)


def test_cp_offline_wait_returns_true_for_a_pd_that_never_comes_up(cp):
    cp.start()
    # Nothing answers on a SilentChannel, so the PD is offline from the start.
    assert cp.offline_wait(101, timeout=2) is True


def test_cp_online_wait_gives_up_when_nothing_answers(cp):
    cp.start()
    assert cp.online_wait(101, timeout=0.2) is False
    assert cp.online_wait_all(timeout=0.2) is False
    assert cp.get_num_online() == 0
    assert cp.status() == 0
