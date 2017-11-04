#ifndef _SYNCH_H_
#define _SYNCH_H_

#include <stdint.h>

struct critical_region
{
	volatile int32_t		value;
};


void init_critical_region( struct critical_region* region );

void enter_critical_region( struct critical_region* region );

void leave_critical_region( struct critical_region* region );

#endif // _SYNCH_H_
