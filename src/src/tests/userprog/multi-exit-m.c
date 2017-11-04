/*
 * This program checks if the exit function works correctly when called by the main thread.
 */

#include <debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"


#define TIME_TO_SLEEP			1000

int child_thread( int* argv )
{
	uthread_msleep( TIME_TO_SLEEP );
	fail( "Child should have been terminated already\n" );
}

int main( int argc, char* argv[] )
{
	int thread_id;
	int status;

	test_name = "multi-exit-m";

	msg ("begin");

	thread_id = uthread_create( child_thread, NULL );
	if( -1 == thread_id )
	{
		fail( "Test creation failed. Test fail\n" );
	}

	msg ("end");
	uthread_exit( 0 );
	fail( "We should never be here" );

	return 0;
}
