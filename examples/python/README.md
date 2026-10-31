# Python Examples

See [Getting Started](https://doc.osdp.dev/python/getting-started) on
doc.osdp.dev for the Python binding walkthrough.

The Python library wraps the C library and presents a friendly API in its
place. The C library is built for high performance and asks its callers to
meet it halfway: objects handed to it must be kept alive for as long as it
holds them, and calls against a device must be serialized against its refresh
loop. None of that surfaces here. The Python library handles it in the
background -- it owns the refresh thread and serializes API calls against it --
so commands and events are ordinary Python objects that you build, submit, and
then forget about, and every call is safe to make from any thread. Status
checks such as `is_online()` and `is_sc_active()` stay lock-free, so polling
one never waits on the bus.

To run the samples, you have to install the following python packages:

```sh
python3 -m pip install "libosdp[serial]"
```

Then you can run start the CP/PD service as,

```sh
./examples/python/cp_app.py /dev/ttyUSB0 --baudrate 115200
# (or)
./examples/python/pd_app.py /dev/ttyUSB0 --baudrate 115200
```

## Note:

To test how the CP and PD would potentially interact with each other, you can
ask socat to create a pair of psudo terminal devices that are connected to each
other and use that as a serial channel for libosdp communications.

To do this run:

```sh
socat pty,raw,echo=0,nonblock,link=/tmp/ttyS0 pty,raw,echo=0,nonblock,link=/tmp/ttyS1
```

While the above command is running, you can use `/tmp/ttyS0` and `/tmp/ttyS1` to
start your CP and PD app as,

```
./examples/python/cp_app.py /tmp/ttyS0
./examples/python/pd_app.py /tmp/ttyS1
```
