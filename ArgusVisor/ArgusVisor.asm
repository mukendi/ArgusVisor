; 
; File : ArgusVisor.asm
; Role    : Low-level assembly routines for the ArgusVisor engine
; 

extern ArgusVisorCExitHandler : proc
extern ArgusVisorVmResumeFailureHandler : proc

.code

; 
; - CONTEXT CAPTURE GUEST / SAFE RETURN POINT (Post-VMLAUNCH)
; 
public AsmGuestContextRip
AsmGuestContextRip proc
    ; As soon as __vmx_vmlaunch is successfully executed, the CPU switches to Guest mode
    ; and starts execution here. To avoid corrupting the active system stack,
    ; We simply return 1 (TRUE) in RAX to notify the C++ code of success.
    
    mov rax, 1
    ret
AsmGuestContextRip endp


; 
; - DESCRIPTOR REGISTER READING FUNCTIONS (GDTR / IDTR)
; 
public AsmReadGdtr
AsmReadGdtr proc
    ; RCX contains the pointer to our DESCRIPTOR_REGISTER structure
    sgdt [rcx]
    ret
AsmReadGdtr endp

public AsmReadIdtr
AsmReadIdtr proc
    ; RCX contains the pointer to our DESCRIPTOR_REGISTER structure
    sidt [rcx]
    ret
AsmReadIdtr endp


; 
; - THE CENTRAL VM-EXIT MANAGER (VMX Root Mode Entry)
; 
public ArgusVisorVmExitHandler
ArgusVisorVmExitHandler proc
    ; The CPU freezes the guest and jumps here to the host's 16KB private stack.
    ; Strict saving of the OS state in the exact order of the GUEST_REGISTERS structure.
    
    pushfq              ; Save RFLAGS
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    sub rsp, 8          ; Reserve space for RSP without using `push rsp` (which uses the host's value).
    push rbx
    push rdx
    push rcx
    push rax            ; Current top of the stack (First element of the structure)

    ; RCX becomes the first argument for the C++ function and points to our structure
    mov rcx, rsp

    ; A VM-exit can interrupt Windows in the middle of a sequence that uses
    ; volatile XMMs. The C++ compiler is free to modify xmm0-xmm5.
    sub rsp, 68h
    movaps xmmword ptr [rsp + 00h], xmm0
    movaps xmmword ptr [rsp + 10h], xmm1
    movaps xmmword ptr [rsp + 20h], xmm2
    movaps xmmword ptr [rsp + 30h], xmm3
    movaps xmmword ptr [rsp + 40h], xmm4
    movaps xmmword ptr [rsp + 50h], xmm5

    ; 
    sub rsp, 20h
    call ArgusVisorCExitHandler
    add rsp, 20h

    movaps xmm0, xmmword ptr [rsp + 00h]
    movaps xmm1, xmmword ptr [rsp + 10h]
    movaps xmm2, xmmword ptr [rsp + 20h]
    movaps xmm3, xmmword ptr [rsp + 30h]
    movaps xmm4, xmmword ptr [rsp + 40h]
    movaps xmm5, xmmword ptr [rsp + 50h]
    add rsp, 68h

    test al, al
    jz exit_vmx

    ; 
    pop rax
    pop rcx
    pop rdx
    pop rbx
    add rsp, 8          ; Restores the battery without overwriting RSP
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    popfq

    ; Resuming Windows execution seamlessly
    vmresume

    ; If vmresume fails, bugcheck with VMX information instead of freezing the core.
    sub rsp, 20h
    call ArgusVisorVmResumeFailureHandler
    add rsp, 20h
    int 3

exit_vmx:
    ; The C++ handler prepared rcx=guest_rip, rdx=guest_rsp,
    ; r8=guest_rflags and rax=return value.
    pop rax
    pop rcx
    pop rdx
    pop rbx
    add rsp, 8
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 8

    vmxoff
    jz vmxoff_failed
    jc vmxoff_failed

    push r8
    popfq
    mov rsp, rdx
    push rcx
    ret

vmxoff_failed:
    sub rsp, 20h
    call ArgusVisorVmResumeFailureHandler
    add rsp, 20h
    int 3
ArgusVisorVmExitHandler endp


; 
; - SEGMENT AND STACK REGISTER READING FUNCTIONS
; 
public AsmReadRsp
AsmReadRsp proc
    mov rax, rsp
    add rax, 8      
    ret
AsmReadRsp endp

public AsmReadCS
AsmReadCS proc
    mov ax, cs
    ret
AsmReadCS endp

public AsmReadDS
AsmReadDS proc
    mov ax, ds
    ret
AsmReadDS endp

public AsmReadES
AsmReadES proc
    mov ax, es
    ret
AsmReadES endp

public AsmReadFS
AsmReadFS proc
    mov ax, fs
    ret
AsmReadFS endp

public AsmReadGS
AsmReadGS proc
    mov ax, gs
    ret
AsmReadGS endp

public AsmReadSS
AsmReadSS proc
    mov ax, ss
    ret
AsmReadSS endp

public AsmReadTR
AsmReadTR proc
    str ax          ; Store Task Register (Current Windows TSS Selector)
    ret
AsmReadTR endp


; 
; - RECOVERY AND CLEANING OF SEGMENT ACCESS RIGHTS (LAR)
; 
public AsmReadSegmentAccessRights
AsmReadSegmentAccessRights proc
    ; RCX contains the selector for the segment to be analyzed
    lar rax, rcx
    jz no_error
    mov rax, 10000h ; Returns 0x10000 (unusable selector) if the instruction fails
    ret

no_error:
    shr rax, 8      ; Intel shifts access rights by 8 bits in the VMCS internal format
    and rax, 0F0FFh ; Strict architectural mask: forces the reserved bits (11:8 and 31:17) to 0
    ret
AsmReadSegmentAccessRights endp

; 
; - VIRTUAL MACHINE LAUNCH INSTRUCTIONS (VMLAUNCH)
; 
public AsmxVmlaunch
AsmxVmlaunch proc
    ; Updates the guest resumption point at the last minute. RSP points here
    ; to the return address of the C++ call, so guest_entry will be able to do ret.
    mov rax, rsp
    mov rcx, 0681Ch      ; VMCS_GUEST_RSP
    vmwrite rcx, rax
    jbe launch_failed

    lea rax, guest_entry
    mov rcx, 0681Eh      ; VMCS_GUEST_RIP
    vmwrite rcx, rax
    jbe launch_failed

    vmlaunch

launch_failed:
    mov rax, 0
    ret

guest_entry:
    mov rax, 1
    ret
AsmxVmlaunch endp

public AsmxVmcall
AsmxVmcall proc
    vmcall
    jz vmcall_failed
    jc vmcall_failed
    mov rax, 1
    ret

vmcall_failed:
    xor rax, rax
    ret
AsmxVmcall endp

public AsmxInveptGlobal
AsmxInveptGlobal proc
    sub rsp, 18h
    xor rax, rax
    mov qword ptr [rsp], rax
    mov qword ptr [rsp + 8], rax
    mov ecx, 2          ; all-context INVEPT
    invept rcx, oword ptr [rsp]
    jz invept_failed
    jc invept_failed
    add rsp, 18h
    mov rax, 1
    ret

invept_failed:
    add rsp, 18h
    xor rax, rax
    ret
AsmxInveptGlobal endp

end

