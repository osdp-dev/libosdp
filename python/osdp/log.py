#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Where the library's own log messages go."""

from typing import Callable

from . import _sys
from .enums import LogLevel

__all__ = ["LogHandler", "set_log_handler"]

LogHandler = Callable[[int, LogLevel, str, str, int], None]
"""Called with (pd_address, level, message, file, line) for each message.

`pd_address` is -1 for messages that are not about a PD. Runs on whichever
thread made the library log: usually a CP or PD refresh thread.
"""


def set_log_handler(handler: LogHandler | None) -> None:
    """Route the library's log messages to `handler` instead of stderr.

    Messages are still filtered by the `log_level` a ControlPanel or
    PeripheralDevice was created with. Pass None to log to stderr again. The
    setting is process-wide, shared by every CP and PD.

    @see osdp_set_log_callback

    Example:
        import logging
        log = logging.getLogger("osdp")
        levels = {LogLevel.Debug: logging.DEBUG, LogLevel.Info: logging.INFO}

        def to_logging(pd, level, message, file, line):
            log.log(levels.get(level, logging.WARNING), "%s", message)

        set_log_handler(to_logging)
    """
    if handler is None:
        _sys.set_log_handler(None)
        return
    if not callable(handler):
        raise TypeError("handler must be callable")

    def trampoline(pd: int, level: int, msg: str, file: str | None,
                   line: int) -> None:
        handler(pd, LogLevel(level), msg.rstrip("\n"), file or "", line)

    _sys.set_log_handler(trampoline)
