// contract: the bridge's shader store (src/winemetal/unix/cache.c), which converted shaders are kept in between
// runs. a lookup returns the bytes last stored under its key, or nothing, from any number of threads at once; writes
// are queued and take effect in the order they were asked for, a removal among them; and a process that ends in
// order has stored everything it queued, so the next process finds it. what one version of the converter stored is
// a file of its own, and a process that opens the store at one version removes the files of the others, with those
// of a store from before versions had their own, and nothing else. the pipeline archive's files (d3d12_device.cpp,
// WMTRemoveOtherVersions) go the same way: those of other converter versions, and those named before versions, go,
// and this version's, another Metal version's and the store's stay.
// the keys and sizes differ per entry and every byte of a value follows from its key, so a lookup that returns
// another entry's bytes, or part of its own, is told from a right one.
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/wait.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"

int _CacheReader_alloc_init(void *obj);
int _CacheReader_get(void *obj);
int _CacheWriter_alloc_init(void *obj);
int _CacheWriter_set(void *obj);
int _WMTRemoveOtherVersions(void *obj);
extern char **environ;

enum { kThreads = 12, kEach = 200, kQueued = 1000 };
static obj_handle_t reader, writer;
static atomic_int wrong;

static void
put(uint64_t key, dispatch_data_t value) {
  struct unixcall_cache_set params = {writer, {(void *)&key}, sizeof(key), (obj_handle_t)value};
  _CacheWriter_set(&params);
}

static dispatch_data_t
get(uint64_t key) {
  struct unixcall_cache_get params = {reader, {(void *)&key}, sizeof(key), 0};
  _CacheReader_get(&params);
  return (dispatch_data_t)params.ret_data;
}

// from a few bytes to several of the store's pages
static size_t
size_of(uint64_t key) {
  return 16 + (key * 2654435761u) % 20000;
}

static uint8_t
byte_of(uint64_t key, size_t at, uint8_t version) {
  return (uint8_t)(key * 31 + at * 7 + version);
}

static void
store(uint64_t key, uint8_t version) {
  size_t size = size_of(key);
  uint8_t *bytes = malloc(size);
  for (size_t i = 0; i < size; i++)
    bytes[i] = byte_of(key, i, version);
  dispatch_data_t value = dispatch_data_create(bytes, size, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
  put(key, value);
  dispatch_release(value);
}

static bool
holds(uint64_t key, uint8_t version) {
  dispatch_data_t value = get(key);
  if (!value)
    return false;
  const void *bytes;
  size_t size;
  dispatch_data_t flat = dispatch_data_create_map(value, &bytes, &size);
  bool right = size == size_of(key);
  for (size_t i = 0; right && i < size; i++)
    right = ((const uint8_t *)bytes)[i] == byte_of(key, i, version);
  dispatch_release(flat);
  dispatch_release(value);
  return right;
}

// writes are queued: the last one asked for is stored last
static bool
stored_after(uint64_t key, uint8_t version) {
  for (int i = 0; i < 60000; i++) {
    if (holds(key, version))
      return true;
    usleep(500);
  }
  return false;
}

static void *
storing_thread(void *thread) {
  uint64_t base = (uint64_t)thread << 32;
  for (uint64_t i = 0; i < kEach; i++) {
    store(base | i, 0);
    // beside lookups of what may not be stored yet
    dispatch_data_t value = get(base | (i / 2));
    if (value)
      dispatch_release(value);
  }
  return NULL;
}

static void *
looking_thread(void *thread) {
  uint64_t base = (uint64_t)thread << 32;
  for (uint64_t i = 0; i < kEach; i++)
    if (!holds(base | i, 0)) {
      printf("wrong: entry %llu of thread %llu is not what was stored\n", i, base >> 32);
      atomic_fetch_add(&wrong, 1);
    }
  return NULL;
}

int
main(int argc, char **argv) {
  char made[] = "/tmp/mullion_shader_store.XXXXXX";
  const char *directory = argc > 2 ? argv[2] : mkdtemp(made);
  if (!directory)
    return printf("failed: no temporary directory\n"), 1;
  NSString *path = [NSString stringWithFormat:@"%s/shaders_test.db", directory];
  bool other = argc > 1 && !strcmp(argv[1], "other");
  struct unixcall_cache_alloc_init params = {{(void *)path.UTF8String}, other ? 2 : 1, 0};
  _CacheWriter_alloc_init(&params);
  writer = params.ret_cache;
  params.ret_cache = 0;
  _CacheReader_alloc_init(&params);
  reader = params.ret_cache;
  if (!writer || !reader)
    return printf("failed: the store did not open\n"), 1;
  // the process of another version, which stores one entry and ends
  if (other) {
    store(1, 0);
    return 0;
  }
  const uint64_t queued = (uint64_t)1 << 40;
  // the process that only queues, and ends
  if (argc > 1 && !strcmp(argv[1], "queue")) {
    for (uint64_t i = 0; i < kQueued; i++)
      store(queued | i, 3);
    return 0;
  }

  pthread_t threads[kThreads];
  for (uintptr_t t = 0; t < kThreads; t++)
    pthread_create(&threads[t], NULL, storing_thread, (void *)(t + 1));
  for (int t = 0; t < kThreads; t++)
    pthread_join(threads[t], NULL);
  // set, removed, set again stays; set, removed is gone
  store(9001, 1);
  put(9001, NULL);
  store(9001, 2);
  store(9002, 1);
  put(9002, NULL);
  store(9003, 0);
  if (!stored_after(9003, 0))
    return printf("failed: the queued writes were not stored\n"), 1;
  if (!holds(9001, 2)) {
    printf("wrong: an entry set, removed and set again is not the last one set\n");
    wrong++;
  }
  if (get(9002)) {
    printf("wrong: an entry set and then removed is still stored\n");
    wrong++;
  }
  for (uintptr_t t = 0; t < kThreads; t++)
    pthread_create(&threads[t], NULL, looking_thread, (void *)(t + 1));
  for (int t = 0; t < kThreads; t++)
    pthread_join(threads[t], NULL);

  char *arguments[] = {argv[0], "queue", (char *)directory, NULL};
  pid_t child;
  int status = -1;
  if (posix_spawn(&child, argv[0], NULL, NULL, arguments, environ) || waitpid(child, &status, 0) != child || status)
    return printf("failed: the queueing process ended with %d\n", status), 1;
  unsigned missing = 0;
  for (uint64_t i = 0; i < kQueued; i++)
    missing += !holds(queued | i, 3);
  if (missing) {
    printf("wrong: %u of %d entries a process queued before it ended are not stored\n", missing, kQueued);
    wrong++;
  }
  // files as an earlier store left them, and two that are not the store's
  NSFileManager *files = [NSFileManager defaultManager];
  NSString *folder = [NSString stringWithUTF8String:directory];
  NSArray<NSString *> *left = @[@"shaders_test.db", @"shaders_test.db-wal", @"shaders_test.v0.db"];
  NSArray<NSString *> *unrelated = @[@"shaders_tests.db", @"pipelines_400_0.metallib"];
  for (NSString *name in [left arrayByAddingObjectsFromArray:unrelated])
    [files createFileAtPath:[folder stringByAppendingPathComponent:name] contents:[NSData data] attributes:nil];
  char *another[] = {argv[0], "other", (char *)directory, NULL};
  if (posix_spawn(&child, argv[0], NULL, NULL, another, environ) || waitpid(child, &status, 0) != child || status)
    return printf("failed: the process of another version ended with %d\n", status), 1;
  NSArray<NSString *> *now = [files contentsOfDirectoryAtPath:folder error:nil];
  if (![now containsObject:@"shaders_test.v2.db"]) {
    printf("wrong: the other version's store is not a file of its own\n");
    wrong++;
  }
  for (NSString *name in now)
    if ([name hasPrefix:@"shaders_test.v1."] || [left containsObject:name]) {
      printf("wrong: %s is still there after another version opened the store\n", name.UTF8String);
      wrong++;
    }
  for (NSString *name in unrelated)
    if (![now containsObject:name]) {
      printf("wrong: %s, which is not the store's, was removed\n", name.UTF8String);
      wrong++;
    }
  // the archive's files of Metal version 400 at converter version 2, beside those it removes: another version's, one
  // whose number starts as 2 does, a shard written halfway, and shards from before versions
  NSArray<NSString *> *archived = @[@"pipelines_400_v2_0.metallib", @"pipelines_400_v2_1.metallib",
                                    @"pipelines_400_v2_2.metallib.new", @"pipelines_320_v1_0.metallib"],
                      *stale = @[@"pipelines_400_v1_0.metallib", @"pipelines_400_v21_0.metallib",
                                 @"pipelines_400_v1_1.metallib.new", @"pipelines_400_0.metallib",
                                 @"pipelines_400_7.metallib"];
  for (NSString *name in [archived arrayByAddingObjectsFromArray:stale])
    [files createFileAtPath:[folder stringByAppendingPathComponent:name] contents:[NSData data] attributes:nil];
  NSArray<NSString *> *before = [files contentsOfDirectoryAtPath:folder error:nil];
  struct unixcall_remove_other_versions removal = {
      {[folder stringByAppendingPathComponent:@"pipelines_400_"].UTF8String},
      {[folder stringByAppendingPathComponent:@"pipelines_400_v2_"].UTF8String}};
  _WMTRemoveOtherVersions(&removal);
  now = [files contentsOfDirectoryAtPath:folder error:nil];
  for (NSString *name in before)
    if ([now containsObject:name] == [stale containsObject:name]) {
      printf("wrong: %s was %s\n", name.UTF8String, [stale containsObject:name] ? "kept" : "removed");
      wrong++;
    }
  [files removeItemAtPath:folder error:nil];
  printf(wrong ? "failed: %d wrong\n" : "passed\n", atomic_load(&wrong));
  return wrong != 0;
}
