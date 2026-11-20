"""what a run's result belongs to (docs/TESTING.md, section 3): the source, how the build was made, the Wine it ran
in, and every module it loaded, each by its hash. written into a run's own directory and printed as one line, so a
number in a report names the tree and the artifacts it came from instead of a directory that may since have been
rebuilt.

the identity of an artifact is derived from what was built and what ran, never from when it ran: no time, duration,
process id or attempt number is part of it, so two runs of one artifact have one identity and a run with a module
swapped has another. an attempt's directory keeps its own logs, so a later attempt never overwrites an earlier result.

there are two different records here, and a result needs both:

  modules  what the runner selected and installed from the builds, by hash
  loaded   what the Wine's loader actually loaded, read from its own record (+loaddll) and from the probe's
           GetModuleFileName, by the path Wine resolved and the hash of that file

a module selected but not loaded, or a loaded file whose hash is not the build's, is a provenance failure: the run
measured something other than the tree it names. `verify()` says so; a required acceptance gate refuses to pass.
"""
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

# the build options that decide what a result means, and the ones that decide what a module holds
OPTIONS = ("buildtype", "b_ndebug", "debug", "enable_d3d9", "enable_d3d12", "enable_tests", "wine_builtin_dll",
           "mullion_debug", "enable_nvapi", "enable_nvngx", "build_airconv_for_windows")
# the directories whose uncommitted files are part of the source under test; .agent-obj grows during a campaign and is
# evidence, not source
SOURCE = ("src", "include", "tests", "libs", "tools", "meson.build", "meson.options", "mullion.conf", "version.h.in",
          ".gitmodules", "build-win32.txt", "build-win64.txt", "build-arm64ec.txt")
# the environment a run's own switches come from: every MULLION_* the caller set, and the loader's overrides (a family
# or a Metal version is set per test, and is in the result's own name)
ENVIRONMENT = re.compile(r"^(MULLION_.*|WINEDLLOVERRIDES|WINEDEBUG|DYLD_.*|MTL_.*)$")
# what Wine's loader says when it maps a module (+loaddll): the path it chose, and where it put it
LOADED = re.compile(r'Loaded L"([^"]+)" at ([0-9A-Fa-f]+)')
# the modules a run's provenance is about: the Direct3D frontends, their bridges, and the Metal bridge both load
WATCHED = ("d3d9", "d3d10core", "d3d11", "d3d12", "d3d12core", "dxgi", "winemetal", "nvngx", "nvapi64")

# Wine's names for a PE file's machine (IMAGE_FILE_HEADER.Machine)
MACHINES = {0x8664: "x86_64", 0xAA64: "aarch64", 0x14C: "i386", 0xA641: "arm64ec"}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def machine(path):
    """the machine a PE file is for, from its header: what says whether a run's library was the one for its program"""
    try:
        with open(path, "rb") as file:
            file.seek(0x3C)
            file.seek(int.from_bytes(file.read(4), "little") + 4)
            return MACHINES.get(int.from_bytes(file.read(2), "little"))
    except (OSError, OverflowError):
        return None


def builtin(path):
    """whether the build marked a library as one of Wine's own (winebuild --builtin)"""
    try:
        with open(path, "rb") as file:
            file.seek(0x40)
            return file.read(16) == b"Wine builtin DLL"
    except OSError:
        return False


def git(repository, *arguments):
    """what git says in the repository, or nothing where there is none"""
    try:
        return subprocess.run(["git", "-C", str(repository), *arguments], capture_output=True, text=True,
                              stdin=subprocess.DEVNULL).stdout
    except OSError:
        return ""


def submodules(repository):
    """the revision of every submodule the tree pins, by path: the headers a build reads are source too"""
    said = {}
    for line in git(repository, "submodule", "status", "--recursive").splitlines():
        fields = line.lstrip("+-U ").split()
        if len(fields) >= 2:
            said[fields[1]] = fields[0]
    return said


def source(repository):
    """the source a build was made from: the revision, a hash of what the revision does not hold, and the content of
    every file the revision does not hold, so a changed tree is a different source, an unchanged one is named the same
    twice, and editing an already-untracked file changes the identity as editing a tracked one does"""
    revision = git(repository, "rev-parse", "HEAD").strip() or None
    branch = git(repository, "rev-parse", "--abbrev-ref", "HEAD").strip() or None
    # what the working tree changed in the source under test, and what it added there
    changed = git(repository, "diff", "HEAD", "--", *SOURCE)
    # a file python made while running is not source: it changes whenever this script does, and names nothing
    added = {}
    for line in git(repository, "status", "--porcelain", "--untracked-files=all", "--", *SOURCE).splitlines():
        if not line.startswith("?? "):
            continue
        name = line[3:]
        if "__pycache__" in name or name.endswith((".pyc", ".pyo")):
            continue
        path = pathlib.Path(repository) / name
        added[name] = sha256(path) if path.is_file() else "absent"
    digest = hashlib.sha256(changed.encode()).hexdigest() if changed else hashlib.sha256(b"").hexdigest()
    return {"revision": revision, "branch": branch, "changed": digest, "changed_bytes": len(changed),
            "added": added, "submodules": submodules(repository)}


def said_version(executable):
    """the first line a compiler prints for --version, or nothing where it cannot run here"""
    try:
        said = subprocess.run([str(executable), "--version"], capture_output=True, text=True,
                              stdin=subprocess.DEVNULL, timeout=60).stdout.splitlines()
    except (OSError, subprocess.SubprocessError):
        return None
    return said[0] if said else None


def candidates(program):
    """the files a compiler name can mean on this host: what PATH resolves it to, and every toolchain in this tree's
    bin directory -- a build configured with a toolchain on PATH and a manifest made without it must still name the
    compiler that made the modules"""
    found = []
    for candidate in [shutil.which(program),
                      *sorted(pathlib.Path(__file__).resolve().parent.parent.glob(f"toolchains/*/bin/{program}"))]:
        if candidate and pathlib.Path(candidate).is_file():
            # the entry as the toolchain names it, not the file a wrapper symlink points at: the name is what says
            # which sysroot the compiler's target uses
            entry = pathlib.Path(candidate)
            if entry not in found:
                found.append(entry)
    return found


def resolved(program, wanted):
    """the executable that is the build's compiler: the candidate whose own --version is the version the build
    recorded, so the manifest names llvm-mingw rather than an unrelated compiler of the same name on PATH"""
    found = candidates(program)
    if not found:
        return pathlib.Path(program), False
    chosen = next((path for path in found if wanted and wanted in (said_version(path) or "")), found[0])
    return chosen, bool(wanted) and wanted in (said_version(chosen) or "")


def sysroot(executable, target, program):
    """the SDK a compiler was made against: the mingw sysroot beside a llvm-mingw compiler, and the macOS SDK cc
    selects here. the triple is in the name the build invoked, not the wrapper it resolves to"""
    path = pathlib.Path(executable)
    triple = next((program[: -len(suffix)] for suffix in ("-gcc", "-g++", "-clang", "-clang++")
                   if program.endswith(suffix)), target)
    for parent in path.absolute().parents:
        if parent.name.startswith(("llvm-mingw-", "mingw")):
            root = parent if parent.name.startswith("llvm-mingw-") else parent.parent
            return {"kind": "mingw", "path": str(root / triple if (root / triple).is_dir() else root),
                    "triple": triple, "target": target}
    if path.name.startswith(("cc", "c++", "clang")):
        said = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True,
                              stdin=subprocess.DEVNULL).stdout.strip()
        if said:
            return {"kind": "macos", "path": said, "triple": None, "target": target}
    return None


def build(directory):
    """how a build was made: the options that decide what its modules hold, the compilers and their paths, versions,
    target architecture and SDK, and the machines the build is for and was made on"""
    said = {}
    for name in ("intro-buildoptions.json", "intro-compilers.json", "intro-machines.json"):
        try:
            said[name] = json.loads((pathlib.Path(directory) / "meson-info" / name).read_text())
        except (OSError, json.JSONDecodeError):
            said[name] = None
    options = {entry["name"]: entry["value"] for entry in said["intro-buildoptions.json"] or [] if entry["name"] in OPTIONS}
    machines = said["intro-machines.json"] or {}
    target = (machines.get("host") or {}).get("cpu")
    compilers = {}
    for language, compiler in ((said["intro-compilers.json"] or {}).get("host") or {}).items():
        command = compiler.get("exelist") or []
        if not command:
            continue
        # a wrapper is the build's choice of compiler, but the compiler is what made the module
        program = command[-1] if pathlib.Path(command[0]).name in ("ccache", "sccache", "distcc") else command[0]
        wanted = compiler.get("full_version") or compiler.get("version")
        where, matches = resolved(program, wanted)
        compilers[language] = {"program": pathlib.Path(program).name, "path": str(where),
                               "version": said_version(where) or wanted,
                               "built_with": wanted, "matches_build": matches, "target": target,
                               "sha256": sha256(where) if where.is_file() else None,
                               "sdk": sysroot(where, target, pathlib.Path(program).name)}
    return {"directory": pathlib.Path(directory).name, "options": options, "compilers": compilers,
            "target": {"system": (machines.get("host") or {}).get("system"), "cpu": target},
            "build": {"system": (machines.get("build") or {}).get("system"),
                      "cpu": (machines.get("build") or {}).get("cpu")}}


def wine(at):
    """the Wine the tests ran in, by its own version, and whether it is a copy of one made here (--clone)"""
    if at is None:
        return None
    said = subprocess.run([str(at), "--version"], capture_output=True, text=True, stdin=subprocess.DEVNULL).stdout.strip()
    origin = pathlib.Path(at).resolve()
    return {"version": said or None, "clone": "tests-wine" in str(origin)}


def programs(prefix):
    """Microsoft's shader compilers the prefix holds, by machine: the tests compile their HLSL with these, and Wine's
    own compile fewer of the shaders, so which ones a run used decides whether it measured the library"""
    found = {}
    windows = pathlib.Path(prefix) / "drive_c" / "windows"
    for directory in (windows / "system32", windows / "syswow64"):
        for name in ("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll"):
            path = directory / name
            if path.is_file() and not builtin(path):
                found[f"{directory.name}/{name}"] = {"sha256": sha256(path), "machine": machine(path)}
    return found


def modules(unix, pe):
    """every module a run selects from the builds, each by its hash: what it installs, whichever directory built it"""
    said = {}
    for path in sorted([*unix.values(), *pe.values()]):
        said[path.name] = {"sha256": sha256(path), "machine": machine(path) if path.suffix == ".dll" else "unix",
                           "builtin": builtin(path) if path.suffix == ".dll" else None, "path": str(path)}
    return said


def test_programs(builds, tests):
    """the programs a run declares, each by its hash: a test result belongs to the executable that produced it"""
    said = {}
    for test in tests or []:
        path = pathlib.Path(test["cmd"][0])
        if path.suffix.lower() != ".exe" or not path.is_file():
            continue
        said[test["name"]] = {"path": str(path), "sha256": sha256(path), "machine": machine(path)}
    return said


def windows_path(path, prefix, bits=None):
    """the host path Wine resolves a Windows path to, for a record that names a Windows path. a 32-bit process on a
    64-bit prefix sees system32 redirected to syswow64, so the file the loader named is the one in syswow64"""
    said = str(path)
    drive, _, rest = said.partition(":")
    # Wine's loader log doubles each separator, so collapse runs of them before mapping
    parts = re.sub(r"[\\/]+", "/", rest).lstrip("/")
    if len(drive) == 1:
        root = pathlib.Path(prefix) / f"drive_{drive.lower()}"
        if drive.isalpha() and root.is_dir():
            # the redirector: a 32-bit process's C:\windows\system32 is the prefix's syswow64
            if bits == 32 and parts.lower().startswith("windows/system32/") and (root / "windows" / "syswow64").is_dir():
                return root / "windows" / "syswow64" / parts[len("windows/system32/"):]
            return root / parts
        if drive.upper() == "Z":
            return pathlib.Path("/") / parts
    return pathlib.Path(said)


def loaded(log, prefix):
    """what the loader actually mapped, from Wine's own +loaddll record: every file the run watched, by the path Wine
    chose and the hash of that file. the record is the loader's, not the runner's choice of directory"""
    said, seen = {}, set()
    bits = re.search(r"probe-arch: (\d+)", log)
    bits = int(bits[1]) if bits else None
    for path, address in LOADED.findall(log):
        name = pathlib.Path(re.split(r"[\\/]+", path)[-1].lower()).stem
        if name not in WATCHED or name in seen:
            continue
        seen.add(name)
        at = windows_path(path, prefix, bits)
        if not at.is_file():
            continue
        said[f"{name}.dll"] = {"sha256": sha256(at), "machine": machine(at), "path": str(at),
                               "address": int(address, 16), "evidence": "loader"}
    return said


def probe(builds, at, prefix, environment, names=WATCHED):
    """run the build's loader probe in the Wine, under +loaddll, and read what the loader mapped: a test's result names
    the artifacts it ran with only if the loader's record agrees with the build's files"""
    exe = next((build / "tests" / "manifest_probe.exe" for build in builds
                if (build / "tests" / "manifest_probe.exe").is_file()), None)
    if at is None or exe is None:
        return {}
    log = subprocess.run([str(at), str(exe), *names], env=dict(environment, WINEDEBUG="+loaddll"),
                         capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)
    return loaded("\n".join([log.stdout, log.stderr]), prefix)


def host():
    """the machine and system a result is only true on"""
    def said(*command):
        return subprocess.run(command, capture_output=True, text=True,
                              stdin=subprocess.DEVNULL).stdout.strip() or None
    return {"system": said("sw_vers", "-productVersion"), "build": said("sw_vers", "-buildVersion"),
            "chip": said("sysctl", "-n", "machdep.cpu.brand_string"),
            "kernel": said("uname", "-r")}


def manifest(builds, at, prefix, unix, pe, environment, tests=None, installed=None, loaded_=None, repository=None):
    """what this run's result belongs to"""
    repository = repository or pathlib.Path(__file__).resolve().parent.parent
    return {
        "source": source(repository),
        "builds": [build(directory) for directory in builds],
        "wine": wine(at),
        "host": host(),
        "prefix": pathlib.Path(prefix).name,
        "compilers": programs(prefix),
        "modules": modules(unix, pe),
        "installed": installed if installed is not None else {},
        "loaded": loaded_ if loaded_ is not None else {},
        "programs": test_programs(builds, tests),
        "environment": {name: value for name, value in sorted(environment.items()) if ENVIRONMENT.match(name)},
    }


def digest(said):
    """one hash for a whole manifest, so two runs are the same run without reading them"""
    return hashlib.sha256(json.dumps(said, sort_keys=True).encode()).hexdigest()


def verify(said):
    """whether the record proves that what ran is what the builds made: a missing source, a module installed from
    somewhere other than the build, a loaded file that is not the build's, no loader evidence, or a program whose
    architecture has no module of its architecture. returns the reasons, and acceptance passes only on none"""
    complaints = []
    source_ = said.get("source") or {}
    if not source_.get("revision"):
        complaints.append("no source revision: the run names no tree")
    if not said.get("loaded"):
        complaints.append("no loader evidence: nothing says which files the Wine loaded")
    selected = said.get("modules") or {}
    for name, entry in sorted(selected.items()):
        if entry.get("machine") == "unix":
            continue
        measured = machine(entry["path"]) if pathlib.Path(entry["path"]).is_file() else None
        if measured and entry.get("machine") and measured != entry["machine"]:
            complaints.append(f"{name}: the build says {entry['machine']}, its header says {measured}")
    for name, entry in sorted((said.get("installed") or {}).items()):
        if entry.get("built") and entry.get("sha256") != entry["built"]:
            complaints.append(f"{name}: installed hash is not the build's ({entry['path']})")
    loaded_ = said.get("loaded") or {}
    for name, entry in sorted(loaded_.items()):
        built = selected.get(name)
        if built and built.get("sha256") != entry.get("sha256"):
            complaints.append(f"{name}: the Wine loaded {entry['path']}, which is not the build's file")
    # what the build made and the run meant to install must be what the loader shows; a module of the build that the
    # loader did not map is a gap in the evidence, not a pass. a module of a machine no selected program runs as is
    # the one exception: a PE32 run installs the second build's x86_64 libraries for the Wine it uses (the WoW64
    # bridge's own), and its i386 programs can never load them -- demanding loader evidence for them made every
    # PE32 gate impossible from the commit that built nvapi64.dll and nvngx.dll into that build
    program_machines = {entry.get("machine") for entry in (said.get("programs") or {}).values() if entry.get("machine")}
    for name in sorted(selected):
        machine_ = selected[name].get("machine")
        if machine_ != "unix" and name not in loaded_ and machine_ in program_machines:
            complaints.append(f"{name}: selected from the build but absent from the loader's record")
    # every test program's architecture must be one the Wine runs and one the prefix has modules for; otherwise a
    # test of that architecture did not measure this build
    runnable = {entry.get("machine") for entry in selected.values() if entry.get("machine") not in (None, "unix")}
    for name, entry in sorted((said.get("programs") or {}).items()):
        if entry.get("machine") and entry["machine"] not in runnable:
            complaints.append(f"{name}: program is {entry['machine']}, and no module of that machine was installed")
    return complaints


def write(said, where):
    """the manifest in a file, and one line that names it"""
    where = pathlib.Path(where)
    where.parent.mkdir(parents=True, exist_ok=True)
    where.write_text(json.dumps(said, indent=1, sort_keys=True) + "\n")
    named = digest(said)
    source_ = said["source"]
    libraries = {name: said_["sha256"][:8] for name, said_ in sorted(said["modules"].items()) if name.startswith(WATCHED)}
    # spelled apart from the f-string: the nested same-quote form needs Python 3.12, and the runner must work on the
    # Python the host has (3.9 here)
    if source_["changed_bytes"] or source_["added"]:
        dirty = (" dirty " + source_["changed"][:12] +
                 f" ({source_['changed_bytes']} bytes, {len(source_['added'])} added)")
    else:
        dirty = " clean"
    print(f"manifest {named[:16]} at {where}\n  source: {str(source_['revision'])[:12]}{dirty}"
          f"; wine {(said['wine'] or {}).get('version')}; {len(said['modules'])} modules;"
          f" {len(said.get('loaded') or {})} loaded; {len(said.get('programs') or {})} programs", flush=True)
    if libraries:
        print("  modules: " + " ".join(f"{name}={sha}" for name, sha in libraries.items()), flush=True)
    return named


def emit(builds, at, prefix, unix, pe, environment, tests=None, installed=None, loaded_=None, where=None,
         require=False):
    """write this run's manifest into its own directory, and say what it is. `require` makes a manifest that cannot be
    made, or one that does not prove what ran, a failure of the run: an acceptance gate has no result without it"""
    try:
        said = manifest(builds, at, prefix, unix, pe, environment, tests=tests, installed=installed, loaded_=loaded_)
    except Exception as reason:  # a manifest that cannot be made is said, and fails when the caller requires it
        print(f"manifest: not written ({reason})", file=sys.stderr, flush=True)
        if require:
            sys.exit(f"manifest: not written ({reason}): acceptance requires provenance")
        return None
    where = pathlib.Path(where or pathlib.Path(builds[0]) / "meson-logs" / "manifest.json")
    named = write(said, where)
    complaints = verify(said)
    for complaint in complaints:
        print(f"manifest: {complaint}", file=sys.stderr, flush=True)
    if complaints and require:
        sys.exit("manifest: the record does not prove what ran: " + "; ".join(complaints))
    return named
