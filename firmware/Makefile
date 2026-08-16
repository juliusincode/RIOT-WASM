## Prototype: exchanging WASM modules over BLE on RIOT OS (ESP32-C6)
#
# Structure: a NimBLE GATT service accepts WASM bytecode in chunks,
# validates it via CRC32, and hands it off to a dedicated WAMR worker thread.

APPLICATION = wasm_ble_loader
RIOTBASE = /home/julius/RIOT
# Target board: generic ESP32-C6 DevKit (RISC-V, BLE 5 via NimBLE/HCI)
BOARD ?= esp32c6-devkit

# Path to a local RIOT checkout. Set this via the environment, e.g.:
#   export RIOTBASE=/path/to/RIOT
ifndef RIOTBASE
$(error RIOTBASE is not set. Point it at your RIOT checkout, e.g. \
  `export RIOTBASE=/path/to/RIOT` -- see README.md, section "Build & flash")
endif

# --- BLE: NimBLE as a peripheral with automatic advertising ---
USEPKG    += nimble
USEMODULE += nimble_svc_gap
USEMODULE += nimble_svc_gatt
USEMODULE += nimble_autoadv

# --- GPIO: lets WASM modules drive pins through native functions ---
USEMODULE += periph_gpio

CFLAGS += -DCONFIG_NIMBLE_AUTOADV_DEVICE_NAME='"RIOT-WASM-Loader"'
# We configure GAP/GATT ourselves and start advertising manually once
# everything is registered (see main.c).
CFLAGS += -DCONFIG_NIMBLE_AUTOADV_START_MANUALLY=1

# --- WASM: the WAMR package (experimental in RIOT) ---
USEPKG += wamr
export WAMR_CONFIG := $(abspath wamr_config.cmake)

# --- Convenience: shell for debugging over USB-Serial-JTAG ---
USEMODULE += shell
USEMODULE += shell_cmds_default
USEMODULE += ps

# The WAMR interpreter thread needs noticeably more stack than the default.
# The value below is deliberately generous; for a production build, measure
# actual usage (ps / thread statistics) and trim it down to what's needed.
CFLAGS += -DWAMR_RUNNER_STACKSIZE='(8U*1024U)'

# Receive buffer for one complete WASM module (keep the ESP32-C6's RAM
# budget in mind: ~512 kB SRAM total, and WAMR itself also needs heap/stack).
CFLAGS += -DWASM_XFER_MAX_SIZE='(24U*1024U)'

# Keep safety checks enabled during development
DEVELHELP ?= 1
QUIET ?= 1

include $(RIOTBASE)/Makefile.include
