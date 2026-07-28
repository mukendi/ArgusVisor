#include "ept.h"


void ArgusVisorTerminateEpt() {
	PEPT_DATA eptData = GlobalEptData;
	GlobalEptData = nullptr;

	if (eptData == nullptr) {
		return;
	}

	if (eptData->TablePages != nullptr) {
		for (ULONG i = 0; i < eptData->TablePageCount; i++) {
			if (eptData->TablePages[i] != nullptr) {
				MmFreeContiguousMemory(eptData->TablePages[i]);
			}
		}
		ExFreePoolWithTag(eptData->TablePages, 'LpEH');
	}

	ExFreePoolWithTag(eptData, 'TpEH');

	if (GlobalEptTestPage != nullptr) {
		MmFreeContiguousMemory(GlobalEptTestPage);
		GlobalEptTestPage = nullptr;
		GlobalEptTestPagePhysical.QuadPart = 0;
		GlobalEptTestPte = nullptr;
		InterlockedExchange(&GlobalEptTestArmed, 0);
	}
}

PVOID ArgusVisorAllocateEptTablePage(PEPT_DATA EptData) {
	if (EptData->TablePageCount >= EptData->TablePageCapacity) {
		return nullptr;
	}

	PHYSICAL_ADDRESS low = {};
	PHYSICAL_ADDRESS high = {};
	PHYSICAL_ADDRESS boundary = {};
	high.QuadPart = -1;

	PVOID page = MmAllocateContiguousMemorySpecifyCache(
		PAGE_SIZE, low, high, boundary, MmCached);
	if (page == nullptr) {
		return nullptr;
	}

	RtlZeroMemory(page, PAGE_SIZE);
	EptData->TablePages[EptData->TablePageCount++] = page;
	return page;
}

ULONG64 ArgusVisorEptTablePointer(PVOID TablePage) {
	const ULONG64 pa = MmGetPhysicalAddress(TablePage).QuadPart;
	return kEptReadAccess | kEptWriteAccess | kEptExecuteAccess |
		(pa & kEpt4KbAddressMask);
}

ULONG64* ArgusVisorGetEptPteForGpa(ULONG64 GuestPhysicalAddress, bool SplitLargePage) {
	if (GlobalEptData == nullptr || GlobalEptData->Pml4VirtualAddress == nullptr) {
		return nullptr;
	}

	const ULONG pml4Index = (ULONG)((GuestPhysicalAddress >> 39) & 0x1FF);
	const ULONG pdptIndex = (ULONG)((GuestPhysicalAddress >> 30) & 0x1FF);
	const ULONG pdIndex = (ULONG)((GuestPhysicalAddress >> 21) & 0x1FF);
	const ULONG ptIndex = (ULONG)((GuestPhysicalAddress >> 12) & 0x1FF);

	PULONG64 pml4 = (PULONG64)GlobalEptData->Pml4VirtualAddress;
	if (pml4[pml4Index] == 0) {
		return nullptr;
	}

	PHYSICAL_ADDRESS pdptPa = {};
	pdptPa.QuadPart = pml4[pml4Index] & kEpt4KbAddressMask;
	PULONG64 pdpt = (PULONG64)MmGetVirtualForPhysical(pdptPa);
	if (pdpt == nullptr || pdpt[pdptIndex] == 0) {
		return nullptr;
	}

	PHYSICAL_ADDRESS pdPa = {};
	pdPa.QuadPart = pdpt[pdptIndex] & kEpt4KbAddressMask;
	PULONG64 pd = (PULONG64)MmGetVirtualForPhysical(pdPa);
	if (pd == nullptr || pd[pdIndex] == 0) {
		return nullptr;
	}

	if ((pd[pdIndex] & kEptLargePage) != 0) {
		if (!SplitLargePage) {
			return nullptr;
		}

		const ULONG64 largeEntry = pd[pdIndex];
		const ULONG64 largePageBase = largeEntry & kEpt2MbAddressMask;
		const ULONG64 memoryType = largeEntry & (7ULL << 3);
		const ULONG64 permissions = largeEntry & (kEptReadAccess | kEptWriteAccess | kEptExecuteAccess);

		PULONG64 pt = (PULONG64)ArgusVisorAllocateEptTablePage(GlobalEptData);
		if (pt == nullptr) {
			return nullptr;
		}

		for (ULONG i = 0; i < 512; i++) {
			const ULONG64 pageBase = largePageBase + ((ULONG64)i * PAGE_SIZE);
			pt[i] = permissions | memoryType | (pageBase & kEpt4KbAddressMask);
		}

		pd[pdIndex] = ArgusVisorEptTablePointer(pt);
		ARGUSVISOR_LOG_INFO("EPT split 2MB page: gpa=0x%llX pd_index=%lu pt=%p\n",
			GuestPhysicalAddress,
			pdIndex,
			pt);
	}

	PHYSICAL_ADDRESS ptPa = {};
	ptPa.QuadPart = pd[pdIndex] & kEpt4KbAddressMask;
	PULONG64 pt = (PULONG64)MmGetVirtualForPhysical(ptPa);
	if (pt == nullptr) {
		return nullptr;
	}

	return &pt[ptIndex];
}

bool ArgusVisorIsRamPage2Mb(ULONG64 PageBase, PPHYSICAL_MEMORY_RANGE Ranges) {
	const ULONG64 pageEnd = PageBase + kEpt2MbPageSize - 1;

	for (ULONG i = 0; ; i++) {
		const ULONG64 rangeBase = Ranges[i].BaseAddress.QuadPart;
		const ULONG64 rangeSize = Ranges[i].NumberOfBytes.QuadPart;
		if (rangeBase == 0 && rangeSize == 0) {
			break;
		}

		const ULONG64 rangeEnd = rangeBase + rangeSize - 1;
		if (PageBase <= rangeEnd && pageEnd >= rangeBase) {
			return true;
		}
	}

	return false;
}

NTSTATUS ArgusVisorInitializeEpt() {
	if (GlobalEptData != nullptr) {
		return STATUS_SUCCESS;
	}

	PEPT_DATA eptData = (PEPT_DATA)ExAllocatePool2(
		POOL_FLAG_NON_PAGED,
		sizeof(EPT_DATA),
		'TpEH');
	if (eptData == nullptr) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(eptData, sizeof(EPT_DATA));

	eptData->TablePageCapacity = kArgusVisorMaxEptTablePages;
	eptData->TablePages = (PVOID*)ExAllocatePool2(
		POOL_FLAG_NON_PAGED,
		sizeof(PVOID) * eptData->TablePageCapacity,
		'LpEH');
	if (eptData->TablePages == nullptr) {
		ExFreePoolWithTag(eptData, 'TpEH');
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(eptData->TablePages, sizeof(PVOID) * eptData->TablePageCapacity);
	GlobalEptData = eptData;

	PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
	if (ranges == nullptr) {
		ArgusVisorTerminateEpt();
		return STATUS_UNSUCCESSFUL;
	}

	ULONG64 highestPhysicalAddress = 0x100000000ULL;
	for (ULONG i = 0; ; i++) {
		const ULONG64 rangeBase = ranges[i].BaseAddress.QuadPart;
		const ULONG64 rangeSize = ranges[i].NumberOfBytes.QuadPart;
		if (rangeBase == 0 && rangeSize == 0) {
			break;
		}

		const ULONG64 rangeEnd = rangeBase + rangeSize;
		if (rangeEnd > highestPhysicalAddress) {
			highestPhysicalAddress = rangeEnd;
		}
	}

	highestPhysicalAddress =
		(highestPhysicalAddress + kEpt2MbPageSize - 1) & ~(kEpt2MbPageSize - 1);

	PULONG64 pml4 = (PULONG64)ArgusVisorAllocateEptTablePage(eptData);
	if (pml4 == nullptr) {
		ExFreePool(ranges);
		ExFreePoolWithTag(eptData->TablePages, 'LpEH');
		ExFreePoolWithTag(eptData, 'TpEH');
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (ULONG64 gpa = 0; gpa < highestPhysicalAddress; gpa += kEpt2MbPageSize) {
		const ULONG pml4Index = (ULONG)((gpa >> 39) & 0x1FF);
		const ULONG pdptIndex = (ULONG)((gpa >> 30) & 0x1FF);
		const ULONG pdIndex = (ULONG)((gpa >> 21) & 0x1FF);

		if (pml4[pml4Index] == 0) {
			PVOID pdptPage = ArgusVisorAllocateEptTablePage(eptData);
			if (pdptPage == nullptr) {
				ExFreePool(ranges);
				ArgusVisorTerminateEpt();
				return STATUS_INSUFFICIENT_RESOURCES;
			}
			pml4[pml4Index] = ArgusVisorEptTablePointer(pdptPage);
		}

		PHYSICAL_ADDRESS pdptPa = {};
		pdptPa.QuadPart = pml4[pml4Index] & 0x000FFFFFFFFFF000ULL;
		PULONG64 pdpt = (PULONG64)MmGetVirtualForPhysical(pdptPa);
		if (pdpt[pdptIndex] == 0) {
			PVOID pdPage = ArgusVisorAllocateEptTablePage(eptData);
			if (pdPage == nullptr) {
				ExFreePool(ranges);
				ArgusVisorTerminateEpt();
				return STATUS_INSUFFICIENT_RESOURCES;
			}
			pdpt[pdptIndex] = ArgusVisorEptTablePointer(pdPage);
		}

		PHYSICAL_ADDRESS pdPa = {};
		pdPa.QuadPart = pdpt[pdptIndex] & 0x000FFFFFFFFFF000ULL;
		PULONG64 pd = (PULONG64)MmGetVirtualForPhysical(pdPa);

		const ULONG64 memoryType = ArgusVisorIsRamPage2Mb(gpa, ranges)
			? kEptMemoryTypeWriteBack
			: kEptMemoryTypeUncacheable;

		pd[pdIndex] = kEptReadAccess | kEptWriteAccess | kEptExecuteAccess |
			(memoryType << 3) | kEptLargePage | (gpa & kEpt2MbAddressMask);
	}

	eptData->Pml4VirtualAddress = pml4;
	eptData->Pml4PhysicalAddress = MmGetPhysicalAddress(pml4);
	eptData->EptPointer = eptData->Pml4PhysicalAddress.QuadPart |
		kEptMemoryTypeWriteBack | kEptPageWalkLength4;

	ARGUSVISOR_LOG_INFO(
		"EPT identity map initialized: max_pa=0x%llX tables=%lu eptp=0x%llX\n",
		highestPhysicalAddress,
		eptData->TablePageCount,
		eptData->EptPointer);

	ExFreePool(ranges);
	return STATUS_SUCCESS;
}

NTSTATUS ArgusVisorPrepareEptWriteViolationTest() {
	if (GlobalEptTestPage != nullptr) {
		return STATUS_SUCCESS;
	}

	PHYSICAL_ADDRESS low = {};
	PHYSICAL_ADDRESS high = {};
	PHYSICAL_ADDRESS boundary = {};
	high.QuadPart = -1;

	GlobalEptTestPage = MmAllocateContiguousMemorySpecifyCache(
		PAGE_SIZE, low, high, boundary, MmCached);
	if (GlobalEptTestPage == nullptr) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(GlobalEptTestPage, PAGE_SIZE);
	GlobalEptTestPagePhysical = MmGetPhysicalAddress(GlobalEptTestPage);

	GlobalEptTestPte = ArgusVisorGetEptPteForGpa(
		GlobalEptTestPagePhysical.QuadPart, true);
	if (GlobalEptTestPte == nullptr) {
		MmFreeContiguousMemory(GlobalEptTestPage);
		GlobalEptTestPage = nullptr;
		GlobalEptTestPagePhysical.QuadPart = 0;
		return STATUS_UNSUCCESSFUL;
	}

	*GlobalEptTestPte &= ~kEptWriteAccess;
	InterlockedExchange(&GlobalEptTestViolationCount, 0);
	InterlockedExchange(&GlobalEptTestArmed, 1);

	ARGUSVISOR_LOG_INFO(
		"EPT write-violation test armed: va=%p gpa=0x%llX pte=%p pte_value=0x%llX\n",
		GlobalEptTestPage,
		GlobalEptTestPagePhysical.QuadPart,
		GlobalEptTestPte,
		*GlobalEptTestPte);

	return STATUS_SUCCESS;
}

void ArgusVisorTriggerEptWriteViolationTest() {
	if (InterlockedCompareExchange(&GlobalEptTestArmed, 2, 1) != 1) {
		return;
	}

	ARGUSVISOR_LOG_INFO("EPT write-violation test trigger: cpu=%lu va=%p gpa=0x%llX\n",
		KeGetCurrentProcessorNumber(),
		GlobalEptTestPage,
		GlobalEptTestPagePhysical.QuadPart);

	volatile UCHAR* testByte = (volatile UCHAR*)GlobalEptTestPage;
	*testByte = (UCHAR)(*testByte + 1);

	ARGUSVISOR_LOG_INFO("EPT write-violation test completed: value=%u violations=%ld\n",
		(ULONG)*testByte,
		GlobalEptTestViolationCount);
}


bool ArgusVisorIsEptSupported() {

	const ULONG64 capability = __readmsr(IA32_VMX_EPT_VPID_CAP);

	// walk4=1 EPT supports 4 levels of tables
	const bool pageWalkLength4 = (capability & (1ULL << 6)) != 0;
	// wb=1 The EPTP can be in WriteBack
	const bool writeBackType = (capability & (1ULL << 14)) != 0;
	// large2m=1 2MB EPT pages are supported
	const bool large2MbPages = (capability & (1ULL << 16)) != 0;

	ARGUSVISOR_LOG_INFO("EPT capability: raw=0x%llX walk4=%d wb=%d large2m=%d\n",
		capability,
		pageWalkLength4,
		writeBackType,
		large2MbPages);

	return pageWalkLength4 && writeBackType && large2MbPages;
}