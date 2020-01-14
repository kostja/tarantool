#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>

#include "uuid/tt_uuid.h"
#include "replication.h"
#include "shmem.h"
#include "unit.h"
#include "checkpoint_schedule.h"

static inline bool
feq(double a, double b)
{
	return fabs(a - b) <= 1;
}

void
one_process_check()
{
	header();
	plan(38);

	srand(time(NULL));
	double now = rand();

	struct checkpoint_schedule sched;
	checkpoint_schedule_cfg(&sched, now, 0);

	is(checkpoint_schedule_timeout(&sched, now), 0,
	   "checkpointing disabled - timeout after configuration");

	now += rand();
	is(checkpoint_schedule_timeout(&sched, now), 0,
	   "checkpointing disabled - timeout after sleep");

	checkpoint_schedule_reset(&sched, now);
	is(checkpoint_schedule_timeout(&sched, now), 0,
	   "checkpointing disabled - timeout after reset");

	double intervals[] = { 100, 600, 1200, 1800, 3600, };
	int intervals_len = sizeof(intervals) / sizeof(intervals[0]);
	for (int i = 0; i < intervals_len; i++) {
		double interval = intervals[i];

		checkpoint_schedule_cfg(&sched, now, interval);
		double t = checkpoint_schedule_timeout(&sched, now);
		ok(t >= interval && t <= interval * 2,
		   "checkpoint interval %.0lf - timeout after configuration",
		   interval);

		double t0;
		for (int j = 0; j < 100; j++) {
			checkpoint_schedule_cfg(&sched, now, interval);
			t0 = checkpoint_schedule_timeout(&sched, now);
			if (fabs(t - t0) > interval / 4)
				break;
		}
		ok(fabs(t - t0) > interval / 4,
		   "checkpoint interval %.0lf - initial timeout randomization",
		   interval);

		now += t0 / 2;
		t = checkpoint_schedule_timeout(&sched, now);
		ok(feq(t, t0 / 2),
		   "checkpoint interval %.0lf - timeout after sleep 1",
		   interval);

		now += t0 / 2;
		t = checkpoint_schedule_timeout(&sched, now);
		ok(feq(t, interval),
		   "checkpoint interval %.0lf - timeout after sleep 2",
		   interval);

		now += interval / 2;
		t = checkpoint_schedule_timeout(&sched, now);
		ok(feq(t, interval / 2),
		   "checkpoint interval %.0lf - timeout after sleep 3",
		   interval);

		now += interval;
		t = checkpoint_schedule_timeout(&sched, now);
		ok(feq(t, interval / 2),
		   "checkpoint interval %.0lf - timeout after sleep 4",
		   interval);

		checkpoint_schedule_reset(&sched, now);
		t = checkpoint_schedule_timeout(&sched, now);
		ok(feq(t, interval),
		   "checkpoint interval %.0lf - timeout after reset",
		   interval);
	}

	char cluster_uuid_str[UUID_STR_LEN + 1];
	tt_uuid_to_string(&CLUSTER_UUID, cluster_uuid_str);
	node_info_close(cluster_uuid_str);

	check_plan();
	footer();
}

void
several_processes_check()
{
	header();
	int i = 0, j = 0;
	int n = get_nprocs();
	pid_t pids[n];
	char instance_id_child[n][UUID_STR_LEN + 1];
	while(j < 10) {
		for (i = 0; i < n; ++i) {
			if ((pids[i] = fork()) < 0) {
				say_error("fork");
				abort();
			} else if (pids[i] == 0) {
				sprintf(instance_id_child[i], "24ffedc8-cbae-4f93-a05e-349f3ab70ba%d", i);
				struct tt_uuid uu;
				if (tt_uuid_from_string(instance_id_child[i], &uu) != 0) {
					say_error("wrong uuid\n");
				}
				REPLICASET_UUID = uu;

				char replicaset_uuid_str[UUID_STR_LEN + 1];
				tt_uuid_to_string(&REPLICASET_UUID, replicaset_uuid_str);

				srand(time(NULL));
				double now = rand();
				struct checkpoint_schedule sched;
				checkpoint_schedule_cfg(&sched, now, 123);
				sleep(5);
				exit(0);
			}
		}
		int status;
		for (i = 0; i < n; ++i) {
			if (waitpid(pids[i], &status, 0) == -1 ) {
					perror("waitpid failed");
			}
		}
		i = 1;
		struct node_info *ci;
		char cluster_uuid_str[UUID_STR_LEN + 1];
		tt_uuid_to_string(&CLUSTER_UUID, cluster_uuid_str);
		ci = node_info_init(cluster_uuid_str, get_nprocs());
		struct instance_info *elem = (struct instance_info *)ci->inst_info_data;
		struct instance_info *control_elem = &elem[0];
		while (i != ci->curr_num_of_items) {
			fail_if(fabs(control_elem->sched_start_time - elem[i].sched_start_time) < __DBL_EPSILON__);
			++i;
		}
		node_info_close(cluster_uuid_str);
		j++;
	}
	footer();
}

int
main()
{
	one_process_check();
	several_processes_check();
	return 0;
}
