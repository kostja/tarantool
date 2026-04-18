/*
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vy_compaction_plan.h"

#include <stdlib.h>

#include "vy_run.h"

int
vy_stitch_plan_build(struct vy_slice **slices, int slice_count,
		     struct vy_entry range_begin,
		     struct vy_entry range_end,
		     struct key_def *cmp_def,
		     int min_copy_pages,
		     struct vy_stitch_plan *plan)
{
	/*
	 * Stub: emit a single MERGE segment covering the whole
	 * range. Follow-up commits flesh the planner out to
	 * detect non-overlapping last-level page runs and emit
	 * COPY segments for them.
	 */
	(void)slices;
	(void)slice_count;
	(void)cmp_def;
	(void)min_copy_pages;

	plan->segs = NULL;
	plan->count = 0;
	if (slice_count == 0)
		return 0;

	plan->segs = calloc(1, sizeof(*plan->segs));
	if (plan->segs == NULL)
		return -1;
	plan->segs[0].type = VY_STITCH_MERGE;
	plan->segs[0].merge.begin = range_begin;
	plan->segs[0].merge.end = range_end;
	plan->count = 1;
	return 0;
}

void
vy_stitch_plan_destroy(struct vy_stitch_plan *plan)
{
	free(plan->segs);
	plan->segs = NULL;
	plan->count = 0;
}
