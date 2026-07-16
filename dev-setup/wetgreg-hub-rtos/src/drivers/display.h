/**
 * display.h — e-ink panel hand-off: build-variant name + canvas→panel
 * transpose.
 *
 * Purpose: expose the panel-variant name and the canvas-to-panel transpose;
 * everything else about the e-ink stays behind display.c.
 *
 * The EPD controller driver itself lives in lib/e-Paper and is touched ONLY
 * by display.c; post-boot, only the Display task (core 1) calls into it. The
 * Display-task hooks (display_grab_into / display_blit / display_init_panel /
 * display_panel_cmd / display_render) are declared in rtos_tasks.h — that
 * header is the task contract; this one is the drawing-side interface.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

/* Panel variant name (build-time -DDISPLAY_Vx selection, see CMakeLists). */
#if defined(DISPLAY_V2)
  #define DISPLAY_NAME "V2"
#elif defined(DISPLAY_V3A)
  #define DISPLAY_NAME "V3a"
#elif defined(DISPLAY_V4)
  #define DISPLAY_NAME "V4"
#else
  #define DISPLAY_NAME "V3"
#endif

/* Map the active canvas (ui/canvas.h) onto the fixed 122x250 panel at the
 * current display_rotation, into the UI task's private transpose buffer.
 * Call display_render() (rtos_tasks.h) afterwards to hand it to core 1. */
void transpose_to_display(void);

#endif /* DISPLAY_H */
