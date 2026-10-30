#ifndef _FUZZY_H_
#define _FUZZY_H_

/*
 * Fuzzy matching (spec 14.2): does every character of what was typed appear in a name, in order,
 * and how well? What a command palette or a jump to a name by a few letters asks of each name in
 * its list. It searches nothing: the app loops over its own list, calls this on each name, sorts
 * by the score and its own tie-break, and highlights the positions it gives.
 *
 * The best-scoring placement, not the first: a small dynamic program over the query's and the
 * name's characters, after fzf's second algorithm. A match scores more at a word's start -- the
 * name's start, after a space, `_`, `-`, `/`, `.`, `:` or `[`, or where lower case turns upper --
 * and for each character run on from the one before, and a gap costs a little. So `rtnt` takes
 * "route net" on its word starts rather than the first r, t, n and t it meets.
 *
 * ASCII letters match either case; any other character matches only itself. No GL, no allocation.
 */

#include <stddef.h>

// The longest query and name, in codepoints, that are matched whole; the rest of either is ignored.
#define FUZZY_QUERY_MAX 32
#define FUZZY_NAME_MAX  256

// The score of `query` against `name`, higher being better, or -1 when the name does not hold the
// query's characters in order. An empty query matches every name at 0. When `positions` is given
// and holds the query's length in codepoints (`cap`), it receives the byte offset in `name` of each
// matched character, in order.
int fuzzy_match(const char* query, const char* name, size_t* positions, size_t cap);

#endif // _FUZZY_H_
