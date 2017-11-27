/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1997, 1998, 1999
 *  Sleepycat Software.  All rights reserved.
 *
 *  @(#)os.h  11.1 (Sleepycat) 7/25/99
 */

#if defined(_MSC_VER)
#include <windows.h>
#endif

/*
 * Filehandle.
 */
struct __fh_t
{
#if defined(_MSC_VER)           /* _WIN32 */
  HANDLE handle;                /* Windows/32 file handle. */
#endif
  int fd;                       /* POSIX file descriptor. */

  u_int32_t log_size;           /* XXX: Log file size. */

#define  DB_FH_NOSYNC  0x01     /* Handle doesn't need to be sync'd. */
#define  DB_FH_VALID  0x02      /* Handle is valid. */
  u_int8_t flags;
};

/*
 * We group certain seek/write calls into a single function so that we
 * can use pread(2)/pwrite(2) where they're available.
 */
#define  DB_IO_READ  1
#define  DB_IO_WRITE  2
typedef struct __io_t
{
  DB_FH *fhp;                   /* I/O file handle. */
  MUTEX *mutexp;                /* Mutex to lock. */
  size_t pagesize;              /* Page size. */
  db_pgno_t pgno;               /* Page number. */
  u_int8_t *buf;                /* Buffer. */
  size_t bytes;                 /* Bytes read/written. */
} DB_IO;
