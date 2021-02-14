/*---------------------------------------------------------------------------
  Copyright 2020 Daan Leijen, Microsoft Corporation.

  This is free software; you can redistribute it and/or modify it under the
  terms of the Apache License, Version 2.0. A copy of the License can be
  found in the file "license.txt" at the root of this distribution.
---------------------------------------------------------------------------*/
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // getenv
#endif
#include "kklib.h"

/*--------------------------------------------------------------------------------------------------
  Posix abstraction layer
--------------------------------------------------------------------------------------------------*/
  
#if defined(WIN32)
#define _UNICODE
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#endif

typedef int kk_file_t;

#ifdef WIN32
typedef struct _stat64  kk_stat_t;
#else
typedef struct stat_t   kk_stat_t;
#endif

static kk_file_t kk_posix_open(kk_string_t path, int mode, kk_context_t* ctx) {
  kk_file_t f = 0;
#ifdef WIN32
  kk_with_string_as_mutf16_borrow(path, wpath, ctx) {
    f = _wopen(wpath, mode);
  }
#else
  kk_with_string_as_mutf8_borrow(path, bpath, ctx) {
    f = open(bpath, mode);
  }
#endif
  kk_string_drop(path,ctx);
  return (f < 0 ? errno : f);
}

static int kk_posix_close(kk_file_t f) {
  return (close(f) < 0 ? errno : 0);
}

static int kk_posix_fstat(kk_file_t f, kk_stat_t* st) {
#ifdef WIN32
  return (_fstat64(f, st) < 0 ? errno : 0);
#else
  return (fstat(f, st) < 0 ? errno : 0);
#endif
}

static int kk_posix_fsize(kk_file_t f, size_t* fsize) {
  *fsize = 0;
  kk_stat_t st;
  int err = kk_posix_fstat(f, &st);
  if (err != 0) return err;
  *fsize = (size_t)st.st_size;
  return 0;
}

static int kk_posix_stat(kk_string_t path, kk_stat_t* st, kk_context_t* ctx) {
  int err = 0;
#if defined(WIN32)
  kk_with_string_as_mutf16_borrow(path, wpath, ctx) {
    if (_wstat64(wpath, st) < 0) err = errno;
  }
#else
  kk_with_string_as_mutf8_borrow(path, cpath, ctx) {
    if (stat(cpath, st) < 0) err = errno;
  }
#endif
  if (err < 0) err = errno;
  return err;
}

// Read at most `buflen` bytes from `inp` into `buf`. Return `0` on success (or an error code).
static int kk_posix_read_retry(const kk_file_t inp, char* buf, const size_t buflen, size_t* read_count) {
  int err = 0;
  size_t ofs = 0;
  do {
    size_t todo = buflen - ofs;
    #ifdef WIN32
    if (todo > INT32_MAX) todo = INT32_MAX;  // on windows read in chunks of at most 2GiB
    kk_ssize_t n = _read(inp, buf + ofs, (unsigned)(todo));
    #else
    if (todo > KK_SSIZE_MAX) todo = KK_SSIZE_MAX;
    kk_ssize_t n = read(inp, buf + ofs, (kk_ssize_t)(todo));
    #endif  
    if (n < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        err = errno;
        break;
      }
      // otherwise try again
    }
    else if (n == 0) { // eof
      break;
    }
    else {
      ofs += n;
    }
  } while (ofs < buflen);
  if (read_count != NULL) {
    *read_count = ofs;
  }
  return err;
}

// Write at `len` bytes to `out` from `buf`. On error, `write_count` may be less than `len`.
static int kk_posix_write_retry(const kk_file_t out, const char* buf, const size_t len, size_t* write_count) {
  int err = 0;
  size_t ofs = 0;
  do {
    size_t todo = len - ofs;
    #ifdef WIN32
    if (todo > INT32_MAX) todo = INT32_MAX;  // on windows write in chunks of at most 2GiB
    kk_ssize_t n = _write(out, buf + ofs, (unsigned)(todo));
    #else
    if (todo > KK_SSIZE_MAX) todo = KK_SSIZE_MAX;
    kk_ssize_t n = write(out, buf + ofs, (kk_ssize_t)todo);
    #endif  
    if (n < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        err = errno;
        break;
      }
      // otherwise try again
    }
    else if (n == 0) { // treat as error to ensure progress
      err = EIO;
      break;
    }
    else {
      ofs += n;
    }
  } while (ofs < len);
  if (write_count != NULL) {
    *write_count = ofs;
  }
  assert(ofs == len || err != 0);
  return err;
}


/*--------------------------------------------------------------------------------------------------
  Text files
--------------------------------------------------------------------------------------------------*/

kk_decl_export int kk_os_read_text_file(kk_string_t path, kk_string_t* result, kk_context_t* ctx)
{
  kk_file_t f = kk_posix_open(path, O_RDONLY, ctx);
  if (f < 0) return errno;

  size_t len;
  int err = kk_posix_fsize(f, &len);
  if (err != 0) {
    kk_posix_close(f);
    return err;
  }
  char* s;
  kk_string_t str = kk_string_alloc_cbuf(len, &s, ctx);

  size_t nread;
  err = kk_posix_read_retry(f, s, len, &nread);
  kk_posix_close(f);
  if (err < 0) {
    kk_string_drop(str, ctx);
    return err;
  }
  if (nread < len) {
    str = kk_string_adjust_length(str, nread, ctx);
  }

  *result = kk_string_convert_from_mutf8(str, ctx);
  return 0;
}

kk_decl_export int kk_os_write_text_file(kk_string_t path, kk_string_t content, kk_context_t* ctx)
{
  kk_file_t f = kk_posix_open(path, O_WRONLY, ctx);
  if (f < 0) {
    kk_string_drop(content, ctx);
    return errno;
  }
  int err = 0;
  size_t len;
  const char* buf = kk_string_cbuf_borrow(content, &len);
  if (len > 0) {
    size_t nwritten;
    err = kk_posix_write_retry(f, buf, len, &nwritten);
    if (err == 0 && nwritten < len) err = EIO;
  }
  kk_string_drop(content, ctx);
  kk_posix_close(f);
  return err;
}



/*--------------------------------------------------------------------------------------------------
  Directories
--------------------------------------------------------------------------------------------------*/


/*--------------------------------------------------------------------------------------------------
  mkdir
--------------------------------------------------------------------------------------------------*/

kk_decl_export int kk_os_ensure_dir(kk_string_t path, int mode, kk_context_t* ctx) 
{
  int err = 0;
  if (mode < 0) {
#if defined(S_IRWXU)
    mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
#else
    mode = 0755;
#endif
  }

  path = kk_string_copy(path, ctx); // copy so we can mutate
#if defined(WIN32)
  kk_with_string_as_mutf16_borrow(path, cpath, ctx) {
    uint16_t* p = (uint16_t*)cpath;
#else
  kk_with_string_as_mutf8_borrow(path, cpath, ctx) {
    char* p = (char*)cpath;
#endif
    do {
      char c = (char)(*p);
      if (c == 0 || c == '/' || c == '\\') {
        *p = 0;
        if (cpath[0] != 0) {
          kk_stat_t st = { 0 };
          #if defined(WIN32)
          _wstat64(cpath, &st);
          #else 
          stat(cpath, &st);
          #endif
          bool isdir = ((st.st_mode & S_IFDIR) != 0);
          if (!isdir) {
            #if defined(WIN32)
            int res = _wmkdir(cpath);
            #else 
            int res = mkdir(cpath, mode);
            #endif
            if (res != 0 && errno != EEXIST) {
              err = errno;
            }
          }
        }
        *p = c;
      }
    } while (err == 0 && *p++ != 0);
  }
  kk_string_drop(path, ctx);
  return err;
}

/*--------------------------------------------------------------------------------------------------
  Copy File
--------------------------------------------------------------------------------------------------*/

#if defined(WIN32)
#include <Windows.h>
kk_decl_export int kk_os_copy_file(kk_string_t from, kk_string_t to, bool preserve_mtime, kk_context_t* ctx) {
  KK_UNUSED(preserve_mtime);
  int err = 0;
  kk_with_string_as_mutf16_borrow(from, wfrom, ctx) {
    kk_with_string_as_mutf16_borrow(to, wto, ctx) {
      if (!CopyFileW(wfrom, wto, FALSE)) {
        DWORD werr = GetLastError();
        if (werr == ERROR_FILE_NOT_FOUND) err = ENOENT;
        else if (werr == ERROR_ACCESS_DENIED) err = EPERM;
        else if (werr == ERROR_PATH_NOT_FOUND) err = ENOTDIR;
        else err = EINVAL;
      }
    }
  }
  kk_string_drop(from, ctx);
  kk_string_drop(to, ctx);
  return err;
}
#else

#if defined(__APPLE__)
#include <copyfile.h>
#elif defined(__linux__)
#include <sys/sendfile.h>
#else
static int kk_posix_copy_file(const int inp, const int out, const size_t len, kk_context_t* ctx) {
  int err = 0;

#if defined(COPY_FR_COPY)
  // try copy_file_range first
  int err = 0;
  loff_t off_in = 0;
  loff_t off_out = 0;
  while(off_in < len) {
    size_t toread = len - off_in;
    if (toread > SSIZE_MAX) toread = SSIZE_MAX;
    ssize_t n = copy_file_range(inp, &off_in, out, &off_out, (ssize_t)toread, 0 /* flags */ );
    if (n < 0) {
      if (errno != EAGAIN && errno != EINTR) {
        err = errno;
        break;
      }
      // otherwise try again
    }
    else if (n == 0) {  // ensure progress
      break;
    }    
  }
  if (err != EINVAL || off_in > 0) return err;
  // fall through if `copy_file_range` failed immediately with `EINVAL` (might be a non-seekable device?)
  err = 0;
#endif

  size_t buflen = 1024 * 1024; // max 1MiB buffer
  if (buflen > len) buflen = len;
  char* buf = kk_malloc(buflen, ctx);
  if (buf == NULL) return ENOMEM;
  size_t todo = len;
  while (todo > 0) {
    size_t toread = (todo > buflen ? buflen : todo);
    size_t read_count = 0;
    err = kk_posix_read_retry(inp, buf, toread, &read_count);
    if (err != 0 || read_count == 0) break;
    size_t write_count = 0;
    err = kk_posix_write_retry(out, buf, read_count, &write_count);
    if (err != 0) break; kk_assert(write_count == read_count);
    todo -= read_count;
  }
  kk_free(buf);
  return err;
}
#endif  // not __APPLE__ or __linux__

kk_decl_export int  kk_os_copy_file(kk_string_t from, kk_string_t to, bool preserve_mtime, kk_context_t* ctx) {
  int inp = 0;
  int out = 0;

  // stat and create/overwrite target
  struct stat finfo = { 0 };
  if ((inp = kk_posix_open(from, O_RDONLY, ctx)) < 0) {
    kk_string_drop(to, ctx);
    return errno;
  }
  if (fstat(inp, &finfo) < 0) {
    close(inp);
    kk_string_drop(to, ctx);
    return errno;
  }
  if ((out = kk_posix_creat(to, finfo.st_mode)) < 0) {  // keep the mode
    close(inp);
    return errno;
  }
  
  // copy contents
  int err = 0;

#if defined(__APPLE__)
  // macOS
  KK_UNUSED(ctx);
  if (fcopyfile(inp, out, 0, COPYFILE_ALL) != 0) {
    err = errno;
  }
#else 
  // Unix
  #if defined(__linux__)
  KK_UNUSED(ctx);
  off_t copied = 0;
  if (sendfile(out, inp, &copied, finfo.st_size) < 0) {
    err = errno;
  }
  #else
  err = kk_posix_copy_file(inp, out, finfo.st_size, ctx);
  #endif

  // maintain access/mod time
  if (err == 0 && preserve_mtime) {
    struct timespec times[2];
    times[0].tv_sec = finfo.st_atim.tv_sec;
    times[0].tv_nsec = finfo.st_atim.tv_nsec;
    times[1].tv_sec = finfo.st_mtim.tv_sec;
    times[1].tv_nsec = finfo.st_mtim.tv_nsec;
    futimens(out, times);  // in <sys/stat.h>
  }
#endif

  // close file descriptors
  close(inp);
  if (close(out) == -1) {
    if (err == 0) err = errno;
  };

  return err;
}
#endif


/*--------------------------------------------------------------------------------------------------
  Stat directory
--------------------------------------------------------------------------------------------------*/

kk_decl_export bool kk_os_is_directory(kk_string_t path, kk_context_t* ctx) {
  kk_stat_t st = { 0 };
  int err = kk_posix_stat(path, &st, ctx);
  if (err != 0) return false;
  return ((st.st_mode & S_IFDIR) != 0);
}

kk_decl_export bool kk_os_is_file(kk_string_t path, kk_context_t* ctx) {
  kk_stat_t st = { 0 };
  int err = kk_posix_stat(path, &st, ctx);
  if (err != 0) return false;
  return ((st.st_mode & S_IFREG) != 0);
}

/*--------------------------------------------------------------------------------------------------
  List directory
--------------------------------------------------------------------------------------------------*/

#if defined(WIN32)
#include <io.h>
#define dir_cursor intptr_t
#define dir_entry  struct _wfinddata64_t
static bool os_findfirst(kk_string_t path, dir_cursor* d, dir_entry* entry, int* err, kk_context_t* ctx) {
  kk_string_t spath = kk_string_cat_from_utf8(path, "\\*", ctx);
  kk_with_string_as_mutf16_borrow(spath, wpath, ctx) {
    *d = _wfindfirsti64(wpath, entry);
  }
  kk_string_drop(spath,ctx);
  bool ok = (*d != -1);
  *err = (ok ? 0 : errno);
  return ok;
}
static bool os_findnext(dir_cursor d, dir_entry* entry, int* err) {
  bool ok = (_wfindnexti64(d, entry) == 0);
  *err = (ok || errno == ENOENT ? 0 : errno);
  return ok;
}
static void os_findclose(dir_cursor d) {
  _findclose(d);
}
static kk_string_t os_direntry_name(dir_entry* entry, kk_context_t* ctx) {
  if (entry->name == NULL || wcscmp(entry->name, L".") == 0 || wcscmp(entry->name, L"..") == 0) {
    return kk_string_empty();
  }
  else {
    return kk_string_alloc_from_mutf16(entry->name, ctx);
  }
}

#else
#include <sys/types.h>
#include <dirent.h>
#define dir_cursor DIR*
#define dir_entry  struct dirent*
static bool os_findnext(dir_cursor d, dir_entry* entry, int* err) {
  *entry = readdir(d);
  *err = (*entry != NULL || errno == ENOENT ? 0 : errno);
  return (*entry != NULL);
}
static bool os_findfirst(kk_string_t path, dir_cursor* d, dir_entry* entry, int* err, kk_context_t* ctx) {
  kk_with_string_as_mutf8_borrow(path, cpath, ctx) {
    *d = opendir(cpath);
  }
  if (*d == NULL) {
    *err = errno;
    return false;
  }
  else {
    return os_findnext(*d, entry, err);
  }
}
static void os_findclose(dir_cursor d) {
  closedir(d);
}
static kk_string_t os_direntry_name(dir_entry* entry, kk_context_t* ctx) {
  const char* dname = (*entry)->d_name;
  if (dname == NULL || strcmp(dname, ".") == 0 || strcmp(dname, "..") == 0) {
    return kk_string_empty();
  }
  else {
    return kk_string_alloc_from_mutf8(dname, ctx);
  }
}
#endif

kk_decl_export int kk_os_list_directory(kk_string_t dir, kk_vector_t* contents, kk_context_t* ctx) {
  dir_cursor d;
  dir_entry entry;
  int err;
  bool ok = os_findfirst(dir, &d, &entry, &err, ctx);  
  if (!ok) {
    *contents = kk_vector_empty();
    return err;
  }

  size_t count = 0;
  size_t len = 100;
  kk_vector_t vec = kk_vector_alloc(len, kk_integer_box(kk_integer_zero), ctx);
  
  do {
    kk_string_t name = os_direntry_name(&entry, ctx);
    if (!kk_string_is_empty_borrow(name)) {
      // push name
      if (count == len) {
        // realloc vector
        const size_t newlen = (len > 1000 ? len + 1000 : 2*len);
        vec = kk_vector_realloc(vec, newlen, kk_integer_box(kk_integer_zero), ctx);
        len = newlen;
      }
      (kk_vector_buf(vec, NULL))[count] = kk_string_box(name);
      count++;
    }
    else {
      kk_string_drop(name, ctx); // no-op as it is always empty
    }
  } while (os_findnext(d, &entry, &err));
  os_findclose(d);
  
  *contents = kk_vector_realloc(vec, count, kk_box_null, ctx);
  return err;
}


/*--------------------------------------------------------------------------------------------------
  Run system command
--------------------------------------------------------------------------------------------------*/

kk_decl_export int kk_os_run_command(kk_string_t cmd, kk_string_t* output, kk_context_t* ctx) {
  FILE* f = NULL;
#if defined(WIN32)
  kk_with_string_as_mutf16_borrow(cmd, wcmd, ctx) {
    f = _wpopen(wcmd, L"rt"); // todo: maybe open as binary?
  }
#else
  kk_with_string_as_mutf8_borrow(cmd, ccmd, ctx) {
    f = popen(ccmd, POPEN_READ);
  }
#endif
  kk_string_drop(cmd, ctx);
  if (f == NULL) return errno;
  kk_string_t out = kk_string_empty();
  char buf[1025];
  while (fgets(buf, 1024, f) != NULL) {
    buf[1024] = 0; // paranoia
    out = kk_string_cat(out, kk_string_alloc_from_mutf8(buf,ctx), ctx);  // todo: avoid allocation
  }
  if (feof(f)) errno = 0;
#if defined(WIN32)
  _pclose(f);
#else
  pclose(f);
#endif
  *output = out;
  return errno;
}

kk_decl_export int kk_os_run_system(kk_string_t cmd, kk_context_t* ctx) {
  int exitcode = 0;
  #if defined(WIN32)
  kk_with_string_as_mutf16_borrow(cmd, wcmd, ctx) {
    exitcode = _wsystem(wcmd);
  }
  #else
  kk_with_string_as_mutf8_borrow(cmd, ccmd, ctx) {
    exitcode = system(ccmd);
  }
  #endif
  kk_string_drop(cmd, ctx);
  return exitcode;
}



/*--------------------------------------------------------------------------------------------------
  Args
--------------------------------------------------------------------------------------------------*/

#if defined(WIN32)
#include <shellapi.h>
kk_vector_t kk_os_get_argv(kk_context_t* ctx) {
  if (ctx->argc == 0 || ctx->argv == NULL) return kk_vector_empty();
  LPWSTR cmd = GetCommandLineW();
  int iwargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(cmd, &iwargc);
  size_t wargc = iwargc;
  if (wargv==NULL) return kk_vector_empty();
  size_t i = 0;
  kk_assert_internal(ctx->argc <= wargc);
  if (ctx->argc < (size_t)wargc) i = (size_t)wargc - ctx->argc;
  kk_vector_t args = kk_vector_alloc(wargc, kk_box_null, ctx);
  kk_box_t* buf = kk_vector_buf(args, NULL);
  for ( ; i < wargc; i++) {
    kk_string_t arg = kk_string_alloc_from_mutf16(wargv[i], ctx);
    buf[i] = kk_string_box(arg);
  }
  LocalFree(wargv);
  return args;
}
#else
kk_vector_t kk_os_get_argv(kk_context_t* ctx) {  
  if (ctx->argc==0 || ctx->argv==NULL) return kk_vector_empty();
  kk_vector_t args = kk_vector_alloc(ctx->argc, kk_box_null, ctx);
  kk_box_t* buf = kk_vector_buf(args, NULL);
  for (size_t i = 0; i < ctx->argc; i++) {
    kk_string_t arg = kk_string_alloc_from_mutf8(ctx->argv[i], ctx);    
    buf[i] = kk_string_box(arg);
  }
  return args;
}
#endif

#if defined WIN32
#include <Windows.h>
kk_decl_export kk_vector_t kk_os_get_env(kk_context_t* ctx) {
  const LPWCH env = GetEnvironmentStringsW();
  if (env==NULL) return kk_vector_empty();
  // first count the number of environment variables  (ends with two zeros)
  size_t count = 0;
  for (size_t i = 0; !(env[i]==0 && env[i+1]==0); i++) {
    if (env[i]==0) count++;
  }
  kk_vector_t v = kk_vector_alloc(count*2, kk_box_null, ctx);
  kk_box_t* buf = kk_vector_buf(v, NULL);
  const uint16_t* p = env;
  // copy the strings into the vector
  for(size_t i = 0; i < count; i++) {
    const uint16_t* pname = p;
    while (*p != '=' && *p != 0) { p++; }
    kk_string_t name = kk_string_alloc_from_mutf16n((size_t)(p - pname), pname, ctx);
    buf[2*i] = kk_string_box( name );
    p++; // skip '='
    const uint16_t* pvalue = p;
    while (*p != 0) { p++; }
    kk_string_t val = kk_string_alloc_from_mutf16n((size_t)(p - pvalue), pvalue, ctx);
    buf[2*i + 1] = kk_string_box(val);
    p++;
  }
  FreeEnvironmentStringsW(env);
  return v;
}
#else
#if defined(__APPLE__) && defined(__has_include) && __has_include(<crt_externs.h>)
#include <crt_externs.h>
static char** kk_get_environ(void) {
  return (*_NSGetEnviron());
}
#else
// On Posix systems use `environ`
extern char** environ;
static char** kk_get_environ(void) {
  return environ;
}
#endif
kk_decl_export kk_vector_t kk_os_get_env(kk_context_t* ctx) {
  const char** env = (const char**)kk_get_environ();
  if (env==NULL) return kk_vector_empty();
  // first count the number of environment variables
  size_t count;
  for (count = 0; env[count]!=NULL; count++) { /* nothing */ }
  kk_vector_t v = kk_vector_alloc(count*2, kk_box_null, ctx);
  kk_box_t* buf = kk_vector_buf(v, NULL);
  // copy the strings into the vector
  for (size_t i = 0; i < count; i++) {
    const char* p = env[i];
    const char* pname = p;
    while (*p != '=' && *p != 0) { p++; }
    kk_string_t name = kk_string_alloc_from_mutf8n((size_t)(p - pname), pname, ctx);
    buf[2*i] = kk_string_box(name);
    p++; // skip '='
    const char* pvalue = p;
    while (*p != 0) { p++; }
    kk_string_t val = kk_string_alloc_from_mutf8n((size_t)(p - pvalue), pvalue, ctx);
    buf[2*i + 1] = kk_string_box(val);
  }
  return v;
}
#endif


/*--------------------------------------------------------------------------------------------------
  Path max
--------------------------------------------------------------------------------------------------*/
kk_decl_export size_t kk_os_path_max(void);

#if defined(WIN32)
kk_decl_export size_t kk_os_path_max(void) {
  return 32*1024; // _MAX_PATH;
}

#elif defined(__MACH__)
#include <sys/syslimits.h>
kk_decl_export size_t kk_os_path_max(void) {
  return PATH_MAX;
}

#elif defined(unix) || defined(__unix__) || defined(__unix)
#include <unistd.h>  // pathconf
kk_decl_export size_t kk_os_path_max(void) {
  #ifdef PATH_MAX
  return PATH_MAX;
  #else
  static size_t path_max = 0;
  if (path_max <= 0) {
    long m = pathconf("/", _PC_PATH_MAX);
    if (m <= 0) path_max = 4096;      // guess
    else if (m < 256) path_max = 256; // at least 256
    else path_max = m;
  }
  return path_max;
  #endif
}
#else
kk_decl_export size_t kk_os_path_max(void) {
#ifdef PATH_MAX
  return PATH_MAX;
#else
  return 4096;
#endif
}
#endif

/*--------------------------------------------------------------------------------------------------
  Realpath
--------------------------------------------------------------------------------------------------*/

#if defined(WIN32)
kk_string_t kk_os_realpath(kk_string_t path, kk_context_t* ctx) {
  kk_string_t rpath = kk_string_empty();
  kk_with_string_as_mutf16_borrow(path, wpath, ctx) {
    uint16_t buf[264];
    DWORD res = GetFullPathNameW(wpath, 264, buf, NULL);
    if (res == 0) {
      // failure
      rpath = kk_string_dup(path);
    }
    else if (res >= 264) {
      DWORD pbuflen = res;
      uint16_t* pbuf = (uint16_t*)kk_malloc(pbuflen * sizeof(uint16_t*), ctx);
      res = GetFullPathNameW(wpath, pbuflen, pbuf, NULL);
      if (res == 0 || res >= pbuflen) {
        // failed again
        rpath = kk_string_dup(path);
      }
      else {
        rpath = kk_string_alloc_from_mutf16(pbuf, ctx);
      }
      kk_free(pbuf);
    }
    else {
      rpath = kk_string_alloc_from_mutf16(buf, ctx);
    }
  }
  kk_string_drop(path,ctx);
  return rpath;
}

#elif defined(__linux__) || defined(__CYGWIN__) || defined(__sun) || defined(unix) || defined(__unix__) || defined(__unix) || defined(__MACH__)
kk_string_t kk_os_realpath(kk_string path, kk_context_t* ctx) {
  kk_string_t s = kk_string_empty();
  kk_with_string_as_mutf8_borrow(path, cpath, ctx) {
    const char* rpath = realpath(cpath, NULL);
    s = kk_string_alloc_from_mutf8((rpath != NULL ? rpath : cpath), ctx);
    free(rpath);
  }
  kk_string_drop(path, ctx);
  return s;
}

#else
#pragma message("realpath ignored on this platform")
kk_string_t kk_os_realpath(kk_string_t fname, kk_context_t* ctx) {
  KK_UNUSED(ctx);
  return fname;
}
#endif


/*--------------------------------------------------------------------------------------------------
  Application path
--------------------------------------------------------------------------------------------------*/
#ifdef WIN32
#define KK_PATH_SEP  ';'
#define KK_DIR_SEP   '\\'
#include <io.h>
#else
#define KK_PATH_SEP  ':'
#define KK_DIR_SEP   '/'
#include <unistd.h>
#endif


static kk_string_t kk_os_searchpathx(const char* paths, const char* fname, kk_context_t* ctx) {
  if (paths==NULL || fname==NULL || fname[0]==0) return kk_string_empty();
  const char* p = paths;
  size_t pathslen = strlen(paths);
  size_t fnamelen = strlen(fname);
  char* buf = (char*)kk_malloc(pathslen + fnamelen + 2, ctx);
  if (buf==NULL) return kk_string_empty();

  kk_string_t s = kk_string_empty();
  const char* pend = p + pathslen;
  while (p < pend) {
    const char* r = strchr(p, KK_PATH_SEP);
    if (r==NULL) r = pend;
    size_t plen = (size_t)(r - p);
    memcpy(buf, p, plen);
    memcpy(buf + plen, "/", 1);
    memcpy(buf + plen + 1, fname, fnamelen);
    buf[plen+1+fnamelen] = 0;
    p = (r == pend ? r : r + 1);
    kk_string_t sfname = kk_string_alloc_from_mutf8(buf, ctx);
    if (kk_os_is_file( kk_string_dup(sfname), ctx)) {
      s = kk_os_realpath(sfname,ctx);
      break;
    }
    else {
      kk_string_drop(sfname,ctx);
    }
  }
  kk_free(buf);
  return s;
}

// generic application path by using argv[0] and looking at the current working directory and the PATH environment.
static kk_string_t kk_os_app_path_generic(kk_context_t* ctx) {
  const char* p = (ctx->argc > 0 ? ctx->argv[0] : NULL);
  if (p==NULL || strlen(p)==0) return kk_string_empty();

  if (p[0]=='/'
#ifdef WIN32
      || (p[1]==':' && ((p[0] >= 'a' && p[0] <= 'z') || ((p[0] >= 'A' && p[0] <= 'Z'))) && (p[2]=='\\' || p[2]=='/'))
#endif
     ) {
    // absolute path
    return kk_os_realpath( kk_string_alloc_from_mutf8(p,ctx), ctx);
  }
  else if (strchr(p,'/') != NULL
#ifdef WIN32
    || strchr(p,'\\') != NULL
#endif
    ) {
    // relative path, combine with "./"
    char* cs;
    kk_string_t s = kk_string_alloc_cbuf( strlen(p) + 2, &cs, ctx);
    strcpy(cs, "./" );
    strcat(cs, p);
    s = kk_string_convert_from_mutf8(s, ctx);
    return kk_os_realpath(s, ctx);
  }
  else {
    // basename, try to prefix with all entries in PATH
    kk_string_t s = kk_os_searchpathx(getenv("PATH"), p, ctx);
    if (kk_string_is_empty_borrow(s)) s = kk_os_realpath(kk_string_alloc_from_mutf8(p,ctx),ctx);
    return s;
  }
}

#if defined(WIN32)
#include <Windows.h>
kk_decl_export kk_string_t kk_os_app_path(kk_context_t* ctx) {
  uint16_t buf[264];
  DWORD len = GetModuleFileNameW(NULL, buf, 264);
  buf[min(len,263)] = 0;
  if (len == 0) { 
    // fail, fall back
    return kk_os_app_path_generic(ctx);
  }
  else if (len < 264) {
    // success
    return kk_string_alloc_from_mutf16(buf, ctx);
  }
  else {
    // not enough space in the buffer, try again with larger buffer
    size_t slen = kk_os_path_max();
    uint16_t* bbuf = (uint16_t*)kk_malloc((slen+1) * sizeof(uint16_t), ctx);
    len = GetModuleFileNameW(NULL, bbuf, (DWORD)slen+1);
    if (len >= slen) {
      // failed again, use fall back
      kk_free(bbuf);
      return kk_os_app_path_generic(ctx);
    }
    else {
      kk_string_t s = kk_string_alloc_from_mutf16(bbuf, ctx);
      kk_free(bbuf);
      return s;
    }
  }
}
#elif defined(__MACH__)
#include <errno.h>
#include <libproc.h>
#include <unistd.h>
kk_string_t kk_os_app_path(kk_context_t* ctx) {
  pid_t pid = getpid();
  char* buf = (char*)kk_malloc(PROC_PIDPATHINFO_MAXSIZE + 1,ctx);  
  int ret = proc_pidpath(pid, buf, PROC_PIDPATHINFO_MAXSIZE /* must be this value or the call fails */);
  if (ret > 0) {
    // failed, use fall back
    kk_free(buf);
    return kk_os_app_path_generic(ctx);
  }
  else {
    kk_string_t path = kk_string_alloc_from_mutf8(buf, ctx);
    kk_free(buf);
    return path;
  }
}

#elif defined(__linux__) || defined(__CYGWIN__) || defined(__sun) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__FreeBSD__)
#if defined(__sun)
#define KK_PROC_SELF "/proc/self/path/a.out"
#elif defined(__NetBSD__)
#define KK_PROC_SELF "/proc/curproc/exe"
#elif defined(__DragonFly__) || defined(__FreeBSD__)
#define KK_PROC_SELF "/proc/curproc/file"
#else
#define KK_PROC_SELF "/proc/self/exe"
#endif

kk_string_t kk_os_app_path(kk_context_t* ctx) {
  kk_string_t s = kk_os_realpath(kk_string_alloc_dup_unsafe(KK_PROC_SELF,ctx),ctx);
  if (strcmp(kk_string_cbuf_borrow(s), KK_PROC_SELF)==0) {
    // failed? try generic search
    kk_string_drop(s, ctx);
    return kk_os_app_path_generic(ctx);
  }
  else {
    return s;
  }
}

#else
#pragma message("using generic application path detection")
kk_string_t kk_os_app_path(kk_context_t* ctx) {
  return kk_os_app_path_generic(ctx);
}
#endif

/*--------------------------------------------------------------------------------------------------
  Misc.
--------------------------------------------------------------------------------------------------*/

kk_decl_export kk_string_t kk_os_path_sep(kk_context_t* ctx) {
  char pathsep[2] = { KK_PATH_SEP, 0 };
  return kk_string_alloc_dup_utf8(pathsep, ctx);
}

kk_decl_export kk_string_t kk_os_dir_sep(kk_context_t* ctx) {
  char dirsep[2] = { KK_DIR_SEP, 0 };
  return kk_string_alloc_dup_utf8(dirsep, ctx);
}

kk_decl_export kk_string_t kk_os_home_dir(kk_context_t* ctx) {
#if defined(WIN32)
  const uint16_t* h = _wgetenv(L"HOME");
  if (h != NULL) return kk_string_alloc_from_mutf16(h, ctx);
  const uint16_t* hd = _wgetenv(L"HOMEDRIVE");
  const uint16_t* hp = _wgetenv(L"HOMEPATH");
  if (hd!=NULL && hp!=NULL) {
    kk_string_t hds = kk_string_alloc_from_mutf16(hd, ctx);
    kk_string_t hdp = kk_string_alloc_from_mutf16(hp, ctx);
    return kk_string_cat(hds,hdp,ctx);
  }
#else
  const char* h = getenv("HOME");
  if (h != NULL) return kk_string_alloc_from_mutf8(h, ctx);  
#endif
  // fallback
  return kk_string_alloc_dup_utf8(".", ctx);
}

kk_decl_export kk_string_t kk_os_temp_dir(kk_context_t* ctx) 
{  
#if defined(WIN32)
  const uint16_t* tmp = _wgetenv(L"TEMP");
  if (tmp != NULL) return kk_string_alloc_from_mutf16(tmp, ctx);
  tmp = _wgetenv(L"TEMPDIR");
  if (tmp != NULL) return kk_string_alloc_from_mutf16(tmp, ctx);
  const uint16_t* ad = _wgetenv(L"LOCALAPPDATA");
  if (ad!=NULL) {
    kk_string_t s = kk_string_alloc_from_mutf16(ad, ctx);
    return kk_string_cat_from_utf8(s, "\\Temp", ctx);
  }
#else
  const char* tmp = getenv("TEMP");
  if (tmp != NULL) return kk_string_alloc_from_mutf8(tmp, ctx);
  tmp = getenv("TEMPDIR");
  if (tmp != NULL) return kk_string_alloc_from_mutf8(tmp, ctx);
#endif
  // fallback
#if defined(WIN32)
  return kk_string_alloc_dup_utf8("c:\\tmp", ctx);
#else
  return kk_string_alloc_dup_unsafe("/tmp", ctx);
#endif
}

/*--------------------------------------------------------------------------------------------------
  Environment
--------------------------------------------------------------------------------------------------*/

kk_string_t kk_os_kernel(kk_context_t* ctx) {
  const char* kernel = "unknown";
#if defined(WIN32)
  #if defined(__MINGW32__)
  kernel = "windows-mingw";
  #else
  kernel = "windows";
  #endif
#elif defined(__linux__)
  kernel = "linux";
#elif defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_IPHONE_SIMULATOR
    kernel = "ios";
  #elif TARGET_OS_IPHONE
    kernel = "ios";
  #elif TARGET_OS_MAC
    kernel = "osx";
  #else
    kernel = "osx";  // unknown?
  #endif
#elif defined(__ANDROID__)
  kernel = "android";
#elif defined(__CYGWIN__) && !defined(WIN32)
  kernel = "unix-cygwin";
#elif defined(__hpux)
  kernel = "unix-hpux";
#elif defined(_AIX)
  kernel = "unix-aix";
#elif defined(__sun) && defined(__SVR4)
  kernel = "unix-solaris";
#elif defined(unix) || defined(__unix__) 
  #include <sys/param.h>
  #if defined(__FreeBSD__)
  kernel = "unix-freebsd";
  #elif defined(__OpenBSD__)
  kernel = "unix-openbsd";
  #elif defined(__DragonFly__)
  kernel = "unix-dragonfly";
  #elif defined(__HAIKU__)
  kernel = "unix-haiku";
  #elif defined(BSD)
  kernel = "unix-bsd"; 
  #else
  kernel = "unix";
  #endif
#elif defined(_POSIX_VERSION)
  kernel = "posix"
#endif
  return kk_string_alloc_dup_utf8(kernel, ctx);
}

kk_string_t kk_os_arch(kk_context_t* ctx) {
  char* arch = "unknown";
#if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64) || defined(_M_X64) || defined(_M_AMD64)
  arch = "amd64";
#elif defined(__i386__) || defined(__i386) || defined(_M_IX86) || defined(_X86_) || defined(__X86__)
  arch = "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
  arch = "arm64";
#elif defined(__arm__) || defined(_ARM) || defined(_M_ARM)  || defined(_M_ARMT) || defined(__arm)
  arch = "arm";
#elif defined(__riscv) || defined(_M_RISCV)
  arch = "riscv";  
#elif defined(__alpha__) || defined(_M_ALPHA) || defined(__alpha)
  arch = "alpha";
#elif defined(__powerpc) || defined(__powerpc__) || defined(_M_PPC) || defined(__ppc)
  arch = "powerpc";
#elif defined(__hppa__)
  arch = "hppa";
#elif defined(__m68k__)
  arch = "m68k";
#elif defined(__mips__)
  arch = "mips";
#elif defined(__sparc__) || defined(__sparc)
  arch = "sparc";
#endif
  return kk_string_alloc_dup_utf8(arch, ctx);
}

kk_string_t kk_compiler_version(kk_context_t* ctx) {
#if defined(KK_COMP_VERSION)
  const char* version = KK_COMP_VERSION;
#else
  const char* version = "2.x.x";
#endif
  return kk_string_alloc_dup_utf8(version,ctx);
}

// note: assumes unistd/Windows etc is already included (like for file copy)
int kk_os_processor_count(kk_context_t* ctx) {
  KK_UNUSED(ctx);
  int cpu_count = 1;
#if defined(WIN32)
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  cpu_count = (int)sysinfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
  cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROCESSORS_CONF)
  cpu_count = sysconf(_SC_NPROCESSORS_CONF);
#elif defined(HW_AVAILCPU)
  int mib[4];
  size_t len = sizeof(cpu_count);
  mib[0] = CTL_HW;
  mib[1] = HW_AVAILCPU;  
  sysctl(mib, 2, &cpu_count, &len, NULL, 0);
  #if defined(HW_NCPU)
  if (cpu_count < 1) {
    mib[1] = HW_NCPU;
    sysctl(mib, 2, &cpu_count, &len, NULL, 0);
  }
  #endif 
#elif defined(MPC_GETNUMSPUS)
  cpu_count = mpctl(MPC_GETNUMSPUS, NULL, NULL);
#endif
  return (cpu_count < 1 ? 1 : cpu_count);
}