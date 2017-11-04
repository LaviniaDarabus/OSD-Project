#include "HAL9000.h"
#include "mmu.h"
#include "int15.h"
#include "pmm.h"
#include "vmm.h"
#include "pte.h"
#include "display.h"
#include "synch.h"
#include "cl_heap.h"
#include "cpumu.h"
#include "thread.h"
#include "pe_parser.h"
#include "ex_event.h"
#include "process_internal.h"
#include "thread_internal.h"
#include "io.h"
#include "mdl.h"

#define PAGING_STRUCTURES_BASE_MEMORY                           (128*KB_SIZE)

// this is in hundreds of percentage => 25 == 0.25%
#define PAGING_STRUCTURES_PERCENTAGE                            25

#define HEAP_NORMAL_BASE_MEMORY                                 (1 * MB_SIZE)

// 100 == 1%
#define HEAP_NORMAL_PERCENTAGE                                  100

#define HEAP_SPECIAL_BASE_MEMORY                                (128 * KB_SIZE)
#define HEAP_SPECIAL_PERCENTAGE                                 25

#define TEMP_STACK_SIZE                                         (2*PAGE_SIZE)

#define VA_METADATA_SIZE_FOR_UM_PROCESS                         (5*GB_SIZE)
#define VA_ALLOCATIONS_START_OFFSET_FROM_IMAGE_BASE             (1*GB_SIZE)

// [0x0000'0000'0000'1000 -> 0x0000'7FFF'FFFF'FFFF] belongs to UM
// [0xFFFF'8000'0000'0000 -> 0xFFFF'FFFF'FFFF'FFFF] belongs to KM
#define PML4_OFFSET_OF_KERNEL_STRUCTURES                        (PAGE_SIZE / 2)
#define PML4_NO_OF_KERNEL_ENTRIES                               (PAGE_SIZE - PML4_OFFSET_OF_KERNEL_STRUCTURES)

#pragma pack(push,1)

#pragma warning(push)

// warning C4201: nonstandard extension used: nameless struct/union
#pragma warning(disable:4201)
typedef union _PAGE_FAULT_ERR_CODE
{
    struct
    {
        DWORD                       Present     :  1;
        DWORD                       Write       :  1;
        DWORD                       Usermode    :  1;
        DWORD                       RsvdBit     :  1;
        DWORD                       Execution   :  1;
        DWORD                       Pk          :  1;
        DWORD                       __Reserved0 : 10;
        DWORD                       Sgx         :  1;
        DWORD                       __Reserved1 : 15;
    };
    DWORD                           Raw;
} PAGE_FAULT_ERR_CODE, *PPAGE_FAULT_ERR_CODE;
STATIC_ASSERT( sizeof(PAGE_FAULT_ERR_CODE) == sizeof(DWORD));

#pragma warning(pop)

#pragma pack(pop)

typedef struct _MMU_ZERO_WORKER_ITEM
{
    LIST_ENTRY                      ListEntry;

    PHYSICAL_ADDRESS                PhysicalAddress;
    DWORD                           NumberOfFrames;
} MMU_ZERO_WORKER_ITEM, *PMMU_ZERO_WORKER_ITEM;

typedef struct _MMU_ZERO_WORKER_THREAD_CTX
{
    PEX_EVENT                       NewPagesEvent;

    PLOCK                           PagesLock;
    PLIST_ENTRY                     PagesToZeroList;
} MMU_ZERO_WORKER_THREAD_CTX, *PMMU_ZERO_WORKER_THREAD_CTX;

typedef struct _MMU_ZERO_THREAD_DATA
{
    PTHREAD                         WorkerThread;

    EX_EVENT                        NewPagesEvent;
    LOCK                            PagesLock;
    LIST_ENTRY                      PagesToZeroList;
} MMU_ZERO_THREAD_DATA, *PMMU_ZERO_THREAD_DATA;

typedef struct _MMU_HEAP_DATA
{
    _Guarded_by_(HeapLock)
    PHEAP_HEADER                    Heap;
    LOCK                            HeapLock;
} MMU_HEAP_DATA, *PMMU_HEAP_DATA;

typedef enum _MMU_HEAP_INDEX
{
    MmuHeapIndexNormal      = 0,
    MmuHeapIndexSpecial     = 1,
    MmuHeapIndexReserved    = MmuHeapIndexSpecial + 1
} MMU_HEAP_INDEX;


typedef struct _MMU_DATA
{
    PAGING_LOCK_DATA                PagingData;

    PE_NT_HEADER_INFO               KernelInfo;
    PVOID                           TemporaryStackBase;
    BOOLEAN                         PcidSupportAvailable;

    MMU_ZERO_THREAD_DATA            ZeroThreadData;

    MMU_HEAP_DATA                   Heaps[MmuHeapIndexReserved];
} MMU_DATA, *PMMU_DATA;

static MMU_DATA m_mmuData;

_No_competing_thread_
static
STATUS
_MmuInitPagingSystem(
    IN      PVOID                   BaseAddress
    );

static
STATUS
_MmuRetrieveKernelInfoAndValidate(
    IN      PVOID                   KernelBase,
    IN      DWORD                   ImageSize,
    OUT     PPE_NT_HEADER_INFO      KernelInfo
    );

static
STATUS
_MmuRemapStack(
    IN      PPAGING_DATA            PagingData,
    IN      PVOID                   NewStackBase,
    IN      DWORD                   StackSize
    );

static
STATUS
_MmuMapKernelMemory(
    IN          PPAGING_DATA            PagingData,
    IN          PHYSICAL_ADDRESS        PhysicalAddress,
    IN          PPE_NT_HEADER_INFO      KernelInfo
    );

static
STATUS
_MmuMapPeInMemory(
    IN          PPAGING_DATA            PagingData,
    IN          PPE_NT_HEADER_INFO      HeaderInfo,
    IN          PVOID                   AddressToMap
    );

static
STATUS
_MmuReserveAndMapMemory(
    IN          PPAGING_DATA            PagingData,
    IN          PVOID                   VirtualAddress,
    IN          DWORD                   Size,
    IN          PHYSICAL_ADDRESS        PhysicalAddress,
    IN          PAGE_RIGHTS             AccessRights
    );

static
STATUS
_MmuInitializeHeap(
    OUT         PMMU_HEAP_DATA          Heap,
    IN          DWORD                   HeapBaseSize,
    IN          WORD                    HeapPercentageSize
    );

static
_Always_(_When_(IsBooleanFlagOn(Flags, PoolAllocatePanicIfFail), RET_NOT_NULL))
PTR_SUCCESS
PVOID
_MmuAllocateFromPoolWithTag(
    IN      MMU_HEAP_INDEX          Heap,
    IN      DWORD                   Flags,
    IN      DWORD                   AllocationSize,
    IN      DWORD                   Tag,
    IN      DWORD                   AllocationAlignment
    );

static
void
_MmuFreeFromPoolWithTag(
    IN      MMU_HEAP_INDEX          Heap,
    _Pre_notnull_ _Post_ptr_invalid_
            PVOID                   MemoryAddress,
    IN      DWORD                   Tag
    );

static
void
_MmuRemapDisplay(
    IN          PPAGING_DATA            PagingData
    );

static
STATUS
_MmuCreatePagingTables(
    OUT_PTR     PPAGING_LOCK_DATA*            PagingTables
    );

_No_competing_thread_
static
void
_MmuDestroyPagingTables(
    _Pre_valid_ _Post_ptr_invalid_
        PPAGING_LOCK_DATA       PagingTables
    );

static FUNC_ThreadStart                 _MmuZeroWorkerThreadFunction;

__forceinline
static
DWORD
_MmuCalculateReservedFrames(
    IN                              DWORD                       BaseSize,
    IN                              WORD                        HundredsOfPercentage,
    IN                              QWORD                       AvailableSystemMemory
    )
{
    QWORD sizeInBytes = BaseSize + CalculatePercentage(AvailableSystemMemory, HundredsOfPercentage);
    QWORD sizeInFrames = AlignAddressUpper(sizeInBytes, 2 * MB_SIZE) / PAGE_SIZE;

    ASSERT(sizeInFrames < MAX_DWORD);

    return (DWORD)sizeInFrames;
}

_No_competing_thread_
void
MmuPreinitSystem(
    void
    )
{
    memzero(&m_mmuData, sizeof(MMU_DATA));

    RecRwSpinlockInit(0, &m_mmuData.PagingData.Lock);

    InitializeListHead(&m_mmuData.ZeroThreadData.PagesToZeroList);
    LockInit(&m_mmuData.ZeroThreadData.PagesLock);

    m_mmuData.PcidSupportAvailable = CpuMuIsPcidFeaturePresent();

    PmmPreinitSystem();
    VmmPreinit();
}

// We have the following virtual memory layout
// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// | 0B        | 0xFFFF'8000'0000'0000 + KernelBase  | + NT.SizeOfImage      | + HighestPA / PAGE_SIZE   | + Highest PA                      | + 1 TB            |  + 4 TB                       |
// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// | UNMAPPED  | Kernel Code                         | PMM Allocation Bitmap | Paging structures         | VMM Reservation Area              | VMM Bitmap Area   | Future virtual reservations   |
// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// |           |                    VA2PA works only for this VA region                                  |                                                                                       |
// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
_No_competing_thread_
STATUS
MmuInitSystem(
    IN          PVOID                   KernelBaseAddress,
    IN          DWORD                   KernelSize,
    IN          PHYSICAL_ADDRESS        MemoryEntries,
    IN          DWORD                   NumberOfMemoryEntries
    )
{
    STATUS status;
    PINT15_MEMORY_MAP_ENTRY pMemoryMap;
    PBYTE pPagingStructuresAddrBase;
    PBYTE pmmBaseAddress;
    PBYTE pVmmAddressBase;
    DWORD pmmSizeRequired;
    DWORD alignedKernelSize;
    PBYTE pNewStackTop;

    if (NULL == KernelBaseAddress)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (0 == KernelSize)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    if (0 == NumberOfMemoryEntries)
    {
        return STATUS_INVALID_PARAMETER3;
    }

    status = STATUS_SUCCESS;
    pMemoryMap = (PINT15_MEMORY_MAP_ENTRY) PA2VA(MemoryEntries);
    pPagingStructuresAddrBase = NULL;
    pVmmAddressBase = NULL;
    pmmBaseAddress = (PBYTE) KernelBaseAddress + KernelSize;
    pmmSizeRequired = 0;
    alignedKernelSize = 0;
    pNewStackTop = NULL;

    status = ExEventInit(&m_mmuData.ZeroThreadData.NewPagesEvent,
                         ExEventTypeNotification,
                         FALSE
                         );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("ExEventInit", status );
        return status;
    }
    LOGL("ExEventInit succeeded\n");

    status = _MmuRetrieveKernelInfoAndValidate(KernelBaseAddress,
                                               KernelSize,
                                               &m_mmuData.KernelInfo
                                               );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuRetrieveKernelInfoAndValidate", status );
        return status;
    }
    LOGL("_MmuRetrieveKernelInfoAndValidate succeeded\n");

    alignedKernelSize = AlignAddressUpper(m_mmuData.KernelInfo.Size, PAGE_SIZE);
    pNewStackTop = PtrOffset(m_mmuData.KernelInfo.ImageBase,
                             alignedKernelSize + TEMP_STACK_SIZE + STACK_GUARD_SIZE);
    pmmBaseAddress = pNewStackTop;
    m_mmuData.TemporaryStackBase = pNewStackTop - TEMP_STACK_SIZE;

    // change to new stack
    LOGL("Will change to a temporary stack at 0x%X\n", pNewStackTop );
    CpuMuChangeStack(pNewStackTop);

    status = PmmInitSystem(pmmBaseAddress,
                           pMemoryMap,
                           NumberOfMemoryEntries,
                           &pmmSizeRequired
                           );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("PmmInitSystem", status );
        return status;
    }

    LOG("PmmInitSystem suceeded and needs %u bytes\n", pmmSizeRequired);
    pPagingStructuresAddrBase = (PVOID) AlignAddressUpper( pmmBaseAddress + pmmSizeRequired, PAGE_SIZE );

    status = _MmuInitPagingSystem(pPagingStructuresAddrBase);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuInitInternal", status);
        return status;
    }

    LOG("_MmuInitPagingSystem succeeded\n");

    // The start VA of the VMM structures will be placed after we reserved as many bytes for the paging structures
    // as there is the highest physical address. This space will be shared by all the processes in the system and
    // they'll ALL need to have virtual addresses on which the VA2PA and PA2VA macros work
    pVmmAddressBase = (PVOID) PtrDiff(PA2VA(PmmGetHighestPhysicalMemoryAddressAvailable()), PAGE_SIZE);

    status = VmmInit(pVmmAddressBase);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("VmmInit", status);
        return status;
    }

    LOG("VmmInit suceeded\n");

    // reserve and map the first page used by the
    // virtual memory manager
    status = _MmuReserveAndMapMemory(&m_mmuData.PagingData.Data,
                                     pVmmAddressBase,
                                     PAGE_SIZE,
                                     VA2PA(pVmmAddressBase),
                                     PAGE_RIGHTS_READWRITE
                                     );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuReserveAndMapMemory", status);
        return status;
    }

    // reserve and map the allocation bitmap used by the
    // physical memory manager
    status = _MmuReserveAndMapMemory(&m_mmuData.PagingData.Data,
                                     pmmBaseAddress,
                                     pmmSizeRequired,
                                     VA2PA(pmmBaseAddress),
                                     PAGE_RIGHTS_READWRITE
                                     );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuReserveAndMapMemory", status);
        return status;
    }

    // maps the kernel to memory
    status = _MmuMapKernelMemory(&m_mmuData.PagingData.Data,
                                 VA2PA(m_mmuData.KernelInfo.ImageBase),
                                 &m_mmuData.KernelInfo);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuMapRequiredMemory", status);
        return status;
    }

    LOG("_MmuMapKernelMemory suceeded\n");

    // Switch to a temporary stack (which is mapped) until we can dynamically allocate
    // memory from the heap
    status = _MmuRemapStack(&m_mmuData.PagingData.Data,
                            m_mmuData.TemporaryStackBase,
                            TEMP_STACK_SIZE
                            );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuRemapStack", status);
        return status;
    }
    LOGL("_MmuRemapStack succeeded\n");

    _MmuRemapDisplay(&m_mmuData.PagingData.Data);

    LOG("Will change to new paging structures\n");
    // #PF's are treatable only after we switch to the new CR3
    // and we initialize the reservation system

    // We can't use the VmmChangeCr3 function because that functions expects a PCID
    // and an invalidate parameter which we currently don't until we spawn the system
    // process
    __writecr3(m_mmuData.PagingData.Data.BasePhysicalAddress);
    LOG("Changed to new cr3\n");
    VmmInitReservationSystem();

    // The heap from which all the kernel allocations come from
    status = _MmuInitializeHeap(&m_mmuData.Heaps[MmuHeapIndexNormal],
                                HEAP_NORMAL_BASE_MEMORY,
                                HEAP_NORMAL_PERCENTAGE
                                );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuInitializeHeap", status );
        return status;
    }
    LOG("_MmuInitializeHeap succeeded for normal heap\n");

    // Currently the special heap is only used by the worker thread responsible
    // for zeroing each physical frame of memory after it was released
    /// TODO: investigate why we need this, as far as I can remember we had some sort of
    /// deadlock if we tried to allocate heap memory in the function responsible for freeing
    /// the physical frames of memory
    status = _MmuInitializeHeap(&m_mmuData.Heaps[MmuHeapIndexSpecial],
                                HEAP_SPECIAL_BASE_MEMORY,
                                HEAP_SPECIAL_PERCENTAGE
                                );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuInitializeHeap", status);
        return status;
    }
    LOG("_MmuInitializeHeap succeeded for special heap\n");

    return status;
}

_No_competing_thread_
void
MmuDiscardIdentityMappings(
    void
    )
{
    PVOID pIdentityMapped = VA2PA(m_mmuData.KernelInfo.ImageBase);
    PVOID tempStack = m_mmuData.TemporaryStackBase;

    ASSERT( NULL != tempStack );

    // we must not release the physical pages
    MmuUnmapSystemMemory(pIdentityMapped, m_mmuData.KernelInfo.Size);

    m_mmuData.TemporaryStackBase = NULL;

    MmuUnmapMemoryEx(tempStack, TEMP_STACK_SIZE, TRUE, NULL );
}

STATUS
MmuInitThreadingSystem(
    void
    )
{
    STATUS status;
    PTHREAD pThread;
    PMMU_ZERO_WORKER_THREAD_CTX pCtx;

    LOG_FUNC_START;

    status = STATUS_SUCCESS;
    pThread = NULL;
    pCtx = NULL;

    pCtx = ExAllocatePoolWithTag(PoolAllocateZeroMemory, sizeof(MMU_ZERO_WORKER_THREAD_CTX), HEAP_MMU_TAG, 0 );
    if (NULL == pCtx)
    {
        LOG_FUNC_ERROR_ALLOC("HeapAllocatePoolWithTag", sizeof(MMU_ZERO_WORKER_THREAD_CTX));
        return STATUS_HEAP_INSUFFICIENT_RESOURCES;
    }
    pCtx->NewPagesEvent = &m_mmuData.ZeroThreadData.NewPagesEvent;
    pCtx->PagesToZeroList = &m_mmuData.ZeroThreadData.PagesToZeroList;
    pCtx->PagesLock = &m_mmuData.ZeroThreadData.PagesLock;

    __try
    {
        status = ThreadCreate("Page Zeroer Thread",
                              ThreadPriorityLowest,
                              _MmuZeroWorkerThreadFunction,
                              pCtx,
                              &pThread
        );
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("ThreadCreate", status);
            __leave;
        }

        pCtx = NULL;
        m_mmuData.ZeroThreadData.WorkerThread = pThread;
    }
    __finally
    {
        if (NULL != pCtx)
        {
            ExFreePoolWithTag(pCtx, HEAP_MMU_TAG);
            pCtx = NULL;
        }

        LOG_FUNC_END;
    }

    return status;
}

QWORD
MmuGetTotalSystemMemory(
    void
    )
{
    return PmmGetTotalSystemMemory();
}

PHYSICAL_ADDRESS
MmuGetHighestPhysicalMemoryAddressPresent(
    void
    )
{
    return PmmGetHighestPhysicalMemoryAddressPresent();
}

PTR_SUCCESS
PVOID
MmuMapMemoryEx(
    IN      PHYSICAL_ADDRESS        PhysicalAddress,
    IN      QWORD                   Size,
    IN      PAGE_RIGHTS             PageRights,
    IN      BOOLEAN                 Invalidate,
    IN      BOOLEAN                 Uncacheable,
    IN_OPT  PPAGING_LOCK_DATA       PagingData
    )
{
    QWORD alignedSize;
    PVOID pCurPointer;
    PHYSICAL_ADDRESS alignedPhysicalAddress;
    DWORD alignmentDifferences;
    PPAGING_LOCK_DATA pPagingData;

    INTR_STATE oldState;

    ASSERT(Size != 0);

    LOG_FUNC_START;

    LOG_TRACE_MMU("Physical address: 0x%X\n", PhysicalAddress);
    LOG_TRACE_MMU("Size: 0x%x\n", Size);

    // get page-aligned PA and size
    // Unfortunately we cannot consider the PA 0 invalid because we may need to map it at some point (ACPI)
    alignedPhysicalAddress = (PHYSICAL_ADDRESS) AlignAddressLower(PhysicalAddress, PAGE_SIZE);

    alignmentDifferences = (DWORD) AddressOffset(PhysicalAddress, PAGE_SIZE);

    alignedSize = AlignAddressUpper(Size + alignmentDifferences, PAGE_SIZE);
    ASSERT(alignedPhysicalAddress < (PHYSICAL_ADDRESS) PtrDiff(MmuGetHighestPhysicalMemoryAddressPresent(),alignedSize));

    pPagingData = (PagingData == NULL) ? &m_mmuData.PagingData : PagingData;

    RecRwSpinlockAcquireExclusive(&pPagingData->Lock, &oldState);
    pCurPointer = VmmMapMemoryEx(&pPagingData->Data,
                                alignedPhysicalAddress,
                                alignedSize,
                                PageRights,
                                Invalidate,
                                Uncacheable
                                );
    RecRwSpinlockReleaseExclusive(&pPagingData->Lock, oldState);
    if (NULL == pCurPointer)
    {
        LOG_ERROR("VmMapMemoryEx failed!\n");
        return NULL;
    }

    LOG_FUNC_END;

    return PtrOffset(pCurPointer,alignmentDifferences);
}

void
MmuMapMemoryInternal(
    IN      PHYSICAL_ADDRESS        PhysicalAddress,
    IN      QWORD                   Size,
    IN      PAGE_RIGHTS             PageRights,
    IN      PVOID                   VirtualAddress,
    IN      BOOLEAN                 Invalidate,
    IN      BOOLEAN                 Uncacheable,
    IN_OPT  PPAGING_LOCK_DATA       PagingData
    )
{
    INTR_STATE oldState;
    PPAGING_LOCK_DATA pPagingData;

    ASSERT( 0 != Size );
    ASSERT( IsAddressAligned(Size, PAGE_SIZE));

    ASSERT( NULL != VirtualAddress );
    ASSERT( IsAddressAligned(VirtualAddress, PAGE_SIZE));

    pPagingData = (PagingData == NULL) ? &m_mmuData.PagingData : PagingData;

    RecRwSpinlockAcquireExclusive(&pPagingData->Lock, &oldState );
    VmmMapMemoryInternal(&pPagingData->Data,
                         PhysicalAddress,
                         Size,
                         VirtualAddress,
                         PageRights,
                         Invalidate,
                         Uncacheable
                         );
    RecRwSpinlockReleaseExclusive(&pPagingData->Lock, oldState);
}

void
MmuUnmapMemoryEx(
    IN      PVOID                   VirtualAddress,
    IN      QWORD                   Size,
    IN      BOOLEAN                 ReleaseMemory,
    IN_OPT  PPAGING_LOCK_DATA       PagingData
    )
{
    QWORD alignedVirtualAddress;
    DWORD alignmentDifferences;
    DWORD alignedSize;
    PML4 cr3;
    INTR_STATE oldState;
    PPAGING_LOCK_DATA pPagingData;

    ASSERT(VirtualAddress != NULL);
    ASSERT(Size != 0);

    pPagingData = (PagingData == NULL) ? &m_mmuData.PagingData : PagingData;

    alignedVirtualAddress = AlignAddressLower(VirtualAddress, PAGE_SIZE);
    alignmentDifferences = (DWORD)((QWORD)VirtualAddress - alignedVirtualAddress);
    alignedSize = AlignAddressUpper(Size + alignmentDifferences, PAGE_SIZE);

    RecRwSpinlockAcquireExclusive(&pPagingData->Lock, &oldState);
    cr3.Raw = (QWORD) pPagingData->Data.BasePhysicalAddress;
    VmmUnmapMemoryEx(cr3,
                    (PVOID) alignedVirtualAddress,
                     alignedSize,
                     ReleaseMemory
                    );
    RecRwSpinlockReleaseExclusive(&pPagingData->Lock, oldState);
}

void
MmuReleaseMemory(
    IN          PHYSICAL_ADDRESS        PhysicalAddr,
    IN          DWORD                   NoOfFrames
    )
{
    BOOLEAN bListEmpty;
    INTR_STATE oldState;
    PMMU_ZERO_WORKER_ITEM pItem;

    LOG_FUNC_START_CPU;

    ASSERT( IsAddressAligned(PhysicalAddr, PAGE_SIZE ) );
    ASSERT( 0 != NoOfFrames );

    bListEmpty = FALSE;
    pItem = NULL;

    pItem = _MmuAllocateFromPoolWithTag(MmuHeapIndexSpecial,
                                        PoolAllocateZeroMemory,
                                        sizeof(MMU_ZERO_WORKER_ITEM),
                                        HEAP_MMU_TAG,
                                        0
                                        );
    ASSERT( NULL != pItem );

    pItem->PhysicalAddress = PhysicalAddr;
    pItem->NumberOfFrames = NoOfFrames;

    LockAcquire(&m_mmuData.ZeroThreadData.PagesLock, &oldState );
    InsertTailList(&m_mmuData.ZeroThreadData.PagesToZeroList, &pItem->ListEntry );
    LockRelease(&m_mmuData.ZeroThreadData.PagesLock, oldState);
    pItem = NULL;

    LOG_TRACE_MMU("About to signal worker thread\n");
    ExEventSignal(&m_mmuData.ZeroThreadData.NewPagesEvent);

    LOG_FUNC_END_CPU;
}

PTR_SUCCESS
PHYSICAL_ADDRESS
MmuGetPhysicalAddress(
    IN      PVOID                   VirtualAddress
    )
{
    return MmuGetPhysicalAddressEx(VirtualAddress,
                                   NULL,
                                   __readcr3());
}

PTR_SUCCESS
PHYSICAL_ADDRESS
MmuGetPhysicalAddressEx(
    IN      PVOID                   VirtualAddress,
    IN_OPT  PPAGING_LOCK_DATA       PagingData,
    IN_OPT  PHYSICAL_ADDRESS        Cr3Base
    )
{
    PML4 cr3;
    INTR_STATE oldState;
    PHYSICAL_ADDRESS pa;
    PVOID alignedAddress;
    DWORD alignmentDifference;

    if (NULL == VirtualAddress)
    {
        return NULL;
    }

    ASSERT((PagingData != NULL) ^ (Cr3Base != NULL));

    oldState = 0;
    alignedAddress = (PVOID) AlignAddressLower(VirtualAddress,PAGE_SIZE);
    alignmentDifference = AddressOffset(VirtualAddress, PAGE_SIZE );

    if (PagingData != NULL)
    {
        RecRwSpinlockAcquireShared(&PagingData->Lock, &oldState);
    }

    cr3.Raw = (QWORD) ((PagingData != NULL ) ? PagingData->Data.BasePhysicalAddress : Cr3Base);
    pa = VmmGetPhysicalAddress(cr3,
                               alignedAddress
                               );

    if (PagingData != NULL)
    {
        RecRwSpinlockReleaseShared(&PagingData->Lock, oldState);
    }

    return (PBYTE) pa + alignmentDifference;
}

_Always_(_When_(IsBooleanFlagOn(Flags, PoolAllocatePanicIfFail), RET_NOT_NULL))
PTR_SUCCESS
PVOID
MmuAllocatePoolWithTag(
    IN      DWORD                   Flags,
    IN      DWORD                   AllocationSize,
    IN      DWORD                   Tag,
    IN      DWORD                   AllocationAlignment
    )
{
    return _MmuAllocateFromPoolWithTag(MmuHeapIndexNormal,
                                       Flags,
                                       AllocationSize,
                                       Tag,
                                       AllocationAlignment
                                       );
}

void
MmuFreePoolWithTag(
    _Pre_notnull_ _Post_ptr_invalid_
            PVOID                   MemoryAddress,
    IN      DWORD                   Tag
    )
{
    _MmuFreeFromPoolWithTag(MmuHeapIndexNormal,
                            MemoryAddress,
                            Tag
                            );
}

void
MmuProbeMemory(
    IN      PVOID                   Buffer,
    IN      DWORD                   NumberOfBytes
    )
{
    PBYTE pBuffer;
    DWORD offset;
    DWORD alignedSize;
    DWORD alignmentDifferences;

    ASSERT( NULL != Buffer );
    ASSERT( 0 != NumberOfBytes );

    pBuffer = (PBYTE) AlignAddressLower(Buffer, PAGE_SIZE);
    alignmentDifferences = AddressOffset(Buffer, PAGE_SIZE);
    alignedSize = AlignAddressUpper(NumberOfBytes + alignmentDifferences, PAGE_SIZE);

    for(offset = 0;
        offset < alignedSize;
        offset = offset + PAGE_SIZE
        )
    {
        BYTE temp = *(pBuffer + offset);temp;
    }
}

BOOLEAN
MmuSolvePageFault(
    IN      PVOID                   FaultingAddress,
    IN      DWORD                   ErrorCode
    )
{
    PAGE_RIGHTS rightsRequested;
    PAGE_FAULT_ERR_CODE pfErrCode;

    ASSERT( INTR_OFF == CpuIntrGetState() );

    pfErrCode.Raw = ErrorCode;
    rightsRequested = PAGE_RIGHTS_READ;

    rightsRequested |= ( pfErrCode.Write ? PAGE_RIGHTS_WRITE : 0 );
    rightsRequested |= ( pfErrCode.Execution ? PAGE_RIGHTS_EXECUTE : 0 );

    return VmmSolvePageFault(FaultingAddress,
                             rightsRequested,
                             pfErrCode.Usermode ? GetCurrentThread()->Process->PagingData : &m_mmuData.PagingData
                             );
}

STATUS
MmuLoadPe(
    IN      PPE_NT_HEADER_INFO      NtHeader,
    IN      PPAGING_LOCK_DATA       PagingData
    )
{
    STATUS status;
    INTR_STATE oldState;

    if (NtHeader == NULL)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (PagingData == NULL)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    status = STATUS_SUCCESS;

    RecRwSpinlockAcquireExclusive(&PagingData->Lock, &oldState);
    status = _MmuMapPeInMemory(&PagingData->Data,
                               NtHeader,
                               NtHeader->Preferred.ImageBase);
    RecRwSpinlockReleaseExclusive(&PagingData->Lock, oldState);

    return status;
}

STATUS
MmuCreateAddressSpaceForProcess(
    INOUT   PPROCESS                Process
    )
{
    STATUS status;

    ASSERT(Process != NULL);
    ASSERT(!ProcessIsSystem(Process));

    status = STATUS_SUCCESS;

    // init will NULL so we can call the destroy function in case of failure
    Process->PagingData = NULL;
    Process->VaSpace = NULL;

    __try
    {
        // Initialize the paging structures
        // The kernel portion of the tables will be identical, only the UM part will differ
        status = _MmuCreatePagingTables(&Process->PagingData);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("_MmuCreatePagingTables", status);
            __leave;
        }

        LOG_TRACE_MMU("Successfully created paging tables for process [%s]\n", ProcessGetName(Process));

        // Create the VMM management structures (VMM_RESERVATION_SPACE) to describe the processes
        // virtual memory allocations
        status = VmmCreateVirtualAddressSpace(&Process->VaSpace,
                                              VA_METADATA_SIZE_FOR_UM_PROCESS,
                                              PtrOffset(Process->HeaderInfo->Preferred.ImageBase, VA_ALLOCATIONS_START_OFFSET_FROM_IMAGE_BASE));
        if(!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("VmmCreateVirtualAddressSpace", status);
            __leave;
        }

        LOG_TRACE_MMU("Successfully initialized VA space for process [%s]\n", ProcessGetName(Process));
    }
    __finally
    {
        if (!SUCCEEDED(status))
        {
            MmuDestroyAddressSpaceForProcess(Process);
        }
    }

    return status;
}

void
MmuDestroyAddressSpaceForProcess(
    INOUT   PPROCESS                Process
    )
{
    ASSERT(Process != NULL);
    ASSERT(!ProcessIsSystem(Process));

    if (Process->VaSpace != NULL)
    {
        VmmDestroyVirtualAddressSpace(Process->VaSpace);
        Process->VaSpace = NULL;
    }

    if (Process->PagingData != NULL)
    {
        // NOTE: from what I've realized nothing bad can happen if an interrupt
        // occurs while we change our paging tables to those of another process
        // this change does not modify the current's thread owner process or its
        // paging structures so the original paging structures will be used in the
        // interrupt handlers

        // invalidate all PCID mappings => new processes can reuse
        // PCID
        ProcessActivatePagingTables(Process, TRUE);

        // restore previous paging table
        ProcessActivatePagingTables(GetCurrentThread()->Process, !m_mmuData.PcidSupportAvailable);

        _MmuDestroyPagingTables(Process->PagingData);
        Process->PagingData = NULL;
    }
}

_No_competing_thread_
void
MmuInitAddressSpaceForSystemProcess(
    void
    )
{
    PPROCESS pProcess;

    pProcess = ProcessRetrieveSystemProcess();

    ASSERT(pProcess != NULL);
    ASSERT(ProcessIsSystem(pProcess));

    // The system process is special, we will not allocate paging data or a VA space
    // because we already have one
    pProcess->PagingData = &m_mmuData.PagingData;
    pProcess->VaSpace = VmmRetrieveReservationSpaceForSystemProcess();

    /// TODO: I have no idea why the PE_NT_HEADER_INFO is allocated dynamically
    /// Nothing bad happens, I just don't know if we should keep this
    memcpy(pProcess->HeaderInfo, &m_mmuData.KernelInfo, sizeof(PE_NT_HEADER_INFO));

    MmuActivateProcessIds();
}

void
MmuActivateProcessIds(
    void
    )
{
    if (m_mmuData.PcidSupportAvailable)
    {
        __writecr4(__readcr4() | CR4_PCIDE);
    }

    MmuChangeProcessSpace(ProcessRetrieveSystemProcess());

    if (m_mmuData.PcidSupportAvailable)
    {
        LOGL("Successfully activated process identifiers!\n");
    }
    else
    {
        LOGL("This CPU doesn't have support for PCIDs, either an AMD or an old Intel!!!\n");
    }
}

void
MmuChangeProcessSpace(
    IN          PPROCESS            Process
    )
{
    ASSERT(Process != NULL);

    ProcessActivatePagingTables(Process, !m_mmuData.PcidSupportAvailable);
}

PTR_SUCCESS
PVOID
MmuAllocStack(
    IN          DWORD               StackSize,
    IN          BOOLEAN             ProtectStack,
    IN          BOOLEAN             LazyMap,
    IN_OPT      PPROCESS            Process
    )
{
    PBYTE pStackBase;
    PBYTE pCommitedStackBase;
    DWORD totalAllocationSize;
    DWORD stackGuardSize;
    VMM_ALLOC_TYPE allocTypeCommit;
    PPAGING_LOCK_DATA pPagingData;
    PVMM_RESERVATION_SPACE pVaSpace;

    ASSERT( IsAddressAligned( StackSize, PAGE_SIZE ));

    /// TODO: could also add upper protection for the stack (should not happen on normal execution because
    /// the stack grows downwards, but it may happen when the stack contents are manipulated by our functions
    /// such as GSConvertCookiesForNewStack, _ThreadSetupInitialState or _ThreadSetupMainThreadUserStack

    // if we want to protect the stack we'll allocate an additional safeguard PAGE
    stackGuardSize = ProtectStack ? STACK_GUARD_SIZE : 0;
    totalAllocationSize = StackSize + stackGuardSize;
    allocTypeCommit = VMM_ALLOC_TYPE_COMMIT;
    allocTypeCommit |= (LazyMap ? 0 : VMM_ALLOC_TYPE_NOT_LAZY);
    pPagingData = (Process == NULL) ? &m_mmuData.PagingData : Process->PagingData;
    pVaSpace = (Process == NULL) ? NULL : Process->VaSpace;

    // Reserve the StackSize requested + the size of the guard (if any)
    pStackBase = VmmAllocRegionEx(NULL,
                                  totalAllocationSize,
                                  VMM_ALLOC_TYPE_RESERVE,
                                  PAGE_RIGHTS_READWRITE,
                                  FALSE,
                                  NULL,
                                  pVaSpace,
                                  pPagingData,
                                  NULL
                                  );
    if (NULL == pStackBase)
    {
        LOG_ERROR("VmmAllocRegion failed!\n");
        return NULL;
    }

    LOG_TRACE_COMP(LogComponentGeneric | LogComponentThread,
        "Stack Base: 0x%X\n", pStackBase );
    LOG_TRACE_COMP(LogComponentGeneric | LogComponentThread,
        "Total stack size (including guard region): 0x%x\n", totalAllocationSize );

    // commit the memory only after the stack guard
    // This way we'll have STACK_GUARD_SIZE bytes unmapped and uncommitted after the
    // stack is depleted and we'll easily detect a stack overflow
    pCommitedStackBase = VmmAllocRegionEx(pStackBase + stackGuardSize,
                                          StackSize,
                                          allocTypeCommit,
                                          PAGE_RIGHTS_READWRITE,
                                          FALSE,
                                          NULL,
                                          pVaSpace,
                                          pPagingData,
                                          NULL
                                          );
    if (NULL == pCommitedStackBase)
    {
        // this is weird
        // and should never happen
        LOG_ERROR("VmmAllocRegion didn't manage to commit previously reserved memory!\n");
        return NULL;
    }

    return (PVOID) (pStackBase + totalAllocationSize);
}

void
MmuFreeStack(
    IN          PVOID       Stack,
    IN_OPT      PPROCESS    Process
    )
{
    PPAGING_LOCK_DATA pPagingData;
    PVMM_RESERVATION_SPACE pVaSpace;

    ASSERT(Stack != NULL);

    pPagingData = (Process == NULL) ? &m_mmuData.PagingData : Process->PagingData;
    pVaSpace = (Process == NULL) ? NULL : Process->VaSpace;

    VmmFreeRegionEx(Stack,
                    0,
                    VMM_FREE_TYPE_RELEASE,
                    TRUE,
                    pVaSpace,
                    pPagingData
                    );
}

STATUS
MmuIsBufferValid(
    IN          PVOID               Buffer,
    IN          QWORD               BufferSize,
    IN          PAGE_RIGHTS         RightsRequested,
    IN          PPROCESS            Process
    )
{
    if (Buffer == NULL)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (BufferSize == 0)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    if (Process == NULL)
    {
        return STATUS_INVALID_PARAMETER4;
    }

    // This is a temporary hack, we should also check the access rights, however HAL9000 currently does not support
    // these checks in case the buffer is inside the binary
    if (Process->HeaderInfo->Preferred.ImageBase <= Buffer
        && Buffer < (PVOID)PtrOffset(Process->HeaderInfo->Preferred.ImageBase, Process->HeaderInfo->Size))
    {
        return STATUS_SUCCESS;
    }

    return VmmIsBufferValid(Buffer,
                            BufferSize,
                            RightsRequested,
                            Process->VaSpace,
                            Process->PagingData->Data.KernelSpace);
}

STATUS
MmuGetSystemVirtualAddressForUserBuffer(
    IN          PVOID               UserAddress,
    IN          QWORD               Size,
    IN          PAGE_RIGHTS         PageRights,
    IN          PPROCESS            Process,
    OUT         PVOID*              KernelAddress
    )
{
    STATUS status;
    PMDL pMdl;
    PVOID pKernelAddress;

    if (UserAddress == NULL)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (Size == 0)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    if (Process == NULL)
    {
        return STATUS_INVALID_PARAMETER3;
    }

    if (KernelAddress == NULL)
    {
        return STATUS_INVALID_PARAMETER4;
    }

    status = STATUS_SUCCESS;
    pKernelAddress = NULL;
    ASSERT(Size <= MAX_DWORD);

    pMdl = NULL;

    __try
    {
        pMdl = MdlAllocateEx(UserAddress,
                             (DWORD)Size,
                             NULL,
                             Process->PagingData);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR_ALLOC("MdlAllocateEx", Size);
            status = STATUS_UNSUCCESSFUL;
            __leave;
        }

        pKernelAddress = VmmAllocRegionEx(NULL,
                                          Size,
                                          VMM_ALLOC_TYPE_RESERVE | VMM_ALLOC_TYPE_COMMIT | VMM_ALLOC_TYPE_NOT_LAZY,
                                          PageRights,
                                          FALSE,
                                          NULL,
                                          NULL,
                                          NULL,
                                          pMdl);
        if (pKernelAddress == NULL)
        {
            LOG_FUNC_ERROR_ALLOC("VmmAllocRegionEx", Size);
            status = STATUS_MEMORY_CANNOT_BE_COMMITED;
            __leave;
        }

    }
    __finally
    {
        // We need to free the MDL no matter what (success or failure)
        if (pMdl != NULL)
        {
            MdlFree(pMdl);
            pMdl = NULL;
        }

        if (SUCCEEDED(status))
        {
            // The UserAddress may not have been page aligned - re-offset it
            *KernelAddress = PtrOffset(pKernelAddress, AddressOffset(UserAddress, PAGE_SIZE));
        }
    }

    return status;
}

void
MmuFreeSystemVirtualAddressForUserBuffer(
    IN          PVOID               KernelAddress
    )
{
    ASSERT(KernelAddress != NULL);

    VmmFreeRegionEx(KernelAddress,
                    0,
                    VMM_FREE_TYPE_RELEASE,
                    FALSE,
                    NULL,
                    NULL);
}

static
STATUS
_MmuCreatePagingTables(
    OUT_PTR     PPAGING_LOCK_DATA*            PagingTables
    )
{
    DWORD framesForPagingStructures;
    STATUS status;
    PHYSICAL_ADDRESS basePa;
    INTR_STATE oldState;
    PPAGING_LOCK_DATA pPagingData;

    ASSERT(NULL != PagingTables);

    status = STATUS_SUCCESS;
    basePa = NULL;

    pPagingData = ExAllocatePoolWithTag(PoolAllocateZeroMemory,
                                        sizeof(PAGING_LOCK_DATA),
                                        HEAP_PROCESS_TAG,
                                        0);
    if (pPagingData == NULL)
    {
        LOG_FUNC_ERROR_ALLOC("ExAllocatePoolWithTag", status);
        return STATUS_INSUFFICIENT_MEMORY;
    }

    RecRwSpinlockInit(0, &pPagingData->Lock);

    // calculate size of paging structure
    framesForPagingStructures = _MmuCalculateReservedFrames(PAGING_STRUCTURES_BASE_MEMORY,
                                                            PAGING_STRUCTURES_PERCENTAGE,
                                                            PmmGetTotalSystemMemory()
                                                            );

    LOG_TRACE_MMU("Frames reserved for paging structures 0x%x\n", framesForPagingStructures);

    __try
    {
        basePa = PmmReserveMemory(framesForPagingStructures);
        if (NULL == basePa)
        {
            status = STATUS_PHYSICAL_MEMORY_NOT_AVAILABLE;
            LOG_FUNC_ERROR("MmuRequestMemoryEx", status);
            __leave;
        }

        RecRwSpinlockAcquireExclusive(&m_mmuData.PagingData.Lock, &oldState);
        status = VmmSetupPageTables(&m_mmuData.PagingData.Data,
                                    &pPagingData->Data,
                                    basePa,
                                    framesForPagingStructures,
                                    FALSE
        );
        RecRwSpinlockReleaseExclusive(&m_mmuData.PagingData.Lock, oldState);

        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("VmmSetupPageTables", status);
            __leave;
        }

        LOG_TRACE_MMU("Will copy kernel PML4 table to new page tables!\n");

        /// TODO: remove this hardcoded value and replace it with the virtual kernel base divided by 512GB (size of
        /// a PML4 entry)
        memcpy((PVOID)PA2VA(PtrOffset(pPagingData->Data.BasePhysicalAddress, PML4_OFFSET_OF_KERNEL_STRUCTURES)),
               (PVOID)PA2VA(PtrOffset(m_mmuData.PagingData.Data.BasePhysicalAddress, PML4_OFFSET_OF_KERNEL_STRUCTURES)),
               PML4_NO_OF_KERNEL_ENTRIES);

        *PagingTables = pPagingData;
    }
    __finally
    {
        if (!SUCCEEDED(status))
        {
            if (basePa != NULL)
            {
                PmmReleaseMemory(basePa, framesForPagingStructures);
                basePa = NULL;
            }

            if (pPagingData != NULL)
            {
                ExFreePoolWithTag(pPagingData, HEAP_PROCESS_TAG);
                pPagingData = NULL;
            }
        }
    }

    return status;
}

_No_competing_thread_
static
void
_MmuDestroyPagingTables(
    _Pre_valid_ _Post_ptr_invalid_
        PPAGING_LOCK_DATA       PagingTables
    )
{
    ASSERT(PagingTables != NULL);

    // When we unmap the paging structures we also release the physical frames reserved
    MmuUnmapMemoryEx((PVOID)PA2VA(PagingTables->Data.BasePhysicalAddress),
                           PagingTables->Data.NumberOfFrames * PAGE_SIZE,
                           TRUE,
                           NULL
                           );

    ExFreePoolWithTag(PagingTables, HEAP_PROCESS_TAG);
}

_No_competing_thread_
static
STATUS
_MmuInitPagingSystem(
    IN      PVOID                   BaseAddress
    )
{
    DWORD framesForPagingStructures;
    STATUS status;
    PHYSICAL_ADDRESS basePa;

    ASSERT(BaseAddress != NULL);

    status = STATUS_SUCCESS;
    basePa = NULL;
    framesForPagingStructures = 0;

    status = VmmPreparePagingData();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("VmmSetupPagingStructures", status);
        return status;
    }

    // calculate size of paging structure
    framesForPagingStructures = _MmuCalculateReservedFrames(PAGING_STRUCTURES_BASE_MEMORY,
                                                            PAGING_STRUCTURES_PERCENTAGE,
                                                            PmmGetTotalSystemMemory()
                                                            );

    LOG("Frames reserved for paging structures 0x%x\n", framesForPagingStructures);

    // Reserve the physical memory for the paging structures
    basePa = PmmReserveMemoryEx(framesForPagingStructures,
                                VA2PA(BaseAddress)
                                );
    if ((NULL == basePa) ||
        (basePa != VA2PA(BaseAddress)))
    {
        status = STATUS_PHYSICAL_MEMORY_NOT_AVAILABLE;
        LOG_FUNC_ERROR("MmuRequestMemoryEx", status);
        return status;
    }

    // Zero the PML4 - else we may have all sorts of junk there
    memzero((PVOID)PA2VA(basePa), PAGE_SIZE);

    status = VmmSetupPageTables(&m_mmuData.PagingData.Data,
                                &m_mmuData.PagingData.Data,
                                basePa,
                                framesForPagingStructures,
                                TRUE);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuInitPagingSystem", status);
        return status;
    }

    return status;
}

static
STATUS
_MmuRetrieveKernelInfoAndValidate(
    IN      PVOID                   KernelBase,
    IN      DWORD                   ImageSize,
    OUT     PPE_NT_HEADER_INFO      KernelInfo
    )
{
    STATUS status;
    PE_DATA_DIRECTORY dataDirectory;

    ASSERT( NULL != KernelBase );
    ASSERT( 0 != ImageSize );
    ASSERT( NULL != KernelInfo );

    status = STATUS_SUCCESS;
    memzero(&dataDirectory, sizeof(PE_DATA_DIRECTORY));

    status = PeRetrieveNtHeader(KernelBase,
                                ImageSize,
                                KernelInfo
                                );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("PeRetrieveNtHeader", status );
        return status;
    }
    LOGL("PeRetrieveNtHeader succeeded\n");

    if (ImageSize < KernelInfo->Size)
    {
        LOG_ERROR("We loaded only %u bytes and the image is %u bytes long\n", ImageSize, KernelInfo->Size );
        return STATUS_IMAGE_NOT_FULLY_LOADED;
    }

    if (IMAGE_FILE_MACHINE_AMD64 != KernelInfo->Machine )
    {
        LOG_ERROR("Expecting a PE64 executable and received: 0x%x\n", KernelInfo->Machine );
        return STATUS_IMAGE_NOT_64_BIT;
    }

    if (IMAGE_SUBSYSTEM_NATIVE != KernelInfo->Subsystem)
    {
        LOG_ERROR("Expecting a native sub-system executable and received: 0x%x\n", KernelInfo->Subsystem );
        return STATUS_IMAGE_SUBSYSTEM_NOT_NATIVE;
    }

    status = PeRetrieveDataDirectory(KernelInfo,
                                     IMAGE_DIRECTORY_ENTRY_BASERELOC,
                                     &dataDirectory
                                     );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("PeRetrieveDataDirectory", status);
        return status;
    }

    if (0 != dataDirectory.Size)
    {
        LOG_ERROR("Image has relocations, we don't support such executables!\n");
        return STATUS_IMAGE_HAS_RELOCATIONS;
    }

    return status;
}

static
STATUS
_MmuRemapStack(
    IN      PPAGING_DATA            PagingData,
    IN      PVOID                   NewStackBase,
    IN      DWORD                   StackSize
    )
{
    STATUS status;
    PBYTE pNewStackTop;

    ASSERT(NULL != PagingData);
    ASSERT( NULL != NewStackBase );
    ASSERT( 0 != StackSize );
    ASSERT(IsAddressAligned(StackSize, PAGE_SIZE));

    status = STATUS_SUCCESS;

    // we will allocate the stack at the end of the kernel image + 2 * PAGE_SIZE
    // this is so we'll have a free guard page in between
    pNewStackTop = (PBYTE)NewStackBase + StackSize;

    status = _MmuReserveAndMapMemory(PagingData,
                                     (PBYTE)NewStackBase,
                                     StackSize,
                                     VA2PA(NewStackBase),
                                     PAGE_RIGHTS_READWRITE
                                     );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuReserveAndMapMemory", status);
        return status;
    }

    return status;
}

static
STATUS
_MmuMapPeInMemory(
    IN          PPAGING_DATA            PagingData,
    IN          PPE_NT_HEADER_INFO      HeaderInfo,
    IN          PVOID                   AddressToMap
    )
{
    STATUS status;
    DWORD alignedKernelSize;
    DWORD noOfFrames;
    PAGE_RIGHTS prevSectionRequiredRights;
    PAGE_RIGHTS curSectionRequiredRights;
    DWORD remainingSizeUntilPageAlignFromPrevSections;
    DWORD sectionSize;
    PVOID pAlignedAddress;

    ASSERT(NULL != PagingData );

    ASSERT(NULL != HeaderInfo);
    ASSERT(IsAddressAligned(HeaderInfo->ImageBase,PAGE_SIZE));

    ASSERT(NULL != AddressToMap);
    ASSERT(IsAddressAligned(AddressToMap,PAGE_SIZE));

    if (HeaderInfo->FileAlignment != HeaderInfo->ImageAlignment)
    {
        LOG_ERROR("We do not support loading PEs which have a different file alignment 0x%x than section alignment 0x%x!\n",
                  HeaderInfo->FileAlignment, HeaderInfo->ImageAlignment);
        return STATUS_NOT_IMPLEMENTED;
    }

    status = STATUS_SUCCESS;
    alignedKernelSize = AlignAddressUpper(HeaderInfo->Size, PAGE_SIZE);
    noOfFrames = alignedKernelSize / PAGE_SIZE;
    prevSectionRequiredRights = PAGE_RIGHTS_READ;
    curSectionRequiredRights = 0;
    sectionSize = 0;
    pAlignedAddress = NULL;

    // the first section will start after the headers end
    remainingSizeUntilPageAlignFromPrevSections = PAGE_SIZE - AddressOffset(HeaderInfo->SizeOfHeaders, PAGE_SIZE);

    LOG_TRACE_MMU("PE image base is 0x%X\n", HeaderInfo->ImageBase);

    // need to map headers if they occupy at least one page
    for (PVOID pHeaderPage = AddressToMap;
         pHeaderPage < (PVOID) PtrDiff(PtrOffset(AddressToMap, HeaderInfo->SizeOfHeaders), PAGE_SIZE);
         pHeaderPage = PtrOffset(pHeaderPage, PAGE_SIZE))
    {
        VmmMapMemoryInternal(PagingData,
                             MmuGetPhysicalAddress(PtrOffset(HeaderInfo->ImageBase, PtrDiff(pHeaderPage, AddressToMap))),
                             PAGE_SIZE,
                             pHeaderPage,
                             PAGE_RIGHTS_READ,
                             TRUE,
                             FALSE
                             );
    }

    // map each section
    for (DWORD i = 0; i < HeaderInfo->NumberOfSections; ++i)
    {
        PE_SECTION_INFO section;

        status = PeRetrieveSection(HeaderInfo,
                                   i,
                                   &section
                                   );
        if (!SUCCEEDED(status))
        {
            /// TODO: it's not ok to return here, we should actually unmap all the memory
            /// we mapped in case of failure
            LOG_FUNC_ERROR("PeRetrieveSection", status );
            return status;
        }

        LOG_TRACE_MMU("Will map section at 0x%X of size 0x%x\n", section.BaseAddress, section.Size);

        // The PE header tells us the section of the size without regard to the image's alignment
        sectionSize = AlignAddressUpper(section.Size, HeaderInfo->ImageAlignment);

        // determine section rights
        curSectionRequiredRights  = IsBooleanFlagOn( section.Characteristics, IMAGE_SCN_MEM_READ ) ? PAGE_RIGHTS_READ : 0;
        curSectionRequiredRights |= IsBooleanFlagOn( section.Characteristics, IMAGE_SCN_MEM_WRITE ) ? PAGE_RIGHTS_WRITE : 0;
        curSectionRequiredRights |= IsBooleanFlagOn( section.Characteristics, IMAGE_SCN_MEM_EXECUTE ) ? PAGE_RIGHTS_EXECUTE : 0;

        // we don't support sections which are write/execute, it's simply a bad practice and no compiler should
        // generate this kind of code
        ASSERT(!IsBooleanFlagOn(curSectionRequiredRights,PAGE_RIGHTS_WRITE | PAGE_RIGHTS_EXECUTE));

        // We do not have enough memory in this section to map the whole page
        if ( sectionSize < remainingSizeUntilPageAlignFromPrevSections )
        {
            LOG_TRACE_MMU("Remaining 0x%x, section size 0x%x\n",
                 remainingSizeUntilPageAlignFromPrevSections,
                 section.Size);

            ASSERT(remainingSizeUntilPageAlignFromPrevSections > sectionSize);
            remainingSizeUntilPageAlignFromPrevSections = remainingSizeUntilPageAlignFromPrevSections - sectionSize;

            // there may be more than 2 sections in a single page of memory
            prevSectionRequiredRights = prevSectionRequiredRights | curSectionRequiredRights;

            // continue to next section
            continue;
        }

        // Calculate the absolute VA where the section will be placed in the new mapping and page align it (it's ok to
        // do this - we need to also map the leftovers from the previous section (if there are any)
        // The reason why we don't first map the previous section is that the requested rights may differ between sections
        // and there's no benefit in calling the function twice
        pAlignedAddress = (PVOID) AlignAddressLower(
                                    PtrOffset(AddressToMap,
                                              PtrDiff(section.BaseAddress,
                                                      HeaderInfo->ImageBase)),
                                    PAGE_SIZE);

        LOG_TRACE_MMU("Aligned address is 0x%X\n", pAlignedAddress);

        if (0 != remainingSizeUntilPageAlignFromPrevSections)
        {
            // map first page of this section, this is shared with the last page
            // of the previous section(s)
            LOG_TRACE_MMU("Will map 0x%X -> 0x%X with rights 0x%x\n",
                          MmuGetPhysicalAddress(PtrOffset(HeaderInfo->ImageBase, PtrDiff(pAlignedAddress,AddressToMap))),
                          pAlignedAddress,
                          prevSectionRequiredRights | curSectionRequiredRights);

            // Because the alignment of the sections may be less than a PAGE_SIZE we may incur executables which
            // have this undesirable property (of having a PAGE mapped with both execute and write rights)
            if(IsBooleanFlagOn(prevSectionRequiredRights | curSectionRequiredRights, PAGE_RIGHTS_EXECUTE | PAGE_RIGHTS_WRITE))
            {
                LOG_WARNING("Section rights will be Write + Execute!!\n");
            }

            VmmMapMemoryInternal(PagingData,
                                 MmuGetPhysicalAddress(PtrOffset(HeaderInfo->ImageBase, PtrDiff(pAlignedAddress,AddressToMap))),
                                 PAGE_SIZE,
                                 pAlignedAddress,
                                 prevSectionRequiredRights | curSectionRequiredRights,
                                 TRUE,
                                 FALSE
                                 );

            // advance to next page
            pAlignedAddress = PtrOffset(pAlignedAddress, PAGE_SIZE);
            sectionSize = sectionSize - remainingSizeUntilPageAlignFromPrevSections;
        }

        // how much bytes are remaining until the page boundary is reached
        remainingSizeUntilPageAlignFromPrevSections = PAGE_SIZE - AddressOffset(sectionSize, PAGE_SIZE);

        // We can't map half a page (or anything less than it) that's why we do the comparison from
        // the aligned address + PAGE_SIZE because we need to have the whole PAGE contained in the current
        // section to be able to map it;
        for (PVOID pPage = pAlignedAddress;
             PtrOffset(pPage, PAGE_SIZE) <= PtrOffset(pAlignedAddress, sectionSize);
             pPage = PtrOffset(pPage, PAGE_SIZE))
        {
            LOG_TRACE_MMU("Will map 0x%X -> 0x%X with rights 0x%x\n",
                          MmuGetPhysicalAddress(PtrOffset(HeaderInfo->ImageBase, PtrDiff(pPage,AddressToMap))),
                          pPage,
                          curSectionRequiredRights);

            VmmMapMemoryInternal(PagingData,
                                 MmuGetPhysicalAddress(PtrOffset(HeaderInfo->ImageBase, PtrDiff(pPage,AddressToMap))),
                                 PAGE_SIZE,
                                 pPage,
                                 curSectionRequiredRights,
                                 TRUE,
                                 FALSE
                                 );
        }

        // we certainly mapped all the memory related to the previous sections
        // => update previous section rights to reflect the rights of the current area
        prevSectionRequiredRights = curSectionRequiredRights;
    }

    // The last section may have not ended nicely at the PAGE boundary => we may need to
    // map it afterwards
    if (sectionSize != 0)
    {
        LOG_TRACE_MMU("Will map 0x%X -> 0x%X with rights 0x%x\n",
                      MmuGetPhysicalAddress(PtrOffset(HeaderInfo->ImageBase, PtrDiff(pAlignedAddress, AddressToMap))),
                      pAlignedAddress,
                      prevSectionRequiredRights);

        // This can occur if there are multiple sections at the end of the PE which do not add up
        // to a full page
        if (IsBooleanFlagOn(prevSectionRequiredRights, PAGE_RIGHTS_EXECUTE | PAGE_RIGHTS_WRITE))
        {
            LOG_WARNING("Section rights will be Write + Execute!!\n");
        }

        VmmMapMemoryInternal(PagingData,
                             MmuGetPhysicalAddress(PtrOffset(HeaderInfo->ImageBase, PtrDiff(pAlignedAddress, AddressToMap))),
                             PAGE_SIZE,
                             pAlignedAddress,
                             prevSectionRequiredRights,
                             TRUE,
                             FALSE
        );
    }

    LOG_TRACE_MMU("PE mapped succeesfully\n");

    return status;

}

static
STATUS
_MmuMapKernelMemory(
    IN          PPAGING_DATA            PagingData,
    IN          PHYSICAL_ADDRESS        PhysicalAddress,
    IN          PPE_NT_HEADER_INFO      KernelInfo
    )
{
    STATUS status;
    PHYSICAL_ADDRESS kernelPa;
    DWORD noOfFrames;

    ASSERT(NULL != PagingData);
    ASSERT(NULL != KernelInfo);
    ASSERT(IsAddressAligned(KernelInfo->ImageBase,PAGE_SIZE));

    status = STATUS_SUCCESS;
    noOfFrames = AlignAddressUpper(KernelInfo->Size, PAGE_SIZE) / PAGE_SIZE;

    // Mark the kernel physical frames as reserved
    kernelPa = PmmReserveMemoryEx(noOfFrames, PhysicalAddress);
    if ((NULL == kernelPa) || (kernelPa != PhysicalAddress))
    {
        LOG_ERROR("PmmRequestMemoryEx failed!\n");
        return STATUS_PHYSICAL_MEMORY_NOT_AVAILABLE;
    }

    status = _MmuMapPeInMemory(PagingData, KernelInfo, KernelInfo->ImageBase);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuMapPeInMemory", status);
        LOG_ERROR("Unable to perform high VA mapping for kernel!\n");
        return status;
    }

    // Perform identity mapping - needed by APs
    // Will be discarded after all the APs get in 64-bit mode
    status = _MmuMapPeInMemory(PagingData, KernelInfo, VA2PA(KernelInfo->ImageBase));
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_MmuMapPeInMemory", status);
        LOG_ERROR("Unable to perform identity mapping for kernel!\n");
        return status;
    }

    LOG("Kernel mapped succeesfully\n");

    return status;
}

static
STATUS
_MmuReserveAndMapMemory(
    IN          PPAGING_DATA            PagingData,
    IN          PVOID                   VirtualAddress,
    IN          DWORD                   Size,
    IN          PHYSICAL_ADDRESS        PhysicalAddress,
    IN          PAGE_RIGHTS             AccessRights
    )
{
    PHYSICAL_ADDRESS pa;
    DWORD noOfFrames;

    ASSERT( NULL != VirtualAddress );

    ASSERT( 0 != Size );
    ASSERT( IsAddressAligned( Size, PAGE_SIZE ));

    noOfFrames = Size / PAGE_SIZE;

    pa = PmmReserveMemoryEx( noOfFrames, PhysicalAddress);
    if( ( NULL == pa ) || ( pa != PhysicalAddress) )
    {
        LOG_ERROR("PmmReserveMemoryEx failed for PA 0x%X!\n", PhysicalAddress);
        return STATUS_PHYSICAL_MEMORY_NOT_AVAILABLE;
    }

    ASSERT_INFO( pa == PhysicalAddress,
                "Requested PA: 0x%X, Received: 0x%X\n",
                PhysicalAddress, pa );

    VmmMapMemoryInternal(PagingData,
                         pa,
                         (DWORD) Size,
                         VirtualAddress,
                         AccessRights,
                         TRUE,
                         FALSE
                         );

    return STATUS_SUCCESS;
}

static
STATUS
_MmuInitializeHeap(
    OUT         PMMU_HEAP_DATA          Heap,
    IN          DWORD                   HeapBaseSize,
    IN          WORD                    HeapPercentageSize
    )
{
    STATUS status;
    DWORD framesForHeapStructures;
    QWORD heapSize;
    PVOID heapBaseAddress;

    ASSERT( NULL != Heap );

    status = STATUS_SUCCESS;

    // calculate number of frames used by the heap
    framesForHeapStructures = _MmuCalculateReservedFrames(HeapBaseSize,
                                                          HeapPercentageSize,
                                                          PmmGetTotalSystemMemory()
                                                          );
    LOG("Frames for heap structures: 0x%x\n", framesForHeapStructures);

    heapSize = (QWORD)framesForHeapStructures * PAGE_SIZE;

    LOG("Total size reserved for heap: %U bytes ( %U KB )\n", heapSize, heapSize / KB_SIZE);

    heapBaseAddress = VmmAllocRegion(NULL,
        heapSize,
        VMM_ALLOC_TYPE_RESERVE | VMM_ALLOC_TYPE_COMMIT,
        PAGE_RIGHTS_READWRITE
    );
    if (heapBaseAddress == NULL)
    {
        LOG_ERROR("VmmAlloc failed to reserve & commit a heap of size %U!\n", heapSize);
        return STATUS_MEMORY_CANNOT_BE_RESERVED;
    }

    status = ClHeapInit(heapBaseAddress, heapSize, &Heap->Heap);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("ClHeapInit", status);
        return status;
    }

    LOG("ClHeapInit suceeded\n");

    LockInit(&Heap->HeapLock);

    return status;
}

static
_Always_(_When_(IsBooleanFlagOn(Flags, PoolAllocatePanicIfFail), RET_NOT_NULL))
PTR_SUCCESS
PVOID
_MmuAllocateFromPoolWithTag(
    IN      MMU_HEAP_INDEX          Heap,
    IN      DWORD                   Flags,
    IN      DWORD                   AllocationSize,
    IN      DWORD                   Tag,
    IN      DWORD                   AllocationAlignment
    )
{
    PVOID pResult;
    INTR_STATE oldState;

    ASSERT( Heap < MmuHeapIndexReserved );
    ASSERT( NULL != m_mmuData.Heaps[Heap].Heap );

    LockAcquire(&m_mmuData.Heaps[Heap].HeapLock, &oldState );
    pResult = ClHeapAllocatePoolWithTag(m_mmuData.Heaps[Heap].Heap,
                                      Flags,
                                      AllocationSize,
                                      Tag,
                                      AllocationAlignment
                                      );
    LockRelease(&m_mmuData.Heaps[Heap].HeapLock, oldState );

    return pResult;
}

static
void
_MmuFreeFromPoolWithTag(
    IN      MMU_HEAP_INDEX          Heap,
    _Pre_notnull_ _Post_ptr_invalid_
            PVOID                   MemoryAddress,
    IN      DWORD                   Tag
    )
{
    INTR_STATE oldState;

    ASSERT(Heap < MmuHeapIndexReserved);
    ASSERT( NULL != m_mmuData.Heaps[Heap].Heap );

    LockAcquire(&m_mmuData.Heaps[Heap].HeapLock, &oldState);
    ClHeapFreePoolWithTag(m_mmuData.Heaps[Heap].Heap,
                        MemoryAddress,
                        Tag
                        );
    LockRelease(&m_mmuData.Heaps[Heap].HeapLock, oldState);
}

static
void
_MmuRemapDisplay(
    IN          PPAGING_DATA            PagingData
    )
{
    // because of the way the PMM is implemented
    // memory under 1MB is reserved and will never be allocated
    // => we don't need to request the frames from the PMM

    VmmMapMemoryInternal(PagingData,
                         (PHYSICAL_ADDRESS) BASE_VIDEO_ADDRESS,
                         AlignAddressUpper(SCREEN_SIZE, PAGE_SIZE),
                         (PVOID) PA2VA(BASE_VIDEO_ADDRESS),
                         PAGE_RIGHTS_READWRITE,
                         TRUE,
                         FALSE
                         );
}

static
STATUS
_MmuZeroWorkerThreadFunction(
    IN_OPT      PVOID           Context
    )
{
    PLIST_ENTRY pListHead;
    STATUS status;
    PMMU_ZERO_WORKER_THREAD_CTX pCtx;
    PEX_EVENT pEvent;
    PLIST_ENTRY pCurrentEntry;
    PLOCK pLock;

    LOG_FUNC_START;

    ASSERT( NULL != Context );

    status = STATUS_SUCCESS;
    pCtx = (PMMU_ZERO_WORKER_THREAD_CTX) Context;
    pCurrentEntry = NULL;

    pListHead = pCtx->PagesToZeroList;
    ASSERT( NULL != pListHead );

    pEvent = pCtx->NewPagesEvent;
    ASSERT( NULL != pEvent );

    pLock = pCtx->PagesLock;
    ASSERT( NULL != pLock );

    ExFreePoolWithTag(pCtx, HEAP_MMU_TAG);
    pCtx = NULL;

    // warning C4127: conditional expression is constant
#pragma warning(suppress:4127)
    while (TRUE)
    {
        PMMU_ZERO_WORKER_ITEM pItem;
        INTR_STATE oldState;
        DWORD noOfBytes;
        PVOID pAddr;

        pItem = NULL;
        noOfBytes = 0;
        pAddr = NULL;

        // may use executive timer in the future
        ExEventWaitForSignal(pEvent);

        LockAcquire(pLock, &oldState);
        pCurrentEntry = RemoveHeadList(pListHead);
        LockRelease(pLock, oldState);

        if (pCurrentEntry == pListHead)
        {
            // list is empty :(
            ExEventClearSignal(pEvent);

            // wait for another signal
            continue;
        }

        pItem = CONTAINING_RECORD(pCurrentEntry, MMU_ZERO_WORKER_ITEM, ListEntry);

        noOfBytes = pItem->NumberOfFrames * PAGE_SIZE;
        pAddr = MmuMapMemoryEx(pItem->PhysicalAddress,
                               noOfBytes,
                               PAGE_RIGHTS_READWRITE,
                               FALSE,
                               FALSE,
                               NULL
                               );
        ASSERT( NULL != pAddr );

        // zero the memory, that's our job :)
        memzero(pAddr, noOfBytes);

        // truly release physical addresses
        PmmReleaseMemory(pItem->PhysicalAddress, pItem->NumberOfFrames );

        // it's ok, this does not release memory => no oo loop
        MmuUnmapSystemMemory(pAddr, noOfBytes);

        _MmuFreeFromPoolWithTag(MmuHeapIndexSpecial, pItem, HEAP_MMU_TAG );
        pItem = NULL;
    }

    LOG_FUNC_END;

    NOT_REACHED;

    return status;
}