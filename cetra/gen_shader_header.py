import os
import argparse
import re

# Shared shader chunks live here and are pulled in with #include "name.glsl".
# They are expanded at build time, into the same string literals the engine has
# always compiled from -- nothing reads .glsl at runtime.
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
    removed -- see the note above the blank-line handling in shader_to_string.
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

def shader_to_string(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as file:
            include_dir = os.path.join(os.path.dirname(file_path), INCLUDE_DIR)
            content = expand_includes(file.readlines(), include_dir, set())
            processed_lines = []
            for line in content:
                # Remove null bytes before processing the line
                clean_line = line.replace('\x00', '')
                # Strip the line to check if it's empty
                stripped_line = clean_line.strip()
                if stripped_line:
                    # If the line is not empty, process and add the escaped version with newline
                    processed_lines.append('    "' + clean_line.replace('"', '\\"').rstrip() + '\\n"')
                else:
                    # A blank line still has to emit its newline. Emitting a bare
                    # "" instead drops the line from the source the driver sees,
                    # so every GLSL error below it reports a line number short by
                    # the number of blank lines above -- measured at 7 in the
                    # worst shader here before this was fixed.
                    processed_lines.append('    "\\n"')
            return "\n".join(processed_lines)
    except IOError:
        print(f"Error reading file: {file_path}")
        return None

def main(input_dir, output_file):
    # Sorted so the generated header is reproducible across machines: two builds
    # of the same sources must diff clean, which is how a refactor proves it
    # changed no shader code. listdir does not recurse, so chunks under
    # include/ never become their own *_shader_str variables -- they only ever
    # appear expanded into a shader.
    shaders = sorted(f for f in os.listdir(input_dir) if f.endswith('.glsl'))

    with open(output_file, 'w', encoding='utf-8') as header_file:
        header_file.write("#ifndef SHADER_STRINGS_H\n")
        header_file.write("#define SHADER_STRINGS_H\n\n")

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

        for shader in shaders:
            shader_path = os.path.join(input_dir, shader)
            shader_var_name = os.path.splitext(shader)[0].replace('.', '_') + "_shader"
            shader_string = shader_to_string(shader_path)
            if shader_string:
                header_file.write(f"static const char* {shader_var_name}_str = \n{shader_string};\n\n")

        # Re-enable the unused variable warning
        header_file.write("#if defined(__GNUC__)\n")
        header_file.write("#pragma GCC diagnostic pop\n")
        header_file.write("#elif defined(__clang__)\n")
        header_file.write("#pragma clang diagnostic pop\n")
        header_file.write("#elif defined(_MSC_VER)\n")
        header_file.write("#pragma warning(pop)\n")
        header_file.write("#endif\n\n")

        header_file.write("#endif // SHADER_STRINGS_H\n")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate shader string header.")
    parser.add_argument('input_dir', type=str, help='Directory containing shader files')
    parser.add_argument('output_file', type=str, help='Output header file path')
    args = parser.parse_args()
    main(args.input_dir, args.output_file)
