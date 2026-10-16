#include "devicekind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built-in kinds: the ones that live in src/framework (not in a module). */
static const HermesDeviceKind HERMES_BUILTIN_DEVICE_KINDS[] = {
    /* internal device: a hermes_threads.h thread IS a device (see
     * src/framework/devices/hermes_device_thread.h) -- same constructors,
     * same handle type, just also reachable via `.join()` etc. */
    { "thread_start",    "thread", "hermes_thread_handle_t" },
    { "thread_start_ex", "thread", "hermes_thread_handle_t" },

    /* core device over the UART driver (src/framework/devices/hermes_uart.h,
     * `import hermes_uart`): communication buses are core, not modules. */
    { "uart", "uart", "hermes_device_uart_handle_t" },
    { "uart_port", "uart", "hermes_device_uart_handle_t" },
    /* a network interface (hermes_net.h): the host's own sockets; modules
     * add others (modules/w5500: w5500(...)) */
    { "net_host", "net", "hermes_device_net_handle_t" },
    /* the SPI bus (hermes_spi.h) */
    { "spi", "spi", "hermes_device_spi_handle_t" },
    /* reliable packets over a serial port (hermes_packet.h) */
    { "packet_link", "packet", "hermes_device_packet_handle_t" },
    /* core devices over the ADC and PWM drivers (src/framework/devices/
     * hermes_adc.h, hermes_pwm.h; `import hermes_adc` / `import hermes_pwm`) */
    { "adc", "adc", "hermes_device_adc_handle_t" },
    { "pwm", "pwm", "hermes_device_pwm_handle_t" },
    /* every hardware timer, with its role (hermes_timers.h, `import hermes_timers`) */
    { "timer", "timer", "hermes_device_timer_handle_t" },
    /* the I2C bus, as master or as slave (hermes_i2c.h, `import hermes_i2c`) */
    { "i2c", "i2c", "hermes_device_i2c_handle_t" },
    { "i2c_slave", "i2c_slave", "hermes_device_i2c_slave_handle_t" },
};
#define HERMES_BUILTIN_DEVICE_KIND_COUNT \
    ((int)(sizeof(HERMES_BUILTIN_DEVICE_KINDS) / sizeof(HERMES_BUILTIN_DEVICE_KINDS[0])))

/* Methods that create a device: `device x = recv.method(...)`, where recv
 * is a device of kind `receiver_kind`. */
static const HermesDeviceMethodKind HERMES_DEVICE_METHOD_KINDS[] = {
    /* a peripheral of an I2C bus: device imu = bus.at(0x68) (hermes_i2c.h) */
    { "i2c", "at", "i2c_dev", "hermes_device_i2c_dev_handle_t" },
    /* a chip of an SPI bus: device chip = bus.at(10) (hermes_spi.h) */
    { "spi", "at", "spi_dev", "hermes_device_spi_dev_handle_t" },
    /* sockets of a network interface (hermes_net.h) */
    { "net", "tcp", "tcp", "hermes_device_tcp_handle_t" },
    { "net", "udp", "udp", "hermes_device_udp_handle_t" },
    { "net", "listen", "tcp_server", "hermes_device_tcp_server_handle_t" },
    { "tcp_server", "accept", "tcp", "hermes_device_tcp_handle_t" },
};

const HermesDeviceKind *hermes_devicekind_lookup_method(const char *receiver_kind, const char *method,
                                                        HermesDeviceKind *out) {
    for (size_t i = 0; i < sizeof(HERMES_DEVICE_METHOD_KINDS) / sizeof(HERMES_DEVICE_METHOD_KINDS[0]); i++) {
        const HermesDeviceMethodKind *m = &HERMES_DEVICE_METHOD_KINDS[i];
        if (strcmp(m->receiver_kind, receiver_kind) != 0 || strcmp(m->method, method) != 0) continue;
        out->constructor_name = m->method;
        out->kind = m->kind;
        out->c_type = m->c_type;
        return out;
    }
    return NULL;
}

/* Kinds added at transpile time by imported modules (module.c). */
static HermesDeviceKind g_module_kinds[HERMES_MAX_MODULE_DEVICE_KINDS];
static int g_module_kind_count = 0;

int hermes_devicekind_is_core_kind(const char *kind) {
    for (int i = 0; i < HERMES_BUILTIN_DEVICE_KIND_COUNT; i++)
        if (strcmp(HERMES_BUILTIN_DEVICE_KINDS[i].kind, kind) == 0) return 1;
    return 0;
}

const HermesDeviceKind *hermes_devicekind_lookup_constructor(const char *constructor_name) {
    for (int i = 0; i < HERMES_BUILTIN_DEVICE_KIND_COUNT; i++) {
        if (strcmp(HERMES_BUILTIN_DEVICE_KINDS[i].constructor_name, constructor_name) == 0) {
            return &HERMES_BUILTIN_DEVICE_KINDS[i];
        }
    }
    for (int i = 0; i < g_module_kind_count; i++) {
        if (strcmp(g_module_kinds[i].constructor_name, constructor_name) == 0) {
            return &g_module_kinds[i];
        }
    }
    return NULL;
}

int hermes_devicekind_register(const char *constructor_name, const char *kind, const char *c_type) {
    if (hermes_devicekind_lookup_constructor(constructor_name)) {
        fprintf(stderr, "hepyc: device constructor '%s' is already defined\n", constructor_name);
        return 0;
    }
    if (g_module_kind_count >= HERMES_MAX_MODULE_DEVICE_KINDS) {
        fprintf(stderr, "hepyc: too many device constructors in modules (at most %d)\n",
                HERMES_MAX_MODULE_DEVICE_KINDS);
        return 0;
    }
    HermesDeviceKind *k = &g_module_kinds[g_module_kind_count++];
    k->constructor_name = constructor_name;
    k->kind = kind;
    k->c_type = c_type;
    return 1;
}
