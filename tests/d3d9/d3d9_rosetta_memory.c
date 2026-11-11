/* what a program from before DEP needs of memory under Wine on Rosetta, where writes to executable memory and write
 * faults are slow: built without NX compatibility, so that Wine lets data be executed.
 * - a watched surface rewritten each frame takes little time, and GetWriteWatch reports what was written;
 * - plain writes to allocated memory take little time;
 * - code in memory allocated without PAGE_EXECUTE runs, and runs as rewritten. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define SURFACE (3u * 1024 * 1024)
#define FRAMES 20
#define WATCH_BUDGET_MS 8.0
#define BUDGET_MS 40.0

static int failures;
#define expect(cond, ...) do { if (!(cond)) { printf("wrong: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static double now_ms(void) {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double median(double *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
      double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
    }
  return v[n / 2];
}

static void write_watch(void) {
  static void *written[SURFACE / 4096];
  double times[FRAMES];
  char *surface = VirtualAlloc(NULL, SURFACE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
  ULONG_PTR count;
  DWORD granularity;
  expect(surface != NULL, "no watched surface");
  if (!surface) return;
  for (int f = 0; f < FRAMES; f++) {
    expect(!ResetWriteWatch(surface, SURFACE), "reset failed");
    double t = now_ms();
    memset(surface, f, SURFACE);
    times[f] = now_ms() - t;
  }
  double m = median(times, FRAMES);
  printf("watched surface rewritten in %.1f ms\n", m);
  expect(m < WATCH_BUDGET_MS, "rewriting a watched surface took %.1f ms, budget %.0f", m, WATCH_BUDGET_MS);

  /* whatever the granularity, a page written has to be reported */
  ResetWriteWatch(surface, SURFACE);
  surface[SURFACE / 2] = 1;
  count = SURFACE / 4096;
  expect(!GetWriteWatch(0, surface, SURFACE, written, &count, &granularity), "GetWriteWatch failed");
  int found = 0;
  for (ULONG_PTR i = 0; i < count; i++)
    if ((char *)written[i] == surface + SURFACE / 2) found = 1;
  expect(found, "a written page wasn't reported, of %lu", (unsigned long)count);
  expect(count < SURFACE / 4096, "every page was reported after one write");

  count = SURFACE / 4096;
  expect(!GetWriteWatch(WRITE_WATCH_FLAG_RESET, surface, SURFACE, written, &count, &granularity), "reset read failed");
  count = SURFACE / 4096;
  expect(!GetWriteWatch(0, surface, SURFACE, written, &count, &granularity), "second read failed");
  expect(count == 0, "%lu pages reported with nothing written", (unsigned long)count);
  VirtualFree(surface, 0, MEM_RELEASE);
}

static void plain_writes(void) {
  double times[FRAMES];
  enum { SIZE = 16 * 1024 * 1024 };
  char *memory = VirtualAlloc(NULL, SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  expect(memory != NULL, "no memory");
  if (!memory) return;
  for (int f = 0; f < FRAMES; f++) {
    double t = now_ms();
    for (size_t i = 0; i + 256 <= SIZE; i += 2480)
      memset(memory + i, f, 256);
    times[f] = now_ms() - t;
  }
  double m = median(times, FRAMES);
  printf("plain writes took %.1f ms\n", m);
  expect(m < BUDGET_MS, "plain writes took %.1f ms, budget %.0f", m, BUDGET_MS);
  VirtualFree(memory, 0, MEM_RELEASE);
}

/* mov eax, value; ret */
static void put_code(unsigned char *at, int value) {
  at[0] = 0xb8;
  memcpy(at + 1, &value, 4);
  at[5] = 0xc3;
}

static void data_as_code(void) {
  unsigned char *memory = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  expect(memory != NULL, "no memory for code");
  if (!memory) return;
  int (*code)(void) = (int (*)(void))(memory + 16);
  put_code(memory + 16, 11);
  expect(code() == 11, "code in data memory returned %d", code());
  /* data beside code in the page, as old programs have */
  for (int i = 0; i < 2000; i++)
    memory[1024 + i] = (unsigned char)i;
  expect(code() == 11, "code beside written data returned %d", code());
  expect(memory[1024 + 7] == 7, "data beside code reads %d", memory[1024 + 7]);
  /* running code while writing nearby, and writing pages that have run nothing, stays quick */
  double times[FRAMES], far_times[FRAMES];
  for (int f = 0; f < FRAMES; f++) {
    double t = now_ms();
    for (int i = 0; i < 200; i++) {
      memory[1024 + i] = (unsigned char)f;
      code();
    }
    times[f] = now_ms() - t;
    t = now_ms();
    for (int i = 0; i < 200; i++) {
      memory[49152 + i] = (unsigned char)f;
      code();
    }
    far_times[f] = now_ms() - t;
  }
  double near_ms = median(times, FRAMES), far_ms = median(far_times, FRAMES);
  printf("writes beside running code took %.1f ms, in another page %.1f ms\n", near_ms, far_ms);
  expect(far_ms < BUDGET_MS, "writes in another page than running code took %.1f ms, budget %.0f", far_ms, BUDGET_MS);
  /* rewritten code runs as rewritten */
  put_code(memory + 16, 22);
  FlushInstructionCache(GetCurrentProcess(), memory, 65536);
  expect(code() == 22, "rewritten code returned %d", code());
  /* a page not yet executed */
  int (*later)(void) = (int (*)(void))(memory + 32768);
  put_code(memory + 32768, 33);
  expect(later() == 33, "code in another page returned %d", later());
  VirtualFree(memory, 0, MEM_RELEASE);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  DWORD flags = 0;
  BOOL permanent = FALSE;
  if (GetProcessDEPPolicy(GetCurrentProcess(), &flags, &permanent))
    printf("DEP flags %lu\n", flags);
  expect(!(flags & PROCESS_DEP_ENABLE), "built to run without DEP, but DEP is on");
  write_watch();
  plain_writes();
  data_as_code();
  printf("%s: %d failure(s)\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
