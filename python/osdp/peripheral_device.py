#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import osdp_sys
import time
import queue
import threading
from typing import Callable, Tuple

from .helpers import PDInfo, PDCapabilities
from .constants import LogLevel

class PeripheralDevice():
    def __init__(self, pd_info: PDInfo, pd_cap: PDCapabilities,
                 log_level: LogLevel=LogLevel.Info,
                 command_handler: Callable[[dict], Tuple[int, dict]]=None,
                 event_completion_handler: Callable[[dict, int], None]=None):
        self.command_queue = queue.Queue()
        self.address = pd_info.address
        self.user_command_handler = None
        self.user_event_completion_handler = None
        osdp_sys.set_loglevel(log_level)
        self.ctx = osdp_sys.PeripheralDevice(pd_info.get(), capabilities=pd_cap.get())
        # Always use our internal handler to ensure queue functionality
        self.ctx.set_command_callback(self._internal_command_handler)
        if hasattr(self.ctx, "set_event_completion_callback"):
            self.ctx.set_event_completion_callback(
                self._internal_event_completion_handler
            )
        self.set_command_handler(command_handler)
        self.set_event_completion_handler(event_completion_handler)
        self.event = None
        self.lock = None
        self.thread = None

    @staticmethod
    def refresh(event, lock, ctx):
        while not event.is_set():
            with lock:
                ctx.refresh()
            time.sleep(0.020) #sleep for 20ms

    def _internal_command_handler(self, command) -> Tuple[int, dict]:
        """Internal handler that manages both queue and user callback"""
        # Always put command in queue for get_command() compatibility
        self.command_queue.put(command)

        # If user has set a custom handler, call it too
        if self.user_command_handler:
            try:
                return self.user_command_handler(command)
            except Exception as e:
                print(f"Error in user command handler: {e}")
                return -1, None

        return 0, None

    def set_command_handler(self, handler: Callable[[dict], Tuple[int, dict]]):
        """Set user command handler while maintaining queue functionality"""
        self.user_command_handler = handler

    def set_event_completion_handler(self, handler: Callable[[dict, int], None]):
        self.user_event_completion_handler = handler

    def _internal_event_completion_handler(self, event, status) -> None:
        if self.user_event_completion_handler:
            try:
                self.user_event_completion_handler(event, status)
            except Exception as e:
                print(f"Error in user event completion handler: {e}")

    def get_command(self, timeout: int=5):
        block = timeout >= 0
        try:
            cmd = self.command_queue.get(block, timeout=timeout)
        except queue.Empty:
            return None
        return cmd

    def submit_event(self, event):
        with self.lock:
            return self.ctx.submit_event(event)

    def notify_event(self, event):
        from warnings import warn
        warn("This method has been renamed to submit_event", DeprecationWarning, 2)
        return self.submit_event(event)

    def register_file_ops(self, fops):
        with self.lock:
            return self.ctx.register_file_ops(0, fops)

    def is_sc_active(self):
        return self.ctx.is_sc_active()

    def is_online(self):
        return self.ctx.is_online()

    def sc_wait(self, timeout=8):
        count = 0
        res = False
        while count < timeout * 2:
            time.sleep(0.5)
            if self.is_sc_active():
                res = True
                break
            count += 1
        return res

    def start(self):
        if self.thread:
            raise RuntimeError("Thread already running!")
        self.event = threading.Event()
        self.lock = threading.Lock()
        args = (self.event, self.lock, self.ctx,)
        self.thread = threading.Thread(name='pd', target=self.refresh, args=args)
        self.thread.start()

    def get_file_tx_status(self):
        with self.lock:
            return self.ctx.get_file_tx_status(0)

    def get_metrics(self):
        with self.lock:
            return self.ctx.get_metrics(0)

    def stop(self):
        if not self.thread:
            raise RuntimeError("Thread not running!")
        while self.thread.is_alive():
            self.event.set()
            self.thread.join(2)
            if not self.thread.is_alive():
                self.thread = None
                break

    def teardown(self):
        self.stop()
        self.ctx = None
