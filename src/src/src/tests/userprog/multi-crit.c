/*
 * This program checks to see if the critical region is working properly.
 * This is done by increment a global variable and checking if the result is
 * the one expected.
 */

#include <debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include <synch.h>
#include "tests/lib.h"


#define NO_OF_THREADS_TO_CREATE		2
#define TIMES_TO_LOOP				20

int global_var;

int child_thread( struct critical_region* region )
{
	int i;


	for( i = 0; i < TIMES_TO_LOOP; ++i )
	{
		enter_critical_region( region );
		global_var = global_var + 1;
		leave_critical_region( region );
	}

	return 0;
}

int main( int argc, char* argv[] )
{
	int thread_id[NO_OF_THREADS_TO_CREATE];
	int i;
	struct critical_region region;
	global_var = 0;

	test_name = "multi-crit";

	msg ("begin");

	init_critical_region( &region );

	for( i = 0; i < NO_OF_THREADS_TO_CREATE; ++i )
	{
		thread_id[i] = uthread_create( child_thread, &region );
		if( -1 == thread_id[i] )
		{
			fail( "Test creation failed. Test fail" );
		}
	}

	uthread_joinall();
	if( NO_OF_THREADS_TO_CREATE * TIMES_TO_LOOP != global_var )
	{
		fail( "Critical region is not critical" );
	}
	msg ("end");


	return 0;
}
