#include <ntddk.h>
#include <intrin.h>
#include <wdm.h>
#include "vmcs_encoder.h"
#include "log.h"



/*
* This structure will save the addresses
* Virtual and Physical VMX Mode
*/
typedef struct _PER_CPU_VMX_DATA {
	PVOID            VmxonVirtualAddress;
	PHYSICAL_ADDRESS VmxonPhysicalAddress;
}PER_CPU_VMX_DATA, * PPER_CPU_VMX_DATA;

/*
* This structure will back up the data from the VMCS structure.
* for each logic processor
*/
typedef struct _PER_CPU_VMCS_DATA {
	PVOID VmcsVirtualAddress;
	PHYSICAL_ADDRESS VmcsPhysicalAddress;
	PVOID MsrBitmapVirtualAddress;
	PHYSICAL_ADDRESS MsrBitmapPhysicalAddress;
	PVOID ArgusVisorStackVirtualAddress;
}PER_CPU_VMCS_DATA, * PPER_CPU_VMCS_DATA;

/*
* Definition of the EPT structure
*/
typedef struct _EPT_DATA {
	PVOID* TablePages; // All pages allocated for EPT tables
	ULONG TablePageCount;
	ULONG TablePageCapacity;
	PVOID Pml4VirtualAddress; // Virtual address of PML4 EPT
	PHYSICAL_ADDRESS Pml4PhysicalAddress; //  Physical address of PML4 EPT
	ULONG64 EptPointer; // Final value written in the VMCS
} EPT_DATA, * PEPT_DATA;

#pragma pack(push, 1)
typedef struct _DESCRIPTOR_REGISTER {
	USHORT Limit;      // 2 bytes: The size of the table minus 1
	ULONG_PTR Base;    // 8 bytes: The linear virtual address of the table in memory
} DESCRIPTOR_REGISTER, * PDESCRIPTOR_REGISTER;
#pragma pack(pop)

/*
* OS context backup structure (Guest)
*/
typedef struct _GUEST_REGISTERS {
	ULONG_PTR Rax;
	ULONG_PTR Rcx;
	ULONG_PTR Rdx;
	ULONG_PTR Rbx;
	ULONG_PTR Rsp;
	ULONG_PTR Rbp;
	ULONG_PTR Rsi;
	ULONG_PTR Rdi;
	ULONG_PTR R8;
	ULONG_PTR R9;
	ULONG_PTR R10;
	ULONG_PTR R11;
	ULONG_PTR R12;
	ULONG_PTR R13;
	ULONG_PTR R14;
	ULONG_PTR R15;
	ULONG_PTR Rflags;
} GUEST_REGISTERS, * PGUEST_REGISTERS;


inline PPER_CPU_VMX_DATA  GlobalVmxData = nullptr;
inline PPER_CPU_VMCS_DATA GlobalVmcsData = nullptr;
inline PEPT_DATA          GlobalEptData = nullptr;



static LONG GlobalVmxLastCheckpoint = 0;
static LONG GlobalVmxLastProcessor = -1;
static LONG GlobalVmxActive = 0;
static LONG64 GlobalVmExitCounters[64] = {};




inline PVOID             GlobalEptTestPage = nullptr;
inline PHYSICAL_ADDRESS  GlobalEptTestPagePhysical = {};
inline ULONG64*          GlobalEptTestPte = nullptr;
inline LONG              GlobalEptTestArmed = 0;
inline LONG              GlobalEptTestViolationCount = 0;



extern "C" {
	// Guest secure execution point at startup (VMLAUNCH)
	void AsmGuestContextRip(void);

	// System table extraction routines (GDT / IDT)
	void AsmReadGdtr(PVOID GdtrStructure);
	void AsmReadIdtr(PVOID IdtrStructure);

	// Host-State Central Anchor Point (RIP)
	void ArgusVisorVmExitHandler(void);
	void ArgusVisorVmResumeFailureHandler(void);


	ULONG_PTR AsmReadRsp(void);
	USHORT    AsmReadCS(void);
	USHORT    AsmReadDS(void);
	USHORT    AsmReadES(void);
	USHORT    AsmReadFS(void);
	USHORT    AsmReadGS(void);
	USHORT    AsmReadSS(void);
	USHORT    AsmReadTR(void);


	ULONG_PTR AsmReadSegmentAccessRights(USHORT Selector);

	ULONG_PTR AsmxVmlaunch(void);
	ULONG_PTR AsmxVmcall(ULONG_PTR HypercallNumber);
	ULONG_PTR AsmxInveptGlobal(void);
}

bool IsVMXSupported();
bool IsVMXEnabledByUEFI();
bool IsHypervisorAlreadyActive();
bool ArgusVisorIsEptSupported();

//static bool ArgusVisorVmWrite(ULONG_PTR Field, ULONG_PTR Value)

ULONG_PTR VmxonIpiWorker(ULONG_PTR Argument);
ULONG_PTR VmxoffIpiWorker(ULONG_PTR Argument);
NTSTATUS  VmxOrcherstator();

ULONG32 AjusterBitsControle(ULONG32 MsrIndex, ULONG32 valueWant);

void UnloadDriver(PDRIVER_OBJECT DriverObject);






