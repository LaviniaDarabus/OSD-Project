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
#include "um_handle_manager.h"


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
	FileHandleObject = NULL;

	if (FileHandle == UM_FILE_HANDLE_STDOUT)
	{
		LOG("[%s]:[%s]\n", ProcessGetName(NULL), Buffer);
		status = STATUS_SUCCESS;
		*BytesWritten = strlen(Buffer) + 1;
	}
	else
	{
		status = IoWriteFile(FileHandleObject, BytesToWrite, NULL, Buffer, BytesWritten);
	}
	return status;
}

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
SyscallThreadExit(
	IN      STATUS                  ExitStatus
) {

	ThreadExit(ExitStatus);
	return ExitStatus;
}