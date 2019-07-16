/*****************************************************************************\
 *  select_cons_res.c - node selection plugin supporting consumable
 *  resources policies.
 *****************************************************************************\
 *
 *  The following example below illustrates how four jobs are allocated
 *  across a cluster using when a processor consumable resource approach.
 *
 *  The example cluster is composed of 4 nodes (10 cpus in total):
 *  linux01 (with 2 processors),
 *  linux02 (with 2 processors),
 *  linux03 (with 2 processors), and
 *  linux04 (with 4 processors).
 *
 *  The four jobs are the following:
 *  1. srun -n 4 -N 4  sleep 120 &
 *  2. srun -n 3 -N 3 sleep 120 &
 *  3. srun -n 1 sleep 120 &
 *  4. srun -n 3 sleep 120 &
 *  The user launches them in the same order as listed above.
 *
 *  Using a processor consumable resource approach we get the following
 *  job allocation and scheduling:
 *
 *  The output of squeue shows that we have 3 out of the 4 jobs allocated
 *  and running. This is a 2 running job increase over the default Slurm
 *  approach.
 *
 *  Job 2, Job 3, and Job 4 are now running concurrently on the cluster.
 *
 *  [<snip>]# squeue
 *  JOBID PARTITION     NAME     USER  ST       TIME  NODES NODELIST(REASON)
 *     5        lsf    sleep     root  PD       0:00      1 (Resources)
 *     2        lsf    sleep     root   R       0:13      4 linux[01-04]
 *     3        lsf    sleep     root   R       0:09      3 linux[01-03]
 *     4        lsf    sleep     root   R       0:05      1 linux04
 *  [<snip>]#
 *
 *  Once Job 2 finishes, Job 5, which was pending, is allocated
 *  available resources and is then running as illustrated below:
 *
 *  [<snip>]# squeue
 *   JOBID PARTITION    NAME     USER  ST       TIME  NODES NODELIST(REASON)
 *     3        lsf    sleep     root   R       1:58      3 linux[01-03]
 *     4        lsf    sleep     root   R       1:54      1 linux04
 *     5        lsf    sleep     root   R       0:02      3 linux[01-03]
 *  [<snip>]#
 *
 *  Job 3, Job 4, and Job 5 are now running concurrently on the cluster.
 *
 *  [<snip>]#  squeue
 *  JOBID PARTITION     NAME     USER  ST       TIME  NODES NODELIST(REASON)
 *     5        lsf    sleep     root   R       1:52      3 linux[01-03]
 *  [<snip>]#
 *
 * The advantage of the consumable resource scheduling policy is that
 * the job throughput can increase dramatically.
 *
 *****************************************************************************
 *  Copyright (C) 2005-2008 Hewlett-Packard Development Company, L.P.
 *  Written by Susanne M. Balle <susanne.balle@hp.com>, who borrowed heavily
 *  from select/linear
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#define _GNU_SOURCE

#include <inttypes.h>
#include <string.h>

#include "src/common/slurm_xlator.h"
#include "src/common/slurm_selecttype_info.h"
#include "src/common/assoc_mgr.h"
#include "src/common/xstring.h"
#include "select_cons_res.h"

#include "dist_tasks.h"
#include "job_test.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "select" for Slurm node selection) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load select plugins if the plugin_type string has a
 * prefix of "select/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[] = "Consumable Resources (CR) Node Selection plugin";
const char *plugin_type = "select/cons_res";
const uint32_t plugin_id      = SELECT_PLUGIN_CONS_RES;
const uint32_t plugin_version = SLURM_VERSION_NUMBER;
const uint32_t pstate_version = 7;	/* version control on saved state */
const uint16_t nodeinfo_magic = 0x82aa;

extern select_nodeinfo_t *select_p_select_nodeinfo_alloc(void);
extern int select_p_select_nodeinfo_free(select_nodeinfo_t *nodeinfo);

/* Procedure Declarations */
static void _spec_core_filter(bitstr_t *node_bitmap, bitstr_t **core_bitmap);

struct sort_support {
	int jstart;
	struct job_resources *tmpjobs;
};
static int _compare_support(const void *, const void *);

static void _add_job_to_cores(job_resources_t *job_resrcs_ptr,
			      struct part_row_data *r_ptr,
			      const uint16_t *bits_per_node)
{
	if (!r_ptr->row_bitmap)
		r_ptr->row_bitmap = build_core_array();

	add_job_to_cores(job_resrcs_ptr, &r_ptr->row_bitmap[0], bits_per_node);
	r_ptr->first_row_bitmap = r_ptr->row_bitmap[0];
}

/* test for conflicting core_bitmap bits */
static int _can_job_fit_in_row(struct job_resources *job,
			       struct part_row_data *r_ptr)
{
	if ((r_ptr->num_jobs == 0) || !r_ptr->first_row_bitmap)
		return 1;

	return job_fits_into_cores(job, r_ptr->first_row_bitmap,
				   cr_node_num_cores);
}

/*
 * _build_row_bitmaps: A job has been removed from the given partition,
 *                     so the first_row_bitmap(s) need to be reconstructed.
 *                     Optimize the jobs into the least number of rows,
 *                     and make the lower rows as dense as possible.
 *
 * IN/OUT: p_ptr   - the partition that has jobs to be optimized
 */
static void _build_row_bitmaps(struct part_res_record *p_ptr,
			       struct job_record *job_ptr)
{
	uint32_t i, j, num_jobs, size;
	int x;
	struct part_row_data *this_row, *orig_row;
	struct sort_support *ss;

	if (!p_ptr->row)
		return;

	if (p_ptr->num_rows == 1) {
		this_row = &(p_ptr->row[0]);
		if (this_row->num_jobs == 0) {
			if (this_row->first_row_bitmap) {
				size = bit_size(this_row->first_row_bitmap);
				bit_nclear(this_row->first_row_bitmap, 0, size-1);
			}
		} else {
			if (job_ptr) { /* just remove the job */
				xassert(job_ptr->job_resrcs);
				remove_job_from_cores(job_ptr->job_resrcs,
						      &(this_row->first_row_bitmap),
						      cr_node_num_cores);
			} else { /* totally rebuild the bitmap */
				size = bit_size(this_row->first_row_bitmap);
				bit_nclear(this_row->first_row_bitmap, 0, size-1);
				for (j = 0; j < this_row->num_jobs; j++) {
					_add_job_to_cores(this_row->job_list[j],
							  this_row,
							  cr_node_num_cores);
				}
			}
		}
		return;
	}

	/* gather data */
	num_jobs = 0;
	for (i = 0; i < p_ptr->num_rows; i++) {
		num_jobs += p_ptr->row[i].num_jobs;
	}
	if (num_jobs == 0) {
		size = bit_size(p_ptr->row[0].first_row_bitmap);
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (p_ptr->row[i].first_row_bitmap) {
				bit_nclear(p_ptr->row[i].first_row_bitmap, 0,
					   size-1);
			}
		}
		return;
	}

	if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		info("DEBUG: _build_row_bitmaps (before):");
		common_dump_parts(p_ptr);
	}
	debug3("cons_res: build_row_bitmaps reshuffling %u jobs", num_jobs);

	/* make a copy, in case we cannot do better than this */
	orig_row = common_dup_row_data(p_ptr->row, p_ptr->num_rows);
	if (orig_row == NULL)
		return;

	/* get row_bitmap size from first row (we can safely assume that the
	 * first row_bitmap exists because there exists at least one job. */
	size = bit_size(p_ptr->row[0].first_row_bitmap);

	/* create a master job list and clear out ALL row data */
	ss = xcalloc(num_jobs, sizeof(struct sort_support));
	x = 0;
	for (i = 0; i < p_ptr->num_rows; i++) {
		for (j = 0; j < p_ptr->row[i].num_jobs; j++) {
			ss[x].tmpjobs = p_ptr->row[i].job_list[j];
			p_ptr->row[i].job_list[j] = NULL;
			ss[x].jstart = bit_ffs(ss[x].tmpjobs->node_bitmap);
			ss[x].jstart = cr_get_coremap_offset(ss[x].jstart);
			ss[x].jstart += bit_ffs(ss[x].tmpjobs->core_bitmap);
			x++;
		}
		p_ptr->row[i].num_jobs = 0;
		if (p_ptr->row[i].first_row_bitmap) {
			bit_nclear(p_ptr->row[i].first_row_bitmap, 0, size-1);
		}
	}

	/* VERY difficult: Optimal placement of jobs in the matrix
	 * - how to order jobs to be added to the matrix?
	 *   - "by size" does not guarantee optimal placement
	 *
	 *   - for now, try sorting jobs by first bit set
	 *     - if job allocations stay "in blocks", then this should work OK
	 *     - may still get scenarios where jobs should switch rows
	 *     - fixme: JOB SHUFFLING BETWEEN ROWS NEEDS TESTING
	 */
	qsort(ss, num_jobs, sizeof(struct sort_support), _compare_support);
	if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		for (i = 0; i < num_jobs; i++) {
			char cstr[64], nstr[64];
			if (ss[i].tmpjobs->core_bitmap) {
				bit_fmt(cstr, (sizeof(cstr)-1) ,
					ss[i].tmpjobs->core_bitmap);
			} else
				sprintf(cstr, "[no core_bitmap]");
			if (ss[i].tmpjobs->node_bitmap) {
				bit_fmt(nstr, (sizeof(nstr)-1),
					ss[i].tmpjobs->node_bitmap);
			} else
				sprintf(nstr, "[no node_bitmap]");
			info("DEBUG:  jstart %d job nb %s cb %s",
			     ss[i].jstart, nstr, cstr);
		}
	}

	/* add jobs to the rows */
	for (j = 0; j < num_jobs; j++) {
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (_can_job_fit_in_row(ss[j].tmpjobs,
						&(p_ptr->row[i]))) {
				/* job fits in row, so add it */
				common_add_job_to_row(ss[j].tmpjobs,
						      &(p_ptr->row[i]));
				ss[j].tmpjobs = NULL;
				break;
			}
		}
		/* job should have been added, so shuffle the rows */
		common_sort_part_rows(p_ptr);
	}

	/* test for dangling jobs */
	for (j = 0; j < num_jobs; j++) {
		if (ss[j].tmpjobs)
			break;
	}
	if (j < num_jobs) {
		/* we found a dangling job, which means our packing
		 * algorithm couldn't improve apon the existing layout.
		 * Thus, we'll restore the original layout here */
		debug3("cons_res: build_row_bitmap: dangling job found");

		if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
			info("DEBUG: _build_row_bitmaps (post-algorithm):");
			common_dump_parts(p_ptr);
		}

		common_destroy_row_data(p_ptr->row, p_ptr->num_rows);
		p_ptr->row = orig_row;
		orig_row = NULL;

		/* still need to rebuild row_bitmaps */
		for (i = 0; i < p_ptr->num_rows; i++) {
			if (p_ptr->row[i].first_row_bitmap)
				bit_nclear(p_ptr->row[i].first_row_bitmap, 0,
					   size-1);
			if (p_ptr->row[i].num_jobs == 0)
				continue;
			for (j = 0; j < p_ptr->row[i].num_jobs; j++) {
				_add_job_to_cores(p_ptr->row[i].job_list[j],
						  &p_ptr->row[i],
						  cr_node_num_cores);
			}
		}
	}

	if (select_debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		info("DEBUG: _build_row_bitmaps (after):");
		common_dump_parts(p_ptr);
	}

	if (orig_row)
		common_destroy_row_data(orig_row, p_ptr->num_rows);
	xfree(ss);

	return;

	/* LEFTOVER DESIGN THOUGHTS, PRESERVED HERE */

	/* 1. sort jobs by size
	 * 2. only load core bitmaps with largest jobs that conflict
	 * 3. sort rows by set count
	 * 4. add remaining jobs, starting with fullest rows
	 * 5. compute  set count: if disparity between rows got closer, then
	 *    switch non-conflicting jobs that were added
	 */

	/*
	 *  Step 1: remove empty rows between non-empty rows
	 *  Step 2: try to collapse rows
	 *  Step 3: sort rows by size
	 *  Step 4: try to swap jobs from different rows to pack rows
	 */

	/* WORK IN PROGRESS - more optimization should go here, such as:
	 *
	 * - try collapsing jobs from higher rows to lower rows
	 *
	 * - produce a load array to identify cores with less load. Test
	 * to see if those cores are in the lower row. If not, try to swap
	 * those jobs with jobs in the lower row. If the job can be swapped
	 * AND the lower row set_count increases, then SUCCESS! else swap
	 * back. The goal is to pack the lower rows and "bubble up" clear
	 * bits to the higher rows.
	 */
}

static int
_compare_support(const void *v, const void *v1)
{
	struct sort_support *s;
	struct sort_support *s1;

	s = (struct sort_support *)v;
	s1 = (struct sort_support *)v1;

	if (s->jstart > s1->jstart
		|| (s->jstart == s1->jstart
			&& s->tmpjobs->ncpus > s1->tmpjobs->ncpus))
		return 1;

	return 0;
}
/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	common_init();

	cons_common_callbacks.add_job_to_res = _add_job_to_cores;
	cons_common_callbacks.can_job_fit_in_row = _can_job_fit_in_row;
	cons_common_callbacks.can_job_run_on_node = can_job_run_on_node;
	cons_common_callbacks.choose_nodes = choose_nodes;
	cons_common_callbacks.mark_avail_cores = make_core_bitmap;
	cons_common_callbacks.build_row_bitmaps = _build_row_bitmaps;
	cons_common_callbacks.dist_tasks_compute_c_b = dist_tasks_compute_c_b;

	return SLURM_SUCCESS;
}

extern int fini(void)
{
	common_fini();

	return SLURM_SUCCESS;
}

/*
 * The remainder of this file implements the standard Slurm
 * node selection API.
 */

/* select_p_state_save() in cons_common */

/* select_p_state_restore() in cons_common */

/* select_p_job_init() in cons_common */

/* select_p_node_ranking() in cons_common */

/* select_p_node_init() in cons_common */

/* select_p_block_init() in cons_common */

/*
 * select_p_job_test - Given a specification of scheduling requirements,
 *	identify the nodes which "best" satisfy the request.
 * 	"best" is defined as either a minimal number of consecutive nodes
 *	or if sharing resources then sharing them with a job of similar size.
 * IN/OUT job_ptr - pointer to job being considered for initiation,
 *                  set's start_time when job expected to start
 * IN/OUT bitmap - usable nodes are set on input, nodes not required to
 *	satisfy the request are cleared, other left set
 * IN min_nodes - minimum count of nodes
 * IN req_nodes - requested (or desired) count of nodes
 * IN max_nodes - maximum count of nodes (0==don't care)
 * IN mode - SELECT_MODE_RUN_NOW   (0): try to schedule job now
 *           SELECT_MODE_TEST_ONLY (1): test if job can ever run
 *           SELECT_MODE_WILL_RUN  (2): determine when and where job can run
 * IN preemptee_candidates - List of pointers to jobs which can be preempted.
 * IN/OUT preemptee_job_list - Pointer to list of job pointers. These are the
 *		jobs to be preempted to initiate the pending job. Not set
 *		if mode=SELECT_MODE_TEST_ONLY or input pointer is NULL.
 * RET zero on success, EINVAL otherwise
 * globals (passed via select_p_node_init):
 *	node_record_count - count of nodes configured
 *	node_record_table_ptr - pointer to global node table
 * NOTE: the job information that is considered for scheduling includes:
 *	req_node_bitmap: bitmap of specific nodes required by the job
 *	contiguous: allocated nodes must be sequentially located
 *	num_cpus: minimum number of processors required by the job
 * NOTE: bitmap must be a superset of req_nodes at the time that
 *	select_p_job_test is called
 */
extern int select_p_job_test(struct job_record *job_ptr, bitstr_t * bitmap,
			     uint32_t min_nodes, uint32_t max_nodes,
			     uint32_t req_nodes, uint16_t mode,
			     List preemptee_candidates,
			     List *preemptee_job_list,
			     bitstr_t *exc_core_bitmap)
{
	xassert(bitmap);

	debug2("%s for %pJ", __func__, job_ptr);

	if (!job_ptr->details)
		return EINVAL;

	return common_job_test(job_ptr, bitmap, min_nodes, max_nodes,
			       req_nodes, mode, preemptee_candidates,
			       preemptee_job_list, &exc_core_bitmap);
}

/* select_p_job_begin() in cons_common */

/* select_p_job_ready() in cons_common */

/* select_p_job_resized() in cons_common */

/* select_p_job_expand() in cons_common */

/* select_p_job_signal() in cons_common */

/* select_p_job_mem_confirm() in cons_common */

/* select_p_job_fini() in cons_common */

/* select_p_job_suspend() in cons_common */

/* select_p_job_resume() in cons_common */

/* select_p_step_pick_nodes() in cons_common */

/* select_p_step_start() in cons_common */

/* select_p_step_finish() in cons_common */

/* select_p_select_nodeinfo_pack() in cons_common */

/* select_p_select_nodeinfo_unpack() in cons_common */

/* select_p_select_nodeinfo_alloc() in cons_common */

/* select_p_select_nodeinfo_free() in cons_common */

/* select_p_select_nodeinfo_set_all() in cons_common */

/* select_p_select_nodeinfo_set() in cons_common */

/* select_p_select_nodeinfo_get() in cons_common */

/* select_p_select_jobinfo_alloc() in cons_common */

/* select_p_select_jobinfo_free() in cons_common */

/* select_p_select_jobinfo_set() in cons_common */

/* select_p_select_jobinfo_get() in cons_common */

/* select_p_select_jobinfo_copy() in cons_common */

/* select_p_select_jobinfo_pack() in cons_common */

/* select_p_select_jobinfo_unpack() in cons_common */

/* select_p_select_jobinfo_sprint() in cons_common */

/* select_p_select_jobinfo_xstrdup() in cons_common */

/* select_p_get_info_from_plugin() in cons_common */

/* select_p_update_node_config() in cons_common */

/* select_p_update_node_state() in cons_common */

/* select_p_reconfigure() in cons_common */

/* given an "avail" node_bitmap, return a corresponding "avail" core_bitmap */
/* DUPLICATE CODE: see job_test.c */
/* Adding a filter for setting cores based on avail bitmap */
bitstr_t *_make_core_bitmap_filtered(bitstr_t *node_map, int filter)
{
	uint32_t c, size;
	uint32_t coff;
	int n, n_first, n_last, nodes;

	nodes = bit_size(node_map);
	size = cr_get_coremap_offset(nodes);
	bitstr_t *core_map = bit_alloc(size);
	if (!core_map)
		return NULL;

	if (!filter)
		return core_map;

	n_first = bit_ffs(node_map);
	if (n_first == -1)
		n_last = -2;
	else
		n_last = bit_fls(node_map);
	for (n = n_first; n <= n_last; n++) {
		if (bit_test(node_map, n)) {
			c = cr_get_coremap_offset(n);
			coff = cr_get_coremap_offset(n + 1);
			while (c < coff) {
				bit_set(core_map, c++);
			}
		}
	}
	return core_map;
}

/* Once here, if core_cnt is NULL, avail_bitmap has nodes not used by any job or
 * reservation */
bitstr_t *_sequential_pick(bitstr_t *avail_bitmap, uint32_t node_cnt,
			   uint32_t *core_cnt, bitstr_t **core_bitmap)
{
	bitstr_t *sp_avail_bitmap;
	char str[300];
	uint32_t cores_per_node = 0, extra_cores_needed = 0;
	bitstr_t *tmpcore;
	int total_core_cnt = 0;

	/* We have these cases here:
	 *	1) Reservation requests using just number of nodes
	 *		- core_cnt is null
	 *	2) Reservations request using number of nodes + number of cores
	 *	3) Reservations request using node list
	 *		- node_cnt is 0
	 *		- core_cnt is null
	 *	4) Reservation request using node list + number of cores list
	 *		- node_cnt is 0
	 */

	if ((node_cnt) && (core_cnt)) {
		total_core_cnt = core_cnt[0];
		cores_per_node = core_cnt[0] / MAX(node_cnt, 1);
		debug2("Reserving %u cores across %d nodes",
			total_core_cnt, node_cnt);
		extra_cores_needed = total_core_cnt -
				     (cores_per_node * node_cnt);
	}
	if ((!node_cnt) && (core_cnt)) {
		int num_nodes = bit_set_count(avail_bitmap);
		int i;
		bit_fmt(str, (sizeof(str) - 1), avail_bitmap);
		debug2("Reserving cores from nodes: %s", str);
		for (i = 0; (i < num_nodes) && core_cnt[i]; i++)
			total_core_cnt += core_cnt[i];
	}

	debug2("Reservations requires %d cores (%u each on %d nodes, plus %u)",
	       total_core_cnt, cores_per_node, node_cnt, extra_cores_needed);

	sp_avail_bitmap = bit_alloc(bit_size(avail_bitmap));
	bit_fmt(str, (sizeof(str) - 1), avail_bitmap);
	bit_fmt(str, (sizeof(str) - 1), sp_avail_bitmap);

	if (core_cnt) { /* Reservation is using partial nodes */
		int node_list_inx = 0;

		debug2("Reservation is using partial nodes");

		_spec_core_filter(avail_bitmap, core_bitmap);
		tmpcore = bit_copy(*core_bitmap);

		bit_not(tmpcore); /* tmpcore contains now current free cores */
		bit_fmt(str, (sizeof(str) - 1), tmpcore);
		debug2("tmpcore contains just current free cores: %s", str);
		bit_and(*core_bitmap, tmpcore);	/* clear core_bitmap */

		while (total_core_cnt) {
			int inx, coff, coff2;
			int i;
			int cores_in_node;
			int local_cores;

			if (node_cnt == 0) {
				cores_per_node = core_cnt[node_list_inx];
				if (cores_per_node == 0)
					break;
			}

			inx = bit_ffs(avail_bitmap);
			if (inx < 0)
				break;
			debug2("Using node %d", inx);

			coff = cr_get_coremap_offset(inx);
			coff2 = cr_get_coremap_offset(inx + 1);
			local_cores = coff2 - coff;

			bit_clear(avail_bitmap, inx);

			if (local_cores < cores_per_node) {
				debug2("Skip node %d (local: %d, needed: %d)",
					inx, local_cores, cores_per_node);
				continue;
			}

			cores_in_node = 0;

			/* First let's see in there are enough cores in
			 * this node */
			for (i = 0; i < local_cores; i++) {
				if (bit_test(tmpcore, coff + i))
					cores_in_node++;
			}
			if (cores_in_node < cores_per_node) {
				debug2("Skip node %d (avail: %d, needed: %d)",
					inx, cores_in_node, cores_per_node);
				continue;
			}

			debug2("Using node %d (avail: %d, needed: %d)",
				inx, cores_in_node, cores_per_node);

			cores_in_node = 0;
			for (i = 0; i < local_cores; i++) {
				if (bit_test(tmpcore, coff + i)) {
					bit_set(*core_bitmap, coff + i);
					total_core_cnt--;
					cores_in_node++;
					if (cores_in_node > cores_per_node)
						extra_cores_needed--;
					if ((total_core_cnt == 0) ||
					    ((extra_cores_needed == 0) &&
					     (cores_in_node >= cores_per_node)))
						break;
				}
			}

			if (cores_in_node) {
				/* Add this node to the final node bitmap */
				debug2("Reservation using %d cores in node %d",
				       cores_in_node, inx);
				bit_set(sp_avail_bitmap, inx);
			} else {
				debug2("Reservation NOT using node %d", inx);
			}
			node_list_inx++;
		}
		FREE_NULL_BITMAP(tmpcore);

		if (total_core_cnt) {
			info("reservation request can not be satisfied");
			FREE_NULL_BITMAP(sp_avail_bitmap);
			return NULL;
		}

		bit_fmt(str, (sizeof(str) - 1), *core_bitmap);
		debug2("sequential pick using coremap: %s", str);

	} else { /* Reservation is using full nodes */
		while (node_cnt) {
			int inx;

			inx = bit_ffs(avail_bitmap);
			if (inx < 0)
				break;

			/* Add this node to the final node bitmap */
			bit_set(sp_avail_bitmap, inx);
			node_cnt--;

			/* Clear this node from the initial available bitmap */
			bit_clear(avail_bitmap, inx);
		}

		if (node_cnt) {
			info("reservation request can not be satisfied");
			FREE_NULL_BITMAP(sp_avail_bitmap);
			return NULL;
		}

		bit_fmt(str, (sizeof(str) - 1), sp_avail_bitmap);
		debug2("sequential pick using nodemap: %s", str);
	}

	return sp_avail_bitmap;
}

bitstr_t *_pick_first_cores(bitstr_t *avail_bitmap, uint32_t node_cnt,
			    uint32_t *core_cnt, bitstr_t **core_bitmap)
{
	bitstr_t *sp_avail_bitmap;
	bitstr_t *tmpcore;
	int inx, jnx, first_node, last_node;
	int node_offset = 0;
	int coff, coff2, local_cores;

	if (!core_cnt || (core_cnt[0] == 0))
		return NULL;

	sp_avail_bitmap = bit_alloc(bit_size(avail_bitmap));

	_spec_core_filter(avail_bitmap, core_bitmap);
	tmpcore = bit_copy(*core_bitmap);
	bit_not(tmpcore); /* tmpcore contains now current free cores */
	bit_and(*core_bitmap, tmpcore);	/* clear core_bitmap */

	first_node = bit_ffs(avail_bitmap);
	if (first_node >= 0)
		last_node  = bit_fls(avail_bitmap);
	else
		last_node = first_node - 1;
	for (inx = first_node; inx <= last_node; inx++) {
		coff = cr_get_coremap_offset(inx);
		coff2 = cr_get_coremap_offset(inx + 1);
		local_cores = coff2 - coff;

		bit_clear(avail_bitmap, inx);
		if (local_cores < core_cnt[node_offset])
			local_cores = -1;
		else
			local_cores = core_cnt[node_offset];
		for (jnx = 0; jnx < local_cores; jnx++) {
			if (!bit_test(tmpcore, coff + jnx))
				break;
			bit_set(*core_bitmap, coff + jnx);
		}
		if (jnx < core_cnt[node_offset])
			continue;
		local_cores = coff2 - coff;
		for (jnx = core_cnt[node_offset]; jnx < local_cores; jnx++) {
			bit_clear(tmpcore, coff + jnx);
		}
		bit_set(sp_avail_bitmap, inx);
		if (core_cnt[++node_offset] == 0)
			break;
	}

	FREE_NULL_BITMAP(tmpcore);
	if (core_cnt[node_offset]) {
		info("reservation request can not be satisfied");
		FREE_NULL_BITMAP(sp_avail_bitmap);
	}

	return sp_avail_bitmap;
}

/* Test that sufficient cores are available on the specified node for use
 * IN/OUT core_bitmap - cores which are NOT available for use (i.e. specialized
 *		cores or those already reserved), all cores/bits for the
 *		specified node will be cleared if the available count is too low
 * IN node - index of node to test
 * IN cores_per_node - minimum number of nodes which should be available
 * RET count of cores available on this node
 */
static int _get_avail_core_in_node(bitstr_t *core_bitmap, int node,
				   int cores_per_node)
{
	int coff;
	int total_cores;
	int i;
	int avail = 0;

	coff = cr_get_coremap_offset(node);
	total_cores = cr_node_num_cores[node];

	if (!core_bitmap)
		return total_cores;

	for (i = 0; i < total_cores; i++) {
		if (!bit_test(core_bitmap, coff + i))
			avail++;
	}

	if (avail >= cores_per_node)
		return avail;

	bit_nclear(core_bitmap, coff, coff + total_cores - 1);
	return 0;
}

/* Given available node and core bitmaps, remove all specialized cores
 * node_bitmap IN - Nodes available for use
 * core_bitmap IN/OUT - Cores currently NOT available for use */
static void _spec_core_filter(bitstr_t *node_bitmap, bitstr_t **core_bitmap)
{
	bitstr_t **p_spec_core_map = make_core_bitmap(node_bitmap, NO_VAL16);
	bitstr_t *spec_core_map = *p_spec_core_map;

	*p_spec_core_map = NULL;
	free_core_array(&p_spec_core_map);

	bit_not(spec_core_map);

	xassert(core_bitmap);
	if (*core_bitmap) {
		bit_or(*core_bitmap, spec_core_map);
		bit_free(spec_core_map);
	} else {
		*core_bitmap = spec_core_map;
	}
}

extern bitstr_t * select_p_resv_test(resv_desc_msg_t *resv_desc_ptr,
				     uint32_t node_cnt,
				     bitstr_t *avail_bitmap,
				     bitstr_t **core_bitmap)
{
	bitstr_t **switches_bitmap;		/* nodes on this switch */
	bitstr_t **switches_core_bitmap;	/* cores on this switch */
	int       *switches_cpu_cnt;		/* total CPUs on switch */
	int       *switches_node_cnt;		/* total nodes on switch */
	int       *switches_required;		/* set if has required node */

	bitstr_t  *avail_nodes_bitmap = NULL;	/* nodes on any switch */
	bitstr_t *sp_avail_bitmap;
	int rem_nodes, rem_cores = 0;		/* remaining resources desired */
	int c, i, j, k, n, prev_rem_cores;
	int best_fit_inx, first, last;
	int best_fit_nodes;
	int best_fit_location = 0, best_fit_sufficient;
	bool sufficient;
	int cores_per_node = 1;	/* Minimum cores per node to consider */
	uint32_t *core_cnt, flags, rem_cores_save;
	bool aggr_core_cnt = false, clear_core;

	xassert(avail_bitmap);
	xassert(resv_desc_ptr);

	core_cnt = resv_desc_ptr->core_cnt;
	flags = resv_desc_ptr->flags;

	if ((flags & RESERVE_FLAG_FIRST_CORES) && core_cnt) {
		return _pick_first_cores(avail_bitmap, node_cnt, core_cnt,
					 core_bitmap);
	}

	/* When reservation includes a nodelist we use _sequential_pick code */
	if (!switch_record_cnt || !switch_record_table || !node_cnt)  {
		return _sequential_pick(avail_bitmap, node_cnt, core_cnt,
					core_bitmap);
	}

	/* Use topology state information */
	if (bit_set_count(avail_bitmap) < node_cnt)
		return avail_nodes_bitmap;

	if (core_cnt)
		_spec_core_filter(avail_bitmap, core_bitmap);

	rem_nodes = node_cnt;
	if (core_cnt && core_cnt[1]) {	/* Array of core counts */
		for (j = 0; core_cnt[j]; j++) {
			rem_cores += core_cnt[j];
			if (j == 0)
				cores_per_node = core_cnt[j];
			else if (cores_per_node > core_cnt[j])
				cores_per_node = core_cnt[j];
		}
	} else if (core_cnt) {		/* Aggregate core count */
		rem_cores = core_cnt[0];
		cores_per_node = core_cnt[0] / MAX(node_cnt, 1);
		aggr_core_cnt = true;
	} else if (cr_node_num_cores)
		cores_per_node = cr_node_num_cores[0];
	else
		cores_per_node = 1;
	rem_cores_save = rem_cores;

	/* Construct a set of switch array entries,
	 * use the same indexes as switch_record_table in slurmctld */
	switches_bitmap = xcalloc(switch_record_cnt, sizeof(bitstr_t *));
	switches_core_bitmap = xcalloc(switch_record_cnt, sizeof(bitstr_t *));
	switches_cpu_cnt = xcalloc(switch_record_cnt, sizeof(int));
	switches_node_cnt = xcalloc(switch_record_cnt, sizeof(int));
	switches_required = xcalloc(switch_record_cnt, sizeof(int));

	for (i = 0; i < switch_record_cnt; i++) {
		char str[100];
		switches_bitmap[i] = bit_copy(switch_record_table[i].
					      node_bitmap);
		bit_and(switches_bitmap[i], avail_bitmap);
		switches_node_cnt[i] = bit_set_count(switches_bitmap[i]);

		switches_core_bitmap[i] =
			_make_core_bitmap_filtered(switches_bitmap[i], 1);

		if (*core_bitmap) {
			bit_and_not(switches_core_bitmap[i], *core_bitmap);
		}
		bit_fmt(str, sizeof(str), switches_core_bitmap[i]);
		switches_cpu_cnt[i] = bit_set_count(switches_core_bitmap[i]);
		debug2("switch:%d nodes:%d cores:%d:%s",
		       i, switches_node_cnt[i], switches_cpu_cnt[i], str);
	}

	/* Remove nodes with less available cores than needed */
	if (core_cnt) {
		n = 0;
		for (j = 0; j < switch_record_cnt; j++) {
			first = bit_ffs(switches_bitmap[j]);
			if (first >= 0)
				last = bit_fls(switches_bitmap[j]);
			else
				last = first - 1;
			for (i = first; i <= last; i++) {
				if (!bit_test(switches_bitmap[j], i))
					continue;

				c = _get_avail_core_in_node(*core_bitmap, i,
							    cores_per_node);
				clear_core = false;
				if (aggr_core_cnt && (c < cores_per_node)) {
					clear_core = true;
				} else if (aggr_core_cnt) {
					;
				} else if (c < core_cnt[n]) {
					clear_core = true;
				} else if (core_cnt[n]) {
					n++;
				}
				if (!clear_core)
					continue;
				for (k = 0; k < switch_record_cnt; k++) {
					if (!switches_bitmap[k] ||
					    !bit_test(switches_bitmap[k], i))
						continue;
					bit_clear(switches_bitmap[k], i);
					switches_node_cnt[k]--;
					switches_cpu_cnt[k] -= c;
				}
			}
		}
	}

#if SELECT_DEBUG
	/* Don't compile this, it slows things down too much */
	for (i = 0; i < switch_record_cnt; i++) {
		char *node_names = NULL;
		if (switches_node_cnt[i])
			node_names = bitmap2node_name(switches_bitmap[i]);
		info("switch=%s nodes=%u:%s cpus:%d required:%u speed=%u",
		     switch_record_table[i].name,
		     switches_node_cnt[i], node_names,
		     switches_cpu_cnt[i], switches_required[i],
		     switch_record_table[i].link_speed);
		xfree(node_names);
	}
#endif

	/* Determine lowest level switch satisfying request with best fit */
	best_fit_inx = -1;
	for (j = 0; j < switch_record_cnt; j++) {
		if ((switches_node_cnt[j] < rem_nodes) ||
		    (core_cnt && (switches_cpu_cnt[j] < rem_cores)))
			continue;
		if ((best_fit_inx == -1) ||
		    (switch_record_table[j].level <
		     switch_record_table[best_fit_inx].level) ||
		    ((switch_record_table[j].level ==
		      switch_record_table[best_fit_inx].level) &&
		     (switches_node_cnt[j] < switches_node_cnt[best_fit_inx])))
			/* We should use core count by switch here as well */
			best_fit_inx = j;
	}
	if (best_fit_inx == -1) {
		debug("select_p_resv_test: could not find resources for "
		      "reservation");
		goto fini;
	}

	/* Identify usable leafs (within higher switch having best fit) */
	for (j = 0; j < switch_record_cnt; j++) {
		if ((switch_record_table[j].level != 0) ||
		    (!bit_super_set(switches_bitmap[j],
				    switches_bitmap[best_fit_inx]))) {
			switches_node_cnt[j] = 0;
		}
	}

	/* Select resources from these leafs on a best-fit basis */
	avail_nodes_bitmap = bit_alloc(node_record_count);
	while (rem_nodes > 0) {
		int avail_cores_in_node;
		best_fit_nodes = best_fit_sufficient = 0;
		for (j = 0; j < switch_record_cnt; j++) {
			if (switches_node_cnt[j] == 0)
				continue;
			if (core_cnt) {
				sufficient =
					(switches_node_cnt[j] >= rem_nodes) &&
					(switches_cpu_cnt[j] >= rem_cores);
			} else
				sufficient = switches_node_cnt[j] >= rem_nodes;
			/* If first possibility OR */
			/* first set large enough for request OR */
			/* tightest fit (less resource waste) OR */
			/* nothing yet large enough, but this is biggest */
			if ((best_fit_nodes == 0) ||
			    (sufficient && (best_fit_sufficient == 0)) ||
			    (sufficient &&
			     (switches_node_cnt[j] < best_fit_nodes)) ||
			    ((sufficient == 0) &&
			     (switches_node_cnt[j] > best_fit_nodes))) {
				best_fit_nodes = switches_node_cnt[j];
				best_fit_location = j;
				best_fit_sufficient = sufficient;
			}
		}
		if (best_fit_nodes == 0)
			break;
		/* Use select nodes from this leaf */
		first = bit_ffs(switches_bitmap[best_fit_location]);
		if (first >= 0)
			last  = bit_fls(switches_bitmap[best_fit_location]);
		else
			last = first - 1;
		for (i = first; i <= last; i++) {
			if (!bit_test(switches_bitmap[best_fit_location], i))
				continue;
			bit_clear(switches_bitmap[best_fit_location], i);
			switches_node_cnt[best_fit_location]--;

			if (bit_test(avail_nodes_bitmap, i)) {
				/* node on multiple leaf switches
				 * and already selected */
				continue;
			}

			avail_cores_in_node = 0;
			if (*core_bitmap) {
				int coff;
				coff = cr_get_coremap_offset(i);
				debug2("Testing node %d, core offset %d",
				       i, coff);
				for (j = 0; j < cr_node_num_cores[i]; j++) {
					if (!bit_test(*core_bitmap, coff + j))
						avail_cores_in_node++;
				}
				if (avail_cores_in_node < cores_per_node)
					continue;

				debug2("Using node %d with %d cores available",
				       i, avail_cores_in_node);
			}

			bit_set(avail_nodes_bitmap, i);
			rem_cores -= avail_cores_in_node;
			if (--rem_nodes <= 0)
				break;
		}
		switches_node_cnt[best_fit_location] = 0;
	}
	if ((rem_nodes > 0) || (rem_cores > 0))	/* insufficient resources */
		FREE_NULL_BITMAP(avail_nodes_bitmap);

fini:	for (i = 0; i < switch_record_cnt; i++) {
		FREE_NULL_BITMAP(switches_bitmap[i]);
		FREE_NULL_BITMAP(switches_core_bitmap[i]);
	}

	xfree(switches_bitmap);
	xfree(switches_core_bitmap);
	xfree(switches_cpu_cnt);
	xfree(switches_node_cnt);
	xfree(switches_required);

	if (avail_nodes_bitmap && core_cnt) {
		/* Reservation is using partial nodes */
		bitstr_t *exc_core_bitmap = NULL;

		sp_avail_bitmap = bit_alloc(bit_size(avail_bitmap));
		if (*core_bitmap) {
			exc_core_bitmap = *core_bitmap;
			*core_bitmap = bit_alloc(bit_size(exc_core_bitmap));
		}

		rem_cores = rem_cores_save;
		n = 0;
		prev_rem_cores = -1;
		while (rem_cores) {
			uint32_t coff;
			int inx, i;
			int avail_cores_in_node;

			inx = bit_ffs(avail_nodes_bitmap);
			if ((inx < 0) && aggr_core_cnt && (rem_cores > 0) &&
			    (rem_cores != prev_rem_cores)) {
				/* Make another pass over nodes to reach
				 * requested aggregate core count */
				bit_or(avail_nodes_bitmap, sp_avail_bitmap);
				inx = bit_ffs(avail_nodes_bitmap);
				prev_rem_cores = rem_cores;
				cores_per_node = 1;
			}
			if (inx < 0)
				break;

			debug2("Using node inx %d cores_per_node %d "
			       "rem_cores %u", inx, cores_per_node, rem_cores);
			coff = cr_get_coremap_offset(inx);

			/* Clear this node from the initial available bitmap */
			bit_clear(avail_nodes_bitmap, inx);

			if (cr_node_num_cores[inx] < cores_per_node)
				continue;

			avail_cores_in_node = 0;
			for (i = 0; i < cr_node_num_cores[inx]; i++) {
				if (!bit_test(exc_core_bitmap, coff + i)) {
					avail_cores_in_node++;
				}
			}

			debug2("Node %d has %d available cores", inx,
			       avail_cores_in_node);

			if (avail_cores_in_node < cores_per_node)
				continue;

			avail_cores_in_node = 0;
			for (i = 0; i < cr_node_num_cores[inx]; i++) {
				if (!bit_test(exc_core_bitmap, coff + i)) {
					// info("PICK NODE:%u BIT:%u", inx, i);
					bit_set(*core_bitmap, coff + i);
					bit_set(exc_core_bitmap, coff + i);
					rem_cores--;
					avail_cores_in_node++;
				}

				if (rem_cores == 0)
					break;
				if (aggr_core_cnt &&
				    (avail_cores_in_node >= cores_per_node))
					break;
				if (!aggr_core_cnt &&
				    (avail_cores_in_node >= core_cnt[n]))
					break;
			}

			/* Add this node to the final node bitmap */
			bit_set(sp_avail_bitmap, inx);
			n++;
		}
		FREE_NULL_BITMAP(avail_nodes_bitmap);
		FREE_NULL_BITMAP(exc_core_bitmap);

		if (rem_cores) {
			info("reservation request can not be satisfied");
			FREE_NULL_BITMAP(sp_avail_bitmap);
			return NULL;
		}
		return sp_avail_bitmap;
	}

	return avail_nodes_bitmap;
}
