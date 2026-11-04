#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time

from osdp import *
from osdp import commands
from conftest import assert_command_received

pd_addr = 101

BUZZ = commands.Buzzer(
    control_code=BuzzerControlCode.Off,
    on_count=1,
    off_count=1,
    rep_count=1,
)


def wait_until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.02)
    return True


def test_hotplug_disable_rekey_enable(fifo_pair):
    key = KeyStore.gen_key()
    cp_chan, pd_chan = fifo_pair()
    pd = PeripheralDevice(
        PDInfo(pd_addr, pd_chan, scbk=key, flags=[LibFlag.EnforceSecure]),
        PDCapabilities([(Capability.AudibleControl, 1, 1)]),
        log_level=LogLevel.Debug,
    )
    cp = ControlPanel(
        [PDInfo(pd_addr, cp_chan, scbk=key, flags=[LibFlag.EnforceSecure])],
        log_level=LogLevel.Debug,
    )
    pd.start()
    cp.start()
    try:
        assert cp.sc_wait(pd_addr, timeout=10)
        assert cp.is_pd_enabled(pd_addr)
        assert not cp.enable_pd(pd_addr), "enabling an enabled PD succeeded"
        assert not cp.set_scbk(pd_addr, key), \
            "set_scbk must refuse a PD that is being polled"
        assert cp.submit_command(pd_addr, BUZZ)
        assert_command_received(pd, BUZZ)

        # Both calls only post a request; the FSM acts on it later.
        assert cp.disable_pd(pd_addr)
        assert wait_until(lambda: not cp.is_pd_enabled(pd_addr)), \
            "PD never reached the disabled state"
        assert not cp.submit_command(pd_addr, BUZZ), \
            "a disabled PD accepted a command"

        assert cp.set_scbk(pd_addr, key)
        assert cp.enable_pd(pd_addr)
        assert wait_until(lambda: cp.is_pd_enabled(pd_addr)), \
            "PD never left the disabled state"
        assert cp.sc_wait(pd_addr, timeout=10), \
            "PD did not come back on the key set while it was disabled"
    finally:
        cp.teardown()
        pd.teardown()
