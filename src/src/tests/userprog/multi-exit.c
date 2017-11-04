/*
 * This program checks if the exit function works correctly for a secondary thread.
 * It checks if after the exit if there is anything else executing
 * and it also checks if the parent thread sees the exit code correctly.
 */

#include <debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"


#define CHILD_EXIT_CODE			93

int child_thread( int* argv )
{
	uthread_exit( CHILD_EXIT_CODE );
	fail( "It should not get here\n" );
}

int main( int argc, char* argv[] )
{
	int thread_id;
	int status;

	test_name = "multi-exit";

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

	if( CHILD_EXIT_CODE != status )
	{
		fail( "Exit code wasn't sent properly" );
	}

	msg ("end");

	return 0;
}
