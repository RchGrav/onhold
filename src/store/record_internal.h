#pragma once
#ifndef HOLD_STORE_RECORD_INTERNAL_H
#define HOLD_STORE_RECORD_INTERNAL_H

#include "hold/config.h"
#include "hold/types.h"

/* Shared between the record reader (record_read.c) and the public-index
 * reader (public_index.c): checked narrowing from parsed JSON integers. */
int hold_record_checked_i64_to_pid(int64_t v, pid_t *out);

#endif /* HOLD_STORE_RECORD_INTERNAL_H */
