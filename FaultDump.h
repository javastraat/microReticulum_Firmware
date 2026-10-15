// Hard-fault dump for the nRF52840: the fault handlers copy the Cortex-M4
// fault registers and the stacked PC/LR into a record that lives in .noinit
// RAM (see boards/nrf52840_s140_v7.ld), then reset the chip. The next boot
// prints the record next to the reset reason and clears it.
//
// The Adafruit core's utility/debug.cpp also defines HardFault_Handler (it
// only resets), and that object is always linked in for dbg_err_str(), so
// the nRF52 envs link with -Wl,--allow-multiple-definition: the sketch's
// definition comes first on the link line and wins.
#pragma once
#include <nrf.h>

struct FaultRecord { uint32_t magic, cfsr, hfsr, bfar, mmfar, pc, lr, xpsr, sp; };
#define FAULT_RECORD_MAGIC 0xFA17D0D0u
__attribute__((section(".noinit"))) FaultRecord fault_record;

// r0 = the stack frame the exception pushed (r0-r3, r12, lr, pc, xpsr).
extern "C" __attribute__((used)) void fault_dump_save(uint32_t* frame) {
  fault_record.cfsr  = SCB->CFSR;
  fault_record.hfsr  = SCB->HFSR;
  fault_record.bfar  = SCB->BFAR;
  fault_record.mmfar = SCB->MMFAR;
  fault_record.lr    = frame[5];
  fault_record.pc    = frame[6];
  fault_record.xpsr  = frame[7];
  fault_record.sp    = (uint32_t)frame;
  fault_record.magic = FAULT_RECORD_MAGIC;
  NVIC_SystemReset();
}

// Pick the stack that was in use (EXC_RETURN bit 2: 0 = MSP, 1 = PSP).
extern "C" __attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n b fault_dump_save\n");
}
extern "C" void MemoryManagement_Handler(void) __attribute__((alias("HardFault_Handler")));
extern "C" void BusFault_Handler(void)         __attribute__((alias("HardFault_Handler")));
extern "C" void UsageFault_Handler(void)       __attribute__((alias("HardFault_Handler")));

// Uncaught C++ exception. libstdc++'s default handler demangles the type name
// for a stderr nobody reads, and drags ~29 KB of demangler into flash; this
// one records it like a fault (cfsr = hfsr = 0, lr = caller of terminate).
namespace __gnu_cxx {
  void __verbose_terminate_handler() {
    fault_record = FaultRecord{FAULT_RECORD_MAGIC, 0, 0, 0, 0, 0, (uint32_t)__builtin_return_address(0), 0, 0};
    NVIC_SystemReset();
  }
}

static void fault_dump_print() {
  if (fault_record.magic != FAULT_RECORD_MAGIC) { printf("[init] fault record: none\n"); return; }
  if (!fault_record.cfsr && !fault_record.hfsr) printf("[init] fault record: uncaught C++ exception\n");
#if defined(SCOTMESH_PAGES) && defined(SM_LXMF)
  sm_note_fault(!fault_record.cfsr && !fault_record.hfsr, fault_record.pc, fault_record.lr);   // for the "back up" alert
#endif
  printf("[init] fault record: cfsr=%08lx hfsr=%08lx bfar=%08lx mmfar=%08lx pc=%08lx lr=%08lx xpsr=%08lx sp=%08lx\n",
         (unsigned long)fault_record.cfsr, (unsigned long)fault_record.hfsr, (unsigned long)fault_record.bfar,
         (unsigned long)fault_record.mmfar, (unsigned long)fault_record.pc, (unsigned long)fault_record.lr,
         (unsigned long)fault_record.xpsr, (unsigned long)fault_record.sp);
  fault_record.magic = 0;
}
