#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha@osdp.dev>
#
#  SPDX-License-Identifier: Apache-2.0
#

import os

import pytest

from osdp.key_store import KEY_LEN, KeyStore


def test_gen_key_is_random_and_the_right_length():
    assert len(KeyStore.gen_key()) == KEY_LEN
    assert len(KeyStore.gen_key(24)) == 24
    assert KeyStore.gen_key() != KeyStore.gen_key()


def test_new_key_is_held_in_memory():
    store = KeyStore()
    key = store.new_key("pd-101")
    assert len(key) == KEY_LEN
    assert store.get_key("pd-101") == key


def test_new_key_overwrites_only_when_forced():
    store = KeyStore()
    first = store.new_key("pd-101")

    with pytest.raises(KeyError):
        store.new_key("pd-101", force=False)
    assert store.get_key("pd-101") == first

    assert store.new_key("pd-101") != first


def test_update_key_replaces_the_held_key():
    store = KeyStore()
    store.new_key("pd-101")
    store.update_key("pd-101", b"\xab" * KEY_LEN)
    assert store.get_key("pd-101") == b"\xab" * KEY_LEN


@pytest.mark.parametrize("method", ["get_key", "update_key", "commit_key"])
def test_operations_on_an_unknown_key_raise(method):
    store = KeyStore()
    args = (b"\x00" * KEY_LEN,) if method == "update_key" else ()
    with pytest.raises(KeyError):
        getattr(store, method)("no-such-pd", *args)


def test_committed_key_survives_into_a_new_store(tmp_path):
    store = KeyStore(dir=str(tmp_path))
    key = store.new_key("pd-101")
    store.commit_key("pd-101")

    assert os.path.exists(store.key_file("pd-101"))

    # A fresh store over the same directory must read the same key back.
    reopened = KeyStore(dir=str(tmp_path))
    assert reopened.load_key("pd-101") == key
    assert reopened.get_key("pd-101") == key


def test_load_key_without_a_stored_file_raises(tmp_path):
    store = KeyStore(dir=str(tmp_path))
    with pytest.raises(FileNotFoundError):
        store.load_key("pd-101")


def test_load_key_rejects_a_key_of_the_wrong_length(tmp_path):
    store = KeyStore(dir=str(tmp_path))
    store.new_key("pd-101", key_len=8)
    store.commit_key("pd-101")

    with pytest.raises(ValueError, match="8 bytes, expected 16"):
        store.load_key("pd-101")

    # ...but it is fine when the caller says how long the key should be.
    assert len(store.load_key("pd-101", key_len=8)) == 8
