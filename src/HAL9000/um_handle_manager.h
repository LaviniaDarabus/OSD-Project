#pragma once

typedef enum handleId{
    PROCESS = 10,
	THREAD = 20,
	FILE = 30
}HandleId;

typedef QWORD UM_HANDLE;

UM_HANDLE
generateUmHandle(
	IN HandleId id,
	IN QWORD handleLow
);

