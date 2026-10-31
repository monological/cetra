#ifndef _EMBEDDED_FILE_H_
#define _EMBEDDED_FILE_H_

/*
 * A file built into a program (spec 14.6) by cetra_embed_files, in CMake: a font, so the program is
 * one file with nothing to find beside it. The build's list is the one place a file is named; the
 * program indexes the generated table by what each file is for, and makes a font from an entry
 * with load_font_memory or create_glyph_cache_memory.
 */

#include <stddef.h>

typedef struct CetraEmbeddedFile {
    const char* name;   // the file's own name, which a font pool keeps it under
    const char* source; // the file it was built from, from the source root
    const unsigned char* bytes;
    size_t size;
} CetraEmbeddedFile;

#endif // _EMBEDDED_FILE_H_
