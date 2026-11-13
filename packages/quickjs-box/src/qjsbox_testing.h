// For the box's own tests only (tools/qjsbox-run.c, the Swift tests): not part of the client's surface.
#ifndef QJSBOX_TESTING_H
#define QJSBOX_TESTING_H

#include <stddef.h>
#include <stdint.h>

#include "qjsbox_host.h"

/* The box's linear memory. Writing to it plays an engine whose memory bug corrupted its own heap. */
uint8_t *qjsbox_testing_memory(qjsbox *box, size_t *size);

#endif
