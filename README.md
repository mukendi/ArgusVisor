# ArgusVisor

> A lightweight Intel VT-x research hypervisor for Windows x64.

![Platform](https://img.shields.io/badge/Platform-Windows%20x64-blue)
![Language](https://img.shields.io/badge/C%2B%2B-Kernel-orange)
![Virtualization](https://img.shields.io/badge/Intel-VT--x-success)
![License](https://img.shields.io/badge/License-Educational-lightgrey)

---

## Overview

ArgusVisor is an educational Intel VT-x hypervisor designed to help security researchers, reverse engineers, and systems programmers understand how modern hardware virtualization actually works.

Unlike many hypervisor tutorials that focus only on implementation, ArgusVisor emphasizes **understanding the architecture** behind Intel VT-x.

The project demonstrates:

- VMX Root / Non-root Operation
- VMCS lifecycle
- VM Entry / VM Exit
- CPUID interception
- MSR interception
- VMCALL-based unload
- Extended Page Tables (EPT)
- Dynamic EPT permission management

The objective is not to build another production hypervisor, but to create a readable research platform that exposes the internal mechanisms used by modern virtualization-based security technologies.

---

# Current Features

- Intel VT-x capability detection
- VMXON initialization
- VMCS initialization
- VMCS per logical processor
- Multi-core virtualization
- VM Entry / VM Resume
- CPUID interception
- RDMSR / WRMSR interception
- CR Access interception
- XSETBV interception
- VMCALL hypercalls
- EPT Identity Mapping
- 2 MB → 4 KB EPT splitting
- Dynamic EPT permission updates
- INVEPT support
- DebugView tracing

---

# Project Layout

```
ArgusVisor
│
├── DriverEntry.cpp
├── ArgusVisor.asm
├── vmcs_encoder.h
├── ept.cpp
├── vmexit.cpp
├── log.cpp
├── Docs/
└── README.md
```

---

# VMX Initialization

```
DriverEntry
      │
      ▼
CPUID
      │
      ▼
IA32_FEATURE_CONTROL
      │
      ▼
VMXON
      │
      ▼
Allocate VMCS
      │
      ▼
Configure VMCS
      │
      ▼
VMLAUNCH
      │
      ▼
Windows executes inside VMX Non-root
```

---

# VM Exit Flow

```
Guest executes instruction
        │
        ▼
CPU detects interception
        │
        ▼
VM Exit
        │
        ▼
Host State restored
        │
        ▼
ArgusVisor VM Exit Handler
        │
        ▼
Instruction Emulation
        │
        ▼
VMRESUME
```

---

# Extended Page Tables

ArgusVisor uses Intel Extended Page Tables (EPT) to perform second-level address translation.

```
Guest Virtual Address
        │
Windows Page Tables
        │
Guest Physical Address
        │
EPT
        │
Host Physical Address
```

The current implementation provides:

- Identity Mapping
- Dynamic permission updates
- Controlled EPT write violations
- INVEPT invalidation

---

# Educational Goals

ArgusVisor was created to explain not hide the internal mechanisms of Intel VT-x.

Topics explored include:

- VMX operation
- VMCS
- VM Entry
- VM Exit
- EPT
- Hypervisor introspection
- Memory virtualization
- Hardware-assisted security

---

# Build Requirements

- Windows 10 / 11 x64
- Visual Studio 2022
- Windows Driver Kit (WDK)
- Intel CPU with VT-x and EPT
- Test Signing enabled

---

# Roadmap

## Completed

- VMXON
- VMCS
- VMLAUNCH
- VMRESUME
- VM Exit dispatcher
- CPUID interception
- MSR interception
- EPT Identity Mapping

## Planned

- Monitor Trap Flag
- VPID
- INVVPID
- EPT Hooking
- Hypervisor Introspection API
- Memory Cloaking
- Nested Virtualization

---

# Research Vision

ArgusVisor serves as the experimental platform for studying:

- Hypervisor-based EDRs
- Memory introspection
- Kernel integrity monitoring
- EPT-based detection
- Anti-rootkit techniques
- Hardware-assisted security

---

# Warning

This project runs in Windows kernel mode using Intel VT-x.

Incorrect VMCS configuration or invalid EPT entries can freeze or crash the operating system.

Use only inside a dedicated research laboratory.

---

# Acknowledgments

Inspired by the work of:

- Intel documentation
- Microsoft Hyper-V Team
- Satoshi Tanda 

---

# License

Educational and research purposes only.
