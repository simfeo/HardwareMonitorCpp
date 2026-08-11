# Bindings

Anything that can call a C function and parse JSON can drive the library. `webserver/` builds a
small C-ABI shared library around the C++ core, and the bundled dashboard uses it from Python
through `ctypes` with no packages to install.

## The C ABI

Three functions, in [`webserver/binding.cpp`](../webserver/binding.cpp):

```c
void*       hmc_create();            // creates a Monitor, opens it, primes delta metrics
const char* hmc_poll_json(void* h);  // samples; returns a JSON snapshot
void        hmc_destroy(void* h);
```

`hmc_create` already performs `open()` and the priming poll, so the first `hmc_poll_json` returns
usable rates.

The string from `hmc_poll_json` is **owned by the handle and valid only until the next call on
that handle**. Copy or parse it before polling again. In Python `ctypes.c_char_p` copies into a
`bytes` object on access, so the usual usage is safe.

The library is built as:

| Platform | File |
| :--- | :--- |
| Windows | `hardware_monitor_cpp_c.dll` |
| Linux | `libhardware_monitor_cpp_c.so` |
| macOS | `libhardware_monitor_cpp_c.dylib` |

Build it with `python build_server.py`, which assembles `webserver/build/dist/` containing the
library, the server and the web assets.

## Python

A complete client:

```python
import ctypes
import json
import os
import sys
import time

NAMES = [
    "hardware_monitor_cpp_c.dll",
    "libhardware_monitor_cpp_c.so",
    "libhardware_monitor_cpp_c.dylib",
]


def load(directory):
    for name in NAMES:
        path = os.path.join(directory, name)
        if os.path.exists(path):
            lib = ctypes.CDLL(path)
            lib.hmc_create.restype = ctypes.c_void_p
            lib.hmc_poll_json.restype = ctypes.c_char_p
            lib.hmc_poll_json.argtypes = [ctypes.c_void_p]
            lib.hmc_destroy.argtypes = [ctypes.c_void_p]
            return lib
    sys.exit("shared library not found in %s" % directory)


lib = load(os.path.dirname(os.path.abspath(__file__)))
handle = lib.hmc_create()
try:
    while True:
        snap = json.loads(lib.hmc_poll_json(handle))

        names = {d["id"]: d["name"] for d in snap["devices"]}
        for r in snap["readings"]:
            if r["quantity"] == "temperature":
                print("%-18s %-12s %6.1f %s"
                      % (names[r["device"]], r["channel"], r["value"], r["unit"]))

        time.sleep(1)
finally:
    lib.hmc_destroy(handle)
```

Setting `restype` matters. Without `lib.hmc_create.restype = ctypes.c_void_p`, Python truncates
the returned pointer to a 32-bit `int` on 64-bit builds and the handle is corrupted.

Keep one handle for the lifetime of your loop. Creating one per sample re-enumerates devices and
discards the delta baselines that load and throughput are computed from, so those read zero.

## The JSON snapshot

```json
{
  "devices": [
    {
      "id": "cpu/0",
      "kind": "cpu",
      "name": "Apple M1 Pro",
      "attributes": { "performance_cores": "8", "efficiency_cores": "2" }
    }
  ],
  "readings": [
    { "device": "cpu/0", "quantity": "load",        "unit": "%",  "channel": "Total",       "value": 12.5 },
    { "device": "cpu/0", "quantity": "temperature", "unit": "°C", "channel": "Cores (avg)", "value": 50.4 }
  ]
}
```

`readings[].device` joins to `devices[].id`, which is why the example above builds a lookup
dictionary first. `unit` is the display symbol, ready to print.

`quantity` is one of:

```
temperature  load     power    voltage  current
clock        fan      energy   capacity datarate
datavolume   level    duration count    other
```

`kind` comes from `DeviceKind`: `cpu`, `gpu-integrated`, `gpu-discrete`, `memory`, `storage`,
`network`, `battery`, `cooler`, `system`, `other`.

Treat both lists as open: a reading that needs privileges or a vendor runtime is simply absent
rather than present-and-zero, so look values up defensively.

## The bundled dashboard

```sh
python build_server.py
cd webserver/build/dist && python hardware_monitor_server.py    # http://127.0.0.1:8000
```

It serves the page plus a `/api/data` endpoint returning the JSON above, using only the standard
library (`http.server` and `ctypes`). Options:

```sh
python hardware_monitor_server.py --host 0.0.0.0 --port 8080
```

Binding to `0.0.0.0` exposes your hardware telemetry to the network. There is no authentication,
so keep it on `127.0.0.1` unless you are on a network you trust.

For CPU temperature and package power, start it elevated. On Windows that also requires PawnIO;
see [PawnIO on Windows](pawnio.md), including the DLL-path pitfall that is specific to loading
the library from Python.

## Other languages

Any FFI works the same way. The contract is small: call `hmc_create` once, call `hmc_poll_json`
on an interval, parse the returned UTF-8 JSON, and call `hmc_destroy` when finished. The handle
is not thread-safe, so serialize calls or give each thread its own handle.
