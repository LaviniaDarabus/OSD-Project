/*
 * This program checks the joinall functionality.
 * All the child threads should first terminate and only then the
 * call should return.
 */

#include <debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"


#define NO_OF_THREADS_TO_CREATE		2
#define TIME_TO_SLEEP				2000

int child_thread( int* argv )
{
	int tid;

	tid = uthread_gettid();
	uthread_msleep( TIME_TO_SLEEP);
	msg( "Thread about to terminate" );


	return 0;
}

int main( int argc, char* argv[] )
{
	int thread_id[NO_OF_THREADS_TO_CREATE];
	int status;
	int i;

	test_name = "multi-joinall";

	msg ("begin");

	for( i = 0; i < NO_OF_THREADS_TO_CREATE; ++i )
	{
		thread_id[i] = uthread_create( child_thread, NULL );
		if( -1 == thread_id )
		{
			fail( "Test creation failed. Test fail" );
		}
	}

	if( -1 == uthread_joinall() )
	{
		fail( "Join all failed" );
	}

	msg( "Main is finishing" );
	msg( "end" );


	return 0;
}
