#include "HAL9000.h"
#include "io.h"
#include "filesystem.h"
#include "iomu.h"

#include "strutils.h"

// X:\\ => minimum 3 letters to open root
#define FILE_NAME_MIN_LEN               3

__forceinline
static
void
_IoFreeFileObject(
    IN          PFILE_OBJECT            FileObject
    )
{
    ASSERT(NULL != FileObject);

    ExFreePoolWithTag(FileObject, HEAP_FILE_OBJECT_TAG);
}

__forceinline
static
void
_IoAllocateFileObject(
    INOUT       PIO_STACK_LOCATION      StackLocation,
    IN_Z        char*                   FileName,
    IN          BOOLEAN                 Asynchronous,
    IN          BOOLEAN                 Create,
    IN          BOOLEAN                 Directory
    )
{
    PFILE_OBJECT pFileObject;

    ASSERT(NULL != StackLocation);
    ASSERT(NULL != FileName);

    pFileObject = ExAllocatePoolWithTag(PoolAllocateZeroMemory, sizeof(FILE_OBJECT), HEAP_FILE_OBJECT_TAG, 0);
    ASSERT(NULL != pFileObject);

    pFileObject->FileName = (char*) FileName;

    pFileObject->Flags.Asynchronous = Asynchronous;
    pFileObject->Flags.Create = Create;
    pFileObject->Flags.DirectoryFile = Directory;

    pFileObject->FileSystemDevice = StackLocation->DeviceObject;

    StackLocation->FileObject = pFileObject;
}

__forceinline
static
BOOLEAN
_IoIsFilePathValid(
    IN_Z        char*                   FileName
    )
{
    DWORD strLength;

    ASSERT(NULL != FileName);

    strLength = strlen(FileName);
    if (strLength < FILE_NAME_MIN_LEN)
    {
        return FALSE;
    }

    // there must be at least a backslash
    if (FileName == strrchr(FileName, '\\'))
    {
        return FALSE;
    }

    return TRUE;
}

static
STATUS
_IoReadWriteFile(
    IN          PFILE_OBJECT            FileHandle,
    _When_(!Write,OUT_WRITES_BYTES(Length))
    _When_(Write,IN_READS_BYTES(Length))
                PVOID                   Buffer,
    IN          QWORD                   Length,
    IN_OPT      QWORD*                  FileOffset,
    OUT         QWORD*                  BytesTransferred,
    IN          BOOLEAN                 Write
    );

STATUS
IoCreateFile(
    OUT_PTR     PFILE_OBJECT*           Handle,
    IN_Z        char*                   FileName,
    IN          BOOLEAN                 Directory,
    IN          BOOLEAN                 Create,
    IN          BOOLEAN                 Asynchronous
    )
{
    STATUS status;
    PIRP pIrp;
    PVPB pVpb;
    char driveLetter;
    PDEVICE_OBJECT pFileSystemDevice;
    PIO_STACK_LOCATION pStackLocation;

    ASSERT(NULL != Handle);
    ASSERT(NULL != FileName);

    status = STATUS_SUCCESS;
    pIrp = NULL;
    pVpb = NULL;
    pFileSystemDevice = NULL;
    pStackLocation = NULL;

    // the search is case insensitive, but its nicer to print
    // driver letters in uppercase :)
    driveLetter = toupper(FileName[0]);

    if (!_IoIsFilePathValid(FileName))
    {
        LOG_ERROR("_IoIsFilePathValid failed\n");
        return STATUS_INVALID_FILE_NAME;
    }

    pVpb = IomuSearchForVpb(driveLetter);
    if (NULL == pVpb)
    {
        LOG_ERROR("There is no corresponding VPB for drive letter: %c\n", driveLetter);
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    LOG_TRACE_IO("Found VPB for file %s\n", FileName);

    if (!pVpb->Flags.Mounted)
    {
        LOG_ERROR("There is no file system mounted on drive %c\n", driveLetter);
        return STATUS_DEVICE_NO_FILESYSTEM_MOUNTED;
    }

    pFileSystemDevice = pVpb->FilesystemDevice;
    ASSERT(NULL != pFileSystemDevice);

    pIrp = IoAllocateIrp(pFileSystemDevice->StackSize);
    if (NULL == pIrp)
    {
        LOG_FUNC_ERROR_ALLOC("IoAllocateIrp", sizeof(IRP));
        return STATUS_HEAP_NO_MORE_MEMORY;
    }

    pStackLocation = IoGetNextIrpStackLocation(pIrp);
    pStackLocation->MajorFunction = IRP_MJ_CREATE;
    pStackLocation->DeviceObject = pFileSystemDevice;

    // currently there are no parameters for the create operation

    // create the FILE_OBJECT for the stack location
    _IoAllocateFileObject(pStackLocation, &FileName[FILE_NAME_MIN_LEN - 1], Asynchronous, Create, Directory);

    __try
    {

        // call file system
        status = IoCallDriver(pFileSystemDevice, pIrp);
        if (!SUCCEEDED(status))
        {
            __leave;
        }

        status = pIrp->IoStatus.Status;
    }
    __finally
    {
        if (!SUCCEEDED(status))
        {
            LOG_TRACE_IO("[ERROR]IoCallDriver failed with status 0x%x\n", status);

            ASSERT(NULL != pStackLocation);
            ASSERT(NULL != pStackLocation->FileObject);
            _IoFreeFileObject(pStackLocation->FileObject);
        }
        else
        {
            LOG_TRACE_IO("File size: 0x%X\n", pStackLocation->FileObject->FileSize);

            // complete file handle
            *Handle = pStackLocation->FileObject;
        }

        if (NULL != pIrp)
        {
            IoFreeIrp(pIrp);
            pIrp = NULL;
        }
    }

    return status;
}

STATUS
IoCloseFile(
    IN          PFILE_OBJECT            FileHandle
    )
{
    STATUS status;
    PIRP pIrp;
    PDEVICE_OBJECT pFileSystemDevice;
    PIO_STACK_LOCATION pStackLocation;

    ASSERT(NULL != FileHandle);

    status = STATUS_SUCCESS;
    pIrp = NULL;
    pFileSystemDevice = FileHandle->FileSystemDevice;
    pStackLocation = NULL;

    ASSERT(NULL != pFileSystemDevice);

    pIrp = IoAllocateIrp(pFileSystemDevice->StackSize);
    if (NULL == pIrp)
    {
        LOG_FUNC_ERROR_ALLOC("IoAllocateIrp", sizeof(IRP));
        return STATUS_HEAP_NO_MORE_MEMORY;
    }

    pStackLocation = IoGetNextIrpStackLocation(pIrp);

    pStackLocation->MajorFunction = IRP_MJ_CLOSE;
    pStackLocation->FileObject = FileHandle;

    __try
    {
        status = IoCallDriver(pFileSystemDevice, pIrp);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("IoCallDriver", status);
            __leave;
        }
        status = pIrp->IoStatus.Status;
    }
    __finally
    {
        if (NULL != pIrp)
        {
            IoFreeIrp(pIrp);
            pIrp = NULL;
        }
    }

    return status;
}

STATUS
IoReadFile(
    IN          PFILE_OBJECT            FileHandle,
    IN          QWORD                   BytesToRead,
    IN_OPT      QWORD*                  FileOffset,
    OUT_WRITES_BYTES(BytesToRead)
                PVOID                   Buffer,
    OUT         QWORD*                  BytesRead
    )
{
    return _IoReadWriteFile(FileHandle,
                            Buffer,
                            BytesToRead,
                            FileOffset,
                            BytesRead,
                            FALSE);
}

STATUS
IoWriteFile(
    IN          PFILE_OBJECT            FileHandle,
    IN          QWORD                   BytesToWrite,
    IN_OPT      QWORD*                  FileOffset,
    IN_READS_BYTES(BytesToWrite)
                PVOID                   Buffer,
    OUT         QWORD*                  BytesWritten
    )
{
    return _IoReadWriteFile(FileHandle,
                            Buffer,
                            BytesToWrite,
                            FileOffset,
                            BytesWritten,
                            TRUE);
}

STATUS
IoGetFileSize(
    IN          PFILE_OBJECT            FileHandle,
    OUT         QWORD*                  FileSize
    )
{
    if (FileHandle == NULL)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (FileSize == NULL)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    *FileSize = FileHandle->FileSize;

    return STATUS_SUCCESS;
}

STATUS
IoQueryInformationFile(
    IN          PFILE_OBJECT            FileHandle,
    OUT         PFILE_INFORMATION       FileInformation
    )
{
    STATUS status;
    PIRP pIrp;
    PDEVICE_OBJECT pFileSystemDevice;
    PIO_STACK_LOCATION pStackLocation;

    ASSERT(NULL != FileHandle);
    ASSERT(NULL != FileInformation);

    status = STATUS_SUCCESS;
    pIrp = NULL;
    pFileSystemDevice = NULL;
    pStackLocation = NULL;

    pFileSystemDevice = FileHandle->FileSystemDevice;
    ASSERT(NULL != pFileSystemDevice);

    pIrp = IoAllocateIrp(pFileSystemDevice->StackSize);
    if (NULL == pIrp)
    {
        LOG_FUNC_ERROR_ALLOC("IoAllocateIrp", sizeof(IRP));
        return STATUS_HEAP_NO_MORE_MEMORY;
    }

    pStackLocation = IoGetNextIrpStackLocation(pIrp);
    pStackLocation->MajorFunction = IRP_MJ_QUERY_INFORMATION;
    pStackLocation->MinorFunction = IRP_MN_INFORMATION_FILE_INFORMATION;
    pStackLocation->DeviceObject = pFileSystemDevice;
    pStackLocation->FileObject = FileHandle;

    // setup parameters
    pStackLocation->Parameters.QueryFile.Length = sizeof(FILE_INFORMATION);
    pIrp->Buffer = FileInformation;

    __try
    {
        // call file system
        status = IoCallDriver(pFileSystemDevice, pIrp);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("IoCallDriver", status);
            __leave;
        }

        status = pIrp->IoStatus.Status;
    }
    __finally
    {
        if (NULL != pIrp)
        {
            IoFreeIrp(pIrp);
            pIrp = NULL;
        }
    }

    return status;
}

STATUS
IoQueryDirectoryFile(
    IN          PFILE_OBJECT                    FileHandle,
    IN          DWORD                           BufferSize,
    _When_(0==BufferSize,OUT_OPT)
    _When_(0!=BufferSize,OUT)
                PFILE_DIRECTORY_INFORMATION     DirectoryInformation,
    OUT         DWORD*                          SizeRequired
    )
{
    STATUS status;
    PIRP pIrp;
    PDEVICE_OBJECT pFileSystemDevice;
    PIO_STACK_LOCATION pStackLocation;

    ASSERT(NULL != FileHandle);
    ASSERT(NULL != DirectoryInformation || 0 == BufferSize);
    ASSERT(NULL != SizeRequired);

    status = STATUS_SUCCESS;
    pIrp = NULL;
    pFileSystemDevice = NULL;
    pStackLocation = NULL;

    pFileSystemDevice = FileHandle->FileSystemDevice;
    ASSERT(NULL != pFileSystemDevice);

    pIrp = IoAllocateIrp(pFileSystemDevice->StackSize);
    if (NULL == pIrp)
    {
        LOG_FUNC_ERROR_ALLOC("IoAllocateIrp", sizeof(IRP));
        return STATUS_HEAP_NO_MORE_MEMORY;
    }

    pStackLocation = IoGetNextIrpStackLocation(pIrp);
    pStackLocation->MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
    pStackLocation->MinorFunction = IRP_MN_QUERY_DIRECTORY;
    pStackLocation->DeviceObject = pFileSystemDevice;

    pStackLocation->FileObject = FileHandle;
    pStackLocation->Parameters.QueryDirectory.Length = BufferSize;

    pIrp->Buffer = DirectoryInformation;

    __try
    {
        status = IoCallDriver(pFileSystemDevice, pIrp);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("IoCallDriver", status);
            __leave;
        }

        status = pIrp->IoStatus.Status;

        ASSERT(pIrp->IoStatus.Information <= MAX_DWORD);
        *SizeRequired = (DWORD)pIrp->IoStatus.Information;
    }
    __finally
    {
        if (NULL != pIrp)
        {
            IoFreeIrp(pIrp);
            pIrp = NULL;
        }
    }

    return status;
}

static
STATUS
_IoReadWriteFile(
    IN          PFILE_OBJECT            FileHandle,
    _When_(!Write,OUT_WRITES_BYTES(Length))
    _When_(Write,IN_READS_BYTES(Length))
                PVOID                   Buffer,
    IN          QWORD                   Length,
    IN_OPT      QWORD*                  FileOffset,
    OUT         QWORD*                  BytesTransferred,
    IN          BOOLEAN                 Write
    )
{
    STATUS status;
    PIRP pIrp;
    PDEVICE_OBJECT pFileSystemDevice;
    PIO_STACK_LOCATION pStackLocation;
    QWORD fileOffset;

    LOG_FUNC_START;

    ASSERT(NULL != FileHandle);
    ASSERT(NULL != Buffer);
    ASSERT(NULL != BytesTransferred);

    status = STATUS_SUCCESS;
    pIrp = NULL;
    pFileSystemDevice = NULL;
    pStackLocation = NULL;

    if (FileHandle->Flags.Asynchronous)
    {
        ASSERT(NULL != FileOffset);

        fileOffset = *FileOffset;
    }
    else
    {
        if (NULL == FileOffset)
        {
            fileOffset = FileHandle->CurrentByteOffset;
        }
        else
        {
            fileOffset = *FileOffset;
        }
    }

    pFileSystemDevice = FileHandle->FileSystemDevice;
    ASSERT(NULL != pFileSystemDevice);

    pIrp = IoAllocateIrp(pFileSystemDevice->StackSize);
    if (NULL == pIrp)
    {
        LOG_FUNC_ERROR_ALLOC("IoAllocateIrp", sizeof(IRP));
        return STATUS_HEAP_NO_MORE_MEMORY;
    }
    pIrp->Buffer = Buffer;

    // pass async parameter
    pIrp->Flags.Asynchronous = FileHandle->Flags.Asynchronous;

    pStackLocation = IoGetNextIrpStackLocation(pIrp);
    pStackLocation->MajorFunction = Write ? IRP_MJ_WRITE : IRP_MJ_READ;
    pStackLocation->DeviceObject = pFileSystemDevice;

    // setup parameters
    pStackLocation->Parameters.ReadWrite.Length = Length;
    pStackLocation->Parameters.ReadWrite.Offset = fileOffset;
    pStackLocation->FileObject = FileHandle;

    __try
    {
        // call file system
        status = IoCallDriver(pFileSystemDevice, pIrp);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("IoCallDriver", status);
            __leave;
        }

        status = pIrp->IoStatus.Status;
        *BytesTransferred = pIrp->IoStatus.Information;

        if (SUCCEEDED(status))
        {
            // if synchronous operation => update file offset
            if (!FileHandle->Flags.Asynchronous)
            {
                FileHandle->CurrentByteOffset = FileHandle->CurrentByteOffset + *BytesTransferred;
            }
        }
    }
    __finally
    {
        if (NULL != pIrp)
        {
            IoFreeIrp(pIrp);
            pIrp = NULL;
        }

        LOG_FUNC_END;
    }

    return status;
}