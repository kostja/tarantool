/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include "unit.h"
#include "shmem.h"
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <say.h>
#include <shmem.h>

static void
test_shmem_one_bind_one_proc()
{
	header();
	int res;
	struct node_info *ci, *an_ci;
	int n_proc = get_nprocs();
	int curr_proc;
	cpu_set_t set;
	struct instance_info *inst_i;

	char cluster_id[11] = "cluster_id";
	cluster_id[10] = '\0';
	char another_cluster_id[14] = "an_cluster_id";
	another_cluster_id[13] = '\0';

	char instance_id[12] = "instance_id";
	instance_id[11] = '\0';
	char another_instance_id[15] = "an_instance_id";
	another_instance_id[14] = '\0';

	fail_if((ci = node_info_init(cluster_id, n_proc)) == NULL);
	fail_if((an_ci = node_info_init(cluster_id, n_proc)) == NULL);

	fail_if(strcmp(ci->name, cluster_id) !=0);
	fail_if(strcmp(an_ci->name, cluster_id) !=0);

	fail_if((ci = node_info_init(another_cluster_id, n_proc)) != NULL);
	fail_if((an_ci = node_info_init(another_cluster_id, ++n_proc)) != ci);

	fail_if((inst_i = node_info_find_or_create_instance(instance_id)) == NULL);
	fail_if((inst_i->cpu_id) != 0);
	curr_proc = sched_getcpu();
	fail_if(curr_proc != 0);

	curr_proc++;
	CPU_ZERO(&set);
	CPU_SET(curr_proc, &set);
	fail_if((res = sched_setaffinity(0, CPU_SETSIZE, &set)) != 0);
	inst_i = node_info_find_or_create_instance(instance_id);
	fail_if((inst_i->cpu_id) != 0);
	fail_if((res = node_info_close(cluster_id)) != 0);

	fail_if((ci = node_info_init(cluster_id, n_proc)) == NULL);
	fail_if((inst_i = node_info_find_or_create_instance(instance_id)) == NULL);
	fail_if((inst_i->cpu_id) != 0);
	fail_if((inst_i = node_info_find_or_create_instance(another_instance_id)) != NULL);
	fail_if((res = node_info_close(cluster_id)) != 0);

	fail_if((ci = node_info_init(another_cluster_id, n_proc)) == NULL);
	fail_if((inst_i = node_info_find_or_create_instance(another_instance_id)) == NULL);
	fail_if((inst_i->cpu_id) != 0);
	fail_if((res = node_info_close(another_cluster_id)) != 0);

	fail_if((inst_i = node_info_find_or_create_instance(another_instance_id)) != NULL);
	fail_if((res = node_info_close(another_cluster_id)) != -1);

	footer();
}

static void
test_binds_to_all_procs()
{
	header();
	int res;
	static void *ci;
	struct instance_info *inst_i;

	char cluster_id[11] = "cluster_id";
	cluster_id[10] = '\0';

	char instance_id_parent[16] = "instance_parent";
	instance_id_parent[15] = '\0';
	char instance_id_child[10][20];

	int i;
	int n = get_nprocs();
	pid_t pids[n];

	for (i = 0; i < n; ++i) {
		if ((pids[i] = fork()) < 0) {
			say_error("fork");
			abort();
		} else if (pids[i] == 0) {
			fail_if((ci = node_info_init(cluster_id, get_nprocs())) == NULL);
			sprintf(instance_id_child[i], "%d", i);
			fail_if((inst_i = node_info_find_or_create_instance(instance_id_child[i])) == NULL);
			sleep(2);
			exit(sched_getcpu());
		}
	}
	int status;
	int es = 0, es_fin = 0;
	for (i = 0; i < n; ++i) {
		if (waitpid(pids[i], &status, 0) == -1 ) {
				perror("waitpid failed");
		}
		if (WIFEXITED(status)) {
			es = WEXITSTATUS(status);
			es_fin += es;
		}
	}
	fail_if((res = node_info_close(cluster_id)) != 0);
	fail_if(es_fin != (0 + 1 + 2 + 3));
	footer();
}

static void
test_binds_to_all_procs_try_to_exceed_proc_num()
{
	header();
	int res;
	static void *ci;
	struct instance_info *inst_i;

	char cluster_id[11] = "cluster_id";
	cluster_id[10] = '\0';

	char instance_id_parent[16] = "instance_parent";
	instance_id_parent[15] = '\0';
	char instance_id_child[10][20];

	int i;
	pid_t pids[10];
	int n = 10;

	for (i = 0; i < n; ++i) {
		if ((pids[i] = fork()) < 0) {
			say_error("fork");
			abort();
		} else if (pids[i] == 0) {
			fail_if((ci = node_info_init(cluster_id, n)) == NULL);
			sprintf(instance_id_child[i], "%d", i);
			if (i < get_nprocs()) {
				fail_if((inst_i = node_info_find_or_create_instance(instance_id_child[i])) == NULL);
			} else {
				fail_if((inst_i = node_info_find_or_create_instance(instance_id_child[i])) != NULL);
			}
			sleep(10);
			exit(0);
		}
		sleep(1);
	}
	int status;
	for (i = 0; i < n; ++i) {
		if (waitpid(pids[i], &status, 0) == -1 ) {
				perror("waitpid failed");
		}
	}
	fail_if((res = node_info_close(cluster_id)) != 0);
	footer();
}

int is_mmaped(void *ptr, size_t length) {
	FILE *file = fopen("/proc/self/maps", "r");
	char line[1024];
	int result = 0;
	while (!feof(file)) {
		if (fgets(line, sizeof(line) / sizeof(char), file) == NULL) {
			break;
		}
		unsigned long start, end;
		if (sscanf(line, "%lx-%lx", &start, &end) != 2) {
			continue; // could not parse. fail gracefully and try again on the next line.
		}
		unsigned long ptri = (long) ptr;
		if (ptri >= start && ptri + length <= end) {
			result = 1;
			break;
		}
	}
	fclose(file);
	return result;
}

static void
test_binds_to_all_procs_check_proc_check_dead_state()
{
	header();
	int res;
	static void *ci;
	struct instance_info *inst_i;

	char cluster_id[11] = "cluster_id";
	cluster_id[10] = '\0';

	int i;
	int n = 10 * get_nprocs();
	int nn = 9 * get_nprocs();
	pid_t pids[n];
	char instance_id_parent[16] = "instance_parent";
	instance_id_parent[15] = '\0';
	char instance_id_child[n][20];

	for (i = 0; i < n; ++i) {
		if ((pids[i] = fork()) < 0) {
			say_error("fork");
			abort();
		} else if (pids[i] == 0) {
			fail_if((ci = node_info_init(cluster_id, n)) == NULL);
			sprintf(instance_id_child[i], "%d", i);
			if (i >= nn) {
				sleep(1);
				fail_if((inst_i = node_info_find_or_create_instance(instance_id_child[i])) == NULL);
				sleep(1);
				exit(sched_getcpu());
			} else {
				inst_i = node_info_find_or_create_instance(instance_id_child[i]);
				exit(0);
			}
		}
	}
	int status;
	int es = 0, es_fin = 0;
	for (i = 0; i < n; ++i) {
		if (waitpid(pids[i], &status, 0) == -1 ) {
				perror("waitpid failed");
		}
		if (WIFEXITED(status)) {
			es = WEXITSTATUS(status);
			es_fin += es;
		}
	}
	fail_if((res = node_info_close(cluster_id)) != 0);
	fail_if(es_fin != (0 + 1 + 2 + 3));


	footer();
}

static void
test_binds_to_all_procs_check_child_and_parent()
{
	header();
	int res;
	static void *ci;
	struct instance_info *inst_i;

	char cluster_id[11] = "cluster_id";
	cluster_id[10] = '\0';

	int i, curr_proc = 0;
	cpu_set_t set;
	int n = 10 * get_nprocs();
	int nn = 9 * get_nprocs();
	pid_t pids[n];
	char instance_id_parent[16] = "instance_parent";
	instance_id_parent[15] = '\0';
	char instance_id_child[n][20];

	for (i = 0; i < n; ++i) {
		if ((pids[i] = fork()) < 0) {
			say_error("fork");
			abort();
		} else if (pids[i] == 0) {
			fail_if((ci = node_info_init(cluster_id, get_nprocs())) == NULL);
			sprintf(instance_id_child[i], "%d", i);
			if ((inst_i = node_info_find_or_create_instance(instance_id_child[i])) != NULL) {
				exit(sched_getcpu());
			} else {
				exit(0);
			}
		}
	}
	int status;
	int es = 0, es_fin = 0;
	for (i = 0; i < n; ++i) {
		if (waitpid(pids[i], &status, 0) == -1 ) {
				perror("waitpid failed");
		}
		if (WIFEXITED(status)) {
			es = WEXITSTATUS(status);
			es_fin += es;
		}
	}
	fail_if((res = node_info_close(cluster_id)) != 0);
	fail_if(es_fin != (0 + 1 + 2 + 3));

	((curr_proc = sched_getcpu()) == get_nprocs() - 1)? curr_proc-- : curr_proc++;
	CPU_ZERO(&set);
	CPU_SET(curr_proc, &set);
	fail_if((res = sched_setaffinity(0, CPU_SETSIZE, &set)) != 0);
	fail_if((ci = node_info_init(cluster_id, n)) == NULL);
	inst_i = node_info_find_or_create_instance(instance_id_parent);
	fail_if((inst_i->cpu_id) != 0);
	fail_if((res = node_info_close(cluster_id)) != 0);

	footer();
}

int
main(void)
{
	test_shmem_one_bind_one_proc();
	test_binds_to_all_procs();
	test_binds_to_all_procs_try_to_exceed_proc_num();
	test_binds_to_all_procs_check_proc_check_dead_state();
	test_binds_to_all_procs_check_child_and_parent();

	// TODO: what if the library isn't closed and something happens?
	return 0;
}
