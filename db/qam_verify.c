/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1999, 2000
 *  Sleepycat Software.  All rights reserved.
 */

#include "htconfig.h"

#ifndef lint
static const char revid[] =
  "$Id: qam_verify.c,v 1.2 2002/02/02 18:18:05 ghutchis Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#endif

#include "db_int.h"
#include "db_page.h"
#include "db_verify.h"
#include "qam.h"
#include "db_ext.h"

/*
 * CDB___qam_vrfy_meta --
 *  Verify the queue-specific part of a metadata page.
 *
 * PUBLIC: int CDB___qam_vrfy_meta __P((DB *, VRFY_DBINFO *, QMETA *,
 * PUBLIC:     db_pgno_t, u_int32_t));
 */
int
CDB___qam_vrfy_meta (dbp, vdp, meta, pgno, flags)
     DB *dbp;
     VRFY_DBINFO *vdp;
     QMETA *meta;
     db_pgno_t pgno;
     u_int32_t flags;
{
  VRFY_PAGEINFO *pip;
  int isbad, ret, t_ret;

  if ((ret = CDB___db_vrfy_getpageinfo (vdp, pgno, &pip)) != 0)
    return (ret);
  isbad = 0;

  /*
   * Queue can't be used in subdatabases, so if this isn't set
   * something very odd is going on.
   */
  if (!F_ISSET (pip, VRFY_INCOMPLETE))
    EPRINT ((dbp->dbenv, "Queue databases must be one-per-file."));

  /* start */
  if (meta->start != 1)
  {
    EPRINT ((dbp->dbenv, "Queue start offset of %lu", meta->start));
    isbad = 1;
  }

  /* first_recno, cur_recno */
  if (meta->cur_recno < meta->first_recno)
  {
    EPRINT ((dbp->dbenv,
             "Wrongly ordered first/current recnos, %lu/%lu",
             meta->first_recno, meta->cur_recno));
    isbad = 1;
  }

  /* cur_recno/rec_page */
  if (vdp->last_pgno > 0 &&
      (1 + ((meta->cur_recno - meta->start) / meta->rec_page)) !=
      vdp->last_pgno)
  {
    EPRINT ((dbp->dbenv, "Incorrect last page number in queue database"));
    isbad = 1;
  }

  /*
   * re_len:  If this is bad, we can't safely verify queue data pages, so
   * return DB_VERIFY_FATAL
   */
  if (ALIGN (meta->re_len + sizeof (QAMDATA) - 1, sizeof (u_int32_t)) *
      meta->rec_page + sizeof (QPAGE) > dbp->pgsize)
  {
    EPRINT ((dbp->dbenv,
             "Queue record length %lu impossibly high for page size and records per page",
             meta->re_len));
    ret = DB_VERIFY_FATAL;
    goto err;
  }
  else
  {
    vdp->re_len = meta->re_len;
    vdp->rec_page = meta->rec_page;
  }

err:if ((t_ret = CDB___db_vrfy_putpageinfo (vdp, pip)) != 0 && ret == 0)
    ret = t_ret;
  return (ret == 0 && isbad == 1 ? DB_VERIFY_BAD : ret);
}

/*
 * CDB___qam_vrfy_data --
 *  Verify a queue data page.
 *
 * PUBLIC: int CDB___qam_vrfy_data __P((DB *, VRFY_DBINFO *, QPAGE *,
 * PUBLIC:     db_pgno_t, u_int32_t));
 */
int
CDB___qam_vrfy_data (dbp, vdp, h, pgno, flags)
     DB *dbp;
     VRFY_DBINFO *vdp;
     QPAGE *h;
     db_pgno_t pgno;
     u_int32_t flags;
{
  DB fakedb;
  struct __queue fakeq;
  QAMDATA *qp;
  db_recno_t i;
  u_int8_t qflags;

  /*
   * Not much to do here, except make sure that flags are reasonable.
   *
   * QAM_GET_RECORD assumes a properly initialized q_internal
   * structure, however, and we don't have one, so we play
   * some gross games to fake it out.
   */
  fakedb.q_internal = &fakeq;
  fakeq.re_len = vdp->re_len;

  for (i = 0; i < vdp->rec_page; i++)
  {
    qp = QAM_GET_RECORD (&fakedb, h, i);
    if ((u_int8_t *) qp >= (u_int8_t *) h + dbp->pgsize)
    {
      EPRINT ((dbp->dbenv,
               "Queue record %lu extends past end of page %lu", i, pgno));
      return (DB_VERIFY_BAD);
    }

    qflags = qp->flags;
    qflags &= !(QAM_VALID | QAM_SET);
    if (qflags != 0)
    {
      EPRINT ((dbp->dbenv,
               "Queue record %lu on page %lu has bad flags", i, pgno));
      return (DB_VERIFY_BAD);
    }
  }

  return (0);
}

/*
 * CDB___qam_vrfy_structure --
 *  Verify a queue database structure, such as it is.
 *
 * PUBLIC: int CDB___qam_vrfy_structure __P((DB *, VRFY_DBINFO *, u_int32_t));
 */
int
CDB___qam_vrfy_structure (dbp, vdp, flags)
     DB *dbp;
     VRFY_DBINFO *vdp;
     u_int32_t flags;
{
  VRFY_PAGEINFO *pip;
  db_pgno_t i;
  int ret, isbad;

  isbad = 0;

  if ((ret = CDB___db_vrfy_getpageinfo (vdp, PGNO_BASE_MD, &pip)) != 0)
    return (ret);

  if (pip->type != P_QAMMETA)
  {
    EPRINT ((dbp->dbenv, "Queue database has no meta page"));
    isbad = 1;
    goto err;
  }

  if ((ret = CDB___db_vrfy_pgset_inc (vdp->pgset, 0)) != 0)
    goto err;

  for (i = 1; i <= vdp->last_pgno; i++)
  {
    if ((ret = CDB___db_vrfy_putpageinfo (vdp, pip)) != 0 ||
        (ret = CDB___db_vrfy_getpageinfo (vdp, i, &pip)) != 0)
      return (ret);
    if (!F_ISSET (pip, VRFY_IS_ALLZEROES) && pip->type != P_QAMDATA)
    {
      EPRINT ((dbp->dbenv,
               "Queue database page %lu of incorrect type %lu",
               i, pip->type));
      isbad = 1;
      goto err;
    }
    else if ((ret = CDB___db_vrfy_pgset_inc (vdp->pgset, i)) != 0)
      goto err;
  }

err:if ((ret = CDB___db_vrfy_putpageinfo (vdp, pip)) != 0)
    return (ret);
  return (isbad == 1 ? DB_VERIFY_BAD : 0);
}
