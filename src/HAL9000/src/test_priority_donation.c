#include "test_common.h"
#include "test_thread.h"
#include "test_priority_donation.h"
#include "mutex.h"
#include "thread_internal.h"
#include "pit.h"

#define TEST_SLEEP_TILL_EXECUTION       (50*MS_IN_US)

static FUNC_ThreadStart _ThreadTakeMutex;
static FUNC_ThreadStart _ThreadChainer;

//******************************************************************************
// Function:     _ThreadValidatePriority
// Description:  Validates the priority of the current thread to match
//               ExpectedPriority.
// Returns:      BOOLEAN - TRUE if the priorities match, FALSE otherwise
// Parameter:    IN THREAD_PRIORITY ExpectedPriority
//******************************************************************************
static
__forceinline
BOOLEAN
_ThreadValidatePriority(
    IN          THREAD_PRIORITY     ExpectedPriority
    )
{
    if (ThreadGetPriority(NULL) != ExpectedPriority)
    {
        LOG_ERROR("Thread should have priority %u, actual priority is %u\n",
                  ExpectedPriority, ThreadGetPriority(NULL));
        return FALSE;
    }

    return TRUE;
}

//******************************************************************************
// Function:     _SpawnThreadAndCheckPriority
// Description:  Creates a new thread and validates that after the new thread
//               was created the initial thread has priority Priority.
// Returns:      STATUS
// Parameter:    IN PFUNC_ThreadStart ThreadFunction
// Parameter:    IN_OPT PVOID Context
// Parameter:    IN THREAD_PRIORITY Priority
// Parameter:    OUT PTHREAD * Thread
// Parameter:    OUT BOOLEAN * PriorityCheckFailed
//******************************************************************************
static
STATUS
_SpawnThreadAndCheckPriority(
    IN          PFUNC_ThreadStart   ThreadFunction,
    IN_OPT      PVOID               Context,
    IN          THREAD_PRIORITY     Priority,
    OUT         PTHREAD*            Thread,
    OUT         BOOLEAN*            PriorityCheckFailed
    )
{
    PTHREAD pThread;
    STATUS status;
    BOOLEAN bPriorityCheckFailed;

    ASSERT(ThreadFunction != NULL);
    ASSERT(Thread != NULL);
    ASSERT(PriorityCheckFailed != NULL);

    pThread = NULL;
    status = STATUS_SUCCESS;
    bPriorityCheckFailed = FALSE;

    __try
    {
        status = ThreadCreate("Donator",
                              Priority,
                              ThreadFunction,
                              Context,
                              &pThread);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("ThreadCreate", status);
            __leave;
        }

        /// This is a stupid hack to make sure the new thread has time to be scheduled
        /// and to donate its priority to us
        PitSleep(TEST_SLEEP_TILL_EXECUTION);

        if (!_ThreadValidatePriority(Priority))
        {
            bPriorityCheckFailed = TRUE;
            __leave;
        }
    }
    __finally
    {
        if (SUCCEEDED(status))
        {
            *Thread = pThread;
            *PriorityCheckFailed = bPriorityCheckFailed;
        }
    }

    return status;
}

STATUS
(__cdecl TestThreadPriorityDonationBasic)(
    IN_OPT      PVOID       Context
    )
{
    STATUS status;
    MUTEX mutex;
    PTHREAD pThread;
    BOOLEAN bCheckFuncCall;
    BOOLEAN bCheckFailed;

    ASSERT(Context != NULL);

    ASSERT(ThreadGetPriority(NULL) == ThreadPriorityDefault);

    bCheckFuncCall = *((BOOLEAN*) Context);

    MutexInit(&mutex, FALSE);
    MutexAcquire(&mutex);

    status = STATUS_SUCCESS;
    pThread = NULL;

    __try
    {
        // Creates a thread to take the mutex and validates that the created thread
        // donated its priority to this thread
        status = _SpawnThreadAndCheckPriority(_ThreadTakeMutex,
                                              &mutex,
                                              ThreadPriorityMaximum,
                                              &pThread,
                                              &bCheckFailed);
        if (!SUCCEEDED(status) || bCheckFailed)
        {
            status = STATUS_ASSERTION_FAILURE;
            __leave;
        }

        if (bCheckFuncCall)
        {
            // Checks to see if its possible to lower the current thread's priority even if a higher priority thread
            // donated its priority: it must NOT be possible.
            ThreadSetPriority(ThreadPriorityLowest);

            if (!_ThreadValidatePriority(ThreadPriorityMaximum))
            {
                LOG_ERROR("Main thread should not be able to lower its priority to %u while it received a donation!\n",
                          ThreadGetPriority(NULL));
                status = STATUS_ASSERTION_FAILURE;
                __leave;
            }
        }

        LOG_TEST_PASS;
    }
    __finally
    {
        MutexRelease(&mutex);

        if (pThread != NULL)
        {
            STATUS exitStatus;

            ThreadWaitForTermination(pThread, &exitStatus);

            ThreadCloseHandle(pThread);
            pThread = NULL;
        }
    }

    return status;
}

STATUS
(__cdecl TestThreadPriorityDonationMultiple)(
    IN_OPT      PVOID       Context
    )
{
    STATUS status;
    MUTEX mutexes[2];
    BOOLEAN bReleasedMutexes[2];
    PTHREAD pThreads[3];
    TEST_PRIORITY_DONATION_MULTIPLE testType;
    BOOLEAN bTwoThreadsOnAlock;
    BOOLEAN bInverseRelease;
    BOOLEAN bCheckFailed;

    ASSERT(Context != NULL);

    ASSERT(ThreadGetPriority(NULL) == ThreadPriorityDefault);

    testType = *((TEST_PRIORITY_DONATION_MULTIPLE*)Context);
    bTwoThreadsOnAlock = (testType == TestPriorityDonationMultipleTwoThreadsPerLock) ||
                         (testType == TestPriorityDonationMultipleTwoThreadsPerLockInverseRelease);
    bInverseRelease = (testType == TestPriorityDonationMultipleOneThreadPerLockInverseRelease) ||
                      (testType == TestPriorityDonationMultipleTwoThreadsPerLockInverseRelease);

    LOG_TEST_LOG("Will check scenario 0x%x\n", testType);

    for (DWORD i = 0; i < ARRAYSIZE(mutexes); ++i)
    {
        MutexInit(&mutexes[i], FALSE);
        MutexAcquire(&mutexes[i]);
    }

    status = STATUS_SUCCESS;
    memzero(pThreads, ARRAYSIZE(pThreads) * sizeof(PTHREAD));
    memzero(bReleasedMutexes, ARRAYSIZE(bReleasedMutexes) * sizeof(BOOLEAN));
    STATIC_ASSERT(ARRAYSIZE(bReleasedMutexes) == ARRAYSIZE(mutexes));

    __try
    {
        status = _SpawnThreadAndCheckPriority(_ThreadTakeMutex,
                                                   &mutexes[0],
                                                   ThreadPriorityDefault + 4,
                                                   &pThreads[0],
                                                   &bCheckFailed);
        if (!SUCCEEDED(status) || bCheckFailed)
        {
            status = STATUS_ASSERTION_FAILURE;
            __leave;
        }

        status = _SpawnThreadAndCheckPriority(_ThreadTakeMutex,
                                                   &mutexes[1],
                                                   ThreadPriorityDefault + 8,
                                                   &pThreads[1],
                                                   &bCheckFailed);
        if (!SUCCEEDED(status) || bCheckFailed)
        {
            status = STATUS_ASSERTION_FAILURE;
            __leave;
        }

        if (bTwoThreadsOnAlock)
        {
            status = _SpawnThreadAndCheckPriority(_ThreadTakeMutex,
                                                       &mutexes[1],
                                                       ThreadPriorityDefault + 12,
                                                       &pThreads[2],
                                                       &bCheckFailed);
            if (!SUCCEEDED(status) || bCheckFailed)
            {
                status = STATUS_ASSERTION_FAILURE;
                __leave;
            }
        }

        if (!bInverseRelease)
        {
            MutexRelease(&mutexes[1]);
            bReleasedMutexes[1] = TRUE;

            if (!_ThreadValidatePriority(ThreadPriorityDefault + 4))
            {
                status = STATUS_ASSERTION_FAILURE;
                __leave;
            }

            MutexRelease(&mutexes[0]);
            bReleasedMutexes[0] = TRUE;

            if (!_ThreadValidatePriority(ThreadPriorityDefault))
            {
                status = STATUS_ASSERTION_FAILURE;
                __leave;
            }
        }
        else
        {
            MutexRelease(&mutexes[0]);
            bReleasedMutexes[0] = TRUE;

            if (!_ThreadValidatePriority(bTwoThreadsOnAlock ? ThreadPriorityDefault + 12 : ThreadPriorityDefault + 8))
            {
                status = STATUS_ASSERTION_FAILURE;
                __leave;
            }

            MutexRelease(&mutexes[1]);
            bReleasedMutexes[1] = TRUE;

            if (!_ThreadValidatePriority(ThreadPriorityDefault))
            {
                status = STATUS_ASSERTION_FAILURE;
                __leave;
            }
        }

        LOG_TEST_PASS;
    }
    __finally
    {
        for (DWORD i = 0; i < ARRAYSIZE(mutexes); ++i)
        {
            if (!bReleasedMutexes[i])
            {
                MutexRelease(&mutexes[i]);
                bReleasedMutexes[i] = TRUE;
            }
        }

        for (DWORD i = 0; i < ARRAYSIZE(pThreads); ++i)
        {
            if (pThreads[i] != NULL)
            {
                STATUS exitStatus;

                ThreadWaitForTermination(pThreads[i], &exitStatus);

                ThreadCloseHandle(pThreads[i]);
                pThreads[i] = NULL;
            }
        }
    }

    return status;
}

typedef struct _DONATION_CHAIN_THREAD_CTX
{
    PMUTEX                      FirstMutex;
    PMUTEX                      SecondMutex;
    THREAD_PRIORITY             ExpectedPriority;
} DONATION_CHAIN_THREAD_CTX, *PDONATION_CHAIN_THREAD_CTX;

typedef struct _DONATION_CHAIN_THREAD_DATA
{
    PTHREAD                     Thread;
    MUTEX                       ThreadMutex;
    DONATION_CHAIN_THREAD_CTX   ThreadContext;
} DONATION_CHAIN_THREAD_DATA, *PDONATION_CHAIN_THREAD_DATA;

STATUS
(__cdecl TestThreadPriorityDonationChain)(
    IN_OPT      PVOID       Context
    )
{
    STATUS status;
    DWORD noOfThreads;
    PDONATION_CHAIN_THREAD_DATA pDonationChainData;
    BOOLEAN bMutexAcquired;

    ASSERT(Context != NULL);

    ASSERT(ThreadGetPriority(NULL) == ThreadPriorityDefault);

    status = STATUS_SUCCESS;
    noOfThreads = *((DWORD*)Context) + 1;
    pDonationChainData = NULL;
    bMutexAcquired = FALSE;

    LOG_TEST_LOG("Will run test for 0x%x threads!\n", noOfThreads);

    __try
    {
        pDonationChainData = ExAllocatePoolWithTag(PoolAllocatePanicIfFail | PoolAllocateZeroMemory,
                                                   sizeof(DONATION_CHAIN_THREAD_DATA) * noOfThreads,
                                                   HEAP_TEST_TAG,
                                                   0);

        for (DWORD i = 0; i < noOfThreads; ++i)
        {
            // T[0] (main thread) will only take M[0]
            // T[1] will take M[1] and M[0]
            // T[2] will take M[2] and M[1]
            // ...
            DWORD nxtThIdx = (i-1) % noOfThreads;

            MutexInit(&pDonationChainData[i].ThreadMutex, FALSE);

            LOGL("Thread 0x%x will acquire mutex 0x%x and 0x%x\n", i, i, nxtThIdx);

            pDonationChainData[i].ThreadContext.FirstMutex = &pDonationChainData[i].ThreadMutex;
            pDonationChainData[i].ThreadContext.SecondMutex = &pDonationChainData[nxtThIdx].ThreadMutex;
            pDonationChainData[i].ThreadContext.ExpectedPriority = ThreadPriorityDefault + (noOfThreads - 1);
        }

        MutexAcquire(pDonationChainData[0].ThreadContext.FirstMutex);
        bMutexAcquired = TRUE;

        for (DWORD i = 1; i < noOfThreads; ++i)
        {
            BOOLEAN bPriorityCheck;

            // Each time the main thread creates a new thread it should have received a priority donation from it
            status = _SpawnThreadAndCheckPriority(_ThreadChainer,
                                                  &pDonationChainData[i].ThreadContext,
                                                  ThreadPriorityDefault + i,
                                                  &pDonationChainData[i].Thread,
                                                  &bPriorityCheck);
            if (!SUCCEEDED(status) || bPriorityCheck)
            {
                status = STATUS_ASSERTION_FAILURE;
                __leave;
            }
        }

        MutexRelease(pDonationChainData[0].ThreadContext.FirstMutex);
        bMutexAcquired = FALSE;

        if (!_ThreadValidatePriority(ThreadPriorityDefault))
        {
            status = STATUS_ASSERTION_FAILURE;
            __leave;
        }

        LOG_TEST_PASS;
    }
    __finally
    {
        if (bMutexAcquired)
        {
            ASSERT(pDonationChainData != NULL);

            MutexRelease(pDonationChainData[0].ThreadContext.FirstMutex);
        }

        if (pDonationChainData != NULL)
        {
            for (DWORD i = 1; i < noOfThreads; ++i)
            {
                if (pDonationChainData[i].Thread != NULL)
                {
                    STATUS exitStatus;

                    ThreadWaitForTermination(pDonationChainData[i].Thread, &exitStatus);
                    ThreadCloseHandle(pDonationChainData[i].Thread);
                }
            }

            ExFreePoolWithTag(pDonationChainData, HEAP_TEST_TAG);
            pDonationChainData = NULL;
        }
    }

    return status;
}

STATUS
(__cdecl _ThreadTakeMutex)(
    IN_OPT      PVOID       Context
    )
{
    PMUTEX pMutex = (PMUTEX) Context;

    ASSERT(pMutex != NULL);

    LOG_TEST_LOG("Donator thread with priority %u will attempt to take mutex!\n",
                 ThreadGetPriority(NULL));
    MutexAcquire(pMutex);

    LOG_TEST_LOG("Donator thread with priority %u acquired mutex!\n",
                 ThreadGetPriority(NULL));

    MutexRelease(pMutex);

    LOG_TEST_LOG("Donator thread with priority %u released mutex!\n",
                 ThreadGetPriority(NULL));

    return STATUS_SUCCESS;
}

STATUS
(__cdecl _ThreadChainer)(
    IN_OPT      PVOID       Context
    )
{
    PDONATION_CHAIN_THREAD_CTX pCtx;
    TID tid;
    THREAD_PRIORITY originalPriority;

    pCtx = Context;
    ASSERT(pCtx != NULL);

    tid = ThreadGetId(NULL);
    originalPriority = ThreadGetPriority(NULL);

    MutexAcquire(pCtx->FirstMutex);
    LOG_TEST_LOG("Thread 0x%X got the first lock!\n", tid);

    MutexAcquire(pCtx->SecondMutex);
    LOG_TEST_LOG("Thread 0x%X got the second lock\n", tid);

    if (!_ThreadValidatePriority(pCtx->ExpectedPriority))
    {
        return STATUS_ASSERTION_FAILURE;
    }

    MutexRelease(pCtx->SecondMutex);
    LOG_TEST_LOG("Thread 0x%X released the second lock\n", tid);

    if (!_ThreadValidatePriority(pCtx->ExpectedPriority))
    {
        return STATUS_ASSERTION_FAILURE;
    }

    MutexRelease(pCtx->FirstMutex);
    LOG_TEST_LOG("Thread 0x%X released the first lock\nIt should have reverted to its original priority!\n", tid);

    if (!_ThreadValidatePriority(originalPriority))
    {
        return STATUS_ASSERTION_FAILURE;
    }

    return STATUS_SUCCESS;
}
