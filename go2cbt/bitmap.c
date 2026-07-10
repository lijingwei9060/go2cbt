#include "go2cbt.h"


// ========================================
// InitCbtState - Initialize CBT bitmap for a disk
//
// Allocates NonPagedPool memory for the RTL_BITMAP buffer,
// initializes the bitmap and spinlock.
//
// Parameters:
//   cbtState       - [out] CBT state structure to initialize
//   diskSizeBytes  - [in]  Total size of the physical disk in bytes
//
// Returns:
//   STATUS_SUCCESS on success
//   STATUS_INSUFFICIENT_RESOURCES if pool allocation fails
// ========================================
NTSTATUS
InitCbtState(
	_Out_ PDISK_CBT_STATE cbtState,
	_In_  ULONGLONG diskSizeBytes
)
{
	if (diskSizeBytes == 0) {
		return STATUS_INVALID_PARAMETER;
	}

	RtlZeroMemory(cbtState, sizeof(DISK_CBT_STATE));

	ULONGLONG totalBits = (diskSizeBytes + CBT_BLOCK_SIZE - 1) / CBT_BLOCK_SIZE;

	// RTL_BITMAP buffer must be aligned to ULONG boundary
	ULONG totalBytes = (ULONG)(((totalBits + 31) / 32) * sizeof(ULONG));

	// Allocate from NonPagedPool because RtlSetBits may be called at IRQL > PASSIVE_LEVEL
	cbtState->Buffer = (PULONG)ExAllocatePool2(
		POOL_FLAG_NON_PAGED,
		totalBytes,
		CBT_BITMAP_POOL_TAG
	);

	if (!cbtState->Buffer) {
		KdPrint(("CBT: Failed to allocate %lu bytes for bitmap (%llu blocks)\n", totalBytes, totalBits));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// Zero the bitmap buffer (all blocks initially "unchanged")
	RtlZeroMemory(cbtState->Buffer, totalBytes);

	// Initialize RTL_BITMAP structure
	RtlInitializeBitMap(&cbtState->Bitmap, cbtState->Buffer, (ULONG)totalBits);

	// Store metadata
	cbtState->TotalBits = totalBits;
	cbtState->TotalBytes = totalBytes;

	// Initialize spinlock for protecting concurrent Set/Clear operations
	KeInitializeSpinLock(&cbtState->Lock);

	KdPrint(("CBT: Bitmap initialized: %llu blocks, %lu bytes, block_size=%uKB\n", totalBits, totalBytes, CBT_BLOCK_SIZE / 1024));

	return STATUS_SUCCESS;
}





// ================================================
// CleanupCbtState: Free CBT bitmap memory (called during driver unload).
//
// No lock needed: by this point all hooks have been restored,
// so no new writes will enter HwReadWrite.
// ================================================
void CleanupCbtState(_In_ PDISK_CBT_STATE cbtState)
{
	if (!cbtState) return;

	if (cbtState->Buffer) {
		ExFreePoolWithTag(cbtState->Buffer, CBT_BITMAP_POOL_TAG);
		cbtState->Buffer = NULL;
	}

	RtlZeroMemory(cbtState, sizeof(DISK_CBT_STATE));
}