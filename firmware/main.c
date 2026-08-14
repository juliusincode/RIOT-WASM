/*
 * Prototype: load WASM modules onto a RIOT device over BLE and execute them
 * with WAMR. See README.md for the protocol description and test walkthrough.
 */

#include <stdio.h>

#include "ble_wasm_xfer.h"
#include "wamr_runner.h"

int main(void)
{
    puts("=== RIOT WASM-over-BLE Loader Prototype ===");

    /* Ordering matters: the WAMR runtime and worker thread must be up
     * before the GATT service can accept uploads, otherwise a COMMIT
     * right after boot would have nowhere to go. */
    if (wamr_runner_init() != 0) {
        puts("FATAL: failed to start the WAMR worker thread");
        return 1;
    }

    ble_wasm_xfer_init();

    puts("Ready. Device is advertising as 'RIOT-WASM-Loader'.");

    /* Everything else happens event-driven in the NimBLE and WAMR worker
     * threads; the main thread is no longer needed.
     * (No shell loop here to keep main() minimal -- the shell/
     * shell_cmds_default modules are still linked in, though, so a debug
     * shell can be added if needed; see README.md.) */

    return 0;
}
