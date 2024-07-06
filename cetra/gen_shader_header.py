import os
import argparse

def shader_to_string(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as file:
            content = file.readlines()
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
                    # For empty lines, add an empty string literal that doesn't close
                    processed_lines.append('    ""')
            return "\n".join(processed_lines)
    except IOError:
        print(f"Error reading file: {file_path}")
        return None

def main(input_dir, output_file):
    shaders = [f for f in os.listdir(input_dir) if f.endswith('.glsl')]

    with open(output_file, 'w') as header_file:
        header_file.write("#ifndef SHADER_STRINGS_H\n")
        header_file.write("#define SHADER_STRINGS_H\n\n")

        for shader in shaders:
            shader_path = os.path.join(input_dir, shader)
            shader_var_name = os.path.splitext(shader)[0].replace('.', '_') + "_shader"
            shader_string = shader_to_string(shader_path)
            if shader_string:
                header_file.write(f"static const char* {shader_var_name}_str = \n{shader_string};\n\n")

        header_file.write("#endif // SHADER_STRINGS_H\n")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate shader string header.")
    parser.add_argument('input_dir', type=str, help='Directory containing shader files')
    parser.add_argument('output_file', type=str, help='Output header file path')
    args = parser.parse_args()
    main(args.input_dir, args.output_file)
