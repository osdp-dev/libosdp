#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import os
import tempfile
import random

class KeyStore():
    def __init__(self, dir=None):
        self.temp_dir = None
        self.keys = {}
        if not dir:
            self.temp_dir = tempfile.TemporaryDirectory()
            self.key_dir = self.temp_dir.name
        else:
            self.key_dir = dir

    def key_file(self, name):
        return os.path.join(self.key_dir, 'key_' + name + '.bin')

    @staticmethod
    def gen_key(key_len=16):
        key = []
        for i in range(key_len):
            key.append(random.randint(0, 255))
        return bytes(key)

    def _store_key(self, key):
        with open(self.key_file(key), "w") as f:
            f.write(key.hex())

    def get_key(self, name):
        if name not in self.keys:
            raise RuntimeError
        return self.keys[name]

    def new_key(self, name, key_len=16, force=True):
        if not force and name in self.keys:
            raise RuntimeError
        self.keys[name] = self.gen_key(key_len)
        return self.keys[name]

    def commit_key(self, name):
        if name not in self.keys:
            raise RuntimeError
        self._store_key(self.keys[name])

    def update_key(self, name, key):
        if name not in self.keys:
            raise RuntimeError
        self.keys[name] = key

    def load_key(self, name, key_len=16):
        if not os.path.exists(self.key_file(name)):
            raise RuntimeError
        with open(self.key_file(name), "r") as f:
            key = bytes.fromhex(f.read())
        if not key or not isinstance(key, bytes) or len(key) != key_len:
            raise RuntimeError
        self.keys[name] = key
        return key

    def __del__(self):
        if self.temp_dir:
            self.temp_dir.cleanup()
