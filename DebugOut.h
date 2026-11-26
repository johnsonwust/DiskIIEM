#ifndef DEBUG_OUT_H
#define DEBUG_OUT_H

// Debug text. The board has no serial port (stdio is switched off in
// CMakeLists.txt and the UART pins are used for other signals), so by default
// DBG_PRINTF compiles to nothing: no code, no text in RAM, arguments are not
// evaluated. Build with -DFLOPPY_DEBUG_PRINTF=1 (and enable a stdio output in
// CMakeLists.txt) to get the messages back.
#if defined(FLOPPY_DEBUG_PRINTF) && FLOPPY_DEBUG_PRINTF
#include <stdio.h>
#define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...) ((void)0)
#endif

#endif // DEBUG_OUT_H
