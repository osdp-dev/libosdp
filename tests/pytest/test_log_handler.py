#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import threading

from osdp import *


def test_log_handler_receives_messages_at_the_set_level(fifo_pair):
    seen = []
    lock = threading.Lock()

    def handler(pd, level, message, file, line):
        with lock:
            seen.append((pd, level, message, file, line))

    set_log_handler(handler)
    try:
        cp_chan, pd_chan = fifo_pair()
        pd = PeripheralDevice(PDInfo(101, pd_chan), PDCapabilities(),
                              log_level=LogLevel.Info)
        cp = ControlPanel([PDInfo(101, cp_chan)], log_level=LogLevel.Info)
        pd.start()
        cp.start()
        try:
            assert cp.online_wait(101, timeout=10)
        finally:
            cp.teardown()
            pd.teardown()
    finally:
        set_log_handler(None)

    with lock:
        records = list(seen)
    assert records, "no log message reached the handler"
    for pd, level, message, file, line in records:
        assert isinstance(level, LogLevel)
        assert level <= LogLevel.Info
        assert message and not message.endswith("\n")
        assert isinstance(file, str) and isinstance(line, int)
    assert any(pd == 101 for pd, *_ in records)


def test_log_handler_must_be_callable():
    try:
        set_log_handler(42)
    except TypeError:
        return
    finally:
        set_log_handler(None)
    raise AssertionError("a non-callable handler was accepted")
