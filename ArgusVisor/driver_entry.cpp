/*
* @author Simon Ngoy Mukendi
* @project ArgusVisor Engine
* @version 1.0.0
* /
// Use of this source code is governed by a MIT license that can be found in the LICENSE file.
*/
#include "ept.h"



extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);
	NTSTATUS status = STATUS_SUCCESS;

	DriverObject->DriverUnload = UnloadDriver;
	//ARGUSVISOR_LOG_INFO("=============================================\n");
	ARGUSVISOR_LOG_INFO("Loading ArgusVisor Engine v1.0.0...");

	if (!IsVMXSupported()) {
		ARGUSVISOR_LOG_ERROR("ERROR: VT-x does'nt support\n");
		return STATUS_NOT_SUPPORTED;
	}
	ARGUSVISOR_LOG_INFO("SUCCESS: VT-x is support !\n");

	/*
	if (isHypervisorAlreadyActive()) {
		ARGUSVISOR_LOG_ERROR("[ArgusVisor] ERROR: A hypervisor is already active. Disable Hyper-V/VBS or enable nested VT-x.\n");
		return STATUS_HV_FEATURE_UNAVAILABLE;
	}
	*/

	if (!IsVMXEnabledByUEFI()) {
		ARGUSVISOR_LOG_ERROR("ERROR: VT-x is disabled in UEFI\n");
		return STATUS_UNSUCCESSFUL;
	}

	if (!ArgusVisorIsEptSupported()) {
		ARGUSVISOR_LOG_ERROR("ERROR: EPT 2MB identity mapping is not supported by this CPU/VM\n");
		return STATUS_HV_FEATURE_UNAVAILABLE;
	}

	status = ArgusVisorInitializeEpt();
	if (!NT_SUCCESS(status)) {
		ARGUSVISOR_LOG_ERROR("ERROR: EPT initialization failed: 0x%08X\n", status);
		return status;
	}

	status = ArgusVisorPrepareEptWriteViolationTest();
	if (!NT_SUCCESS(status)) {
		ARGUSVISOR_LOG_ERROR("ERROR: EPT write-violation test setup failed: 0x%08X\n", status);
		ArgusVisorTerminateEpt();
		return status;
	}

	ULONG processorCount = KeQueryActiveProcessorCount(nullptr);
	GlobalVmxData = (PPER_CPU_VMX_DATA)ExAllocatePool2(
		POOL_FLAG_NON_PAGED,
		processorCount * sizeof(PER_CPU_VMX_DATA),
		'XmVB'
	);

	GlobalVmcsData = (PPER_CPU_VMCS_DATA)ExAllocatePool2(
		POOL_FLAG_NON_PAGED,
		processorCount * sizeof(PER_CPU_VMCS_DATA),
		'Vmcs'
	);

	if (GlobalVmxData == nullptr || GlobalVmcsData == nullptr) {
		ARGUSVISOR_LOG_WARN("WARNING: Allocated pool global structures impossible.\n");
		if (GlobalVmxData) ExFreePoolWithTag(GlobalVmxData, 'XmVB');
		if (GlobalVmcsData) ExFreePoolWithTag(GlobalVmcsData, 'Vmcs');
		ArgusVisorTerminateEpt();
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(GlobalVmxData, processorCount * sizeof(PER_CPU_VMX_DATA));
	RtlZeroMemory(GlobalVmcsData, processorCount * sizeof(PER_CPU_VMCS_DATA));

	status = VmxOrcherstator();

	if (!NT_SUCCESS(status)) {
		ARGUSVISOR_LOG_ERROR("ERROR: Critical failure during VMX initialization. Loading canceled... \n");
		if (GlobalVmxData != nullptr) {
			ExFreePoolWithTag(GlobalVmxData, 'XmVB');
			GlobalVmxData = nullptr;
		}
		if (GlobalVmcsData != nullptr) {
			ExFreePoolWithTag(GlobalVmcsData, 'Vmcs');
			GlobalVmcsData = nullptr;
		}
		ArgusVisorTerminateEpt();
		return status;
	}
	InterlockedExchange(&GlobalVmxActive, 1);
	ARGUSVISOR_LOG_INFO("SUCCESS: Hypervisor running in VMX Root\n");
	return status;
}

void UnloadDriver(PDRIVER_OBJECT DriverObject) {
	UNREFERENCED_PARAMETER(DriverObject);

	if (InterlockedCompareExchange(&GlobalVmxActive, 0, 0) != 0) {
		LONG errorCounter = 0;
		KeIpiGenericCall(VmxoffIpiWorker, (ULONG_PTR)&errorCounter);

		if (errorCounter != 0) {
			KeBugCheckEx(0x20002, errorCounter, GlobalVmxLastProcessor,
				GlobalVmxLastCheckpoint, 0);
		}

		InterlockedExchange(&GlobalVmxActive, 0);
	}

	ArgusVisorDumpVmExitSummary();
	ArgusVisorFreeAllocatedState();
	ARGUSVISOR_LOG_INFO("Unload Driver With Success!\n");
}

bool IsVMXSupported() {
	int cpuInfo[4];
	__cpuid(cpuInfo, 1);
	if ((cpuInfo[2] & (1 << 5)) == 0) return FALSE;
	return TRUE;
}


bool IsVMXEnabledByUEFI() {
	ULONG64 featureCtl = __readmsr(MS_IA32_FEATURE_CONTROL);
	if ((featureCtl & 0x5) == 0x5) return TRUE;
	return FALSE;
}

bool IsHypervisorAlreadyActive() {
	int cpuInfo[4] = {};
	__cpuid(cpuInfo, 1);
	return (cpuInfo[2] & (1 << 31)) != 0;
}



ULONG32 AjusterBitsControle(ULONG32 MsrIndex, ULONG32 valueWant) {
	unsigned __int64 vmxBasic = __readmsr(IA32_VMX_BASIC);
	bool useTrue = (vmxBasic & (1ULL << 55)) != 0;
	ULONG32 actualMsrIndex = MsrIndex;
	if (!useTrue) {
		switch (MsrIndex) {
		case IA32_VMX_TRUE_PINBASED_CTLS:  actualMsrIndex = 0x481; break;
		case IA32_VMX_TRUE_PROCBASED_CTLS: actualMsrIndex = 0x482; break;
		case IA32_VMX_TRUE_EXIT_CTLS:      actualMsrIndex = 0x483; break;
		case IA32_VMX_TRUE_ENTRY_CTLS:     actualMsrIndex = 0x484; break;
		}
	}
	LARGE_INTEGER msr;
	msr.QuadPart = __readmsr(actualMsrIndex);
	valueWant |= msr.LowPart;
	valueWant &= msr.HighPart;
	return valueWant;
}

static ULONG_PTR* ArgusVisorSelectGuestRegister(PGUEST_REGISTERS GuestRegisters, ULONG Index) {
	switch (Index) {
	case 0: return &GuestRegisters->Rax;
	case 1: return &GuestRegisters->Rcx;
	case 2: return &GuestRegisters->Rdx;
	case 3: return &GuestRegisters->Rbx;
	case 4: return &GuestRegisters->Rsp;
	case 5: return &GuestRegisters->Rbp;
	case 6: return &GuestRegisters->Rsi;
	case 7: return &GuestRegisters->Rdi;
	case 8: return &GuestRegisters->R8;
	case 9: return &GuestRegisters->R9;
	case 10: return &GuestRegisters->R10;
	case 11: return &GuestRegisters->R11;
	case 12: return &GuestRegisters->R12;
	case 13: return &GuestRegisters->R13;
	case 14: return &GuestRegisters->R14;
	case 15: return &GuestRegisters->R15;
	default: return nullptr;
	}
}

static bool ArgusVisorReadGuestRegister(PGUEST_REGISTERS GuestRegisters, ULONG Index, ULONG_PTR* Value) {
	if (Index == 4) {
		return __vmx_vmread(VMCS_GUEST_RSP, Value) == 0;
	}

	ULONG_PTR* reg = ArgusVisorSelectGuestRegister(GuestRegisters, Index);
	if (reg == nullptr) {
		return false;
	}

	*Value = *reg;
	return true;
}

static bool ArgusVisorWriteGuestRegister(PGUEST_REGISTERS GuestRegisters, ULONG Index, ULONG_PTR Value) {
	if (Index == 4) {
		return __vmx_vmwrite(VMCS_GUEST_RSP, Value) == 0;
	}

	ULONG_PTR* reg = ArgusVisorSelectGuestRegister(GuestRegisters, Index);
	if (reg == nullptr) {
		return false;
	}

	*reg = Value;
	return true;
}

static void ArgusVisorAdvanceGuestRip(ULONG_PTR GuestRip, ULONG_PTR ExitInstructionLength) {
	__vmx_vmwrite(VMCS_GUEST_RIP, GuestRip + ExitInstructionLength);
}

static ULONG_PTR ArgusVisorApplyFixedCr0Bits(ULONG_PTR Value) {
	Value |= __readmsr(IA32_VMX_CR0_FIXED0);
	Value &= __readmsr(IA32_VMX_CR0_FIXED1);
	return Value;
}

static ULONG_PTR ArgusVisorApplyFixedCr4Bits(ULONG_PTR Value) {
	Value |= __readmsr(IA32_VMX_CR4_FIXED0);
	Value &= __readmsr(IA32_VMX_CR4_FIXED1);
	return Value;
}

static void ArgusVisorHandleCrAccess(PGUEST_REGISTERS GuestRegisters, ULONG_PTR GuestRip, ULONG_PTR ExitInstructionLength) {
	ULONG_PTR qualification = 0;
	__vmx_vmread(VMCS_EXIT_QUALIFICATION, &qualification);

	const ULONG controlRegister = (ULONG)(qualification & 0x0F);
	const ULONG accessType = (ULONG)((qualification >> 4) & 0x03);
	const ULONG registerIndex = (ULONG)((qualification >> 8) & 0x0F);

	ULONG_PTR value = 0;
	switch (accessType) {
	case 0: // MOV to CR
		if (!ArgusVisorReadGuestRegister(GuestRegisters, registerIndex, &value)) {
			KeBugCheckEx(0xDEADDEAD, EXIT_REASON_CR_ACCESS, qualification, GuestRip, 1);
		}

		switch (controlRegister) {
		case 0:
			value = ArgusVisorApplyFixedCr0Bits(value);
			__vmx_vmwrite(VMCS_GUEST_CR0, value);
			__vmx_vmwrite(VMCS_CTRL_CR0_READ_SHADOW, value);
			break;
		case 3:
			__vmx_vmwrite(VMCS_GUEST_CR3, value & ~(1ULL << 63));
			break;
		case 4:
			value = ArgusVisorApplyFixedCr4Bits(value);
			__vmx_vmwrite(VMCS_GUEST_CR4, value);
			__vmx_vmwrite(VMCS_CTRL_CR4_READ_SHADOW, value);
			break;
		case 8:
			__writecr8(value);
			break;
		default:
			KeBugCheckEx(0xDEADDEAD, EXIT_REASON_CR_ACCESS, qualification, GuestRip, 2);
		}
		break;

	case 1: // MOV from CR
		switch (controlRegister) {
		case 0:
			__vmx_vmread(VMCS_GUEST_CR0, &value);
			break;
		case 3:
			__vmx_vmread(VMCS_GUEST_CR3, &value);
			break;
		case 4:
			__vmx_vmread(VMCS_GUEST_CR4, &value);
			break;
		case 8:
			value = __readcr8();
			break;
		default:
			KeBugCheckEx(0xDEADDEAD, EXIT_REASON_CR_ACCESS, qualification, GuestRip, 3);
		}

		if (!ArgusVisorWriteGuestRegister(GuestRegisters, registerIndex, value)) {
			KeBugCheckEx(0xDEADDEAD, EXIT_REASON_CR_ACCESS, qualification, GuestRip, 4);
		}
		break;

	case 2: // CLTS
		__vmx_vmread(VMCS_GUEST_CR0, &value);
		value &= ~(1ULL << 3);
		value = ArgusVisorApplyFixedCr0Bits(value);
		__vmx_vmwrite(VMCS_GUEST_CR0, value);
		__vmx_vmwrite(VMCS_CTRL_CR0_READ_SHADOW, value);
		break;

	case 3: { // LMSW
		ULONG_PTR lmsw = (qualification >> 16) & 0x0F;
		__vmx_vmread(VMCS_GUEST_CR0, &value);
		value = (value & ~0x0EULL) | (lmsw & 0x0EULL) | (value & 1);
		value = ArgusVisorApplyFixedCr0Bits(value);
		__vmx_vmwrite(VMCS_GUEST_CR0, value);
		__vmx_vmwrite(VMCS_CTRL_CR0_READ_SHADOW, value);
		break;
	}

	default:
		KeBugCheckEx(0xDEADDEAD, EXIT_REASON_CR_ACCESS, qualification, GuestRip, 5);
	}

	ArgusVisorAdvanceGuestRip(GuestRip, ExitInstructionLength);
}

static void ArgusVisorHandleMsrAccess(PGUEST_REGISTERS GuestRegisters, bool ReadAccess, ULONG_PTR GuestRip, ULONG_PTR ExitInstructionLength) {
	const ULONG msr = (ULONG)GuestRegisters->Rcx;

	__try {
		if (ReadAccess) {
			const ULONG64 value = __readmsr(msr);
			GuestRegisters->Rax = (ULONG)(value & 0xFFFFFFFF);
			GuestRegisters->Rdx = (ULONG)(value >> 32);
		}
		else {
			const ULONG64 value = ((ULONG64)(ULONG)GuestRegisters->Rdx << 32) | (ULONG)(GuestRegisters->Rax);
			__writemsr(msr, value);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		KeBugCheckEx(0xDEADDEAD, ReadAccess ? EXIT_REASON_RDMSR : EXIT_REASON_WRMSR, msr, GuestRip, GetExceptionCode());
	}

	ArgusVisorAdvanceGuestRip(GuestRip, ExitInstructionLength);
}

static void ArgusVisorHandleXsetbv(PGUEST_REGISTERS GuestRegisters, ULONG_PTR GuestRip, ULONG_PTR ExitInstructionLength) {
	const ULONG64 value = ((ULONG64)(ULONG)GuestRegisters->Rdx << 32) | (ULONG)(GuestRegisters->Rax);
	_xsetbv((ULONG)GuestRegisters->Rcx, value);
	ArgusVisorAdvanceGuestRip(GuestRip, ExitInstructionLength);
}

static const char* ArgusVisorGetVmExitName(USHORT Reason) {
	switch (Reason) {
	case EXIT_REASON_CPUID: return "CPUID";
	case EXIT_REASON_VMCALL: return "VMCALL";
	case EXIT_REASON_CR_ACCESS: return "CR_ACCESS";
	case EXIT_REASON_RDMSR: return "RDMSR";
	case EXIT_REASON_WRMSR: return "WRMSR";
	case EXIT_REASON_EPT_VIOLATION: return "EPT_VIOLATION";
	case EXIT_REASON_EPT_MISCONFIG: return "EPT_MISCONFIG";
	case EXIT_REASON_XSETBV: return "XSETBV";
	default: return "UNHANDLED";
	}
}

static const char* ArgusVisorGetMsrName(ULONG Msr) {
	switch (Msr) {
	case 0x40000000: return "HV_GUEST_OS_ID";
	case 0x40000001: return "HV_HYPERCALL";
	case 0x40000002: return "HV_VP_INDEX";
	case 0x40000003: return "HV_RESET";
	case 0x40000010: return "HV_VP_RUNTIME";
	case 0x40000020: return "HV_TIME_REF_COUNT";
	case 0x40000080: return "HV_SYNIC_CONTROL";
	case 0x40000081: return "HV_SIMP";
	case 0x40000082: return "HV_SIEFP";
	case 0x40000083: return "HV_SINT0_ALIAS";
	case 0x400000B0: return "HV_STIMER0_CONFIG";
	case 0x400000B1: return "HV_STIMER0_COUNT";
	case 0x400000B2: return "HV_STIMER1_CONFIG";
	case 0x400000B3: return "HV_STIMER1_COUNT";
	case 0x400000B4: return "HV_STIMER2_CONFIG";
	case 0x400000B5: return "HV_STIMER2_COUNT";
	case 0x400000B6: return "HV_STIMER3_CONFIG";
	case 0x400000B7: return "HV_STIMER3_COUNT";
	default: return "UNKNOWN_MSR";
	}
}

static bool ArgusVisorShouldLogVmExit(LONG64 Count) {
	return Count <= 8 || Count == 16 || Count == 32 || Count == 64 ||
		Count == 128 || Count == 256 || Count == 512 || Count == 1024 ||
		(Count % 10000) == 0;
}

static void ArgusVisorTraceVmExit(
	USHORT BasicExitReason,
	ULONG_PTR ExitReason,
	ULONG_PTR GuestRip,
	ULONG_PTR ExitInstructionLength,
	PGUEST_REGISTERS GuestRegisters) {

	LONG64 count = -1;
	if (BasicExitReason < RTL_NUMBER_OF(GlobalVmExitCounters)) {
		count = InterlockedIncrement64(
			reinterpret_cast<volatile LONG64*>(&GlobalVmExitCounters[BasicExitReason]));
	}

	if (count < 0 || !ArgusVisorShouldLogVmExit(count)) {
		return;
	}

	ULONG_PTR qualification = 0;
	__vmx_vmread(VMCS_EXIT_QUALIFICATION, &qualification);

	const bool isMsrAccess =
		BasicExitReason == EXIT_REASON_RDMSR ||
		BasicExitReason == EXIT_REASON_WRMSR;
	const ULONG msr = isMsrAccess ? (ULONG)GuestRegisters->Rcx : 0;

	ARGUSVISOR_LOG_INFO(
		"[VMEXIT] cpu=%lu reason=%hu/%s raw=0x%llX count=%lld rip=0x%llX len=%llu qual=0x%llX rax=0x%llX rcx=0x%llX msr=%s",
		KeGetCurrentProcessorNumber(),
		BasicExitReason,
		ArgusVisorGetVmExitName(BasicExitReason),
		ExitReason,
		count,
		GuestRip,
		ExitInstructionLength,
		qualification,
		GuestRegisters->Rax,
		GuestRegisters->Rcx,
		isMsrAccess ? ArgusVisorGetMsrName(msr) : "N/A");
}

void ArgusVisorDumpVmExitSummary() {
	ARGUSVISOR_LOG_INFO("[VMEXIT] Summary begin");

	for (USHORT reason = 0; reason < RTL_NUMBER_OF(GlobalVmExitCounters); reason++) {
		const LONG64 count = GlobalVmExitCounters[reason];
		if (count == 0) {
			continue;
		}

		ARGUSVISOR_LOG_INFO(
			"[VMEXIT] reason=%hu/%s total=%lld",
			reason,
			ArgusVisorGetVmExitName(reason),
			count);
	}

	ARGUSVISOR_LOG_INFO("[VMEXIT] Summary end");
}



void ArgusVisorFreeAllocatedState() {
	ULONG processorCount = KeQueryActiveProcessorCount(nullptr);

	ArgusVisorTerminateEpt();

	if (GlobalVmxData != nullptr) {
		for (ULONG i = 0; i < processorCount; i++) {
			if (GlobalVmxData[i].VmxonVirtualAddress != nullptr) {
				MmFreeContiguousMemory(GlobalVmxData[i].VmxonVirtualAddress);
				GlobalVmxData[i].VmxonVirtualAddress = nullptr;
			}
		}
	}

	if (GlobalVmcsData != nullptr) {
		for (ULONG i = 0; i < processorCount; i++) {
			if (GlobalVmcsData[i].VmcsVirtualAddress != nullptr) {
				MmFreeContiguousMemory(GlobalVmcsData[i].VmcsVirtualAddress);
				GlobalVmcsData[i].VmcsVirtualAddress = nullptr;
			}
			if (GlobalVmcsData[i].MsrBitmapVirtualAddress != nullptr) {
				MmFreeContiguousMemory(GlobalVmcsData[i].MsrBitmapVirtualAddress);
				GlobalVmcsData[i].MsrBitmapVirtualAddress = nullptr;
			}
			if (GlobalVmcsData[i].ArgusVisorStackVirtualAddress != nullptr) {
				ExFreePoolWithTag(GlobalVmcsData[i].ArgusVisorStackVirtualAddress, 'StHG');
				GlobalVmcsData[i].ArgusVisorStackVirtualAddress = nullptr;
			}
		}
	}

	if (GlobalVmxData != nullptr) {
		ExFreePoolWithTag(GlobalVmxData, 'XmVB');
		GlobalVmxData = nullptr;
	}

	if (GlobalVmcsData != nullptr) {
		ExFreePoolWithTag(GlobalVmcsData, 'Vmcs');
		GlobalVmcsData = nullptr;
	}
}


/*
* CENTRAL VM-EXITS MANAGER (VMX ROOT MODE)
*/
extern "C" bool ArgusVisorCExitHandler(PGUEST_REGISTERS GuestRegisters) {
	ULONG_PTR exitReason = 0;
	ULONG_PTR exitInstructionLength = 0;
	ULONG_PTR guestRip = 0;
	ULONG_PTR guestRflags = 0;
	ULONG_PTR guestRsp = 0;

	
	__vmx_vmread(VMCS_EXIT_REASON, &exitReason);
	__vmx_vmread(VMCS_VM_EXIT_INSTRUCTION_LENGTH, &exitInstructionLength);
	__vmx_vmread(VMCS_GUEST_RIP, &guestRip);
	__vmx_vmread(VMCS_GUEST_RFLAGS, &guestRflags);
	__vmx_vmread(VMCS_GUEST_RSP, &guestRsp);

	
	USHORT basicExitReason = (USHORT)(exitReason & 0xFFFF);
	ArgusVisorTraceVmExit(basicExitReason, exitReason, guestRip,
		exitInstructionLength, GuestRegisters);

	switch (basicExitReason) {

	case EXIT_REASON_CPUID: {
		// ----------------------------------------------------------------
		// CPUID instruction management (Stealth / Emulation)
		// ----------------------------------------------------------------
		// Windows or a process has just launched CPUID.
		// The input arguments are in GuestRegisters->Rax and Rcx.
		int cpuInfo[4] = { 0 };

		// We execute the actual CPUID of the physical processor to retrieve the true values
		__cpuidex(cpuInfo, (int)GuestRegisters->Rax, (int)GuestRegisters->Rcx);

		
		// The results are then fed back into the guest's records.
		GuestRegisters->Rax = cpuInfo[0];
		GuestRegisters->Rbx = cpuInfo[1];
		GuestRegisters->Rcx = cpuInfo[2];
		GuestRegisters->Rdx = cpuInfo[3];

		// ARCHITECTURAL RULE: Advance the guest RIP by the size of the CPUID instruction (2 bytes)
		// to proceed to the next Windows instruction.
		ArgusVisorAdvanceGuestRip(guestRip, exitInstructionLength);
		break;
	}

	case EXIT_REASON_CR_ACCESS: {
		// Interception if Windows attempts to modify CR0/CR4 outside of your shadow mask
		// For now, we're logging the event for security purposes.
		ArgusVisorHandleCrAccess(GuestRegisters, guestRip, exitInstructionLength);
		break;
	}

	case EXIT_REASON_VMCALL: {
		if (GuestRegisters->Rcx != ARGUSVISOR_HYPERCALL_TERMINATE) {
			KeBugCheckEx(0xDEADDEAD, exitReason, guestRip, exitInstructionLength, GuestRegisters->Rcx);
		}

		guestRflags &= ~(1ULL << 0);
		guestRflags &= ~(1ULL << 6);

		GuestRegisters->Rax = 1;
		GuestRegisters->Rcx = guestRip + exitInstructionLength;
		GuestRegisters->Rdx = guestRsp;
		GuestRegisters->R8 = guestRflags;
		return false;
	}

	case EXIT_REASON_RDMSR: {
		ArgusVisorHandleMsrAccess(GuestRegisters, true, guestRip, exitInstructionLength);
		break;
	}

	case EXIT_REASON_WRMSR: {
		ArgusVisorHandleMsrAccess(GuestRegisters, false, guestRip, exitInstructionLength);
		break;
	}

	case EXIT_REASON_EPT_VIOLATION: {
		ULONG_PTR faultGpa = 0;
		ULONG_PTR faultGla = 0;
		ULONG_PTR qualification = 0;
		__vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS, &faultGpa);
		__vmx_vmread(VMCS_GUEST_LINEAR_ADDRESS, &faultGla);
		__vmx_vmread(VMCS_EXIT_QUALIFICATION, &qualification);

		const ULONG64 testPageBase = GlobalEptTestPagePhysical.QuadPart & ~0xFFFULL;
		const ULONG64 faultPageBase = (ULONG64)faultGpa & ~0xFFFULL;
		if (GlobalEptTestPte != nullptr && faultPageBase == testPageBase) {
			*GlobalEptTestPte |= kEptWriteAccess;
			InterlockedIncrement(&GlobalEptTestViolationCount);
			InterlockedExchange(&GlobalEptTestArmed, 3);

			const ULONG_PTR inveptOk = AsmxInveptGlobal();
			ARGUSVISOR_LOG_INFO(
				"EPT test violation handled: rip=0x%llX gla=0x%llX gpa=0x%llX qual=0x%llX pte=0x%llX invept=%llu\n",
				guestRip,
				faultGla,
				faultGpa,
				qualification,
				(ULONG64)*GlobalEptTestPte,
				(ULONG64)inveptOk);

			return true;
		}

		ARGUSVISOR_LOG_ERROR(
			"EPT violation: rip=0x%llX gla=0x%llX gpa=0x%llX qual=0x%llX\n",
			guestRip,
			faultGla,
			faultGpa,
			qualification);
		KeBugCheckEx(0xE7000001, faultGpa, faultGla, qualification, guestRip);
		break;
	}

	case EXIT_REASON_EPT_MISCONFIG: {
		ULONG_PTR faultGpa = 0;
		__vmx_vmread(VMCS_GUEST_PHYSICAL_ADDRESS, &faultGpa);
		ARGUSVISOR_LOG_ERROR("EPT misconfig: rip=0x%llX gpa=0x%llX\n",
			guestRip,
			faultGpa);
		KeBugCheckEx(0xE7000002, faultGpa, guestRip, 0, 0);
		break;
	}

	case EXIT_REASON_XSETBV: {
		ArgusVisorHandleXsetbv(GuestRegisters, guestRip, exitInstructionLength);
		break;
	}

	default: {
		// ----------------------------------------------------------------
		// Security: Unhandled reason - BugCheck (BSOD checked)
		// ----------------------------------------------------------------
		// If we don't know how to emulate the instruction that caused the output,
		// Doing nothing would freeze the thread. We trigger a clean BSOD to analyze it.
		//DbgPrint("[ArgusVisor] CRITICAL: Unhandled VM-Exit! Reason: %d, RIP: 0x%llX\n", basicExitReason, guestRip);

		// KeBugCheck is structured for Kernel-mode diagnostics.
		KeBugCheckEx(0xDEADDEAD, exitReason, guestRip, exitInstructionLength, 0);
		break;
	}
	}

	// As soon as this function finishes, the stream returns to ArgusVisorAsm.asm
	// which will execute the hardware instruction 'vmresume'.
	return true;
}

extern "C" void ArgusVisorVmResumeFailureHandler(void) {
	ULONG_PTR vmInstructionError = 0;
	ULONG_PTR exitReason = 0;
	ULONG_PTR guestRip = 0;

	__vmx_vmread(VMCS_VM_INSTRUCTION_ERROR, &vmInstructionError);
	__vmx_vmread(VMCS_EXIT_REASON, &exitReason);
	__vmx_vmread(VMCS_GUEST_RIP, &guestRip);

	KeBugCheckEx(0x20001, vmInstructionError, exitReason, guestRip, 0);
}

ULONG_PTR VmxoffIpiWorker(ULONG_PTR Argument) {
	PLONG errorCounter = (PLONG)Argument;

	__try {
		if (AsmxVmcall(ARGUSVISOR_HYPERCALL_TERMINATE) == 0) {
			InterlockedIncrement(errorCounter);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		InterlockedIncrement(errorCounter);
	}

	return 0;
}

ULONG_PTR VmxonIpiWorker(ULONG_PTR Argument) {
	PLONG errorCounter = (PLONG)Argument;
	ULONG processorIndex = KeGetCurrentProcessorNumber();
	
	if (GlobalVmxData == nullptr || GlobalVmcsData == nullptr || GlobalEptData == nullptr) {
		ARGUSVISOR_LOG_ERROR("ERROR: Global structures (Vmx/Vmcs/EptData) are NULL on CPU %lu!\n", processorIndex);
		InterlockedIncrement(errorCounter);
		return 0;
	}
	
	PPER_CPU_VMX_DATA cpuData = &GlobalVmxData[processorIndex];
	PPER_CPU_VMCS_DATA cpuVmcs = &GlobalVmcsData[processorIndex];

#define VMX_CHECKPOINT(Value) \
    do { \
        GlobalVmxLastProcessor = (LONG)processorIndex; \
        GlobalVmxLastCheckpoint = (Value); \
        _ReadWriteBarrier(); \
    } while (0)

	__try {
		unsigned __int64 cr0 = __readcr0();
		unsigned __int64 cr4 = __readcr4();

		VMX_CHECKPOINT(10);
		cr0 |= __readmsr(IA32_VMX_CR0_FIXED0);
		cr0 &= __readmsr(IA32_VMX_CR0_FIXED1);
		__writecr0(cr0);

		VMX_CHECKPOINT(20);
		cr4 |= __readmsr(IA32_VMX_CR4_FIXED0);
		cr4 &= __readmsr(IA32_VMX_CR4_FIXED1);
		cr4 |= (1ULL << 13);
		__writecr4(cr4);

		if (cpuData->VmxonVirtualAddress == nullptr || cpuVmcs->VmcsVirtualAddress == nullptr) {
			InterlockedIncrement(errorCounter);
			return 0;
		}

		VMX_CHECKPOINT(30);
		if (__vmx_on((unsigned __int64*)&cpuData->VmxonPhysicalAddress.QuadPart) != 0) {
			InterlockedIncrement(errorCounter);
			return 0;
		}

		VMX_CHECKPOINT(40);
		if (__vmx_vmclear((unsigned __int64*)&cpuVmcs->VmcsPhysicalAddress.QuadPart) != 0) {
			InterlockedIncrement(errorCounter);
			__vmx_off();
			return 0;
		}

		VMX_CHECKPOINT(50);
		if (__vmx_vmptrld((unsigned __int64*)&cpuVmcs->VmcsPhysicalAddress.QuadPart) != 0) {
			InterlockedIncrement(errorCounter);
			__vmx_off();
			return 0;
		}

		// Pin-Based VM Execution Controls
		ULONG32 pinBased = AjusterBitsControle(IA32_VMX_TRUE_PINBASED_CTLS, 0);
		__vmx_vmwrite(VMCS_CTRL_PIN_BASED_VM_EXEC_CONTROLS, pinBased);

		// Processor-Based VM-Execution Controls
		// We activate bit 28 (MSR Bitmaps) AND bit 31 (Secondary Controls required for RDTSCP)
		ULONG32 procBased = AjusterBitsControle(IA32_VMX_TRUE_PROCBASED_CTLS, (1U << 28) | (1U << 31));
		__vmx_vmwrite(VMCS_CTRL_PROCESSOR_BASED_VM_EXEC_CONTROLS, procBased);

		// Secondary Processor-Based Controls (Added for RDTSCP support)
		ULONG32 secondaryProcBased = AjusterBitsControle(IA32_VMX_PROCBASED_CTLS2, (1 << 1) | (1 << 3));
		__vmx_vmwrite(VMCS_CTRL_SECONDARY_PROCESSOR_BASED_VM_EXEC_CONTROLS, secondaryProcBased);

		// VM-Exit Controls
		ULONG32 vmExit = AjusterBitsControle(IA32_VMX_TRUE_EXIT_CTLS, (1 << 9));
		__vmx_vmwrite(VMCS_CTRL_VM_EXIT_CONTROLS, vmExit);

		// VM-Entry Controls
		ULONG32 vmEntry = AjusterBitsControle(IA32_VMX_TRUE_ENTRY_CTLS, (1 << 9));
		__vmx_vmwrite(VMCS_CTRL_VM_ENTRY_CONTROLS, vmEntry);

		// Cleaning up optional control fields
		__vmx_vmwrite(VMCS_CTRL_EXCEPTION_BITMAP, 0);
		__vmx_vmwrite(VMCS_CTRL_PAGE_FAULT_ERROR_CODE_MASK, 0);
		__vmx_vmwrite(VMCS_CTRL_PAGE_FAULT_ERROR_CODE_MATCH, 0);
		__vmx_vmwrite(VMCS_CTRL_CR3_TARGET_COUNT, 0);
		__vmx_vmwrite(VMCS_CTRL_VM_EXIT_MSR_STORE_COUNT, 0);
		__vmx_vmwrite(VMCS_CTRL_VM_EXIT_MSR_LOAD_COUNT, 0);
		__vmx_vmwrite(VMCS_CTRL_VM_ENTRY_MSR_LOAD_COUNT, 0);
		__vmx_vmwrite(VMCS_CTRL_VM_ENTRY_INTR_INFO_FIELD, 0);
		__vmx_vmwrite(VMCS_CTRL_VM_ENTRY_EXCEPTION_ERROR_CODE, 0);
		__vmx_vmwrite(VMCS_CTRL_VM_ENTRY_INSTRUCTION_LENGTH, 0);

		
		// - HOST-STATE AREA CONFIGURATION
		__vmx_vmwrite(VMCS_HOST_CR0, __readcr0());
		__vmx_vmwrite(VMCS_HOST_CR3, __readcr3());
		__vmx_vmwrite(VMCS_HOST_CR4, __readcr4());

		__vmx_vmwrite(VMCS_HOST_CS_SELECTOR, AsmReadCS() & 0xF8);
		__vmx_vmwrite(VMCS_HOST_DS_SELECTOR, AsmReadDS() & 0xF8);
		__vmx_vmwrite(VMCS_HOST_ES_SELECTOR, AsmReadES() & 0xF8);
		__vmx_vmwrite(VMCS_HOST_FS_SELECTOR, AsmReadFS() & 0xF8);
		__vmx_vmwrite(VMCS_HOST_GS_SELECTOR, AsmReadGS() & 0xF8);
		__vmx_vmwrite(VMCS_HOST_SS_SELECTOR, AsmReadSS() & 0xF8);

		__vmx_vmwrite(VMCS_HOST_FS_BASE, __readmsr(0xC0000100));
		__vmx_vmwrite(VMCS_HOST_GS_BASE, __readmsr(0xC0000101));
		__vmx_vmwrite(VMCS_HOST_SYSENTER_CS, __readmsr(IA32_SYSENTER_CS));
		__vmx_vmwrite(VMCS_HOST_SYSENTER_ESP, __readmsr(IA32_SYSENTER_ESP));
		__vmx_vmwrite(VMCS_HOST_SYSENTER_EIP, __readmsr(IA32_SYSENTER_EIP));

		DESCRIPTOR_REGISTER hostGdtr, hostIdtr;
		AsmReadGdtr(&hostGdtr);
		AsmReadIdtr(&hostIdtr);
		__vmx_vmwrite(VMCS_HOST_GDTR_BASE, hostGdtr.Base);
		__vmx_vmwrite(VMCS_HOST_IDTR_BASE, hostIdtr.Base);

		USHORT trSelector = AsmReadTR();
		PUCHAR gdtBase = (PUCHAR)hostGdtr.Base;
		PULONG_PTR tssDescriptor = (PULONG_PTR)(gdtBase + (trSelector & 0xFFF8));

		ULONG_PTR baseLow = (tssDescriptor[0] >> 16) & 0xFFFF;
		ULONG_PTR baseMid = (tssDescriptor[0] >> 32) & 0xFF;
		ULONG_PTR baseHigh = (tssDescriptor[0] >> 56) & 0xFF;
		ULONG_PTR baseUpper = tssDescriptor[1];
		ULONG_PTR trBase = baseLow | (baseMid << 16) | (baseHigh << 24) | (baseUpper << 32);

		__vmx_vmwrite(VMCS_HOST_TR_SELECTOR, trSelector & 0xF8);
		__vmx_vmwrite(VMCS_HOST_TR_BASE, trBase);

		ULONG_PTR hostStackTop = (ULONG_PTR)cpuVmcs->ArgusVisorStackVirtualAddress + 0x4000;
		hostStackTop &= ~(ULONG_PTR)0xF;

		__vmx_vmwrite(VMCS_HOST_RSP, hostStackTop);
		__vmx_vmwrite(VMCS_HOST_RIP, (ULONG_PTR)ArgusVisorVmExitHandler);

		
		// - GUEST-STATE AREA CONFIGURATION
		__vmx_vmwrite(VMCS_GUEST_CR0, __readcr0());
		__vmx_vmwrite(VMCS_GUEST_CR3, __readcr3());
		__vmx_vmwrite(VMCS_GUEST_CR4, __readcr4());
		__vmx_vmwrite(VMCS_GUEST_DR7, __readdr(7));

		ULONG_PTR guestRflags = __readeflags();
		guestRflags |= (1 << 9);
		__vmx_vmwrite(VMCS_GUEST_RFLAGS, guestRflags);

		__vmx_vmwrite(VMCS_GUEST_IA32_DEBUGCTL, __readmsr(IA32_DEBUGCTL));
		__vmx_vmwrite(VMCS_GUEST_PENDING_DBG_EXCEPTIONS, 0);

		__vmx_vmwrite(VMCS_GUEST_GDTR_BASE, hostGdtr.Base);
		__vmx_vmwrite(VMCS_GUEST_GDTR_LIMIT, hostGdtr.Limit);
		__vmx_vmwrite(VMCS_GUEST_IDTR_BASE, hostIdtr.Base);
		__vmx_vmwrite(VMCS_GUEST_IDTR_LIMIT, hostIdtr.Limit);

		__vmx_vmwrite(VMCS_GUEST_CS_SELECTOR, AsmReadCS());
		__vmx_vmwrite(VMCS_GUEST_DS_SELECTOR, AsmReadDS());
		__vmx_vmwrite(VMCS_GUEST_ES_SELECTOR, AsmReadES());
		__vmx_vmwrite(VMCS_GUEST_FS_SELECTOR, AsmReadFS());
		__vmx_vmwrite(VMCS_GUEST_GS_SELECTOR, AsmReadGS());
		__vmx_vmwrite(VMCS_GUEST_SS_SELECTOR, AsmReadSS());
		__vmx_vmwrite(VMCS_GUEST_TR_SELECTOR, AsmReadTR());
		__vmx_vmwrite(VMCS_GUEST_LDTR_SELECTOR, 0);

		__vmx_vmwrite(VMCS_GUEST_CS_LIMIT, __segmentlimit(AsmReadCS()));
		__vmx_vmwrite(VMCS_GUEST_DS_LIMIT, __segmentlimit(AsmReadDS()));
		__vmx_vmwrite(VMCS_GUEST_ES_LIMIT, __segmentlimit(AsmReadES()));
		__vmx_vmwrite(VMCS_GUEST_FS_LIMIT, __segmentlimit(AsmReadFS()));
		__vmx_vmwrite(VMCS_GUEST_GS_LIMIT, __segmentlimit(AsmReadGS()));
		__vmx_vmwrite(VMCS_GUEST_SS_LIMIT, __segmentlimit(AsmReadSS()));
		__vmx_vmwrite(VMCS_GUEST_TR_LIMIT, __segmentlimit(AsmReadTR()));
		__vmx_vmwrite(VMCS_GUEST_LDTR_LIMIT, 0);

		__vmx_vmwrite(VMCS_GUEST_CS_ACCESS_RIGHTS, AsmReadSegmentAccessRights(AsmReadCS()));
		__vmx_vmwrite(VMCS_GUEST_DS_ACCESS_RIGHTS, AsmReadSegmentAccessRights(AsmReadDS()));
		__vmx_vmwrite(VMCS_GUEST_ES_ACCESS_RIGHTS, AsmReadSegmentAccessRights(AsmReadES()));
		__vmx_vmwrite(VMCS_GUEST_FS_ACCESS_RIGHTS, AsmReadSegmentAccessRights(AsmReadFS()));
		__vmx_vmwrite(VMCS_GUEST_GS_ACCESS_RIGHTS, AsmReadSegmentAccessRights(AsmReadGS()));
		__vmx_vmwrite(VMCS_GUEST_SS_ACCESS_RIGHTS, AsmReadSegmentAccessRights(AsmReadSS()));
		__vmx_vmwrite(VMCS_GUEST_TR_ACCESS_RIGHTS, AsmReadSegmentAccessRights(AsmReadTR()));
		__vmx_vmwrite(VMCS_GUEST_LDTR_ACCESS_RIGHTS, 0x00010000);

		__vmx_vmwrite(VMCS_GUEST_CS_BASE, 0);
		__vmx_vmwrite(VMCS_GUEST_DS_BASE, 0);
		__vmx_vmwrite(VMCS_GUEST_ES_BASE, 0);
		__vmx_vmwrite(VMCS_GUEST_SS_BASE, 0);
		__vmx_vmwrite(VMCS_GUEST_FS_BASE, __readmsr(0xC0000100));
		__vmx_vmwrite(VMCS_GUEST_GS_BASE, __readmsr(0xC0000101));
		__vmx_vmwrite(VMCS_GUEST_LDTR_BASE, 0);
		__vmx_vmwrite(VMCS_GUEST_TR_BASE, trBase);
		__vmx_vmwrite(VMCS_GUEST_INTERRUPTIBILITY_STATE, 0);
		__vmx_vmwrite(VMCS_GUEST_ACTIVITY_STATE, 0);
		__vmx_vmwrite(VMCS_GUEST_SYSENTER_CS, __readmsr(IA32_SYSENTER_CS));
		__vmx_vmwrite(VMCS_GUEST_SYSENTER_ESP, __readmsr(IA32_SYSENTER_ESP));
		__vmx_vmwrite(VMCS_GUEST_SYSENTER_EIP, __readmsr(IA32_SYSENTER_EIP));

		__vmx_vmwrite(VMCS_GUEST_RIP, (ULONG_PTR)AsmGuestContextRip);
		__vmx_vmwrite(VMCS_GUEST_RSP, AsmReadRsp());

		// VMCS Link Pointer unique required at -1
		__vmx_vmwrite(0x00002800, 0xFFFFFFFFFFFFFFFF);

		// Shadows disabled for initial stabilization
		__vmx_vmwrite(VMCS_CTRL_CR0_GUEST_HOST_MASK, 0);
		__vmx_vmwrite(VMCS_CTRL_CR0_READ_SHADOW, __readcr0());
		__vmx_vmwrite(VMCS_CTRL_CR4_GUEST_HOST_MASK, 0);
		__vmx_vmwrite(VMCS_CTRL_CR4_READ_SHADOW, __readcr4());

		// MSR Bitmap Address Binding
		__vmx_vmwrite(VMCS_CTRL_MSR_BITMAP_ADDRESS, cpuVmcs->MsrBitmapPhysicalAddress.QuadPart);
		__vmx_vmwrite(VMCS_CTRL_EPT_POINTER, GlobalEptData->EptPointer);

		ULONG_PTR vmcsSecondaryControls = 0;
		ULONG_PTR vmcsEptPointer = 0;
		__vmx_vmread(VMCS_CTRL_SECONDARY_PROCESSOR_BASED_VM_EXEC_CONTROLS, &vmcsSecondaryControls);
		__vmx_vmread(VMCS_CTRL_EPT_POINTER, &vmcsEptPointer);

		ARGUSVISOR_LOG_INFO(
			"EPT VMCS armed: cpu=%lu secondary=0x%llX enable_ept=%llu eptp=0x%llX expected=0x%llX pml4_pa=0x%llX\n",
			processorIndex,
			(ULONG64)vmcsSecondaryControls,
			((ULONG64)vmcsSecondaryControls >> 1) & 1,
			(ULONG64)vmcsEptPointer,
			GlobalEptData->EptPointer,
			GlobalEptData->Pml4PhysicalAddress.QuadPart);

		// Effective launch of the machine
		VMX_CHECKPOINT(60);
		if (AsmxVmlaunch() == 0) {
			ULONG_PTR errorInstruction = 0;
			__vmx_vmread(0x00004400, &errorInstruction);
			KeBugCheckEx(0x0000E1A0, errorInstruction, 0, 0, 0);
		}

		ArgusVisorTriggerEptWriteViolationTest();

		return 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {

		ARGUSVISOR_LOG_ERROR("ERROR: VMX worker exception on core %lu. Exception code: 0x%08X. Checkpoint=%ld\n",
			processorIndex,
			GetExceptionCode(),
			GlobalVmxLastCheckpoint);
		InterlockedIncrement(errorCounter);
		return 0;
	}
#undef VMX_CHECKPOINT
}
NTSTATUS  VmxOrcherstator() {
	LONG errorCounter = 0;
	ULONG processorCount = KeQueryActiveProcessorCount(nullptr);

	ARGUSVISOR_LOG_INFO("INFO: There are %d cores.\n", processorCount);

	for (ULONG i = 0; i < processorCount; i++) {
		PHYSICAL_ADDRESS low; low.QuadPart = 0;
		PHYSICAL_ADDRESS high; high.QuadPart = -1;
		PHYSICAL_ADDRESS boundary; boundary.QuadPart = 0;

		// Reading of high physical address constraints in MSR_BASIC
		unsigned __int64 vmxBasic = __readmsr(IA32_VMX_BASIC);

		if ((vmxBasic & (1ULL << 48)) != 0) {
			high.QuadPart = 0xFFFFFFFF; // Strict limit below 4 GB
		}

		GlobalVmxData[i].VmxonVirtualAddress = MmAllocateContiguousMemorySpecifyCache(
			PAGE_SIZE, low, high, boundary, MmCached
		);
		GlobalVmcsData[i].VmcsVirtualAddress = MmAllocateContiguousMemorySpecifyCache(
			PAGE_SIZE, low, high, boundary, MmCached
		);

		GlobalVmcsData[i].MsrBitmapVirtualAddress = MmAllocateContiguousMemorySpecifyCache(
			PAGE_SIZE, low, high, boundary, MmCached
		);

		GlobalVmcsData[i].ArgusVisorStackVirtualAddress = ExAllocatePool2(
			POOL_FLAG_NON_PAGED,
			0x4000,
			'StHG'
		);

		if ((GlobalVmxData[i].VmxonVirtualAddress == nullptr) ||
			(GlobalVmcsData[i].VmcsVirtualAddress == nullptr) ||
			(GlobalVmcsData[i].MsrBitmapVirtualAddress == nullptr) ||
			(GlobalVmcsData[i].ArgusVisorStackVirtualAddress == nullptr)) {
			//DbgPrint("[ArgusVisor] ERROR: memory allocation on %d core failed!\n", i);

			for (ULONG j = 0; j <= i; j++) {
				if (GlobalVmxData[j].VmxonVirtualAddress != nullptr) {
					MmFreeContiguousMemory(GlobalVmxData[j].VmxonVirtualAddress);
					GlobalVmxData[j].VmxonVirtualAddress = nullptr;
				}
				if (GlobalVmcsData[j].VmcsVirtualAddress != nullptr) {
					MmFreeContiguousMemory(GlobalVmcsData[j].VmcsVirtualAddress);
					GlobalVmcsData[j].VmcsVirtualAddress = nullptr;
				}
				if (GlobalVmcsData[j].MsrBitmapVirtualAddress != nullptr) {
					MmFreeContiguousMemory(GlobalVmcsData[j].MsrBitmapVirtualAddress);
					GlobalVmcsData[j].MsrBitmapVirtualAddress = nullptr;
				}
				if (GlobalVmcsData[j].ArgusVisorStackVirtualAddress != nullptr) {
					ExFreePoolWithTag(GlobalVmcsData[j].ArgusVisorStackVirtualAddress, 'StHG');
					GlobalVmcsData[j].ArgusVisorStackVirtualAddress = nullptr;
				}
			}
			return STATUS_INSUFFICIENT_RESOURCES;
		}

		RtlZeroMemory(GlobalVmxData[i].VmxonVirtualAddress, PAGE_SIZE);
		GlobalVmxData[i].VmxonPhysicalAddress = MmGetPhysicalAddress(GlobalVmxData[i].VmxonVirtualAddress);

		RtlZeroMemory(GlobalVmcsData[i].VmcsVirtualAddress, PAGE_SIZE);
		GlobalVmcsData[i].VmcsPhysicalAddress = MmGetPhysicalAddress(GlobalVmcsData[i].VmcsVirtualAddress);

		RtlZeroMemory(GlobalVmcsData[i].MsrBitmapVirtualAddress, PAGE_SIZE);
		GlobalVmcsData[i].MsrBitmapPhysicalAddress = MmGetPhysicalAddress(GlobalVmcsData[i].MsrBitmapVirtualAddress);

		RtlZeroMemory(GlobalVmcsData[i].ArgusVisorStackVirtualAddress, 0x4000);

		// Write the Revision ID with mandatory mask bit 31
		unsigned int revisionId = (unsigned int)vmxBasic & 0x7FFFFFFF;
		*(unsigned int*)GlobalVmxData[i].VmxonVirtualAddress = revisionId;
		*(unsigned int*)GlobalVmcsData[i].VmcsVirtualAddress = revisionId;

	}

	//DbgPrint("[ArgusVisor] Sending material interrupt at all cores!\n");

	KeIpiGenericCall(VmxonIpiWorker, (ULONG_PTR)&errorCounter);

	if (errorCounter > 0) {
		//DbgPrint("[ArgusVisor] ERROR: %d core(s) could not activate VMX Root mode.\n", errorCounter);

		for (ULONG i = 0; i < processorCount; i++) {
			if (GlobalVmxData[i].VmxonVirtualAddress != nullptr) {
				MmFreeContiguousMemory(GlobalVmxData[i].VmxonVirtualAddress);
				GlobalVmxData[i].VmxonVirtualAddress = nullptr;
			}
			if (GlobalVmcsData[i].VmcsVirtualAddress != nullptr) {
				MmFreeContiguousMemory(GlobalVmcsData[i].VmcsVirtualAddress);
				GlobalVmcsData[i].VmcsVirtualAddress = nullptr;
			}
			if (GlobalVmcsData[i].MsrBitmapVirtualAddress != nullptr) {
				MmFreeContiguousMemory(GlobalVmcsData[i].MsrBitmapVirtualAddress);
				GlobalVmcsData[i].MsrBitmapVirtualAddress = nullptr;
			}
			if (GlobalVmcsData[i].ArgusVisorStackVirtualAddress != nullptr) {
				ExFreePoolWithTag(GlobalVmcsData[i].ArgusVisorStackVirtualAddress, 'StHG');
				GlobalVmcsData[i].ArgusVisorStackVirtualAddress = nullptr;
			}
		}

		return STATUS_UNSUCCESSFUL;
	}


	return STATUS_SUCCESS;
}
