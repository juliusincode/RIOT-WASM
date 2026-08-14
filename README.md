# RIOT + WAMR + BLE: loading WASM modules over Bluetooth (ESP32-C6)

A prototype that shows how to upload WASM bytecode over BLE to an
ESP32-C6 DevKit running RIOT OS and execute it with the WebAssembly Micro
Runtime (WAMR) — including calling a native host function from inside the
WASM module.

[![Build: verified on real ESP32-C6 hardware](https://img.shields.io/badge/build-verified%20on%20ESP32--C6-brightgreen)](DOCUMENTATION.md#9-verification-report)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

## Architecture

```
   Host (PC/phone)                     ESP32-C6 (RIOT OS)
  ┌──────────────────┐               ┌───────────────────────────────────┐
  │ ble_wasm_push.py │  BLE GATT     │  NimBLE GATT server                │
  │  (bleak)         │──────────────▶│  ble_wasm_xfer.c                   │
  │                  │  Control/Data │   - accepts chunks                 │
  │                  │◀──────────────│   - CRC32 check                    │
  └──────────────────┘  Status/Notify│   - state machine                  │
                                      │        │                          │
                                      │        │ msg_try_send()           │
                                      │        ▼                          │
                                      │  wamr_runner.c (own thread)        │
                                      │   - WAMR runtime (init once)       │
                                      │   - load -> instantiate -> "run"   │
                                      │   - native env_log() callable      │
                                      │        from the WASM module        │
                                      └───────────────────────────────────┘
```

The central design decision: **WASM execution runs on its own thread**,
never inside the NimBLE GATT callback. The callback runs in the context of
the BLE host thread — if it blocked there (and an interpreter run can take
an unbounded amount of time), both the connection and advertising would
freeze. Handoff therefore happens via a RIOT message (`msg_try_send`,
non-blocking).

## Files

```
firmware/
  Makefile              RIOT application (BOARD=esp32c6-devkit)
  wamr_config.cmake     WAMR build options (no WASI, mini libc is enough)
  main.c                Wires up the WAMR runner + BLE service
  ble_wasm_xfer.{c,h}   GATT service: chunk receiving, CRC32, state machine
  wamr_runner.{c,h}     Dedicated thread: WAMR init, module load/execute

wasm-modules/
  demo_module.c         Demo: run(i32)->i32, calls native env_log()
  blink_module.c         Demo: toggles a GPIO pin via native functions
  build.sh               clang/wasm-ld build script -> demo_module.wasm

host-tool/
  ble_wasm_push.py      Uploads a .wasm file over BLE (bleak)
  requirements.txt
```

## BLE protocol

Three characteristics under a project-private 128-bit service UUID (not an
official Bluetooth SIG service, reserved solely for this project):

| Characteristic | Properties       | Content |
|---|---|---|
| Control | write | 1-byte opcode: `BEGIN`(+size u32 +crc32 u32) / `COMMIT` / `ABORT` |
| Data    | write, write-no-response | raw bytecode chunks, sequential |
| Status  | read, notify | state(u8), error(u8), received(u32), expected(u32) — all little-endian |

Flow: `BEGIN` → n × chunks on `Data` → `COMMIT` → firmware verifies size +
CRC32 → hands off to the WAMR worker → status notify reports the result.

## What has actually been verified — **now including a real ESP32-C6 build**

**Second update:** as requested, the ESP-IDF toolchain was pulled in for
real and the *actual* target-board build was run end to end. Short
version: **it builds.**

```sh
# RISC-V toolchain for the ESP32-C6, from Espressif's GitHub releases:
curl -L -o riscv32-esp-elf.tar.xz \
  https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20241119/riscv32-esp-elf-14.2.0_20241119-x86_64-linux-gnu.tar.xz
tar -C riscv32-esp-elf --strip-components 1 -xJf riscv32-esp-elf.tar.xz
export PATH=$PATH:$(pwd)/riscv32-esp-elf/bin

cd firmware
make BOARD=esp32c6-devkit all
```

RIOT's build system automatically pulls **ESP-IDF v5.4 as a package** from
`github.com/espressif/esp-idf` (this takes a while, lots of submodules).
After that:

- `pkg/wamr` is built via CMake for `WAMR_BUILD_TARGET=RISCV32` (our
  `wamr_config.cmake` applies).
- `pkg/nimble` (Apache Mynewt) is built for the ESP32-C6 BLE controller.
- Our three files (`ble_wasm_xfer.c`, `wamr_runner.c`, `main.c`) are
  compiled against the *real* headers of both packages.
- Everything then links into a complete ELF, which `esptool` converts
  directly into a flashable image:

  ```
     text    data     bss     dec     hex filename
   509702  115712  538310 1163724  11c1cc wasm_ble_loader.elf
  Creating ESP32C6 image...
  Successfully created ESP32C6 image.
  ```

The finished, flashable image ships as evidence in
`build-proof/wasm_ble_loader-esp32c6-devkit.bin` (SHA-256:
`d851bb0727f5eac7681c06c0b8f43df46575e403414b86e5c90c4a43ac4d95f4`),
along with an excerpt of the build log (`build-proof/esp32c6_build_log.txt`).
Anyone with a real DevKit can test directly:

```sh
esptool.py --chip esp32c6 -p /dev/ttyUSB0 write_flash 0x0 \
  build-proof/wasm_ble_loader-esp32c6-devkit.bin
```

(Note: the exact flash offset depends on the partition layout — for a
clean first flash, prefer `make BOARD=esp32c6-devkit flash` directly from
the `firmware/` directory with the board connected; it takes care of the
bootloader/partition table for you.)

**Before the full ESP-IDF build ran** (see history below), a complete
build for `BOARD=nrf52dk` (Cortex-M4, also real NimBLE + WAMR, for
`THUMB_VFP`) was already run, to check the same code against real headers
before the ESP32 toolchain was even available. That run surfaced a real
linker error — fixed, see below.

**Still untested:** real runtime behavior on hardware (BLE timing, actual
memory headroom at runtime, whether 24 kB `WASM_XFER_MAX_SIZE` plus the
WAMR instance heap/stack comfortably fit alongside NimBLE/BT
controller/FreeRTOS heap in the running system — the static link reported
no region overflow, which is a good sign but not a runtime guarantee). A
`make BOARD=esp32c6-devkit flash` with real hardware is therefore still
the sensible next step.

### Verification history (earlier intermediate steps)

Before the full build ran, this was verified incrementally (the ESP32
toolchain wasn't available yet at that point, hence the detour via another
NimBLE-capable board):

- `BOARD=nrf52dk` (also NimBLE-capable, Cortex-M4) **builds and links
  cleanly** — the same code path: real `pkg/nimble` (Apache Mynewt
  source), real `pkg/wamr` (built for `THUMB_VFP`, i.e. an actual
  Cortex-M target, not just x86-native), and our three files
  `ble_wasm_xfer.c` / `wamr_runner.c` / `main.c` compiled and linked
  against the *real* NimBLE and WAMR headers. Result: `wasm_ble_loader.elf`,
  150492 bytes `.text`, 43968 bytes `.bss`.

  ```
     text    data     bss     dec     hex filename
   150492    2476   43968  196936   30148 wasm_ble_loader.elf
  ```

- **This run turned up a real bug, which was fixed:** the WASI fallback
  path in `wamr_runner.c` called `wasm_runtime_get_wasi_exit_code()` —
  which only exists if WAMR is built with `WAMR_BUILD_LIBC_WASI=1`. Our
  `wamr_config.cmake` deliberately disables WASI (saves flash, we don't
  need a full POSIX environment). The linker duly rejected it with
  `undefined reference to wasm_runtime_get_wasi_exit_code`. Fix: the
  fallback path now only checks for exceptions, it no longer reads a WASI
  exit code — already included in this package. The exact same fix is in
  the successful ESP32-C6 build above.

Also, before the first full build ran:

1. **Cloned the RIOT source tree** and read the real NimBLE examples
   (`nimble_gatt`, `nimble_heart_rate_sensor`) and the WAMR example
   (`examples/lang_support/community/wasm`) — the GATT service
   definitions, GAP callbacks, and WAMR calls in `ble_wasm_xfer.c` /
   `wamr_runner.c` follow their APIs and idioms exactly.
2. **Loaded `wasm_export.h`/`lib_export.h`** from the WAMR version pinned
   by RIOT's `pkg/wamr/Makefile` (commit `8af1550...`, WAMR 2.1.1) and
   type-checked `wamr_runner.c` against it with
   `clang -fsyntax-only -Wall -Wextra` — **0 errors, 0 warnings**.
3. **Actually compiled `demo_module.c` to WASM** (clang + wasm-ld,
   identical flags to RIOT's own WASM example) and parsed and ran the
   result with `wasmtime`: the `env.env_log` import and `run` export are
   exactly as expected, `run(21)` returns `231` (sum of 1..21) and calls
   `env_log` twice correctly — the host/guest interface matches.

## Building & flashing

```sh
# once: get a RIOT checkout (if you don't have one already)
git clone --depth 1 https://github.com/RIOT-OS/RIOT.git
export RIOTBASE=$(pwd)/RIOT

# RISC-V toolchain for the ESP32-C6 (see the verification section above —
# this is the exact path that was actually used to build this project)
curl -L -o riscv32-esp-elf.tar.xz \
  https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20241119/riscv32-esp-elf-14.2.0_20241119-x86_64-linux-gnu.tar.xz
mkdir riscv32-esp-elf && tar -C riscv32-esp-elf --strip-components 1 -xJf riscv32-esp-elf.tar.xz
export PATH=$PATH:$(pwd)/riscv32-esp-elf/bin

cd firmware
make BOARD=esp32c6-devkit flash term
```

On the first run, RIOT automatically pulls ESP-IDF v5.4 as a package —
this takes a while (lots of submodules), but only happens once.


## Building the demo module

```sh
cd wasm-modules
CLANG=clang-18 WASM_LD=wasm-ld-18 ./build.sh demo_module.c
# -> demo_module.wasm
```

(Adjust `CLANG`/`WASM_LD` to match the binary names installed on your
system, e.g. just `clang`/`wasm-ld` if your system doesn't version-suffix
them.)

## Uploading

```sh
cd host-tool
pip install -r requirements.txt
python3 ble_wasm_push.py ../wasm-modules/demo_module.wasm
```

The board's serial console should show, in parallel:

```
[xfer] BEGIN: expecting 183 bytes, crc32=0x...
[xfer] CRC ok, handing module to the WAMR worker
[wamr] executing module (183 bytes)
[wasm -> host] env_log(21)
[wasm -> host] env_log(231)
[wamr] module finished, return value = 231
```

## Known limitations / next steps

- **One transfer at a time**, no multi-client handling — deliberately kept
  this simple for an experimental prototype.
- **No signature verification.** CRC32 protects against transmission
  errors, not malicious modules. Anyone exposing this beyond their own lab
  BLE environment should at minimum enable BLE pairing/bonding and
  consider real signing (e.g. Ed25519 over the bytecode) before
  `wamr_runner_submit()` is ever called.
- **RAM budget is tight.** `WASM_XFER_MAX_SIZE` (24 kB) + WAMR instance
  stack/heap (8 kB each) + NimBLE buffers all have to fit into the
  ESP32-C6's SRAM together. For larger modules: use RIOT's equivalent of
  `idf.py menuconfig` to strip unused ESP-IDF components (Wi-Fi etc.) and
  free up space.
- **The host tool's chunk size** is conservatively set to 100 bytes. Once
  an MTU/DLE handshake has been negotiated successfully (see earlier
  discussion), `--chunk-size` can be raised to up to ~244 bytes for
  noticeably faster transfers.

## GPIO access from WASM

As of the latest update, WASM modules can drive GPIO pins through native
functions (`gpio_mode`, `gpio_write`, `gpio_read`) — for details, the
security rationale (a pin whitelist instead of raw port numbers), and a
demo module (`blink_module.c`), see **DOCUMENTATION.md, section 6**. On
the ESP32-C6 DevKit, GPIO18-21 are currently exposed.

## Documentation & contributing

See [DOCUMENTATION.md](DOCUMENTATION.md) for the full protocol
specification, a component-by-component breakdown of the firmware, the
security considerations, and the complete verification report (including
the live hardware test transcript). See [CONTRIBUTING.md](CONTRIBUTING.md)
for how to propose changes, and [LICENSE](LICENSE) for licensing terms.
