/*
 * form_parser.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Parses application/x-www-form-urlencoded POST bodies into a
 * roster_entry_t. Uses the field table in form_parser.c to map
 * keys → offsets/types. No dynamic allocation.
 */
#ifndef FORM_PARSER_H_
#define FORM_PARSER_H_

#include <stdbool.h>
#include <stddef.h>
#include "roster.h"

// Parses `body` (length `len`, not necessarily NUL-terminated) into `*out`.
// `out` should be zeroed by the caller before calling.
// Unknown keys are ignored. Missing checkboxes remain false.
// Returns true on success; false only if the body is malformed enough
// to make forward progress impossible.
bool form_parse_roster(const char *body, size_t len, roster_entry_t *out);

#endif /* FORM_PARSER_H_ */
