/* ============================================================================
 *  freertos_hooks.c  —  the callbacks FreeRTOS REQUIRES us to provide
 * ============================================================================
 *
 *  WHY DOES THIS FILE EXIST?
 *  -------------------------
 *  FreeRTOS is a library, but it can't do everything by itself. A few jobs have
 *  to be handled by US (the application), so the kernel calls back into
 *  functions with specific, fixed names. If we enable a feature in
 *  FreeRTOSConfig.h, we MUST supply its matching callback or the program won't
 *  link. These callbacks are called "hooks".
 *
 *  Think of them like the lifecycle/error handlers a framework demands you
 *  implement. In JavaScript a framework might throw if you don't provide an
 *  `onError` handler; here the *linker* refuses to build if a required hook is
 *  missing. We gather them all in this one file so they're easy to find.
 *
 *  This file supplies two kinds of hooks:
 *    A) ERROR hooks — called when something goes wrong (out of memory, stack
 *       overflow). We stop hard, because limping along with corrupted memory is
 *       worse than halting.
 *    B) STATIC-MEMORY hooks — because we set configSUPPORT_STATIC_ALLOCATION=1,
 *       the kernel asks US to hand it the RAM for its internal "idle" and
 *       "timer" helper tasks instead of allocating that RAM itself. We just give
 *       it some fixed global arrays. (See docs/02-MEMORY-POINTERS-ADDRESSING.md
 *       for what "static memory" means versus the heap.)
 * ========================================================================== */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

/* ----------------------------------------------------------------------------
 *  A) ERROR HOOKS
 * ------------------------------------------------------------------------- */

/*  Called by the kernel if a memory allocation from the FreeRTOS heap fails
 *  (the 128 KB pool in FreeRTOSConfig.h is exhausted). There is no graceful
 *  recovery from "out of RAM" on a microcontroller, so we print and halt. A
 *  frozen device with a clear message is far easier to debug than one that
 *  silently misbehaves. */
void vApplicationMallocFailedHook( void )
{
    /* taskDISABLE_INTERRUPTS stops the scheduler from switching away; we are
     * never coming back from here. */
    taskDISABLE_INTERRUPTS();
    printf("\n*** FreeRTOS: malloc failed (heap exhausted) ***\n");
    for( ;; ) { /* hang on purpose so the failure is obvious */ }
}

/*  Called by the kernel (because configCHECK_FOR_STACK_OVERFLOW=2) when a task
 *  uses more stack than we reserved for it. Each task gets a fixed-size stack;
 *  unlike a PC, there is no "grow the stack" — overrunning it corrupts whatever
 *  memory sits next to it. Catching this immediately, with the offending task's
 *  name, saves hours. The fix when this fires is "give that task a bigger stack
 *  at xTaskCreate()". */
void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    ( void ) xTask;
    taskDISABLE_INTERRUPTS();
    printf("\n*** FreeRTOS: stack overflow in task '%s' ***\n", pcTaskName);
    for( ;; ) { }
}

/* ----------------------------------------------------------------------------
 *  B) STATIC-MEMORY HOOKS
 * ------------------------------------------------------------------------- */
/*  configSUPPORT_STATIC_ALLOCATION=1 means some kernel objects use memory the
 *  caller provides, rather than the kernel's heap. The kernel's own internal
 *  helper tasks (idle + timer) then need US to provide their RAM via these
 *  hooks. We do it the standard way: declare fixed global buffers and hand back
 *  pointers to them.
 *
 *  POINTER NOTE for JS devs: a parameter like `StaticTask_t **ppxBuf` is a
 *  "pointer to a pointer". The kernel passes us the ADDRESS of one of its
 *  pointer variables; we write the address of our buffer INTO it (`*ppxBuf =
 *  &ourBuffer`). That's how C functions return values through their arguments —
 *  there is no multiple-return like JS destructuring, so we pass addresses to
 *  fill in. `&x` means "address of x"; `*p` means "the thing p points at". */

/*  -- The TIMER service task's memory ------------------------------------- */
/*  FreeRTOS software timers run on a hidden "timer daemon" task. Give it a TCB
 *  (Task Control Block — the kernel's bookkeeping struct for a task) and a
 *  stack. configTIMER_TASK_STACK_DEPTH is set in FreeRTOSConfig.h. */
void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     configSTACK_DEPTH_TYPE * puxTimerTaskStackSize )
{
    /* `static` here means: allocate ONCE, in fixed RAM, for the whole program
     * lifetime (not on any task's stack). Exactly what the kernel needs. */
    static StaticTask_t xTimerTaskTCB;
    static StackType_t  uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *puxTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}

/*  -- The IDLE task's memory ---------------------------------------------- */
/*  The idle task runs when no other task is ready (it lets the CPU rest / does
 *  deferred cleanup). On SMP there is one "active" idle task here plus one
 *  "passive" idle per extra core (next function). */
void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE * puxIdleTaskStackSize )
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *puxIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

/*  -- The PASSIVE idle tasks' memory (SMP only) --------------------------- */
/*  With two cores there is an extra idle task for the second core. The kernel
 *  calls this once per passive idle task, passing an index (0, 1, ...). We keep
 *  a small array of buffers — one per passive idle task — and return the slot
 *  for the requested index. The count is (configNUMBER_OF_CORES - 1). */
#if ( configNUMBER_OF_CORES > 1 )
void vApplicationGetPassiveIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                           StackType_t ** ppxIdleTaskStackBuffer,
                                           configSTACK_DEPTH_TYPE * puxIdleTaskStackSize,
                                           BaseType_t xPassiveIdleTaskIndex )
{
    /* One TCB + one stack per passive idle task. [configNUMBER_OF_CORES-1]
     * because the "active" idle task above covers the first core. */
    static StaticTask_t xPassiveIdleTaskTCB[ configNUMBER_OF_CORES - 1 ];
    static StackType_t  uxPassiveIdleTaskStack[ configNUMBER_OF_CORES - 1 ][ configMINIMAL_STACK_SIZE ];

    *ppxIdleTaskTCBBuffer   = &xPassiveIdleTaskTCB[ xPassiveIdleTaskIndex ];
    *ppxIdleTaskStackBuffer = uxPassiveIdleTaskStack[ xPassiveIdleTaskIndex ];
    *puxIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}
#endif
