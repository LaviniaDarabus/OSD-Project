#include <synch.h>
#include <syscall.h>

void init_critical_region( struct critical_region* region )
{
	region->value = 0;
}

void enter_critical_region( struct critical_region* region )
{
	asm volatile (
				"loop:\n\t"
	            "lock bts $0, (%0)\n\t"
				"jnc finish\n\t"
				"jmp loop\n\t"
				"finish:\n\t"
	            : : "r" (&region->value) : "memory" );
}

void leave_critical_region( struct critical_region* region )
{
	asm volatile (
            "lock decl (%0)"
            : : "r" (&region->value) : "memory" );
}
