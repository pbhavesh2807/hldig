/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996, 1997, 1998, 1999
 *  Sleepycat Software.  All rights reserved.
 */
/*
 * Copyright (c) 1995, 1996
 *  The President and Fellows of Harvard University.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Margo Seltzer.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "db_config.h"

#ifndef lint
static const char sccsid[] = "@(#)txn.c  11.13 (Sleepycat) 11/10/99";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#if TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <time.h>
#endif
#endif

#include <errno.h>
#include <string.h>
#endif

#include "db_int.h"
#include "db_shash.h"
#include "txn.h"
#include "lock.h"
#include "log.h"
#include "db_dispatch.h"

static int CDB___txn_begin __P ((DB_TXN *));
static int CDB___txn_check_running __P ((const DB_TXN *, TXN_DETAIL **));
static int CDB___txn_count __P ((DB_TXN *));
static void CDB___txn_freekids __P ((DB_TXN *));
static void CDB___txn_lsn __P ((DB_TXN *, DB_LSN **));
static int CDB___txn_makefamily __P ((DB_TXN *, int *, DB_LSN **));
static int CDB___txn_undo __P ((DB_TXN *));

#define  TXN_BUBBLE(AP, MAX) {            \
  int __j;              \
  DB_LSN __tmp;              \
                  \
  for (__j = 0; __j < MAX - 1; __j++)        \
    if (CDB_log_compare(&AP[__j], &AP[__j + 1]) < 0) {    \
      __tmp = AP[__j];        \
      AP[__j] = AP[__j + 1];        \
      AP[__j + 1] = __tmp;        \
    }              \
}

/*
 * CDB_txn_begin --
 *  This is a wrapper to the actual begin process.  Normal CDB_txn_begin()
 * allocates a DB_TXN structure for the caller, while txn_xa_begin() does
 * not.  Other than that, both call into the common CDB___txn_begin code().
 *
 * Internally, we use TXN_DETAIL structures, but the DB_TXN structure
 * provides access to the transaction ID and the offset in the transaction
 * region of the TXN_DETAIL structure.
 */
int
CDB_txn_begin (dbenv, parent, txnpp, flags)
     DB_ENV *dbenv;
     DB_TXN *parent, **txnpp;
     u_int32_t flags;
{
  DB_TXN *txn;
  int ret;

  PANIC_CHECK (dbenv);
  ENV_REQUIRES_CONFIG (dbenv, dbenv->tx_handle, DB_INIT_TXN);

  if ((ret = CDB___db_fchk (dbenv,
                            "CDB_txn_begin", flags,
                            DB_TXN_NOWAIT | DB_TXN_NOSYNC | DB_TXN_SYNC)) !=
      0)
    return (ret);
  if ((ret = CDB___db_fcchk (dbenv,
                             "CDB_txn_begin", flags, DB_TXN_NOSYNC,
                             DB_TXN_SYNC)) != 0)
    return (ret);

  if ((ret = CDB___os_calloc (1, sizeof (DB_TXN), &txn)) != 0)
    return (ret);

  txn->mgrp = dbenv->tx_handle;
  txn->parent = parent;
  TAILQ_INIT (&txn->kids);
  txn->flags = TXN_MALLOC;
  if (LF_ISSET (DB_TXN_NOSYNC))
    F_SET (txn, TXN_NOSYNC);
  if (LF_ISSET (DB_TXN_SYNC))
    F_SET (txn, TXN_SYNC);
  if (LF_ISSET (DB_TXN_NOWAIT))
    F_SET (txn, TXN_NOWAIT);

  if ((ret = CDB___txn_begin (txn)) != 0)
  {
    CDB___os_free (txn, sizeof (DB_TXN));
    txn = NULL;
  }

  if (txn != NULL && parent != NULL)
    TAILQ_INSERT_HEAD (&parent->kids, txn, klinks);

  *txnpp = txn;
  return (ret);
}

/*
 * CDB___txn_xa_begin --
 *  XA version of CDB_txn_begin.
 *
 * PUBLIC: int CDB___txn_xa_begin __P((DB_ENV *, DB_TXN *));
 */
int
CDB___txn_xa_begin (dbenv, txn)
     DB_ENV *dbenv;
     DB_TXN *txn;
{
  PANIC_CHECK (dbenv);

  memset (txn, 0, sizeof (DB_TXN));

  txn->mgrp = dbenv->tx_handle;

  return (CDB___txn_begin (txn));
}

/*
 * CDB___txn_begin --
 *  Normal DB version of CDB_txn_begin.
 */
static int
CDB___txn_begin (txn)
     DB_TXN *txn;
{
  DB_ENV *dbenv;
  DB_LSN begin_lsn;
  DB_TXNMGR *mgr;
  DB_TXNREGION *region;
  TXN_DETAIL *td;
  size_t off;
  u_int32_t id;
  int ret;

  mgr = txn->mgrp;
  dbenv = mgr->dbenv;
  region = mgr->reginfo.primary;

  /*
   * We do not have to write begin records (and if we do not, then we
   * need never write records for read-only transactions).  However,
   * we do need to find the current LSN so that we can store it in the
   * transaction structure, so we can know where to take checkpoints.
   */
  if (F_ISSET (dbenv, DB_ENV_LOGGING) && (ret =
                                          CDB_log_put (dbenv, &begin_lsn,
                                                       NULL, DB_CURLSN)) != 0)
    goto err2;

  R_LOCK (dbenv, &mgr->reginfo);

  /* Make sure that last_txnid is not going to wrap around. */
  if (region->last_txnid == TXN_INVALID)
  {
    CDB___db_err (dbenv, "CDB_txn_begin: %s  %s",
                  "Transaction ID wrapping.",
                  "Snapshot your database and start a new log.");
    ret = EINVAL;
    goto err1;
  }

  /* Allocate a new transaction detail structure. */
  if ((ret =
       CDB___db_shalloc (mgr->reginfo.addr, sizeof (TXN_DETAIL), 0,
                         &td)) != 0)
    goto err1;

  /* Place transaction on active transaction list. */
  SH_TAILQ_INSERT_HEAD (&region->active_txn, td, links, __txn_detail);

  id = ++region->last_txnid;
  ++region->nbegins;
  if (++region->nactive > region->maxnactive)
    region->maxnactive = region->nactive;

  td->txnid = id;
  td->begin_lsn = begin_lsn;
  ZERO_LSN (td->last_lsn);
  td->status = TXN_RUNNING;
  if (txn->parent != NULL)
    td->parent = txn->parent->off;
  else
    td->parent = INVALID_ROFF;

  off = R_OFFSET (&mgr->reginfo, td);
  R_UNLOCK (dbenv, &mgr->reginfo);

  ZERO_LSN (txn->last_lsn);
  txn->txnid = id;
  txn->off = off;

  /*
   * If this is a transaction family, we must
   * link the child to the maximal grandparent
   * in the lock table for deadlock detection.
   */
  if (txn->parent != NULL && F_ISSET (dbenv, DB_ENV_LOCKING | DB_ENV_CDB))
  {
    if ((ret = CDB___lock_addfamilylocker (dbenv,
                                           txn->parent->txnid,
                                           txn->txnid)) != 0)
      goto err2;
  }


  if (F_ISSET (txn, TXN_MALLOC))
  {
    MUTEX_THREAD_LOCK (mgr->mutexp);
    TAILQ_INSERT_TAIL (&mgr->txn_chain, txn, links);
    MUTEX_THREAD_UNLOCK (mgr->mutexp);
  }

  return (0);

err1:R_UNLOCK (dbenv, &mgr->reginfo);

err2:return (ret);
}

/*
 * CDB_txn_commit --
 *  Commit a transaction.
 */
int
CDB_txn_commit (txnp, flags)
     DB_TXN *txnp;
     u_int32_t flags;
{
  DB_ENV *dbenv;
  DB_TXN *kids;
  DB_TXNMGR *mgr;
  int ret;

  mgr = txnp->mgrp;
  dbenv = mgr->dbenv;

  PANIC_CHECK (dbenv);
  if ((ret = CDB___db_fchk (dbenv,
                            "CDB_txn_commit", flags,
                            DB_TXN_NOSYNC | DB_TXN_SYNC)) != 0)
    return (ret);

  if ((ret = CDB___db_fcchk (dbenv,
                             "CDB_txn_commit", flags, DB_TXN_NOSYNC,
                             DB_TXN_SYNC)) != 0)
    return (ret);

  if ((ret = CDB___txn_check_running (txnp, NULL)) != 0)
    return (ret);

  if (LF_ISSET (DB_TXN_NOSYNC))
  {
    F_CLR (txnp, TXN_SYNC);
    F_SET (txnp, TXN_NOSYNC);
  }
  if (LF_ISSET (DB_TXN_SYNC))
  {
    F_CLR (txnp, TXN_NOSYNC);
    F_SET (txnp, TXN_SYNC);
  }

  /* Commit any uncommitted children. */
  for (kids = TAILQ_FIRST (&txnp->kids);
       kids != NULL; kids = TAILQ_NEXT (kids, klinks))
    if (!F_ISSET (kids, TXN_CHILDCOMMIT) &&
        (ret = CDB_txn_commit (kids, flags)) != 0)
      return (ret);

  /*
   * If there are any log records, write a log record and sync the log,
   * else do no log writes.  If the commit is for a child transaction,
   * we do not need to commit the child synchronously since it may still
   * abort (if its parent aborts), and otherwise its parent or ultimate
   * ancestor will write synchronously.
   */
  if (F_ISSET (dbenv, DB_ENV_LOGGING) &&
      (F_ISSET (txnp, TXN_MUSTFLUSH) || !IS_ZERO_LSN (txnp->last_lsn)))
  {
    if (txnp->parent == NULL)
      ret = CDB___txn_regop_log (dbenv, txnp, &txnp->last_lsn,
                                 (F_ISSET (mgr->dbenv, DB_ENV_TXN_NOSYNC) &&
                                  !F_ISSET (txnp, TXN_SYNC)) ||
                                 F_ISSET (txnp, TXN_NOSYNC) ? 0 : DB_FLUSH,
                                 TXN_COMMIT);
    else
    {
      F_SET (txnp->parent, TXN_MUSTFLUSH);
      ret = CDB___txn_child_log (dbenv, txnp, &txnp->last_lsn, 0,
                                 TXN_COMMIT, txnp->parent->txnid);
    }
    if (ret != 0)
      return (ret);
  }

  /*
   * If this is the senior ancestor (i.e., it has no parent), then we
   * can release all the child transactions since everyone is committing.
   * Then we can release this transaction.  If this is not the ultimate
   * ancestor, then we can neither free it or its children.
   */
  if (txnp->parent == NULL)
    CDB___txn_freekids (txnp);

  return (CDB___txn_end (txnp, 1));
}

/*
 * CDB_txn_abort --
 *  Abort a transaction.
 */
int
CDB_txn_abort (txnp)
     DB_TXN *txnp;
{
  int ret;

  PANIC_CHECK (txnp->mgrp->dbenv);
  if ((ret = CDB___txn_check_running (txnp, NULL)) != 0)
    return (ret);

  if ((ret = CDB___txn_undo (txnp)) != 0)
  {
    CDB___db_err (txnp->mgrp->dbenv,
                  "CDB_txn_abort: Log undo failed %s", CDB_db_strerror (ret));
    return (ret);
  }
  return (CDB___txn_end (txnp, 0));
}

/*
 * CDB_txn_prepare --
 *  Flush the log so a future commit is guaranteed to succeed.
 */
int
CDB_txn_prepare (txnp)
     DB_TXN *txnp;
{
  DBT xid;
  DB_ENV *dbenv;
  TXN_DETAIL *td;
  int ret;

  if ((ret = CDB___txn_check_running (txnp, &td)) != 0)
    return (ret);

  dbenv = txnp->mgrp->dbenv;
  memset (&xid, 0, sizeof (xid));
  xid.data = td->xid;
  xid.size = sizeof (td->xid);
  if (F_ISSET (dbenv, DB_ENV_LOGGING) &&
      (ret = CDB___txn_xa_regop_log (dbenv, txnp, &txnp->last_lsn,
                                     (F_ISSET (dbenv, DB_ENV_TXN_NOSYNC) &&
                                      !F_ISSET (txnp, TXN_SYNC)) ||
                                     F_ISSET (txnp,
                                              TXN_NOSYNC) ? 0 : DB_FLUSH,
                                     TXN_PREPARE, &xid, td->format, td->gtrid,
                                     td->bqual)) != 0)
  {
    CDB___db_err (dbenv,
                  "CDB_txn_prepare: log_write failed %s\n",
                  CDB_db_strerror (ret));
    return (ret);
  }

  MUTEX_THREAD_LOCK (txnp->mgrp->mutexp);
  td->status = TXN_PREPARED;
  MUTEX_THREAD_UNLOCK (txnp->mgrp->mutexp);
  return (ret);
}

/*
 * Return the transaction ID associated with a particular transaction
 */
u_int32_t
CDB_txn_id (txnp)
     DB_TXN *txnp;
{
  return (txnp->txnid);
}

/* Internal routines. */

/*
 * Return 0 if the txnp is reasonable, otherwise returns EINVAL.
 */
static int
CDB___txn_check_running (txnp, tdp)
     const DB_TXN *txnp;
     TXN_DETAIL **tdp;
{
  DB_TXNMGR *mgrp;
  TXN_DETAIL *tp;

  tp = NULL;
  mgrp = txnp->mgrp;
  if (txnp != NULL && mgrp != NULL && mgrp->reginfo.primary != NULL)
  {
    tp = (TXN_DETAIL *) R_ADDR (&mgrp->reginfo, txnp->off);
    /*
     * Child transactions could be marked committed which is OK.
     */
    if (tp->status != TXN_RUNNING &&
        tp->status != TXN_PREPARED && tp->status != TXN_COMMITTED)
      tp = NULL;
    if (tdp != NULL)
      *tdp = tp;
  }

  return (tp == NULL ? EINVAL : 0);
}

/*
 * CDB___txn_end --
 *  Internal transaction end routine.
 *
 * PUBLIC: int CDB___txn_end __P((DB_TXN *, int));
 */
int
CDB___txn_end (txnp, is_commit)
     DB_TXN *txnp;
     int is_commit;
{
  DB_ENV *dbenv;
  DB_LOCKREQ request;
  DB_TXN *kids;
  DB_TXNMGR *mgr;
  DB_TXNREGION *region;
  TXN_DETAIL *tp;
  int ret;

  mgr = txnp->mgrp;
  dbenv = mgr->dbenv;
  region = mgr->reginfo.primary;

  /*
   * On aborts, we've undone the children, but we still need
   * to free the up.
   */
  if (!is_commit)
  {
    while ((kids = TAILQ_FIRST (&txnp->kids)) != NULL)
      if ((ret = CDB___txn_end (kids, is_commit)) != 0)
        return (DB_RUNRECOVERY);
  }

  /* Release the locks. */
  request.op = txnp->parent == NULL ||
    is_commit == 0 ? DB_LOCK_PUT_ALL : DB_LOCK_INHERIT;

  if (F_ISSET (dbenv, DB_ENV_LOCKING))
  {
    ret = CDB_lock_vec (dbenv, txnp->txnid, 0, &request, 1, NULL);
    if (ret != 0 && (ret != DB_LOCK_DEADLOCK || is_commit))
    {
      CDB___db_err (dbenv, "%s: release locks failed %s",
                    is_commit ? "CDB_txn_commit" : "CDB_txn_abort",
                    CDB_db_strerror (ret));
      return (ret);
    }
  }

  /* End the transaction. */
  R_LOCK (dbenv, &mgr->reginfo);

  /*
   * Child transactions that are committing cannot be released until
   * the parent commits, since the parent may abort, causing the child
   * to abort as well.
   */
  tp = (TXN_DETAIL *) R_ADDR (&mgr->reginfo, txnp->off);
  if (txnp->parent == NULL || !is_commit)
  {
    SH_TAILQ_REMOVE (&region->active_txn, tp, links, __txn_detail);

    CDB___db_shalloc_free (mgr->reginfo.addr, tp);
  }
  else
  {
    tp->status = TXN_COMMITTED;
    F_SET (txnp, TXN_CHILDCOMMIT);
  }

  if (is_commit)
    region->ncommits++;
  else
    region->naborts++;
  --region->nactive;

  R_UNLOCK (dbenv, &mgr->reginfo);

  /*
   * If the transaction aborted, we can remove it from its parent links.
   * If it committed, then we need to leave it on, since the parent can
   * still abort.
   * The transaction cannot get more locks, remove its locker info.
   */
  if (txnp->parent != NULL)
  {
    if (F_ISSET (dbenv, DB_ENV_LOCKING | DB_ENV_CDB))
      CDB___lock_freefamilylocker (dbenv->lk_handle, txnp->txnid);
    if (!is_commit)
      TAILQ_REMOVE (&txnp->parent->kids, txnp, klinks);
  }

  /* Free the space. */
  if (F_ISSET (txnp, TXN_MALLOC) && (txnp->parent == NULL || !is_commit))
  {
    MUTEX_THREAD_LOCK (mgr->mutexp);
    TAILQ_REMOVE (&mgr->txn_chain, txnp, links);
    MUTEX_THREAD_UNLOCK (mgr->mutexp);

    CDB___os_free (txnp, sizeof (*txnp));
  }

  return (0);
}


/*
 * CDB___txn_undo --
 *  Undo the transaction with id txnid.  Returns 0 on success and
 *  errno on failure.
 */
static int
CDB___txn_undo (txnp)
     DB_TXN *txnp;
{
  DBT rdbt;
  DB_ENV *dbenv;
  DB_LSN *lsn_array, *key_lsnp;
  DB_TXNMGR *mgr;
  int ntxns, ret, threaded;

  mgr = txnp->mgrp;
  dbenv = mgr->dbenv;
  lsn_array = NULL;

  if (!F_ISSET (dbenv, DB_ENV_LOGGING))
    return (0);

  /*
   * This is the simplest way to code this, but if the mallocs during
   * recovery turn out to be a performance issue, we can do the
   * allocation here and use DB_DBT_USERMEM.
   */
  memset (&rdbt, 0, sizeof (rdbt));
  threaded = F_ISSET (dbenv, DB_ENV_THREAD) ? 1 : 0;
  if (threaded)
    F_SET (&rdbt, DB_DBT_MALLOC);

  key_lsnp = &txnp->last_lsn;

  if (TAILQ_FIRST (&txnp->kids) != NULL)
  {
    if ((ret = CDB___txn_makefamily (txnp, &ntxns, &lsn_array)) != 0)
      return (ret);
    key_lsnp = &lsn_array[0];
  }

  for (ret = 0; ret == 0 && !IS_ZERO_LSN (*key_lsnp);)
  {
    /*
     * The dispatch routine returns the lsn of the record
     * before the current one in the key_lsnp argument.
     */
    if ((ret = CDB_log_get (dbenv, key_lsnp, &rdbt, DB_SET)) == 0)
    {
      ret = mgr->recover (dbenv, &rdbt, key_lsnp, TXN_UNDO, NULL);
      if (threaded && rdbt.data != NULL)
      {
        CDB___os_free (rdbt.data, rdbt.size);
        rdbt.data = NULL;
      }
      if (lsn_array != NULL)
        TXN_BUBBLE (lsn_array, ntxns);
    }
    if (ret != 0)
      return (ret);
  }

  return (ret);
}

/*
 * Transaction checkpoint.
 * If either kbytes or minutes is non-zero, then we only take the checkpoint
 * more than "minutes" minutes have passed since the last checkpoint or if
 * more than "kbytes" of log data have been written since the last checkpoint.
 * When taking a checkpoint, find the oldest active transaction and figure out
 * its first LSN.  This is the lowest LSN we can checkpoint, since any record
 * written after since that point may be involved in a transaction and may
 * therefore need to be undone in the case of an abort.
 */
int
CDB_txn_checkpoint (dbenv, kbytes, minutes)
     DB_ENV *dbenv;
     u_int32_t kbytes, minutes;
{
  DB_LOG *dblp;
  DB_LSN ckp_lsn, sync_lsn, last_ckp;
  DB_TXNMGR *mgr;
  DB_TXNREGION *region;
  LOG *lp;
  TXN_DETAIL *txnp;
  time_t last_ckp_time, now;
  u_int32_t kbytes_written;
  int ret;

  PANIC_CHECK (dbenv);
  ENV_REQUIRES_CONFIG (dbenv, dbenv->tx_handle, DB_INIT_TXN);

  mgr = dbenv->tx_handle;
  region = mgr->reginfo.primary;
  dblp = dbenv->lg_handle;
  lp = dblp->reginfo.primary;

  /*
   * Check if we need to run recovery.
   */
  ZERO_LSN (ckp_lsn);
  if (minutes != 0)
  {
    (void) time (&now);

    R_LOCK (dbenv, &mgr->reginfo);
    last_ckp_time = region->time_ckp;
    R_UNLOCK (dbenv, &mgr->reginfo);

    if (now - last_ckp_time >= (time_t) (minutes * 60))
      goto do_ckp;
  }

  if (kbytes != 0)
  {
    R_LOCK (dbenv, &dblp->reginfo);
    kbytes_written =
      lp->stat.st_wc_mbytes * 1024 + lp->stat.st_wc_bytes / 1024;
    ckp_lsn = lp->lsn;
    R_UNLOCK (dbenv, &dblp->reginfo);
    if (kbytes_written >= (u_int32_t) kbytes)
      goto do_ckp;
  }

  /*
   * If we checked time and data and didn't go to checkpoint,
   * we're done.
   */
  if (minutes != 0 || kbytes != 0)
    return (0);

do_ckp:
  if (IS_ZERO_LSN (ckp_lsn))
  {
    R_LOCK (dbenv, &dblp->reginfo);
    ckp_lsn = lp->lsn;
    R_UNLOCK (dbenv, &dblp->reginfo);
  }

  /*
   * We have to find an LSN such that all transactions begun
   * before that LSN are complete.
   */
  R_LOCK (dbenv, &mgr->reginfo);

  if (IS_ZERO_LSN (region->pending_ckp))
  {
    for (txnp =
         SH_TAILQ_FIRST (&region->active_txn, __txn_detail);
         txnp != NULL; txnp = SH_TAILQ_NEXT (txnp, links, __txn_detail))
    {

      /*
       * Look through the active transactions for the
       * lowest begin lsn.
       */
      if (!IS_ZERO_LSN (txnp->begin_lsn) &&
          CDB_log_compare (&txnp->begin_lsn, &ckp_lsn) < 0)
        ckp_lsn = txnp->begin_lsn;
    }
    region->pending_ckp = ckp_lsn;
  }
  else
    ckp_lsn = region->pending_ckp;

  R_UNLOCK (dbenv, &mgr->reginfo);

  /*
   * CDB_memp_sync may change the lsn you pass it, so don't pass it
   * the actual ckp_lsn, pass it a temp instead.
   */
  sync_lsn = ckp_lsn;
  if (mgr->dbenv->mp_handle != NULL &&
      (ret = CDB_memp_sync (mgr->dbenv, &sync_lsn)) != 0)
  {
    /*
     * ret == DB_INCOMPLETE means that there are still buffers to
     * flush, the checkpoint is not complete.  Wait and try again.
     */
    if (ret > 0)
      CDB___db_err (mgr->dbenv,
                    "CDB_txn_checkpoint: system failure in CDB_memp_sync %s\n",
                    CDB_db_strerror (ret));
    return (ret);
  }
  if (F_ISSET (mgr->dbenv, DB_ENV_LOGGING))
  {
    R_LOCK (dbenv, &mgr->reginfo);
    last_ckp = region->last_ckp;
    ZERO_LSN (region->pending_ckp);
    R_UNLOCK (dbenv, &mgr->reginfo);

    if ((ret = CDB___txn_ckp_log (mgr->dbenv,
                                  NULL, &ckp_lsn, DB_CHECKPOINT, &ckp_lsn,
                                  &last_ckp)) != 0)
    {
      CDB___db_err (mgr->dbenv,
                    "CDB_txn_checkpoint: log failed at LSN [%ld %ld] %s\n",
                    (long) ckp_lsn.file, (long) ckp_lsn.offset,
                    CDB_db_strerror (ret));
      return (ret);
    }

    R_LOCK (dbenv, &mgr->reginfo);
    region->last_ckp = ckp_lsn;
    (void) time (&region->time_ckp);
    R_UNLOCK (dbenv, &mgr->reginfo);
  }
  return (0);
}

static void
CDB___txn_freekids (txnp)
     DB_TXN *txnp;
{
  DB_ENV *dbenv;
  DB_TXN *kids;
  DB_TXNMGR *mgr;
  DB_TXNREGION *region;
  TXN_DETAIL *tp;

  mgr = txnp->mgrp;
  dbenv = mgr->dbenv;
  region = mgr->reginfo.primary;

  for (kids = TAILQ_FIRST (&txnp->kids);
       kids != NULL; kids = TAILQ_FIRST (&txnp->kids))
  {
    /* Free any children of this transaction. */
    CDB___txn_freekids (kids);

    /* Free the transaction detail in the region. */
    R_LOCK (dbenv, &mgr->reginfo);
    tp = (TXN_DETAIL *) R_ADDR (&mgr->reginfo, kids->off);
    SH_TAILQ_REMOVE (&region->active_txn, tp, links, __txn_detail);

    CDB___db_shalloc_free (mgr->reginfo.addr, tp);
    R_UNLOCK (dbenv, &mgr->reginfo);

    /* Now remove from its parent. */
    TAILQ_REMOVE (&txnp->kids, kids, klinks);
    if (F_ISSET (txnp, TXN_MALLOC))
    {
      MUTEX_THREAD_LOCK (mgr->mutexp);
      TAILQ_REMOVE (&mgr->txn_chain, kids, links);
      MUTEX_THREAD_UNLOCK (mgr->mutexp);
      CDB___os_free (kids, sizeof (*kids));
    }
  }
}

/*
 * CDB___txn_is_ancestor --
 *   Determine if a transaction is an ancestor of another transaction.
 * This is used during lock promotion when we do not have the per-process
 * data structures that link parents together.  Instead, we'll have to
 * follow the links in the transaction region.
 *
 * PUBLIC: int CDB___txn_is_ancestor __P((DB_ENV *, size_t, size_t));
 */
int
CDB___txn_is_ancestor (dbenv, hold_off, req_off)
     DB_ENV *dbenv;
     size_t hold_off, req_off;
{
  DB_TXNMGR *mgr;
  TXN_DETAIL *hold_tp, *req_tp;

  mgr = dbenv->tx_handle;
  hold_tp = (TXN_DETAIL *) R_ADDR (&mgr->reginfo, hold_off);
  req_tp = (TXN_DETAIL *) R_ADDR (&mgr->reginfo, req_off);

  while (req_tp->parent != INVALID_ROFF)
  {
    req_tp = (TXN_DETAIL *) R_ADDR (&mgr->reginfo, req_tp->parent);
    if (req_tp->txnid == hold_tp->txnid)
      return (1);
  }

  return (0);
}

/*
 * CDB___txn_makefamily --
 *  Create an array of DB_LSNs for every member of the family being
 * aborted so that we can undo the records in the appropriate order.  We
 * allocate memory here and expect our caller to free it when they're done.
 */
static int
CDB___txn_makefamily (txnp, np, arrayp)
     DB_TXN *txnp;
     int *np;
     DB_LSN **arrayp;
{
  DB_LSN *ap, *tmpp;
  int i, ret;

  /* Figure out how many we have. */
  *np = CDB___txn_count (txnp);

  /* Malloc space. */
  if ((ret = CDB___os_malloc (*np * sizeof (DB_LSN), NULL, arrayp)) != 0)
    return (ret);

  /* Fill in the space. */
  tmpp = *arrayp;
  CDB___txn_lsn (txnp, &tmpp);

  /* Sort the LSNs. */
  ap = *arrayp;
  for (i = 0; i < *np; i++)
    TXN_BUBBLE (ap, *np - i);

  return (0);
}

/*
 * CDB___txn_count --
 *  Routine to count the number of members in a transaction family.  We
 * include the incoming transaction in the count.  We assume that we never
 * call this routine with NULL.
 */
static int
CDB___txn_count (txnp)
     DB_TXN *txnp;
{
  DB_TXN *kids;
  int n;

  n = 1;
  for (kids = TAILQ_FIRST (&txnp->kids);
       kids != NULL; kids = TAILQ_NEXT (kids, klinks))
    n += CDB___txn_count (kids);

  return (n);
}

/*
 * CDB___txn_lsn ---
 *  Fill in the array with the last_lsn field of every transaction
 * in the family.  Array is an in/out parameter that leaves you pointing
 * to the next space in which to place an LSN.
 */
static void
CDB___txn_lsn (txnp, array)
     DB_TXN *txnp;
     DB_LSN **array;
{
  DB_LSN *lsn;
  DB_TXN *kids;

  lsn = *array;
  lsn[0] = txnp->last_lsn;
  *array = &lsn[1];

  for (kids = TAILQ_FIRST (&txnp->kids);
       kids != NULL; kids = TAILQ_NEXT (kids, klinks))
    CDB___txn_lsn (kids, array);
}

/*
 * CDB___txn_activekids --
 *  Determine if this transaction has any active children.  Returns 1
 * if any active children are present; 0 otherwise.
 *
 * PUBLIC: int CDB___txn_activekids __P((DB_TXN *));
 */
int
CDB___txn_activekids (txnp)
     DB_TXN *txnp;
{
  DB_TXN *kids;

  for (kids = TAILQ_FIRST (&txnp->kids);
       kids != NULL; kids = TAILQ_NEXT (kids, klinks))
    if (!F_ISSET (kids, TXN_CHILDCOMMIT))
      return (1);
  return (0);
}
