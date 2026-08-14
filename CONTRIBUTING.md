# Contributing

Thanks for taking a look at this project. It's an experimental prototype,
so contributions that keep that spirit — small, focused, well-explained —
are the most welcome kind.

## Before you open a PR

1. **Read `DOCUMENTATION.md` first.** It covers the protocol, the
   firmware layout, and the security model in detail — most design
   questions ("why not X?") are already answered there.
2. **Check `README.md`, section "Known limitations / next steps"** and
   `DOCUMENTATION.md`, section 12 ("Known limitations & roadmap") for
   ideas that are already on the radar.
3. If you're proposing a behavioral change (protocol, native function
   signatures, GPIO whitelist, etc.), please open an issue first to
   discuss it — the wire protocol is intentionally minimal and small
   changes can be breaking for anyone else already using it.

## Development workflow

- **Firmware (`firmware/`, `wasm-modules/`):** requires a local RIOT
  checkout and the ESP32-C6 RISC-V toolchain; see README.md, "Building &
  flashing". Before submitting firmware changes, please at minimum run:
  ```sh
  clang -fsyntax-only -Wall -Wextra -I<path-to-wamr-headers> firmware/wamr_runner.c
  ```
  and, if possible, a full `make BOARD=esp32c6-devkit all` (or
  `BOARD=nrf52dk`, which doesn't require the ESP-IDF toolchain and builds
  much faster for a quick sanity check).
- **Host tool (`host-tool/`):** plain Python 3, only dependency is
  `bleak`. Please keep it dependency-light — this tool is meant to be
  usable with a five-minute `pip install -r requirements.txt`, not a
  project of its own.
- **WASM modules (`wasm-modules/`):** verify with `wasmtime` (or an
  equivalent standalone WASM runtime) against stubbed host functions
  before assuming they'll work on-device — see `DOCUMENTATION.md`,
  section 9, for the pattern used so far.

## Code style

- C: follows RIOT's own conventions reasonably closely (4-space indent,
  `_leading_underscore` for static/file-local symbols, braces on their
  own logic, not a specific formatter). When in doubt, match the
  surrounding file.
- Python: PEP 8, no particular formatter enforced.
- Comments and documentation: English, please — this keeps the project
  accessible to the wider RIOT/WAMR communities it depends on.

## Reporting issues

Please include:
- Board (`BOARD=...`) and RIOT/ESP-IDF/WAMR commit or version, if
  relevant.
- Whether the issue reproduces in the firmware, the host tool, or both.
- Serial console output around the failure, if you have it — the
  firmware logs quite verbosely on purpose (see `DOCUMENTATION.md`,
  section 10, for what "normal" output looks like).

## Security

This is a lab prototype with **no authentication or signature
verification** by design (see `DOCUMENTATION.md`, section 11). If you
find an issue that's specific to a real deployment scenario (not just
"the lab prototype has no auth, which is documented"), feel free to open
it as a regular issue — there's no separate private disclosure process
for this project at this stage.
