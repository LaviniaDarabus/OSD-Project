#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");

  int syscall_no = ((int*)f->esp)[0];

  printf ("SYS_GET_PREEMPTIONS = %d\nsyscall_no = %d\n\n", SYS_GET_PREEMPTIONS, syscall_no);

  switch(syscall_no)
  {
  case SYS_WRITE:
  {
	  f->eax = 0;
	  break;
  }

     case SYS_GET_PREEMPTIONS:
	 {
	   printf("Threads' number of preemptions %d\n", thread_current()->preemptions);
	   f->eax = thread_current()->preemptions;
	   break;
	 }
  }
}
