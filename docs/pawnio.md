# PawnIO on Windows

CPU **package temperature** and **RAPL package power** come from model-specific registers (MSRs),
which x86 only exposes to ring 0. A userspace process cannot read them at all, no matter how
elevated. This library reaches them through [**PawnIO**](https://pawnio.eu), a small signed
kernel driver that executes a sandboxed, signed bytecode module on your behalf.

Nothing else needs this. CPU load and clock, all GPU telemetry, memory, storage, network and
battery work as an ordinary user with no driver. If you skip this page entirely, the only thing
you lose is CPU temperature and package power, which read as absent.

## What you need

| Piece | Where it comes from | Bundled? |
| :--- | :--- | :--- |
| PawnIO driver + `PawnIOLib.dll` | <https://pawnio.eu> | No, install it yourself |
| Signed module `.bin` for your CPU | `namazso/PawnIO.Modules` release `0.1.6` | No, downloaded by the helper script |
| Administrator rights | You | Every run |

The module *sources* are pinned in this repo as the `third_party/pawnio-modules` submodule for
reference. The signed binaries are release artifacts and are deliberately not committed.

Which module you need depends on the CPU vendor:

| CPU | Module |
| :--- | :--- |
| Intel | `IntelMSR.bin` |
| AMD (Zen, Family 17h and newer) | `AMDFamily17.bin` |
| ARM64 | none - there are no x86 MSRs, so CPU temp/power stay unavailable |

## Setup

**1. Install the driver.** Download and install PawnIO from <https://pawnio.eu>. This is a
one-time system install and provides the kernel driver plus `PawnIOLib.dll`.

**2. Install the module.** The helper script detects your CPU vendor, downloads the pinned
release and copies the matching `.bin` into a `modules\` folder next to each built executable:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\windows\setup-pawnio.ps1
```

`python build_lib.py`, `build_console.py` and `build_server.py` already do this for their own
output, so a normal build needs no separate step. Run the script yourself when you built with
plain CMake, or to install into a specific folder:

```powershell
# install into one folder explicitly
powershell -File scripts\windows\setup-pawnio.ps1 -Destination C:\tools\hwmon

# force a module rather than detecting the vendor
powershell -File scripts\windows\setup-pawnio.ps1 -Module IntelMSR
```

**3. Run elevated.** Reading MSRs requires administrator rights even with the driver installed:

```powershell
sudo .\hardware_monitor_console.exe
```

## Where the module is looked up

At runtime the library searches, in order:

1. `%HARDWARE_MONITOR_CPP_PAWNIO_DIR%`
2. `<library directory>\modules`
3. `<library directory>`
4. `<executable directory>\modules`
5. `<executable directory>`

"Library directory" is the folder holding the binary that contains this code: the executable when
you link statically, or the DLL when the library is embedded in something else. That distinction
matters more than it sounds like it should, as the next section explains.

The environment variable wins over everything, which makes it the quickest way to test a setup
without moving files:

```cmd
set HARDWARE_MONITOR_CPP_PAWNIO_DIR=D:\path\to\dist\modules
```

Point it at the folder that *contains* the `.bin`, not at the file.

## Troubleshooting

**CPU temperature and power read `n/a`, everything else works.** Expected when any of the three
pieces is missing. Work through them in order: driver installed, `.bin` present in one of the
searched folders, process elevated. Since the elevated CPU path is the only thing affected, an
otherwise healthy dashboard with `n/a` on exactly those two fields is the signature.

**It works in the console monitor but not the web dashboard.** This one has a specific cause. The
dashboard is `python.exe` loading `hardware_monitor_cpp_c.dll` through `ctypes`, so the *process
executable* is Python, not anything in your build folder. A lookup based only on the process
executable searches Python's install directory and never finds the module sitting next to the
DLL. That is why the search list above includes the library's own directory. If you are running
an older build that predates this, the workaround is the environment variable:

```cmd
set HARDWARE_MONITOR_CPP_PAWNIO_DIR=<repo>\webserver\build\dist\modules
python hardware_monitor_server.py
```

**Running elevated did not help.** Elevation alone is not sufficient, and it is not the usual
culprit either. If the module cannot be found, administrator rights change nothing. Check the
lookup paths first.

**Graphs show boxes instead of blocks in an elevated console.** Unrelated to PawnIO. The console
monitor draws with Unicode block glyphs, which need a TrueType console font. If `sudo` is set to
"In a new window", that window uses the raster default font. Set `sudo` to **Inline**
(Settings > System > For developers > Sudo > *Inline*), or pick a TrueType font (Cascadia Mono,
Consolas) via the console window's Properties > Font.

**ARM64 Windows.** There is no module and there will not be one; x86 MSRs do not exist on ARM.
The build scripts detect this and skip the install with a warning rather than failing.
