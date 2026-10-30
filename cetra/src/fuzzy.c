#include "fuzzy.h"

#include <stdbool.h>
#include <stdint.h>

#include "text.h"

// fzf's numbers: a match, a gap's first character and each one after, the bonus at a word's start
// and at a turn into upper case or a digit, a run's bonus, and the first character's multiplier.
#define SCORE_MATCH       16
#define GAP_START         (-3)
#define GAP_EXTEND        (-1)
#define BONUS_BOUNDARY    8
#define BONUS_CAMEL       7
#define BONUS_CONSECUTIVE 4
#define FIRST_MULTIPLIER  2
#define NONE              (-30000) // no placement ends here

static uint32_t _fold(uint32_t c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool _starts_word_after(uint32_t c) {
    return c == ' ' || c == '\t' || c == '_' || c == '-' || c == '/' || c == '.' || c == ':' ||
           c == '[';
}

// The bonus a match at name position `j` earns from where it stands.
static int _bonus(const uint32_t* cps, int j) {
    if (j == 0 || _starts_word_after(cps[j - 1]))
        return BONUS_BOUNDARY;
    const uint32_t prev = cps[j - 1], cur = cps[j];
    if (prev >= 'a' && prev <= 'z' && cur >= 'A' && cur <= 'Z')
        return BONUS_CAMEL;
    if (!(prev >= '0' && prev <= '9') && cur >= '0' && cur <= '9')
        return BONUS_CAMEL;
    return 0;
}

int fuzzy_match(const char* query, const char* name, FuzzyPositions* positions) {
    if (positions)
        positions->count = 0;
    if (!query || !name)
        return -1;
    uint32_t q[FUZZY_QUERY_MAX];
    int m = 0;
    for (const char* p = query; *p && m < FUZZY_QUERY_MAX;)
        q[m++] = _fold(text_utf8_next(&p));
    if (m == 0)
        return 0;

    uint32_t cps[FUZZY_NAME_MAX];
    size_t offs[FUZZY_NAME_MAX];
    int n = 0;
    for (const char* p = name; *p && n < FUZZY_NAME_MAX;) {
        offs[n] = (size_t)(p - name);
        cps[n++] = text_utf8_next(&p);
    }

    // The query's characters in order at all, first: most names in a list refuse here.
    int k = 0;
    for (int j = 0; j < n && k < m; j++)
        if (_fold(cps[j]) == q[k])
            k++;
    if (k < m)
        return -1;

    // best[i][j]: the best score of the query's first i + 1 characters with the last at name
    // position j. A placement arrives at j either straight from j - 1, a run, or across a gap,
    // whose best over every start is carried along the row as `gap`.
    int16_t best[FUZZY_QUERY_MAX][FUZZY_NAME_MAX];
    for (int i = 0; i < m; i++) {
        int gap = NONE;
        for (int j = 0; j < n; j++) {
            if (i > 0) {
                if (gap > NONE)
                    gap += GAP_EXTEND;
                if (j >= 2 && best[i - 1][j - 2] > NONE && best[i - 1][j - 2] + GAP_START > gap)
                    gap = best[i - 1][j - 2] + GAP_START;
            }
            if (_fold(cps[j]) != q[i]) {
                best[i][j] = NONE;
                continue;
            }
            const int b = _bonus(cps, j);
            int score;
            if (i == 0) {
                score = SCORE_MATCH + b * FIRST_MULTIPLIER;
            } else {
                score = NONE;
                if (j >= 1 && best[i - 1][j - 1] > NONE)
                    score = best[i - 1][j - 1] + SCORE_MATCH +
                            (b > BONUS_CONSECUTIVE ? b : BONUS_CONSECUTIVE);
                if (gap > NONE && gap + SCORE_MATCH + b > score)
                    score = gap + SCORE_MATCH + b;
            }
            best[i][j] = (int16_t)score;
        }
    }

    int score = NONE, end = -1;
    for (int j = 0; j < n; j++)
        if (best[m - 1][j] > score) {
            score = best[m - 1][j];
            end = j;
        }
    if (end < 0)
        return -1;

    // Back from the end: each character came from a run if a run gives its score, else from the
    // nearest gap start that does.
    if (positions) {
        positions->count = (size_t)m;
        int j = end;
        for (int i = m - 1; i >= 0; i--) {
            positions->at[i] = offs[j];
            if (i == 0)
                break;
            const int b = _bonus(cps, j), here = best[i][j];
            const int run = b > BONUS_CONSECUTIVE ? b : BONUS_CONSECUTIVE;
            if (j >= 1 && best[i - 1][j - 1] > NONE &&
                best[i - 1][j - 1] + SCORE_MATCH + run == here) {
                j--;
                continue;
            }
            for (int s = j - 2; s >= 0; s--)
                if (best[i - 1][s] > NONE &&
                    best[i - 1][s] + GAP_START + GAP_EXTEND * (j - s - 2) + SCORE_MATCH + b ==
                        here) {
                    j = s;
                    break;
                }
        }
    }
    return score < 0 ? 0 : score;
}
