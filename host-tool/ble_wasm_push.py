#!/usr/bin/env python3
"""
Uploads a .wasm module over BLE to the RIOT prototype (wasm_ble_loader).

Protocol (must match firmware/ble_wasm_xfer.c exactly):

  Control characteristic (write):
    CMD_BEGIN  (0x01) + size:u32-LE + crc32:u32-LE
    CMD_COMMIT (0x02)
    CMD_ABORT  (0x03)

  Data characteristic (write-without-response):
    raw bytecode chunks, sequential

  Status characteristic (read + notify):
    state:u8, error:u8, received:u32-LE, expected:u32-LE

Dependency: pip install bleak
"""

import argparse
import asyncio
import struct
import sys
import zlib

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "RIOT-WASM-Loader"

# must match the UUIDs in firmware/ble_wasm_xfer.c exactly
UUID_CONTROL = "b99687ea-c917-44d1-b6a8-92195be40df9"
UUID_DATA    = "c4b871ee-f32c-47f4-9c81-aa5024afeaae"
UUID_STATUS  = "cf2060e9-0f4f-4198-8be3-38ad73d717ff"

CMD_BEGIN, CMD_COMMIT, CMD_ABORT = 0x01, 0x02, 0x03

STATE_NAMES = {0: "IDLE", 1: "RECEIVING", 2: "READY", 3: "RUNNING", 4: "ERROR"}
ERROR_NAMES = {
    0: "NONE", 1: "TOO_LARGE", 2: "OVERFLOW", 3: "SIZE_MISMATCH",
    4: "CRC_MISMATCH", 5: "BUSY", 6: "RUNTIME",
}

# Conservative default chunk size. If BLE 5 Data Length Extension has been
# negotiated successfully, --chunk-size can be raised to ~244 bytes.
DEFAULT_CHUNK_SIZE = 100


def parse_status(data: bytes):
    state, error, received, expected = struct.unpack("<BBII", data)
    return state, error, received, expected


async def find_device(timeout: float):
    print(f"Scanning for '{DEVICE_NAME}' ...")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)
    if dev is None:
        sys.exit(f"Device '{DEVICE_NAME}' not found (timeout {timeout}s).")
    print(f"Found: {dev.address}")
    return dev


async def push_module(address: str, wasm_path: str, chunk_size: int):
    with open(wasm_path, "rb") as f:
        payload = f.read()
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    print(f"Module: {wasm_path} ({len(payload)} bytes, crc32=0x{crc:08x})")

    done = asyncio.get_event_loop().create_future()

    def on_status(_handle, data: bytearray):
        state, error, received, expected = parse_status(bytes(data))
        state_s = STATE_NAMES.get(state, f"?{state}")
        err_s = ERROR_NAMES.get(error, f"?{error}")
        print(f"  status: {state_s:<10} received={received}/{expected} error={err_s}")
        if state in (0, 4) and not done.done():  # IDLE (done) or ERROR
            done.set_result(state == 0)

    async with BleakClient(address) as client:
        await client.start_notify(UUID_STATUS, on_status)

        begin_payload = bytes([CMD_BEGIN]) + struct.pack("<II", len(payload), crc)
        await client.write_gatt_char(UUID_CONTROL, begin_payload, response=True)

        print(f"Sending data in {chunk_size}-byte chunks ...")
        for offset in range(0, len(payload), chunk_size):
            chunk = payload[offset:offset + chunk_size]
            await client.write_gatt_char(UUID_DATA, chunk, response=False)

        await client.write_gatt_char(UUID_CONTROL, bytes([CMD_COMMIT]), response=True)

        print("Waiting for the execution result ...")
        success = await asyncio.wait_for(done, timeout=15.0)
        await client.stop_notify(UUID_STATUS)

        if success:
            print("Module uploaded and executed successfully.")
        else:
            print("Transfer or execution failed, see the status log above.")
            sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("wasm_file", help="Path to the .wasm file")
    ap.add_argument("--address", help="BLE address (skips scanning)")
    ap.add_argument("--scan-timeout", type=float, default=10.0)
    ap.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE,
                     help="Bytes per data write (default: %(default)s)")
    args = ap.parse_args()

    async def run():
        address = args.address
        if not address:
            dev = await find_device(args.scan_timeout)
            address = dev.address
        await push_module(address, args.wasm_file, args.chunk_size)

    asyncio.run(run())


if __name__ == "__main__":
    main()
