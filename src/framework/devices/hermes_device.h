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
 */

typedef struct hermes_device hermes_device_t;

typedef struct hermes_device_class {
    const char *name;            /* kind name, e.g. "thread", "gpio" */
    unsigned char max_instances; /* hardware ceiling; 0 = none */
    void (*abort)(hermes_device_t *d); /* MANDATORY: deactivate one device */

    /* runtime state -- leave out of the initializer (zero) */
    hermes_device_t *head;       /* oldest device of this kind */
    hermes_device_t *tail;       /* newest: O(1) append in creation order */
    unsigned short count;
    unsigned char linked;        /* already in the chain of kinds */
    struct hermes_device_class *next_class;
} hermes_device_class_t;

struct hermes_device {
    hermes_device_class_t *cls;
    const char *name; /* instance name, e.g. "main", "thread", "gpio" */
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
        kind_abort((handle_t)d);                                                   \
    }                                                                              \
    static hermes_device_class_t hermes_device_class_##kind = {                    \
        .name = #kind, .max_instances = (max), .abort = hermes_device_abort_entry_##kind \
    }

/* Sets d->cls/d->name and appends `d` to its kind's list. On any error `d`
 * is left unregistered and nothing changes. */
hermes_device_status_t hermes_device_register(hermes_device_t *d,
                                              hermes_device_class_t *cls,
                                              const char *name);

/* Same, but a failure is fatal (diagnostic on the host, then abort()) --
 * the contract every device constructor already has for "out of memory". */
void hermes_device_register_or_abort(hermes_device_t *d,
                                     hermes_device_class_t *cls,
                                     const char *name);

/* Removes `d` from its kind's list, keeping the rest in creation order.
 * Returns 1 if it was registered, 0 otherwise. Nothing calls it yet (no
 * device is destroyed today); it's the counterpart a future destroy
 * operation will use. */
int hermes_device_unregister(hermes_device_t *d);

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

static inline const char *hermes_device_name(const hermes_device_t *d) {
    return d->name;
}

static inline const char *hermes_device_class_name(const hermes_device_t *d) {
    return d->cls ? d->cls->name : "?";
}

#endif
