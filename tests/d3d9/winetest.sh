#!/bin/bash
# Runs one of Wine's d3d9 conformance suites against the Mullion runtime run.sh set up, and counts failures per test.
# Usage: tests/d3d9/winetest.sh <wine-build-dir> <wine-source-dir> <suite> [--each] [test_function ...|--list]
#   suite: visual, device, stateblock, d3d9ex; naming functions runs only those, -name skips one, --list prints them
# The tests run in one process. One that stalls (no output for STALL seconds) or crashes is recorded, its stacks
# kept, and a new process resumes with the tests after it; --each gives every test its own process instead.
set -uo pipefail

build="$(cd "${1:?wine build dir}" && pwd)"
source="$(cd "${2:?wine source dir}" && pwd)"
suite="${3:?suite}"
shift 3
work="$(pwd)/toolchains/test"
export WINEPREFIX="$work/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
filter="$(cd "$(dirname "$0")" && pwd)/winetest_filter.py"
suite_source="$source/dlls/d3d9/tests/$suite.c"

"$(dirname "$0")/install.sh"

all=($(python3 -I "$filter" "$suite_source" /dev/null --list))
if [[ "${1:-}" == --list ]]; then
  printf '%s\n' "${all[@]}"
  exit
fi
each=0
if [[ "${1:-}" == --each ]]; then
  each=1
  shift
fi

# a copy of the suite that runs only the functions in WINETEST_ONLY, linked with Wine's other test objects
one="$work/one"
mkdir -p "$one"
exe="$one/d3d9_test-$suite.exe"
objs="$build/dlls/d3d9/tests/x86_64-windows"
if [[ ! -f "$one/$suite.o" || "$suite_source" -nt "$one/$suite.o" || "$filter" -nt "$one/$suite.o" ]]; then
  python3 -I "$filter" "$suite_source" "$one/$suite.c" || exit 1
  x86_64-w64-mingw32-gcc -c -o "$one/$suite.o" "$one/$suite.c" -I"$build/dlls/d3d9/tests" \
    -I"$source/dlls/d3d9/tests" -I"$build/include" -I"$source/include" -I"$source/include/msvcrt" \
    -D_MSVCR_VER=0 -D__WINESRC__ -D__WINE_PE_BUILD -fno-strict-aliasing -mcx16 -mcmodel=small -O2 -w || { echo "winetest.sh: compiling the filtered suite failed"; exit 1; }
  rm -f "$exe"
fi
if [[ ! -f "$exe" ]]; then
  linked=()
  for name in d3d9ex device stateblock visual testlist; do
    if [[ $name == "$suite" ]]; then linked+=("$one/$suite.o"); else linked+=("$objs/$name.o"); fi
  done
  (cd "$build" && tools/winegcc/winegcc -o "$exe" --wine-objdir . -b x86_64-w64-mingw32 "${linked[@]}" \
    dlls/d3d9/x86_64-windows/libd3d9.a dlls/user32/x86_64-windows/libuser32.a dlls/gdi32/x86_64-windows/libgdi32.a \
    dlls/winecrt0/x86_64-windows/libwinecrt0.a dlls/msvcrt/x86_64-windows/libmsvcrt.a \
    dlls/kernel32/x86_64-windows/libkernel32.a dlls/ntdll/x86_64-windows/libntdll.a) || { echo "winetest.sh: linking the filtered suite failed"; exit 1; }
fi

tests=()
skip=" "
for arg in "$@"; do
  if [[ $arg == -* ]]; then skip+="${arg#-} "; else tests+=("$arg"); fi
done
((${#tests[@]})) || tests=("${all[@]}")
# in the suite's order, the order they run in, so resuming after one skips exactly those before it
wanted=" ${tests[*]} "
pending=()
for test in "${all[@]}"; do
  [[ $wanted == *" $test "* && $skip != *" $test "* ]] && pending+=("$test")
done
out="$work/logs/winetest-$suite.out"
(($#)) && out="$work/logs/winetest-$suite-only.out"
: > "$out"
rm -f "$out".*.stall.txt "$out".part

# Runs the listed tests in one process into $out.part; sets $finished (Wine's summary line seen) and $current.
run() {
  local list
  list="$(IFS=,; echo "$*")"
  WINETEST_ONLY="$list" "$work/wine/bin/wine" "$exe" "$suite" > "$out.part" 2>&1 &
  local pid=$!
  stalled=0
  for ((i = 0; ; i++)); do
    kill -0 $pid 2>/dev/null || break
    if ((i >= 5 && $(date +%s) - $(stat -f %m "$out.part") > ${STALL:-60})); then
      current="$(grep '^### ' "$out.part" | tail -1 | cut -c5- | tr -d '\r')"
      sample $pid 1 -f "$out.${current:-start}.stall.txt" > /dev/null 2>&1
      "$work/wine/bin/wineserver" -k
      # the next process can't start until the server is gone
      "$work/wine/bin/wineserver" -w
      stalled=1
      break
    fi
    sleep 1
  done
  wait $pid 2>/dev/null
  current="$(grep '^### ' "$out.part" | tail -1 | cut -c5- | tr -d '\r')"
  finished=0
  grep -q 'tests executed' "$out.part" && finished=1
  cat "$out.part" >> "$out"
  rm -f "$out.part"
}

broken=()
while ((${#pending[@]})); do
  if ((each)); then
    run "${pending[0]}"
    current="${pending[0]}"
  else
    run "${pending[@]}"
  fi
  if ((!finished)); then
    if ((stalled)); then
      echo "STALLED: ${current:-before the first test} (stacks: $out.${current:-start}.stall.txt)"
    else
      echo "CRASHED: ${current:-before the first test}"
    fi
    broken+=("${current:-?}")
    [[ -n $current ]] || break
  elif ((!each)); then
    break
  fi
  # resume after the test that ran last
  rest=()
  seen=0
  for test in "${pending[@]}"; do
    ((seen)) && rest+=("$test")
    [[ $test == "$current" ]] && seen=1
  done
  pending=(${rest[@]+"${rest[@]}"})
done

python3 -I - "$suite_source" "$out" ${broken[@]+"${broken[@]}"} <<'EOF'
import collections, re, sys
failures = collections.Counter()
# Wine counts checks it marks todo_wine as failures when they pass; those are places Mullion is right and Wine isn't
better = executed = 0
current = '?'
for line in open(sys.argv[2], errors='replace'):
    if line.startswith('### '):
        current = line[4:].strip()
    elif re.search(r'\.c:\d+: Test failed', line):
        failures[current] += 1
    elif 'Test succeeded inside todo block' in line:
        better += 1
    elif m := re.search(r'(\d+) tests executed', line):
        executed += int(m.group(1))
for name, count in failures.most_common():
    print(f'{count:5} {name}')
print(f'{executed} checks, {sum(failures.values())} failures in {len(failures)} tests, {better} todo_wine checks passing')
if sys.argv[3:]:
    print('stalled or crashed: ' + ' '.join(sys.argv[3:]))
EOF
echo "log: $out"
