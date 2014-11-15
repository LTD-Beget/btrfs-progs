
#ifndef __PROGRESS_
#define __PROGRESS_

#include <pthread.h>

struct periodic_info {
	int timer_fd;
	unsigned long long wakeups_missed;
};

struct task_info {
	struct periodic_info periodic;
	pthread_t id;
	void *private_data;
	void *(*threadfn)(void *);
	int (*postfn)(void *);
};

/* task life cycle */
struct task_info *task_init(void *(*threadfn)(void *), int (*postfn)(void *),
			    void *thread_private);
int task_start(struct task_info *info);
void task_stop(struct task_info *info);
void task_deinit(struct task_info *info);

/* periodic life cycle */
int task_period_start(struct task_info *info, unsigned int period_ms);
void task_period_wait(struct task_info *info);
void task_period_stop(struct task_info *info);

#endif /* __PROGRESS_ */

