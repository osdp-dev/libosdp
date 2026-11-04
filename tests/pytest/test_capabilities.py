#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#
# Capability negotiation, for every capability there is.
#
# test_data.py checks four of them. This declares the whole enum on one PD and
# reads all of it back over the wire, so that a capability the bindings forget
# to export -- as happened with SecurePinEntry and OSDPVersion -- cannot pass
# unnoticed again.

import pytest

from osdp import (
    Capability,
    ControlPanel,
    KeyStore,
    LogLevel,
    PDCapabilities,
    PDInfo,
    PeripheralDevice,
)

# libosdp answers these itself, from what it actually supports, whatever the
# application asks for. Everything else is the application's to declare.
# CommunicationSecurity is what a *keyed* PD reports; see
# test_a_keyless_pd_reports_no_communication_security for the other case.
LIBRARY_OWNED = {
    Capability.CheckCharacter: (1, 0),
    Capability.CommunicationSecurity: (1, 0),
    Capability.ReceiveBufferSize: (0, 1),
    Capability.OSDPVersion: (2, 0),  # compliance level 2 == SIA OSDP 2.2
}

# Unused is the enum's zero member and not something a PD declares.
DECLARABLE = [c for c in Capability if c is not Capability.Unused]

# A distinct num_items per capability, so a mixed-up index cannot pass.
DECLARED = {c: (1, i + 1) for i, c in enumerate(DECLARABLE)}


@pytest.fixture(scope="module")
def cp_with_a_fully_capable_pd(tmp_path_factory):
    from conftest import FIFOChannel

    tmp = tmp_path_factory.mktemp("caps")
    one, two = str(tmp / "one"), str(tmp / "two")
    pd_chan, cp_chan = FIFOChannel(one, two), FIFOChannel(two, one)

    key = KeyStore.gen_key()
    capabilities = PDCapabilities(
        [(c, level, items) for c, (level, items) in DECLARED.items()]
    )

    pd = PeripheralDevice(
        PDInfo(101, pd_chan, scbk=key), capabilities, log_level=LogLevel.Error
    )
    cp = ControlPanel([PDInfo(101, cp_chan, scbk=key)], log_level=LogLevel.Error)
    pd.start()
    cp.start()
    assert cp.sc_wait_all(timeout=10), "PD never came up"

    yield cp

    cp.teardown()
    pd.teardown()
    for chan in (pd_chan, cp_chan):
        chan.close()


@pytest.mark.parametrize(
    "capability",
    [c for c in DECLARABLE if c not in LIBRARY_OWNED],
    ids=lambda c: c.name,
)
def test_a_declared_capability_reaches_the_cp(
    cp_with_a_fully_capable_pd, capability
):
    cp = cp_with_a_fully_capable_pd
    assert cp.check_capability(101, capability) == DECLARED[capability]


@pytest.mark.parametrize(
    "capability, expected",
    list(LIBRARY_OWNED.items()),
    ids=lambda v: v.name if isinstance(v, Capability) else "",
)
def test_a_library_owned_capability_ignores_what_the_pd_asked_for(
    cp_with_a_fully_capable_pd, capability, expected
):
    # The PD declared something else for each of these; the library's own answer
    # is what has to reach the CP.
    cp = cp_with_a_fully_capable_pd
    assert DECLARED[capability] != expected, "test no longer proves anything"
    assert cp.check_capability(101, capability) == expected


def test_the_unused_capability_code_is_not_queryable(
    cp_with_a_fully_capable_pd,
):
    # Unused is the enum's zero member, not a real function code. Asking about
    # it is a programming error rather than a capability the PD happens to lack.
    cp = cp_with_a_fully_capable_pd
    with pytest.raises(ValueError, match="function code"):
        cp.check_capability(101, Capability.Unused)
