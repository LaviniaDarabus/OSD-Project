#pragma once

typedef DWORD SYSCALL_IF_VERSION;

#define SYSCALL_IMPLEMENTED_IF_VERSION      0x1

#define UM_INVALID_HANDLE_VALUE             0

#define UM_FILE_HANDLE_STDOUT               (UM_HANDLE)0x1

typedef QWORD UM_HANDLE;

typedef enum handleType {
	HANDLE_TYPE_PROCESS = 567,
	HANDLE_TYPE_FILE = 568,
	HANDLE_TYPE_THREAD = 569
}HandleType;

typedef struct _UM_HANDLE_STRUCT
{
	UM_HANDLE       HandleID;
	LIST_ENTRY      HandleList;
} UM_HANDLE_STRUCT, *PUM_HANDLE_STRUCT;

#include "mem_structures.h"
#include "thread_defs.h"
#include "process_defs.h"
