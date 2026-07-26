#include "header.h"

extern PEPT_DATA    GlobalEptData;
// Test EPT violation so VMExit
extern PVOID             GlobalEptTestPage;
extern PHYSICAL_ADDRESS  GlobalEptTestPagePhysical;
extern ULONG64*          GlobalEptTestPte;
extern LONG              GlobalEptTestArmed;
extern LONG              GlobalEptTestViolationCount ;

static const ULONG64 kEptReadAccess = 1ULL << 0;
static const ULONG64 kEptWriteAccess = 1ULL << 1;
static const ULONG64 kEptExecuteAccess = 1ULL << 2;
static const ULONG64 kEptMemoryTypeUncacheable = 0ULL;
static const ULONG64 kEptMemoryTypeWriteBack = 6ULL;
static const ULONG64 kEptLargePage = 1ULL << 7;
static const ULONG64 kEptPageWalkLength4 = 3ULL << 3;
static const ULONG64 kEpt4KbAddressMask = 0x000FFFFFFFFFF000ULL;
static const ULONG64 kEpt2MbPageSize = 0x200000ULL;
static const ULONG64 kEpt2MbAddressMask = 0x000FFFFFFFE00000ULL;
static const ULONG   kArgusVisorMaxEptTablePages = 4096;

void ArgusVisorFreeAllocatedState();
void ArgusVisorDumpVmExitSummary();
void ArgusVisorTerminateEpt();
void ArgusVisorTriggerEptWriteViolationTest();

NTSTATUS ArgusVisorInitializeEpt();
NTSTATUS ArgusVisorPrepareEptWriteViolationTest();

bool ArgusVisorIsEptSupported();