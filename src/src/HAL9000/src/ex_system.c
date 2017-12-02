#include "HAL9000.h"
#include "ex_system.h"
#include "thread_internal.h"
#include "ex_event.h"
#include "iomu.h"
void
ExSystemTimerTick(
    void
    )
{
	SignalTimerWakeup();
    ThreadTick();
}