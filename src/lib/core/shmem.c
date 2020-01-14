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
#include <fcntl.h>
#include <semaphore.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shmem.h"
#include "small/quota.h"
#include "memory.h"
#include "say.h"

static const char *sem_name = "14ffedc8-cbae-4f93-a05e-349f3ab70bac";
static const size_t BIND_CPU_MEM_SIZE = 4 * 1024 * 1024;

static struct node_info *ci = NULL;
static sem_t *sem_id = NULL;

static void
cpu_affinity_mask_clear(cpu_set_t *curr_set)
{
	CPU_ZERO(curr_set);
	for (int i = 0; i < get_nprocs(); ++i) {
		CPU_SET(i, curr_set);
	}
}

void *
get_existing_ci(const char *cluster_uuid)
{
	int fd;
	struct stat sb;
	if (ci != NULL) {
		return ci;
	}
	if ((fd = shm_open(cluster_uuid, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1 && errno == EEXIST) {
		errno = 0;
		if ((fd = shm_open(cluster_uuid, O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
			say_error("shm_open failed");
		}
		if (fstat(fd, &sb) == -1) {
			say_error("fstat failed");
		}
		if ((ci = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
			say_error("mmap failed");
		}
		return ci;
	} else {
		if (shm_unlink(cluster_uuid) == -1) {
			say_error("shm_unlink failed");
		}
	}
	return NULL;
}

struct node_info *
node_info_init(const char *cluster_uuid, int max_num_of_items)
{
	if ((sem_id = sem_open(sem_name, O_CREAT, 0660, 1)) == SEM_FAILED) {
			say_error("sem_open failed");
	}
	if (sem_wait(sem_id) < 0) {
		say_error("sem_wait failed");
	}

	struct node_info *slab_ci = NULL;
	struct node_info *ret = NULL;
	int fd;
	if (ci == NULL) {
		if ((ci = get_existing_ci(cluster_uuid)) != NULL) {
			ret = ci;
			goto final_ret;
		} else {
			if ((fd = shm_open(cluster_uuid, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
				say_error("shm_open failed");
			}
			if (ftruncate(fd, BIND_CPU_MEM_SIZE) == -1) {
				say_error("ftruncate failed");
			}
			if ((ci = mmap(NULL, BIND_CPU_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
				say_error("mmap failed");
			}
		}
		size_t SHMEM_OBJ_SIZE = sizeof(struct instance_info) * max_num_of_items;
		slab_ci = (struct node_info *)malloc(sizeof(struct node_info));
		strcpy(slab_ci->name, cluster_uuid);
		slab_ci->max_num_of_items = max_num_of_items;
		slab_ci->curr_num_of_items = 0;
		slab_ci->inst_info_data = ci + sizeof(struct node_info);
		slab_ci->alloc_mem_size = SHMEM_OBJ_SIZE;
		cpu_affinity_mask_clear(&slab_ci->cpu_set);
		memcpy(ci, slab_ci, sizeof(struct node_info));
		free(slab_ci);
		ret = ci;
	} else if (strcmp(ci->name, cluster_uuid) == 0) {
		say_warn("node_info is already opened with number of elements: "
				 "%d", ci->max_num_of_items);
		ret = ci;
	} else {
		say_warn("ci is already initialized with another name");
		ret = NULL;
	}
final_ret:
	if (sem_post(sem_id) < 0) {
		say_error("sem_post failed");
	}
	return ret;
}

int
node_info_close(const char *cluster_uuid)
{
	if (sem_id == NULL) {
		goto final_ret;
	}
	if (sem_wait(sem_id) < 0) {
		say_error("sem_wait failed");
	}
final_ret:
	if ((ci = get_existing_ci(cluster_uuid)) != NULL) {
		if (strcmp(ci->name, cluster_uuid) == 0) {
			if (shm_unlink(cluster_uuid) == -1) {
				say_error("shm_unlink failed");
			}
			ci = NULL;
			sem_id = NULL;
			sem_unlink(sem_name);
		} else {
			say_error("Wrong name of node_info item");
			return -1;
		}
	} else {
		say_error("node_info does not exists");
		return -1;
	}
	return 0;
}

static int
get_free_cpu(int curr_item, int num_of_proc, cpu_set_t *curr_set)
{
	for (int i = curr_item; i < num_of_proc; ++i) {
		if (!CPU_ISSET(i, curr_set)) {
			return i;
		}
	}
	return -1;
}

static int
cpu_is_free(int cpu_num, cpu_set_t *curr_set)
{
	return CPU_ISSET(cpu_num, curr_set);
}

static void
bind_curr_proc_to_cpu(int cpu_id, cpu_set_t *curr_set)
{
	int res;
	cpu_set_t tmp_cpu_set;

	CPU_ZERO(&tmp_cpu_set);
	CPU_SET(cpu_id, &tmp_cpu_set);
	if ((res = sched_setaffinity(0, CPU_SETSIZE, &tmp_cpu_set)) != 0) {
		say_error("sched_setaffinity failed");
	}
	CPU_CLR(cpu_id, curr_set);
}

struct instance_info *
node_info_find_or_create_instance(const char *instance_uuid)
{
	int n_el, cpu_count;
	struct instance_info *ret = NULL;
	int found_cpu_num = -1;

	if (sem_id == NULL) {
		return NULL;
	}
	if (sem_wait(sem_id) < 0) {
		say_error("sem_wait failed");
	}
	if (ci == NULL) {
		goto final_ret;
	}
	struct instance_info *el_to_replace = NULL,
			*elem = (struct instance_info *)ci->inst_info_data;
	pid_t pid = getpid();

	if ((n_el = ci->curr_num_of_items) == 0) {
		goto put_logic;
	} else if (n_el == ci->max_num_of_items) {
		say_warn("Denied. The number of elements is reached "
				  "max_num_of_items");
		ret = NULL;
		goto final_ret;
	}
	for (int i = 0; i < n_el; ++i) {
		if (i == get_nprocs()) {
			say_warn("Denied. The number of available CPUs "
					  "is less than item count");
			break;
		}
		if (elem[i].pid == pid && strcmp(instance_uuid, elem[i].uuid) != 0) {
			say_warn("The current process is already binded");
			ret = NULL;
			goto final_ret;
		}
		if (strcmp(instance_uuid, elem[i].uuid) == 0) {
			if (cpu_is_free(elem[i].cpu_id, &ci->cpu_set)) {
				bind_curr_proc_to_cpu(elem[i].cpu_id, &ci->cpu_set);
				ret = &elem[i];
				goto final_ret;
			} else {
				if ((found_cpu_num = get_free_cpu(i, n_el, &ci->cpu_set)) != -1) {
					bind_curr_proc_to_cpu(found_cpu_num, &ci->cpu_set);
					elem[i].cpu_id = found_cpu_num;
					elem[i].pid = pid;
					ret = &elem[i];
					goto final_ret;
				} else {
					ret = NULL;
					goto final_ret;
				}
			}
		} else if (el_to_replace == NULL && (kill(elem[i].pid, 0) == -1)
				   && errno == ESRCH) {
			el_to_replace = &elem[i];
			errno = 0;
		}
	}
put_logic:
	if (el_to_replace != NULL) {
		bind_curr_proc_to_cpu(el_to_replace->cpu_id, &ci->cpu_set);
		memset(el_to_replace, 0, sizeof(*el_to_replace));
		strcpy(el_to_replace->uuid, instance_uuid);
		el_to_replace->cpu_id = sched_getcpu();
		el_to_replace->pid = pid;
		ret = el_to_replace;
		goto final_ret;
	}
	cpu_count = ci->max_num_of_items;
	for (int i = 0; i < cpu_count; ++i) {
		if (i == get_nprocs()) {
			say_warn("Denied. All CPUs are busy for binding");
			ret = NULL;
			goto final_ret;
		}
		if (cpu_is_free(i, &ci->cpu_set)) {
			bind_curr_proc_to_cpu(i, &ci->cpu_set);
			strcpy(elem[n_el].uuid, instance_uuid);
			memcpy(&elem[n_el].cpu_id, &i, sizeof(i));
			memcpy(&elem[n_el].pid, &pid, sizeof(pid));
			elem[i].sched_start_time = 0;
			ret = &elem[ci->curr_num_of_items++];
			goto final_ret;
		}
	}

final_ret:
	if (sem_post(sem_id) < 0) {
		say_error("sem_post failed");
	}
	return ret;
}
