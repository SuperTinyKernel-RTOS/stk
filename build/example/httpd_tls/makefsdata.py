#!/usr/bin/env python3
import argparse
import hashlib
import io
import mimetypes
from pathlib import Path
import re

response_types = {
  200: "HTTP/1.0 200 OK",
  400: "HTTP/1.0 400 Bad Request",
  404: "HTTP/1.0 404 File not found",
  501: "HTTP/1.0 501 Not Implemented",
}

PAYLOAD_ALIGNMENT = 4
HTTPD_SERVER_AGENT = "lwIP/2.2.0d (http://savannah.nongnu.org/projects/lwip)"
LWIP_HTTPD_SSI_EXTENSIONS = [".shtml", ".shtm", ".ssi", ".xml", ".json"]

def process_file(input_dir, file):
    results = []

    # Check content type
    content_type, content_encoding = mimetypes.guess_type(file)
    if content_type is None:
        content_type = "application/octet-stream"

    # file name with posix directory separators 
    file_path_posix = file.relative_to(input_dir).as_posix()
    data = f"/{file_path_posix}\x00"
    comment = f"\"/{file_path_posix}\" ({len(data)} chars)"
    while(len(data) % PAYLOAD_ALIGNMENT != 0):
        data += "\x00"
    results.append({'data': bytes(data, "utf-8"), 'comment': comment});

    # Header
    response_type = 200
    for response_id in response_types:
        if file.name.startswith(f"{response_id}."):
            response_type = response_id
            break
    data = f"{response_types[response_type]}\r\n"
    comment = f"\"{response_types[response_type]}\" ({len(data)} chars)"
    results.append({'data': bytes(data, "utf-8"), 'comment': comment});

    # user agent
    data = f"Server: {HTTPD_SERVER_AGENT}\r\n"
    comment = f"\"Server: {HTTPD_SERVER_AGENT}\" ({len(data)} chars)"
    results.append({'data': bytes(data, "utf-8"), 'comment': comment});

    if file.suffix not in LWIP_HTTPD_SSI_EXTENSIONS:
        # content length
        file_size = file.stat().st_size
        data = f"Content-Length: {file_size}\r\n"
        comment = f"\"Content-Length: {file_size}\" ({len(data)} chars)"
        results.append({'data': bytes(data, "utf-8"), 'comment': comment});

    # content type and content encoding
    content_type_header = f"Content-Type: {content_type}"
    if content_encoding is None:
        data = f"{content_type_header}\r\n\r\n"
        comment = f"\"{content_type_header}\" ({len(data)} chars)"
    else:
        content_encoding_header = f"Content-Encoding: {content_encoding}"
        data = f"{content_type_header}\r\n{content_encoding_header}\r\n\r\n"
        comment = f"\"{content_type_header} {content_encoding_header}\" ({len(data)} chars)"
    results.append({'data': bytes(data, "utf-8"), 'comment': comment});

    # file contents
    data = file.read_bytes()
    comment = f"raw file data ({len(data)} bytes)"
    results.append({'data': data, 'comment': comment});

    return results;

def process_file_list(fd, input, base_dirs):
    data = []
    fd.write("#include \"lwip/apps/fs.h\"\n")
    fd.write("\n")
    # generate the page contents
    for file in input:
        if not file.is_file():
            raise RuntimeError(f"File not found: {file}")
        
        # Take the input directory relative to matching base folder
        input_dir = file.parent
        for base in base_dirs:
            if file.is_relative_to(base):
                input_dir = base if base.is_dir() else base.parent
                break

        results = process_file(input_dir, file)

        # make a variable name
        var_name = str(file.relative_to(input_dir))
        var_name = re.sub(r"\W+", "_", var_name, flags=re.ASCII)

        # Add a suffix if the variable name is used already
        if any(d["data_var"] == f"data_{var_name}" for d in data):
            var_name += f"_{len(data)}"

        data_var = f"data_{var_name}"
        file_var = f"file_{var_name}"

        # variable containing the raw data
        fd.write(f"static const unsigned char {data_var}[] = {{\n")
        for entry in results:
            fd.write(f"\n    /* {entry['comment']} */\n")
            byte_count = 0
            for b in entry['data']:
                if byte_count % 16 == 0:
                    fd.write("    ")
                byte_count += 1
                fd.write(f"0x{b:02x},")
                if byte_count % 16 == 0:
                    fd.write("\n")
            if byte_count % 16 != 0:
                fd.write("\n")
        fd.write(f"}};\n\n")

        # set the flags
        flags = "FS_FILE_FLAGS_HEADER_INCLUDED"
        if file.suffix not in LWIP_HTTPD_SSI_EXTENSIONS:
            flags += " | FS_FILE_FLAGS_HEADER_PERSISTENT"
        else:
            flags += " | FS_FILE_FLAGS_SSI"

        # add variable details to the list
        data.append({'data_var': data_var, 'file_var': file_var, 'name_size': len(results[0]['data']), 'flags': flags})

    # generate the page details
    last_var = "NULL"
    for entry in data:
        fd.write(f"const struct fsdata_file {entry['file_var']}[] = {{{{\n")
        fd.write(f"    {last_var},\n")
        fd.write(f"    {entry['data_var']},\n")
        fd.write(f"    {entry['data_var']} + {entry['name_size']},\n")
        fd.write(f"    sizeof({entry['data_var']}) - {entry['name_size']},\n")
        fd.write(f"    {entry['flags']},\n")
        fd.write(f"}}}};\n\n")
        last_var = entry['file_var']
    fd.write(f"#define FS_ROOT {last_var}\n")
    fd.write(f"#define FS_NUMFILES {len(data)}\n")

def run_tool():
    parser = argparse.ArgumentParser(prog="makefsdata.py", description="Generates a source file for the lwip httpd server")
    parser.add_argument(
        "-i",
        "--input",
        help="input files or directories to add as http content",
        required=True,
        nargs='+'
    )
    parser.add_argument(
        "-o",
        "--output",
        help="name of the source file to generate",
        required=True,
    )
    args = parser.parse_args()

    mimetypes.init()
    for ext in [".shtml", ".shtm", ".ssi"]:
        mimetypes.add_type("text/html", ext)

    base_dirs = [Path(item).resolve() for item in args.input]
    file_list = []

    for item in args.input:
        path = Path(item).resolve()
        if path.is_dir():
            # Recursively find all files in subdirectories (sorted to ensure deterministic build)
            for file_path in sorted(path.rglob("*")):
                if file_path.is_file() and file_path not in file_list:
                    file_list.append(file_path)
        elif path.is_file():
            if path not in file_list:
                file_list.append(path)
        else:
            raise RuntimeError(f"Input path does not exist: {item}")

    if not file_list:
        raise RuntimeError("No valid input files found to process.")

    # Write output into an in-memory buffer to verify MD5 checksum before disk updates
    buffer = io.StringIO()
    process_file_list(buffer, file_list, base_dirs)
    generated_content = buffer.getvalue()

    output_path = Path(args.output).resolve()

    # Compare checksum against existing target file
    if output_path.exists():
        existing_hash = hashlib.md5(output_path.read_bytes()).hexdigest()
        new_hash = hashlib.md5(generated_content.encode("utf-8")).hexdigest()

        if existing_hash == new_hash:
            print(f"No changes detected. '{args.output}' kept unchanged.")
            return

    output_path.write_text(generated_content, encoding="utf-8", newline="")
    print(f"Updated '{args.output}'.")

if __name__ == "__main__":
    run_tool()