#include "HAL9000.h"
#include "syscall.h"
#include "gdtmu.h"
#include "syscall_defs.h"
#include "syscall_func.h"
#include "syscall_no.h"
#include "dmp_cpu.h"
#include "process_internal.h"
#include "io.h"
#include "filesystem.h"
#include "intutils.h"
#include "iomu.h"
#include "thread.h"
#include "mmu.h"
#include "thread_internal.h"




STATUS
SyscallValidateInterface(
	IN  SYSCALL_IF_VERSION          InterfaceVersion
) {
	return InterfaceVersion == SYSCALL_IMPLEMENTED_IF_VERSION ? STATUS_SUCCESS : STATUS_INCOMPATIBLE_INTERFACE;
}

STATUS
SyscallProcessExit(
	IN      STATUS                  ExitStatus
) {
	ProcessTerminate(GetCurrentProcess());
	return ExitStatus;
}

STATUS
SyscallProcessCreate(
	IN_READS_Z(PathLength)
	char*               ProcessPath,
	IN          QWORD               PathLength,
	IN_READS_OPT_Z(ArgLength)
	char*               Arguments,
	IN          QWORD               ArgLength,
	OUT         UM_HANDLE*          ProcessHandle
) {
	STATUS status;
	PPROCESS pProcess;
	PPROCESS currProcess;
	char result[MAX_PATH];
	char finalArguments[MAX_PATH];
	char finalPath[MAX_PATH];
	PUM_HANDLE_STRUCT handle_str;
	DWORD handleID;
	DWORD type;
	UM_HANDLE tempHandle;

	status = STATUS_SUCCESS;
	pProcess = NULL;
	currProcess = GetCurrentProcess();

	if (ProcessPath == NULL)
	{
		return STATUS_INVALID_PARAMETER1;
	}
	if (cl_strcmp(ProcessPath, "") == 0) {
		return STATUS_INVALID_PARAMETER1;
	}
	if (PathLength <= 0) {
		return STATUS_INVALID_PARAMETER2;
	}
	// la ce trebuie path length si arg length...trebuie validati parametrii...trimit path length bytes
	// din pathul primit ca si parametru, trimit mai departe length bytes

	//cum verific daca pathul e absolut sau relativ ...il verific..merg pe sir sa vad daca e intr-un anumit format
	//folosesc IomuGetSystemPartitionPath() pentru ca crea un path relativ

	snprintf(finalArguments, (DWORD)ArgLength,"%s", Arguments);
	//LOG("Arguments: %s \n",finalArguments);
	snprintf(finalPath, (DWORD)PathLength,"%s", ProcessPath);
	if (finalPath[1] == ':' && finalPath[2] == '\\' && ((finalPath[0] >= 'A' && finalPath[0] <= 'Z') || (finalPath[0] >= 'a' && finalPath[0] <= 'z'))) {
	
		status = ProcessCreate(finalPath, finalArguments, &pProcess);
		
	}
	else {		
		sprintf(result,"%s%s%s", IomuGetSystemPartitionPath(),"APPLIC~1\\", finalPath);
		status = ProcessCreate(result, finalArguments, &pProcess);
	}
	if (status != STATUS_SUCCESS) {
		return status;
	}

		type = HANDLE_TYPE_PROCESS;
		handleID = QWORD_LOW(pProcess->Id);
		handle_str = &pProcess->Handle;

		tempHandle = (UM_HANDLE)DWORDS_TO_QWORD(type, handleID);
		*ProcessHandle = tempHandle;
	//	LOG("ProcessHandle: 0x%X in create process!\n", *ProcessHandle);
		handle_str->HandleID= *ProcessHandle;
		InsertTailList(&currProcess->HandleProcessList, &handle_str->HandleList);

	return status;
}

STATUS findProcess(
	IN      UM_HANDLE               ProcessHandle,
	OUT_PTR     PPROCESS*   Process
) {	
	BOOLEAN foundHandle = FALSE;
	DWORD type = QWORD_HIGH(ProcessHandle);
	LIST_ITERATOR it;
	PLIST_ENTRY pEntry;
	STATUS status;

	if (Process == NULL)
	{
		return STATUS_INVALID_PARAMETER2;
	}

	status = STATUS_SUCCESS;
	if (type != HANDLE_TYPE_PROCESS) {
	//	LOG("Type error:  0x%X, should be: 0x%X", type, PROCESS_HANDLE);
		return STATUS_INVALID_PARAMETER1;
	}
	ListIteratorInit(&GetCurrentProcess()->HandleProcessList, &it);
	while ((pEntry = ListIteratorNext(&it)) != NULL)
	{
		//LOG("Cauta: 0x%X", ProcessHandle);
		PUM_HANDLE_STRUCT phandle_str = CONTAINING_RECORD(pEntry, UM_HANDLE_STRUCT, HandleList);
		if (phandle_str->HandleID == ProcessHandle) {
			foundHandle = TRUE;
		}
	}
	if (!foundHandle) {
		return STATUS_INVALID_PARAMETER1;
	}
	ListIteratorInit(getProcessList(), &it);
	while ((pEntry = ListIteratorNext(&it)) != NULL)
	{
		PPROCESS process = CONTAINING_RECORD(pEntry, PROCESS, NextProcess);
		PUM_HANDLE_STRUCT phandle_str = &process->Handle;
		if (phandle_str->HandleID == ProcessHandle) {
			*Process = process;
			break;
		}
	}
	if (*Process == NULL) {
		return STATUS_INVALID_PARAMETER1;
	}

	return status;
}


STATUS
SyscallProcessGetPid(
	IN_OPT  UM_HANDLE               ProcessHandle,
	OUT     PID*                    ProcessId
) {

	STATUS status;
	PPROCESS pProcess;

	status = STATUS_SUCCESS;
	pProcess = NULL;

	if (ProcessHandle == UM_INVALID_HANDLE_VALUE) {
		pProcess = GetCurrentProcess();
		*ProcessId = pProcess->Id;
	}
	else {
		status = findProcess(ProcessHandle, &pProcess);
		if (status != STATUS_SUCCESS) {
			return status;
		}
		*ProcessId = pProcess->Id;
	}
	return status;
}

STATUS
SyscallProcessWaitForTermination(
	IN      UM_HANDLE               ProcessHandle,
	OUT     STATUS*                 TerminationStatus
) {
	PPROCESS pProcess;
	STATUS status;		

	pProcess = NULL;
	//validate ProcessHandle
	//verificam daca ProcessHandle este pentru tipul process
	//verficam daca ProcessHandle se afla in lista
	//ii asignam lui pProcess procesul corespunzator lui ProcessHandle
	status = findProcess(ProcessHandle, &pProcess);
	if (status != STATUS_SUCCESS) {
		return status;
	}
		
	//LOG("e in wait");
		ProcessWaitForTermination(pProcess, TerminationStatus);

	return status;
}

STATUS
SyscallProcessCloseHandle(
	IN      UM_HANDLE               ProcessHandle
) {

	STATUS status;
	DWORD type = QWORD_HIGH(ProcessHandle);
	LIST_ITERATOR it;
	PLIST_ENTRY pEntry;

	status = STATUS_SUCCESS;
	if (type != HANDLE_TYPE_PROCESS) {
		return STATUS_INVALID_PARAMETER1;
	}
	ListIteratorInit(&GetCurrentProcess()->HandleProcessList, &it);
	while ((pEntry = ListIteratorNext(&it)) != NULL)
	{
		PUM_HANDLE_STRUCT phandle_str = CONTAINING_RECORD(pEntry, UM_HANDLE_STRUCT, HandleList);
		if (phandle_str->HandleID == ProcessHandle) {
			RemoveEntryList(pEntry);
			return status;
		}
	}

	//scoate handlelul din lista de handleluri din currentProcess
	//dau deference
	//ce se intampla cu processul la care este linkat ProcessHandle?
	//isi va continua executia pana termina

	return STATUS_INVALID_PARAMETER1;
}


__forceinline
static
QWORD
_FileSystemGetNextId(
	void
)
{
	static volatile QWORD __currentid = 0;

	return _InterlockedExchangeAdd64(&__currentid, 4);
}

STATUS
SyscallFileCreate(
	IN_READS_Z(PathLength)
	char*                   Path,
	IN          QWORD                   PathLength,
	IN          BOOLEAN                 Directory,
	IN          BOOLEAN                 Create,
	OUT         UM_HANDLE*              FileHandle
)
{
	STATUS status;
	PFILE_OBJECT FileHandleObject;
	char result[MAX_PATH];
	char finalPath[MAX_PATH];
	DWORD handleID;
	DWORD type;
	UM_HANDLE tempHandle;
	PPROCESS currProcess;
	PUM_HANDLE_STRUCT handle_str;

	FileHandleObject = NULL;
	status = STATUS_SUCCESS;
	currProcess = GetCurrentProcess();

	if (Path == NULL)
	{
		return STATUS_INVALID_PARAMETER1;
	}
	if (cl_strcmp(Path, "") == 0) {
		return STATUS_INVALID_PARAMETER1;
	}
	if (PathLength <= 0) {
		return STATUS_INVALID_PARAMETER2;
	}
	
	status = MmuIsBufferValid((PVOID)Path, PathLength, PAGE_RIGHTS_READ,GetCurrentProcess());
		if (status != STATUS_SUCCESS) {
			return status;
		}

	snprintf(finalPath, (DWORD)PathLength, "%s", Path);
	if (finalPath[1] == ':' && finalPath[2] == '\\' && ((finalPath[0] >= 'A' && finalPath[0] <= 'Z') || (finalPath[0] >= 'a' && finalPath[0] <= 'z'))) {

		status = IoCreateFile(&FileHandleObject, finalPath, Directory, Create, FALSE);

	}
	else {
		sprintf(result, "%s%s", IomuGetSystemPartitionPath(), finalPath);
		status = IoCreateFile(&FileHandleObject, result, Directory, Create, FALSE);
	}
	if (status != STATUS_SUCCESS) {
		return status;
	}

	type = HANDLE_TYPE_FILE;
	handleID = QWORD_LOW(_FileSystemGetNextId());
	handle_str = &FileHandleObject->Handle;

	tempHandle = (UM_HANDLE)DWORDS_TO_QWORD(type, handleID);
	*FileHandle = tempHandle;
	
	handle_str->HandleID = *FileHandle;
	InsertTailList(&currProcess->HandleProcessList, &handle_str->HandleList);
	InsertTailList(&currProcess->OpenedFilesList, &FileHandleObject->FileList);
	
	return status;
}


STATUS findFileObject(
	IN      UM_HANDLE               FileHandle,
	OUT_PTR     PFILE_OBJECT*   FileHandleObject
) {
	STATUS status;
	LIST_ITERATOR it;
	PLIST_ENTRY pEntry;
	DWORD type = QWORD_HIGH(FileHandle);
	BOOLEAN foundHandle = FALSE;


	if (FileHandleObject == NULL)
	{
		return STATUS_INVALID_PARAMETER2;
	}

	status = STATUS_SUCCESS;

	if (type != HANDLE_TYPE_FILE) {
		return STATUS_INVALID_PARAMETER1;
	}

	ListIteratorInit(&GetCurrentProcess()->HandleProcessList, &it);
	while ((pEntry = ListIteratorNext(&it)) != NULL)
	{
		PUM_HANDLE_STRUCT phandle_str = CONTAINING_RECORD(pEntry, UM_HANDLE_STRUCT, HandleList);
		if (phandle_str->HandleID == FileHandle) {
			foundHandle = TRUE;
		}
	}
	if (!foundHandle) {
		return STATUS_INVALID_PARAMETER1;
	}

	ListIteratorInit(&GetCurrentProcess()->OpenedFilesList, &it);
	while ((pEntry = ListIteratorNext(&it)) != NULL)
	{
		PFILE_OBJECT fileObject = CONTAINING_RECORD(pEntry, FILE_OBJECT, FileList);
		PUM_HANDLE_STRUCT phandle_str = &fileObject->Handle;
		if (phandle_str->HandleID == FileHandle) {
			*FileHandleObject = fileObject;
			break;
		}
	}
	if (*FileHandleObject == NULL) {
		return STATUS_INVALID_PARAMETER1;
	}
	return status;
}

STATUS
SyscallFileClose(
	IN          UM_HANDLE               FileHandle
) {
	STATUS status;
	PFILE_OBJECT FileHandleObject;
	LIST_ITERATOR it;
	PLIST_ENTRY pEntry;
	
	FileHandleObject = NULL;
	status = STATUS_SUCCESS;
	if (FileHandle== UM_FILE_HANDLE_STDOUT) {
		ListIteratorInit(&GetCurrentProcess()->HandleProcessList, &it);
		while ((pEntry = ListIteratorNext(&it)) != NULL)
		{
			PUM_HANDLE_STRUCT phandle_str = CONTAINING_RECORD(pEntry, UM_HANDLE_STRUCT, HandleList);
			if (phandle_str->HandleID == FileHandle) {
				RemoveEntryList(&phandle_str->HandleList);
				return status;
			}
		}
		return STATUS_INVALID_PARAMETER1;
	}
	status = findFileObject(FileHandle, &FileHandleObject);
	if (status != STATUS_SUCCESS) {
		return status;
	}

	status = IoCloseFile(FileHandleObject);
	if (status == STATUS_SUCCESS) {
		RemoveEntryList(&FileHandleObject->FileList);
	}

	return status;
}



STATUS
SyscallFileRead(
	IN  UM_HANDLE                   FileHandle,
	OUT_WRITES_BYTES(BytesToRead)
	PVOID                       Buffer,
	IN  QWORD                       BytesToRead,
	OUT QWORD*                      BytesRead
) {
	STATUS status;
	PFILE_OBJECT FileHandleObject;

	FileHandleObject = NULL;
	status = findFileObject(FileHandle, &FileHandleObject);
	if (status != STATUS_SUCCESS) {
		return status;
	}
	status = IoReadFile(FileHandleObject, BytesToRead, NULL, Buffer, BytesRead);

	return status;
}


STATUS
SyscallFileWrite(
	IN  UM_HANDLE                   FileHandle,
	IN_READS_BYTES(BytesToWrite)
	PVOID                       Buffer,
	IN  QWORD                       BytesToWrite,
	OUT QWORD*                      BytesWritten
) {
	STATUS status;
	PFILE_OBJECT FileHandleObject;
	LIST_ITERATOR it;
	PLIST_ENTRY pEntry;
	BOOLEAN foundHandle = FALSE;

	FileHandleObject = NULL;
	status = STATUS_SUCCESS;

	if (FileHandle == UM_FILE_HANDLE_STDOUT)
	{
		
		ListIteratorInit(&GetCurrentProcess()->HandleProcessList, &it);
		while ((pEntry = ListIteratorNext(&it)) != NULL)
		{
			
			PUM_HANDLE_STRUCT phandle_str = CONTAINING_RECORD(pEntry, UM_HANDLE_STRUCT, HandleList);
			if (phandle_str->HandleID == FileHandle) {
				foundHandle = TRUE;
			}
		}
		*BytesWritten = strlen(Buffer) + 1;
		if (!foundHandle) {
			return status;
		}		
		LOG("[%s]:[%s]\n", ProcessGetName(NULL), Buffer);
		
	}
	else
	{
		status = findFileObject(FileHandle, &FileHandleObject);
		if (status != STATUS_SUCCESS) {
			return status;
		}
		status = IoWriteFile(FileHandleObject, BytesToWrite, NULL, Buffer, BytesWritten);
	}
	return status;
}

DWORD threadHandleIncrement = 1;

STATUS
SyscallThreadCreate(
	IN      PFUNC_ThreadStart       StartFunction,
	IN_OPT  PVOID                   Context,
	OUT     UM_HANDLE*              ThreadHandle
) {
	UM_HANDLE handle;
	char* name = "Name";
	handle = DWORDS_TO_QWORD(HANDLE_TYPE_THREAD, threadHandleIncrement++);
	PTHREAD thread;
	*ThreadHandle = handle;
	STATUS status = ThreadCreateEx(name, ThreadPriorityDefault, StartFunction, Context, &thread, GetCurrentProcess());
	thread->handle = handle;
	if (status != STATUS_SUCCESS) {
		threadHandleIncrement--;
		*ThreadHandle = 0;
	}
	return status;
}

PTHREAD findThread(
	IN_OPT  UM_HANDLE               ThreadHandle
) {
	LIST_ITERATOR it;
	PLIST_ENTRY pEntry;
	PTHREAD pNextThread;

	if (ThreadHandle == UM_INVALID_HANDLE_VALUE) {
		return GetCurrentThread();
	}

	ListIteratorInit(&GetCurrentProcess()->ThreadList, &it);
	while ((pEntry = ListIteratorNext(&it)) != NULL)
	{
		pNextThread = CONTAINING_RECORD(pEntry, THREAD, ProcessList);
		if (pNextThread->handle == ThreadHandle) {
			return pNextThread;
		}
	}
	return NULL;
}

STATUS
SyscallThreadGetTid(
	IN_OPT  UM_HANDLE               ThreadHandle,
	OUT     TID*                    ThreadId
) {
	LIST_ITERATOR it;
	PLIST_ENTRY pEntry;
	STATUS status = STATUS_SUCCESS;
	PTHREAD pNextThread;

	if (ThreadHandle == UM_INVALID_HANDLE_VALUE) {
		*ThreadId = GetCurrentThread()->Id;
		return STATUS_SUCCESS;
	}

	if (QWORD_HIGH(ThreadHandle) != HANDLE_TYPE_THREAD) {
		return STATUS_INVALID_PARAMETER1;
	}

	ListIteratorInit(&GetCurrentProcess()->ThreadList, &it);
	while ((pEntry = ListIteratorNext(&it)) != NULL)
	{
		pNextThread = CONTAINING_RECORD(pEntry, THREAD, ProcessList);
		if (pNextThread->handle == ThreadHandle) {
			*ThreadId = pNextThread->Id;
			status = STATUS_SUCCESS;
			break;
		}
	}
	return status;
}

STATUS
SyscallThreadExit(
	IN      STATUS                  ExitStatus
) {
	PTHREAD Thread = GetCurrentThread();
	if (Thread == NULL) {
		return STATUS_INVALID_PARAMETER1;
	}
	//don't wait on closed handle
	if (Thread->handle == 0) {
		return STATUS_UNSUCCESSFUL;
	}
	//don't wait on already terminated thread
	if (Thread->State == ThreadStateDying) {
		return STATUS_UNSUCCESSFUL;
	}
	Thread->handle = 0;
	ThreadExit(ExitStatus);
	return ExitStatus;
}

STATUS
SyscallThreadWaitForTermination(
	IN      UM_HANDLE               ThreadHandle,
	OUT     STATUS*                 TerminationStatus
) {
	PTHREAD Thread = findThread(ThreadHandle);
	if (Thread == NULL) {
		return STATUS_INVALID_PARAMETER1;
	}
	//don't wait on closed handle
	if (Thread->handle == 0) {
		return STATUS_UNSUCCESSFUL;
	}
	//don't wait on already terminated thread
	if (Thread->State == ThreadStateDying) {
		return STATUS_UNSUCCESSFUL;
	}

	ThreadWaitForTermination(Thread, TerminationStatus);
	return *TerminationStatus;
}

STATUS
SyscallThreadCloseHandle(
	IN      UM_HANDLE               ThreadHandle
) {
	PTHREAD Thread = findThread(ThreadHandle);
	if (Thread == NULL) {
		return STATUS_INVALID_PARAMETER1;
	}
	if (Thread->handle == 0) {
		return STATUS_UNSUCCESSFUL;
	}
	Thread->handle = 0;
	return STATUS_SUCCESS;
}
