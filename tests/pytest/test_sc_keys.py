#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

from osdp import *
from conftest import make_fifo_pair, cleanup_fifo_pair

def test_set_new_scbk(utils):
    # Create single CP-PD pair
    f1, f2 = make_fifo_pair("sc_keys")
    pd_addr = 101
    key = utils.ks.new_key('sc-keys-pd')
    pd = utils.create_pd(PDInfo(pd_addr, f1, scbk=key))
    cp = utils.create_cp([ PDInfo(pd_addr, f2, scbk=key) ], sc_wait=True)

    # Set a new SCBK from the CP side and verify whether it was received
    # by the PD as we intended.
    new_key = utils.ks.gen_key()
    keyset_cmd = {
        'command': Command.Keyset,
        'type': 1,
        'data': new_key
    }
    assert cp.submit_command(pd_addr, keyset_cmd)
    cmd = pd.get_command()
    assert cmd == keyset_cmd
    utils.ks.update_key('sc-keys-pd', new_key)

    # Stop CP and restart SC with new SCBK. PD should accept it
    cp.teardown()
    cp = utils.create_cp([ PDInfo(pd_addr, f2, scbk=utils.ks.get_key('sc-keys-pd')) ])
    assert cp.sc_wait(pd_addr)

    # Cleanup
    cp.teardown()
    pd.teardown()
    cleanup_fifo_pair("sc_keys")

def test_install_mode_set_scbk(utils):
    f1, f2 = make_fifo_pair("install_mode")
    pd_addr = 101
    pd = utils.create_pd(PDInfo(pd_addr, f1, flags=[ LibFlag.InstallMode ]))
    cp = utils.create_cp([
        PDInfo(pd_addr, f2, scbk=utils.ks.new_key('install-mode-pd'))
     ])
    assert cp.sc_wait_all()

    # Cleanup
    cp.teardown()
    pd.teardown()
    cleanup_fifo_pair("install_mode")