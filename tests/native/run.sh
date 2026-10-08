#!/bin/sh
# builds and runs each probe of Metal's own behaviour here and compares what it prints with <probe>.txt, which is
# what an Apple10 GPU printed. these are not tests of the library: a difference on another GPU family is a finding
# about that family, and the constants that cite a probe have to be looked at again. run it under the GPU lock.
cd "$(dirname "$0")" && out=$(mktemp -d) && code=0
for probe in *.swift; do
  name=${probe%.swift}
  xcrun swiftc -O "$probe" -o "$out/$name" && "$out/$name" > "$out/$name.txt" && diff "$name.txt" "$out/$name.txt" &&
    echo "$name: as recorded" || code=1
done
rm -rf "$out"
exit $code
