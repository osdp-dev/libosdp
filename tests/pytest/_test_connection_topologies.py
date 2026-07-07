#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
from osdp import *

def test_daisy_chained_pds(utils):
    # Created single CP-PD pair
    pd0_info = PDInfo(101, utils.ks.gen_key(), name='daisy_chained_pd', channel_type='unix_bus')
    pd1_info = PDInfo(102, utils.ks.gen_key(), name='daisy_chained_pd', channel_type='unix_bus')
    pd0 = utils.create_pd(pd0_info)
    pd1 = utils.create_pd(pd1_info)
    cp = utils.create_cp([ pd0_info, pd1_info ], sc_wait=True)

    # Exercise both the PDs while checking whether they are alive and well
    for _ in range(5):
        test_cmd = {
            'command': Command.Comset,
            'address': 101,
            'baud_rate': 9600
        }
        assert cp.is_sc_active(101)
        assert cp.send_command(101, test_cmd)
        assert pd0.get_command() == test_cmd

        test_cmd = {
            'command': Command.Comset,
            'address': 102,
            'baud_rate': 9600
        }
        assert cp.is_sc_active(102)
        assert cp.send_command(102, test_cmd)
        assert pd1.get_command() == test_cmd

        time.sleep(1)

    # Cleanup
    cp.teardown()
    pd1.teardown()
    pd0.teardown()
