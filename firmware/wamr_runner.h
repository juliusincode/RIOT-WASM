/*
 * Dedicated thread that executes incoming WASM modules.
 *
 * Important: NEVER trigger WAMR execution directly from the NimBLE GATT
 * callback! The callback runs in the context of the NimBLE host thread; a
 * blocking or long-running call there would stall the whole BLE stack
 * (timeouts, dropped connections). Instead, the buffer is handed off to
 * this worker thread via an IPC message.
 */
#ifndef WAMR_RUNNER_H
#define WAMR_RUNNER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Initializes the WAMR runtime (once) and starts the worker
 *          thread. Must be called before the first wamr_runner_submit().
 *
 * @return  0 on success, <0 on error
 */
int wamr_runner_init(void);

/**
 * @brief   Hands a fully received and CRC-verified WASM module off to the
 *          worker thread for execution.
 *
 *          The buffer passed in is modified in place by the worker thread
 *          (WAMR builds its module layout inside it) and must not be
 *          written to by anyone else until the
 *          ble_wasm_xfer_notify_run_done() callback fires.
 *
 * @param[in] buf   pointer to the bytecode (stays allocated by the caller)
 * @param[in] len   length of the bytecode in bytes
 *
 * @return  0 if the request was accepted, <0 on error (e.g. the worker is
 *          still busy with a previous module)
 */
int wamr_runner_submit(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* WAMR_RUNNER_H */
