// One of the 8 translation units wasm2c wrote, compiled with the box's configuration (qjsbox_config.h).
#include "qjsbox_config.h"

#pragma clang diagnostic ignored "-Wshorten-64-to-32" /* wasm's i64 -> i32 conversions, as the module has them */
#include "../generated/qjsbox_4.c"
