// SPDX-License-Identifier: LGPL-2.1+

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/poll.h>

#include <ell/ell.h>

#include "src/edisksd.h"

#include "src/task.h"

struct task {
	task_run_t run;
	task_cb_t done;
	void *data;
};

static struct l_queue *task_queue;
static struct l_queue *completed_tasks;
static pthread_mutex_t task_mutex;
static struct l_io *task_io;
static int task_eventfd;

static void tasks_done(void)
{
	struct task *task;
	uint64_t count;

	if (read(task_eventfd, &count, sizeof(count)) != sizeof(count)) {
		if (errno != -EAGAIN)
			l_warn("failed to read eventfd");
		return;
	}

	pthread_mutex_lock(&task_mutex);
	while ((task = l_queue_pop_head(completed_tasks))) {
		pthread_mutex_unlock(&task_mutex);
		if (task->done)
			task->done(task->data);
		l_free(task);
		pthread_mutex_lock(&task_mutex);
	}
	pthread_mutex_unlock(&task_mutex);
}

static bool task_read(struct l_io *io, void *user_data)
{
	tasks_done();
	return true;
}

static void *task_run(void *data)
{
	struct task *task = data;
	uint64_t count = 1;

	task->run(task->data);

	pthread_mutex_lock(&task_mutex);
	l_queue_remove(task_queue, task);
	l_queue_push_tail(completed_tasks, task);
	pthread_mutex_unlock(&task_mutex);

	if (write(task_eventfd, &count, sizeof(count)) != sizeof(count))
		l_error("failed to notify task completion: %s", strerror(errno));

	return NULL;
}

int task_start(task_run_t run, task_cb_t done, void *data)
{
	struct task *task = l_new(struct task, 1);
	pthread_t thread;
	int ret;

	if (!run)
		return -EINVAL;

	task->run = run;
	task->done = done;
	task->data = data;

	pthread_mutex_lock(&task_mutex);
	l_queue_push_tail(task_queue, task);
	pthread_mutex_unlock(&task_mutex);

	ret = pthread_create(&thread, NULL, task_run, task);
	if (ret < 0) {
		l_error("failed to spawn task: %d", ret);

		pthread_mutex_lock(&task_mutex);
		l_queue_remove(task_queue, task);
		pthread_mutex_unlock(&task_mutex);

		l_free(task);
		return ret;
	}

	pthread_detach(thread);
	return 0;
}

int task_init(void)
{
	int ret;

	task_queue = l_queue_new();
	completed_tasks = l_queue_new();

	task_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (task_eventfd < 0) {
		l_error("failed to create eventfd");
		return -errno;
	}

	task_io = l_io_new(task_eventfd);
	if (!task_io) {
		l_error("failed to create eventfd i/o");
		return -ENOMEM;
	}

	if (!l_io_set_read_handler(task_io, task_read, NULL, NULL)) {
		l_error("failed to set eventfd read handler");
		return -ENOMEM;
	}

	ret = pthread_mutex_init(&task_mutex, NULL);
	if (ret < 0) {
		l_error("failed to create mutex");
		return ret;
	}

	return 0;
}

void task_exit(void)
{
	int ret;

	pthread_mutex_lock(&task_mutex);
	while (!l_queue_isempty(task_queue)) {
		struct pollfd pfd = {
			.fd = task_eventfd,
			.events = POLLIN,
		};

		pthread_mutex_unlock(&task_mutex);

		ret = poll(&pfd, 1, -1);
		if (ret < 0)
			l_warn("poll failed");
		tasks_done();

		pthread_mutex_lock(&task_mutex);
	}
	pthread_mutex_unlock(&task_mutex);
	tasks_done();

	ret = pthread_mutex_destroy(&task_mutex);
	if (ret < 0)
		l_warn("error when destroying mutex");

	close(task_eventfd);

	l_queue_destroy(completed_tasks, NULL);
	l_queue_destroy(task_queue, NULL);
}
