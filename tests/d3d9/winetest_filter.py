#!/usr/bin/env python3
# Writes a copy of a Wine test suite whose START_TEST runs only the functions named in WINETEST_ONLY.
# Usage: winetest_filter.py <suite.c> <out.c> [--list]
import re
import sys

source, out = sys.argv[1], sys.argv[2]
text = open(source).read()
start = re.search(r'^START_TEST\((\w+)\)\s*\{', text, re.M)
if not start:
    sys.exit(f'{source}: no START_TEST')
body = text[start.end():]
call = re.compile(r'^(\s+)(\w+)\(\);$', re.M)
names = list(dict.fromkeys(m.group(2) for m in call.finditer(body)))
if '--list' in sys.argv:
    print('\n'.join(names))
    sys.exit()

guard = '''
static int winetest_only(const char *name)
{
    const char *list = getenv("WINETEST_ONLY");
    size_t len = strlen(name);
    int included = 0, includes = 0;
    if (!list || !*list) return 1;
    while (*list)
    {
        const char *end = strchr(list, ',');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        if (*list == '-')
        {
            if (n - 1 == len && !strncmp(list + 1, name, n - 1)) return 0;
        }
        else
        {
            ++includes;
            if (n == len && !strncmp(list, name, n)) included = 1;
        }
        if (!end) break;
        list = end + 1;
    }
    return included || !includes;
}

/* tells winetest.sh which test is running, so it can resume after one that hangs or crashes */
static int winetest_mark(const char *name)
{
    printf("### %s\\n", name);
    fflush(stdout);
    return 1;
}

'''
body = call.sub(lambda m: f'{m.group(1)}if (winetest_only("{m.group(2)}") && winetest_mark("{m.group(2)}")) {m.group(2)}();', body)
with open(out, 'w') as f:
    f.write(f'#line 1 "{source}"\n')
    line = text.count('\n', 0, start.start()) + 1
    f.write(text[:start.start()] + guard + f'#line {line} "{source}"\n' + text[start.start():start.end()] + body)
