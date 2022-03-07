// SPDX-License-Identifier: LGPL-2.1+

#ifndef EDISKSD_TASK_H
#define EDISKSD_TASK_H

typedef void (*task_run_t)(void *);
typedef void (*task_cb_t)(void *);

int task_init(void);
void task_exit(void);

int task_start(task_run_t run, task_cb_t done, void *data);

#endif
