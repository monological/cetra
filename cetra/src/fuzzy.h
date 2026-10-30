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
 * and for each character run on from the one before, and a gap costs a little. So `st` takes
 * "fast stop" at its second word's start rather than at the first s and t it meets.
 *
 * ASCII letters match either case; any other character matches only itself. No GL, no allocation.
 */

#include <stddef.h>

// The longest query and name, in codepoints, that are matched whole; the rest of either is ignored.
#define FUZZY_QUERY_MAX 32
#define FUZZY_NAME_MAX  256

// Where a match placed the query: one byte offset into the name a codepoint of the query, in order.
typedef struct FuzzyPositions {
    size_t count; // the query's codepoints; 0 for no match or an empty query
    size_t at[FUZZY_QUERY_MAX];
} FuzzyPositions;

// The score of `query` against `name`, higher being better, or -1 when the name does not hold the
// query's characters in order. An empty query matches every name at 0. `positions` may be NULL for
// the score alone.
int fuzzy_match(const char* query, const char* name, FuzzyPositions* positions);

#endif // _FUZZY_H_
