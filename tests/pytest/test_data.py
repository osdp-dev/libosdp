#
#  Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
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

pd_id = PdId(1, 1, 314, 512, 443)

# TODO remove this.
pd_addr = 101
pd = PeripheralDevice(PDInfo(pd_addr, f1, scbk=key, id=pd_id), pd_cap, log_level=LogLevel.Debug)
cp = ControlPanel([PDInfo(pd_addr, f2, scbk=key, id=pd_id)])

@pytest.fixture(scope='module', autouse=True)
def setup_test():
    pd.start()
    cp.start()
    cp.sc_wait_all()
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

def test_cp_check_capability():
    assert cp.online_wait(pd.address)
    # check libosdp default capabilities
    assert cp.check_capability(pd.address, Capability.CheckCharacter) == (1, 0)
    assert cp.check_capability(pd.address, Capability.CommunicationSecurity) == (1, 0)
    assert cp.check_capability(pd.address, Capability.ReceiveBufferSize) == (0, 1)

    # check local capabilities
    assert cp.check_capability(pd.address, Capability.OutputControl) == (1, 1)
    assert cp.check_capability(pd.address, Capability.LEDControl) == (1, 2)
    assert cp.check_capability(pd.address, Capability.AudibleControl) == (1, 3)
    assert cp.check_capability(pd.address, Capability.TextOutput) == (1, 4)
