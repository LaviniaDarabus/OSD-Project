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
			//_InterlockedDecrement(&pThread->timerCountTicks);
			pThread->timerCountTicks--;
			//LOGPL("Decrementeaza");
			}
		else {
			RemoveEntryList(pEntry);
			ThreadUnblock(pThread);
			_InterlockedExchange8(&pThread->timerON, FALSE);
			
			
			//LOGPL("Deblocheaza threadul");

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
			//insertionSortList(&Event->WaitingList, &pCurrentThread->ReadyList)
			//InsertOrderedList(&Event->WaitingList, &pCurrentThread->ReadyList);
		}
		else {
			InsertTailList(&Event->WaitingList, &pCurrentThread->ReadyList);
		}

		ThreadTakeBlockLock();
		//LOGTPL("a adaugat threadul in waiting list..nr th: %d ", ListSize(&Event->WaitingList));
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