/*
 * BLE GATT service for transferring WASM modules onto a RIOT device.
 *
 * Protocol (modeled after classic BLE DFU/OTA services):
 *
 *   Control characteristic (write):
 *     Byte 0        = opcode
 *     CMD_BEGIN  (0x01): + 4 bytes size (LE) + 4 bytes crc32 (LE)
 *     CMD_COMMIT (0x02): no payload -> verifies the CRC and starts the module
 *     CMD_ABORT  (0x03): no payload -> cancels the transfer in progress
 *
 *   Data characteristic (write / write-without-response):
 *     raw bytes, appended to the receive buffer sequentially
 *
 *   Status characteristic (read + notify):
 *     Byte 0        = state (see wasm_xfer_state_t)
 *     Byte 1        = last error code (see wasm_xfer_error_t)
 *     Byte 2..5     = bytes received so far (u32 LE)
 *     Byte 6..9     = expected total bytes (u32 LE)
 */
#ifndef BLE_WASM_XFER_H
#define BLE_WASM_XFER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WASM_XFER_STATE_IDLE      = 0,  /**< ready for CMD_BEGIN */
    WASM_XFER_STATE_RECEIVING = 1,  /**< accepting data packets */
    WASM_XFER_STATE_READY     = 2,  /**< CRC ok, module waiting to be executed */
    WASM_XFER_STATE_RUNNING   = 3,  /**< WAMR worker is currently executing the module */
    WASM_XFER_STATE_ERROR     = 4,  /**< see the error field for details */
} wasm_xfer_state_t;

typedef enum {
    WASM_XFER_ERR_NONE          = 0,
    WASM_XFER_ERR_TOO_LARGE     = 1, /**< size > WASM_XFER_MAX_SIZE */
    WASM_XFER_ERR_OVERFLOW      = 2, /**< more data received than announced */
    WASM_XFER_ERR_SIZE_MISMATCH = 3, /**< COMMIT before the transfer completed */
    WASM_XFER_ERR_CRC_MISMATCH  = 4, /**< CRC32 does not match */
    WASM_XFER_ERR_BUSY          = 5, /**< BEGIN while RUNNING/READY */
    WASM_XFER_ERR_RUNTIME       = 6, /**< WAMR: load/instantiate/execute failed */
} wasm_xfer_error_t;

/**
 * @brief   Registers the GATT service and initializes internal state.
 *          Must be called before the GATT server starts (ble_gatts_start()).
 */
void ble_wasm_xfer_init(void);

/**
 * @brief   Called by the WAMR worker thread once execution of a previously
 *          transferred module has finished (whether it succeeded or not).
 *
 * @param[in] success   true if the module ran without error
 * @param[in] result    return value of the WASM function (only if success)
 */
void ble_wasm_xfer_notify_run_done(bool success, int32_t result);

#ifdef __cplusplus
}
#endif
#endif /* BLE_WASM_XFER_H */
