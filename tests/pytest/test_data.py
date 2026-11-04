#
#  Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import pytest

from osdp import *
from conftest import make_fifo_pair, cleanup_fifo_pair

pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 1, 2),
    (Capability.AudibleControl, 1, 3),
    (Capability.TextOutput, 1, 4),
])

key = KeyStore.gen_key()
f1, f2 = make_fifo_pair("data")

pd_id = PdId(
    version=1,
    model=1,
    vendor_code=314,
    serial_number=512,
    firmware_version=443,
)

# TODO remove this.
pd_addr = 101
pd = PeripheralDevice(PDInfo(pd_addr, f1, scbk=key, id=pd_id), pd_cap, log_level=LogLevel.Debug)
cp = ControlPanel([PDInfo(pd_addr, f2, scbk=key, id=pd_id)])

@pytest.fixture(scope='module', autouse=True)
def setup_test():
    pd.start()
    cp.start()
    if not cp.sc_wait_all(timeout=10):
        teardown_test()
        pytest.fail("Failed to establish secure channel within timeout")
    yield
    teardown_test()

def teardown_test():
    cp.teardown()
    pd.teardown()
    cleanup_fifo_pair("data")

def test_cp_pd_id():
    assert cp.online_wait(pd.address)
    pd_id_recv = cp.get_pd_id(pd.address)
    assert pd_id_recv.version == pd_id.version
    assert pd_id_recv.model == pd_id.model
    assert pd_id_recv.vendor_code == pd_id.vendor_code
    assert pd_id_recv.serial_number == pd_id.serial_number
    assert pd_id_recv.firmware_version == pd_id.firmware_version
