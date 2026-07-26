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
