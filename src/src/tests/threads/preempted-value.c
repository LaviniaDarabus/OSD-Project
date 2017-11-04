#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

static void my_function (void *lock_);

void
test_preempted_value (void)
{
	struct lock lock;
	lock_init (&lock);
	lock_acquire (&lock);
	msg("Testing preempted value...");

	int i = 0;

	for(i = 0; i < 5; i++)
	{
		thread_create("preemption", PRI_DEFAULT, my_function, &lock);
	}
	msg("Threads successfully created...");
    lock_release (&lock);

    for(;;);
}

static void
my_function (void *lock_)
{
	struct lock *lock = lock_;
	int i = 0;
	for(i = 0; i < 10; i++)
	{
		lock_acquire (lock);
		printf("Thread %d preemtions %d\n", thread_current()->tid, thread_current()->preemptions);
	    lock_release (lock);
	}
}

