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
static const char sccsid[] = "@(#)db_dispatch.c  11.7 (Sleepycat) 9/9/99";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "db_int.h"
#include "db_page.h"
#include "db_dispatch.h"
#include "db_am.h"
#include "log_auto.h"
#include "txn.h"
#include "txn_auto.h"
#include "log.h"

/*
 * CDB___db_dispatch --
 *
 * This is the transaction dispatch function used by the db access methods.
 * It is designed to handle the record format used by all the access
 * methods (the one automatically generated by the db_{h,log,read}.sh
 * scripts in the tools directory).  An application using a different
 * recovery paradigm will supply a different dispatch function to txn_open.
 *
 * PUBLIC: int CDB___db_dispatch __P((DB_ENV *, DBT *, DB_LSN *, int, void *));
 */
int
CDB___db_dispatch (dbenv, db, lsnp, redo, info)
     DB_ENV *dbenv;             /* The environment. */
     DBT *db;                   /* The log record upon which to dispatch. */
     DB_LSN *lsnp;              /* The lsn of the record being dispatched. */
     int redo;                  /* Redo this op (or undo it). */
     void *info;
{
  u_int32_t rectype, txnid;

  memcpy (&rectype, db->data, sizeof (rectype));
  memcpy (&txnid, (u_int8_t *) db->data + sizeof (rectype), sizeof (txnid));

  switch (redo)
  {
  case TXN_REDO:
  case TXN_UNDO:
    return ((dbenv->dtab[rectype]) (dbenv, db, lsnp, redo, info));
  case TXN_OPENFILES:
    if (rectype < DB_txn_BEGIN)
      return ((dbenv->dtab[rectype]) (dbenv, db, lsnp, redo, info));
    break;
  case TXN_BACKWARD_ROLL:
    /*
     * Running full recovery in the backward pass.  If we've
     * seen this txnid before and added to it our commit list,
     * then we do nothing during this pass.  If we've never
     * seen it, then we call the appropriate recovery routine
     * in "abort mode".
     *
     * We need to always undo DB_db_noop records, so that we
     * properly handle any aborts before the file was closed.
     */
    if (rectype == DB_log_register || rectype == DB_txn_ckp ||
        rectype == DB_db_noop ||
        (CDB___db_txnlist_find (info, txnid) == DB_NOTFOUND && txnid != 0))
      return ((dbenv->dtab[rectype]) (dbenv, db, lsnp, TXN_UNDO, info));
    break;
  case TXN_FORWARD_ROLL:
    /*
     * In the forward pass, if we haven't seen the transaction,
     * do nothing, else recovery it.
     *
     * We need to always redo DB_db_noop records, so that we
     * properly handle any commits after the file was closed.
     */
    if (rectype == DB_log_register || rectype == DB_txn_ckp ||
        rectype == DB_db_noop ||
        CDB___db_txnlist_find (info, txnid) != DB_NOTFOUND)
      return ((dbenv->dtab[rectype]) (dbenv, db, lsnp, TXN_REDO, info));
    break;
  default:
    abort ();
  }
  return (0);
}

/*
 * CDB___db_add_recovery --
 *
 * PUBLIC: int CDB___db_add_recovery __P((DB_ENV *,
 * PUBLIC:    int (*)(DB_ENV *, DBT *, DB_LSN *, int, void *), u_int32_t));
 */
int
CDB___db_add_recovery (dbenv, func, ndx)
     DB_ENV *dbenv;
     int (*func) __P ((DB_ENV *, DBT *, DB_LSN *, int, void *));
     u_int32_t ndx;
{
  u_int32_t i;
  int ret;

  /* Check if we have to grow the table. */
  if (ndx >= dbenv->dtab_size)
  {
    if ((ret = CDB___os_realloc ((DB_user_BEGIN +
                                  dbenv->dtab_size) * sizeof (dbenv->dtab[0]),
                                 NULL, &dbenv->dtab)) != 0)
      return (ret);
    for (i = dbenv->dtab_size,
         dbenv->dtab_size += DB_user_BEGIN; i < dbenv->dtab_size; ++i)
      dbenv->dtab[i] = NULL;
  }

  dbenv->dtab[ndx] = func;
  return (0);
}

/*
 * CDB___db_txnlist_init --
 *  Initialize transaction linked list.
 *
 * PUBLIC: int CDB___db_txnlist_init __P((void *));
 */
int
CDB___db_txnlist_init (retp)
     void *retp;
{
  DB_TXNHEAD *headp;
  int ret;

  if ((ret = CDB___os_malloc (sizeof (DB_TXNHEAD), NULL, &headp)) != 0)
    return (ret);

  LIST_INIT (&headp->head);
  headp->maxid = 0;
  headp->generation = 1;

  *(void **) retp = headp;
  return (0);
}

/*
 * CDB___db_txnlist_add --
 *  Add an element to our transaction linked list.
 *
 * PUBLIC: int CDB___db_txnlist_add __P((void *, u_int32_t));
 */
int
CDB___db_txnlist_add (listp, txnid)
     void *listp;
     u_int32_t txnid;
{
  DB_TXNHEAD *hp;
  DB_TXNLIST *elp;
  int ret;

  if ((ret = CDB___os_malloc (sizeof (DB_TXNLIST), NULL, &elp)) != 0)
    return (ret);

  hp = (DB_TXNHEAD *) listp;
  LIST_INSERT_HEAD (&hp->head, elp, links);

  elp->type = TXNLIST_TXNID;
  elp->u.t.txnid = txnid;
  if (txnid > hp->maxid)
    hp->maxid = txnid;
  elp->u.t.generation = hp->generation;

  return (0);
}

/* CDB___db_txnlist_close --
 *
 *   Call this when we close a file.  It allows us to reconcile whether
 * we have done any operations on this file with whether the file appears
 * to have been deleted.  If you never do any operations on a file, then
 * we assume it's OK to appear deleted.
 *
 * PUBLIC: int CDB___db_txnlist_close __P((void *, u_int32_t, u_int32_t));
 */

int
CDB___db_txnlist_close (listp, lid, count)
     void *listp;
     u_int32_t lid;
     u_int32_t count;
{
  DB_TXNHEAD *hp;
  DB_TXNLIST *p;

  hp = (DB_TXNHEAD *) listp;
  for (p = hp->head.lh_first; p != NULL; p = p->links.le_next)
  {
    if (p->type == TXNLIST_DELETE)
      if (lid == p->u.d.fileid && !F_ISSET (&p->u.d, TXNLIST_FLAG_CLOSED))
      {
        p->u.d.count += count;
        return (0);
      }
  }

  return (0);
}

/*
 * CDB___db_txnlist_delete --
 *
 *  Record that a file was missing or deleted.  If the deleted
 * flag is set, then we've encountered a delete of a file, else we've
 * just encountered a file that is missing.  The lid is the log fileid
 * and is only meaningful if deleted is not equal to 0.
 *
 * PUBLIC: int CDB___db_txnlist_delete __P((void *, char *, u_int32_t, int));
 */
int
CDB___db_txnlist_delete (listp, name, lid, deleted)
     void *listp;
     char *name;
     u_int32_t lid;
     int deleted;
{
  DB_TXNHEAD *hp;
  DB_TXNLIST *p;
  int ret;

  hp = (DB_TXNHEAD *) listp;
  for (p = hp->head.lh_first; p != NULL; p = p->links.le_next)
  {
    if (p->type == TXNLIST_DELETE)
      if (strcmp (name, p->u.d.fname) == 0)
      {
        if (deleted)
          F_SET (&p->u.d, TXNLIST_FLAG_DELETED);
        else
          F_CLR (&p->u.d, TXNLIST_FLAG_CLOSED);
        return (0);
      }
  }

  /* Need to add it. */
  if ((ret = CDB___os_malloc (sizeof (DB_TXNLIST), NULL, &p)) != 0)
    return (ret);
  LIST_INSERT_HEAD (&hp->head, p, links);

  p->type = TXNLIST_DELETE;
  p->u.d.flags = 0;
  if (deleted)
    F_SET (&p->u.d, TXNLIST_FLAG_DELETED);
  p->u.d.fileid = lid;
  p->u.d.count = 0;
  ret = CDB___os_strdup (name, &p->u.d.fname);

  return (ret);
}

/*
 * CDB___db_txnlist_end --
 *  Discard transaction linked list. Print out any error messages
 * for deleted files.
 *
 * PUBLIC: void CDB___db_txnlist_end __P((DB_ENV *, void *));
 */
void
CDB___db_txnlist_end (dbenv, listp)
     DB_ENV *dbenv;
     void *listp;
{
  DB_TXNHEAD *hp;
  DB_TXNLIST *p;
  DB_LOG *lp;

  hp = (DB_TXNHEAD *) listp;
  lp = (DB_LOG *) dbenv->lg_handle;
  while (hp != NULL && (p = LIST_FIRST (&hp->head)) != LIST_END (&hp->head))
  {
    LIST_REMOVE (p, links);
    if (p->type == TXNLIST_DELETE)
    {
      /*
       * If we have a file that is not deleted and has
       * some operations, we flag the warning.  Since
       * the file could still be open, we need to check
       * the actual log table as well.
       */
      if ((!F_ISSET (&p->u.d, TXNLIST_FLAG_DELETED) &&
           p->u.d.count != 0) ||
          (!F_ISSET (&p->u.d, TXNLIST_FLAG_CLOSED) &&
           p->u.d.fileid < lp->dbentry_cnt &&
           lp->dbentry[p->u.d.fileid].count != 0))
        CDB___db_err (dbenv, "warning: %s: %s",
                      p->u.d.fname, CDB_db_strerror (ENOENT));
      CDB___os_freestr (p->u.d.fname);
    }
    CDB___os_free (p, sizeof (DB_TXNLIST));
  }
  CDB___os_free (listp, sizeof (DB_TXNHEAD));
}

/*
 * CDB___db_txnlist_find --
 *  Checks to see if a txnid with the current generation is in the
 *  txnid list.
 *
 * PUBLIC: int CDB___db_txnlist_find __P((void *, u_int32_t));
 */
int
CDB___db_txnlist_find (listp, txnid)
     void *listp;
     u_int32_t txnid;
{
  DB_TXNHEAD *hp;
  DB_TXNLIST *p;

  if ((hp = (DB_TXNHEAD *) listp) == NULL)
    return (DB_NOTFOUND);

  for (p = hp->head.lh_first; p != NULL; p = p->links.le_next)
  {
    if (p->type != TXNLIST_TXNID)
      continue;
    if (p->u.t.txnid == txnid && hp->generation == p->u.t.generation)
      return (0);
  }

  return (DB_NOTFOUND);
}

/*
 * CDB___db_txnlist_gen --
 *  Change the current generation number.
 *
 * PUBLIC: void CDB___db_txnlist_gen __P((void *, int));
 */
void
CDB___db_txnlist_gen (listp, incr)
     void *listp;
     int incr;
{
  DB_TXNHEAD *hp;

  /*
   * During recovery generation numbers keep track of how many "restart"
   * checkpoints we've seen.  Restart checkpoints occur whenever we take
   * a checkpoint and there are no outstanding transactions.  When that
   * happens, we can reset transaction IDs back to 1.  It always happens
   * at recovery and it prevents us from exhausting the transaction IDs
   * name space.
   */
  hp = (DB_TXNHEAD *) listp;
  hp->generation += incr;
}

#ifdef DEBUG
/*
 * CDB___db_txnlist_print --
 *  Print out the transaction list.
 *
 * PUBLIC: void CDB___db_txnlist_print __P((void *));
 */
void
CDB___db_txnlist_print (listp)
     void *listp;
{
  DB_TXNHEAD *hp;
  DB_TXNLIST *p;

  hp = (DB_TXNHEAD *) listp;

  printf ("Maxid: %lu Generation: %lu\n",
          (u_long) hp->maxid, (u_long) hp->generation);
  for (p = hp->head.lh_first; p != NULL; p = p->links.le_next)
  {
    switch (p->type)
    {
    case TXNLIST_TXNID:
      printf ("TXNID: %lu(%lu)\n",
              (u_long) p->u.t.txnid, (u_long) p->u.t.generation);
      break;
    case TXNLIST_DELETE:
      printf ("FILE: %s id=%d ops=%d %s %s\n",
              p->u.d.fname, p->u.d.fileid, p->u.d.count,
              F_ISSET (&p->u.d, TXNLIST_FLAG_DELETED) ?
              "(deleted)" : "(missing)",
              F_ISSET (&p->u.d, TXNLIST_FLAG_CLOSED) ? "(closed)" : "(open)");

      break;
    default:
      printf ("Unrecognized type: %d\n", p->type);
      break;
    }
  }
}
#endif
