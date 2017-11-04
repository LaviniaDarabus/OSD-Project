/*
 * This program checks if all the threads belonging to a single process( i.e.
 * created by uthread_create) have the same PID.
 */

#include <debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"



int child_thread( int* argv )
{
	int pid;

	pid = uthread_getpid();

	if( pid != *argv )
	{
		fail( "They should have the same PID" );
	}

	return 0;
}

int main( int argc, char* argv[] )
{
	int thread_id;
	int status;
	int pid;

	test_name = "multi-pid";

	msg ("begin");

	pid = uthread_getpid();

	thread_id = uthread_create( child_thread, &pid );
	if( -1 == thread_id )
	{
		fail( "Test creation failed. Test fail" );
	}

	uthread_joinall();

	msg( "end" );

	return 0;
}
