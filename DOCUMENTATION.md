# WASM modules over BLE on RIOT OS (ESP32-C6)

**Complete project documentation**
Status: **verified on real hardware** (ESP32-C6 DevKit, firmware boots, BLE
transfer + WASM execution tested successfully)

---

## Table of contents

1. [Overview & motivation](#1-overview--motivation)
2. [Architecture](#2-architecture)
3. [Project structure](#3-project-structure)
4. [BLE GATT protocol — full specification](#4-ble-gatt-protocol--full-specification)
5. [Firmware components in detail](#5-firmware-components-in-detail)
6. [WASM module contract](#6-wasm-module-contract)
7. [Host tool (`ble_wasm_push.py`)](#7-host-tool-ble_wasm_pushpy)
8. [Build, toolchain & flashing](#8-build-toolchain--flashing)
9. [Verification report](#9-verification-report)
10. [Live hardware test — transcript](#10-live-hardware-test--transcript)
11. [Security considerations](#11-security-considerations)
12. [Known limitations & roadmap](#12-known-limitations--roadmap)
13. [Appendix](#13-appendix)

---

## 1. Overview & motivation

This project is an experimental prototype that combines two technologies
not normally seen together on microcontrollers:

- **RIOT OS** as a real-time operating system for IoT hardware, here on an
  **ESP32-C6** (RISC-V, BLE 5).
- **WAMR** (WebAssembly Micro Runtime, Bytecode Alliance) as an embedded
  WASM interpreter that executes bytecode loaded at runtime.

The two are connected over **Bluetooth Low Energy**: a host (PC/phone)
sends a `.wasm` module to the device in chunks over a project-private GATT
service, the module is validated via CRC32, and then executed by a
dedicated WAMR worker thread — including a call from the WASM code into a
native RIOT function.

**Use case:** reloading or swapping device logic without reflashing the
device — classic "change over-the-air behavior," but with a portable,
sandboxed bytecode VM instead of native code. The bytecode runs inside
WAMR's interpreter sandbox, so (unlike a classic firmware update) it has
no direct access to arbitrary memory — only to whatever is explicitly
exposed to it as a host function.

**Why no WASI?** Deliberately disabled for this project (see
`wamr_config.cmake`). WASI brings a full POSIX-like environment
(filesystem access, `argv`, exit codes, etc.) that a lean IoT device
neither needs nor benefits from — every interaction with the outside world
should go through explicitly defined native functions (principle of least
privilege), not through a generic POSIX layer.

---

## 2. Architecture

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

### Central design decision: a dedicated worker thread

WASM execution **never** runs inside the NimBLE GATT callback itself. The
callback runs in the context of the NimBLE host thread; a blocking or
long-running call there would stall the entire BLE stack (connection
timeouts, missed advertising packets, in the worst case disconnects).
Instead:

1. The GATT callback copies incoming bytes into a static buffer.
2. After `COMMIT` and a successful CRC check, the buffer pointer is handed
   off to the `wamr_runner` thread via `msg_try_send()` (non-blocking!).
3. The worker thread executes the module and reports the result back via
   `ble_wasm_xfer_notify_run_done()` — this callback updates the status
   and notifies the host via a GATT notify.

This separation is the difference between "the BLE connection stays
stable while WASM is computing" and "the connection drops as soon as a
module takes more than a few milliseconds."

### Layering

```
┌─────────────────────────────────────────────┐
│  Application: main.c                          │  wires up the init order
├─────────────────────────────────────────────┤
│  ble_wasm_xfer.c   │  wamr_runner.c          │  two independent modules,
│  (BLE/GATT/protocol)│ (WAMR/thread/execution) │  communicate only via
│                     │                         │  msg_try_send() +
│                     │                         │  ble_wasm_xfer_notify_run_done()
├─────────────────────────────────────────────┤
│  NimBLE (pkg/nimble)│  WAMR (pkg/wamr)        │  external packages, pulled
│  Apache Mynewt       │  Bytecode Alliance      │  automatically from GitHub
│                     │                         │  by RIOT's build system
├─────────────────────────────────────────────┤
│  RIOT OS kernel (threads, messages, mutexes)  │
├─────────────────────────────────────────────┤
│  ESP-IDF (esp32_sdk package) + hardware      │
└─────────────────────────────────────────────┘
```

---

## 3. Project structure

```
riot-wasm-ble/
├── DOCUMENTATION.md            ← this document
├── README.md                   ← short version / quick start
├── firmware/
│   ├── Makefile                RIOT app definition (BOARD=esp32c6-devkit, incl. periph_gpio)
│   ├── wamr_config.cmake       WAMR build options (no WASI, fast interpreter)
│   ├── main.c                  Init order: WAMR runner before the BLE service
│   ├── ble_wasm_xfer.{c,h}     GATT service: state machine, CRC32, notify
│   └── wamr_runner.{c,h}       Worker thread: WAMR runtime, native functions (log + GPIO)
├── wasm-modules/
│   ├── demo_module.c           Test module: run(i32)->i32, calls native env_log()
│   ├── demo_module.wasm        precompiled result (183 bytes)
│   ├── blink_module.c          Test module: toggles a GPIO pin via native functions
│   ├── blink_module.wasm       precompiled result (261 bytes)
│   └── build.sh                clang/wasm-ld build script
├── host-tool/
│   ├── ble_wasm_push.py        Upload tool (Python + bleak)
│   └── requirements.txt
└── build-proof/
    ├── wasm_ble_loader-esp32c6-devkit.bin   real, linked flash image
    └── esp32c6_build_log.txt                excerpt of the build log
```

---

## 4. BLE GATT protocol — full specification

### 4.1 Service & characteristics

Project-private 128-bit UUIDs (generated via `uuid4` — **not** official
Bluetooth SIG UUIDs, reserved exclusively for this project):

| Element | UUID | Properties | Byte layout |
|---|---|---|---|
| Service | `3e8f5f79-970b-4a1b-a5c1-8769323967a1` | — | — |
| Control characteristic | `b99687ea-c917-44d1-b6a8-92195be40df9` | `WRITE` | see 4.2 |
| Data characteristic | `c4b871ee-f32c-47f4-9c81-aa5024afeaae` | `WRITE`, `WRITE_NO_RESPONSE` | raw bytecode chunks |
| Status characteristic | `cf2060e9-0f4f-4198-8be3-38ad73d717ff` | `READ`, `NOTIFY` | see 4.3 |

### 4.2 Control opcodes

| Opcode | Name | Payload | Effect |
|---|---|---|---|
| `0x01` | `CMD_BEGIN` | 4 bytes `size` (LE) + 4 bytes `crc32` (LE) | starts a new transfer, resets the receive buffer |
| `0x02` | `CMD_COMMIT` | — | verifies size + CRC32, hands the module to the WAMR worker |
| `0x03` | `CMD_ABORT` | — | cancels the transfer in progress, returns to `IDLE` |

### 4.3 Status structure (10 bytes, `packed`, little-endian)

```c
struct __attribute__((packed)) {
    uint8_t  state;      // see state table below
    uint8_t  error;       // see error table below
    uint32_t received;    // bytes received so far
    uint32_t expected;     // total bytes expected (from BEGIN)
};
```

**States** (`wasm_xfer_state_t`):

| Value | Name | Meaning |
|---|---|---|
| 0 | `IDLE` | ready for `CMD_BEGIN` |
| 1 | `RECEIVING` | accepting data packets |
| 2 | `READY` | (internal, usually superseded by `RUNNING` immediately) |
| 3 | `RUNNING` | the WAMR worker is currently executing the module |
| 4 | `ERROR` | see the `error` field |

**Errors** (`wasm_xfer_error_t`):

| Value | Name | Cause |
|---|---|---|
| 0 | `NONE` | — |
| 1 | `TOO_LARGE` | `size` in `BEGIN` > `WASM_XFER_MAX_SIZE` |
| 2 | `OVERFLOW` | more data received than announced in `BEGIN` |
| 3 | `SIZE_MISMATCH` | `COMMIT` before the transfer completed, or without an active `BEGIN` |
| 4 | `CRC_MISMATCH` | CRC32 over the received data ≠ the announced CRC32 |
| 5 | `BUSY` | `BEGIN` while `RECEIVING`/`RUNNING` |
| 6 | `RUNTIME` | WAMR: load/instantiate/execute failed |

### 4.4 Flow diagram (state machine)

```
        CMD_BEGIN (size, crc32)
   ┌─────────────────────────────┐
   │                             ▼
┌──────┐                   ┌───────────┐   data chunks until
│ IDLE │◀── notify_run_done ┤ RECEIVING │◀── received == expected
└──────┘   (success)        └─────┬─────┘
   ▲                              │ CMD_COMMIT
   │                              ▼
   │                        CRC check
   │                       ┌──────┴──────┐
   │                  ok   │             │  mismatch/overflow/
   │                       ▼             ▼  size mismatch
   │                 ┌───────────┐  ┌───────┐
   │  notify_run_done│ RUNNING   │  │ ERROR │
   └──────────────────┤(WAMR runs)│  └───┬───┘
     (error)          └───────────┘      │ CMD_ABORT or
                                          │ new CMD_BEGIN
                                          └──────────────▶ IDLE
```

Disconnecting during `RECEIVING` automatically triggers the same behavior
as `CMD_ABORT` (see `_gap_event_cb`, `BLE_GAP_EVENT_DISCONNECT` handler).

### 4.5 Typical message flow (from the real hardware test)

```
Host                                    Device
 │  connect                               │
 │────────────────────────────────────────▶│  [ble] client connected
 │  write Control: BEGIN(183, 0xb42061ec) │
 │────────────────────────────────────────▶│  [xfer] BEGIN: expecting 183 bytes
 │  write Data chunk 1 (100 bytes)        │
 │────────────────────────────────────────▶│
 │  write Data chunk 2 (83 bytes)         │
 │────────────────────────────────────────▶│
 │  write Control: COMMIT                 │
 │────────────────────────────────────────▶│  [xfer] CRC ok, handing to WAMR
 │                                         │  [wamr] executing module (183 bytes)
 │                                         │  [wasm -> host] env_log(21)
 │                                         │  [wasm -> host] env_log(231)
 │  ◀── notify: state=IDLE                │  [wamr] return value = 231
 │◀────────────────────────────────────────│
 │  disconnect                            │
 │────────────────────────────────────────▶│  [ble] client disconnected
```

---

## 5. Firmware components in detail

### 5.1 `main.c`

Wires up only the init order — deliberately minimal:

```c
wamr_runner_init();   // 1. the WAMR runtime + worker thread MUST come up first
ble_wasm_xfer_init();  // 2. only then register the GATT service + advertise
```

Ordering matters: if the GATT service were already accepting uploads
before the WAMR worker existed, a `COMMIT` right after boot would go
nowhere (`wamr_runner_submit()` fails because `_runner_pid ==
KERNEL_PID_UNDEF`).

### 5.2 `ble_wasm_xfer.c` / `.h`

**Public API:**

| Function | Purpose |
|---|---|
| `void ble_wasm_xfer_init(void)` | registers the GATT service, starts advertising |
| `void ble_wasm_xfer_notify_run_done(bool success, int32_t result)` | called by the WAMR worker once execution has finished |

**Internal structure:**

- Static receive buffer `_rx_buf[WASM_XFER_MAX_SIZE]` (default 24 kB,
  configurable via `CFLAGS` in the Makefile) — its address stays stable
  for the lifetime of the program and is handed off 1:1 to the WAMR
  worker.
- The `_status` structure (see 4.3) is updated on every relevant state
  change and reported to connected clients via
  `ble_gattc_notify_custom()`. For data chunks, notifications are **not**
  sent on every single packet (that would flood the already-limited BLE
  throughput) but only every 8th packet, or once the transfer is
  complete.
- CRC32 is a small, dependency-free implementation of its own (standard
  polynomial `0xEDB88320`, reflected algorithm) — no external dependency
  tree required.
- GAP event handling: on `BLE_GAP_EVENT_DISCONNECT`, an in-progress
  `RECEIVING` transfer is automatically canceled and advertising is
  restarted (`nimble_autoadv_start(NULL)`), so the device is immediately
  discoverable again.

### 5.3 `wamr_runner.c` / `.h`

**Public API:**

| Function | Purpose |
|---|---|
| `int wamr_runner_init(void)` | initializes the WAMR runtime, starts the worker thread |
| `int wamr_runner_submit(uint8_t *buf, size_t len)` | hands a module off for execution (non-blocking) |

**Flow inside the worker thread:**

1. `wasm_runtime_full_init()` — once, with `RuntimeInitArgs` that also
   register the native function table (`_native_symbols`) under the
   import module name `"env"`.
2. Infinite loop: `msg_receive()` blocks until a job arrives.
3. Per job: `wasm_runtime_load()` → `wasm_runtime_instantiate()` →
   `wasm_runtime_lookup_function(inst, "run")` → if found:
   `wasm_runtime_create_exec_env()` + `wasm_runtime_call_wasm()` with a
   fixed test argument (`21`). If **no** `"run"` export exists: fall back
   to `wasm_application_execute_main()` (for modules built more in the
   classic `main()` style — without reading a WASI exit code, since WASI
   is disabled in this project).
4. Clean up (`wasm_runtime_deinstantiate`, `wasm_runtime_unload`), report
   the result back via `ble_wasm_xfer_notify_run_done()`.

**Native host functions:**

```c
static void native_env_log(wasm_exec_env_t exec_env, int32_t value)
{
    printf("[wasm -> host] env_log(%" PRId32 ")\n", value);
}
```

Signature `"(i)"` = one `i32` parameter, no return value — WAMR calls this
function automatically with `exec_env` as the implicit first parameter
whenever the WASM module calls the matching imported function `env_log`.

Since the GPIO extension, three more native functions have been added
(`native_gpio_mode`, `native_gpio_write`, `native_gpio_read`, signatures
`"(ii)i"` and `"(i)i"` respectively). All three first check the passed-in
`pin` index against a static whitelist `_allowed_pins[]` — so WASM never
sees a raw RIOT `gpio_t`, only `0..N-1`. For details and the security
rationale see [section 6](#6-wasm-module-contract) and
[section 11](#11-security-considerations).

---

## 6. WASM module contract

For a `.wasm` module to be executable by this loader, it must:

1. **Export a function `run`** with signature `(i32) -> i32`.
   (Alternatively: a classic `main()`, invoked via
   `wasm_application_execute_main()` — though in that case there's no
   usable return value in this prototype, since WASI is disabled.)
2. **Use the import module name `"env"`** if it wants to call native host
   functions (the default behavior of `clang --target=wasm32-unknown-unknown`
   for external symbols not defined in the module).
3. Natively available functions today:

   | Function | Signature (WASM) | Return values |
   |---|---|---|
   | `env_log(i32)` | `(i32) -> ()` | — (logs to the serial console) |
   | `gpio_mode(i32 pin, i32 mode)` | `(i32, i32) -> i32` | `0` ok, `-1` invalid pin/mode |
   | `gpio_write(i32 pin, i32 value)` | `(i32, i32) -> i32` | `0` ok, `-1` invalid pin, `-2` not initialized |
   | `gpio_read(i32 pin)` | `(i32) -> i32` | `0`/`1` level, negative on error |

   **Important:** `pin` is **not** a raw port/pin value, it's an index
   `0..N-1` into a firmware-side whitelist (`_allowed_pins[]` in
   `wamr_runner.c`). On the ESP32-C6 DevKit, this currently maps: index
   0-3 → GPIO18-21 (strapping pins GPIO8/9 and the boot button are
   deliberately excluded — still, check against your own board's
   schematic before connecting hardware, since different DevKit variants
   expose different pins). `gpio_mode()` must be called before
   `gpio_write()`/`gpio_read()` on a pin, otherwise `-2` is returned.

**Minimal example — logging** (`demo_module.c`):

```c
extern void env_log(int value);

__attribute__((visibility("default")))
int run(int x)
{
    env_log(x);
    int result = 0;
    for (int i = 1; i <= x; i++) result += i;
    env_log(result);
    return result;
}
```

**Minimal example — GPIO** (`blink_module.c`):

```c
extern void env_log(int value);
extern int  gpio_mode(int pin, int mode);
extern int  gpio_write(int pin, int value);
extern int  gpio_read(int pin);

__attribute__((visibility("default")))
int run(int toggle_count)
{
    int rc = gpio_mode(/*pin=*/0, /*mode=OUT*/0);
    if (rc != 0) { env_log(rc); return rc; }

    int state = 0;
    for (int i = 0; i < toggle_count; i++) {
        state = !state;
        gpio_write(0, state);
        env_log(state);
    }
    return gpio_read(0);
}
```

Functionally verified (see section 9): run against stubbed
`gpio_mode`/`gpio_write`/`gpio_read` host functions in `wasmtime` —
`run(5)` toggles the pin five times and correctly returns the final state
`1`, `env_log` records each transition `[1,0,1,0,1]`.

**Build command** (see `wasm-modules/build.sh` for the full set of flags):

```sh
clang --target=wasm32-unknown-unknown-wasm -Os -flto -fvisibility=hidden \
      -ffunction-sections -fdata-sections -c demo_module.c -o demo_module.o
wasm-ld -o demo_module.wasm -z stack-size=4096 --export=run \
        --export=__heap_base --export=__data_end --allow-undefined \
        --strip-all --gc-sections --initial-memory=65536 --no-entry \
        demo_module.o
```

Verified (see section 9): the resulting `demo_module.wasm` imports exactly
`env.env_log` and exports exactly `run`, `memory`, `__data_end`,
`__heap_base`.

---

## 7. Host tool (`ble_wasm_push.py`)

Python script built on [`bleak`](https://github.com/hbldh/bleak) (a
cross-platform BLE library for Linux/macOS/Windows).

**Flow:**

1. Scans by name for `RIOT-WASM-Loader` (or uses `--address` to skip
   scanning).
2. Computes the CRC32 over the `.wasm` file (`zlib.crc32`).
3. Writes `CMD_BEGIN` with size + CRC32 to the control characteristic.
4. Writes the file in configurable chunks (default 100 bytes,
   `--chunk-size`) via `write-without-response` to the data characteristic.
5. Writes `CMD_COMMIT`.
6. Subscribes to notifications on the status characteristic and waits
   until `state == IDLE` (success) or `state == ERROR` is reported.

**Usage:**

```sh
pip install -r requirements.txt
python3 ble_wasm_push.py ../wasm-modules/demo_module.wasm
python3 ble_wasm_push.py --address F0:F5:BD:09:E4:B1 --chunk-size 180 module.wasm
```

---

## 8. Build, toolchain & flashing

### 8.1 Prerequisites

- A local RIOT checkout (`RIOTBASE` set).
- A RISC-V toolchain for the ESP32-C6 (`riscv32-esp-elf-gcc`).
- ESP-IDF is **automatically** pulled by RIOT's package system from
  `github.com/espressif/esp-idf` on the first build (version v5.4, lots of
  submodules — the first build takes correspondingly longer).

### 8.2 Full command sequence

```sh
git clone --depth 1 https://github.com/RIOT-OS/RIOT.git
export RIOTBASE=$(pwd)/RIOT

curl -L -o riscv32-esp-elf.tar.xz \
  https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20241119/riscv32-esp-elf-14.2.0_20241119-x86_64-linux-gnu.tar.xz
mkdir riscv32-esp-elf && tar -C riscv32-esp-elf --strip-components 1 -xJf riscv32-esp-elf.tar.xz
export PATH=$PATH:$(pwd)/riscv32-esp-elf/bin

cd firmware
make BOARD=esp32c6-devkit flash term
```

### 8.3 Relevant Makefile knobs

| CFLAGS define | Default | Meaning |
|---|---|---|
| `WASM_XFER_MAX_SIZE` | 24 kB | size of the receive buffer for one complete module |
| `WAMR_RUNNER_STACKSIZE` | 8 kB | stack size of the worker thread (C stack, not the WASM instance) |
| `CONFIG_NIMBLE_AUTOADV_DEVICE_NAME` | `"RIOT-WASM-Loader"` | name under which the device shows up when scanned |

Additionally hard-coded in `wamr_runner.c` (not via CFLAGS):
`WASM_INSTANCE_STACK_SIZE` and `WASM_INSTANCE_HEAP_SIZE` (8 kB each) —
these are the stack/heap **inside** the WASM instance, independent of the
runner's C thread stack.

---

## 9. Verification report

This project wasn't just written — it was checked step by step against
real code and real hardware. Chronologically:

### Step 1 — API cross-check against real RIOT source

The RIOT repository was cloned and the official examples were read:
`examples/networking/ble/nimble/nimble_gatt`,
`examples/networking/ble/nimble/nimble_heart_rate_sensor`,
`examples/lang_support/community/wasm`. The GATT service definitions, GAP
callbacks, and WAMR calls in this project follow their APIs 1:1.

### Step 2 — Type-checking against real WAMR headers

`wasm_export.h` / `lib_export.h` were pulled from exactly the WAMR commit
that RIOT's `pkg/wamr/Makefile` pins
(`8af155076c6c62d9766ede640cf3f29fa73a4b53`). `wamr_runner.c` was checked
against it with `clang -fsyntax-only -Wall -Wextra`:

```
0 errors, 0 warnings
```

### Step 3 — WASM module actually compiled & executed

`demo_module.c` was built into a real 183-byte `.wasm` with clang/wasm-ld.
Parsed and run with `wasmtime` (as a stand-in runtime):

```
Exports: ['memory', 'run', '__data_end', '__heap_base']
Imports: [('env', 'env_log')]
run(21) = 231   (expected: sum(1..21) = 231)
env_log was called with: [21, 231]
```

### Step 4 — Complete RIOT build for `BOARD=nrf52dk`

Before the ESP32 toolchain was available: an ARM cross-toolchain
(`gcc-arm-none-eabi` + `newlib-arm-none-eabi`) was installed and a
complete build was run for a different NimBLE-capable board (Cortex-M4) —
the same `pkg/nimble`, the same `pkg/wamr` (for `THUMB_VFP` instead of
native x86), the same three source files.

**Result:** the build initially failed with a real linker error:

```
undefined reference to `wasm_runtime_get_wasi_exit_code'
```

Cause: the WASI fallback path in `wamr_runner.c` called a function that
only exists if WAMR is built with `WAMR_BUILD_LIBC_WASI=1` — but that's
deliberately disabled in `wamr_config.cmake`. **Fix:** the fallback path
no longer reads a WASI exit code, it only checks for exceptions. After the
fix: a clean build and link.

```
   text    data     bss     dec     hex filename
 150492    2476   43968  196936   30148 wasm_ble_loader.elf
```

### Step 5 — Complete RIOT build for `BOARD=esp32c6-devkit`

The RISC-V toolchain (`riscv32-esp-elf-gcc` 14.2.0, from Espressif's
GitHub releases) was installed; RIOT automatically pulled ESP-IDF v5.4.
The build completed cleanly **with the already-fixed code**:

```
   text    data     bss     dec     hex filename
 509702  115712  538310 1163724  11c1cc wasm_ble_loader.elf
Creating ESP32C6 image...
Successfully created ESP32C6 image.
```

No region overflow when linking. `esptool` produced a finished, flashable
image. **Note:** the `build-proof/wasm_ble_loader-esp32c6-devkit.bin` file
in this package was overwritten after the GPIO extension (step 7) with
the more recent build there — so it reflects the final state including
GPIO support, SHA-256
`aa187441cd419c277f494e0d7067501db087b01d5203e1c4e77256cc8097b08f`,
`text/data/bss = 510122/116064/537958` (see step 7 for the comparison).

### Step 6 — Real hardware test

See [section 10](#10-live-hardware-test--transcript) — firmware flashed
onto a real ESP32-C6 DevKit, booted, connected over BLE, module uploaded
and executed. **All steps succeeded.**

### Step 7 — GPIO extension: syntax check + real rebuild

After the hardware test, GPIO access from WASM was added (pin whitelist +
three native functions `gpio_mode`/`gpio_write`/`gpio_read` in
`wamr_runner.c`, the `periph_gpio` module in the Makefile). Again checked
in two stages before the code was considered "done":

1. `clang -fsyntax-only -Wall -Wextra` against stub headers with the real
   RIOT `periph/gpio.h` signatures (`gpio_init`, `gpio_read`,
   `gpio_write`, the `gpio_mode_t` enum) — **0 errors, 0 warnings**.
2. **Complete rebuild for `BOARD=esp32c6-devkit`** with the same toolchain
   as in step 5 — ran incrementally (only the changed files plus the
   newly enabled `periph_gpio` module were rebuilt) and **linked without
   errors**:

   ```
      text    data     bss     dec     hex filename
    510122  116064  537958 1164144  11c370 wasm_ble_loader.elf
   ```

   Compared to step 5 (`509702/115712/538310`): a minimal growth of about
   400 bytes text/data for the three new functions, `.bss` even slightly
   smaller (layout noise) — no sign of resource pressure from the
   extension.

3. **`blink_module.c` compiled** (261-byte `.wasm`) and run with
   `wasmtime` against stubbed `gpio_mode`/`gpio_write`/`gpio_read` host
   functions: `run(5)` toggles the (simulated) pin five times
   (`0→1→0→1→0→1`) and correctly returns the final state `1`, `env_log`
   records every intermediate value `[1, 0, 1, 0, 1]`.

**Still untested:** the GPIO extension on real hardware (which physical
pins on the DevKit actually correspond to GPIO18-21, whether an LED can be
connected there, etc., has not been verified — only the firmware-side
build and the pure execution logic have been).

### Summary: what is (still) unverified

- **Long-term stability / memory fragmentation** across many consecutive
  uploads.
- **Multiple simultaneous connections** (the design explicitly supports
  only one transfer at a time).
- **Actual throughput with larger modules** (the test case was a very
  small 183 bytes — connection-interval overhead dominates at such small
  data volumes; the picture changes for, say, 10 kB modules).
- **BLE 5 Data Length Extension** was not explicitly negotiated/tested
  (the host tool uses conservative 100-byte chunks).
- **GPIO on real hardware**: the build and execution logic are verified
  (see step 7), but whether GPIO18-21 are actually freely usable,
  hazard-free pins on the specific DevKit module at hand hasn't yet been
  checked on real hardware with, say, an attached LED.

---

## 10. Live hardware test — transcript

Firmware boot (serial console, `esptool`/RIOT output):

```
detected chip: boya
flash io: dio

main(): This is RIOT! (Version: 2026.10-devel-92-g918894)
=== RIOT WASM-over-BLE Loader Prototype ===
[wamr] runtime ready, waiting for modules...
Ready. Device is advertising as 'RIOT-WASM-Loader'.
```

*("detected chip: boya" is `esptool` identifying the flash chip vendor on
the board — Boya Microelectronics — not an error.)*

Host tool output:

```
Scanning for 'RIOT-WASM-Loader' ...
Found: F0:F5:BD:09:E4:B1
Module: ../wasm-modules/demo_module.wasm (183 bytes, crc32=0xb42061ec)
Sending data in 100-byte chunks ...
  status: RECEIVING  received=183/183 error=NONE
Waiting for the execution result ...
  status: IDLE       received=0/0 error=NONE
Module uploaded and executed successfully.
```

Firmware console during the transfer:

```
[ble] client connected
[xfer] BEGIN: expecting 183 bytes, crc32=0xb42061ec
[xfer] CRC ok, handing module to the WAMR worker
[wamr] executing module (183 bytes)
[wasm -> host] env_log(21)
[wasm -> host] env_log(231)
[wamr] module finished, return value = 231
[ble] client disconnected, aborting transfer in progress
```

**Timing breakdown:**

| Phase | Duration (from timestamps) | Comment |
|---|---|---|
| Connect → BEGIN | ~1.5 s | BLE connection setup + service discovery by `bleak` |
| BEGIN → CRC ok | ~143 ms | 183 bytes in 2 chunks — at such small data volumes, connection-interval overhead dominates |
| CRC ok → result | < 5 ms | WAMR interpreter for a trivial module on the RISC-V core |

The "client disconnected" event afterwards is expected behavior from
`bleak`'s `async with BleakClient(...)` context manager, which
automatically closes the connection once the Python script finishes
running — not an error case, since the state had already returned to
`IDLE` by then.

---

## 11. Security considerations

**CRC32 protects against transmission errors, not malicious modules.**
That's a deliberate design decision for a lab prototype, but a clear
point to address before any deployment outside a controlled network:

- **No pairing/bonding enabled.** Currently any BLE device in range can
  connect and upload code for execution. For anything outside your own
  lab: enable at least BLE bonding with a passkey.
- **No signature verification.** An attacker with a connection can inject
  arbitrary bytecode (as long as it runs inside the WAMR sandbox). A
  sensible addition: an Ed25519 signature over the bytecode, verified
  **before** `wamr_runner_submit()` is ever called.
- **The WAMR sandbox as an isolation layer.** WASM code has no direct
  memory access outside its linear memory and can only call functions
  that were explicitly registered as native symbols (here: only
  `env_log`, plus the GPIO functions). That significantly limits the
  damage a malicious module can do — it can't simply read or write
  arbitrary registers/RAM addresses — but it does not replace
  authenticating the source.
- **No rate limiting / multi-client protection.** A single client can
  keep the device busy with repeated malformed uploads (not a security
  issue per se, but an availability concern).
- **GPIO whitelisting already implemented** (see section 6): WASM modules
  never get raw pin numbers, only indices into a fixed, firmware-side
  list of safe pins — this prevents a module (accidentally or
  maliciously) from touching strapping pins, the flash SPI bus, or the
  UART console. This whitelist pattern should be reused consistently for
  every additional peripheral (ADC, I2C, PWM, etc.).

---

## 12. Known limitations & roadmap

| Area | Current state | Possible next step |
|---|---|---|
| Concurrency | One transfer at a time | Multiple independent buffers + client tracking |
| Security | CRC32 only, no pairing | Ed25519 signature + BLE bonding |
| Throughput | Conservative 100-byte chunks | Negotiate the DLE handshake, raise `--chunk-size` dynamically |
| Hardware access from WASM | `env_log` (logging) + `gpio_mode`/`gpio_write`/`gpio_read` (GPIO18-21 whitelisted) | Wire up/test on real hardware (LED, sensor); add more peripherals (ADC, I2C, PWM) following the same whitelist pattern |
| RAM budget | 24 kB module buffer + 2×8 kB WAMR instance | Strip unused ESP-IDF components (Wi-Fi etc.) from the build as needed to make room for larger modules |
| Robustness | No automatic restart on a WAMR crash | Watchdog / restart strategy for the worker thread |

---

## 13. Appendix

### 13.1 CRC32 reference implementation (firmware, `ble_wasm_xfer.c`)

```c
static uint32_t _crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
```

Compatible with `zlib.crc32()` (Python, used by the host tool) —
standard CRC-32/ISO-HDLC, reflected, init `0xFFFFFFFF`, XOR-out
`0xFFFFFFFF`.

### 13.2 Full UUID reference

```
Service:  3e8f5f79-970b-4a1b-a5c1-8769323967a1
Control:  b99687ea-c917-44d1-b6a8-92195be40df9
Data:     c4b871ee-f32c-47f4-9c81-aa5024afeaae
Status:   cf2060e9-0f4f-4198-8be3-38ad73d717ff
```

### 13.3 External package versions used (pinned by RIOT)

| Package | Source | Version/commit |
|---|---|---|
| NimBLE | `github.com/apache/mynewt-nimble` | `719bd3c435b728f07ce7aaffaf6cebbd9c659a46` |
| WAMR | `github.com/bytecodealliance/wasm-micro-runtime` | `8af155076c6c62d9766ede640cf3f29fa73a4b53` |
| ESP-IDF | `github.com/espressif/esp-idf` | `c8bb53292d08d6449a09823cf554e62ac839cd8c` (v5.4) |

### 13.4 Tested toolchain versions

| Toolchain | Version |
|---|---|
| `riscv32-esp-elf-gcc` (ESP32-C6 build) | 14.2.0 (crosstool-NG esp-14.2.0_20241119) |
| `arm-none-eabi-gcc` (nrf52dk intermediate step) | 13.2.1 |
| `clang`/`wasm-ld` (WASM module build) | 18 |

### 13.5 Glossary

| Term | Meaning |
|---|---|
| **GATT** | Generic Attribute Profile — the BLE data model for services/characteristics |
| **NimBLE** | Apache Mynewt's BLE host/controller stack, integrated into RIOT as `pkg/nimble` |
| **WAMR** | WebAssembly Micro Runtime — a lean WASM interpreter for embedded systems |
| **WASI** | WebAssembly System Interface — a POSIX-like environment for WASM, disabled here |
| **DLE** | Data Length Extension — a BLE 5 feature for larger packets per connection event |
| **Import module (WASM)** | the namespace under which an external function is referenced in a `.wasm` module (here: `"env"`) |
