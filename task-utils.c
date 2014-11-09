#include <pthread.h>
#include <sys/timerfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "task-util.h"

struct task_info *task_init(void *(*threadfn)(void *), int (*postfn)(void *),
			    void *thread_private)
{
	struct task_info *info = calloc(1, sizeof(struct task_info));

	if (!info)
		return NULL;

	info->private_data = thread_private;
	info->threadfn = threadfn;
	info->postfn = postfn;

	return info;
}

int task_start(struct task_info *info)
{
	int ret;

	if (!info)
		return -1;

	if (!info->threadfn)
		return -1;

	ret = pthread_create(&info->id, NULL, info->threadfn,
			     info->private_data);

	if (ret == 0)
		pthread_detach(info->id);
	else
		info->id = -1;

	return ret;
}

void task_stop(struct task_info *info)
{
	if (!info)
		return;

	if (info->periodic.timer_fd)
		close(info->periodic.timer_fd);

	if (info->id > 0)
		pthread_cancel(info->id);

	if (info->postfn)
		info->postfn(info->private_data);
}

void task_deinit(struct task_info *info)
{
	if (!info)
		return;

	free(info);
}

int task_period_start(struct task_info *info, unsigned int period_ms)
{
	unsigned int ns;
	unsigned int sec;
	struct itimerspec itval;

	if (!info)
		return -1;

	info->periodic.timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (info->periodic.timer_fd == -1)
		return info->periodic.timer_fd;

	info->periodic.wakeups_missed = 0;

	sec = period_ms/1000;
	ns = (period_ms - (sec * 1000)) * 1000;
	itval.it_interval.tv_sec = sec;
	itval.it_interval.tv_nsec = ns;
	itval.it_value.tv_sec = sec;
	itval.it_value.tv_nsec = ns;

	return timerfd_settime(info->periodic.timer_fd, 0, &itval, NULL);
};

void task_period_wait(struct task_info *info)
{
	unsigned long long missed;
	int ret;

	if (!info)
		return;

	ret = read(info->periodic.timer_fd, &missed, sizeof (missed));
	if (ret == -1) {
		perror ("read timer");
		return;
	}

	if (missed > 0)
		info->periodic.wakeups_missed += (missed - 1);
}

void task_period_stop(struct task_info *info)
{
	if (!info)
		return;

	if (info->periodic.timer_fd) {
		timerfd_settime(info->periodic.timer_fd, 0, NULL, NULL);
		close(info->periodic.timer_fd);
	}
}
