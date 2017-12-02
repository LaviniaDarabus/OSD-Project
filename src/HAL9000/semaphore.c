#include "hal9000.h";
#include "semaphore.h";

void
SemaphoreInit(
	OUT     PSEMAPHORE      Semaphore,
	IN      DWORD           InitialValue
) {
	ASSERT(NULL != Semaphore);
	memzero(Semaphore, sizeof(Semaphore));
	LockInit(&Semaphore->SemaphoreLock);
}

void
SemaphoreDown(
	INOUT   PSEMAPHORE      Semaphore,
	IN      DWORD           Value
);

void
SemaphoreUp(
	INOUT   PSEMAPHORE      Semaphore,
	IN      DWORD           Value
);