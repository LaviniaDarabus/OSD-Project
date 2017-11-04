/*
 * This program checks to see if the TID's are assigned correctly to the child threads.
 * The TIDs must have consecutive values.
 */

#include <debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"


#define NO_OF_THREADS_TO_CREATE		5

int child_thread( int* argv )
{
	return 0;
}

int main( int argc, char* argv[] )
{
	int thread_id[NO_OF_THREADS_TO_CREATE];
	int status;
	int i;

	test_name = "multi-tid";

	msg ("begin");

	for( i = 0; i < NO_OF_THREADS_TO_CREATE; ++i )
	{
		thread_id[i] = uthread_create( child_thread, &thread_id[i] );
		if( -1 == thread_id )
		{
			fail( "Test creation failed. Test fail" );
		}
	}

	for( i = 1; i < NO_OF_THREADS_TO_CREATE; ++i )
	{
		if( thread_id[i] != thread_id[i-1] + 1 )
		{
			fail( "TID's should be consecutive" );
		}
	}

	uthread_joinall();

	msg( "end" );


	return 0;
}
