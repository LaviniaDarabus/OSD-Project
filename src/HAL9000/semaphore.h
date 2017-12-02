#pragma once

#include "synch.h"

typedef struct _SEMAPHORE
{
    DWORD           Value;
	DWORD			MaxValue;
	LOCK			SemaphoreLock;
} SEMAPHORE, *PSEMAPHORE;

void
SemaphoreInit(
    OUT     PSEMAPHORE      Semaphore,
    IN      DWORD           InitialValue
    );
    
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