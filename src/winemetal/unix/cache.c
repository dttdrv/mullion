#import <Foundation/Foundation.h>
#include <os/lock.h>
#include "sqlite3.h"
#define WINEMETAL_API
#include "../winemetal_thunks.h"

@interface CacheReader : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (dispatch_data_t)get:(NSData *)key;
@end

@interface CacheWriter : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (void)set:(NSData *)key value:(dispatch_data_t)value;
@end

// a connection to the store with the lookup prepared on it. connections are opened without SQLite's own mutexes, so
// each is used by one thread at a time
struct reader_connection {
  sqlite3 *db;
  sqlite3_stmt *stmt;
};

// how many connections a reader keeps open between lookups: those of more threads than that looking up at once are
// opened and closed around their lookups
enum { kReaderConnections = 8 };

@interface CacheReader () {
  NSString *_dbPath;
  NSString *_sqlGet;
  // the connections no lookup is using. a lookup takes one, or opens another when all are taken, so the lookups of
  // different threads do not wait for each other
  os_unfair_lock _lock;
  struct reader_connection _idle[kReaderConnections];
  unsigned _idle_count;
}
@end

static bool
reader_open(NSString *path, NSString *sql, struct reader_connection *connection) {
  connection->db = NULL;
  connection->stmt = NULL;
  if (sqlite3_open_v2([path fileSystemRepresentation], &connection->db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
      SQLITE_OK) {
    NSLog(@"[CacheReader] Failed to open DB: %s", sqlite3_errmsg(connection->db));
    sqlite3_close(connection->db);
    return false;
  }
  if (sqlite3_prepare_v2(connection->db, sql.UTF8String, -1, &connection->stmt, NULL) != SQLITE_OK) {
    NSLog(@"[CacheReader] Failed to prepare SELECT: %s", sqlite3_errmsg(connection->db));
    sqlite3_close(connection->db);
    return false;
  }
  return true;
}

static void
reader_close(struct reader_connection connection) {
  sqlite3_finalize(connection.stmt);
  sqlite3_close(connection.db);
}

NSString *
resolve_cache_dir(NSString *path, bool path_is_file) {
  if (![path hasPrefix:@"/"]) {
    char buf[PATH_MAX];
    size_t len = confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf));

    if (!len) {
      return nil;
    }

    NSString *base = [NSString stringWithUTF8String:buf];
    path = [base stringByAppendingPathComponent:path];
  }
  [[NSFileManager defaultManager] createDirectoryAtPath:path_is_file ? [path stringByDeletingLastPathComponent] : path
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return path;
}

// what one converter version stored is a file of its own beside the name asked for: "shaders.db" at version 7 is
// "shaders.v7.db"
static NSString *
versioned(NSString *path, uint64_t version) {
  return [[[path stringByDeletingPathExtension] stringByAppendingFormat:@".v%llu", version]
      stringByAppendingPathExtension:path.pathExtension];
}

// no version reads what another stored, so a store that kept each grew by a whole version's conversions at every new
// one: the files beside `stem` whose names start as its does and not as `kept`'s go. a process that still has one
// open keeps it until it ends
static void
remove_other_versions(NSString *stem, NSString *kept) {
  NSString *directory = [stem stringByDeletingLastPathComponent];
  for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:nil])
    if ([name hasPrefix:[stem lastPathComponent]] && ![name hasPrefix:[kept lastPathComponent]])
      unlink([[directory stringByAppendingPathComponent:name] fileSystemRepresentation]);
}

@implementation CacheReader

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    _lock = OS_UNFAIR_LOCK_INIT;
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      [self release];
      return nil;
    }
    _dbPath = [versioned(dbPath, version) copy];
    _sqlGet = [[NSString alloc] initWithFormat:@"SELECT value FROM cache_%llu WHERE key = ?;", version];
    // the first connection tells whether the store can be read at all
    if (!reader_open(_dbPath, _sqlGet, &_idle[0])) {
      [self release];
      return nil;
    }
    _idle_count = 1;
  }
  return self;
}

- (dispatch_data_t)get:(NSData *)key {
  struct reader_connection connection;
  os_unfair_lock_lock(&_lock);
  bool taken = _idle_count > 0;
  if (taken)
    connection = _idle[--_idle_count];
  os_unfair_lock_unlock(&_lock);
  if (!taken && !reader_open(_dbPath, _sqlGet, &connection))
    return nil;

  sqlite3_bind_blob64(connection.stmt, 1, key.bytes, key.length, SQLITE_STATIC);
  dispatch_data_t result = nil;
  if (sqlite3_step(connection.stmt) == SQLITE_ROW) {
    // the value is SQLite's until the statement is reset, and the caller keeps it for as long as the function made
    // of it lives: it is copied out, and the connection given back
    const void *bytes = sqlite3_column_blob(connection.stmt, 0);
    int len = sqlite3_column_bytes(connection.stmt, 0);
    result = dispatch_data_create(bytes, len, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  }
  // the reset ends the read, so the next lookup on the connection sees what was written since
  sqlite3_reset(connection.stmt);
  sqlite3_clear_bindings(connection.stmt);

  os_unfair_lock_lock(&_lock);
  bool kept = _idle_count < kReaderConnections;
  if (kept)
    _idle[_idle_count++] = connection;
  os_unfair_lock_unlock(&_lock);
  if (!kept)
    reader_close(connection);
  return result;
}

- (void)dealloc {
  for (unsigned i = 0; i < _idle_count; i++)
    reader_close(_idle[i]);
  [_dbPath release];
  [_sqlGet release];
  [super dealloc];
}

@end

@interface CacheWriter () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt, *_delete_stmt;
  int _lock_fd;
}
- (void)write:(NSData *)key value:(dispatch_data_t)value;
@end

static dispatch_queue_t write_queue;

// a process that ends in order writes what it still has queued; one that is killed loses that, and converts it again
static void
finish_writes(void) {
  dispatch_sync(write_queue, ^{
  });
}

// the queue every writer's writes go through, one after another in the order they were asked for, so that the thread
// that converted a shader does not wait for the store
static dispatch_queue_t
writes(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    write_queue = dispatch_queue_create(
        "mullion.shader-cache.writes", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)
    );
    atexit(finish_writes);
  });
  return write_queue;
}

@implementation CacheWriter

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    _lock_fd = -1;
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      [self release];
      return nil;
    }

    // those of the name itself among them: a store from before a version had its own
    remove_other_versions([[dbPath stringByDeletingPathExtension] stringByAppendingString:@"."],
                          versioned(dbPath, version));
    dbPath = versioned(dbPath, version);
    NSString *lockPath = [dbPath stringByAppendingString:@"-lock"];
    _lock_fd = open([lockPath fileSystemRepresentation], O_RDWR | O_CREAT, 0666);
    if (_lock_fd < 0) {
      NSLog(@"[CacheWriter] Failed to open file for locking %@", lockPath);
      [self release];
      return nil;
    }
    while (flock(_lock_fd, LOCK_EX)) {
      if (errno == EINTR)
        continue;
      NSLog(@"[CacheWriter] Failed to lock DB: %s", strerror(errno));
      [self release];
      return nil;
    }

    if (sqlite3_open_v2(
            [dbPath fileSystemRepresentation], &_db, //
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL
        ) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to open DB: %s", sqlite3_errmsg(_db));
      [self release];
      return nil;
    }

    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    NSString *sqlCreate = [NSString stringWithFormat:@"CREATE TABLE IF NOT EXISTS %@ ("
                                                      "key BLOB PRIMARY KEY, "
                                                      "value BLOB NOT NULL);",
                                                     tableName];

    char *errMsg = NULL;
    if (sqlite3_exec(_db, sqlCreate.UTF8String, NULL, NULL, &errMsg) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to create table: %s", errMsg);
      sqlite3_free(errMsg);
    }

    flock(_lock_fd, LOCK_UN);

    NSString *sqlSet = [NSString stringWithFormat:@"INSERT OR REPLACE INTO %@ (key, value) VALUES (?, ?);", tableName];
    NSString *sqlDelete = [NSString stringWithFormat:@"DELETE FROM %@ WHERE key = ?;", tableName];
    if (sqlite3_prepare_v2(_db, sqlSet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(_db, sqlDelete.UTF8String, -1, &_delete_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to prepare write: %s", sqlite3_errmsg(_db));
      [self release];
      return nil;
    }
  }
  return self;
}

- (void)set:(NSData *)key value:(dispatch_data_t)value {
  [key retain];
  if (value)
    dispatch_retain(value);
  dispatch_async(writes(), ^{
    [self write:key value:value];
    [key release];
    if (value)
      dispatch_release(value);
  });
}

- (void)write:(NSData *)key value:(dispatch_data_t)value {
  while (flock(_lock_fd, LOCK_EX)) {
    if (errno == EINTR)
      continue;
    NSLog(@"[CacheWriter] Failed to lock DB: %s", strerror(errno));
    return;
  }
  sqlite3_stmt *stmt = value ? _stmt : _delete_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_blob64(stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  const void *bytes = NULL;
  size_t length = 0;
  dispatch_data_t flat = value ? dispatch_data_create_map(value, &bytes, &length) : NULL;
  if (value)
    sqlite3_bind_blob64(stmt, 2, bytes, length, SQLITE_STATIC);

  if (sqlite3_step(stmt) != SQLITE_DONE)
    NSLog(@"[CacheWriter] Failed to write: %s", sqlite3_errmsg(_db));

  if (flat)
    dispatch_release(flat);
  sqlite3_reset(stmt);
  flock(_lock_fd, LOCK_UN);
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_delete_stmt)
    sqlite3_finalize(_delete_stmt);
  if (_db)
    sqlite3_close(_db);
  if (_lock_fd >= 0)
    close(_lock_fd);
  [super dealloc];
}

@end

int
_CacheReader_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheReader alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheReader_get(void *obj) {
  struct unixcall_cache_get *params = obj;
  NSMutableData *key = [[NSMutableData alloc] initWithBytes:&params->key_length length:sizeof(params->key_length)];
  [key appendBytes:params->key.ptr length:params->key_length];
  const char *skip = (getenv("MULLION_AIRCONV_SKIP") ?: getenv("DXMT_AIRCONV_SKIP"));
  if (skip)
    [key appendBytes:skip length:strlen(skip)];
  const char *hoist = getenv("MULLION_AIRCONV_HOIST_LOADS");
  if (hoist)
    [key appendBytes:hoist length:strlen(hoist)];
  CacheReader *reader = (CacheReader *)params->cache;
  params->ret_data = (obj_handle_t)[reader get:key];
  [key release];
  return 0;
}

int
_CacheWriter_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheWriter alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheWriter_set(void *obj) {
  struct unixcall_cache_set *params = obj;
  // airconv reads the Unix environment, which Windows environment changes do not update
  NSMutableData *key = [[NSMutableData alloc] initWithBytes:&params->key_length length:sizeof(params->key_length)];
  [key appendBytes:params->key.ptr length:params->key_length];
  const char *skip = (getenv("MULLION_AIRCONV_SKIP") ?: getenv("DXMT_AIRCONV_SKIP"));
  if (skip)
    [key appendBytes:skip length:strlen(skip)];
  const char *hoist = getenv("MULLION_AIRCONV_HOIST_LOADS");
  if (hoist)
    [key appendBytes:hoist length:strlen(hoist)];
  CacheWriter *writer = (CacheWriter *)params->cache;
  [writer set:key value:(dispatch_data_t)params->value_data];
  [key release];
  return 0;
}

int
_WMTRemoveOtherVersions(void *obj) {
  struct unixcall_remove_other_versions *params = obj;
  // the calling thread may have no pool of its own
  @autoreleasepool {
    NSString *stem = resolve_cache_dir([NSString stringWithUTF8String:params->stem.ptr], true);
    if (stem)
      remove_other_versions(stem, [NSString stringWithUTF8String:params->kept.ptr]);
  }
  return 0;
}

#ifndef MULLION_NO_PRIVATE_API

extern void MTLSetShaderCachePath(NSString* path);
extern NSString* MTLGetShaderCachePath();

// Metal keeps a working folder ("gpuarchiver-" and six digits) in its cache folder for every binary archive that is
// added to, and leaves it there when the archive and its process are gone (macOS 27.3, 1.6 MB each). every process
// holds the cache folder's lock shared for as long as it lives, so one that takes it alone finds only folders of
// processes that ended: those are removed, off this thread. the lock is this process's alone: one it starts (Wine's
// explorer) does not keep it
static void
remove_archive_folders(NSString *cache) {
  int lock = open([cache stringByAppendingPathComponent:@"mullion.lock"].fileSystemRepresentation,
                  O_RDWR | O_CREAT | O_CLOEXEC, 0666);
  if (lock < 0)
    return;
  NSMutableArray<NSString *> *ended = [NSMutableArray array];
  if (!flock(lock, LOCK_EX | LOCK_NB))
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:cache error:nil])
      if ([name hasPrefix:@"gpuarchiver-"])
        [ended addObject:[cache stringByAppendingPathComponent:name]];
  while (flock(lock, LOCK_SH) && errno == EINTR)
    ;
  if (ended.count)
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      for (NSString *folder in ended)
        [[NSFileManager defaultManager] removeItemAtPath:folder error:nil];
    });
}

int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  NSString *resolved_path = resolve_cache_dir(path, false);
  MTLSetShaderCachePath(resolved_path);
  params->ret_success = [MTLGetShaderCachePath() isEqualToString:resolved_path];
  if (params->ret_success)
    remove_archive_folders(resolved_path);
  [path release];
  return 0;
};

#else

int
WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  params->ret_success = 0;
  return 0;
}

#endif
