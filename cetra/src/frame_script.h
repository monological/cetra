#ifndef _FRAME_SCRIPT_H_
#define _FRAME_SCRIPT_H_

/*
 * A text script of FRAME RANGES: the grammar --pad-script introduced (spec
 * 11.109) and --pointer-script reuses (spec 12.19).
 *
 * One line per range -- a frame or `from-to`, then whitespace-separated tokens
 * the caller interprets. `#` starts a comment and a blank line is skipped. A
 * frame no line covers takes the caller's IDLE range, and where two lines cover
 * one frame the LAST one wins: that is what lets a later line override part of
 * an earlier span instead of the spans having to be disjoint.
 *
 * The grammar is here and the DEVICE is the caller's -- this knows nothing
 * about what a token means. Two devices share it because a scripted pad and a
 * scripted pointer are the same file format over different state, and having
 * one parser is what makes "the same grammar" a fact rather than a claim in a
 * document.
 *
 * A range struct must BEGIN with a FrameScriptRange, which is what lets one
 * parser fill two unrelated device states. Each call site asserts that with
 * offsetof rather than trusting it, since getting it wrong reads a frame number
 * out of the device state and covers nothing at all.
 */

#include <stdbool.h>
#include <stddef.h>

// The header every range struct starts with. Inclusive both ends.
typedef struct FrameScriptRange {
    int from;
    int to;
} FrameScriptRange;

// One token into the range being built; false with the reason logged. `path`
// and `line` are for that message and nothing else.
typedef bool (*FrameScriptTokenFn)(void* range, const char* token, const char* path, int line);

/*
 * Parse `text` -- MODIFIED IN PLACE, and the caller still owns it -- into a
 * malloc'd array of `*out_count` ranges of `stride` bytes, each seeded from
 * `idle` before its tokens are applied. The caller frees `*out_ranges`.
 *
 * False, with the reason logged against `path:line`, when a line does not
 * parse. Returning a bool rather than the array is what keeps an EMPTY script
 * -- true, no ranges, every frame idle -- from reading as a failure: both
 * hand back a NULL array, and only one of them is an error.
 */
bool frame_script_parse(char* text, const char* path, size_t stride, const void* idle,
                        FrameScriptTokenFn on_token, void** out_ranges, size_t* out_count);

// The range covering `frame`, or `idle` where none does. The last match wins.
const void* frame_script_at(const void* ranges, size_t count, size_t stride, const void* idle,
                            int frame);

#endif // _FRAME_SCRIPT_H_
