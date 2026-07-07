#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

from .constants import Capability, LibFlag
from .channel import Channel

class PdId:
    def __init__(self, version: int, model: int, vendor_code: int,
                 serial_number: int, firmware_version: int):
        self.version = version
        self.model = model
        self.vendor_code = vendor_code
        self.serial_number = serial_number
        self.firmware_version = firmware_version

class PDInfo:
    def __init__(self, address: int, channel: Channel, scbk: bytes=None,
                 name: str=None, flags=[], id: PdId=None, baud_rate: int=9600):
        self.address = address
        self.flags = flags
        self.scbk = scbk
        self.baud_rate = baud_rate
        if name:
            self.name = name
        else:
            self.name = "PD-" + str(address)
        self.channel = channel
        if id:
            self.id = id
        else:
            self.id = PdId(1, 1, 0xcafebabe, 0xdeadbeaf, 0xdeaddead)

    def get_flags(self) -> int:
        ret = 0
        for flag in self.flags:
            ret |= flag
        return ret

    def get(self) -> dict:
        return {
            'name': self.name,
            'baud_rate': self.baud_rate,
            'address': self.address,
            'flags': self.get_flags(),
            'scbk': self.scbk,
            'channel': self.channel,

            # Following are needed only for PD. For CP these are don't cares
            'version': self.id.version,
            'model': self.id.model,
            'vendor_code': self.id.vendor_code,
            'serial_number': self.id.serial_number,
            'firmware_version': self.id.firmware_version
        }

class _PDCapEntity:
    def __init__(self, function_code: Capability,
                 compliance_level: int=1, num_items: int=1):
        self.function_code = function_code
        self.compliance_level = compliance_level
        self.num_items = num_items

    def get(self) -> dict:
        return {
            'function_code': self.function_code,
            'compliance_level': self.compliance_level,
            'num_items': self.num_items
        }

class PDCapabilities:
    def __init__(self, cap_list=[]):
        self.capabilities = {}
        for cap in cap_list:
            (fc, cl, ni) = cap
            self.capabilities[fc] = _PDCapEntity(fc, cl, ni)

    def get(self):
        ret = []
        for fc in self.capabilities.keys():
            ret.append(self.capabilities[fc].get())
        return ret
