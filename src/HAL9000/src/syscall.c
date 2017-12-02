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



extern void SyscallEntry();

void
SyscallHandler(
	INOUT   COMPLETE_PROCESSOR_STATE    *CompleteProcessorState
)
{
	SYSCALL_ID sysCallId;
	PQWORD pSyscallParameters;
	STATUS status;
	REGISTER_AREA* usermodeProcessorState;

	ASSERT(CompleteProcessorState != NULL);

	// It is NOT ok to setup the FMASK so that interrupts will be enabled when the system call occurs
	// The issue is that we'll have a user-mode stack and we wouldn't want to receive an interrupt on
	// that stack. This is why we only enable interrupts here.
	ASSERT(CpuIntrGetState() == INTR_OFF);
	CpuIntrSetState(INTR_ON);

	LOG_TRACE_USERMODE("The syscall handler has been called!\n");

	status = STATUS_SUCCESS;
	pSyscallParameters = NULL;
	usermodeProcessorState = &CompleteProcessorState->RegisterArea;

	__try
	{
		if (LogIsComponentTraced(LogComponentUserMode))
		{
			DumpProcessorState(CompleteProcessorState);
		}

		sysCallId = usermodeProcessorState->RegisterValues[RegisterR8];

		LOG_TRACE_USERMODE("System call ID is %u\n", sysCallId);

		// The first parameter is the system call ID, we don't care about it => +1
		pSyscallParameters = (PQWORD)usermodeProcessorState->RegisterValues[RegisterRbp] + 1;

		switch (sysCallId) {
		case SyscallIdIdentifyVersion:
			//if (pSyscallParameters[0] is of type  SYSCALL_IF_VERSION )
			//{
			status = SyscallValidateInterface((SYSCALL_IF_VERSION)pSyscallParameters[0]);
			//}
			//else
			//{
			//status = 
			//}
			break;
		case SyscallIdProcessExit:
			status = SyscallProcessExit((STATUS)pSyscallParameters[0]);
			break;
		case SyscallIdFileWrite:
			status = SyscallFileWrite((UM_HANDLE)pSyscallParameters[0], (PVOID)pSyscallParameters[1], (QWORD)pSyscallParameters[2], (QWORD*)pSyscallParameters[3]);
			break;
		case SyscallIdThreadExit:
			status = SyscallThreadExit((STATUS)pSyscallParameters[0]);
			break;
	/*	case SyscallIdProcessCreate:
			status = SyscallProcessCreate((char*)pSyscallParameters[0], (QWORD)pSyscallParameters[1],(char*) pSyscallParameters[2],(QWORD) pSyscallParameters[3],(UM_HANDLE*) pSyscallParameters[4]);
			LOG("syscall ID !!!!!!!!!");
			break;
			*/
		}

	}
	__finally
	{
		LOG_TRACE_USERMODE("Will set UM RAX to 0x%x\n", status);

		usermodeProcessorState->RegisterValues[RegisterRax] = status;

		CpuIntrSetState(INTR_OFF);
	}
}

void
SyscallPreinitSystem(
    void
    )
{

}

STATUS
SyscallInitSystem(
    void
    )
{
    return STATUS_SUCCESS;
}

STATUS
SyscallUninitSystem(
    void
    )
{
    return STATUS_SUCCESS;
}

void
SyscallCpuInit(
    void
    )
{
    IA32_STAR_MSR_DATA starMsr;
    WORD kmCsSelector;
    WORD umCsSelector;

    memzero(&starMsr, sizeof(IA32_STAR_MSR_DATA));

    kmCsSelector = GdtMuGetCS64Supervisor();
    ASSERT(kmCsSelector + 0x8 == GdtMuGetDS64Supervisor());

    umCsSelector = GdtMuGetCS32Usermode();
    /// DS64 is the same as DS32
    ASSERT(umCsSelector + 0x8 == GdtMuGetDS32Usermode());
    ASSERT(umCsSelector + 0x10 == GdtMuGetCS64Usermode());

    // Syscall RIP <- IA32_LSTAR
    __writemsr(IA32_LSTAR, (QWORD) SyscallEntry);

    LOG_TRACE_USERMODE("Successfully set LSTAR to 0x%X\n", (QWORD) SyscallEntry);

    // Syscall RFLAGS <- RFLAGS & ~(IA32_FMASK)
    __writemsr(IA32_FMASK, RFLAGS_INTERRUPT_FLAG_BIT);

    LOG_TRACE_USERMODE("Successfully set FMASK to 0x%X\n", RFLAGS_INTERRUPT_FLAG_BIT);

    // Syscall CS.Sel <- IA32_STAR[47:32] & 0xFFFC
    // Syscall DS.Sel <- (IA32_STAR[47:32] + 0x8) & 0xFFFC
    starMsr.SyscallCsDs = kmCsSelector;

    // Sysret CS.Sel <- (IA32_STAR[63:48] + 0x10) & 0xFFFC
    // Sysret DS.Sel <- (IA32_STAR[63:48] + 0x8) & 0xFFFC
    starMsr.SysretCsDs = umCsSelector;

    __writemsr(IA32_STAR, starMsr.Raw);

    LOG_TRACE_USERMODE("Successfully set STAR to 0x%X\n", starMsr.Raw);
}

