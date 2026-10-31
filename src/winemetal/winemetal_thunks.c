#ifdef _WIN32
#define WINEMETAL_API __declspec(dllexport)
#else
#define WINEMETAL_API
#endif

#include "winemetal_thunks.h"
#include <wineunixlib.h>
#include "assert.h"
#include "string.h"

#ifdef NDEBUG
#define UNIX_CALL(code, params) WINE_UNIX_CALL(code, params)
#else
#define UNIX_CALL(code, params)                                                                                        \
  {                                                                                                                    \
    NTSTATUS status = WINE_UNIX_CALL(code, params);                                                                    \
    assert(!status && "unix call failed");                                                                             \
  }
#endif

#define PtrToUInt64(v) ((uint64_t)(uintptr_t)(v))

#ifndef MULLION_NATIVE
#include "winbase.h"
#include "winnls.h"
#include <intrin.h>

int wmt_diag_calls;
static struct WMTDiagCall diag_calls[WMTUnixCallCount];

NTSTATUS
wmt_diag_call(unsigned int code, void *args, const char *name) {
  LARGE_INTEGER began, ended;
  QueryPerformanceCounter(&began);
  NTSTATUS status = WINE_UNIX_CALL_UNTIMED(code, args);
  QueryPerformanceCounter(&ended);
  // the name before the counts: whoever takes a count sees its name
  diag_calls[code].name = name;
  InterlockedIncrement64((LONG64 *)&diag_calls[code].count);
  InterlockedAdd64((LONG64 *)&diag_calls[code].ticks, ended.QuadPart - began.QuadPart);
  return status;
}

WINEMETAL_API void
WMTDiagTimeCalls(void) {
  wmt_diag_calls = 1;
}

/* ntdll's (Wine's winternl.h): 0 is SemaphoreBasicInformation, a SEMAPHORE_BASIC_INFORMATION */
NTSYSAPI NTSTATUS WINAPI NtQuerySemaphore(HANDLE, int, void *, ULONG, ULONG *);

WINEMETAL_API int64_t
WMTDiagSemaphoreCount(void *semaphore) {
  struct {
    LONG current, maximum;
  } info;
  return NtQuerySemaphore(semaphore, 0, &info, sizeof(info), NULL) ? -1 : info.current;
}

WINEMETAL_API void
WMTDiagCallTotals(struct WMTDiagCall calls[WMTUnixCallCount]) {
  for (unsigned int code = 0; code < WMTUnixCallCount; code++) {
    calls[code].count = InterlockedExchange64((LONG64 *)&diag_calls[code].count, 0);
    calls[code].ticks = InterlockedExchange64((LONG64 *)&diag_calls[code].ticks, 0);
    calls[code].name = diag_calls[code].name;
  }
}

// a thread's ring holds about its last few frames of records. a ring outlives its thread, whose records it keeps
// until the next thread that takes it writes over them
enum { kFlightRecords = 4096 };
struct flight_ring {
  struct flight_ring *next;
  DWORD owner;
  // `dumped`: the count the dump holding flight_dump_lock read and copies to, so that what threads write meanwhile
  // cannot take the room it counted for another ring's records
  int64_t written, dumped;
  struct WMTFlightEntry entries[kFlightRecords];
};
static struct flight_ring *flight_rings;
static DWORD flight_slot = TLS_OUT_OF_INDEXES;
static int64_t flight_lost;
static SRWLOCK flight_dump_lock = SRWLOCK_INIT;
static LONG flight_dumps;
// QueryPerformanceCounter is a system call in Wine on macOS (hundreds of ns a record), so records read the CPU's
// counter, which the dump scales by the QueryPerformanceCounter time since the recorder started
static uint64_t flight_started, flight_started_qpc;

static inline uint64_t
flight_ticks(void) {
// ARM64EC code defines __x86_64__ too
#if defined(__aarch64__) || defined(__arm64ec__)
  uint64_t ticks;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return ticks;
#elif defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
#error "the flight recorder has no counter for this CPU"
#endif
}
static const void *flight_exception_site;

WINEMETAL_API const void *
WMTFlightSite(const char *format) {
  size_t size = strlen(format) + 1;
  char *site = HeapAlloc(GetProcessHeap(), 0, size);
  if (site)
    memcpy(site, format, size);
  return site;
}

static struct flight_ring *
flight_take(void) {
  DWORD self = GetCurrentThreadId();
  struct flight_ring *ring;
  for (ring = __atomic_load_n(&flight_rings, __ATOMIC_ACQUIRE); ring; ring = ring->next)
    if (!__atomic_load_n(&ring->owner, __ATOMIC_RELAXED) && !InterlockedCompareExchange((LONG *)&ring->owner, self, 0))
      break;
  if (!ring) {
    if (!(ring = VirtualAlloc(NULL, sizeof(*ring), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
      return NULL;
    ring->owner = self;
    do
      ring->next = __atomic_load_n(&flight_rings, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&flight_rings, &ring->next, ring, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  }
  TlsSetValue(flight_slot, ring);
  return ring;
}

WINEMETAL_API void
WMTFlightRecord(const void *site, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  struct flight_ring *ring = flight_slot == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(flight_slot);
  if (!site || flight_slot == TLS_OUT_OF_INDEXES || (!ring && !(ring = flight_take()))) {
    InterlockedIncrement64(&flight_lost);
    return;
  }
  struct WMTFlightEntry *entry = &ring->entries[ring->written % kFlightRecords];
  entry->time = flight_ticks();
  WMT_MEMPTR_SET(entry->site, site);
  entry->args[0] = a, entry->args[1] = b, entry->args[2] = c, entry->args[3] = d;
  entry->thread = GetCurrentThreadId();
  // a dump reads what is written before the count, and may read the one record being written torn
  __atomic_store_n(&ring->written, ring->written + 1, __ATOMIC_RELEASE);
}

static int64_t
flight_kept(int64_t written) {
  return written < kFlightRecords ? written : kFlightRecords;
}

WINEMETAL_API void
WMTFlightDump(const char *reason) {
  AcquireSRWLockExclusive(&flight_dump_lock);
  uint64_t count = 0, overwritten = 0;
  struct flight_ring *rings = __atomic_load_n(&flight_rings, __ATOMIC_ACQUIRE), *ring;
  for (ring = rings; ring; ring = ring->next)
    count += flight_kept(ring->dumped = __atomic_load_n(&ring->written, __ATOMIC_ACQUIRE));
  struct WMTFlightEntry *entries =
      count ? VirtualAlloc(NULL, count * sizeof(*entries), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
  if (count && !entries)
    count = 0;
  uint64_t at = 0;
  for (ring = rings; ring && at < count; ring = ring->next) {
    int64_t written = ring->dumped, kept = flight_kept(written);
    for (int64_t i = written - kept; i < written; i++)
      entries[at++] = ring->entries[i % kFlightRecords];
    overwritten += written - kept;
  }
  WCHAR path[MAX_PATH];
  char executable[3 * MAX_PATH] = "";
  DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
  WCHAR *name = path + length;
  while (name > path && name[-1] != '\\' && name[-1] != '/')
    name--;
  WideCharToMultiByte(CP_UTF8, 0, name, -1, executable, sizeof(executable), NULL, NULL);
  LARGE_INTEGER frequency, now;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&now);
  uint64_t ticks = flight_ticks(), elapsed = now.QuadPart - flight_started_qpc;
  // where MULLION_DIAG=profile writes its records, when it is set
  char directory[MAX_PATH] = "";
  if (GetEnvironmentVariableA("MULLION_DIAG_PATH", directory, sizeof(directory)) >= sizeof(directory))
    directory[0] = 0;
  struct unixcall_wmtflight_write params;
  WMT_MEMPTR_SET(params.reason, reason);
  WMT_MEMPTR_SET(params.directory, directory);
  WMT_MEMPTR_SET(params.executable, executable);
  WMT_MEMPTR_SET(params.records, entries);
  params.count = at;
  params.frequency = elapsed ? (double)(ticks - flight_started) * frequency.QuadPart / elapsed : 0;
  params.now = ticks;
  params.lost = __atomic_load_n(&flight_lost, __ATOMIC_RELAXED);
  params.overwritten = overwritten;
  UNIX_CALL(WMTUnixFlightWrite, &params);
  flight_dumps++;
  if (entries)
    VirtualFree(entries, 0, MEM_RELEASE);
  ReleaseSRWLockExclusive(&flight_dump_lock);
}

// the exceptions that end a process unless it catches them: the first dumps the records before its handlers run
static LONG CALLBACK
flight_exception(EXCEPTION_POINTERS *pointers) {
  static LONG dumped;
  EXCEPTION_RECORD *record = pointers->ExceptionRecord;
  // informational and warning codes, such as OutputDebugString's, are not failures
  if ((record->ExceptionCode >> 30) != 3)
    return EXCEPTION_CONTINUE_SEARCH;
  WMTFlightRecord(flight_exception_site, record->ExceptionCode, (uint64_t)(uintptr_t)record->ExceptionAddress,
                  record->NumberParameters > 0 ? record->ExceptionInformation[0] : 0,
                  record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0);
  switch (record->ExceptionCode) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_STACK_OVERFLOW:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    if (!InterlockedExchange(&dumped, 1))
      WMTFlightDump("exception");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// a process that ends after a dump dumps what followed it, and MULLION_FLIGHT=exit dumps every process at its end
void
wmt_flight_main(DWORD reason, BOOL exiting) {
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    flight_slot = TlsAlloc();
    LARGE_INTEGER started;
    QueryPerformanceCounter(&started);
    flight_started = flight_ticks();
    flight_started_qpc = started.QuadPart;
    flight_exception_site = WMTFlightSite("exception %#llx at %#llx, information %#llx %#llx");
    AddVectoredExceptionHandler(1, flight_exception);
    break;
  case DLL_THREAD_DETACH: {
    struct flight_ring *ring = TlsGetValue(flight_slot);
    if (ring)
      __atomic_store_n(&ring->owner, 0, __ATOMIC_RELEASE);
    break;
  }
  case DLL_PROCESS_DETACH: {
    char mode[8];
    DWORD length = GetEnvironmentVariableA("MULLION_FLIGHT", mode, sizeof(mode));
    if (exiting && (flight_dumps || (length < sizeof(mode) && !strcmp(mode, "exit"))))
      WMTFlightDump("exit");
    break;
  }
  }
}
#endif

WINEMETAL_API void
NSObject_retain(obj_handle_t obj) {
  UNIX_CALL(0, &obj);
}

WINEMETAL_API void
NSObject_release(obj_handle_t obj) {
  UNIX_CALL(1, &obj);
}

WINEMETAL_API obj_handle_t
NSArray_object(obj_handle_t array, uint64_t index) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = array;
  params.arg = index;
  params.ret = 0;
  UNIX_CALL(2, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
NSArray_count(obj_handle_t array) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = array;
  params.ret = 0;
  UNIX_CALL(3, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
WMTCopyAllDevices() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(4, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_recommendedMaxWorkingSetSize(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(5, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_currentAllocatedSize(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(6, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_name(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(7, &params);
  return params.ret;
}

WINEMETAL_API uint32_t
NSString_getCString(obj_handle_t str, char *buffer, uint64_t maxLength, enum WMTStringEncoding encoding) {
  struct unixcall_nsstring_getcstring params;
  params.str = str;
  params.buffer_ptr = PtrToUInt64(buffer);
  params.max_length = maxLength;
  params.encoding = encoding;
  UNIX_CALL(8, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newCommandQueue(obj_handle_t device, uint64_t maxCommandBufferCount) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = device;
  params.arg = maxCommandBufferCount;
  params.ret = 0;
  UNIX_CALL(9, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
NSAutoreleasePool_alloc_init() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(10, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandQueue_commandBuffer(obj_handle_t queue) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = queue;
  params.ret = 0;
  UNIX_CALL(11, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandBuffer_commit(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_noret params;
  params.handle = cmdbuf;
  UNIX_CALL(12, &params);
  return;
}

WINEMETAL_API void
MTLCommandBuffer_waitUntilCompleted(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_noret params;
  params.handle = cmdbuf;
  UNIX_CALL(13, &params);
  return;
}

WINEMETAL_API enum WMTCommandBufferStatus
MTLCommandBuffer_status(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = cmdbuf;
  params.ret = 0;
  UNIX_CALL(14, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSharedEvent(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(15, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLSharedEvent_signaledValue(obj_handle_t event) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = event;
  params.ret = 0;
  UNIX_CALL(16, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandBuffer_encodeSignalEvent(obj_handle_t cmdbuf, obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_obj_uint64_noret params;
  params.handle = cmdbuf;
  params.arg0 = event;
  params.arg1 = value;
  UNIX_CALL(17, &params);
  return;
}

WINEMETAL_API obj_handle_t
MTLDevice_newBuffer(obj_handle_t device, struct WMTBufferInfo *info) {
  struct unixcall_mtldevice_newbuffer params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(18, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSamplerState(obj_handle_t device, struct WMTSamplerInfo *info) {
  struct unixcall_mtldevice_newsamplerstate params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(19, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newDepthStencilState(obj_handle_t device, const struct WMTDepthStencilInfo *info) {
  struct unixcall_mtldevice_newdepthstencilstate params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(20, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  struct unixcall_mtldevice_newtexture params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(21, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLBuffer_newTexture(obj_handle_t buffer, struct WMTTextureInfo *info, uint64_t offset, uint64_t bytes_per_row) {
  struct unixcall_mtlbuffer_newtexture params;
  params.buffer = buffer;
  WMT_MEMPTR_SET(params.info, info);
  params.offset = offset;
  params.bytes_per_row = bytes_per_row;
  UNIX_CALL(22, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLTexture_newTextureView(
    obj_handle_t texture, enum WMTPixelFormat format, enum WMTTextureType texture_type, uint16_t level_start,
    uint16_t level_count, uint16_t slice_start, uint16_t slice_count, struct WMTTextureSwizzleChannels swizzle,
    uint64_t *out_gpu_resource_id
) {
  struct unixcall_mtltexture_newtextureview params;
  params.texture = texture;
  params.format = format;
  params.texture_type = texture_type;
  params.level_start = level_start;
  params.level_count = level_count;
  params.slice_start = slice_start;
  params.slice_count = slice_count;
  params.swizzle = swizzle;
  UNIX_CALL(23, &params);
  *out_gpu_resource_id = params.gpu_resource_id;
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_minimumLinearTextureAlignmentForPixelFormat(obj_handle_t device, enum WMTPixelFormat format) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = device;
  params.arg = (uint64_t)format;
  params.ret = 0;
  UNIX_CALL(24, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newLibrary(
    obj_handle_t device, obj_handle_t data, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newlibrary params;
  params.device = device;
  params.data = data;
  params.ret_error = 0;
  params.ret_library = 0;
  UNIX_CALL(25, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_library;
}

WINEMETAL_API obj_handle_t
MTLLibrary_newFunction(obj_handle_t library, const char *name) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = library;
  params.arg = PtrToUInt64(name);
  params.ret = 0;
  UNIX_CALL(26, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
NSString_lengthOfBytesUsingEncoding(obj_handle_t str, enum WMTStringEncoding encoding) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = str;
  params.arg = (uint64_t)(encoding);
  params.ret = 0;
  UNIX_CALL(27, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
NSObject_description(obj_handle_t nserror) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = nserror;
  UNIX_CALL(28, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newComputePipelineState(
    obj_handle_t device, const struct WMTComputePipelineInfo *info, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newcomputepso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(29, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_blitCommandEncoder(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = cmdbuf;
  params.ret = 0;
  UNIX_CALL(30, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_computeCommandEncoder(obj_handle_t cmdbuf, bool concurrent) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = cmdbuf;
  params.arg = concurrent;
  params.ret = 0;
  UNIX_CALL(31, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_renderCommandEncoder(obj_handle_t cmdbuf, struct WMTRenderPassInfo *info) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = cmdbuf;
  params.arg = PtrToUInt64(info);
  params.ret = 0;
  UNIX_CALL(32, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandEncoder_endEncoding(obj_handle_t encoder) {
  struct unixcall_generic_obj_noret params;
  params.handle = encoder;
  UNIX_CALL(33, &params);
  return;
}

WINEMETAL_API obj_handle_t
MTLDevice_newRenderPipelineState(obj_handle_t device, const struct WMTRenderPipelineInfo *info, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newrenderpso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(34, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API obj_handle_t
MTLDevice_newMeshRenderPipelineState(
    obj_handle_t device, const struct WMTMeshRenderPipelineInfo *info, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newmeshrenderpso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(35, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API void
MTLBlitCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head) {
  struct unixcall_generic_obj_cmd_noret params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(36, &params);
}

WINEMETAL_API void
MTLComputeCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head) {
  struct unixcall_generic_obj_cmd_noret params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(37, &params);
}

WINEMETAL_API void
MTLRenderCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head) {
  struct unixcall_generic_obj_cmd_noret params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(38, &params);
}

WINEMETAL_API enum WMTPixelFormat
MTLTexture_pixelFormat(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(39, &params);
  return (enum WMTPixelFormat)params.ret;
}

WINEMETAL_API uint64_t
MTLTexture_width(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(40, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_height(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(41, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_depth(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(42, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_arrayLength(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(43, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_mipmapLevelCount(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(44, &params);
  return params.ret;
}
WINEMETAL_API void
MTLTexture_replaceRegion(
    obj_handle_t texture, struct WMTOrigin origin, struct WMTSize size, uint64_t level, uint64_t slice,
    struct WMTMemoryPointer data, uint64_t bytes_per_row, uint64_t bytes_per_image
) {
  struct unixcall_mtltexture_replaceregion params;
  params.texture = texture;
  params.origin = origin;
  params.size = size;
  params.level = level;
  params.slice = slice;
  params.data = data;
  params.bytes_per_row = bytes_per_row;
  params.bytes_per_image = bytes_per_image;
  UNIX_CALL(45, &params);
}

WINEMETAL_API void
MTLBuffer_didModifyRange(obj_handle_t buffer, uint64_t start, uint64_t length) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = buffer;
  params.arg = start;
  params.ret = length;
  UNIX_CALL(46, &params);
}

WINEMETAL_API void
MTLCommandBuffer_presentDrawable(obj_handle_t cmdbuf, obj_handle_t drawable) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = cmdbuf;
  params.arg = drawable;
  UNIX_CALL(47, &params);
};

WINEMETAL_API void
MTLCommandBuffer_presentDrawableAfterMinimumDuration(obj_handle_t cmdbuf, obj_handle_t drawable, double after) {
  struct unixcall_generic_obj_obj_double_noret params;
  params.handle = cmdbuf;
  params.arg0 = drawable;
  params.arg1 = after;
  UNIX_CALL(48, &params);
};

WINEMETAL_API bool
MTLDevice_supportsFamily(obj_handle_t device, enum WMTGPUFamily gpu_family) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = device;
  params.arg = gpu_family;
  params.ret = 0;
  UNIX_CALL(49, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supportsBCTextureCompression(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(50, &params);
  return params.ret;
};

WINEMETAL_API bool
MTLDevice_supportsTextureSampleCount(obj_handle_t device, uint8_t sample_count) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = device;
  params.arg = sample_count;
  params.ret = 0;
  UNIX_CALL(51, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_hasUnifiedMemory(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(52, &params);
  return params.ret;
};

WINEMETAL_API obj_handle_t
MTLCaptureManager_sharedCaptureManager() {
  struct unixcall_generic_obj_ret params;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(53, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLCaptureManager_startCapture(obj_handle_t mgr, struct WMTCaptureInfo *info) {
  struct unixcall_mtlcapturemanager_startcapture params;
  params.capture_manager = mgr;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(54, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCaptureManager_stopCapture(obj_handle_t mgr) {
  struct unixcall_generic_obj_noret params;
  params.handle = mgr;
  UNIX_CALL(55, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newTemporalScaler(obj_handle_t device, const struct WMTFXTemporalScalerInfo *info) {
  struct unixcall_mtldevice_newfxtemporalscaler params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(56, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSpatialScaler(obj_handle_t device, const struct WMTFXSpatialScalerInfo *info) {
  struct unixcall_mtldevice_newfxspatialscaler params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(57, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandBuffer_encodeTemporalScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t depth,
    obj_handle_t motion, obj_handle_t exposure, obj_handle_t fence, const struct WMTFXTemporalScalerProps *props
) {
  struct unixcall_mtlcommandbuffer_temporal_scale params;
  params.cmdbuf = cmdbuf;
  params.scaler = scaler;
  params.color = color;
  params.output = output;
  params.depth = depth;
  params.motion = motion;
  params.exposure = exposure;
  params.fence = fence;
  WMT_MEMPTR_SET(params.props, props);
  UNIX_CALL(58, &params);
}

WINEMETAL_API void
MTLCommandBuffer_encodeSpatialScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t fence
) {
  struct unixcall_mtlcommandbuffer_spatial_scale params;
  params.cmdbuf = cmdbuf;
  params.scaler = scaler;
  params.color = color;
  params.output = output;
  params.fence = fence;
  UNIX_CALL(59, &params);
}

WINEMETAL_API obj_handle_t
NSString_string(const char *data, enum WMTStringEncoding encoding) {
  struct unixcall_nsstring_string params;
  WMT_MEMPTR_SET(params.buffer_ptr, data);
  params.encoding = encoding;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(60, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
NSString_alloc_init(const char *data, enum WMTStringEncoding encoding) {
  struct unixcall_nsstring_string params;
  WMT_MEMPTR_SET(params.buffer_ptr, data);
  params.encoding = encoding;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(61, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
DeveloperHUDProperties_instance() {
  struct unixcall_generic_obj_ret params;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(62, &params);
  return params.ret;
}

WINEMETAL_API bool
DeveloperHUDProperties_addLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t after) {
  struct unixcall_generic_obj_obj_obj_uint64_ret params;
  params.handle = obj;
  params.arg0 = label;
  params.arg1 = after;
  params.ret = 0;
  UNIX_CALL(63, &params);
  return params.ret;
}

WINEMETAL_API void
DeveloperHUDProperties_updateLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t value) {
  struct unixcall_generic_obj_obj_obj_noret params;
  params.handle = obj;
  params.arg0 = label;
  params.arg1 = value;
  UNIX_CALL(64, &params);
}

WINEMETAL_API void
DeveloperHUDProperties_remove(obj_handle_t obj, obj_handle_t label) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = obj;
  params.arg = label;
  UNIX_CALL(65, &params);
}

WINEMETAL_API obj_handle_t
MetalDrawable_texture(obj_handle_t drawable) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = drawable;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(66, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MetalLayer_nextDrawable(obj_handle_t layer) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = layer;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(67, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supportsFXSpatialScaler(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(68, &params);
  return params.ret;
};

WINEMETAL_API bool
MTLDevice_supportsFXTemporalScaler(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(69, &params);
  return params.ret;
};

WINEMETAL_API void
MetalLayer_setProps(obj_handle_t layer, const struct WMTLayerProps *props) {
  struct unixcall_generic_obj_constptr_noret params;
  params.handle = layer;
  WMT_MEMPTR_SET(params.arg, props);
  UNIX_CALL(70, &params);
}

WINEMETAL_API void
MetalLayer_getProps(obj_handle_t layer, struct WMTLayerProps *props) {
  struct unixcall_generic_obj_ptr_noret params;
  params.handle = layer;
  WMT_MEMPTR_SET(params.arg, props);
  UNIX_CALL(71, &params);
}

WINEMETAL_API obj_handle_t
CreateMetalViewFromHWND(
    intptr_t hwnd, const struct WMTWindowPlacement *placement, obj_handle_t device, obj_handle_t *layer
) {
  struct unixcall_create_metal_view_from_hwnd params;
  params.hwnd = (uint64_t)(uintptr_t)hwnd;
  params.placement = *placement;
  params.device = device;
  params.ret_layer = NULL_OBJECT_HANDLE;
  params.ret_view = NULL_OBJECT_HANDLE;
  UNIX_CALL(72, &params);
  *layer = params.ret_layer;
  return params.ret_view;
}

WINEMETAL_API void
ReleaseMetalView(obj_handle_t view) {
  struct unixcall_generic_obj_noret params;
  params.handle = view;
  UNIX_CALL(73, &params);
}

WINEMETAL_API bool
MetalView_setPlacement(obj_handle_t view, const struct WMTWindowPlacement *placement) {
  struct unixcall_metal_view_set_placement params;
  params.view = view;
  params.placement = *placement;
  params.ret = 0;
  UNIX_CALL(165, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandEncoder_setLabel(obj_handle_t encoder, obj_handle_t label) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = encoder;
  params.arg = label;
  UNIX_CALL(86, &params);
}

WINEMETAL_API void
MTLDevice_setShouldMaximizeConcurrentCompilation(obj_handle_t device, bool value) {
  struct unixcall_generic_obj_uint64_noret params;
  params.handle = device;
  params.arg = value;
  UNIX_CALL(87, &params);
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_error(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = cmdbuf;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(89, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_logs(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = cmdbuf;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(90, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLLogContainer_enumerate(obj_handle_t logs, uint64_t start, uint64_t buffer_size, obj_handle_t *buffer) {
  struct unixcall_enumerate params;
  params.enumerable = logs;
  params.start = start;
  params.buffer_size = buffer_size;
  WMT_MEMPTR_SET(params.buffer, buffer);
  params.ret_read = 0;
  UNIX_CALL(91, &params);
  return params.ret_read;
}

WINEMETAL_API bool
CGColorSpace_checkColorSpaceSupported(enum WMTColorSpace colorspace) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = colorspace;
  params.ret = 0;
  UNIX_CALL(92, &params);
  return params.ret;
}

WINEMETAL_API bool
MetalLayer_setColorSpace(obj_handle_t layer, enum WMTColorSpace colorspace) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = layer;
  params.arg = colorspace;
  params.ret = 0;
  UNIX_CALL(93, &params);
  return params.ret;
}

WINEMETAL_API uint32_t
WMTGetPrimaryDisplayId() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(94, &params);
  return params.ret;
}

WINEMETAL_API uint32_t
WMTGetSecondaryDisplayId() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(95, &params);
  return params.ret;
}

WINEMETAL_API void
WMTGetDisplayDescription(uint32_t display_id, struct WMTDisplayDescription *desc) {
  struct unixcall_generic_obj_ptr_noret params;
  params.handle = display_id;
  WMT_MEMPTR_SET(params.arg, desc);
  UNIX_CALL(96, &params);
}

WINEMETAL_API void
MetalLayer_getEDRValue(obj_handle_t layer, struct WMTEDRValue *value) {
  struct unixcall_generic_obj_constptr_noret params;
  params.handle = layer;
  WMT_MEMPTR_SET(params.arg, value);
  UNIX_CALL(97, &params);
}

WINEMETAL_API obj_handle_t
MTLLibrary_newFunctionWithConstants(
    obj_handle_t library, const char *name, const struct WMTFunctionConstant *constants, uint32_t num_constants,
    obj_handle_t *err_out
) {
  struct unixcall_mtllibrary_newfunction_with_constants params;
  params.library = library;
  WMT_MEMPTR_SET(params.name, name);
  WMT_MEMPTR_SET(params.constants, constants);
  params.num_constants = num_constants;
  params.ret = 0;
  params.ret_error = 0;
  UNIX_CALL(98, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret;
}

WINEMETAL_API bool
WMTQueryDisplaySetting(uint32_t display_id, enum WMTColorSpace *colorspace, struct WMTHDRMetadata *metadata) {
  struct unixcall_query_display_setting params;
  params.display_id = display_id;
  WMT_MEMPTR_SET(params.hdr_metadata, metadata);
  UNIX_CALL(99, &params);
  *colorspace = params.colorspace;
  return params.ret;
}

WINEMETAL_API void
WMTUpdateDisplaySetting(uint32_t display_id, enum WMTColorSpace colorspace, const struct WMTHDRMetadata *metadata) {
  struct unixcall_update_display_setting params;
  params.display_id = display_id;
  params.colorspace = colorspace;
  WMT_MEMPTR_SET(params.hdr_metadata, metadata);
  UNIX_CALL(100, &params);
}

WINEMETAL_API void
WMTQueryDisplaySettingForLayer(
    obj_handle_t layer, uint64_t *version, enum WMTColorSpace *colorspace, struct WMTHDRMetadata *metadata,
    struct WMTEDRValue *edr_value
) {
  struct unixcall_query_display_setting_for_layer params;
  params.layer = layer;
  WMT_MEMPTR_SET(params.hdr_metadata, metadata);
  UNIX_CALL(101, &params);
  *version = params.version;
  *colorspace = params.colorspace;
  *edr_value = params.edr_value;
}

WINEMETAL_API void
MTLCommandBuffer_encodeWaitForEvent(obj_handle_t cmdbuf, obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_obj_uint64_noret params;
  params.handle = cmdbuf;
  params.arg0 = event;
  params.arg1 = value;
  UNIX_CALL(102, &params);
  return;
}

WINEMETAL_API void
MTLSharedEvent_signalValue(obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_uint64_noret params;
  params.handle = event;
  params.arg = value;
  UNIX_CALL(103, &params);
}

WINEMETAL_API void
MTLSharedEvent_setWin32EventAtValue(
    obj_handle_t event, obj_handle_t shared_event_listener, void *nt_event_handle, uint64_t at_value
) {
  struct unixcall_mtlsharedevent_setevent params;
  params.shared_event = event;
  params.shared_event_listener = shared_event_listener;
  params.event_handle = (obj_handle_t)PtrToUInt64(nt_event_handle);
  params.value = at_value;
  UNIX_CALL(104, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newFence(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(105, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newEvent(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(106, &params);
  return params.ret;
}

WINEMETAL_API void
MTLBuffer_updateContents(obj_handle_t buffer, uint64_t offset, struct WMTConstMemoryPointer data, uint64_t length) {
  struct unixcall_mtlbuffer_updatecontents params;
  params.buffer = buffer;
  params.offset = offset;
  params.data = data;
  params.length = length;
  UNIX_CALL(107, &params);
}

WINEMETAL_API obj_handle_t
SharedEventListener_create() {
  struct unixcall_generic_obj_ret params;
  UNIX_CALL(108, &params);
  return params.ret;
}

WINEMETAL_API void
SharedEventListener_start(obj_handle_t event_queue) {
  struct unixcall_generic_obj_noret params;
  params.handle = event_queue;
  UNIX_CALL(109, &params);
}

WINEMETAL_API void
SharedEventListener_destroy(obj_handle_t event_queue) {
  struct unixcall_generic_obj_noret params;
  params.handle = event_queue;
  UNIX_CALL(110, &params);
}

WINEMETAL_API void
WMTGetOSVersion(uint64_t *major, uint64_t *minor, uint64_t *patch) {
  struct unixcall_get_os_version params;
  UNIX_CALL(111, &params);
  if (major)
    *major = params.ret_major;
  if (minor)
    *minor = params.ret_minor;
  if (patch)
    *patch = params.ret_patch;
}

WINEMETAL_API obj_handle_t
MTLDevice_newBinaryArchive(obj_handle_t device, const char *url, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newbinaryarchive params;
  params.device = device;
  WMT_MEMPTR_SET(params.url, url);
  params.ret_archive = 0;
  params.ret_error = 0;
  UNIX_CALL(112, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_archive;
}

WINEMETAL_API void
MTLBinaryArchive_serialize(obj_handle_t archive, const char *url, obj_handle_t *err_out) {
  struct unixcall_mtlbinaryarchive_serialize params;
  params.archive = archive;
  WMT_MEMPTR_SET(params.url, url);
  UNIX_CALL(113, &params);
  if (err_out)
    *err_out = params.ret_error;
}

WINEMETAL_API obj_handle_t
DispatchData_alloc_init(uint64_t native_ptr, uint64_t length) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = native_ptr;
  params.arg = length;
  UNIX_CALL(114, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
CacheReader_alloc_init(const char *path, uint64_t version) {
  struct unixcall_cache_alloc_init params;
  WMT_MEMPTR_SET(params.path, path);
  params.version = version;
  UNIX_CALL(115, &params);
  return params.ret_cache;
}

WINEMETAL_API obj_handle_t
CacheReader_get(obj_handle_t reader, const void *key, uint64_t length) {
  struct unixcall_cache_get params;
  params.cache = reader;
  WMT_MEMPTR_SET(params.key, key);
  params.key_length = length;
  UNIX_CALL(116, &params);
  return params.ret_data;
}

WINEMETAL_API obj_handle_t
CacheWriter_alloc_init(const char *path, uint64_t version) {
  struct unixcall_cache_alloc_init params;
  WMT_MEMPTR_SET(params.path, path);
  params.version = version;
  UNIX_CALL(117, &params);
  return params.ret_cache;
}

WINEMETAL_API void
CacheWriter_set(
    obj_handle_t writer, const void *key, uint64_t key_length, obj_handle_t value
) {
  struct unixcall_cache_set params;
  params.cache = writer;
  WMT_MEMPTR_SET(params.key, key);
  params.key_length = key_length;
  params.value_data = value;
  UNIX_CALL(118, &params);
}

WINEMETAL_API bool
WMTSetMetalShaderCachePath(const char *path) {
  struct unixcall_setmetalcachepath params;
  WMT_MEMPTR_SET(params.path, path);
  params.ret_success = 0;
  UNIX_CALL(119, &params);
  return params.ret_success;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSharedTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  struct unixcall_mtldevice_newtexture params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(120, &params);
  return params.ret;
}

WINEMETAL_API bool
WMTBootstrapRegister(const char *name, mach_port_t mach_port) {
  struct unixcall_bootstrap params;
  strncpy(params.name, name, sizeof(params.name) - 1);
  params.name[sizeof(params.name) - 1] = '\0';
  params.mach_port = mach_port;
  NTSTATUS ret = WINE_UNIX_CALL(121, &params);
  return !ret;
}

WINEMETAL_API bool
WMTBootstrapLookUp(const char *name, mach_port_t *mach_port) {
  struct unixcall_bootstrap params;
  strncpy(params.name, name, sizeof(params.name) - 1);
  params.name[sizeof(params.name) - 1] = '\0';
  params.mach_port = 0;
  NTSTATUS ret = WINE_UNIX_CALL(122, &params);
  *mach_port = params.mach_port;
  return !ret;
}

WINEMETAL_API mach_port_t
MTLSharedEvent_createMachPort(obj_handle_t event) {
  struct unixcall_mtlsharedevent_createmachport params;
  params.event = event;
  params.ret_mach_port = 0;
  UNIX_CALL(123, &params);
  return params.ret_mach_port;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSharedEventWithMachPort(obj_handle_t device, mach_port_t mach_port) {
  struct unixcall_mtldevice_newsharedeventwithmachport params;
  params.device = device;
  params.mach_port = mach_port;
  params.ret_event = 0;
  UNIX_CALL(124, &params);
  return params.ret_event;
}

WINEMETAL_API uint64_t
MTLDevice_registryID(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(125, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLComputePipelineState_threadExecutionWidth(obj_handle_t pipeline) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = pipeline;
  params.ret = 0;
  UNIX_CALL(147, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLComputePipelineState_maxTotalThreadsPerThreadgroup(obj_handle_t pipeline) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = pipeline;
  params.ret = 0;
  UNIX_CALL(WMTUnixComputeMaxThreadsPerThreadgroup, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supports32BitFloatFiltering(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(WMTUnixDeviceSupports32BitFloatFiltering, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supports32BitMSAA(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(WMTUnixDeviceSupports32BitMSAA, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supportsShaderBarycentricCoordinates(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(WMTUnixDeviceSupportsShaderBarycentrics, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_maxArgumentBufferSamplerCount(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(WMTUnixDeviceMaxArgumentBufferSamplerCount, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLRenderPipelineState_maxTotalThreadgroupsPerMeshGrid(obj_handle_t pipeline) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = pipeline;
  params.ret = 0;
  UNIX_CALL(166, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supportsPlacementSparse(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(149, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_sparseTileSizeInBytes(obj_handle_t device, enum WMTSparsePageSize page_size) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = page_size;
  UNIX_CALL(150, &params);
  return params.ret;
}

WINEMETAL_API struct WMTSize
MTLDevice_sparseTileSize(
    obj_handle_t device, enum WMTTextureType type, enum WMTPixelFormat format, uint32_t sample_count,
    enum WMTSparsePageSize page_size
) {
  struct unixcall_sparse_tile_size params = {device, type, format, sample_count, page_size};
  UNIX_CALL(151, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newPlacementSparseBuffer(
    obj_handle_t device, uint64_t length, enum WMTResourceOptions options, enum WMTSparsePageSize page_size,
    uint64_t *gpu_address
) {
  struct unixcall_new_placement_sparse_buffer params = {device, length, options, page_size};
  UNIX_CALL(152, &params);
  *gpu_address = params.gpu_address;
  return params.ret;
}

WINEMETAL_API void
MTLTexture_sparseTail(obj_handle_t texture, uint64_t *first_mip, uint64_t *bytes) {
  struct unixcall_sparse_tail params = {texture};
  UNIX_CALL(153, &params);
  *first_mip = params.first_mip;
  *bytes = params.bytes;
}

WINEMETAL_API obj_handle_t
MTLDevice_newMTL4CommandQueue(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(154, &params);
  return params.ret;
}

WINEMETAL_API void
MTL4CommandQueue_updateMappings(
    obj_handle_t queue, obj_handle_t resource, bool texture, obj_handle_t heap, const void *operations, uint64_t count
) {
  struct unixcall_mtl4_mappings params = {queue, resource, heap, texture};
  params.operations.ptr = (void *)operations;
  params.count = count;
  UNIX_CALL(155, &params);
}

WINEMETAL_API void
MTL4CommandQueue_copyMappings(
    obj_handle_t queue, obj_handle_t source, obj_handle_t destination, bool texture, const void *operations,
    uint64_t count
) {
  struct unixcall_mtl4_mappings params = {queue, source, destination, texture};
  params.operations.ptr = (void *)operations;
  params.count = count;
  UNIX_CALL(156, &params);
}

WINEMETAL_API void
MTL4CommandQueue_event(obj_handle_t queue, obj_handle_t event, uint64_t value, bool wait) {
  struct unixcall_mtl4_event params = {queue, event, value, wait};
  UNIX_CALL(157, &params);
}

WINEMETAL_API uint64_t
MTLDevice_gpuCoreCount(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(148, &params);
  return params.ret;
}

WINEMETAL_API void
MTLDevice_sampleTimestamps(obj_handle_t device, uint64_t *cpu_timestamp, uint64_t *gpu_timestamp) {
  struct unixcall_mtldevice_sampletimestamps params;
  params.device = device;
  params.cpu_timestamp = 0;
  params.gpu_timestamp = 0;
  UNIX_CALL(146, &params);
  *cpu_timestamp = params.cpu_timestamp;
  *gpu_timestamp = params.gpu_timestamp;
}

WINEMETAL_API bool
MTLSharedEvent_waitUntilSignaledValue(obj_handle_t event, uint64_t value, uint64_t timeout) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue params;
  params.event = event;
  params.value = value;
  params.timeout_ms = timeout;
  params.ret_timeout = 0;
  UNIX_CALL(126, &params);
  return params.ret_timeout;
}

WINEMETAL_API obj_handle_t
MTLCounterSampleBuffer_newTimestampBuffer(obj_handle_t device, uint32_t sample_count, bool shared) {
  struct unixcall_mtlcountersamplebuffer_newtimestampbuffer params;
  params.device = device;
  params.sample_count = sample_count;
  params.shared = shared;
  params.ret = 0;
  UNIX_CALL(127, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCounterSampleBuffer_resolveCounterRange(
    obj_handle_t sample_buffer, uint32_t start, uint32_t len, void *data_out, uint64_t data_length
) {
  struct unixcall_mtlcountersamplebuffer_resolvecounterrange params;
  params.sample_buffer = sample_buffer;
  params.start = start;
  params.len = len;
  params.data_length = data_length;
  WMT_MEMPTR_SET(params.data_out, data_out);
  UNIX_CALL(128, &params);
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(
    obj_handle_t cmdbuf, struct WMTSampleBufferAttachmentInfo *sample_buffer_attachments,
    uint64_t num_sample_buffer_attachments) {
  struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers params;
  params.cmdbuf = cmdbuf;
  WMT_MEMPTR_SET(params.attachments, sample_buffer_attachments);
  params.num_attachments = num_sample_buffer_attachments;
  params.ret = 0;
  UNIX_CALL(129, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLCommandBuffer_property(obj_handle_t cmdbuf, enum WMTCommandBufferProperty prop) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = cmdbuf;
  params.arg = prop;
  UNIX_CALL(130, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newTileRenderPipelineState(
    obj_handle_t device, const struct WMTTileRenderPipelineInfo *info, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newrenderpso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(131, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API obj_handle_t
MTLDevice_newResidencySet(obj_handle_t device, uint64_t init_capacity, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newresidencyset params;
  params.device = device;
  params.init_capacity = init_capacity;
  params.ret_error = 0;
  params.ret_set = 0;
  UNIX_CALL(132, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_set;
}

WINEMETAL_API void
MTLResidencySet_addAllocations(obj_handle_t residency_set, const obj_handle_t *allocations, uint64_t count) {
  struct unixcall_mtlresidencyset_addallocations params;
  params.set = residency_set;
  WMT_MEMPTR_SET(params.allocations, allocations);
  params.count = count;
  UNIX_CALL(133, &params);
}

WINEMETAL_API void
MTLResidencySet_removeAllocations(obj_handle_t residency_set, const obj_handle_t *allocations, uint64_t count) {
  struct unixcall_mtlresidencyset_addallocations params;
  params.set = residency_set;
  WMT_MEMPTR_SET(params.allocations, allocations);
  params.count = count;
  UNIX_CALL(134, &params);
}

WINEMETAL_API void
MTLResidencySet_removeAllAllocations(obj_handle_t residency_set) {
  struct unixcall_generic_obj_noret params;
  params.handle = residency_set;
  UNIX_CALL(135, &params);
}

WINEMETAL_API void
MTLResidencySet_commit(obj_handle_t residency_set) {
  struct unixcall_generic_obj_noret params;
  params.handle = residency_set;
  UNIX_CALL(136, &params);
}

WINEMETAL_API void
MTLCommandQueue_addResidencySet(obj_handle_t queue, obj_handle_t residency_set) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = queue;
  params.arg = residency_set;
  UNIX_CALL(137, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newHeap(obj_handle_t device, const struct WMTHeapInfo *info) {
  struct unixcall_mtldevice_newheap params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(138, &params);
  return params.ret;
}

WINEMETAL_API struct WMTSizeAndAlign
MTLDevice_heapBufferSizeAndAlign(obj_handle_t device, uint64_t length, enum WMTResourceOptions options) {
  struct unixcall_mtldevice_heapbuffersizeandalign params;
  params.device = device;
  params.length = length;
  params.options = options;
  UNIX_CALL(139, &params);
  struct WMTSizeAndAlign ret;
  ret.size = params.ret_size;
  ret.align = params.ret_align;
  return ret;
}

WINEMETAL_API struct WMTSizeAndAlign
MTLDevice_heapTextureSizeAndAlign(obj_handle_t device, struct WMTTextureInfo *info) {
  struct unixcall_mtldevice_heaptexturesizeandalign params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(140, &params);
  struct WMTSizeAndAlign ret;
  ret.size = params.ret_size;
  ret.align = params.ret_align;
  return ret;
}

WINEMETAL_API obj_handle_t
MTLHeap_newBuffer(obj_handle_t heap, struct WMTBufferInfo *info, uint64_t offset) {
  struct unixcall_mtlheap_newbuffer params;
  params.heap = heap;
  WMT_MEMPTR_SET(params.info, info);
  params.offset = offset;
  UNIX_CALL(141, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLHeap_newTexture(obj_handle_t heap, struct WMTTextureInfo *info, uint64_t offset) {
  struct unixcall_mtlheap_newtexture params;
  params.heap = heap;
  WMT_MEMPTR_SET(params.info, info);
  params.offset = offset;
  UNIX_CALL(142, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newIndirectCommandBuffer(
    obj_handle_t device, struct WMTIndirectCommandBufferInfo *info, uint64_t max_count, enum WMTResourceOptions options
) {
  struct unixcall_mtldevice_newicb params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.max_count = max_count;
  params.options = options;
  UNIX_CALL(143, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newLibraryWithSource(obj_handle_t device, const char *source, uint64_t length, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newlibrarywithsource params;
  params.device = device;
  WMT_MEMPTR_SET(params.source, source);
  params.length = length;
  params.ret_error = 0;
  params.ret_library = 0;
  UNIX_CALL(144, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_library;
}

WINEMETAL_API void
MTLTexture_getBytes(
    obj_handle_t texture, struct WMTOrigin origin, struct WMTSize size, uint64_t level, uint64_t slice,
    struct WMTMemoryPointer data, uint64_t bytes_per_row, uint64_t bytes_per_image
) {
  // use compatible struct
  struct unixcall_mtltexture_replaceregion params;
  params.texture = texture;
  params.origin = origin;
  params.size = size;
  params.level = level;
  params.slice = slice;
  params.data = data;
  params.bytes_per_row = bytes_per_row;
  params.bytes_per_image = bytes_per_image;
  UNIX_CALL(145, &params);
}

WINEMETAL_API void
MTLDevice_accelerationStructureSizes(obj_handle_t device, struct WMTAccelerationStructureInfo *info) {
  struct unixcall_mtldevice_accelerationstructuresizes params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(158, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newAccelerationStructure(obj_handle_t device, uint64_t size, uint64_t *gpu_resource_id) {
  struct unixcall_mtldevice_newaccelerationstructure params;
  params.device = device;
  params.size = size;
  params.ret_gpu_resource_id = 0;
  params.ret = 0;
  UNIX_CALL(159, &params);
  *gpu_resource_id = params.ret_gpu_resource_id;
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_accelerationStructureCommandEncoder(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = cmdbuf;
  params.ret = 0;
  UNIX_CALL(160, &params);
  return params.ret;
}

WINEMETAL_API void
MTLAccelerationStructureCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head) {
  struct unixcall_generic_obj_cmd_noret params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(161, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newLinkedComputePipelineState(
    obj_handle_t device, const struct WMTLinkedComputePipelineInfo *info, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newlinkedcomputepso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(162, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API obj_handle_t
MTLComputePipelineState_newVisibleFunctionTable(obj_handle_t pso, const obj_handle_t *functions, uint64_t function_count) {
  struct unixcall_mtlcomputepso_newvisiblefunctiontable params;
  params.pso = pso;
  WMT_MEMPTR_SET(params.functions, functions);
  params.function_count = function_count;
  params.ret = 0;
  UNIX_CALL(163, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_sampledCommandEncoder(obj_handle_t cmdbuf, enum WMTSampledEncoder type,
                                       const struct WMTSampleBufferAttachmentInfo *samples,
                                       const struct WMTRenderPassInfo *render_info) {
  struct unixcall_mtlcommandbuffer_sampledcommandencoder params;
  params.cmdbuf = cmdbuf;
  WMT_MEMPTR_SET(params.samples, samples);
  WMT_MEMPTR_SET(params.render_info, render_info);
  params.type = type;
  params.ret = 0;
  UNIX_CALL(WMTUnixSampledEncoder, &params);
  return params.ret;
}

WINEMETAL_API uint32_t
WMTDiagWrite(const char *directory, const char *lines, uint64_t length) {
  struct unixcall_wmtdiag_write params;
  WMT_MEMPTR_SET(params.directory, directory);
  WMT_MEMPTR_SET(params.lines, lines);
  params.length = length;
  params.ret = 0;
  UNIX_CALL(WMTUnixDiagWrite, &params);
  return params.ret;
}

WINEMETAL_API bool
WMTTraceWrite(const char *directory, const char *lines, uint64_t length, bool encoder_errors, bool memory_samples) {
  struct unixcall_wmttrace_write params;
  WMT_MEMPTR_SET(params.directory, directory);
  WMT_MEMPTR_SET(params.lines, lines);
  params.length = length;
  params.encoder_errors = encoder_errors;
  params.memory_samples = memory_samples;
  params.ret = 0;
  UNIX_CALL(WMTUnixTraceWrite, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
WMTGPUStatistics(obj_handle_t device, char *buffer, uint64_t size) {
  struct unixcall_wmtgpu_statistics params;
  params.device = device;
  WMT_MEMPTR_SET(params.buffer, buffer);
  params.size = size;
  params.ret = 0;
  UNIX_CALL(WMTUnixGPUStatistics, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandBuffer_pushDebugGroup(obj_handle_t cmdbuf, obj_handle_t name) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = cmdbuf;
  params.arg = name;
  UNIX_CALL(WMTUnixCommandBufferPushDebugGroup, &params);
}

WINEMETAL_API void
MTLCommandBuffer_popDebugGroup(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_noret params;
  params.handle = cmdbuf;
  UNIX_CALL(WMTUnixCommandBufferPopDebugGroup, &params);
}

WINEMETAL_API uint64_t
DispatchData_getBytes(obj_handle_t data, void *buffer, uint64_t size) {
  struct unixcall_dispatchdata_bytes params;
  params.data = data;
  WMT_MEMPTR_SET(params.buffer, buffer);
  params.size = size;
  params.ret = 0;
  UNIX_CALL(WMTUnixDispatchDataBytes, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
WMTPresentStatistics_new(void) {
  struct unixcall_generic_obj_ret params;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(WMTUnixPresentStatisticsNew, &params);
  return params.ret;
}

WINEMETAL_API void
MetalDrawable_countPresent(obj_handle_t drawable, obj_handle_t statistics, uint64_t number) {
  struct unixcall_generic_obj_obj_uint64_noret params;
  params.handle = drawable;
  params.arg0 = statistics;
  params.arg1 = number;
  UNIX_CALL(WMTUnixDrawableCountPresent, &params);
}

WINEMETAL_API void
WMTPresentStatistics_get(obj_handle_t statistics, struct WMTPresentStatistics *out) {
  struct unixcall_generic_obj_ptr_noret params;
  params.handle = statistics;
  WMT_MEMPTR_SET(params.arg, out);
  UNIX_CALL(WMTUnixPresentStatisticsGet, &params);
}

WINEMETAL_API obj_handle_t
MTL4CommandBuffer_new(
    obj_handle_t queue, obj_handle_t fence, obj_handle_t event, uint64_t value, obj_handle_t residency,
    struct WMTMetal4BindingCounts bindings
) {
  struct unixcall_metal4_new p = {
      .queue = queue, .fence = fence, .event = event, .residency = residency, .value = value, .bindings = bindings
  };
  UNIX_CALL(WMTUnixMetal4NewBuffer, &p);
  return p.ret;
}
WINEMETAL_API obj_handle_t
MTL4CommandBuffer_encoder(
    obj_handle_t buffer, enum WMTMetal4Stream kind, const struct WMTRenderPassInfo *render,
    struct WMTSampleBufferAttachmentInfo *samples
) {
  struct unixcall_metal4_encoder p = {.buffer = buffer, .kind = kind};
  WMT_MEMPTR_SET(p.render, render);
  WMT_MEMPTR_SET(p.samples, samples);
  UNIX_CALL(WMTUnixMetal4Encoder, &p);
  return p.ret;
}
WINEMETAL_API void
MTL4CommandEncoder_encodeCommands(obj_handle_t encoder, enum WMTMetal4Stream kind, const struct wmtcmd_base *head) {
  struct unixcall_metal4_commands p = {.encoder = encoder, .kind = kind};
  WMT_MEMPTR_SET(p.head, head);
  UNIX_CALL(WMTUnixMetal4Commands, &p);
}
WINEMETAL_API void
MTL4CommandEncoder_endEncoding(obj_handle_t encoder, bool segment) {
  struct unixcall_generic_obj_uint64_noret p = {encoder, segment};
  UNIX_CALL(WMTUnixMetal4EndEncoder, &p);
}
WINEMETAL_API void
MTL4CommandBuffer_commit(obj_handle_t buffer) {
  UNIX_CALL(WMTUnixMetal4Commit, &buffer);
}
WINEMETAL_API struct WMTMetal4Feedback
MTL4CommandBuffer_feedback(obj_handle_t buffer) {
  struct unixcall_metal4_feedback p = {.buffer = buffer};
  UNIX_CALL(WMTUnixMetal4Feedback, &p);
  return p.ret;
}
WINEMETAL_API void
MTL4CommandBuffer_waitUntilCompleted(obj_handle_t buffer) {
  UNIX_CALL(WMTUnixMetal4Wait, &buffer);
}
WINEMETAL_API obj_handle_t
MTL4CounterHeap_new(obj_handle_t device, uint64_t count) {
  struct unixcall_generic_obj_uint64_obj_ret p = {.handle = device, .arg = count};
  UNIX_CALL(WMTUnixMetal4CounterHeap, &p);
  return p.ret;
}
WINEMETAL_API uint64_t
MTLDevice_queryTimestampFrequency(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret p = {.handle = device};
  UNIX_CALL(WMTUnixMetal4Frequency, &p);
  return p.ret;
}
WINEMETAL_API void
MTL4CommandBuffer_present(obj_handle_t buffer, obj_handle_t drawable, double after) {
  struct unixcall_generic_obj_obj_double_noret p = {buffer, drawable, after};
  UNIX_CALL(WMTUnixMetal4Present, &p);
}

WINEMETAL_API void
MTL4CommandBuffer_waitForEvent(obj_handle_t buffer, obj_handle_t event, uint64_t value) {
  struct unixcall_mtl4_event p = {.queue = buffer, .event = event, .value = value};
  UNIX_CALL(WMTUnixMetal4WaitEvent, &p);
}

WINEMETAL_API void
MTL4CommandBuffer_orderAliases(obj_handle_t buffer, bool inside) {
  struct unixcall_generic_obj_uint64_noret p = {.handle = buffer, .arg = inside};
  UNIX_CALL(WMTUnixMetal4OrderAliases, &p);
}
