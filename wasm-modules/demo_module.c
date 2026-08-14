/*
 * Demo WASM module for the RIOT BLE loader prototype.
 *
 * Exports a function "run(i32) -> i32" that gets uploaded onto the device
 * via BLE and is then invoked by wamr_runner.c.
 *
 * "env_log" is not actually defined in this module -- it's only declared
 * (extern, default visibility), so the compiler/linker leaves it as an
 * import from the WASM module "env". On the device, wamr_runner.c provides
 * exactly this function as a native host function (see native_env_log()).
 */

extern void env_log(int value);

__attribute__((visibility("default")))
int run(int x)
{
    env_log(x);          /* have the host log this value (RIOT serial console) */

    int result = 0;
    for (int i = 1; i <= x; i++) {
        result += i;     /* small "payload": sum of 1..x */
    }

    env_log(result);      /* log the result as well */
    return result;
}
