# 09 — Sources and Further Reading

The design choices in this firmware are not invented — they follow established embedded /
RTOS practice. This doc cites the references that justify them, so you can verify and go
deeper. Claims in the other docs point back here.

---

## Primary references (the authoritative ones)

1. **"Mastering the FreeRTOS Real Time Kernel — a Hands-On Tutorial Guide"** (Richard
   Barry / FreeRTOS). The canonical book on tasks, the scheduler, queues, semaphores,
   mutexes, priorities, and notifications.
   <https://www.freertos.org/Documentation/RTOS_book.html>
   → Backs **doc 03** (tasks/scheduling/priorities) and **doc 06** (queues/mutexes/
   notifications, priority inheritance).

2. **FreeRTOS Kernel — Symmetric Multiprocessing (SMP) documentation.** Explains
   `configNUMBER_OF_CORES`, core affinity (`vTaskCoreAffinitySet`),
   `configRUN_MULTIPLE_PRIORITIES`, and the multi-core scheduling model.
   <https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/11-Symmetric-multiprocessing-introduction>
   → Backs the **dual-core SMP** decision in **doc 05** and the `FreeRTOSConfig.h` SMP
   settings.

3. **FreeRTOS API reference** — exact semantics of `xQueueCreate`/`xQueueSend`/
   `xQueueReceive`/`xQueueOverwrite`, `xSemaphoreCreateMutex`, `vTaskDelay`,
   `xTaskCreate`, `ulTaskNotifyTake`/`vTaskNotifyGiveFromISR`, `uxTaskGetStackHighWaterMark`.
   <https://www.freertos.org/a00106.html>
   → Backs every kernel call in the code and **doc 06**.

4. **Raspberry Pi — "Raspberry Pi Pico-series C/C++ SDK"** (PDF) and the **RP2350
   datasheet**. The SDK book covers `pico_cyw43_arch` variants (poll vs
   threadsafe-background vs **FreeRTOS**), `async_context`, and `pico_flash` /
   `flash_safe_execute`. The datasheet covers the dual Cortex-M33 cores, memory map, and
   XIP.
   <https://datasheets.raspberrypi.com/pico/raspberry-pi-pico-c-sdk.pdf> ·
   <https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf>
   → Backs the cyw43-arch swap (**doc 02/05**), the GPIO29/VSYS sharing and
   `flash_safe_execute` SMP-safety (**doc 04 §8**, **doc 06 §6**).

5. **pico-examples — `pico_w/wifi/freertos` and the FreeRTOS integration examples.** The
   reference for wiring FreeRTOS + lwIP (`NO_SYS=0`, `LWIP_TCPIP_CORE_LOCKING`) + the cyw43
   FreeRTOS arch on the Pico W.
   <https://github.com/raspberrypi/pico-examples>
   → Backs the `lwipopts.h` changes (**doc 02**) and the boot/`cyw43_arch_init`-under-the-
   scheduler ordering (**doc 05 §7**).

6. **FreeRTOS-Kernel — RP2350 ARM port (`RP2350_ARM_NTZ`, Community-Supported-Ports).** Its
   README documents the required ARMv8-M options (`configENABLE_FPU/MPU/TRUSTZONE`,
   `configRUN_FREERTOS_SECURE_ONLY`) and the tested `configMAX_SYSCALL_INTERRUPT_PRIORITY`.
   <https://github.com/FreeRTOS/FreeRTOS-Kernel-Community-Supported-Ports>
   → Backs the exact `FreeRTOSConfig.h` port values we set in **doc 01/02**.

7. **lwIP documentation — "Common pitfalls" / multithreading.** Explains OS mode
   (`NO_SYS=0`), the tcpip thread, and core-locking (`cyw43_arch_lwip_begin/end`).
   <https://www.nongnu.org/lwip/2_1_0/pitfalls.html>
   → Backs **doc 06 §6** (networking lock) and the `lwipopts.h` mode switch.

---

## Conceptual / background reading

8. **"Super loop vs RTOS"** discussions — the classic trade-off this whole project
   embodies (a blocking call in a super-loop starves everything; an RTOS lets it block in
   isolation). See FreeRTOS's own "Why use an RTOS?" page.
   <https://www.freertos.org/about-RTOS.html>
   → Backs the motivation in **doc 00/03**.

9. **Interrupt-driven vs polled I/O** — replacing a polled BUSY wait with a GPIO interrupt
   + task notification is a standard latency/power optimization. (ARM Cortex-M NVIC basics;
   any embedded systems text, e.g. *Making Embedded Systems*, Elecia White, O'Reilly.)
   → Backs the Phase 3 BUSY-interrupt design in **doc 06 §4** and **doc 08 §B**.

10. **Race conditions, deadlock, and priority inversion** — foundational concurrency theory
    (e.g. the Mars Pathfinder priority-inversion case study is the famous real-world
    example). *Making Embedded Systems* and the FreeRTOS book both cover the practical
    embedded form.
    → Backs **doc 06 §1/§5** and the memory-sharing hazards in **doc 04 §6/§8**.

11. **Waveshare 2.13" e-Paper (SSD1680) driver & datasheet** — the panel's command
    sequences and the BUSY-wait timing (~300 ms full/partial refresh) that motivate moving
    the display onto its own core.
    <https://www.waveshare.com/wiki/2.13inch_e-Paper_HAT>
    → Backs the ~300 ms figure used throughout.

---

## How to use this list

When a design doc makes a claim — "SMP needs core affinity," "flash writes must coordinate
both cores," "a blocking call is fine inside a task" — find the matching numbered source
here and read the relevant section. The goal is that nothing in this codebase is "because
the author said so": every non-obvious choice traces to one of these references plus the
project's own measured results (the Phase 0 build verification in **doc 07 §6**).
