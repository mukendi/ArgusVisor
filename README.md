# ArgusVisor

ArgusVisor is a research-oriented mini security hypervisor for Windows x64.

The goal of this project is educational: build a small Intel VT-x based hypervisor step by step, understand the mechanics behind VMX, VMCS, VM-exits, VMRESUME, VMCALL-based unload, and EPT-based memory control.

This is not a production security product. It is a kernel research project intended to make low-level virtualization concepts easier to understand.

## Project Goals

ArgusVisor demonstrates how to:

- verify Intel VT-x support with `CPUID`;
- verify VMX enablement through `IA32_FEATURE_CONTROL`;
- enter VMX root operation with `VMXON`;
- configure one VMCS per logical processor;
- launch the running Windows kernel into VMX non-root mode;
- handle VM-exits through an assembly entry point and a C++ dispatcher;
- emulate selected instructions such as `CPUID`, `RDMSR`, `WRMSR`, `XSETBV`, and `VMCALL`;
- unload the hypervisor cleanly using a controlled `VMCALL` and `VMXOFF`;
- enable EPT with an identity map;
- prove EPT control with a controlled write violation on a test page.

## High-Level Architecture

ArgusVisor uses in-place virtualization.

It does not boot a separate guest operating system. Instead, Windows starts normally, the driver is loaded, and the current Windows execution context is placed under VMX control.

Before loading the driver:

```text
Hardware
-> Windows kernel
-> ArgusVisor driver


## Driver Entry Subsystem (driver_entry.cpp)

The driver_entry.cpp module serves as the orchestrator for ArgusVisor. It prepares the kernel execution environment, allocates hardware structures required by Intel VT-x/EPT extensions, and seamlessly transitions all active logical cores into VMX Root Operation.

Core Components
Hardware & Capability Checking

IsVMXSupported(): Verifies VT-x extensions via CPUID (Feature Flag ECX.VMX[bit 5]).

IsVMXEnabledByUEFI(): Queries the IA32_FEATURE_CONTROL MSR (0x3A) to ensure virtualization is enabled and locked at the BIOS/UEFI level.

ArgusVisorIsEptSupported(): Queries the IA32_VMX_EPT_VPID_CAP MSR (0x4C0) to confirm support for EPT features (Identity Mapping, 2 MB Large Pages, Execute-Only Memory).

EPT Subsystem Initializer

ArgusVisorInitializeEpt(): Allocates the EPT data structure (GlobalEptData), prepares second-level address translation tables (PML4, PDPT, PDT), and constructs the EPTP (EPT Pointer).

ArgusVisorPrepareEptWriteViolationTest(): Allocates a test page in non-paged memory and flips the associated EPT entry to read-only (R/O) to validate EPT_VIOLATION traps post-launch.

Per-CPU Memory Allocation

Dynamic allocations performed in the non-paged pool (POOL_FLAG_NON_PAGED), scaled to the active processor count (KeQueryActiveProcessorCount):

GlobalVmxData (PER_CPU_VMX_DATA): Virtual/physical addresses for VMXON regions, MSR Bitmaps, and Host execution stack.

GlobalVmcsData (PER_CPU_VMCS_DATA): Pointers and state metadata for VMCS (Virtual Machine Control Structure) regions.

VMX Orchestration & IPI Dispatch

VmxOrcherstator(): Uses KeIpiGenericCall to force synchronous execution of VmxonIpiWorker across all logical processor cores.

VmxonIpiWorker(): Executes at IRQL 14 (IPI_LEVEL) to perform the VT-x sequence:
__vmx_on ➔ __vmx_vmclear ➔ __vmx_vmptrld ➔ Setup VMCS ➔ __vmx_vmlaunch

Cleanup & Fault Tolerance

ArgusVisorCleanupVmxStructures(): Releases per-CPU allocations (VMXON, VMCS, Host stack) if initialization fails on any core.

ArgusVisorTerminateEpt(): Destroys EPT paging structures and frees GlobalEptData.
