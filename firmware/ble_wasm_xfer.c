/*
 * GATT service implementation. The API pattern (ble_gatt_svc_def table,
 * access_cb dispatch, GAP event handling via nimble_autoadv) follows the
 * official RIOT NimBLE examples exactly:
 *   examples/networking/ble/nimble/nimble_gatt/main.c
 *   examples/networking/ble/nimble/nimble_heart_rate_sensor/main.c
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "nimble_riot.h"
#include "nimble_autoadv.h"

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_wasm_xfer.h"
#include "wamr_runner.h"

#ifndef WASM_XFER_MAX_SIZE
#define WASM_XFER_MAX_SIZE (24U * 1024U)
#endif

/* --- Control characteristic opcodes --- */
#define CMD_BEGIN  0x01
#define CMD_COMMIT 0x02
#define CMD_ABORT  0x03

/* Project-private 128-bit UUIDs (generated via uuid4, not official Bluetooth
 * SIG UUIDs, reserved solely for this project) */
/* Service:  3e8f5f79-970b-4a1b-a5c1-8769323967a1 */
static const ble_uuid128_t _svc_uuid = BLE_UUID128_INIT(
    0xa1, 0x67, 0x39, 0x32, 0x69, 0x87, 0xc1, 0xa5,
    0x1b, 0x4a, 0x0b, 0x97, 0x79, 0x5f, 0x8f, 0x3e);

/* Control: b99687ea-c917-44d1-b6a8-92195be40df9 */
static const ble_uuid128_t _chr_control_uuid = BLE_UUID128_INIT(
    0xf9, 0x0d, 0xe4, 0x5b, 0x19, 0x92, 0xa8, 0xb6,
    0xd1, 0x44, 0x17, 0xc9, 0xea, 0x87, 0x96, 0xb9);

/* Data: c4b871ee-f32c-47f4-9c81-aa5024afeaae */
static const ble_uuid128_t _chr_data_uuid = BLE_UUID128_INIT(
    0xae, 0xea, 0xaf, 0x24, 0x50, 0xaa, 0x81, 0x9c,
    0xf4, 0x47, 0x2c, 0xf3, 0xee, 0x71, 0xb8, 0xc4);

/* Status: cf2060e9-0f4f-4198-8be3-38ad73d717ff */
static const ble_uuid128_t _chr_status_uuid = BLE_UUID128_INIT(
    0xff, 0x17, 0xd7, 0x73, 0xad, 0x38, 0xe3, 0x8b,
    0x98, 0x41, 0x4f, 0x0f, 0xe9, 0x60, 0x20, 0xcf);

/* Receive buffer for one complete WASM module. Statically allocated so its
 * address stays stable for the lifetime of the program -- the pointer is
 * handed off to the WAMR worker thread as-is. */
static uint8_t _rx_buf[WASM_XFER_MAX_SIZE];

static struct __attribute__((packed)) {
    uint8_t  state;
    uint8_t  error;
    uint32_t received;
    uint32_t expected;
} _status = { WASM_XFER_STATE_IDLE, WASM_XFER_ERR_NONE, 0, 0 };

static uint32_t _expected_crc;
static uint16_t _status_val_handle;
static uint16_t _conn_handle = BLE_HS_CONN_HANDLE_NONE;

static int _gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);
static int _gap_event_cb(struct ble_gap_event *event, void *arg);

static const struct ble_gatt_svc_def _gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (ble_uuid_t *)&_chr_control_uuid.u,
                .access_cb = _gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = (ble_uuid_t *)&_chr_data_uuid.u,
                .access_cb = _gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = (ble_uuid_t *)&_chr_status_uuid.u,
                .access_cb = _gatt_access_cb,
                .val_handle = &_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, /* end of characteristics list */
        },
    },
    { 0 }, /* end of service list */
};

/* --- small, dependency-free CRC32 (standard polynomial 0xEDB88320) --- */
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

static uint32_t _rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void _notify_status(void)
{
    if (_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&_status, sizeof(_status));
    if (om) {
        ble_gattc_notify_custom(_conn_handle, _status_val_handle, om);
    }
}

static void _set_error(wasm_xfer_error_t err)
{
    _status.state = WASM_XFER_STATE_ERROR;
    _status.error = err;
    _notify_status();
}

static void _handle_begin(const uint8_t *payload, size_t len)
{
    if (_status.state == WASM_XFER_STATE_RECEIVING ||
        _status.state == WASM_XFER_STATE_RUNNING) {
        _set_error(WASM_XFER_ERR_BUSY);
        return;
    }
    if (len < 8) {
        _set_error(WASM_XFER_ERR_SIZE_MISMATCH);
        return;
    }

    uint32_t size = _rd_u32le(payload);
    uint32_t crc  = _rd_u32le(payload + 4);

    if (size == 0 || size > WASM_XFER_MAX_SIZE) {
        printf("[xfer] BEGIN rejected: %" PRIu32 " bytes > buffer (%u)\n",
               size, (unsigned)WASM_XFER_MAX_SIZE);
        _set_error(WASM_XFER_ERR_TOO_LARGE);
        return;
    }

    _status.expected = size;
    _status.received = 0;
    _expected_crc = crc;
    _status.state = WASM_XFER_STATE_RECEIVING;
    _status.error = WASM_XFER_ERR_NONE;

    printf("[xfer] BEGIN: expecting %" PRIu32 " bytes, crc32=0x%08" PRIx32 "\n",
           size, crc);
    _notify_status();
}

static void _handle_data(const uint8_t *payload, size_t len)
{
    if (_status.state != WASM_XFER_STATE_RECEIVING) {
        /* Data without an active BEGIN -> ignore, don't spam status */
        return;
    }
    if (_status.received + len > _status.expected) {
        _set_error(WASM_XFER_ERR_OVERFLOW);
        return;
    }

    memcpy(_rx_buf + _status.received, payload, len);
    _status.received += len;

    /* Deliberately not notifying on every single chunk -- only
     * occasionally, so we don't flood the already-limited BLE throughput
     * with notifications. "Every 8th packet, or once complete" is good
     * enough for a prototype. */
    static uint8_t chunk_ctr;
    if ((++chunk_ctr % 8) == 0 || _status.received == _status.expected) {
        _notify_status();
    }
}

static void _handle_commit(void)
{
    if (_status.state != WASM_XFER_STATE_RECEIVING) {
        _set_error(WASM_XFER_ERR_SIZE_MISMATCH);
        return;
    }
    if (_status.received != _status.expected) {
        printf("[xfer] COMMIT too early: %" PRIu32 "/%" PRIu32 " bytes\n",
               _status.received, _status.expected);
        _set_error(WASM_XFER_ERR_SIZE_MISMATCH);
        return;
    }

    uint32_t actual_crc = _crc32(_rx_buf, _status.received);
    if (actual_crc != _expected_crc) {
        printf("[xfer] CRC mismatch: expected 0x%08" PRIx32
               ", computed 0x%08" PRIx32 "\n", _expected_crc, actual_crc);
        _set_error(WASM_XFER_ERR_CRC_MISMATCH);
        return;
    }

    puts("[xfer] CRC ok, handing module to the WAMR worker");
    _status.state = WASM_XFER_STATE_RUNNING;
    _notify_status();

    if (wamr_runner_submit(_rx_buf, _status.received) != 0) {
        puts("[xfer] WAMR worker was not ready (submit failed)");
        _set_error(WASM_XFER_ERR_RUNTIME);
    }
}

static void _handle_abort(void)
{
    _status.state = WASM_XFER_STATE_IDLE;
    _status.error = WASM_XFER_ERR_NONE;
    _status.received = 0;
    _status.expected = 0;
    puts("[xfer] transfer aborted");
    _notify_status();
}

void ble_wasm_xfer_notify_run_done(bool success, int32_t result)
{
    (void)result;
    _status.state = success ? WASM_XFER_STATE_IDLE : WASM_XFER_STATE_ERROR;
    if (!success) {
        _status.error = WASM_XFER_ERR_RUNTIME;
    }
    else {
        _status.error = WASM_XFER_ERR_NONE;
    }
    _status.received = 0;
    _status.expected = 0;
    _notify_status();
}

static int _gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ble_uuid_cmp(ctxt->chr->uuid, (ble_uuid_t *)&_chr_control_uuid.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[9]; /* 1 byte opcode + up to 8 bytes payload (BEGIN) */
        if (om_len == 0 || om_len > sizeof(buf)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint16_t copied;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &copied);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        switch (buf[0]) {
            case CMD_BEGIN:  _handle_begin(buf + 1, copied - 1); break;
            case CMD_COMMIT: _handle_commit(); break;
            case CMD_ABORT:  _handle_abort(); break;
            default:
                printf("[xfer] unknown opcode 0x%02x\n", buf[0]);
                return BLE_ATT_ERR_UNLIKELY;
        }
        return 0;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, (ble_uuid_t *)&_chr_data_uuid.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        /* max ATT payload at full BLE 5 Data Length Extension (251-byte PDU
         * minus the ATT opcode/handle overhead) */
        uint8_t tmp[247];
        if (om_len == 0 || om_len > sizeof(tmp)) {
            _set_error(WASM_XFER_ERR_OVERFLOW);
            return 0;
        }
        uint16_t copied;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &copied);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        _handle_data(tmp, copied);
        return 0;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, (ble_uuid_t *)&_chr_status_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            int rc = os_mbuf_append(ctxt->om, &_status, sizeof(_status));
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int _gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                _conn_handle = event->connect.conn_handle;
                puts("[ble] client connected");
            }
            else {
                nimble_autoadv_start(NULL);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            puts("[ble] client disconnected, aborting transfer in progress");
            _conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (_status.state == WASM_XFER_STATE_RECEIVING) {
                _handle_abort();
            }
            nimble_autoadv_start(NULL);
            break;

        default:
            break;
    }
    return 0;
}

void ble_wasm_xfer_init(void)
{
    int rc = ble_gatts_count_cfg(_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(_gatt_svcs);
    assert(rc == 0);
    (void)rc;

    ble_svc_gap_device_name_set(CONFIG_NIMBLE_AUTOADV_DEVICE_NAME);
    ble_gatts_start();

    nimble_autoadv_cfg_t cfg = {
        .adv_duration_ms = BLE_HS_FOREVER,
        .adv_itvl_ms = BLE_GAP_ADV_ITVL_MS(100),
        .flags = NIMBLE_AUTOADV_FLAG_CONNECTABLE |
                 NIMBLE_AUTOADV_FLAG_LEGACY |
                 NIMBLE_AUTOADV_FLAG_SCANNABLE,
        .channel_map = 0,
        .filter_policy = 0,
        .own_addr_type = nimble_riot_own_addr_type,
        .phy = NIMBLE_PHY_1M,
        .tx_power = 0,
    };
    nimble_autoadv_cfg_update(&cfg);
    nimble_autoadv_set_gap_cb(&_gap_event_cb, NULL);

    nimble_autoadv_start(NULL);
}
