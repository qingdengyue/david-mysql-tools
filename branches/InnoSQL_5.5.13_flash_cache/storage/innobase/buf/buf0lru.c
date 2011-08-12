/*****************************************************************************

Copyright (c) 1995, 2010, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0lru.c
The database buffer replacement algorithm

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "buf0lru.h"

#ifdef UNIV_NONINL
#include "buf0lru.ic"
#endif

#include "ut0byte.h"
#include "ut0lst.h"
#include "ut0rnd.h"
#include "sync0sync.h"
#include "sync0rw.h"
#include "hash0hash.h"
#include "os0sync.h"
#include "fil0fil.h"
#include "btr0btr.h"
#include "buf0buddy.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "btr0sea.h"
#include "ibuf0ibuf.h"
#include "os0file.h"
#include "page0zip.h"
#include "log0recv.h"
#include "srv0srv.h"
#include "trx0sys.h"

#define FLASH_CACHE_MIGRATE_LIMIT (1.0*srv_flash_cache_migrate_pct*trx_doublewrite->fc->fc_size/100)

/** The number of blocks from the LRU_old pointer onward, including
the block pointed to, must be buf_pool->LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV
of the whole LRU list length, except that the tolerance defined below
is allowed. Note that the tolerance must be small enough such that for
even the BUF_LRU_OLD_MIN_LEN long LRU list, the LRU_old pointer is not
allowed to point to either end of the LRU list. */

#define BUF_LRU_OLD_TOLERANCE	20

/** The minimum amount of non-old blocks when the LRU_old list exists
(that is, when there are more than BUF_LRU_OLD_MIN_LEN blocks).
@see buf_LRU_old_adjust_len */
#define BUF_LRU_NON_OLD_MIN_LEN	5
#if BUF_LRU_NON_OLD_MIN_LEN >= BUF_LRU_OLD_MIN_LEN
# error "BUF_LRU_NON_OLD_MIN_LEN >= BUF_LRU_OLD_MIN_LEN"
#endif

/** When dropping the search hash index entries before deleting an ibd
file, we build a local array of pages belonging to that tablespace
in the buffer pool. Following is the size of that array. */
#define BUF_LRU_DROP_SEARCH_HASH_SIZE	1024

/** If we switch on the InnoDB monitor because there are too few available
frames in the buffer pool, we set this to TRUE */
static ibool	buf_lru_switched_on_innodb_mon	= FALSE;

/******************************************************************//**
These statistics are not 'of' LRU but 'for' LRU.  We keep count of I/O
and page_zip_decompress() operations.  Based on the statistics,
buf_LRU_evict_from_unzip_LRU() decides if we want to evict from
unzip_LRU or the regular LRU.  From unzip_LRU, we will only evict the
uncompressed frame (meaning we can evict dirty blocks as well).  From
the regular LRU, we will evict the entire block (i.e.: both the
uncompressed and compressed data), which must be clean. */

/* @{ */

/** Number of intervals for which we keep the history of these stats.
Each interval is 1 second, defined by the rate at which
srv_error_monitor_thread() calls buf_LRU_stat_update(). */
#define BUF_LRU_STAT_N_INTERVAL 50

/** Co-efficient with which we multiply I/O operations to equate them
with page_zip_decompress() operations. */
#define BUF_LRU_IO_TO_UNZIP_FACTOR 50

/** Sampled values buf_LRU_stat_cur.
Not protected by any mutex.  Updated by buf_LRU_stat_update(). */
static buf_LRU_stat_t		buf_LRU_stat_arr[BUF_LRU_STAT_N_INTERVAL];

/** Cursor to buf_LRU_stat_arr[] that is updated in a round-robin fashion. */
static ulint			buf_LRU_stat_arr_ind;

/** Current operation counters.  Not protected by any mutex.  Cleared
by buf_LRU_stat_update(). */
UNIV_INTERN buf_LRU_stat_t	buf_LRU_stat_cur;

/** Running sum of past values of buf_LRU_stat_cur.
Updated by buf_LRU_stat_update().  Not Protected by any mutex. */
UNIV_INTERN buf_LRU_stat_t	buf_LRU_stat_sum;

/* @} */

/** @name Heuristics for detecting index scan @{ */
/** Move blocks to "new" LRU list only if the first access was at
least this many milliseconds ago.  Not protected by any mutex or latch. */
UNIV_INTERN uint	buf_LRU_old_threshold_ms;
/* @} */

/******************************************************************//**
Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed and buf_pool->zip_mutex will be released.

If a compressed page or a compressed-only block descriptor is freed,
other compressed pages or compressed-only block descriptors may be
relocated.
@return the new state of the block (BUF_BLOCK_ZIP_FREE if the state
was BUF_BLOCK_ZIP_PAGE, or BUF_BLOCK_REMOVE_HASH otherwise) */
static
enum buf_page_state
buf_LRU_block_remove_hashed_page(
/*=============================*/
	buf_page_t*	bpage,	/*!< in: block, must contain a file page and
				be in a state where it can be freed; there
				may or may not be a hash index to the page */
	ibool		zip);	/*!< in: TRUE if should remove also the
				compressed page of an uncompressed page */
/******************************************************************//**
Puts a file page whose has no hash index to the free list. */
static
void
buf_LRU_block_free_hashed_page(
/*===========================*/
	buf_block_t*	block);	/*!< in: block, must contain a file page and
				be in a state where it can be freed */

/******************************************************************//**
Determines if the unzip_LRU list should be used for evicting a victim
instead of the general LRU list.
@return	TRUE if should use unzip_LRU */
UNIV_INLINE
ibool
buf_LRU_evict_from_unzip_LRU(
/*=========================*/
	buf_pool_t*	buf_pool)
{
	ulint	io_avg;
	ulint	unzip_avg;

	ut_ad(buf_pool_mutex_own(buf_pool));

	/* If the unzip_LRU list is empty, we can only use the LRU. */
	if (UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0) {
		return(FALSE);
	}

	/* If unzip_LRU is at most 10% of the size of the LRU list,
	then use the LRU.  This slack allows us to keep hot
	decompressed pages in the buffer pool. */
	if (UT_LIST_GET_LEN(buf_pool->unzip_LRU)
	    <= UT_LIST_GET_LEN(buf_pool->LRU) / 10) {
		return(FALSE);
	}

	/* If eviction hasn't started yet, we assume by default
	that a workload is disk bound. */
	if (buf_pool->freed_page_clock == 0) {
		return(TRUE);
	}

	/* Calculate the average over past intervals, and add the values
	of the current interval. */
	io_avg = buf_LRU_stat_sum.io / BUF_LRU_STAT_N_INTERVAL
		+ buf_LRU_stat_cur.io;
	unzip_avg = buf_LRU_stat_sum.unzip / BUF_LRU_STAT_N_INTERVAL
		+ buf_LRU_stat_cur.unzip;

	/* Decide based on our formula.  If the load is I/O bound
	(unzip_avg is smaller than the weighted io_avg), evict an
	uncompressed frame from unzip_LRU.  Otherwise we assume that
	the load is CPU bound and evict from the regular LRU. */
	return(unzip_avg <= io_avg * BUF_LRU_IO_TO_UNZIP_FACTOR);
}

/******************************************************************//**
Attempts to drop page hash index on a batch of pages belonging to a
particular space id. */
static
void
buf_LRU_drop_page_hash_batch(
/*=========================*/
	ulint		space_id,	/*!< in: space id */
	ulint		zip_size,	/*!< in: compressed page size in bytes
					or 0 for uncompressed pages */
	const ulint*	arr,		/*!< in: array of page_no */
	ulint		count)		/*!< in: number of entries in array */
{
	ulint	i;

	ut_ad(arr != NULL);
	ut_ad(count <= BUF_LRU_DROP_SEARCH_HASH_SIZE);

	for (i = 0; i < count; ++i) {
		btr_search_drop_page_hash_when_freed(space_id, zip_size,
						     arr[i]);
	}
}

/******************************************************************//**
When doing a DROP TABLE/DISCARD TABLESPACE we have to drop all page
hash index entries belonging to that table. This function tries to
do that in batch. Note that this is a 'best effort' attempt and does
not guarantee that ALL hash entries will be removed. */
static
void
buf_LRU_drop_page_hash_for_tablespace(
/*==================================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		id)		/*!< in: space id */
{
	buf_page_t*	bpage;
	ulint*		page_arr;
	ulint		num_entries;
	ulint		zip_size;

	zip_size = fil_space_get_zip_size(id);

	if (UNIV_UNLIKELY(zip_size == ULINT_UNDEFINED)) {
		/* Somehow, the tablespace does not exist.  Nothing to drop. */
		ut_ad(0);
		return;
	}

	page_arr = ut_malloc(
		sizeof(ulint) * BUF_LRU_DROP_SEARCH_HASH_SIZE);

	buf_pool_mutex_enter(buf_pool);
	num_entries = 0;

scan_again:
	bpage = UT_LIST_GET_LAST(buf_pool->LRU);

	while (bpage != NULL) {
		buf_page_t*	prev_bpage;
		ibool		is_fixed;

		prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

		ut_a(buf_page_in_file(bpage));

		if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE
		    || bpage->space != id
		    || bpage->io_fix != BUF_IO_NONE) {
			/* Compressed pages are never hashed.
			Skip blocks of other tablespaces.
			Skip I/O-fixed blocks (to be dealt with later). */
next_page:
			bpage = prev_bpage;
			continue;
		}

		mutex_enter(&((buf_block_t*) bpage)->mutex);
		is_fixed = bpage->buf_fix_count > 0
			|| !((buf_block_t*) bpage)->is_hashed;
		mutex_exit(&((buf_block_t*) bpage)->mutex);

		if (is_fixed) {
			goto next_page;
		}

		/* Store the page number so that we can drop the hash
		index in a batch later. */
		page_arr[num_entries] = bpage->offset;
		ut_a(num_entries < BUF_LRU_DROP_SEARCH_HASH_SIZE);
		++num_entries;

		if (num_entries < BUF_LRU_DROP_SEARCH_HASH_SIZE) {
			goto next_page;
		}

		/* Array full. We release the buf_pool->mutex to obey
		the latching order. */
		buf_pool_mutex_exit(buf_pool);

		buf_LRU_drop_page_hash_batch(
			id, zip_size, page_arr, num_entries);

		num_entries = 0;

		buf_pool_mutex_enter(buf_pool);

		/* Note that we released the buf_pool mutex above
		after reading the prev_bpage during processing of a
		page_hash_batch (i.e.: when the array was full).
		Because prev_bpage could belong to a compressed-only
		block, it may have been relocated, and thus the
		pointer cannot be trusted. Because bpage is of type
		buf_block_t, it is safe to dereference.

		bpage can change in the LRU list. This is OK because
		this function is a 'best effort' to drop as many
		search hash entries as possible and it does not
		guarantee that ALL such entries will be dropped. */

		/* If, however, bpage has been removed from LRU list
		to the free list then we should restart the scan.
		bpage->state is protected by buf_pool mutex. */
		if (bpage
		    && buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
			goto scan_again;
		}
	}

	buf_pool_mutex_exit(buf_pool);

	/* Drop any remaining batch of search hashed pages. */
	buf_LRU_drop_page_hash_batch(id, zip_size, page_arr, num_entries);
	ut_free(page_arr);
}

/******************************************************************//**
Invalidates all pages belonging to a given tablespace inside a specific
buffer pool instance when we are deleting the data file(s) of that
tablespace. */
static
void
buf_LRU_invalidate_tablespace_buf_pool_instance(
/*============================================*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	ulint		id)		/*!< in: space id */
{
	buf_page_t*	bpage;
	ibool		all_freed;

scan_again:
	buf_pool_mutex_enter(buf_pool);

	all_freed = TRUE;

	bpage = UT_LIST_GET_LAST(buf_pool->LRU);

	while (bpage != NULL) {
		buf_page_t*	prev_bpage;
		ibool		prev_bpage_buf_fix = FALSE;

		ut_a(buf_page_in_file(bpage));

		prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

		/* bpage->space and bpage->io_fix are protected by
		buf_pool->mutex and block_mutex.  It is safe to check
		them while holding buf_pool->mutex only. */

		if (buf_page_get_space(bpage) != id) {
			/* Skip this block, as it does not belong to
			the space that is being invalidated. */
		} else if (buf_page_get_io_fix(bpage) != BUF_IO_NONE) {
			/* We cannot remove this page during this scan
			yet; maybe the system is currently reading it
			in, or flushing the modifications to the file */

			all_freed = FALSE;
		} else {
			mutex_t* block_mutex = buf_page_get_mutex(bpage);
			mutex_enter(block_mutex);

			if (bpage->buf_fix_count > 0) {

				/* We cannot remove this page during
				this scan yet; maybe the system is
				currently reading it in, or flushing
				the modifications to the file */

				all_freed = FALSE;

				goto next_page;
			}

#ifdef UNIV_DEBUG
			if (buf_debug_prints) {
				fprintf(stderr,
					"Dropping space %lu page %lu\n",
					(ulong) buf_page_get_space(bpage),
					(ulong) buf_page_get_page_no(bpage));
			}
#endif
			if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
				/* This is a compressed-only block
				descriptor.  Ensure that prev_bpage
				cannot be relocated when bpage is freed. */
				if (UNIV_LIKELY(prev_bpage != NULL)) {
					switch (buf_page_get_state(
							prev_bpage)) {
					case BUF_BLOCK_FILE_PAGE:
						/* Descriptors of uncompressed
						blocks will not be relocated,
						because we are holding the
						buf_pool->mutex. */
						break;
					case BUF_BLOCK_ZIP_PAGE:
					case BUF_BLOCK_ZIP_DIRTY:
						/* Descriptors of compressed-
						only blocks can be relocated,
						unless they are buffer-fixed.
						Because both bpage and
						prev_bpage are protected by
						buf_pool_zip_mutex, it is
						not necessary to acquire
						further mutexes. */
						ut_ad(&buf_pool->zip_mutex
						      == block_mutex);
						ut_ad(mutex_own(block_mutex));
						prev_bpage_buf_fix = TRUE;
						prev_bpage->buf_fix_count++;
						break;
					default:
						ut_error;
					}
				}
			} else if (((buf_block_t*) bpage)->is_hashed) {
				ulint	page_no;
				ulint	zip_size;

				buf_pool_mutex_exit(buf_pool);

				zip_size = buf_page_get_zip_size(bpage);
				page_no = buf_page_get_page_no(bpage);

				mutex_exit(block_mutex);

				/* Note that the following call will acquire
				an S-latch on the page */

				btr_search_drop_page_hash_when_freed(
					id, zip_size, page_no);
				goto scan_again;
			}

			if (bpage->oldest_modification != 0) {

				buf_flush_remove(bpage);
			}

			/* Remove from the LRU list. */

			if (buf_LRU_block_remove_hashed_page(bpage, TRUE)
			    != BUF_BLOCK_ZIP_FREE) {
				buf_LRU_block_free_hashed_page((buf_block_t*)
							       bpage);
			} else {
				/* The block_mutex should have been
				released by buf_LRU_block_remove_hashed_page()
				when it returns BUF_BLOCK_ZIP_FREE. */
				ut_ad(block_mutex == &buf_pool->zip_mutex);
				ut_ad(!mutex_own(block_mutex));

				if (prev_bpage_buf_fix) {
					/* We temporarily buffer-fixed
					prev_bpage, so that
					buf_buddy_free() could not
					relocate it, in case it was a
					compressed-only block
					descriptor. */

					mutex_enter(block_mutex);
					ut_ad(prev_bpage->buf_fix_count > 0);
					prev_bpage->buf_fix_count--;
					mutex_exit(block_mutex);
				}

				goto next_page_no_mutex;
			}
next_page:
			mutex_exit(block_mutex);
		}

next_page_no_mutex:
		bpage = prev_bpage;
	}

	buf_pool_mutex_exit(buf_pool);

	if (!all_freed) {
		os_thread_sleep(20000);

		goto scan_again;
	}
}

/******************************************************************//**
Invalidates all pages belonging to a given tablespace when we are deleting
the data file(s) of that tablespace. */
UNIV_INTERN
void
buf_LRU_invalidate_tablespace(
/*==========================*/
	ulint	id)	/*!< in: space id */
{
	ulint	i;

	/* Before we attempt to drop pages one by one we first
	attempt to drop page hash index entries in batches to make
	it more efficient. The batching attempt is a best effort
	attempt and does not guarantee that all pages hash entries
	will be dropped. We get rid of remaining page hash entries
	one by one below. */
	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);
		buf_LRU_drop_page_hash_for_tablespace(buf_pool, id);
		buf_LRU_invalidate_tablespace_buf_pool_instance(buf_pool, id);
	}
}

/********************************************************************//**
Insert a compressed block into buf_pool->zip_clean in the LRU order. */
UNIV_INTERN
void
buf_LRU_insert_zip_clean(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	buf_page_t*	b;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_PAGE);

	/* Find the first successor of bpage in the LRU list
	that is in the zip_clean list. */
	b = bpage;
	do {
		b = UT_LIST_GET_NEXT(LRU, b);
	} while (b && buf_page_get_state(b) != BUF_BLOCK_ZIP_PAGE);

	/* Insert bpage before b, i.e., after the predecessor of b. */
	if (b) {
		b = UT_LIST_GET_PREV(list, b);
	}

	if (b) {
		UT_LIST_INSERT_AFTER(list, buf_pool->zip_clean, b, bpage);
	} else {
		UT_LIST_ADD_FIRST(list, buf_pool->zip_clean, bpage);
	}
}

/******************************************************************//**
Try to free an uncompressed page of a compressed block from the unzip
LRU list.  The compressed page is preserved, and it need not be clean.
@return	TRUE if freed */
UNIV_INLINE
ibool
buf_LRU_free_from_unzip_LRU_list(
/*=============================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		n_iterations)	/*!< in: how many times this has
					been called repeatedly without
					result: a high value means that
					we should search farther; we will
					search n_iterations / 5 of the
					unzip_LRU list, or nothing if
					n_iterations >= 5 */
{
	buf_block_t*	block;
	ulint		distance;

	ut_ad(buf_pool_mutex_own(buf_pool));

	/* Theoratically it should be much easier to find a victim
	from unzip_LRU as we can choose even a dirty block (as we'll
	be evicting only the uncompressed frame).  In a very unlikely
	eventuality that we are unable to find a victim from
	unzip_LRU, we fall back to the regular LRU list.  We do this
	if we have done five iterations so far. */

	if (UNIV_UNLIKELY(n_iterations >= 5)
	    || !buf_LRU_evict_from_unzip_LRU(buf_pool)) {

		return(FALSE);
	}

	distance = 100 + (n_iterations
			  * UT_LIST_GET_LEN(buf_pool->unzip_LRU)) / 5;

	for (block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);
	     UNIV_LIKELY(block != NULL) && UNIV_LIKELY(distance > 0);
	     block = UT_LIST_GET_PREV(unzip_LRU, block), distance--) {

		enum buf_lru_free_block_status	freed;

		ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);

		mutex_enter(&block->mutex);
		freed = buf_LRU_free_block(&block->page, FALSE);
		mutex_exit(&block->mutex);

		switch (freed) {
		case BUF_LRU_FREED:
			return(TRUE);

		case BUF_LRU_CANNOT_RELOCATE:
			/* If we failed to relocate, try
			regular LRU eviction. */
			return(FALSE);

		case BUF_LRU_NOT_FREED:
			/* The block was buffer-fixed or I/O-fixed.
			Keep looking. */
			continue;
		}

		/* inappropriate return value from
		buf_LRU_free_block() */
		ut_error;
	}

	return(FALSE);
}

/******************************************************************//**
Try to free a clean page from the common LRU list.
@return	TRUE if freed */
UNIV_INLINE
ibool
buf_LRU_free_from_common_LRU_list(
/*==============================*/
	buf_pool_t*	buf_pool,
	ulint		n_iterations)
				/*!< in: how many times this has been called
				repeatedly without result: a high value means
				that we should search farther; if
				n_iterations < 10, then we search
				n_iterations / 10 * buf_pool->curr_size
				pages from the end of the LRU list */
{
	buf_page_t*	bpage;
	ulint		distance;

	ut_ad(buf_pool_mutex_own(buf_pool));

	distance = 100 + (n_iterations * buf_pool->curr_size) / 10;

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	     UNIV_LIKELY(bpage != NULL) && UNIV_LIKELY(distance > 0);
	     bpage = UT_LIST_GET_PREV(LRU, bpage), distance--) {

		enum buf_lru_free_block_status	freed;
		unsigned			accessed;
		mutex_t*			block_mutex
			= buf_page_get_mutex(bpage);

		ut_ad(buf_page_in_file(bpage));
		ut_ad(bpage->in_LRU_list);

		mutex_enter(block_mutex);
		accessed = buf_page_is_accessed(bpage);
		freed = buf_LRU_free_block(bpage, TRUE);
		mutex_exit(block_mutex);

		switch (freed) {
		case BUF_LRU_FREED:
			/* Keep track of pages that are evicted without
			ever being accessed. This gives us a measure of
			the effectiveness of readahead */
			if (!accessed) {
				++buf_pool->stat.n_ra_pages_evicted;
			}
			return(TRUE);

		case BUF_LRU_NOT_FREED:
			/* The block was dirty, buffer-fixed, or I/O-fixed.
			Keep looking. */
			continue;

		case BUF_LRU_CANNOT_RELOCATE:
			/* This should never occur, because we
			want to discard the compressed page too. */
			break;
		}

		/* inappropriate return value from
		buf_LRU_free_block() */
		ut_error;
	}

	return(FALSE);
}

/******************************************************************//**
Try to free a replaceable block.
@return	TRUE if found and freed */
UNIV_INTERN
ibool
buf_LRU_search_and_free_block(
/*==========================*/
	buf_pool_t*	buf_pool,
				/*!< in: buffer pool instance */
	ulint		n_iterations)
				/*!< in: how many times this has been called
				repeatedly without result: a high value means
				that we should search farther; if
				n_iterations < 10, then we search
				n_iterations / 10 * buf_pool->curr_size
				pages from the end of the LRU list; if
				n_iterations < 5, then we will also search
				n_iterations / 5 of the unzip_LRU list. */
{
	ibool	freed = FALSE;

	buf_pool_mutex_enter(buf_pool);

	freed = buf_LRU_free_from_unzip_LRU_list(buf_pool, n_iterations);

	if (!freed) {
		freed = buf_LRU_free_from_common_LRU_list(
			buf_pool, n_iterations);
	}

	if (!freed) {
		buf_pool->LRU_flush_ended = 0;
	} else if (buf_pool->LRU_flush_ended > 0) {
		buf_pool->LRU_flush_ended--;
	}

	buf_pool_mutex_exit(buf_pool);

	return(freed);
}

/******************************************************************//**
Tries to remove LRU flushed blocks from the end of the LRU list and put them
to the free list. This is beneficial for the efficiency of the insert buffer
operation, as flushed pages from non-unique non-clustered indexes are here
taken out of the buffer pool, and their inserts redirected to the insert
buffer. Otherwise, the flushed blocks could get modified again before read
operations need new buffer blocks, and the i/o work done in flushing would be
wasted. */
UNIV_INTERN
void
buf_LRU_try_free_flushed_blocks(
/*============================*/
	buf_pool_t*	buf_pool)		/*!< in: buffer pool instance */
{

	if (buf_pool == NULL) {
		ulint	i;

		for (i = 0; i < srv_buf_pool_instances; i++) {
			buf_pool = buf_pool_from_array(i);
			buf_LRU_try_free_flushed_blocks(buf_pool);
		}
	} else {
		buf_pool_mutex_enter(buf_pool);

		while (buf_pool->LRU_flush_ended > 0) {

			buf_pool_mutex_exit(buf_pool);

			buf_LRU_search_and_free_block(buf_pool, 1);

			buf_pool_mutex_enter(buf_pool);
		}

		buf_pool_mutex_exit(buf_pool);
	}
}

/******************************************************************//**
Returns TRUE if less than 25 % of the buffer pool in any instance is
available. This can be used in heuristics to prevent huge transactions
eating up the whole buffer pool for their locks.
@return	TRUE if less than 25 % of buffer pool left */
UNIV_INTERN
ibool
buf_LRU_buf_pool_running_out(void)
/*==============================*/
{
	ulint	i;
	ibool	ret = FALSE;

	for (i = 0; i < srv_buf_pool_instances && !ret; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_pool_mutex_enter(buf_pool);

		if (!recv_recovery_on
		    && UT_LIST_GET_LEN(buf_pool->free)
		       + UT_LIST_GET_LEN(buf_pool->LRU)
		       < buf_pool->curr_size / 4) {

			ret = TRUE;
		}

		buf_pool_mutex_exit(buf_pool);
	}

	return(ret);
}

/******************************************************************//**
Returns a free block from the buf_pool.  The block is taken off the
free list.  If it is empty, returns NULL.
@return	a free control block, or NULL if the buf_block->free list is empty */
UNIV_INTERN
buf_block_t*
buf_LRU_get_free_only(
/*==================*/
	buf_pool_t*	buf_pool)
{
	buf_block_t*	block;

	ut_ad(buf_pool_mutex_own(buf_pool));

	block = (buf_block_t*) UT_LIST_GET_FIRST(buf_pool->free);

	if (block) {

		ut_ad(block->page.in_free_list);
		ut_d(block->page.in_free_list = FALSE);
		ut_ad(!block->page.in_flush_list);
		ut_ad(!block->page.in_LRU_list);
		ut_a(!buf_page_in_file(&block->page));
		UT_LIST_REMOVE(list, buf_pool->free, (&block->page));

		mutex_enter(&block->mutex);

		buf_block_set_state(block, BUF_BLOCK_READY_FOR_USE);
		UNIV_MEM_ALLOC(block->frame, UNIV_PAGE_SIZE);

		ut_ad(buf_pool_from_block(block) == buf_pool);

		mutex_exit(&block->mutex);
	}

	return(block);
}

/******************************************************************//**
Returns a free block from the buf_pool. The block is taken off the
free list. If it is empty, blocks are moved from the end of the
LRU list to the free list.
@return	the free control block, in state BUF_BLOCK_READY_FOR_USE */
UNIV_INTERN
buf_block_t*
buf_LRU_get_free_block(
/*===================*/
	buf_pool_t*	buf_pool)	/*!< in/out: buffer pool instance */
{
	buf_block_t*	block		= NULL;
	ibool		freed;
	ulint		n_iterations	= 1;
	ibool		mon_value_was	= FALSE;
	ibool		started_monitor	= FALSE;
loop:
	buf_pool_mutex_enter(buf_pool);

	if (!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free)
	    + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->curr_size / 20) {
		ut_print_timestamp(stderr);

		fprintf(stderr,
			"  InnoDB: ERROR: over 95 percent of the buffer pool"
			" is occupied by\n"
			"InnoDB: lock heaps or the adaptive hash index!"
			" Check that your\n"
			"InnoDB: transactions do not set too many row locks.\n"
			"InnoDB: Your buffer pool size is %lu MB."
			" Maybe you should make\n"
			"InnoDB: the buffer pool bigger?\n"
			"InnoDB: We intentionally generate a seg fault"
			" to print a stack trace\n"
			"InnoDB: on Linux!\n",
			(ulong) (buf_pool->curr_size
				 / (1024 * 1024 / UNIV_PAGE_SIZE)));

		ut_error;

	} else if (!recv_recovery_on
		   && (UT_LIST_GET_LEN(buf_pool->free)
		       + UT_LIST_GET_LEN(buf_pool->LRU))
		   < buf_pool->curr_size / 3) {

		if (!buf_lru_switched_on_innodb_mon) {

			/* Over 67 % of the buffer pool is occupied by lock
			heaps or the adaptive hash index. This may be a memory
			leak! */

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: WARNING: over 67 percent of"
				" the buffer pool is occupied by\n"
				"InnoDB: lock heaps or the adaptive"
				" hash index! Check that your\n"
				"InnoDB: transactions do not set too many"
				" row locks.\n"
				"InnoDB: Your buffer pool size is %lu MB."
				" Maybe you should make\n"
				"InnoDB: the buffer pool bigger?\n"
				"InnoDB: Starting the InnoDB Monitor to print"
				" diagnostics, including\n"
				"InnoDB: lock heap and hash index sizes.\n",
				(ulong) (buf_pool->curr_size
					 / (1024 * 1024 / UNIV_PAGE_SIZE)));

			buf_lru_switched_on_innodb_mon = TRUE;
			srv_print_innodb_monitor = TRUE;
			os_event_set(srv_lock_timeout_thread_event);
		}
	} else if (buf_lru_switched_on_innodb_mon) {

		/* Switch off the InnoDB Monitor; this is a simple way
		to stop the monitor if the situation becomes less urgent,
		but may also surprise users if the user also switched on the
		monitor! */

		buf_lru_switched_on_innodb_mon = FALSE;
		srv_print_innodb_monitor = FALSE;
	}

	/* If there is a block in the free list, take it */
	block = buf_LRU_get_free_only(buf_pool);
	buf_pool_mutex_exit(buf_pool);

	if (block) {
		ut_ad(buf_pool_from_block(block) == buf_pool);
		memset(&block->page.zip, 0, sizeof block->page.zip);

		if (started_monitor) {
			srv_print_innodb_monitor = mon_value_was;
		}

		return(block);
	}

	/* If no block was in the free list, search from the end of the LRU
	list and try to free a block there */

	freed = buf_LRU_search_and_free_block(buf_pool, n_iterations);

	if (freed > 0) {
		goto loop;
	}

	if (n_iterations > 30) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: Warning: difficult to find free blocks in\n"
			"InnoDB: the buffer pool (%lu search iterations)!"
			" Consider\n"
			"InnoDB: increasing the buffer pool size.\n"
			"InnoDB: It is also possible that"
			" in your Unix version\n"
			"InnoDB: fsync is very slow, or"
			" completely frozen inside\n"
			"InnoDB: the OS kernel. Then upgrading to"
			" a newer version\n"
			"InnoDB: of your operating system may help."
			" Look at the\n"
			"InnoDB: number of fsyncs in diagnostic info below.\n"
			"InnoDB: Pending flushes (fsync) log: %lu;"
			" buffer pool: %lu\n"
			"InnoDB: %lu OS file reads, %lu OS file writes,"
			" %lu OS fsyncs\n"
			"InnoDB: Starting InnoDB Monitor to print further\n"
			"InnoDB: diagnostics to the standard output.\n",
			(ulong) n_iterations,
			(ulong) fil_n_pending_log_flushes,
			(ulong) fil_n_pending_tablespace_flushes,
			(ulong) os_n_file_reads, (ulong) os_n_file_writes,
			(ulong) os_n_fsyncs);

		mon_value_was = srv_print_innodb_monitor;
		started_monitor = TRUE;
		srv_print_innodb_monitor = TRUE;
		os_event_set(srv_lock_timeout_thread_event);
	}

	/* No free block was found: try to flush the LRU list */

	buf_flush_free_margin(buf_pool);
	++srv_buf_pool_wait_free;

	os_aio_simulated_wake_handler_threads();

	buf_pool_mutex_enter(buf_pool);

	if (buf_pool->LRU_flush_ended > 0) {
		/* We have written pages in an LRU flush. To make the insert
		buffer more efficient, we try to move these pages to the free
		list. */

		buf_pool_mutex_exit(buf_pool);

		buf_LRU_try_free_flushed_blocks(buf_pool);
	} else {
		buf_pool_mutex_exit(buf_pool);
	}

	if (n_iterations > 10) {

		os_thread_sleep(500000);
	}

	n_iterations++;

	goto loop;
}

/*******************************************************************//**
Moves the LRU_old pointer so that the length of the old blocks list
is inside the allowed limits. */
UNIV_INLINE
void
buf_LRU_old_adjust_len(
/*===================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	ulint	old_len;
	ulint	new_len;

	ut_a(buf_pool->LRU_old);
	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(buf_pool->LRU_old_ratio >= BUF_LRU_OLD_RATIO_MIN);
	ut_ad(buf_pool->LRU_old_ratio <= BUF_LRU_OLD_RATIO_MAX);
#if BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN <= BUF_LRU_OLD_RATIO_DIV * (BUF_LRU_OLD_TOLERANCE + 5)
# error "BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN <= BUF_LRU_OLD_RATIO_DIV * (BUF_LRU_OLD_TOLERANCE + 5)"
#endif
#ifdef UNIV_LRU_DEBUG
	/* buf_pool->LRU_old must be the first item in the LRU list
	whose "old" flag is set. */
	ut_a(buf_pool->LRU_old->old);
	ut_a(!UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)
	     || !UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)->old);
	ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)
	     || UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */

	old_len = buf_pool->LRU_old_len;
	new_len = ut_min(UT_LIST_GET_LEN(buf_pool->LRU)
			 * buf_pool->LRU_old_ratio / BUF_LRU_OLD_RATIO_DIV,
			 UT_LIST_GET_LEN(buf_pool->LRU)
			 - (BUF_LRU_OLD_TOLERANCE
			    + BUF_LRU_NON_OLD_MIN_LEN));

	for (;;) {
		buf_page_t*	LRU_old = buf_pool->LRU_old;

		ut_a(LRU_old);
		ut_ad(LRU_old->in_LRU_list);
#ifdef UNIV_LRU_DEBUG
		ut_a(LRU_old->old);
#endif /* UNIV_LRU_DEBUG */

		/* Update the LRU_old pointer if necessary */

		if (old_len + BUF_LRU_OLD_TOLERANCE < new_len) {

			buf_pool->LRU_old = LRU_old = UT_LIST_GET_PREV(
				LRU, LRU_old);
#ifdef UNIV_LRU_DEBUG
			ut_a(!LRU_old->old);
#endif /* UNIV_LRU_DEBUG */
			old_len = ++buf_pool->LRU_old_len;
			buf_page_set_old(LRU_old, TRUE);

		} else if (old_len > new_len + BUF_LRU_OLD_TOLERANCE) {

			buf_pool->LRU_old = UT_LIST_GET_NEXT(LRU, LRU_old);
			old_len = --buf_pool->LRU_old_len;
			buf_page_set_old(LRU_old, FALSE);
		} else {
			return;
		}
	}
}

/*******************************************************************//**
Initializes the old blocks pointer in the LRU list. This function should be
called when the LRU list grows to BUF_LRU_OLD_MIN_LEN length. */
static
void
buf_LRU_old_init(
/*=============*/
	buf_pool_t*	buf_pool)
{
	buf_page_t*	bpage;

	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN);

	/* We first initialize all blocks in the LRU list as old and then use
	the adjust function to move the LRU_old pointer to the right
	position */

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU); bpage != NULL;
	     bpage = UT_LIST_GET_PREV(LRU, bpage)) {
		ut_ad(bpage->in_LRU_list);
		ut_ad(buf_page_in_file(bpage));
		/* This loop temporarily violates the
		assertions of buf_page_set_old(). */
		bpage->old = TRUE;
	}

	buf_pool->LRU_old = UT_LIST_GET_FIRST(buf_pool->LRU);
	buf_pool->LRU_old_len = UT_LIST_GET_LEN(buf_pool->LRU);

	buf_LRU_old_adjust_len(buf_pool);
}

/******************************************************************//**
Remove a block from the unzip_LRU list if it belonged to the list. */
static
void
buf_unzip_LRU_remove_block_if_needed(
/*=================================*/
	buf_page_t*	bpage)	/*!< in/out: control block */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_page_in_file(bpage));
	ut_ad(buf_pool_mutex_own(buf_pool));

	if (buf_page_belongs_to_unzip_LRU(bpage)) {
		buf_block_t*	block = (buf_block_t*) bpage;

		ut_ad(block->in_unzip_LRU_list);
		ut_d(block->in_unzip_LRU_list = FALSE);

		UT_LIST_REMOVE(unzip_LRU, buf_pool->unzip_LRU, block);
	}
}

/******************************************************************//**
Removes a block from the LRU list. */
UNIV_INLINE
void
buf_LRU_remove_block(
/*=================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own(buf_pool));

	ut_a(buf_page_in_file(bpage));

	ut_ad(bpage->in_LRU_list);

	/* If the LRU_old pointer is defined and points to just this block,
	move it backward one step */

	if (UNIV_UNLIKELY(bpage == buf_pool->LRU_old)) {

		/* Below: the previous block is guaranteed to exist,
		because the LRU_old pointer is only allowed to differ
		by BUF_LRU_OLD_TOLERANCE from strict
		buf_pool->LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV of the LRU
		list length. */
		buf_page_t*	prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

		ut_a(prev_bpage);
#ifdef UNIV_LRU_DEBUG
		ut_a(!prev_bpage->old);
#endif /* UNIV_LRU_DEBUG */
		buf_pool->LRU_old = prev_bpage;
		buf_page_set_old(prev_bpage, TRUE);

		buf_pool->LRU_old_len++;
	}

	/* Remove the block from the LRU list */
	UT_LIST_REMOVE(LRU, buf_pool->LRU, bpage);
	ut_d(bpage->in_LRU_list = FALSE);

	buf_unzip_LRU_remove_block_if_needed(bpage);

	/* If the LRU list is so short that LRU_old is not defined,
	clear the "old" flags and return */
	if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN) {

		for (bpage = UT_LIST_GET_FIRST(buf_pool->LRU); bpage != NULL;
		     bpage = UT_LIST_GET_NEXT(LRU, bpage)) {
			/* This loop temporarily violates the
			assertions of buf_page_set_old(). */
			bpage->old = FALSE;
		}

		buf_pool->LRU_old = NULL;
		buf_pool->LRU_old_len = 0;

		return;
	}

	ut_ad(buf_pool->LRU_old);

	/* Update the LRU_old_len field if necessary */
	if (buf_page_is_old(bpage)) {

		buf_pool->LRU_old_len--;
	}

	/* Adjust the length of the old block list if necessary */
	buf_LRU_old_adjust_len(buf_pool);
}

/******************************************************************//**
Adds a block to the LRU list of decompressed zip pages. */
UNIV_INTERN
void
buf_unzip_LRU_add_block(
/*====================*/
	buf_block_t*	block,	/*!< in: control block */
	ibool		old)	/*!< in: TRUE if should be put to the end
				of the list, else put to the start */
{
	buf_pool_t*	buf_pool = buf_pool_from_block(block);

	ut_ad(buf_pool);
	ut_ad(block);
	ut_ad(buf_pool_mutex_own(buf_pool));

	ut_a(buf_page_belongs_to_unzip_LRU(&block->page));

	ut_ad(!block->in_unzip_LRU_list);
	ut_d(block->in_unzip_LRU_list = TRUE);

	if (old) {
		UT_LIST_ADD_LAST(unzip_LRU, buf_pool->unzip_LRU, block);
	} else {
		UT_LIST_ADD_FIRST(unzip_LRU, buf_pool->unzip_LRU, block);
	}
}

/******************************************************************//**
Adds a block to the LRU list end. */
UNIV_INLINE
void
buf_LRU_add_block_to_end_low(
/*=========================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own(buf_pool));

	ut_a(buf_page_in_file(bpage));

	ut_ad(!bpage->in_LRU_list);
	UT_LIST_ADD_LAST(LRU, buf_pool->LRU, bpage);
	ut_d(bpage->in_LRU_list = TRUE);

	if (UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN) {

		ut_ad(buf_pool->LRU_old);

		/* Adjust the length of the old block list if necessary */

		buf_page_set_old(bpage, TRUE);
		buf_pool->LRU_old_len++;
		buf_LRU_old_adjust_len(buf_pool);

	} else if (UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init(buf_pool);
	} else {
		buf_page_set_old(bpage, buf_pool->LRU_old != NULL);
	}

	/* If this is a zipped block with decompressed frame as well
	then put it on the unzip_LRU list */
	if (buf_page_belongs_to_unzip_LRU(bpage)) {
		buf_unzip_LRU_add_block((buf_block_t*) bpage, TRUE);
	}
}

/******************************************************************//**
Adds a block to the LRU list. */
UNIV_INLINE
void
buf_LRU_add_block_low(
/*==================*/
	buf_page_t*	bpage,	/*!< in: control block */
	ibool		old)	/*!< in: TRUE if should be put to the old blocks
				in the LRU list, else put to the start; if the
				LRU list is very short, the block is added to
				the start, regardless of this parameter */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool);
	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own(buf_pool));

	ut_a(buf_page_in_file(bpage));
	ut_ad(!bpage->in_LRU_list);

	if (!old || (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN)) {

		UT_LIST_ADD_FIRST(LRU, buf_pool->LRU, bpage);

		bpage->freed_page_clock = buf_pool->freed_page_clock;
	} else {
#ifdef UNIV_LRU_DEBUG
		/* buf_pool->LRU_old must be the first item in the LRU list
		whose "old" flag is set. */
		ut_a(buf_pool->LRU_old->old);
		ut_a(!UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)
		     || !UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)->old);
		ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)
		     || UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */
		UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU, buf_pool->LRU_old,
				     bpage);
		buf_pool->LRU_old_len++;
	}

	ut_d(bpage->in_LRU_list = TRUE);

	if (UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN) {

		ut_ad(buf_pool->LRU_old);

		/* Adjust the length of the old block list if necessary */

		buf_page_set_old(bpage, old);
		buf_LRU_old_adjust_len(buf_pool);

	} else if (UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init(buf_pool);
	} else {
		buf_page_set_old(bpage, buf_pool->LRU_old != NULL);
	}

	/* If this is a zipped block with decompressed frame as well
	then put it on the unzip_LRU list */
	if (buf_page_belongs_to_unzip_LRU(bpage)) {
		buf_unzip_LRU_add_block((buf_block_t*) bpage, old);
	}
}

/******************************************************************//**
Adds a block to the LRU list. */
UNIV_INTERN
void
buf_LRU_add_block(
/*==============*/
	buf_page_t*	bpage,	/*!< in: control block */
	ibool		old)	/*!< in: TRUE if should be put to the old
				blocks in the LRU list, else put to the start;
				if the LRU list is very short, the block is
				added to the start, regardless of this
				parameter */
{
	buf_LRU_add_block_low(bpage, old);
}

/******************************************************************//**
Moves a block to the start of the LRU list. */
UNIV_INTERN
void
buf_LRU_make_block_young(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool_mutex_own(buf_pool));

	if (bpage->old) {
		buf_pool->stat.n_pages_made_young++;
	}

	buf_LRU_remove_block(bpage);
	buf_LRU_add_block_low(bpage, FALSE);
}

/******************************************************************//**
Moves a block to the end of the LRU list. */
UNIV_INTERN
void
buf_LRU_make_block_old(
/*===================*/
	buf_page_t*	bpage)	/*!< in: control block */
{
	buf_LRU_remove_block(bpage);
	buf_LRU_add_block_to_end_low(bpage);
}

/******************************************************************//**
Try to free a block.  If bpage is a descriptor of a compressed-only
page, the descriptor object will be freed as well.

NOTE: If this function returns BUF_LRU_FREED, it will temporarily
release buf_pool->mutex.  Furthermore, the page frame will no longer be
accessible via bpage.

The caller must hold buf_pool->mutex and buf_page_get_mutex(bpage) and
release these two mutexes after the call.  No other
buf_page_get_mutex() may be held when calling this function.
@return BUF_LRU_FREED if freed, BUF_LRU_CANNOT_RELOCATE or
BUF_LRU_NOT_FREED otherwise. */
UNIV_INTERN
enum buf_lru_free_block_status
buf_LRU_free_block(
/*===============*/
	buf_page_t*	bpage,	/*!< in: block to be freed */
	ibool		zip)	/*!< in: TRUE if should remove also the
				compressed page of an uncompressed page */
{
	buf_page_t*	b = NULL;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	mutex_t*	block_mutex = buf_page_get_mutex(bpage);

	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(mutex_own(block_mutex));
	ut_ad(buf_page_in_file(bpage));
	ut_ad(bpage->in_LRU_list);
	ut_ad(!bpage->in_flush_list == !bpage->oldest_modification);
#if UNIV_WORD_SIZE == 4
	/* On 32-bit systems, there is no padding in buf_page_t.  On
	other systems, Valgrind could complain about uninitialized pad
	bytes. */
	UNIV_MEM_ASSERT_RW(bpage, sizeof *bpage);
#endif

	if (!buf_page_can_relocate(bpage)) {

		/* Do not free buffer-fixed or I/O-fixed blocks. */
		return(BUF_LRU_NOT_FREED);
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(bpage->space, bpage->offset) == 0);
#endif /* UNIV_IBUF_COUNT_DEBUG */

	if (zip || !bpage->zip.data) {
		/* This would completely free the block. */
		/* Do not completely free dirty blocks. */

		if (bpage->oldest_modification) {
			return(BUF_LRU_NOT_FREED);
		}
	} else if (bpage->oldest_modification) {
		/* Do not completely free dirty blocks. */

		if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
			ut_ad(buf_page_get_state(bpage)
			      == BUF_BLOCK_ZIP_DIRTY);
			return(BUF_LRU_NOT_FREED);
		}

		goto alloc;
	} else if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {
		/* Allocate the control block for the compressed page.
		If it cannot be allocated (without freeing a block
		from the LRU list), refuse to free bpage. */
alloc:
		buf_pool_mutex_exit_forbid(buf_pool);
		b = buf_buddy_alloc(buf_pool, sizeof *b, NULL);
		buf_pool_mutex_exit_allow(buf_pool);

		if (UNIV_UNLIKELY(!b)) {
			return(BUF_LRU_CANNOT_RELOCATE);
		}

		memcpy(b, bpage, sizeof *b);
	}

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr, "Putting space %lu page %lu to free list\n",
			(ulong) buf_page_get_space(bpage),
			(ulong) buf_page_get_page_no(bpage));
	}
#endif /* UNIV_DEBUG */

	if (buf_LRU_block_remove_hashed_page(bpage, zip)
	    != BUF_BLOCK_ZIP_FREE) {
		ut_a(bpage->buf_fix_count == 0);

		if (b) {
			buf_page_t*	hash_b;
			buf_page_t*	prev_b	= UT_LIST_GET_PREV(LRU, b);

			const ulint	fold = buf_page_address_fold(
				bpage->space, bpage->offset);

			hash_b	= buf_page_hash_get_low(
				buf_pool, bpage->space, bpage->offset, fold);

			ut_a(!hash_b);

			b->state = b->oldest_modification
				? BUF_BLOCK_ZIP_DIRTY
				: BUF_BLOCK_ZIP_PAGE;
			UNIV_MEM_DESC(b->zip.data,
				      page_zip_get_size(&b->zip), b);

			/* The fields in_page_hash and in_LRU_list of
			the to-be-freed block descriptor should have
			been cleared in
			buf_LRU_block_remove_hashed_page(), which
			invokes buf_LRU_remove_block(). */
			ut_ad(!bpage->in_page_hash);
			ut_ad(!bpage->in_LRU_list);
			/* bpage->state was BUF_BLOCK_FILE_PAGE because
			b != NULL. The type cast below is thus valid. */
			ut_ad(!((buf_block_t*) bpage)->in_unzip_LRU_list);

			/* The fields of bpage were copied to b before
			buf_LRU_block_remove_hashed_page() was invoked. */
			ut_ad(!b->in_zip_hash);
			ut_ad(b->in_page_hash);
			ut_ad(b->in_LRU_list);

			HASH_INSERT(buf_page_t, hash,
				    buf_pool->page_hash, fold, b);

			/* Insert b where bpage was in the LRU list. */
			if (UNIV_LIKELY(prev_b != NULL)) {
				ulint	lru_len;

				ut_ad(prev_b->in_LRU_list);
				ut_ad(buf_page_in_file(prev_b));
#if UNIV_WORD_SIZE == 4
				/* On 32-bit systems, there is no
				padding in buf_page_t.  On other
				systems, Valgrind could complain about
				uninitialized pad bytes. */
				UNIV_MEM_ASSERT_RW(prev_b, sizeof *prev_b);
#endif
				UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU,
						     prev_b, b);

				if (buf_page_is_old(b)) {
					buf_pool->LRU_old_len++;
					if (UNIV_UNLIKELY
					    (buf_pool->LRU_old
					     == UT_LIST_GET_NEXT(LRU, b))) {

						buf_pool->LRU_old = b;
					}
				}

				lru_len = UT_LIST_GET_LEN(buf_pool->LRU);

				if (lru_len > BUF_LRU_OLD_MIN_LEN) {
					ut_ad(buf_pool->LRU_old);
					/* Adjust the length of the
					old block list if necessary */
					buf_LRU_old_adjust_len(buf_pool);
				} else if (lru_len == BUF_LRU_OLD_MIN_LEN) {
					/* The LRU list is now long
					enough for LRU_old to become
					defined: init it */
					buf_LRU_old_init(buf_pool);
				}
#ifdef UNIV_LRU_DEBUG
				/* Check that the "old" flag is consistent
				in the block and its neighbours. */
				buf_page_set_old(b, buf_page_is_old(b));
#endif /* UNIV_LRU_DEBUG */
			} else {
				ut_d(b->in_LRU_list = FALSE);
				buf_LRU_add_block_low(b, buf_page_is_old(b));
			}

			if (b->state == BUF_BLOCK_ZIP_PAGE) {
				buf_LRU_insert_zip_clean(b);
			} else {
				/* Relocate on buf_pool->flush_list. */
				buf_flush_relocate_on_flush_list(bpage, b);
			}

			bpage->zip.data = NULL;
			page_zip_set_size(&bpage->zip, 0);

			/* Prevent buf_page_get_gen() from
			decompressing the block while we release
			buf_pool->mutex and block_mutex. */
			b->buf_fix_count++;
			b->io_fix = BUF_IO_READ;
		}

		buf_pool_mutex_exit(buf_pool);
		mutex_exit(block_mutex);

		/* Remove possible adaptive hash index on the page.
		The page was declared uninitialized by
		buf_LRU_block_remove_hashed_page().  We need to flag
		the contents of the page valid (which it still is) in
		order to avoid bogus Valgrind warnings.*/

		UNIV_MEM_VALID(((buf_block_t*) bpage)->frame,
			       UNIV_PAGE_SIZE);
		btr_search_drop_page_hash_index((buf_block_t*) bpage);
		UNIV_MEM_INVALID(((buf_block_t*) bpage)->frame,
				 UNIV_PAGE_SIZE);

		if (b) {
			/* Compute and stamp the compressed page
			checksum while not holding any mutex.  The
			block is already half-freed
			(BUF_BLOCK_REMOVE_HASH) and removed from
			buf_pool->page_hash, thus inaccessible by any
			other thread. */

			mach_write_to_4(
				b->zip.data + FIL_PAGE_SPACE_OR_CHKSUM,
				UNIV_LIKELY(srv_use_checksums)
				? page_zip_calc_checksum(
					b->zip.data,
					page_zip_get_size(&b->zip))
				: BUF_NO_CHECKSUM_MAGIC);
		}

		buf_pool_mutex_enter(buf_pool);
		mutex_enter(block_mutex);

		if (b) {
			mutex_enter(&buf_pool->zip_mutex);
			b->buf_fix_count--;
			buf_page_set_io_fix(b, BUF_IO_NONE);
			mutex_exit(&buf_pool->zip_mutex);
		}

		buf_LRU_block_free_hashed_page((buf_block_t*) bpage);
	} else {
		/* The block_mutex should have been released by
		buf_LRU_block_remove_hashed_page() when it returns
		BUF_BLOCK_ZIP_FREE. */
		ut_ad(block_mutex == &buf_pool->zip_mutex);
		mutex_enter(block_mutex);
	}

	return(BUF_LRU_FREED);
}

/******************************************************************//**
Puts a block back to the free list. */
UNIV_INTERN
void
buf_LRU_block_free_non_file_page(
/*=============================*/
	buf_block_t*	block)	/*!< in: block, must not contain a file page */
{
	void*		data;
	buf_pool_t*	buf_pool = buf_pool_from_block(block);

	ut_ad(block);
	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(mutex_own(&block->mutex));

	switch (buf_block_get_state(block)) {
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_READY_FOR_USE:
		break;
	default:
		ut_error;
	}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	ut_a(block->n_pointers == 0);
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	ut_ad(!block->page.in_free_list);
	ut_ad(!block->page.in_flush_list);
	ut_ad(!block->page.in_LRU_list);

	buf_block_set_state(block, BUF_BLOCK_NOT_USED);

	UNIV_MEM_ALLOC(block->frame, UNIV_PAGE_SIZE);
#ifdef UNIV_DEBUG
	/* Wipe contents of page to reveal possible stale pointers to it */
	memset(block->frame, '\0', UNIV_PAGE_SIZE);
#else
	/* Wipe page_no and space_id */
	memset(block->frame + FIL_PAGE_OFFSET, 0xfe, 4);
	memset(block->frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xfe, 4);
#endif
	data = block->page.zip.data;

	if (data) {
		block->page.zip.data = NULL;
		mutex_exit(&block->mutex);
		buf_pool_mutex_exit_forbid(buf_pool);

		buf_buddy_free(
			buf_pool, data, page_zip_get_size(&block->page.zip));

		buf_pool_mutex_exit_allow(buf_pool);
		mutex_enter(&block->mutex);
		page_zip_set_size(&block->page.zip, 0);
	}

	UT_LIST_ADD_FIRST(list, buf_pool->free, (&block->page));
	ut_d(block->page.in_free_list = TRUE);

	UNIV_MEM_ASSERT_AND_FREE(block->frame, UNIV_PAGE_SIZE);
}

/******************************************************************//**
Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed and buf_pool->zip_mutex will be released.

If a compressed page or a compressed-only block descriptor is freed,
other compressed pages or compressed-only block descriptors may be
relocated.
@return the new state of the block (BUF_BLOCK_ZIP_FREE if the state
was BUF_BLOCK_ZIP_PAGE, or BUF_BLOCK_REMOVE_HASH otherwise) */
static
enum buf_page_state
buf_LRU_block_remove_hashed_page(
/*=============================*/
	buf_page_t*	bpage,	/*!< in: block, must contain a file page and
				be in a state where it can be freed; there
				may or may not be a hash index to the page */
	ibool		zip)	/*!< in: TRUE if should remove also the
				compressed page of an uncompressed page */
{
	ulint			fold;
	const buf_page_t*	hashed_bpage;
	buf_pool_t*		buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(bpage);
	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	ut_a(bpage->buf_fix_count == 0);

#if UNIV_WORD_SIZE == 4
	/* On 32-bit systems, there is no padding in
	buf_page_t.  On other systems, Valgrind could complain
	about uninitialized pad bytes. */
	UNIV_MEM_ASSERT_RW(bpage, sizeof *bpage);
#endif

	buf_LRU_remove_block(bpage);

	buf_pool->freed_page_clock += 1;

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_FILE_PAGE:
		if ( srv_flash_cache_size > 0 && trx_doublewrite && srv_flash_cache_enable_migrate ){
			buf_LRU_move_to_flash_read_cache(bpage);
		}
		UNIV_MEM_ASSERT_W(bpage, sizeof(buf_block_t));
		UNIV_MEM_ASSERT_W(((buf_block_t*) bpage)->frame,
				  UNIV_PAGE_SIZE);
		buf_block_modify_clock_inc((buf_block_t*) bpage);
		if (bpage->zip.data) {
			const page_t*	page = ((buf_block_t*) bpage)->frame;
			const ulint	zip_size
				= page_zip_get_size(&bpage->zip);

			ut_a(!zip || bpage->oldest_modification == 0);

			switch (UNIV_EXPECT(fil_page_get_type(page),
					    FIL_PAGE_INDEX)) {
			case FIL_PAGE_TYPE_ALLOCATED:
			case FIL_PAGE_INODE:
			case FIL_PAGE_IBUF_BITMAP:
			case FIL_PAGE_TYPE_FSP_HDR:
			case FIL_PAGE_TYPE_XDES:
				/* These are essentially uncompressed pages. */
				if (!zip) {
					/* InnoDB writes the data to the
					uncompressed page frame.  Copy it
					to the compressed page, which will
					be preserved. */
					memcpy(bpage->zip.data, page,
					       zip_size);
				}
				break;
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			case FIL_PAGE_INDEX:
#ifdef UNIV_ZIP_DEBUG
				ut_a(page_zip_validate(&bpage->zip, page));
#endif /* UNIV_ZIP_DEBUG */
				break;
			default:
				ut_print_timestamp(stderr);
				fputs("  InnoDB: ERROR: The compressed page"
				      " to be evicted seems corrupt:", stderr);
				ut_print_buf(stderr, page, zip_size);
				fputs("\nInnoDB: Possibly older version"
				      " of the page:", stderr);
				ut_print_buf(stderr, bpage->zip.data,
					     zip_size);
				putc('\n', stderr);
				ut_error;
			}

			break;
		}
		/* fall through */
	case BUF_BLOCK_ZIP_PAGE:
		ut_a(bpage->oldest_modification == 0);
		UNIV_MEM_ASSERT_W(bpage->zip.data,
				  page_zip_get_size(&bpage->zip));
		break;
	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_ZIP_DIRTY:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	}

	fold = buf_page_address_fold(bpage->space, bpage->offset);
	hashed_bpage = buf_page_hash_get_low(
		buf_pool, bpage->space, bpage->offset, fold);

	if (UNIV_UNLIKELY(bpage != hashed_bpage)) {
		fprintf(stderr,
			"InnoDB: Error: page %lu %lu not found"
			" in the hash table\n",
			(ulong) bpage->space,
			(ulong) bpage->offset);
		if (hashed_bpage) {
			fprintf(stderr,
				"InnoDB: In hash table we find block"
				" %p of %lu %lu which is not %p\n",
				(const void*) hashed_bpage,
				(ulong) hashed_bpage->space,
				(ulong) hashed_bpage->offset,
				(const void*) bpage);
		}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		mutex_exit(buf_page_get_mutex(bpage));
		buf_pool_mutex_exit(buf_pool);
		buf_print();
		buf_LRU_print();
		buf_validate();
		buf_LRU_validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		ut_error;
	}

	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_d(bpage->in_page_hash = FALSE);
	HASH_DELETE(buf_page_t, hash, buf_pool->page_hash, fold, bpage);
	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_ZIP_PAGE:
		ut_ad(!bpage->in_free_list);
		ut_ad(!bpage->in_flush_list);
		ut_ad(!bpage->in_LRU_list);
		ut_a(bpage->zip.data);
		ut_a(buf_page_get_zip_size(bpage));

		UT_LIST_REMOVE(list, buf_pool->zip_clean, bpage);

		mutex_exit(&buf_pool->zip_mutex);
		buf_pool_mutex_exit_forbid(buf_pool);

		buf_buddy_free(
			buf_pool, bpage->zip.data,
			page_zip_get_size(&bpage->zip));

		bpage->state = BUF_BLOCK_ZIP_FREE;
		buf_buddy_free(buf_pool, bpage, sizeof(*bpage));
		buf_pool_mutex_exit_allow(buf_pool);

		UNIV_MEM_UNDESC(bpage);
		return(BUF_BLOCK_ZIP_FREE);

	case BUF_BLOCK_FILE_PAGE:
		memset(((buf_block_t*) bpage)->frame
		       + FIL_PAGE_OFFSET, 0xff, 4);
		memset(((buf_block_t*) bpage)->frame
		       + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		UNIV_MEM_INVALID(((buf_block_t*) bpage)->frame,
				 UNIV_PAGE_SIZE);
		buf_page_set_state(bpage, BUF_BLOCK_REMOVE_HASH);

		if (zip && bpage->zip.data) {
			/* Free the compressed page. */
			void*	data = bpage->zip.data;
			bpage->zip.data = NULL;

			ut_ad(!bpage->in_free_list);
			ut_ad(!bpage->in_flush_list);
			ut_ad(!bpage->in_LRU_list);
			mutex_exit(&((buf_block_t*) bpage)->mutex);
			buf_pool_mutex_exit_forbid(buf_pool);

			buf_buddy_free(
				buf_pool, data,
				page_zip_get_size(&bpage->zip));

			buf_pool_mutex_exit_allow(buf_pool);
			mutex_enter(&((buf_block_t*) bpage)->mutex);
			page_zip_set_size(&bpage->zip, 0);
		}

		return(BUF_BLOCK_REMOVE_HASH);

	case BUF_BLOCK_ZIP_FREE:
	case BUF_BLOCK_ZIP_DIRTY:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		break;
	}

	ut_error;
	return(BUF_BLOCK_ZIP_FREE);
}

/******************************************************************//**
Puts a file page whose has no hash index to the free list. */
static
void
buf_LRU_block_free_hashed_page(
/*===========================*/
	buf_block_t*	block)	/*!< in: block, must contain a file page and
				be in a state where it can be freed */
{
#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_block(block);
	ut_ad(buf_pool_mutex_own(buf_pool));
#endif
	ut_ad(mutex_own(&block->mutex));

	buf_block_set_state(block, BUF_BLOCK_MEMORY);

	buf_LRU_block_free_non_file_page(block);
}

/**********************************************************************//**
Updates buf_pool->LRU_old_ratio for one buffer pool instance.
@return	updated old_pct */
static
uint
buf_LRU_old_ratio_update_instance(
/*==============================*/
	buf_pool_t*	buf_pool,/*!< in: buffer pool instance */
	uint		old_pct,/*!< in: Reserve this percentage of
				the buffer pool for "old" blocks. */
	ibool		adjust)	/*!< in: TRUE=adjust the LRU list;
				FALSE=just assign buf_pool->LRU_old_ratio
				during the initialization of InnoDB */
{
	uint	ratio;

	ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100;
	if (ratio < BUF_LRU_OLD_RATIO_MIN) {
		ratio = BUF_LRU_OLD_RATIO_MIN;
	} else if (ratio > BUF_LRU_OLD_RATIO_MAX) {
		ratio = BUF_LRU_OLD_RATIO_MAX;
	}

	if (adjust) {
		buf_pool_mutex_enter(buf_pool);

		if (ratio != buf_pool->LRU_old_ratio) {
			buf_pool->LRU_old_ratio = ratio;

			if (UT_LIST_GET_LEN(buf_pool->LRU)
			   >= BUF_LRU_OLD_MIN_LEN) {

				buf_LRU_old_adjust_len(buf_pool);
			}
		}

		buf_pool_mutex_exit(buf_pool);
	} else {
		buf_pool->LRU_old_ratio = ratio;
	}
	/* the reverse of 
	ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100 */
	return((uint) (ratio * 100 / (double) BUF_LRU_OLD_RATIO_DIV + 0.5));
}

/**********************************************************************//**
Updates buf_pool->LRU_old_ratio.
@return	updated old_pct */
UNIV_INTERN
ulint
buf_LRU_old_ratio_update(
/*=====================*/
	uint	old_pct,/*!< in: Reserve this percentage of
			the buffer pool for "old" blocks. */
	ibool	adjust)	/*!< in: TRUE=adjust the LRU list;
			FALSE=just assign buf_pool->LRU_old_ratio
			during the initialization of InnoDB */
{
	ulint	i;
	ulint	new_ratio = 0;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		new_ratio = buf_LRU_old_ratio_update_instance(
			buf_pool, old_pct, adjust);
	}

	return(new_ratio);
}

/********************************************************************//**
Update the historical stats that we are collecting for LRU eviction
policy at the end of each interval. */
UNIV_INTERN
void
buf_LRU_stat_update(void)
/*=====================*/
{
	ulint		i;
	buf_LRU_stat_t*	item;
	buf_pool_t*	buf_pool;
	ibool		evict_started = FALSE;
	buf_LRU_stat_t	cur_stat;

	/* If we haven't started eviction yet then don't update stats. */
	for (i = 0; i < srv_buf_pool_instances; i++) {

		buf_pool = buf_pool_from_array(i);

		if (buf_pool->freed_page_clock != 0) {
			evict_started = TRUE;
			break;
		}
	}

	if (!evict_started) {
		goto func_exit;
	}

	/* Update the index. */
	item = &buf_LRU_stat_arr[buf_LRU_stat_arr_ind];
	buf_LRU_stat_arr_ind++;
	buf_LRU_stat_arr_ind %= BUF_LRU_STAT_N_INTERVAL;

	/* Add the current value and subtract the obsolete entry.
	Since buf_LRU_stat_cur is not protected by any mutex,
	it can be changing between adding to buf_LRU_stat_sum
	and copying to item. Assign it to local variables to make
	sure the same value assign to the buf_LRU_stat_sum
	and item */
	cur_stat = buf_LRU_stat_cur;

	buf_LRU_stat_sum.io += cur_stat.io - item->io;
	buf_LRU_stat_sum.unzip += cur_stat.unzip - item->unzip;

	/* Put current entry in the array. */
	memcpy(item, &cur_stat, sizeof *item);

func_exit:
	/* Clear the current entry. */
	memset(&buf_LRU_stat_cur, 0, sizeof buf_LRU_stat_cur);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**********************************************************************//**
Validates the LRU list for one buffer pool instance. */
static
void
buf_LRU_validate_instance(
/*======================*/
	buf_pool_t*	buf_pool)
{
	buf_page_t*	bpage;
	buf_block_t*	block;
	ulint		old_len;
	ulint		new_len;

	ut_ad(buf_pool);
	buf_pool_mutex_enter(buf_pool);

	if (UT_LIST_GET_LEN(buf_pool->LRU) >= BUF_LRU_OLD_MIN_LEN) {

		ut_a(buf_pool->LRU_old);
		old_len = buf_pool->LRU_old_len;
		new_len = ut_min(UT_LIST_GET_LEN(buf_pool->LRU)
				 * buf_pool->LRU_old_ratio
				 / BUF_LRU_OLD_RATIO_DIV,
				 UT_LIST_GET_LEN(buf_pool->LRU)
				 - (BUF_LRU_OLD_TOLERANCE
				    + BUF_LRU_NON_OLD_MIN_LEN));
		ut_a(old_len >= new_len - BUF_LRU_OLD_TOLERANCE);
		ut_a(old_len <= new_len + BUF_LRU_OLD_TOLERANCE);
	}

	UT_LIST_VALIDATE(LRU, buf_page_t, buf_pool->LRU,
			 ut_ad(ut_list_node_313->in_LRU_list));

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	old_len = 0;

	while (bpage != NULL) {

		switch (buf_page_get_state(bpage)) {
		case BUF_BLOCK_ZIP_FREE:
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			ut_error;
			break;
		case BUF_BLOCK_FILE_PAGE:
			ut_ad(((buf_block_t*) bpage)->in_unzip_LRU_list
			      == buf_page_belongs_to_unzip_LRU(bpage));
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_ZIP_DIRTY:
			break;
		}

		if (buf_page_is_old(bpage)) {
			const buf_page_t*	prev
				= UT_LIST_GET_PREV(LRU, bpage);
			const buf_page_t*	next
				= UT_LIST_GET_NEXT(LRU, bpage);

			if (!old_len++) {
				ut_a(buf_pool->LRU_old == bpage);
			} else {
				ut_a(!prev || buf_page_is_old(prev));
			}

			ut_a(!next || buf_page_is_old(next));
		}

		bpage = UT_LIST_GET_NEXT(LRU, bpage);
	}

	ut_a(buf_pool->LRU_old_len == old_len);

	UT_LIST_VALIDATE(list, buf_page_t, buf_pool->free,
			 ut_ad(ut_list_node_313->in_free_list));

	for (bpage = UT_LIST_GET_FIRST(buf_pool->free);
	     bpage != NULL;
	     bpage = UT_LIST_GET_NEXT(list, bpage)) {

		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_NOT_USED);
	}

	UT_LIST_VALIDATE(unzip_LRU, buf_block_t, buf_pool->unzip_LRU,
			 ut_ad(ut_list_node_313->in_unzip_LRU_list
			       && ut_list_node_313->page.in_LRU_list));

	for (block = UT_LIST_GET_FIRST(buf_pool->unzip_LRU);
	     block;
	     block = UT_LIST_GET_NEXT(unzip_LRU, block)) {

		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);
		ut_a(buf_page_belongs_to_unzip_LRU(&block->page));
	}

	buf_pool_mutex_exit(buf_pool);
}

/**********************************************************************//**
Validates the LRU list.
@return	TRUE */
UNIV_INTERN
ibool
buf_LRU_validate(void)
/*==================*/
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);
		buf_LRU_validate_instance(buf_pool);
	}

	return(TRUE);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/**********************************************************************//**
Sync flash cache hash table from LRU remove page opreation */
UNIV_INTERN
void
buf_LRU_flash_cache_sync_hash_table(
/*==========================*/
trx_flashcache_block_t* b, /*!< flash cache block to be removed */
buf_page_t* bpage /*!< frame to be written */
){

	
	/* block to be written */
	trx_flashcache_block_t* b2 = &trx_doublewrite->fc->block[trx_doublewrite->fc->write_off];

	ut_ad(mutex_own(&trx_doublewrite->fc->fc_mutex));

	if ( b != NULL ){
		HASH_DELETE(trx_flashcache_block_t,hash,trx_doublewrite->fc->fc_hash,
			buf_page_address_fold(b->space, b->offset),
			b);
		b->state = BLOCK_NOT_USED;
		srv_flash_cache_used = srv_flash_cache_used - 1;
	}

	if ( b2->state != BLOCK_NOT_USED ){
		HASH_DELETE(trx_flashcache_block_t,hash,trx_doublewrite->fc->fc_hash,
			buf_page_address_fold(b2->space, b2->offset),
			b2);
		srv_flash_cache_used = srv_flash_cache_used - 1;
	}

	b2->space = bpage->space;
	b2->offset = bpage->offset;
	b2->state = BLOCK_READ_CACHE;
	/* insert to hash table */
	HASH_INSERT(trx_flashcache_block_t,hash,trx_doublewrite->fc->fc_hash,
		buf_page_address_fold(bpage->space, bpage->offset),
		b2);
	srv_flash_cache_used = srv_flash_cache_used + 1;

}

UNIV_INTERN
ibool
buf_LRU_is_flash_cache_migrate_avaliable(){
	if ( trx_doublewrite->fc->write_round == trx_doublewrite->fc->flush_round ){
		return TRUE;
	}
	else{
		if ( trx_doublewrite->fc->write_off + 1 < trx_doublewrite->fc->flush_off ) {
			return TRUE;
		}
	}
	return FALSE;
}

/**********************************************************************//**
Move to flash cache if possible */
UNIV_INTERN
void
buf_LRU_move_to_flash_read_cache(
/*===============*/
buf_page_t* bpage)
{
	trx_flashcache_block_t* b;
	const page_t*	page = ((buf_block_t*) bpage)->frame;
	ulint ret ;
	ulint write_offset;

	if ( fil_page_get_type(page) != FIL_PAGE_INDEX
		&& fil_page_get_type(page) != FIL_PAGE_INODE ){
			return;
	}

retry:
	flash_cache_mutex_enter();
	flash_cache_hash_mutex_enter(bpage->space,bpage->offset);	
	/* search the same space offset in hash table */
	HASH_SEARCH(hash,trx_doublewrite->fc->fc_hash,
		buf_page_address_fold(bpage->space,bpage->offset),
		trx_flashcache_block_t*,b,
		ut_ad(1),
		bpage->space == b->space && bpage->offset == b->offset);

	write_offset = trx_doublewrite->fc->write_off;

	/*if ( b ){
		if ( (((trx_doublewrite->fc->write_off > b->fil_offset) && (trx_doublewrite->fc->write_off - b->fil_offset ) >= FLASH_CACHE_MIGRATE_LIMIT)
			 || ((trx_doublewrite->fc->write_off < b->fil_offset) && ( b->fil_offset - trx_doublewrite->fc->write_off ) >= FLASH_CACHE_MIGRATE_LIMIT))
				&& buf_LRU_is_flash_cache_migrate_avaliable()
				&& b->state != BLOCK_READY_FOR_FLUSH ){
			buf_LRU_flash_cache_sync_hash_table(b,bpage);
			ret = fil_io(OS_FILE_WRITE,TRUE,FLASH_CACHE_SPACE,0,trx_doublewrite->fc->write_off,0,UNIV_PAGE_SIZE,((buf_block_t*)bpage)->frame,NULL);
			if ( ret != DB_SUCCESS ){
				ut_print_timestamp(stderr);
				fprintf(stderr,"	InnoDB: Error to migrate from buffer pool to flash cache, space:%lu, offset %lu",bpage->space,bpage->offset);
				ut_error;
			}
			srv_flash_cache_migrate++;
			trx_doublewrite->fc->write_off = ( trx_doublewrite->fc->write_off + 1 ) % trx_doublewrite->fc->fc_size;
			if ( write_offset > trx_doublewrite->fc->write_off ){
				trx_doublewrite->fc->write_round = trx_doublewrite->fc->write_round + 1;
			}
			srv_flash_cache_write++;
		}
	}
	else*/ 
		if ( b == NULL && bpage->access_time != 0 
			/*&& (100.0*srv_flash_cache_used)/trx_doublewrite->fc->fc_size < 80 */
			){
		if ( !buf_LRU_is_flash_cache_migrate_avaliable()  ){
			ut_print_timestamp(stderr);
			fprintf(stderr,"	InnoDB: sleep for free space.write offset %lu, flush offset %lu.Thread id is %lu.\n",trx_doublewrite->fc->write_off,trx_doublewrite->fc->flush_off,os_thread_get_curr_id());
			flash_cache_hash_mutex_exit(bpage->space,bpage->offset);	
			flash_cache_mutex_exit();
			os_thread_sleep(5000);
			goto retry;
		}
		buf_LRU_flash_cache_sync_hash_table(NULL,bpage);
		ret = fil_io(OS_FILE_WRITE,TRUE,FLASH_CACHE_SPACE,0,trx_doublewrite->fc->write_off,0,UNIV_PAGE_SIZE,((buf_block_t*)bpage)->frame,NULL);
		if ( ret != DB_SUCCESS ){
			ut_print_timestamp(stderr);
			fprintf(stderr,"	InnoDB: Error to migrate from buffer pool to flash cache, space:%lu, offset %lu",bpage->space,bpage->offset);
			ut_error;
		}
		srv_flash_cache_write++;
		trx_doublewrite->fc->write_off = ( trx_doublewrite->fc->write_off + 1 ) % trx_doublewrite->fc->fc_size;
		if ( write_offset > trx_doublewrite->fc->write_off ){
			trx_doublewrite->fc->write_round = trx_doublewrite->fc->write_round + 1;
		}
		srv_flash_cache_migrate++;
	}
	flash_cache_hash_mutex_exit(bpage->space,bpage->offset);
	//flash_cache_log_commit();
	flash_cache_mutex_exit();
#ifdef UNIV_DEBUG
	buf_flush_flash_cache_validate();
#endif
}

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**********************************************************************//**
Prints the LRU list for one buffer pool instance. */
UNIV_INTERN
void
buf_LRU_print_instance(
/*===================*/
	buf_pool_t*	buf_pool)
{
	const buf_page_t*	bpage;

	ut_ad(buf_pool);
	buf_pool_mutex_enter(buf_pool);

	bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

	while (bpage != NULL) {

		mutex_enter(buf_page_get_mutex(bpage));
		fprintf(stderr, "BLOCK space %lu page %lu ",
			(ulong) buf_page_get_space(bpage),
			(ulong) buf_page_get_page_no(bpage));

		if (buf_page_is_old(bpage)) {
			fputs("old ", stderr);
		}

		if (bpage->buf_fix_count) {
			fprintf(stderr, "buffix count %lu ",
				(ulong) bpage->buf_fix_count);
		}

		if (buf_page_get_io_fix(bpage)) {
			fprintf(stderr, "io_fix %lu ",
				(ulong) buf_page_get_io_fix(bpage));
		}

		if (bpage->oldest_modification) {
			fputs("modif. ", stderr);
		}

		switch (buf_page_get_state(bpage)) {
			const byte*	frame;
		case BUF_BLOCK_FILE_PAGE:
			frame = buf_block_get_frame((buf_block_t*) bpage);
			fprintf(stderr, "\ntype %lu"
				" index id %llu\n",
				(ulong) fil_page_get_type(frame),
				(ullint) btr_page_get_index_id(frame));
			break;
		case BUF_BLOCK_ZIP_PAGE:
			frame = bpage->zip.data;
			fprintf(stderr, "\ntype %lu size %lu"
				" index id %llu\n",
				(ulong) fil_page_get_type(frame),
				(ulong) buf_page_get_zip_size(bpage),
				(ullint) btr_page_get_index_id(frame));
			break;

		default:
			fprintf(stderr, "\n!state %lu!\n",
				(ulong) buf_page_get_state(bpage));
			break;
		}

		mutex_exit(buf_page_get_mutex(bpage));
		bpage = UT_LIST_GET_NEXT(LRU, bpage);
	}

	buf_pool_mutex_exit(buf_pool);
}

/**********************************************************************//**
Prints the LRU list. */
UNIV_INTERN
void
buf_LRU_print(void)
/*===============*/
{
	ulint		i;
	buf_pool_t*	buf_pool;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool = buf_pool_from_array(i);
		buf_LRU_print_instance(buf_pool);
	}
}


#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */
