#ifndef HERMES_DEVICE_H
#define HERMES_DEVICE_H

/* Common base for every "device" in HERMES: the abstraction, per the
 * original specification (docs/raw/Definicion.es.md, "Gestion de
 * recursos"), for anything the system manages as an object with its own
 * data and methods -- external hardware (sensors, actuators, other
 * microcontrollers) as well as internal resources (threads, in the
 * future maybe other runtime primitives).
 *
 * Every concrete device struct embeds this as its FIRST member:
 *
 *   typedef struct {
 *       hermes_device_t device;   // must be first
 *       ... kind-specific fields ...
 *   } hermes_device_whatever_t;
 *
 * That makes a `hermes_device_whatever_t *` safely reinterpretable as a
 * `hermes_device_t *` (the struct's address IS its base member's address,
 * same idiom used throughout C for this -- e.g. BSD's sockaddr/sockaddr_in).
 * This base is deliberately NOT a vtable: HERMESPY's `receiver.method(args)`
 * syntax is resolved entirely at transpile time, to a plain function call
 * (`device_<kind>_<method>(receiver, args)`), based on the receiver's
 * statically-known kind -- see src/engine/compiler/devicekind.c and
 * docs/raw/hermes_devices.en.md.
 *
 * --- device classes (open set, no central enum) --------------------------
 *
 * Each device kind owns one `hermes_device_class_t` descriptor and every
 * instance points at it. A kind is identified by the descriptor's
 * *address*, so a module (modules/<name>/, see docs/raw/hermes_modules.en.md)
 * can add a new kind without touching any shared file. Rule: define each
 * descriptor exactly once in the whole program (in the module's .c, or
 * `static` in a header that only the generated .c includes). Descriptors
 * live in RAM (not const): they hold their kind's list.
 *
 * --- tracking: one linked list per kind -----------------------------------
 *
 * Every device is registered at construction time in its kind's list, in
 * creation order. The list is intrusive: the link (`next`) lives inside
 * each device, so tracking needs no allocation of its own -- if a device
 * could be created, it is tracked; tracking can't fail separately or
 * overflow. No limit on how many devices exist, no transpile-time count,
 * no restriction on where devices are created (loops, recursion, C code).
 * Cost: one pointer per device + head/tail/count per kind.
 *
 * Kinds with at least one registered device are themselves chained
 * (`next_class`, in order of first registration), so every device of every
 * kind can be walked. Access by index walks the list (O(n)); appending is
 * O(1); removal walks to the device (singly linked: one pointer per device
 * instead of two).
 *
 * A class may also keep a per-kind hardware limit (`max_instances`: one
 * UART exists, 20 pins exist) -- not a memory limit.
 *
 * --- abort: mandatory for every device kind -------------------------------
 *
 * Every kind MUST define how to deactivate one of its devices, as the
 * method `device_<kind>_abort(handle)` (so `x.abort()` works in HERMESPY)
 * and as the class's `abort` pointer (so Total_abort() can reach it without
 * knowing the kind). Enforced three ways:
 *   - define the class with HERMES_DEVICE_CLASS below: it doesn't compile
 *     unless device_<kind>_abort exists;
 *   - hepyc rejects a module whose header doesn't define it (module.c);
 *   - hermes_device_register refuses a class with no `abort` pointer.
 * "Deactivate" = leave the hardware in a safe, inert state (outputs LOW,
 * motors stopped...) -- not free its memory.
 *
 * hermes_device_abort_all() (`Total_abort()` in HERMESPY) aborts every
 * registered device of every kind. The framework's fatal-error path
 * (hermes_fatal) calls it before abort(), so a crash leaves actuators safe.
 *
 * --- tick: optional background work per kind -------------------------------
 *
 * A kind may also have a `tick`: work done for each of its devices every
 * `tick_ms` ms (debouncing a button, sampling a sensor, a motor ramp...),
 * as `device_<kind>_tick(handle)`; define the class with
 * HERMES_DEVICE_CLASS_TICK instead. Only under hermes_os, while os_run()
 * runs (hermes_device_ticks_on): the OS tick activates the OS and the OS
 * calls it -- outside the interrupt, but in the OS's own context, which is
 * no thread: it must be short and never block or yield. Lock NOT held
 * (it may take it). The period is counted in ms, checked on every OS
 * tick (os_tick_ms): never earlier, late by up to one OS tick.
 * A device with a tick is never released while the ticks run
 * (hermes_device_destroy waits for them).
 *
 * --- describe: mandatory for every device kind -----------------------------
 *
 * `device_<kind>_describe(handle, out)` tells what one device holds: the
 * hardware pins it uses (`out->pins`, for conflict detection), the memory
 * it takes (`out->bytes`, for devices_memory()) and up to
 * HERMES_DEVICE_FIELDS details for the dump (`mode=out`; HERMES_DEVICE_VALUE/
 * TEXT, compiled only with `import hermes_devices`). It only
 * fills `out` (no output, no lock, nothing that can fail): the printing is
 * common (hermes_device_dump) and only linked if the program dumps.
 * Enforced like abort: the HERMES_DEVICE_CLASS macros don't compile
 * without it, and hepyc rejects a module that doesn't define it.
 *
 * --- hardware conflicts -----------------------------------------------------
 *
 * Every constructor of a device that takes pins calls
 * hermes_device_pin_claim for each of them first (lock held): if another
 * device's describe lists that pin, it's a fatal error naming that kind
 * ("gpio(9): pin 9 is already used by a pwm device"). No table of its own:
 * the device lists are the record (no RAM per pin).
 *
 * --- destroy: removing a device ---------------------------------------------
 *
 * hermes_device_destroy(d, release): the kind's release (abort, then leave
 * the hardware free: a pin back to input), out of its list, memory freed.
 * Each kind that can be destroyed defines `device_<kind>_destroy` on top;
 * hepyc then leaves the variable at 0 (`x.destroy()`). Threads have their
 * own destroy (hermes_thread_destroy); main and the process can't be
 * destroyed.
 *
 * --- handles: what HERMESPY holds -------------------------------------------
 *
 * A device variable (`device g = gpio(13)`) holds a HANDLE, not a pointer
 * (decided with the user, 2026-10-15): the device's serial number, given
 * when it's registered from a counter that only grows (decided with the
 * user over a table of slots with generations, which cost the Uno the RAM
 * three threads and the OS need). A destroyed device leaves its kind's
 * list, so its handle -- the variable itself is left at 0, but a copy may
 * remain -- is found nowhere any more, even when a new device lives in the
 * same memory: it has another serial; and nothing reads the freed memory
 * to find out. Every method resolves its handle first (HERMES_DEVICE_GET:
 * a walk of its kind's list): a stale handle, 0, or one of another kind is
 * a fatal error with its message. A serial repeats only after the counter
 * wraps: 65535 devices created on AVR (16 bits), 4 294 967 295 elsewhere.
 * Inside the framework (C) devices are still plain pointers;
 * hermes_device_handle(d) is a device's own handle. The mandatory
 * device_<kind>_abort / _describe (and _tick) take the handle like any
 * method: the kind's class calls them with hermes_device_handle(d).
 * The serial takes the place of the name a device used to carry (no RAM
 * more): a device's name is its kind's ("main" for the main thread, see
 * hermes_device_name).
 */

#include "hermes_diag.h" /* HERMES_PSTR, for HERMES_DEVICE_GET */
#include <stdint.h>

typedef struct hermes_device hermes_device_t;

/* The serial number every device gets (never 0), 16 bits on AVR, 32
 * elsewhere -- the handle when the mechanism is on. */
#if defined(__AVR__)
typedef uint16_t hermes_serial_t;
#else
typedef uint32_t hermes_serial_t;
#endif

/* What a HERMESPY device variable holds (see "handles" above): with the
 * mechanism on (HERMES_HANDLES), the serial; off (the default), the
 * device's address. 0 / NULL (HERMES_HANDLE_NONE) is "no device", what a
 * destroyed device's variable holds. */
#ifdef HERMES_HANDLES
typedef hermes_serial_t hermes_handle_t;
#else
typedef hermes_device_t *hermes_handle_t;
#endif
#define HERMES_HANDLE_NONE ((hermes_handle_t)0)

/* Constant texts of describe (keys, values): in flash on AVR (a literal
 * would be copied to RAM), read where they are elsewhere. Unlike
 * HERMES_PSTR they exist with or without the UART: the dump goes out like
 * print(). */
#if defined(__AVR__)
#include <avr/pgmspace.h>
#define HERMES_DEVICE_STR(s) PSTR(s)
#else
#define HERMES_DEVICE_STR(s) (s)
#endif

#define HERMES_DEVICE_FIELDS 3

typedef struct {
    const char *key;  /* HERMES_DEVICE_STR */
    const char *text; /* HERMES_DEVICE_STR, or NULL: `value` is printed */
    long value;
} hermes_device_field_t;

/* Filled by a kind's describe; the caller sets everything to "none" first
 * (pins -1, bytes 0, no fields). */
typedef struct {
    signed char pins[3];  /* hardware pins it holds (-1: none; an SPI bus has 3) */
    unsigned char nfields;
    unsigned long bytes;  /* memory it takes */
    hermes_device_field_t field[HERMES_DEVICE_FIELDS];
} hermes_device_desc_t;

/* The dump's fields, in a describe: HERMES_DEVICE_VALUE(out, "pin", 13),
 * HERMES_DEVICE_TEXT(out, "mode", HERMES_DEVICE_STR("out")). Only the dump
 * reads them, and only `import hermes_devices` can dump: hepyc then passes
 * -DHERMES_DEVICES. Without it they're nothing at all -- not even their
 * arguments are evaluated -- so a describe is only its pins and bytes
 * (all conflict detection needs). */
#ifdef HERMES_DEVICES
void hermes_device_desc_value(hermes_device_desc_t *o, const char *key, long value);
void hermes_device_desc_text(hermes_device_desc_t *o, const char *key, const char *text);
#define HERMES_DEVICE_VALUE(o, key, value) hermes_device_desc_value((o), HERMES_DEVICE_STR(key), (value))
#define HERMES_DEVICE_TEXT(o, key, text) hermes_device_desc_text((o), HERMES_DEVICE_STR(key), (text))
#else
#define HERMES_DEVICE_VALUE(o, key, value) ((void)0)
#define HERMES_DEVICE_TEXT(o, key, text) ((void)0)
#endif

typedef struct hermes_device_class {
    const char *name;            /* kind name, e.g. "thread", "gpio" */
    unsigned char max_instances; /* hardware ceiling; 0 = none */
    void (*abort)(hermes_device_t *d); /* MANDATORY: deactivate one device */
    void (*tick)(hermes_device_t *d);  /* optional background work (NULL: none) */
    unsigned short tick_ms;            /* ... every this many ms */
    /* MANDATORY: what one device holds (see "describe" above) */
    void (*describe)(hermes_device_t *d, hermes_device_desc_t *out);

    /* runtime state -- leave out of the initializer (zero) */
    hermes_device_t *head;       /* oldest device of this kind */
    hermes_device_t *tail;       /* newest: O(1) append in creation order */
    unsigned short count;
    unsigned char linked;        /* already in the chain of kinds */
    struct hermes_device_class *next_class;
    unsigned long tick_next;     /* OS ms of its next tick */
} hermes_device_class_t;

struct hermes_device {
    hermes_device_class_t *cls;
    hermes_serial_t serial; /* see "handles" above; 0: not registered */
    hermes_device_t *next; /* next device of the same kind (list link) */
};

typedef enum {
    HERMES_DEVICE_OK = 0,
    HERMES_DEVICE_ERR_CLASS_LIMIT, /* this kind's hardware max_instances reached */
    HERMES_DEVICE_ERR_NO_ABORT,    /* the class has no `abort` (mandatory) */
    HERMES_DEVICE_ERR_DUPLICATE,   /* this exact device already registered */
} hermes_device_status_t;

/* Defines the class descriptor `hermes_device_class_<kind>` for a kind
 * whose handle type is `handle_t` and whose abort method is
 * `device_<kind>_abort(handle_t)` -- which must already be declared: if it
 * isn't, this doesn't compile. Use it (after the abort method) in every
 * header that defines a device kind included only by the generated .c:
 *   HERMES_DEVICE_CLASS(gpio, hermes_device_gpio_handle_t, HERMES_GPIO_PIN_COUNT); */
#define HERMES_DEVICE_CLASS(kind, handle_t, max)                                   \
    static void hermes_device_abort_entry_##kind(hermes_device_t *d) {             \
        /* taking its address (not calling it) makes a missing or misdeclared \
         * device_<kind>_abort a hard compile error, not an implicit decl */      \
        void (*const kind_abort)(handle_t) = device_##kind##_abort;                \
        kind_abort(hermes_device_handle(d));                                   \
    }                                                                              \
    HERMES_DEVICE_DESCRIBE_ENTRY_(kind, handle_t)                                  \
    static hermes_device_class_t hermes_device_class_##kind = {                    \
        .name = #kind, .max_instances = (max), .abort = hermes_device_abort_entry_##kind, \
        .describe = hermes_device_describe_entry_##kind                            \
    }

/* (both macros) device_<kind>_describe(handle_t, hermes_device_desc_t *) */
#define HERMES_DEVICE_DESCRIBE_ENTRY_(kind, handle_t)                              \
    static void hermes_device_describe_entry_##kind(hermes_device_t *d,            \
                                                    hermes_device_desc_t *o) {     \
        void (*const kind_describe)(handle_t, hermes_device_desc_t *) =            \
            device_##kind##_describe;                                              \
        kind_describe(hermes_device_handle(d), o);                             \
    }

/* Same, for a kind with background work: `device_<kind>_tick(handle_t)`
 * must be declared too, and is called every `period_ms` ms (1-65535)
 * while the OS runs (see "tick" above). */
#define HERMES_DEVICE_CLASS_TICK(kind, handle_t, max, period_ms)                   \
    static void hermes_device_abort_entry_##kind(hermes_device_t *d) {             \
        void (*const kind_abort)(handle_t) = device_##kind##_abort;                \
        kind_abort(hermes_device_handle(d));                                   \
    }                                                                              \
    static void hermes_device_tick_entry_##kind(hermes_device_t *d) {              \
        void (*const kind_tick)(handle_t) = device_##kind##_tick;                  \
        kind_tick(hermes_device_handle(d));                                    \
    }                                                                              \
    HERMES_DEVICE_DESCRIBE_ENTRY_(kind, handle_t)                                  \
    static hermes_device_class_t hermes_device_class_##kind = {                    \
        .name = #kind, .max_instances = (max), .abort = hermes_device_abort_entry_##kind, \
        .tick = hermes_device_tick_entry_##kind, .tick_ms = (period_ms),           \
        .describe = hermes_device_describe_entry_##kind                            \
    }

/* --- device ticks (for hermes_os) ------------------------------------------
 * ticks_on: set by the OS while os_run() runs (device ticks are being
 * delivered); a kind can fall back to doing without them otherwise.
 * tick_due: is some kind's tick due at OS ms `now`? (from the OS tick's
 * interrupt; cheap). tick_run: runs every due one (OS context, lock not
 * held). */
extern volatile unsigned char hermes_device_ticks_on;
int hermes_device_tick_due(unsigned long now);
void hermes_device_tick_run(unsigned long now);

/* Sets d->cls, gives it its serial (its handle) and appends `d` to its
 * kind's list. On any error `d` is left unregistered and nothing changes. */
hermes_device_status_t hermes_device_register(hermes_device_t *d,
                                              hermes_device_class_t *cls);

/* Same, but a failure is fatal (diagnostic on the host, then abort()) --
 * the contract every device constructor already has for "out of memory". */
void hermes_device_register_or_abort(hermes_device_t *d,
                                     hermes_device_class_t *cls);

/* Removes `d` from its kind's list, keeping the rest in creation order.
 * Returns 1 if it was registered, 0 otherwise. Used by the destroys
 * (hermes_device_destroy, hermes_thread_destroy). */
int hermes_device_unregister(hermes_device_t *d);

/* Destroys a device that isn't a thread: waits until no device tick is
 * running (another core's OS), calls `release(d)` (the kind's abort and
 * whatever frees its hardware; NULL = nothing), takes it out of its list
 * and frees its memory (hermes_runtime_alloc'd). All under the lock. */
void hermes_device_destroy(hermes_device_t *d, void (*release)(hermes_device_t *d));

/* --- handles (see "handles" above) -------------------------------------------
 * hermes_handle_get: the device behind `h`; a fatal error naming the kind
 * and the method if there's none (`cls`: the kind it must be, NULL = any).
 * hermes_handle_peek: the same without the error (NULL). Any core, any
 * lock depth. hermes_device_handle: d's handle.
 * One version or the other is compiled, never both (no test at run time):
 *   mechanism on  -- hermes_device.c: the serial looked up in the kind's
 *                    list; stale, 0 or another kind's: the error
 *   off (default) -- here, inline: the address itself; only 0 (the
 *                    variable of a destroyed device) is the error
 * hermes_handle_gone: that error for 0 (both versions). */
void hermes_handle_gone(const hermes_device_class_t *cls, const char *kind_P, const char *method_P)
    __attribute__((noreturn));
#ifdef HERMES_HANDLES
hermes_device_t *hermes_handle_get(hermes_handle_t h, const hermes_device_class_t *cls,
                                   const char *kind_P, const char *method_P);
hermes_device_t *hermes_handle_peek(hermes_handle_t h);
static inline hermes_handle_t hermes_device_handle(const hermes_device_t *d) { return d->serial; }
#else
static inline hermes_device_t *hermes_handle_get(hermes_handle_t h, const hermes_device_class_t *cls,
                                                 const char *kind_P, const char *method_P) {
    if (!h) hermes_handle_gone(cls, kind_P, method_P);
    return h;
}
static inline hermes_device_t *hermes_handle_peek(hermes_handle_t h) { return h; }
static inline hermes_handle_t hermes_device_handle(const hermes_device_t *d) {
    return (hermes_handle_t)d;
}
#endif

/* `device_handles_on()` in a .hepy program: a marker -- hepyc sees the
 * call and builds the whole program with -DHERMES_HANDLES (the mechanism
 * on). Where it's called doesn't matter; at run time it does nothing. */
static inline void device_handles_on(void) {}

/* Declares a kind's class ahead of its methods, which resolve handles
 * against it (the class itself is defined after them, by
 * HERMES_DEVICE_CLASS: it needs device_<kind>_abort). */
#define HERMES_DEVICE_KIND(kind) static hermes_device_class_t hermes_device_class_##kind

/* First line of a method: `type *p = HERMES_DEVICE_GET(type, kind, h, "method");`
 *   hermes_device_gpio_t *g = HERMES_DEVICE_GET(hermes_device_gpio_t, gpio, h, "write"); */
#define HERMES_DEVICE_GET(type, kind, h, method)                                    \
    ((type *)hermes_handle_get((h), &hermes_device_class_##kind, HERMES_PSTR(#kind), \
                               HERMES_PSTR(method)))

/* --- hardware conflicts (lock held) -----------------------------------------
 * pin_user: the device whose describe lists `pin`, NULL if none.
 * pin_claim: the same check for a constructor about to take `pin`; if some
 * device holds it, lets go of the lock (held exactly once) and dies with
 *   "<ctor>(<arg>): pin <pin> is already used by a <kind> device". */
hermes_device_t *hermes_device_pin_user(int pin);
void hermes_device_pin_claim(int pin, const char *ctor_P, long arg);

/* describe of one device, with `out` set to "none" first. */
void hermes_device_describe(hermes_device_t *d, hermes_device_desc_t *out);

/* --- dump and memory ------------------------------------------------------
 * dump: every device, kind by kind, out like print() (stdout on the host,
 * the UART on bare metal): a line per kind with its count and memory, a
 * line per device with its details, a total line. Each line is written
 * whole (no other thread's print in the middle); the list may change
 * between lines (another core creating or destroying a device).
 *   thread: 2 devices, 712 B
 *     #0 main id=0 state=running prio=4
 *     #1 id=1 state=ready prio=4
 *   gpio: 1 device, 8 B
 *     #0 pin=13 mode=out
 *   total: 3 devices, 720 B
 * memory: the sum of every device's `bytes` (a thread: its whole block;
 * main: 0, its stack isn't the framework's). */
void hermes_device_dump(void);
unsigned long hermes_device_memory(void);

/* Calls the `abort` of every registered device, kind by kind (order of
 * first registration), each kind in creation order. Devices stay
 * registered. Re-entrant calls (an abort that ends up here again) are
 * ignored. HERMESPY: Total_abort(). */
void hermes_device_abort_all(void);

/* Fatal error in the framework: aborts every device (safe state), then
 * abort(). Use instead of a bare abort() wherever a device or thread may
 * already exist. */
void hermes_fatal(void) __attribute__((noreturn));

/* All kinds together. The i-th device walks the kinds in order of first
 * registration, each in creation order (devices grouped by kind). */
int hermes_device_count(void);
hermes_device_t *hermes_device_at(int i);

/* One kind. */
int hermes_device_count_class(const hermes_device_class_t *cls);
hermes_device_t *hermes_device_class_at(const hermes_device_class_t *cls, int i);

static inline const hermes_device_class_t *hermes_device_class(const hermes_device_t *d) {
    return d->cls;
}

/* Its kind's name; "main" for the main thread; "core" for a core's base
 * context (a thread control block that isn't registered). */
const char *hermes_device_name(const hermes_device_t *d);
/* The main thread's device (hermes_thread.c sets it). */
extern hermes_device_t *hermes_device_main;

static inline const char *hermes_device_class_name(const hermes_device_t *d) {
    return d->cls ? d->cls->name : "?";
}

#endif
