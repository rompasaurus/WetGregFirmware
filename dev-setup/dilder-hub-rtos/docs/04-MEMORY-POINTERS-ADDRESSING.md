# 04 — Memory, Pointers, and Addressing for JavaScript Developers

C exposes the machine. JavaScript hides it. This doc explains the low-level memory ideas
the firmware uses everywhere, so the code's pointer/cast/memory comments make sense. Read
it once and the code stops looking scary.

---

## 1. There is no garbage collector

In JavaScript, you make objects and forget about them; the GC frees memory later. **C has
no GC.** Every byte of memory is in one of three places, and you must know which:

| Region | Lifetime | JS analogy | Who frees it |
|---|---|---|---|
| **Static / global** | the whole program | module-level `const` | never freed (always there) |
| **Stack** | one function call | local `let` in a function | freed automatically when the function returns |
| **Heap** | until you free it | objects the GC tracks | **you**, by calling `free()` (or never, if you leak) |

This firmware **avoids the heap almost entirely**. The framebuffers, the quote arrays, and
the sensor state are all **static** (fixed, declared once). The only heap is the FreeRTOS
pool, and even there we lean on **static** task stacks (see `freertos_hooks.c`). Why?
Because on a device that runs for weeks, heap fragmentation and leaks are deadly, and fixed
memory is predictable. **Predictability > flexibility** in firmware.

---

## 2. Variables are boxes; addresses are where the box lives

A C variable is a named box in memory at some **address** (a number, like a street
address). Two operators connect names and addresses:

- `&x` means **"the address of x"** — *where* the box is.
- `*p` means **"the value at the address p holds"** — *follow the pointer* to the box.

A **pointer** is a variable whose value is an address. `int *p = &x;` reads "p points at
x." Then `*p` reads/writes x.

```c
int  steps   = 10;     // a box holding 10
int *ptr     = &steps; // ptr holds the ADDRESS of steps
*ptr         = 42;     // follow ptr, write 42 → steps is now 42
```

### The JS bridge

JavaScript actually uses pointers too — you just never see them. When you do
`const a = { n: 1 }; const b = a; b.n = 2;`, now `a.n` is 2, because `a` and `b` are two
**references** (pointers) to the *same* object. C just makes the pointer visible and lets
you do arithmetic on it. A C pointer is a JS object reference with the hood open.

---

## 3. Why functions take pointers: "returning" through arguments

JS functions return values, and can return an object/array to give back many things. A C
function returns **one** value. To give back more, the caller passes **addresses** for the
function to fill in. You'll see this constantly:

```c
void get_xyz(int16_t *x, int16_t *y, int16_t *z) {
    *x = read_axis(0);   // write into the caller's box
    *y = read_axis(1);
    *z = read_axis(2);
}
int16_t ax, ay, az;
get_xyz(&ax, &ay, &az);  // pass the ADDRESSES so the function can fill them
```

The kernel's static-memory hooks do exactly this: `vApplicationGetIdleTaskMemory
(StaticTask_t **tcb, …)` is handed the **address of a pointer** and writes our buffer's
address into it. `**` ("pointer to a pointer") looks alien but it's just "fill in this
pointer for me."

---

## 4. Arrays are pointers in disguise; the framebuffer

An array's name is essentially a pointer to its first element. The display framebuffer is a
fixed byte array:

```c
static uint8_t display_buf[122 * 250 / 8];  // 1 bit per pixel, packed 8 to a byte
```

- `static` → lives for the whole program, in fixed RAM. Not on any stack, not on the heap.
- `uint8_t` → an **unsigned 8-bit** integer (0–255), i.e. one byte. (`u`=unsigned,
  `int`, `8`=bits, `_t`=type.) Firmware uses exact-width types everywhere because the size
  matters when you talk to hardware.
- The screen is 1-bit (black/white), so 8 pixels share a byte → we do **bit operations** to
  set one pixel: `buf[i] |= (1 << bit)`. That `|=`/`<<` is "turn on one bit," which JS devs
  rarely need but is the bread and butter of pixel/hardware code.

Because `display_buf` is a fixed block we **own**, only the Display task is allowed to
touch it — that ownership rule is how we avoid needing a lock on it (see doc 06).

---

## 5. `volatile` — "this can change behind your back"

Compilers optimize by assuming a variable doesn't change unless the current code changes
it. That assumption **breaks** when a value is changed by *another core*, by an *interrupt*,
or by *hardware*. The `volatile` keyword tells the compiler "always re-read this from
memory; never cache it in a register."

We mark cross-task/ISR flags `volatile` — e.g. a "Wi-Fi connected" flag set inside a
network callback and read by the UI task. Without `volatile`, the UI might read a stale
cached copy forever. There is no JS equivalent because JS has no shared-memory concurrency
(except `SharedArrayBuffer`, which has the same issue and the same kind of fix).

---

## 6. Word-atomicity (why some shared reads are safe without a lock)

The RP2350 is a 32-bit CPU. Reading or writing a single aligned 32-bit value (an `int`, a
pointer, a `bool`) happens in **one indivisible instruction** — another core can't catch it
"half done." That's called being **atomic**. So a lone `volatile bool wifi_connected` can
be shared between tasks without a mutex: every read sees either the old or the new value,
never a corrupted mix.

But a **multi-byte** thing — a string, a struct, an array — is written in several
instructions, so a reader *can* catch it half-updated. Those need a **mutex**. Rule of
thumb in this codebase:

- single `bool`/`int`/pointer shared across tasks → `volatile`, no lock.
- a `struct` / array / string shared across tasks → **mutex** (e.g. the sensor snapshot).

---

## 7. The stack, and why stack size matters

Each task gets its own fixed **stack** — the scratch space for its local variables and
function-call bookkeeping. Deeply nested calls or big local arrays use more stack. If a
task uses more than we reserved, it **overflows** into neighbouring memory and corrupts it
— a classic, nasty embedded bug.

We guard against it two ways: `configCHECK_FOR_STACK_OVERFLOW = 2` (the kernel checks on
every switch and calls our error hook), and in Phase 3 we measure real usage with
`uxTaskGetStackHighWaterMark()` and right-size each stack. The UI task gets the biggest
stack because its render path is the deepest (nested `draw_*` calls + `snprintf`).

> In JS the engine grows the stack and throws `RangeError: Maximum call stack size
> exceeded`. Here there's no automatic growth — you reserve a size up front and must not
> exceed it.

---

## 8. Flash vs RAM (and why writing flash is dangerous under SMP)

The chip has two kinds of memory:

- **Flash** (~4 MB): non-volatile; holds the program and saved settings. The CPU even
  **executes code directly from flash** (called XIP — eXecute In Place).
- **RAM** (520 KB): volatile working memory; variables, stacks, the heap, framebuffers.

Here's the SMP trap: to **write** flash (e.g. save your Wi-Fi/battery-calibration
settings), the flash hardware must be put in a special mode where it **can't be read** —
but if the *other* core tries to fetch its next instruction from flash (XIP) during that
window, it crashes. The original firmware just disabled interrupts on one core, which is
**not** enough with two cores running. Phase 3 fixes this with the SDK's
`flash_safe_execute()`, which coordinates **both** cores (parking the other one safely)
before touching flash. This is a perfect example of a bug that simply *cannot exist* on a
single core or in JavaScript, and why the SMP design needs care.

---

## 9. Cheat sheet

| You see… | It means… |
|---|---|
| `uint8_t`, `int16_t`, `uint32_t` | unsigned/signed integer of exactly 8/16/32 bits |
| `&x` | address of `x` |
| `*p` | the value `p` points to |
| `p->field` | `(*p).field` — follow pointer, then take a field |
| `static` (on a global/local) | one fixed instance for the whole program |
| `volatile` | may change via another core/ISR/hardware — always re-read |
| `x |= (1<<n)` / `x &= ~(1<<n)` | set / clear bit `n` |
| `(uint8_t *)thing` | a **cast**: "treat this memory as bytes" |
| `memcpy(dst, src, n)` | copy `n` raw bytes (no GC, no deep-clone magic) |

With these in hand, the heavily-commented source will read clearly. When in doubt, the
code's inline comments restate the same ideas at the point they're used.
