/* ============================================================================
 *  FreeRTOSConfig.h  —  the "settings file" for the FreeRTOS kernel
 * ============================================================================
 *
 *  WHAT IS THIS FILE?
 *  ------------------
 *  FreeRTOS is a tiny operating system kernel. Unlike a desktop OS, it is not a
 *  program you "install" — it is a *library* that gets compiled directly into
 *  our firmware. Because it has to fit on a microcontroller with kilobytes of
 *  RAM, almost every feature is optional and is switched on/off at COMPILE TIME
 *  with #define macros. This file is where we make all those choices.
 *
 *  If you come from JavaScript: think of this like a build-time `config.json`,
 *  except the values are baked into the binary by the C preprocessor before the
 *  compiler ever runs. There is no runtime config object — these are constants.
 *
 *  Every `#define configXXX  value` below sets one kernel option. The kernel's
 *  source code is full of `#if (configXXX == 1)` checks that include or exclude
 *  whole features based on what we write here.
 *
 *  WHY SO MANY OPTIONS?
 *  --------------------
 *  On a microcontroller you pay for every byte. Each feature you enable costs
 *  flash (code space) and RAM. So FreeRTOS makes you opt in to exactly what you
 *  need. We document each choice so a newcomer understands the trade-off.
 *
 *  THE BIG PICTURE FOR THIS PROJECT (WetGreg Hub, RP2350 / Pico 2 W):
 *    - The RP2350 has TWO CPU cores. We run FreeRTOS in "SMP" mode (Symmetric
 *      Multi-Processing): one kernel schedules tasks across BOTH cores.
 *    - We pin the slow e-ink display work to core 1 so it can block for ~300 ms
 *      without freezing the buttons/sensors/Wi-Fi running on core 0.
 *    - See docs/03-DESIGN-AND-ARCHITECTURE.md for the full reasoning.
 * ========================================================================== */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ----------------------------------------------------------------------------
 *  SMP (multi-core) configuration  —  the defining choice of this firmware
 * ------------------------------------------------------------------------- */

/* How many CPU cores the kernel is allowed to schedule onto.
 * 1 = classic single-core FreeRTOS. 2 = SMP: the scheduler can run two tasks
 * literally at the same instant, one per core. The RP2350 has two Cortex-M33
 * cores, so we use 2. THIS is what lets the display block on core 1 while the
 * input task keeps running on core 0. */
#define configNUMBER_OF_CORES                   2

/* Which core owns the periodic "tick" timer interrupt (the kernel's heartbeat
 * that wakes delayed tasks and time-slices). Core 0 by convention. */
#define configTICK_CORE                         0

/* In SMP, by default the kernel will only let tasks of the SAME priority run on
 * different cores simultaneously. Setting this to 1 lifts that restriction so a
 * HIGH-priority task on one core and a LOW-priority task on the other core can
 * run at the same time. We need this: Display (high prio) on core 1 must run
 * concurrently with the lower-prio UI/sensor tasks on core 0. */
#define configRUN_MULTIPLE_PRIORITIES           1

/* Allow each task to be "pinned" to a specific core (core affinity). We use this
 * to force the Display task onto core 1 and everything else onto core 0.
 * Without it, the scheduler would move tasks between cores freely. */
#define configUSE_CORE_AFFINITY                 1

/* In SMP there is one idle task PER core. The "passive" idle hook is an optional
 * callback that runs on the idle task(s). We don't use it; it MUST be defined
 * (to 0) for SMP builds or the kernel header errors out. */
#define configUSE_PASSIVE_IDLE_HOOK             0

/* ----------------------------------------------------------------------------
 *  ARMv8-M (Cortex-M33) port options  —  required by the RP2350_ARM_NTZ port
 * ------------------------------------------------------------------------- */
/*  The RP2350's CPUs are Cortex-M33 cores (the "ARMv8-M" architecture). That
 *  architecture has optional hardware blocks the FreeRTOS port must be told
 *  about at compile time, or it refuses to build:
 *    - FPU (Floating-Point Unit): the M33 has hardware floating point. We use
 *      floats (accelerometer maths), so enable it = 1. The port then saves/
 *      restores FPU registers across context switches.
 *    - MPU (Memory Protection Unit): hardware that can sandbox tasks' memory
 *      access. We don't use it = 0 (simpler, and we trust our own code).
 *    - TrustZone: a "secure/non-secure world" split for security-critical code.
 *      "NTZ" in the port name literally means Non-TrustZone, so = 0.
 *    - RUN_FREERTOS_SECURE_ONLY: with TrustZone off, FreeRTOS runs entirely in
 *      the single (secure) world; = 1 tells the port exactly that. */
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1

/* The highest interrupt priority from which a task may call FreeRTOS "...FromISR"
 * API functions. Interrupts at a HIGHER priority than this are never masked by
 * the kernel, so they keep their low latency but must NOT call kernel APIs.
 * On the Cortex-M, NVIC priority numbers are stored in the top bits of a byte;
 * 16 is the value the RP2350_ARM_NTZ port author has tested and documents in the
 * port README, so we use it verbatim. (Lower number = higher hardware priority.) */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

/* ----------------------------------------------------------------------------
 *  Scheduler basics
 * ------------------------------------------------------------------------- */

/* 1 = preemptive scheduling: a higher-priority task that becomes ready will
 * immediately interrupt ("preempt") a lower-priority one. This is what makes
 * the Input task feel instant — the moment a button is pressed it can preempt
 * whatever was running. (0 would be cooperative: tasks only switch when they
 * voluntarily yield — not what we want here.) */
#define configUSE_PREEMPTION                    1

/* Let the scheduler pick the next task in O(1) using a hardware bit-scan.
 * Cheap win on Cortex-M; safe to enable. */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0   /* SMP port requires generic selection */

/* Tickless idle saves power by stopping the tick when everything is asleep.
 * It complicates timing and the cyw43/BT stack, so we keep it off for now. */
#define configUSE_TICKLESS_IDLE                 0

/* CPU clock — on the Pico SDK the real value is supplied by the SDK, so 0 here
 * is a placeholder (the port does not use it directly). */
#define configCPU_CLOCK_HZ                      0

/* The kernel "tick" frequency: how many times per second the heartbeat fires.
 * 1000 Hz = a 1 ms tick. This sets the resolution of vTaskDelay() and matches
 * the ~1-10 ms polling cadences the original firmware used, so timing behaviour
 * is preserved when we convert sleep_ms() -> vTaskDelay(). Higher = finer timing
 * but more interrupt overhead. */
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )

/* How many distinct priority LEVELS exist (0 = lowest). A task is assigned one
 * of these. We only use a handful (see src/rtos/sync.h) but 32 is cheap. */
#define configMAX_PRIORITIES                    32

/* The smallest stack (in WORDS, i.e. 4 bytes each on a 32-bit CPU) any task may
 * have. The idle task uses this. 256 words = 1 KB. Each of OUR tasks specifies
 * a larger stack explicitly when created. */
#define configMINIMAL_STACK_SIZE                ( ( configSTACK_DEPTH_TYPE ) 256 )

/* Max characters in a task's human-readable name (for debugging). */
#define configMAX_TASK_NAME_LEN                 16

/* 0 = use a 32-bit type for the tick counter (won't overflow for ~49 days at
 * 1 kHz). 1 would use 16 bits (overflows in ~65 s) — never do that here. */
#define configUSE_16_BIT_TICKS                  0

/* If the idle task and an equal-priority task are both ready, should idle give
 * up its slice immediately? Yes — idle should never hog the CPU. */
#define configIDLE_SHOULD_YIELD                 1

/* ----------------------------------------------------------------------------
 *  Inter-task communication features (we use all of these)
 * ------------------------------------------------------------------------- */

/* "Task notifications": the fastest, lightest way to signal a task (like a
 * per-task mailbox). We use them so the e-ink BUSY interrupt can wake the
 * Display task, and so UI can be told "your frame was drawn". */
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   3   /* a few independent slots per task */

/* Mutexes = "mutual exclusion" locks. A task takes the lock, touches shared
 * data, releases it; no other task can hold it meanwhile. We guard the sensor
 * snapshot with one. Recursive = the same task may take it more than once. */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1

/* Counting semaphores = a thread-safe integer you can wait on. Handy for
 * "N buffers free" style signalling. Enabled for flexibility. */
#define configUSE_COUNTING_SEMAPHORES           1

/* Lets debuggers see registered queues/semaphores by name. Cheap, useful. */
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0

/* Round-robin between equal-priority ready tasks on each tick. Keeps things
 * fair when two same-priority tasks are both runnable. */
#define configUSE_TIME_SLICING                  1

/* We don't use newlib's per-task reentrancy (no malloc-from-multiple-tasks via
 * newlib internals); the pico SDK + heap_4 handle our allocations. */
#define configUSE_NEWLIB_REENTRANT              0

/* Keep old FreeRTOS API spellings available (e.g. xTaskHandle). Harmless. */
#define configENABLE_BACKWARD_COMPATIBILITY     1

/* Stack sizes are expressed in this type. uint32_t lets us request big stacks. */
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* ----------------------------------------------------------------------------
 *  Memory management  —  IMPORTANT for a JS developer to understand
 * ------------------------------------------------------------------------- */
/*  In JavaScript the engine allocates objects and a garbage collector frees
 *  them automatically. C has NO garbage collector. FreeRTOS needs memory for
 *  task stacks, queues, and mutexes. We give it a single fixed-size pool (the
 *  "heap") carved out of RAM, and the kernel hands pieces out of it.
 *
 *  We use the kernel's "heap_4" allocator (selected in CMakeLists.txt): it can
 *  allocate AND free, and it merges adjacent free blocks to fight fragmentation
 *  — a good general-purpose choice. See docs/02-MEMORY-POINTERS-ADDRESSING.md. */

/* Allow objects to be created with kernel-managed (dynamic) memory... */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
/* ...and also allow caller-provided (static) memory for objects we want placed
 * in fixed RAM (the cyw43/btstack async-context task uses static allocation). */
#define configSUPPORT_STATIC_ALLOCATION         1

/* The total size of that FreeRTOS heap pool, in BYTES. The RP2350 has 520 KB of
 * SRAM; lwIP + BTstack already use a chunk statically, so 128 KB here is
 * generous but safe. If we ever run out, the malloc-failed hook below fires. */
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 128 * 1024 ) )

/* 0 = let the linker place the heap automatically (normal). */
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ----------------------------------------------------------------------------
 *  Safety hooks  —  callbacks the kernel invokes when something goes wrong
 * ------------------------------------------------------------------------- */

/* Per-tick / per-idle user callbacks: off (we don't need periodic side-effects
 * from the kernel itself). */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0

/* 2 = the heaviest stack-overflow check: on every context switch the kernel
 * verifies the task's stack canary and stack pointer. Catches the classic
 * embedded bug of a task using more stack than we reserved. Calls
 * vApplicationStackOverflowHook() (defined in main.c) if it trips. */
#define configCHECK_FOR_STACK_OVERFLOW          2

/* If a kernel allocation fails (heap exhausted), call vApplicationMallocFailed
 * Hook() (defined in main.c) so we can flag it instead of silently misbehaving. */
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* ----------------------------------------------------------------------------
 *  Diagnostics
 * ------------------------------------------------------------------------- */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1   /* lets us query task info */
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* ----------------------------------------------------------------------------
 *  Software timers  —  callbacks scheduled to run later, on a kernel task
 * ------------------------------------------------------------------------- */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            1024

/* Co-routines are an ancient, deprecated FreeRTOS feature. Off. */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* ----------------------------------------------------------------------------
 *  Raspberry Pi Pico SDK interoperability
 * ------------------------------------------------------------------------- */
/*  The Pico SDK has its OWN synchronization (spin locks, mutexes) and timing
 *  (sleep_ms, busy_wait) primitives. Lots of SDK code — including the e-ink
 *  driver's DEV_Delay_ms() and the cyw43 driver — calls them. These two options
 *  make those SDK primitives COOPERATE with the FreeRTOS scheduler:
 *    - SYNC_INTEROP: a task blocking on an SDK mutex/semaphore yields to the
 *      scheduler instead of busy-spinning the whole core.
 *    - TIME_INTEROP: sleep_ms()/busy_wait_*() become scheduler-aware so they
 *      yield. This is WHY a stray sleep_ms(10) in the e-ink busy-wait won't
 *      freeze the core — it hands the CPU to other tasks while waiting.
 *  Both are essential when mixing SDK code with FreeRTOS. */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

/* ----------------------------------------------------------------------------
 *  Optional API functions  —  include only the kernel calls we actually use
 * ------------------------------------------------------------------------- */
/*  Each INCLUDE_xxx pulls one API function into the build. Excluding unused ones
 *  saves flash. We enable the common set plus the introspection calls our
 *  hardening phase needs (stack high-water mark, scheduler state, etc.). */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1   /* used in Phase 3 stack tuning */
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xSemaphoreGetMutexHolder        1

/* ----------------------------------------------------------------------------
 *  configASSERT  —  the kernel's "this must be true or we have a bug" check
 * ------------------------------------------------------------------------- */
/*  FreeRTOS sprinkles configASSERT(condition) through its code. If a condition
 *  is false at runtime we want to STOP immediately (a hung firmware is easier to
 *  debug than one limping along with corrupted state). We route it to the Pico
 *  SDK's assert(), which prints the file/line and halts.
 *
 *  The `#ifndef __ASSEMBLER__` guard matters: this header is also #included by
 *  the kernel's assembly (.S) files, which can't see C declarations. We hide all
 *  C-only content from the assembler. */
#ifndef __ASSEMBLER__
    #include <assert.h>
    #define configASSERT( x )                   assert( x )
#endif

#endif /* FREERTOS_CONFIG_H */
