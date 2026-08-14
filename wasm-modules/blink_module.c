/*
 * Demo module: drives a GPIO pin through the native GPIO functions.
 *
 * "WASM pin 0" is an index into the firmware-side whitelist (see
 * wamr_runner.c, _allowed_pins[]), NOT a raw port/pin number. On the
 * ESP32-C6 DevKit, index 0 currently corresponds to GPIO18.
 *
 * Imported host functions (module "env", see wamr_runner.c):
 *   gpio_mode(pin, mode)   -> 0 ok, -1 invalid pin/mode
 *   gpio_write(pin, value) -> 0 ok, -1 invalid pin, -2 not initialized
 *   gpio_read(pin)         -> 0/1 level, or negative on error
 *   env_log(value)         -> debug output on the serial console
 */

extern void env_log(int value);
extern int  gpio_mode(int pin, int mode);
extern int  gpio_write(int pin, int value);
extern int  gpio_read(int pin);

#define WASM_PIN_0     0
#define MODE_OUT       0

__attribute__((visibility("default")))
int run(int toggle_count)
{
    int rc = gpio_mode(WASM_PIN_0, MODE_OUT);
    if (rc != 0) {
        env_log(rc); /* negative error code -> pin not allowed/configurable */
        return rc;
    }

    int state = 0;
    for (int i = 0; i < toggle_count; i++) {
        state = !state;
        gpio_write(WASM_PIN_0, state);
        env_log(state); /* the host sees every transition on the console */
    }

    return gpio_read(WASM_PIN_0);
}
