/*
 * WAMR integration.
 *
 * API usage deliberately stays close to RIOT's own WAMR example
 * (examples/lang_support/community/wasm/iwasmt.c), extended with:
 *   - registration of a native host function (env_log), callable from WASM
 *   - execution on a dedicated, long-lived thread instead of init/teardown
 *     per call, so the runtime only has to be initialized once
 *   - calling an exported "run(i32) -> i32" function instead of a full
 *     WASI "main()", since our transport protocol has no concept of argv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "thread.h"
#include "msg.h"
#include "periph/gpio.h"

#include "wasm_export.h"

#include "wamr_runner.h"
#include "ble_wasm_xfer.h"

#ifndef WAMR_RUNNER_STACKSIZE
#define WAMR_RUNNER_STACKSIZE (8U * 1024U)
#endif

#define WAMR_RUNNER_PRIO      (THREAD_PRIORITY_MAIN - 1)
#define WAMR_RUNNER_MSG_QUEUE_SIZE  (4)

/* The WASM instance gets its own stack/heap inside its linear memory page,
 * independent of the runner's C thread stack. 8 kB is enough for small
 * demo modules -- see iwasmt.c for the same figure. */
#define WASM_INSTANCE_STACK_SIZE (8 * 1024)
#define WASM_INSTANCE_HEAP_SIZE  (8 * 1024)

static char _stack[WAMR_RUNNER_STACKSIZE];
static msg_t _msg_queue[WAMR_RUNNER_MSG_QUEUE_SIZE];
static kernel_pid_t _runner_pid = KERNEL_PID_UNDEF;

typedef struct {
    uint8_t *buf;
    size_t len;
} wasm_job_t;

/* Only one transfer at a time -> a single static job is enough for the
 * prototype */
static wasm_job_t _pending_job;

/* --- Native function callable from the WASM module ---
 *
 * WAMR's convention for wasm_runtime_register_natives(): the native C
 * function always receives the wasm_exec_env_t first, followed by the
 * parameters declared in the signature. Signature "(i)" = one i32
 * parameter, no return value.
 */
static void native_env_log(wasm_exec_env_t exec_env, int32_t value)
{
    (void)exec_env;
    printf("[wasm -> host] env_log(%" PRId32 ")\n", value);
}

/*
 * --- GPIO access from WASM ---
 *
 * IMPORTANT: WASM modules NEVER get raw port/pin numbers that would map
 * 1:1 onto GPIO_PIN(port, pin). Otherwise a module (accidentally or
 * maliciously) could drive strapping pins, the flash SPI bus, or the UART
 * console and brick the device. Instead, WASM only ever sees indices
 * 0..N-1 into this curated whitelist; the translation to real gpio_t
 * values happens exclusively here.
 *
 * NOTE: this specific pin selection (GPIO18-21) is a sensible starting
 * point for the ESP32-C6 DevKit (it avoids GPIO8/GPIO9, which are used as
 * boot strapping pins / BTN0, see arduino_iomap.h), but it should NOT be
 * reused unchecked for a different board variant (DevKitM-1 vs. DevKitC-1
 * expose different pins) -- always cross-check against your own board's
 * schematic before connecting hardware.
 */
static const gpio_t _allowed_pins[] = {
    GPIO18,  /* WASM pin 0 */
    GPIO19,  /* WASM pin 1 */
    GPIO20,  /* WASM pin 2 */
    GPIO21,  /* WASM pin 3 */
};
#define ALLOWED_PINS_NUMOF (sizeof(_allowed_pins) / sizeof(_allowed_pins[0]))

/* Which of the pins listed above have already been configured via
 * gpio_init() -- calling gpio_write()/gpio_read() on an uninitialized pin
 * would be undefined behavior. */
static bool _pin_initialized[ALLOWED_PINS_NUMOF];

/* Return values shared by all three functions: 0 = ok, -1 = invalid WASM
 * pin index, -2 = pin has not been configured via gpio_mode() yet */
#define GPIO_NATIVE_OK            0
#define GPIO_NATIVE_ERR_BAD_PIN  -1
#define GPIO_NATIVE_ERR_NOT_INIT -2

/* Mode encoding on the WASM side (deliberately our own, stable values
 * instead of passing RIOT's gpio_mode_t straight through -- this keeps
 * the module ABI stable even if RIOT's enum ever changes) */
#define WASM_GPIO_MODE_OUT     0
#define WASM_GPIO_MODE_IN      1
#define WASM_GPIO_MODE_IN_PU   2
#define WASM_GPIO_MODE_IN_PD   3

static int32_t native_gpio_mode(wasm_exec_env_t exec_env, int32_t pin, int32_t mode)
{
    (void)exec_env;
    if (pin < 0 || (uint32_t)pin >= ALLOWED_PINS_NUMOF) {
        printf("[gpio] rejected: invalid WASM pin %" PRId32 "\n", pin);
        return GPIO_NATIVE_ERR_BAD_PIN;
    }

    gpio_mode_t riot_mode;
    switch (mode) {
        case WASM_GPIO_MODE_OUT:   riot_mode = GPIO_OUT;    break;
        case WASM_GPIO_MODE_IN:    riot_mode = GPIO_IN;     break;
        case WASM_GPIO_MODE_IN_PU: riot_mode = GPIO_IN_PU;  break;
        case WASM_GPIO_MODE_IN_PD: riot_mode = GPIO_IN_PD;  break;
        default:
            printf("[gpio] rejected: invalid mode %" PRId32 "\n", mode);
            return GPIO_NATIVE_ERR_BAD_PIN;
    }

    if (gpio_init(_allowed_pins[pin], riot_mode) != 0) {
        puts("[gpio] gpio_init() failed");
        return GPIO_NATIVE_ERR_NOT_INIT;
    }
    _pin_initialized[pin] = true;
    printf("[gpio] pin %" PRId32 " initialized, mode %" PRId32 "\n", pin, mode);
    return GPIO_NATIVE_OK;
}

static int32_t native_gpio_write(wasm_exec_env_t exec_env, int32_t pin, int32_t value)
{
    (void)exec_env;
    if (pin < 0 || (uint32_t)pin >= ALLOWED_PINS_NUMOF) {
        return GPIO_NATIVE_ERR_BAD_PIN;
    }
    if (!_pin_initialized[pin]) {
        return GPIO_NATIVE_ERR_NOT_INIT;
    }
    gpio_write(_allowed_pins[pin], value != 0);
    return GPIO_NATIVE_OK;
}

static int32_t native_gpio_read(wasm_exec_env_t exec_env, int32_t pin)
{
    (void)exec_env;
    if (pin < 0 || (uint32_t)pin >= ALLOWED_PINS_NUMOF) {
        return GPIO_NATIVE_ERR_BAD_PIN;
    }
    if (!_pin_initialized[pin]) {
        return GPIO_NATIVE_ERR_NOT_INIT;
    }
    return gpio_read(_allowed_pins[pin]) ? 1 : 0;
}

static NativeSymbol _native_symbols[] = {
    { "env_log",    (void *)native_env_log,    "(i)",  NULL },
    { "gpio_mode",  (void *)native_gpio_mode,  "(ii)i", NULL },
    { "gpio_write", (void *)native_gpio_write, "(ii)i", NULL },
    { "gpio_read",  (void *)native_gpio_read,  "(i)i",  NULL },
};

static bool _runtime_init(void)
{
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));

    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    /* Native functions are registered under the import module name "env",
     * which is the default clang uses for wasm32-unknown-unknown when a
     * symbol is referenced but not defined in the module (see
     * wasm-modules/). */
    init_args.native_module_name = "env";
    init_args.native_symbols = _native_symbols;
    init_args.n_native_symbols =
        sizeof(_native_symbols) / sizeof(_native_symbols[0]);

    if (!wasm_runtime_full_init(&init_args)) {
        puts("[wamr] runtime initialization failed");
        return false;
    }
    return true;
}

/* Executes exactly one module: load -> instantiate -> look up "run" ->
 * call -> tear down. Returns the return value of "run" via *out_result. */
static bool _execute_module(uint8_t *buf, size_t len, int32_t *out_result)
{
    char error_buf[128];
    bool ok = false;

    wasm_module_t module = wasm_runtime_load(buf, len, error_buf, sizeof(error_buf));
    if (!module) {
        printf("[wamr] load failed: %s\n", error_buf);
        return false;
    }

    wasm_module_inst_t inst = wasm_runtime_instantiate(
        module, WASM_INSTANCE_STACK_SIZE, WASM_INSTANCE_HEAP_SIZE,
        error_buf, sizeof(error_buf));
    if (!inst) {
        printf("[wamr] instantiate failed: %s\n", error_buf);
        wasm_runtime_unload(module);
        return false;
    }

    wasm_function_inst_t func = wasm_runtime_lookup_function(inst, "run");
    if (func) {
        wasm_exec_env_t exec_env =
            wasm_runtime_create_exec_env(inst, WASM_INSTANCE_STACK_SIZE);
        if (exec_env) {
            /* one i32 argument goes in; the same slot receives the i32
             * return value on the way out (see wasm_runtime_call_wasm docs) */
            uint32_t wasm_argv[1] = { 21 };
            if (wasm_runtime_call_wasm(exec_env, func, 1, wasm_argv)) {
                *out_result = (int32_t)wasm_argv[0];
                ok = true;
            }
            else {
                const char *exc = wasm_runtime_get_exception(inst);
                printf("[wamr] execution failed: %s\n",
                       exc ? exc : "(unknown)");
            }
            wasm_runtime_destroy_exec_env(exec_env);
        }
    }
    else {
        /* Fallback: the module doesn't export "run", so try running a
         * WASI-style main() instead (e.g. if someone uploads the stock
         * hello-world example from RIOT's WAMR example).
         *
         * Note: wasm_runtime_get_wasi_exit_code() only exists if WAMR was
         * built with WAMR_BUILD_LIBC_WASI=1. Our wamr_config.cmake
         * deliberately disables WASI (see README.md) -> we only check for
         * exceptions here, we don't read back a real exit code. */
        puts("[wamr] no 'run' export found, trying 'main'");
        wasm_application_execute_main(inst, 0, NULL);
        const char *exc = wasm_runtime_get_exception(inst);
        if (exc) {
            printf("[wamr] main() execution failed: %s\n", exc);
        }
        else {
            *out_result = 0;
            ok = true;
        }
    }

    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(module);
    return ok;
}

static void *_runner_thread(void *arg)
{
    (void)arg;
    msg_init_queue(_msg_queue, WAMR_RUNNER_MSG_QUEUE_SIZE);

    if (!_runtime_init()) {
        /* Without a runtime the thread is useless; module execution will
         * always be reported as "failed" via ble_wasm_xfer, since submit()
         * fails as long as _runner_pid stays UNDEF. */
        return NULL;
    }
    puts("[wamr] runtime ready, waiting for modules...");

    msg_t msg;
    while (1) {
        msg_receive(&msg);
        wasm_job_t *job = (wasm_job_t *)msg.content.ptr;

        printf("[wamr] executing module (%u bytes)\n", (unsigned)job->len);
        int32_t result = 0;
        bool ok = _execute_module(job->buf, job->len, &result);
        if (ok) {
            printf("[wamr] module finished, return value = %" PRId32 "\n", result);
        }
        ble_wasm_xfer_notify_run_done(ok, result);
    }

    return NULL;
}

int wamr_runner_init(void)
{
    if (_runner_pid != KERNEL_PID_UNDEF) {
        return 0; /* already initialized */
    }

    _runner_pid = thread_create(_stack, sizeof(_stack), WAMR_RUNNER_PRIO,
                                 THREAD_CREATE_STACKTEST, _runner_thread,
                                 NULL, "wamr_runner");
    return (_runner_pid > KERNEL_PID_UNDEF) ? 0 : -1;
}

int wamr_runner_submit(uint8_t *buf, size_t len)
{
    if (_runner_pid == KERNEL_PID_UNDEF) {
        return -1;
    }

    _pending_job.buf = buf;
    _pending_job.len = len;

    msg_t msg = { .content.ptr = &_pending_job };
    /* msg_try_send instead of msg_send: must not block from the GATT
     * callback context. Since the worker thread only ever accepts one
     * module at a time by design (ble_wasm_xfer refuses new BEGINs while
     * RUNNING), the queue should practically never be full here. */
    return (msg_try_send(&msg, _runner_pid) == 1) ? 0 : -1;
}
