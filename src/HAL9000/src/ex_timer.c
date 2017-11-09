#include "HAL9000.h"
#include "ex_timer.h"
#include "iomu.h"
#include "thread_internal.h"

STATUS
ExTimerInit(
    OUT     PEX_TIMER       Timer,
    IN      EX_TIMER_TYPE   Type,
    IN      QWORD           Time
    )
{
    STATUS status;

    if (NULL == Timer)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (Type > ExTimerTypeMax)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    status = STATUS_SUCCESS;

    memzero(Timer, sizeof(EX_TIMER));

    Timer->Type = Type;
    if (Timer->Type != ExTimerTypeAbsolute)
    {
        // relative time

        // if the time trigger time has already passed the timer will
        // be signaled after the first scheduler tick
        Timer->TriggerTimeUs = IomuGetSystemTimeUs() + Time;
        Timer->ReloadTimeUs = Time;
    }
    else
    {
        // absolute
        Timer->TriggerTimeUs = Time;
    }

    return status;
}

void
ExTimerStart(
    IN      PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = TRUE;
}

void
ExTimerStop(
    IN      PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = FALSE;
}

void
ExTimerWait(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }
	PTHREAD pCurrentThread;

	//	INTR_STATE dummyState;

		
	pCurrentThread = GetCurrentThread();
	//LockAcquire(&Timer->TimerLock, &dummyState);

	if (Timer->Type != ExTimerTypeAbsolute)
	{
		//rotunjesc in sus
		pCurrentThread->timerCountTicks = (Timer->ReloadTimeUs + (IomuGetShedulerTimerInterrupt() / 2)) / IomuGetShedulerTimerInterrupt();
	}
	else
	{
		pCurrentThread->timerCountTicks = (Timer->TriggerTimeUs + (IomuGetShedulerTimerInterrupt() / 2)) / IomuGetShedulerTimerInterrupt();
	}
		
	if (Timer->TimerStarted && pCurrentThread->timerCountTicks > 0) {
		_InterlockedExchange8(&pCurrentThread->timerON, TRUE);
		LOGTPL(" inainte sa intre in event+nr ticks %d:    ", pCurrentThread->timerCountTicks);
		WaitSignalTimerSleep();
		
	}
		//LockRelease(&Timer->TimerLock, dummyState);
}

void
ExTimerUninit(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    ExTimerStop(Timer);

    Timer->TimerUninited = TRUE;
}

INT64
ExTimerCompareTimers(
    IN      PEX_TIMER     FirstElem,
    IN      PEX_TIMER     SecondElem
)
{
    return FirstElem->TriggerTimeUs - SecondElem->TriggerTimeUs;
}