#include "common_lib.h"
#include "rec_rw_spinlock.h"
#include "lock_common.h"

#ifndef _COMMONLIB_NO_LOCKS_

#define RW_DEFAULT_RECURSIVITY_DEPTH_MAX                2

void
RecRwSpinlockInit(
    IN      BYTE                    RecursivityDepth,
    OUT     PREC_RW_SPINLOCK        Spinlock
    )
{
    BYTE recursivityDepth;

    ASSERT(NULL != Spinlock);

    memzero(Spinlock, sizeof(REC_RW_SPINLOCK));

    recursivityDepth = (0 == RecursivityDepth) ? RW_DEFAULT_RECURSIVITY_DEPTH_MAX : RecursivityDepth;

    RwSpinlockInit(&Spinlock->RwSpinlock);

    Spinlock->MaxRecursivityDepth = recursivityDepth;
}

REQUIRES_NOT_HELD_LOCK(*Spinlock)
_When_(Exclusive, ACQUIRES_EXCL_AND_REENTRANT_LOCK(*Spinlock))
_When_(!Exclusive, ACQUIRES_SHARED_AND_NON_REENTRANT_LOCK(*Spinlock))
void
RecRwSpinlockAcquire(
    INOUT   PREC_RW_SPINLOCK    Spinlock,
    OUT     INTR_STATE*         IntrState,
    IN      BOOLEAN             Exclusive
    )
{
    PVOID pCurrentCpu;
    INTR_STATE dummy;

    ASSERT(NULL != Spinlock);
    ASSERT(NULL != IntrState);

    *IntrState = CpuIntrDisable();
    pCurrentCpu = CpuGetCurrent();

    if (Exclusive)
    {
        if (Spinlock->Holder == pCurrentCpu)
        {
            ASSERT_INFO( Spinlock->CurrentRecursivityDepth < Spinlock->MaxRecursivityDepth,
                        "Current recursivity depth: %u\nMax recursivity depth: %u\n",
                        Spinlock->CurrentRecursivityDepth,
                        Spinlock->MaxRecursivityDepth
                        );
            Spinlock->CurrentRecursivityDepth++;
            return;
        }
    }

    // if we get here => we need to call RwSpinlockAcquire
    RwSpinlockAcquire(&Spinlock->RwSpinlock, &dummy, Exclusive );

    ASSERT( NULL == Spinlock->Holder );
    ASSERT( 0 == Spinlock->CurrentRecursivityDepth );

    if (Exclusive)
    {
        Spinlock->CurrentRecursivityDepth = 1;
        Spinlock->Holder = CpuGetCurrent();
    }
}

_When_(Exclusive, REQUIRES_EXCL_LOCK(*Spinlock) RELEASES_EXCL_AND_REENTRANT_LOCK(*Spinlock))
_When_(!Exclusive, REQUIRES_SHARED_LOCK(*Spinlock) RELEASES_SHARED_AND_NON_REENTRANT_LOCK(*Spinlock))
void
RecRwSpinlockRelease(
    INOUT   PREC_RW_SPINLOCK    Spinlock,
    IN      INTR_STATE          IntrState,
    IN      BOOLEAN             Exclusive
    )
{
    PVOID pCurrentCpu;

    ASSERT( NULL != Spinlock );
    ASSERT(INTR_OFF == CpuIntrGetState());

    pCurrentCpu = CpuGetCurrent();

    if (Exclusive)
    {
        ASSERT(pCurrentCpu == Spinlock->Holder);
        Spinlock->CurrentRecursivityDepth--;
    }

    if (0 == Spinlock->CurrentRecursivityDepth || !Exclusive)
    {
        Spinlock->Holder = NULL;

        _Analysis_assume_lock_held_(*Spinlock);
        RwSpinlockRelease(&Spinlock->RwSpinlock, IntrState, Exclusive );
    }
}

#endif // _COMMONLIB_NO_LOCKS_
