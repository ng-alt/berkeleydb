/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996-2001
 *	Sleepycat Software.  All rights reserved.
 */
#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: mp_trickle.c,v 11.18 2001/07/24 18:31:31 bostic Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <stdlib.h>
#endif

#include "db_int.h"
#include "db_shash.h"
#include "mp.h"

static int __memp_trick __P((DB_ENV *, int, int, int *));

/*
 * __memp_trickle --
 *	Keep a specified percentage of the buffers clean.
 *
 * PUBLIC: int __memp_trickle __P((DB_ENV *, int, int *));
 */
int
__memp_trickle(dbenv, pct, nwrotep)
	DB_ENV *dbenv;
	int pct, *nwrotep;
{
	DB_MPOOL *dbmp;
	MPOOL *mp;
	u_int32_t i;
	int ret;

	PANIC_CHECK(dbenv);
	ENV_REQUIRES_CONFIG(dbenv,
	    dbenv->mp_handle, "memp_trickle", DB_INIT_MPOOL);

	dbmp = dbenv->mp_handle;
	mp = dbmp->reginfo[0].primary;

	if (nwrotep != NULL)
		*nwrotep = 0;

	if (pct < 1 || pct > 100)
		return (EINVAL);

	R_LOCK(dbenv, dbmp->reginfo);

	/* Loop through the caches... */
	for (ret = 0, i = 0; i < mp->nreg; ++i)
		if ((ret = __memp_trick(dbenv, i, pct, nwrotep)) != 0)
			break;

	R_UNLOCK(dbenv, dbmp->reginfo);
	return (ret);
}

/*
 * __memp_trick --
 *	Trickle a single cache.
 */
static int
__memp_trick(dbenv, ncache, pct, nwrotep)
	DB_ENV *dbenv;
	int ncache, pct, *nwrotep;
{
	BH *bhp;
	DB_MPOOL *dbmp;
	MPOOL *c_mp;
	MPOOLFILE *mfp;
	db_pgno_t pgno;
	u_long total;
	int nwrote, ret, t_ret, wrote;

	dbmp = dbenv->mp_handle;
	c_mp = dbmp->reginfo[ncache].primary;
	nwrote = 0;
	ret = 0;

	/*
	 * If there are sufficient clean buffers, or no buffers or no dirty
	 * buffers, we're done.
	 *
	 * XXX
	 * Using st_page_clean and st_page_dirty is our only choice at the
	 * moment, but it's not as correct as we might like in the presence
	 * of pools with more than one buffer size, as a free 512-byte buffer
	 * isn't the same as a free 8K buffer.
	 */
loop:	total = c_mp->stat.st_page_clean + c_mp->stat.st_page_dirty;
	if (total == 0 || c_mp->stat.st_page_dirty == 0 ||
	    (c_mp->stat.st_page_clean * 100) / total >= (u_long)pct)
		goto done;

	/* Loop until we write a buffer. */
	for (bhp = SH_TAILQ_FIRST(&c_mp->bhq, __bh);
	    bhp != NULL; bhp = SH_TAILQ_NEXT(bhp, q, __bh)) {
		if (bhp->ref != 0 ||
		    !F_ISSET(bhp, BH_DIRTY) || F_ISSET(bhp, BH_LOCKED))
			continue;

		mfp = R_ADDR(dbmp->reginfo, bhp->mf_offset);

		/*
		 * We can't write to temporary files -- see the comment in
		 * mp_bh.c:__memp_bhwrite().
		 */
		if (F_ISSET(mfp, MP_TEMP))
			continue;

		pgno = bhp->pgno;
		if ((ret =
		    __memp_bhwrite(dbmp, mfp, bhp, 1, NULL, &wrote)) != 0)
			break;

		/*
		 * Any process syncing the shared memory buffer pool had better
		 * be able to write to any underlying file.  Be understanding,
		 * but firm, on this point.
		 */
		if (!wrote) {
			__db_err(dbenv, "%s: unable to flush page: %lu",
			    __memp_fns(dbmp, mfp), (u_long)pgno);
			ret = EPERM;
			break;
		}

		++nwrote;
		goto loop;
	}

done:
	if (nwrotep != NULL)
		*nwrotep = nwrote;
	c_mp->stat.st_page_trickle += nwrote;

	if (nwrote != 0 && dbmp->extents != 0)
		if ((t_ret = __memp_close_flush_files(dbmp)) != 0 && ret == 0)
			ret = t_ret;
	return (ret);
}
