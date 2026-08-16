# ExtIO_Pluto — Changelog

A fork of the original [ExtIO_Pluto](https://github.com/lesha108/ExtIO_Pluto/)
with an added receiver-control GUI and a build system migrated from Visual
Studio to CMake + VS Code. Below is the full list of changes made so far, in
the order they were introduced.

Modified / added files:
- `ExtIO_Pluto.cpp` — all the logic
- `ExtIO_PlutoSDR.rc` — GUI dialog
- `resource.h` — dialog control IDs
- `CMakeLists.txt` — CMake build (new file, replaces `.sln`/`.vcxproj`)
- `.vscode/settings.json` — CMake Tools settings for VS Code (new file)

---

## 1. New GUI controls

The dialog grew from 168×33 to 230×143 and now contains:

| Control | What it does |
|---|---|
| **PlutoSDR URI** + Connect button | already existed — `usb:1.2.5` or `ip:192.168.x.x` |
| **Sample rate** (dropdown) | 10 presets from 528 kHz to 20 MHz, sorted ascending |
| **Gain mode** (dropdown) | Manual / Slow AGC / Fast AGC / Hybrid AGC (AD9361 `gain_control_mode` attribute) |
| **RX Gain, dB** (text field) | active only in Manual mode; `hardwaregain` attribute |
| **RX filter BW, Hz** (text field) | `rf_bandwidth` attribute; until touched manually, auto = 0.8×sample rate |
| **BW step** (dropdown) | mouse-wheel step for the BW field: 1/10/100 kHz, 1/10 MHz |

The sample-rate list is built dynamically in the GUI from `ExtIoGetSrates()`
— new presets can be added simply by extending the `kPresetSrates[]` array
inside that function.

## 2. Mouse-wheel control

The **RX Gain** and **RX filter BW** fields are hooked into `WM_MOUSEWHEEL`
via classic Win32 subclassing (`SetWindowLongPtr(..., GWLP_WNDPROC, ...)`):
click into the field (give it focus) → scrolling the wheel changes the value
by one step (1 dB for Gain, whatever's selected in the BW step combo for BW)
and applies it to the hardware immediately.

## 3. Apply-on-the-fly, no Apply buttons

- **Gain and BW** — applied as the user types (`EN_CHANGE`); the Apply
  buttons were removed and the fields stretched to fill the freed space.
- **Sample rate** — deliberately **left as dropdown-selection only**. Manual
  entry of an arbitrary value was tried (via an editable combo box + a Set
  button), but was abandoned — with a high lower bound (520,833 Hz),
  intermediate values while typing a multi-digit number kept getting
  clamped up to the minimum and stomping on what was being typed. Decided
  not to over-engineer it and reverted to a plain preset list.

## 4. Build via CMake + VS Code instead of Visual Studio

Added `CMakeLists.txt` — no longer need a `.sln`/`.vcxproj`, nor specifically
the old v140/v140_xp toolset from the original project: any modern MSVC
(from "Build Tools for Visual Studio", no full IDE install required) works.

Key details of `CMakeLists.txt`:
- `libiio.lib` and all the runtime DLLs in the repo are **32-bit (x86)** —
  the build hard-checks `CMAKE_SIZEOF_VOID_P == 4` and fails with a clear
  error if an `amd64`/`x64` kit is picked instead of `x86`.
- Function exports (`InitHW`, `StartHW`, ...) still come from
  `ExtIO_Pluto.def`, just passed to the linker via `/DEF:` instead of a
  field in VS project properties.
- The `.rc` file is compiled automatically (CMake finds `rc.exe` for the
  MSVC toolchain on its own).

Step-by-step instructions for installing Build Tools, picking a kit
(must be **x86**, not amd64!), and building — see `README_VSCODE_BUILD.md`
(happy to regenerate/update it as a separate file if needed).

## 5. Persisting all settings across HDSDR restarts

### Why not through HDSDR's built-in mechanism

Initially tried the standard `ExtIoGetSetting`/`ExtIoSetSetting` API — it
turned out unreliable: HDSDR didn't always correctly restore newer fields
(ones the original author hadn't anticipated), causing Gain and BW to
occasionally reset to defaults on restart.

### Attempt #1 — Windows Registry (abandoned)

Next tried saving to `HKEY_CURRENT_USER\Software\R2AJP\ExtIO_Pluto` via
Advapi32 (`RegOpenKeyEx`/`RegSetValueEx`...), loading from `DllMain`. This
didn't work reliably either — the likely cause: calling `Advapi32.dll`
functions directly from `DllMain` risks the classic loader-lock trap (if
`Advapi32.dll` hasn't yet been loaded into the process at that point).
Dropped the registry approach in favor of a plain file.

### Final solution — `ExtIO_Pluto_settings.ini` next to the DLL

A simple `key=value` text file, whose path is computed via
`GetModuleFileNameA` from the DLL's own `__ImageBase` — so the file always
lives in the same folder `ExtIO_Pluto.dll` was copied into.

What gets saved:

```
GainModeIdx=0
GainDB=49.00
BWHz=3000000
BWAuto=0
BWStepIdx=1
SampleRateIdx=4
SDR=ip:192.168.3.1
DialogX=812
DialogY=430
```

When it's saved:
- on any Gain/BW change (typing, mouse wheel);
- on Gain mode / BW step / Sample rate changes;
- on a successful Connect;
- when the dialog window is moved (after the mouse is released,
  `WM_EXITSIZEMOVE`);
- when HDSDR closes (`CloseHW`) — a final safety-net flush.

When it's loaded:
- at the start of `InitHW()`, **after** the debug console is created
  (matters for `_MYDEBUG` diagnostics) and **before** connecting to the
  hardware via `gSDR`; guarded by `!gbInitHW` so the file isn't re-read if
  the host happens to call `InitHW` more than once per session.

### Two restore bugs found and fixed

1. **`ExtIoSetSetting` was overwriting restored values.** HDSDR calls this
   itself, before `InitHW`, and if its *own* `.ini` has nothing saved for
   these fields, it passes empty/zero values — which stomped on what we'd
   just loaded from our own file.
   **Fix:** `ExtIoSetSetting` no longer touches our fields at all (only the
   formal `idx==0` identifier handshake is still handled).

2. **Phantom `EN_CHANGE` from control creation.** The `IDC_EDIT_GAIN` /
   `IDC_EDIT_BW` fields in the `.rc` template have placeholder text `"0"`
   and `"2000000"`. Creating an Edit control with that text itself
   generates `WM_SETTEXT` → `EN_CHANGE`, and our "apply live" handler
   treated that as genuine user input, immediately saving the placeholder
   to the file and overwriting the values just loaded.
   **Fix:** a `gDialogReady` flag — `EN_CHANGE` is ignored until the dialog
   is fully initialized (i.e. until `UpdateDialog()` has run at the end of
   `WM_INITDIALOG`).

## 6. Window positioning

- **Centering on first run.** The dialog used to always appear at (0,0) —
  the screen's top-left corner. Now, if no position has been saved yet, it
  centers itself on the screen's work area (via
  `SystemParametersInfoA(SPI_GETWORKAREA, ...)`, accounting for the
  taskbar).
- **Remembering position.** Coordinates (`DialogX`/`DialogY`) are saved to
  the `.ini` while the window is dragged and restored on the next launch
  (with a check that clamps the window back onto the visible screen area
  in case the resolution changed since it was saved).
- **Always-on-top.** The dialog is created with `HWND_TOPMOST` — it stays
  above the HDSDR window (and above windows in general) regardless of
  focus.

---

## Technical notes for further development

- **Ranges:** Gain −10…77 dB, BW 200 kHz…56 MHz — conservative estimates
  for AD9361/AD9364; worth checking against `iio_info -a` for your specific
  Pluto revision and adjusting the constants in the code if needed.
- **`_MYDEBUG`** is enabled automatically in Debug builds (tied to
  `_DEBUG`) — opens a separate console with logging (`AllocConsole` inside
  `InitHW`); settings load/save diagnostics now print there too.
- **Sample-rate index backward compatibility:** the preset list got
  re-sorted a few times during development — after updating the DLL, an
  old `SampleRateIdx=` value in the `.ini` may temporarily point to a
  different rate than before; just re-select the desired Sample Rate from
  the list once after updating.
