#ifndef TARANTOOL_LIB_CORE_SHMEM_H_INCLUDED
#define TARANTOOL_LIB_CORE_SHMEM_H_INCLUDED
/*
 * Copyright 2010-2016 Tarantool AUTHORS: please see AUTHORS file.
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
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#include <stdbool.h>
#include <sys/types.h>
#include "small/small.h"
#include "small/rb.h"
#include "src/lib/uuid/tt_uuid.h"

/**
 * When configuring the database, create a shared memory segment,
 * in which store the information about the current instance:
 * initially, cpu id of the main thread later, checkpoint
 * daemon schedule.
 * If the segment already exists, read values of other instances
 * in the segment, and adjust own settings accordingly.
 * The segment is identifier by cluster UUID, but the segment name
 * can be reset in box.cfg{}.
 * The entire behaviour can be switched off in box.cfg.
 */
struct instance_info {
	/** Replicaset uuid */
	char uuid[UUID_STR_LEN + 1];
	/** CPU on which replicaset runs */
	int cpu_id;
	/** Instance pid;
	 * required to check if the process is alive */
	pid_t pid;
	/**
	 * The interval between checkpoints, in seconds.
	 * Every instance sets it's own checkpoint schedule start_time
	 * with inuque random deviation(see checkpoint_schedule_cfg() function).
	 * There are several cases where intervals for different
	 * instances can exactly match, this kills randomness.
	 * If so, we need to save the schedule start_time for every instance
	 * and check the sched_start_time value for all
	 * node_info items before assigning new generated value
	 * in checkpoint_schedule_cfg, and generate another one
	 * if a match is found with any of the elements already present.
	 */
	double sched_start_time;
};
struct node_info {
	/** Cluster uuid, it is common to all instances */
	char name[UUID_STR_LEN + 1];
	/** Max number if items that node_info can contain.
	 * Any value can be set, but it will be limited
	 * by the number of available processors */
	int max_num_of_items;
	/** The current number of items within node_info */
	int curr_num_of_items;
	/** Start address for instance_info array */
	void *inst_info_data;
	/** The size of allocated instance_info array.
	 * Must be equal to
	 * sizeof(struct instance_info) * max_num_of_items */
	size_t alloc_mem_size;
	/** The general mask of binded CPUs.
	 * it cannot be used to bind an individual CPU. */
	cpu_set_t cpu_set;
};

struct node_info *
node_info_init(const char *cluster_uuid, int num_of_items);

int
node_info_close(const char *cluster_uuid);

struct instance_info *
node_info_find_or_create_instance(const char *instance_uuid);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif  /* TARANTOOL_LIB_CORE_SHMEM_H_INCLUDED */
