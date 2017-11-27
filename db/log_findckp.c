/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996, 1997, 1998, 1999
 *  Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char sccsid[] = "@(#)log_findckp.c  11.1 (Sleepycat) 7/24/99";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <errno.h>
#include <string.h>
#endif

#include "db_int.h"
#include "log.h"
#include "txn.h"

/*
 * CDB___log_findckp --
 *
 * Looks for the most recent checkpoint that occurs before the most recent
 * checkpoint LSN, subject to the constraint that there must be at least two
 * checkpoints.  The reason you need two checkpoints is that you might have
 * crashed during the most recent one and may not have a copy of all the
 * open files.  This is the point from which recovery can start and the
 * point up to which archival/truncation can take place.  Checkpoints in
 * the log look like:
 *
 * -------------------------------------------------------------------
 *  | ckp A, ckplsn 100 |  .... record .... | ckp B, ckplsn 600 | ...
 * -------------------------------------------------------------------
 *         LSN 500                                 LSN 1000
 *
 * If we read what log returns from using the DB_CKP parameter to logput,
 * we'll get the record at LSN 1000.  The checkpoint LSN there is 600.
 * Now we have to scan backwards looking for a checkpoint before LSN 600.
 * We find one at 500.  This means that we can truncate the log before
 * 500 or run recovery beginning at 500.
 *
 * Returns 0 if we find a suitable checkpoint or we retrieved the first
 * record in the log from which to start.  Returns DB_NOTFOUND if there
 * are no log records, errno on error.
 *
 * PUBLIC: int CDB___log_findckp __P((DB_ENV *, DB_LSN *));
 */
int
CDB___log_findckp (dbenv, lsnp)
     DB_ENV *dbenv;
     DB_LSN *lsnp;
{
  DBT data;
  DB_LSN ckp_lsn, final_ckp, last_ckp, next_lsn;
  __txn_ckp_args *ckp_args;
  int ret;

  /*
   * Need to find the appropriate point from which to begin
   * recovery.
   */
  memset (&data, 0, sizeof (data));
  if (F_ISSET (dbenv, DB_ENV_THREAD))
    F_SET (&data, DB_DBT_MALLOC);
  ZERO_LSN (ckp_lsn);
  if ((ret = CDB_log_get (dbenv, &last_ckp, &data, DB_CHECKPOINT)) != 0)
  {
    if (ret == ENOENT)
      goto get_first;
    else
      return (ret);
  }
  final_ckp = last_ckp;

  next_lsn = last_ckp;
  do
  {
    if (F_ISSET (dbenv, DB_ENV_THREAD))
      CDB___os_free (data.data, data.size);

    if ((ret = CDB_log_get (dbenv, &next_lsn, &data, DB_SET)) != 0)
      return (ret);
    if ((ret = CDB___txn_ckp_read (data.data, &ckp_args)) != 0)
    {
      if (F_ISSET (dbenv, DB_ENV_THREAD))
        CDB___os_free (data.data, data.size);
      return (ret);
    }
    if (IS_ZERO_LSN (ckp_lsn))
      ckp_lsn = ckp_args->ckp_lsn;
    if (FLD_ISSET (dbenv->verbose, DB_VERB_CHKPOINT))
    {
      CDB___db_err (dbenv, "Checkpoint at: [%lu][%lu]",
                    (u_long) last_ckp.file, (u_long) last_ckp.offset);
      CDB___db_err (dbenv, "Checkpoint LSN: [%lu][%lu]",
                    (u_long) ckp_args->ckp_lsn.file,
                    (u_long) ckp_args->ckp_lsn.offset);
      CDB___db_err (dbenv, "Previous checkpoint: [%lu][%lu]",
                    (u_long) ckp_args->last_ckp.file,
                    (u_long) ckp_args->last_ckp.offset);
    }
    last_ckp = next_lsn;
    next_lsn = ckp_args->last_ckp;
    CDB___os_free (ckp_args, sizeof (*ckp_args));

    /*
     * Keep looping until either you 1) run out of checkpoints,
     * 2) you've found a checkpoint before the most recent
     * checkpoint's LSN and you have at least 2 checkpoints.
     */
  }
  while (!IS_ZERO_LSN (next_lsn) &&
         (CDB_log_compare (&last_ckp, &ckp_lsn) > 0 ||
          CDB_log_compare (&final_ckp, &last_ckp) == 0));

  if (F_ISSET (dbenv, DB_ENV_THREAD))
    CDB___os_free (data.data, data.size);

  /*
   * At this point, either, next_lsn is ZERO or ckp_lsn is the
   * checkpoint lsn and last_ckp is the LSN of the last checkpoint
   * before ckp_lsn.  If the compare in the loop is still true, then
   * next_lsn must be 0 and we need to roll forward from the
   * beginning of the log.
   */
  if (CDB_log_compare (&last_ckp, &ckp_lsn) >= 0 ||
      CDB_log_compare (&final_ckp, &last_ckp) == 0)
  {
  get_first:if ((ret =
         CDB_log_get (dbenv, &last_ckp, &data, DB_FIRST)) != 0)
      return (ret);
    if (F_ISSET (dbenv, DB_ENV_THREAD))
      CDB___os_free (data.data, data.size);
  }
  *lsnp = last_ckp;

  return (IS_ZERO_LSN (last_ckp) ? DB_NOTFOUND : 0);
}
