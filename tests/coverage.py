#!/usr/bin/env python3
"""which of the libraries' connections the tests reach, as numbers and as a list of the ones they do not.

a connection is a way into a library from outside it:
- an export: a function the library exports, which an application or another library calls by name. winemetal.dll's
  exports are the Windows side of the Metal bridge, one for each call into the unix library;
- a method: a member function that has the name of a method of an interface in the Direct3D and DXGI headers, which
  an application reaches through a COM pointer. the names are read from the headers the build compiles against.

a coverage build is the first build over again with every function of its libraries for Windows noting that it ran
(the compiler's -fsanitize-coverage=func,trace-pc, and tests/coverage/hook.c linked in); `setup` makes one. the tests
then run with MULLION_COVERAGE naming a directory, and `report` names the functions the notes belong to.

`shaders` counts the other side, the shader converter, which runs in the unix library: which DXIL and DXBC
operations the tests' shaders use (the tests write them to the directory MULLION_TEST_SHADERS names), in which
stage and overload, against the converter's own tables and against the shaders of an application.
"""
import bisect
import configparser
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import run

FLAG = "-fsanitize-coverage=func,trace-pc"
# the headers whose interfaces count: Direct3D 10 to 12 and DXGI, as C declares their method tables
HEADERS = ("d3d12.h", "d3d12sdklayers.h", "d3d11_4.h", "d3d11on12.h", "d3d10_1.h", "dxgi1_6.h")
TABLE = re.compile(r"typedef struct (\w+)Vtbl\s*\{(.*?)\}\s*\1Vtbl;", re.S)
# a table's entry, a pointer to a function: "(convention *Name)("
ENTRY = re.compile(r"\*\s*(\w+)\s*\)\s*\(")


def tools(build):
    """the build's C compiler for Windows, and the directory of its toolchain's programs"""
    cc = json.loads((build / "meson-info" / "intro-compilers.json").read_text())["host"]["c"]["exelist"]
    return cc, Path(shutil.which(cc[0])).parent


def setup(like, out):
    """configures and builds `out` as `like` was, with coverage"""
    cc, _ = tools(like)
    out.mkdir(parents=True, exist_ok=True)
    hook = out / "coverage_hook.o"
    subprocess.run(cc + ["-O2", "-c", Path(__file__).parent / "coverage" / "hook.c", "-o", hook], check=True)
    made = configparser.ConfigParser()
    made.read(like / "meson-private" / "cmd_line.txt")
    source = json.loads((like / "meson-info" / "meson-info.json").read_text())["directories"]["source"]
    options = {**made["options"], "enable_tests": "false", "c_args": FLAG, "cpp_args": FLAG,
               "c_link_args": str(hook), "cpp_link_args": str(hook)}
    files = [f"--{kind.replace('_', '-')}={path}" for kind in ("cross_file", "native_file")
             for path in json.loads(made["properties"].get(kind, "[]").replace("'", '"'))]
    subprocess.run(["meson", "setup", out, source, *files, *(f"-D{k}={v}" for k, v in options.items())], check=True)
    subprocess.run(["ninja", "-C", out], check=True)


def methods(cc):
    """the names of the interfaces' methods"""
    source = "#define CINTERFACE\n#define COBJMACROS\n" + "".join(f"#include <{header}>\n" for header in HEADERS)
    text = subprocess.run(cc + ["-E", "-x", "c", "-"], input=source, capture_output=True, text=True, check=True).stdout
    return {name for _, table in TABLE.findall(text) for name in ENTRY.findall(table)}


def member(symbol):
    """a demangled function's class and name, or none for a function that is no member"""
    name = symbol.split(" thunk to ")[-1]
    depth, parts, start = 0, [], 0
    for i, c in enumerate(name):
        depth += c in "<(" and 1 or c in ">)" and -1 or 0
        if c == "(" and depth == 1:
            parts.append(name[start:i])
            break
        if name.startswith("::", i) and not depth:
            parts.append(name[start:i])
            start = i + 2
    # the libraries' own classes are in their namespace
    return (parts[-2], parts[-1]) if len(parts) > 2 and parts[0] == "dxmt" else None


def report(build, notes, out):
    """prints each library's reached connections of all it has, and writes the unreached ones' names to `out`"""
    cc, bin = tools(build)
    interface_methods = methods(cc)
    unreached, total = [], [0, 0]
    for dll in sorted(run.libraries([build])[1].values()):
        data = dll.read_bytes()
        header = int.from_bytes(data[0x3C:0x40], "little")
        # a PE32+ image's preferred address (IMAGE_OPTIONAL_HEADER64.ImageBase), which its symbols' addresses include
        image_base = int.from_bytes(data[header + 24 + 24:header + 24 + 32], "little")
        symbols = subprocess.run([bin / "llvm-nm", "-C", "--defined-only", "-n", dll], capture_output=True, text=True, check=True).stdout
        functions = [(int(address, 16) - image_base, name) for address, kind, name in
                     (line.split(" ", 2) for line in symbols.splitlines() if line.count(" ") >= 2) if kind in "Tt"]
        starts = [address for address, _ in functions]
        ran = set()
        # a note has the library's name as the process loaded it, in any case
        for note in (note for note in notes.iterdir() if note.name.lower().startswith(dll.name.lower() + ".")):
            for line in note.read_text().split():
                at = bisect.bisect_right(starts, int(line, 16)) - 1
                if at >= 0:
                    ran.add(functions[at][1])
        exported = set(re.findall(r"Name: (\w+)", subprocess.run([bin / "llvm-readobj", "--coff-exports", dll], capture_output=True, text=True).stdout))
        kinds = {
            "exports": {name: name in ran for _, name in functions if name in exported},
            "methods": {},
        }
        for _, name in functions:
            belongs = member(name)
            if belongs and belongs[1] in interface_methods:
                # a template's instances are one class
                key = f"{belongs[0].split('<')[0]}::{belongs[1]}"
                kinds["methods"][key] = kinds["methods"].get(key, False) or name in ran
        for kind, found in kinds.items():
            if found:
                reached = sum(found.values())
                total[0] += reached
                total[1] += len(found)
                print(f"{dll.name:16} {kind:8} {reached:5} of {len(found):5} reached ({100 * reached // len(found)}%)")
                unreached += [f"{dll.name} {kind[:-1]} {name}" for name, hit in sorted(found.items()) if not hit]
    out.write_text("\n".join(unreached) + "\n")
    print(f"{total[0]} of {total[1]} connections reached; the {len(unreached)} others are named in {out}")


def shader_parts(path):
    """a shader container's parts by name (DxilContainerHeader, or DXBC's header of the same shape)"""
    data = path.read_bytes()
    word = lambda at: int.from_bytes(data[at:at + 4], "little")
    if data[:4] != b"DXBC":
        return {}
    return {data[at:at + 4].decode("latin1"): data[at + 8:at + 8 + word(at + 4)] for at in (word(32 + 4 * i) for i in range(word(28)))}


def dxbc_uses(code, opcodes):
    """the instructions of a DXBC shader's SHEX or SHDR part, as (operation's name, stage): tokens of an opcode in
    the low 11 bits and a length in bits 24 to 30, or in the next token for custom data (d3d12tokenizedprogramformat)"""
    word = lambda at: int.from_bytes(code[at:at + 4], "little")
    stage, at, uses = word(0) >> 16, 8, set()
    while at < len(code):
        token = word(at)
        op, length = token & 0x7FF, token >> 24 & 0x7F
        if op < len(opcodes):
            uses.add((opcodes[op], stage))
        at += 4 * (word(at + 4) if opcodes[op:op + 1] == ["D3D10_SB_OPCODE_CUSTOMDATA"] else length or 1)
    return uses


def shaders(build, source, reference, tested, out):
    """sets the operations the shaders of two directories use against each other and against the converter's own
    tables: `reference` is what an application's shaders use, `tested` what the tests' shaders use"""
    converter = (source / "src" / "airconv" / "dxil_converter.cpp").read_text()
    # what the converter lowers: its table of operations, the ray query operations that are one call of Metal's, and
    # the ones it finds by their function's name
    lowered = {int(number): name for name, number in re.findall(r"^  (\w+) = (\d+),", re.search(r"enum Op : uint32_t \{(.*?)\n\};", converter, re.S)[1], re.M)}
    lowered |= {int(number): name for number, name in re.findall(r'\{(\d+), "(\w+)"', re.search(r"ray_query_getters\[\] = \{(.*?)\};", converter, re.S)[1])}
    named = set(re.findall(r'"dx\.op\.(\w+)', converter))
    kinds = ("ps", "vs", "gs", "hs", "ds", "cs", "lib", "raygen", "intersection", "anyhit", "closesthit", "miss", "callable", "ms", "as")
    opcodes = re.findall(r"^\s*(D3D\w*_SB_OPCODE_\w+)\s*,", re.search(r"typedef enum D3D10_SB_OPCODE_TYPE \{(.*?)D3D10_SB_NUM_OPCODES",
                         (source / "libs" / "DXBCParser" / "d3d12tokenizedprogramformat.hpp").read_text(), re.S)[1], re.M)
    handled = set(re.findall(r"D3D\w*_SB_OPCODE_\w+", "".join((source / "src" / "airconv" / f).read_text() for f in
                                                              ("dxbc_instructions.cpp", "dxbc_converter_cfg.cpp", "dxbc_signature.cpp"))))
    uses = {}
    for where, directory in (("reference", reference), ("tested", tested)):
        files = sorted(directory.glob("*.dxbc"))
        dxil, dxbc = set(), set()
        for at in range(0, len(files), 256):
            listed = subprocess.run([build / "tests" / "host" / "dxilops", *files[at:at + 256]], capture_output=True, text=True, check=True).stdout
            for line in listed.splitlines():
                # a function's name may have spaces in it (a type's): a call starts with its operation's number
                kind, *calls = re.split(r" (?=\d+:dx\.op\.)", line.split(" kind ")[1])
                calls = [call.replace(" ", "_") for call in calls]
                dxil |= {(int(call.split(":")[0]), call.split(":", 1)[1].rsplit(":", 1)[0].split(".", 2)[2], kinds[int(kind)]) for call in calls}
                lowered |= {int(call.split(":")[0]): call.split(":")[1].split(".")[2] for call in calls if call.split(":")[1].split(".")[2] in named}
        for file in files:
            parts = shader_parts(file)
            for part in ("SHEX", "SHDR"):
                dxbc |= dxbc_uses(parts[part], opcodes) if part in parts else set()
        uses[where] = dxil, dxbc
        print(f"{where}: {len(files)} shaders in {directory}")
    lines = []
    for what, n, known, name in (("DXIL operations", 0, set(lowered), lambda use: f"{lowered.get(use[0], 'operation ' + str(use[0]))} as {use[1]} in {use[2]}"),
                                 ("DXBC operations", 1, handled, lambda use: f"{use[0]} in {kinds[use[1]]}")):
        want, have = uses["reference"][n], uses["tested"][n]
        missed = sorted(want - have)
        unknown = sorted({use[0] for use in want | have} - known, key=str)
        unused = sorted(known - {use[0] for use in have}, key=str)
        print(f"{what}: the converter has {len(known)}; the tests' shaders use {len({u[0] for u in have})} of them in {len(have)} uses "
              f"(operation, overload, stage); the reference's shaders have {len(want)} uses, {len(want) - len(missed)} of them tested")
        lines += [f"{what}: in the reference and in no test: {name(use)}" for use in missed]
        lines += [f"{what}: in shaders and not in the converter's table: {lowered.get(op, op) if n == 0 else op}" for op in unknown]
        lines += [f"{what}: in the converter and in no test: {lowered[op] if n == 0 else op}" for op in unused]
    out.write_text("\n".join(lines) + "\n")
    print(f"{len(lines)} gaps are named in {out}")


if __name__ == "__main__":
    if len(sys.argv) == 7 and sys.argv[1] == "shaders":
        shaders(*(Path(a).resolve() for a in sys.argv[2:]))
        sys.exit()
    if len(sys.argv) != 4 or sys.argv[1] not in ("setup", "report"):
        sys.exit(f"usage: {sys.argv[0]} setup <a build> <the coverage build to make>\n"
                 f"       {sys.argv[0]} report <the coverage build> <the directory of its notes>\n"
                 f"       {sys.argv[0]} shaders <a build with tests> <the source tree> <a directory of an application's shaders> "
                 f"<the directory of the tests' shaders (MULLION_TEST_SHADERS)> <the file for the gaps>")
    if sys.argv[1] == "setup":
        setup(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve())
    else:
        report(Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve(), Path(sys.argv[3]).resolve() / "unreached.txt")
