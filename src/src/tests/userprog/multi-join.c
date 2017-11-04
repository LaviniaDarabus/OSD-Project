/*
 *	This program checks if the join system call works.
 *	This is done by creating a thread and waiting for it to finish.
 */

#include <debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"

#define TIME_TO_SLEEP			1000

int child_thread( int* argv )
{
	msg( "Child about to sleep" );
	uthread_msleep( TIME_TO_SLEEP );
	msg( "Child about to terminate" );

	return 0;
}

int main( int argc, char* argv[] )
{
	int thread_id;
	int status;

	test_name = "multi-join";

	msg ("begin");

	thread_id = uthread_create( child_thread, NULL );
	if( -1 == thread_id )
	{
		fail( "Test creation failed. Test fail" );
	}

	// we will now wait for our child
	if( -1 == uthread_join( thread_id, &status ) )
	{
		fail( "Thread join failed. Test fail" );
	}
	msg( "Child finished" );

	msg ("end");

	return 0;
}
