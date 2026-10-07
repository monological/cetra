import io
import os
import argparse
import re

# Shared shader chunks live here and are pulled in with #include "name.glsl".
# They are expanded at build time, into the same string literals the engine has
# always compiled from. The chunks are also emitted unexpanded, as a table, for
# the runtime resolver an app's shader goes through (--includes-out); no file is
# read at runtime either way.
INCLUDE_DIR = 'include'

INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"\s*(?://.*)?$')

def expand_includes(lines, include_dir, seen):
    """Splice #include'd chunks into `lines`.

    Include-once, like #pragma once: GLSL has no include guards, so expanding a
    chunk twice is a duplicate-definition compile error. `seen` is per
    top-level shader, and doubles as the cycle guard for chunk-to-chunk loops.

    The begin/end markers are real lines in the compiled source, so a driver
    error inside a chunk lands between two self-describing comments. That is
    deliberately all the traceability there is: #line directives were tried and
    removed -- see the note above the blank-line handling in lines_to_c.
    """
    out = []
    for line in lines:
        match = INCLUDE_RE.match(line)
        if not match:
            # A directive the regex rejected would otherwise be copied verbatim
            # into the GLSL and only surface as a driver error at app startup,
            # with a green build. Fail here instead.
            if line.lstrip().startswith('#include'):
                raise SystemExit(f"malformed #include: {line.strip()!r}")
            out.append(line)
            continue

        name = match.group(1)
        if name in seen:
            out.append(f'// #include "{name}" (already expanded)\n')
            continue
        seen.add(name)

        path = os.path.join(include_dir, name)
        try:
            with open(path, 'r', encoding='utf-8', errors='ignore') as chunk:
                chunk_lines = chunk.readlines()
        except IOError:
            raise SystemExit(f"Error: {path} not found (included from a shader)")

        out.append(f'// ---- begin {name} ----\n')
        # A chunk may include another chunk.
        out.extend(expand_includes(chunk_lines, include_dir, seen))
        out.append(f'// ---- end {name} ----\n')
    return out

def lines_to_c(lines):
    processed_lines = []
    for line in lines:
        # Remove null bytes before processing the line
        clean_line = line.replace('\x00', '')
        # Strip the line to check if it's empty
        stripped_line = clean_line.strip()
        if stripped_line:
            # Only the line ending comes off: stripping the trailing blanks too would turn a
            # backslash that ended in whitespace into a line continuation the file never had.
            body = clean_line.rstrip('\r\n')
            processed_lines.append('    "' + body.replace('\\', '\\\\').replace('"', '\\"') + '\\n"')
        else:
            # A blank line still has to emit its newline. Emitting a bare
            # "" instead drops the line from the source the driver sees,
            # so every GLSL error below it reports a line number short by
            # the number of blank lines above -- measured at 7 in the
            # worst shader here before this was fixed.
            processed_lines.append('    "\\n"')
    return "\n".join(processed_lines)

def shader_to_string(file_path, expand=True):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as file:
            lines = file.readlines()
    except IOError:
        print(f"Error reading file: {file_path}")
        return None
    if expand:
        include_dir = os.path.join(os.path.dirname(file_path), INCLUDE_DIR)
        lines = expand_includes(lines, include_dir, set())
    return lines_to_c(lines)

def write_open(header_file, guard):
    header_file.write(f"#ifndef {guard}\n")
    header_file.write(f"#define {guard}\n\n")

    # Disable the unused variable warning
    header_file.write("#if defined(__GNUC__)\n")
    header_file.write("#pragma GCC diagnostic push\n")
    header_file.write("#pragma GCC diagnostic ignored \"-Wunused-variable\"\n")
    header_file.write("#elif defined(__clang__)\n")
    header_file.write("#pragma clang diagnostic push\n")
    header_file.write("#pragma clang diagnostic ignored \"-Wunused-variable\"\n")
    header_file.write("#elif defined(_MSC_VER)\n")
    header_file.write("#pragma warning(push)\n")
    header_file.write("#pragma warning(disable: 4101)\n")
    header_file.write("#endif\n\n")

def write_close(header_file, guard):
    # Re-enable the unused variable warning
    header_file.write("#if defined(__GNUC__)\n")
    header_file.write("#pragma GCC diagnostic pop\n")
    header_file.write("#elif defined(__clang__)\n")
    header_file.write("#pragma clang diagnostic pop\n")
    header_file.write("#elif defined(_MSC_VER)\n")
    header_file.write("#pragma warning(pop)\n")
    header_file.write("#endif\n\n")

    header_file.write(f"#endif // {guard}\n")

def guard_for(output_file):
    stem = os.path.splitext(os.path.basename(output_file))[0]
    return re.sub(r'[^A-Za-z0-9]', '_', stem).upper() + "_H"

def write_if_changed(output_file, text):
    """Write `text` unless the file already holds it.

    One command makes both headers, so a shader edit would otherwise rewrite the
    include table too and recompile shader.c, whose chunks only change when
    include/ does. Ninja restats a custom command's outputs, so one left untouched
    rebuilds nothing that depends on it.
    """
    try:
        with open(output_file, 'r', encoding='utf-8') as existing:
            if existing.read() == text:
                return
    except OSError:
        pass
    with open(output_file, 'w', encoding='utf-8') as header_file:
        header_file.write(text)

def main(input_dir, output_file, includes_out=None, raw=False):
    # Sorted so the generated header is reproducible across machines: two builds
    # of the same sources must diff clean, which is how a refactor proves it
    # changed no shader code. listdir does not recurse, so chunks under
    # include/ never become their own *_shader_str variables -- they only ever
    # appear expanded into a shader, or as rows of the include table below.
    shaders = sorted(f for f in os.listdir(input_dir) if f.endswith('.glsl'))

    # A variable is named from its file, which an app may call anything: every character a C
    # identifier cannot hold becomes an underscore, and two files that come out the same are
    # refused rather than defining one variable twice.
    names = {}
    for shader in shaders:
        name = re.sub(r'[^A-Za-z0-9_]', '_', os.path.splitext(shader)[0])
        if name[0].isdigit():
            name = '_' + name
        if name in names.values():
            raise SystemExit(f"Error: {shader} and another shader in {input_dir} are both named {name}")
        names[shader] = name

    guard = guard_for(output_file)
    header_file = io.StringIO()
    write_open(header_file, guard)
    for shader in shaders:
        shader_path = os.path.join(input_dir, shader)
        shader_var_name = names[shader] + "_shader"
        # RAW leaves every #include where it stands, for the runtime resolver to
        # expand against the engine's table: an app's shaders are built into the
        # app, and the chunks they name are the engine's, which only the engine
        # holds.
        shader_string = shader_to_string(shader_path, expand=not raw)
        if shader_string:
            header_file.write(f"static const char* {shader_var_name}_str = \n{shader_string};\n\n")
    write_close(header_file, guard)
    write_if_changed(output_file, header_file.getvalue())

    if includes_out:
        write_include_table(os.path.join(input_dir, INCLUDE_DIR), includes_out)

def write_include_table(include_dir, output_file):
    """Every chunk under include/, by name, UNEXPANDED.

    What a shader built at runtime -- an app's, or one a scene file names -- resolves its
    #include lines against. Unexpanded because include-once is a property of the shader
    being built, not of the chunk: a chunk expanded here would carry its own copy of
    every chunk it includes, and a shader including two of them would define both twice.
    """
    chunks = sorted(f for f in os.listdir(include_dir) if f.endswith('.glsl'))
    guard = guard_for(output_file)
    header_file = io.StringIO()
    write_open(header_file, guard)
    header_file.write("static const struct {\n    const char* name;\n    const char* source;\n"
                      "} shader_include_chunks[] = {\n")
    for chunk in chunks:
        text = shader_to_string(os.path.join(include_dir, chunk), expand=False)
        if text is None:
            raise SystemExit(f"Error: could not read {chunk}")
        # An empty chunk is an empty string, never a missing one: the resolver reads every row.
        source = text or '    ""'
        header_file.write(f"    {{\"{chunk}\",\n{source}}},\n")
    header_file.write("};\n\n")
    header_file.write("static const int shader_include_chunk_count =\n"
                      "    (int)(sizeof(shader_include_chunks) / sizeof(shader_include_chunks[0]));\n\n")
    write_close(header_file, guard)
    write_if_changed(output_file, header_file.getvalue())

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate shader string header.")
    parser.add_argument('input_dir', type=str, help='Directory containing shader files')
    parser.add_argument('output_file', type=str, help='Output header file path')
    parser.add_argument('--includes-out', type=str, default=None,
                        help='Also write the table of include/ chunks, unexpanded, here')
    parser.add_argument('--raw', action='store_true',
                        help='Leave #include lines for the runtime resolver (an app\'s shaders)')
    args = parser.parse_args()
    main(args.input_dir, args.output_file, args.includes_out, args.raw)
