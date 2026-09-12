#ifndef _JSON_UTIL_H_
#define _JSON_UTIL_H_

/*
 * The cJSON shapes more than one reader in this tree needs.
 *
 * config_snapshot.c carried the first two as file statics with a comment saying
 * they were worth sharing "the day a third cJSON reader appears rather than on
 * the second". save.c is that third reader, so here they are. That comment also
 * claimed cscene.c held the same pair. It does not, and since this is the file
 * that would have received them, the correction belongs here rather than there:
 * cscene.c's get_float / get_bool / get_vec3 are bool-returning PRESENCE tests,
 * because a .cscn leaves an unspecified field at its engine default. These two
 * answer a different question -- what is the value, or this fallback -- and the
 * two contracts should not be merged into one.
 *
 * json_add_float is not a hoist; it is new, and it is why this file is a .c
 * rather than three static inlines. See its comment.
 */

#include <stdbool.h>

#include "ext/cJSON.h"

// The string at `key`, or NULL when it is absent or is not a string.
const char* json_string_or(const cJSON* obj, const char* key);

// The number at `key` as an int, or `fallback` when it is absent or not a number.
int json_int_or(const cJSON* obj, const char* key, int fallback);

/*
 * A float, written at the shortest precision that still round-trips exactly.
 *
 * cJSON prints every number with "%1.15g" and retries at "%1.17g" if that does
 * not survive a round trip, which is correct and costly: it is sized for a
 * double, and a float promoted to one prints 0.3f as 0.300000011920929. Nine
 * significant digits is the exact round-trip width for binary32, so "%.9g"
 * gives 0.300000012 -- the same value, 40% of the characters, and a file a
 * person can read. It is also the precision the animation probe already prints
 * at, so a textual diff of either is a bit diff.
 *
 * Goes in as a RAW item, since cJSON's number printer has no per-item format
 * hook; the string this writes is the string that lands in the file.
 */
bool json_add_float(cJSON* obj, const char* key, float value);

/*
 * The same float as a free-standing item, for an ARRAY -- cJSON has no
 * AddRawToArray, and a caller that re-typed the snprintf would put the
 * precision rule in a second place where only one of the two would ever be
 * changed. NULL on allocation failure.
 */
cJSON* json_float_item(float value);

#endif // _JSON_UTIL_H_
