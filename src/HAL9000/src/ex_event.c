#include "HAL9000.h"
#include "ex_event.h"
#include "thread_internal.h"

#include "cpumu.h"

STATUS
ExEventInit(
    OUT     EX_EVENT*     Event,
    IN      EX_EVT_TYPE   EventType,
    IN      BOOLEAN       Signaled
    )
{
    if (NULL == Event)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (EventType >= ExEventTypeReserved)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    LockInit(&Event->EventLock);
    InitializeListHead(&Event->WaitingList);

    Event->EventType = EventType;
    _InterlockedExchange8(&Event->Signaled, Signaled );

    return STATUS_SUCCESS;
}

void ExEventTimerSignal(
	INOUT   EX_EVENT*      Event
)
{
	INTR_STATE oldState;
	PLIST_ENTRY pEntry;
	LIST_ITERATOR it;

	ASSERT(NULL != Event);

	pEntry = NULL;
	LockAcquire(&Event->EventLock, &oldState);

	ListIteratorInit(&Event->WaitingList, &it);
	//LOG("List size: %d", ListSize(&Event->WaitingList));
	_InterlockedExchange8(&Event->Signaled, TRUE);
		while ((pEntry = ListIteratorNext(&it)) != NULL)
		{
		PTHREAD pThread = CONTAINING_RECORD(pEntry, THREAD, ReadyList);
		if (pThread->timerCountTicks > 0) {
		//	LOGPL("Tickuri %u \n" + pThread->timerCountTicks);
			pThread->timerCountTicks--;
			LOGPL("Decrementeaza");
			}
		else {
			RemoveEntryList(pEntry);
			ThreadUnblock(pThread);
			_InterlockedExchange8(&pThread->timerON, FALSE);


			LOGPL("Deblocheaza threadul");

			}
	}
	LockRelease(&Event->EventLock, oldState);
}


void
ExEventSignal(
    INOUT   EX_EVENT*      Event
    )
{
    INTR_STATE oldState;
    PLIST_ENTRY pEntry;

    ASSERT(NULL != Event);

    pEntry = NULL;

    LockAcquire(&Event->EventLock, &oldState);
    _InterlockedExchange8(&Event->Signaled, TRUE);

    for(pEntry = RemoveHeadList(&Event->WaitingList);
        pEntry != &Event->WaitingList;
        pEntry = RemoveHeadList(&Event->WaitingList)
            )
    {
        PTHREAD pThreadToSignal = CONTAINING_RECORD(pEntry, THREAD, ReadyList);
        ThreadUnblock(pThreadToSignal);

        if (ExEventTypeSynchronization == Event->EventType)
        {
            // sorry, we only wake one thread
            // we must not clear the signal here, because the first thread which will
            // wake up will claar it :)
            break;
        }
    }

    LockRelease(&Event->EventLock, oldState);
}

void
ExEventClearSignal(
    INOUT   EX_EVENT*      Event
    )
{
    ASSERT( NULL != Event );

    _InterlockedExchange8(&Event->Signaled, FALSE);
}


static
INT64
(__cdecl _TickCompare) (
	IN      PLIST_ENTRY     FirstElem,
	IN      PLIST_ENTRY     SecondElem,
	IN_OPT  PVOID           Context
	)
{

	ASSERT(NULL != FirstElem);
	ASSERT(NULL != SecondElem);
	ASSERT(Context == NULL);

	PTHREAD thread1 = CONTAINING_RECORD(FirstElem, THREAD, ReadyList);
	PTHREAD thread2 = CONTAINING_RECORD(SecondElem, THREAD, ReadyList);

	if (thread1->timerCountTicks > thread2->timerCountTicks) {
		return -1;
	}
	else {
		if (thread1->timerCountTicks == thread2->timerCountTicks) {
			return 0;
		}
	}

	return 1;
}

static
INT64
(__cdecl _PriorityCompare) (
	IN      PLIST_ENTRY     FirstElem,
	IN      PLIST_ENTRY     SecondElem,
	IN_OPT  PVOID           Context
	)
{

	ASSERT(NULL != FirstElem);
	ASSERT(NULL != SecondElem);
	ASSERT(Context == NULL);

	PTHREAD thread1 = CONTAINING_RECORD(FirstElem, THREAD, ReadyList);
	PTHREAD thread2 = CONTAINING_RECORD(SecondElem, THREAD, ReadyList);

	if (ThreadGetPriority(thread1) > ThreadGetPriority(thread2)) {
		return -1;
	}
	else {
		if (ThreadGetPriority(thread1) == ThreadGetPriority(thread2)) {
			return 0;
		}
	}

	return 1;
}

void
ExEventWaitForSignal(
    INOUT   EX_EVENT*      Event
    )
{
    PTHREAD pCurrentThread;
    INTR_STATE dummyState;
    INTR_STATE oldState;
    BYTE newState;

    ASSERT(NULL != Event);

    pCurrentThread = GetCurrentThread();
    ASSERT( NULL != pCurrentThread);

    newState = ExEventTypeNotification == Event->EventType;

    oldState = CpuIntrDisable();
    while (TRUE != _InterlockedCompareExchange8(&Event->Signaled, newState, TRUE))
    {
        LockAcquire(&Event->EventLock, &dummyState);

		if (pCurrentThread->timerON == TRUE)
		{
			//InsertTailList(&Event->WaitingList, &pCurrentThread->ReadyList);
			InsertOrderedList(&Event->WaitingList, &pCurrentThread->ReadyList, _TickCompare,NULL);
		}
		else {
			InsertOrderedList(&Event->WaitingList, &pCurrentThread->ReadyList, _PriorityCompare, NULL);
		}

		ThreadTakeBlockLock();
		LOGTPL("a adaugat threadul in waiting list..nr th: %d ", ListSize(&Event->WaitingList));
        LockRelease(&Event->EventLock, dummyState);
        ThreadBlock();

        // if we are waiting for a notification type event => all threads
        // must be woken up => we have no reason to check the state of the
        // event again
        if (ExEventTypeNotification == Event->EventType)
        {
            break;
        }
    }

    CpuIntrSetState(oldState);
}
