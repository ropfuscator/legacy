//
// Created by Daniele Ferla on 22/10/2018.
//

#include "ROPseeker.h"
#include "../X86.h"
#include "../X86InstrBuilder.h"
#include "../X86MachineFunctionInfo.h"
#include "../X86RegisterInfo.h"
#include "../X86Subtarget.h"
#include "../X86TargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <stdio.h>
#include <string>
#include <sys/time.h>

using namespace llvm;
enum type_t { OFFSET, IMMEDIATE };

struct ROPGadget {
  /* Maps values to be pushed onto the stack to a specific type */
  type_t type;
  int64_t value;

  ROPGadget(type_t type, int64_t value) : type(type), value(value) {}
};

struct Stats {
    int totalInstr;
    int replaced;

    Stats() : totalInstr(0), replaced(0){};
};

class ROPChain {
  /* Keeps track of all the instructions to be replaced with the obfuscated
   * ones. Handles the injection of auxiliary machine code to guarantee the
   * correct chain execution and to resume the non-obfuscated code execution
   * afterwards. */

  // IDs
  static int globalChainID;
  int chainID;

  // A finalized chain can't get gadgets anymore
  bool finalized = false;

  // Gadgets to be pushed onto the stack during the injection phase
  std::vector<ROPGadget> gadgets;

  // Input instructions that we want to replace with obfuscated ones
  std::vector<MachineInstr *> instructionsToDelete;

  typedef std::map<std::string, Gadget*> ropmap;
  static ropmap libc_microgadgets;

public:
  // Labels for inline asm instructions ("C" = colon)
  char chainLabel[16];    // chain_X
  char chainLabel_C[16];  // chain_X:
  char resumeLabel[16];   // resume_X
  char resumeLabel_C[16]; // resume_X:

  // Injection location within the program code
  MachineBasicBlock *MBB;
  MachineFunction *MF;
  MachineInstr *injectionPoint;
  MCInstrInfo const *TII;

  // Methods
  int addInstruction(MachineInstr &MI);
  int mapBindings(MachineInstr &MI);
  unsigned getBaseAddress();
  void inject();
  void loadEffectiveAddress(int64_t displacement);

  // Helper methods
  unsigned getOffsetByAsm(std::string asmInstr);
  bool isFinalized();
  void finalize();
  bool isEmpty();

  ROPChain(MachineBasicBlock &MBB, MachineInstr &injectionPoint)
      : MBB(&MBB), injectionPoint(&injectionPoint) {
    MF = MBB.getParent();
    TII = MF->getTarget().getMCInstrInfo();
    chainID = globalChainID++;

    // Creates all the labels
    sprintf(chainLabel, ".chain_%d", chainID);
    sprintf(chainLabel_C, ".chain_%d:", chainID);
    sprintf(resumeLabel, ".resume_%d", chainID);
    sprintf(resumeLabel_C, ".resume_%d:", chainID);
  }

  ~ROPChain() { globalChainID--; }
};

ROPChain::ropmap ROPChain::libc_microgadgets = findGadgets();

unsigned ROPChain::getOffsetByAsm(std::string asmInstr) {
  unsigned offset = libc_microgadgets[asmInstr]->address;
  assert(offset != 0 &&
         "Unable to find specified asm instruction in the gadget library!");
  return offset;
}

int ROPChain::globalChainID = 0;

unsigned ROPChain::getBaseAddress() {
  // TODO: Opaque constant generation must be implemented here to conceal the
  // base address
  return 0xb7e11000;
}

void ROPChain::inject() {
  /* PROLOGUE: saves the EIP value before executing the ROP chain */
  BuildMI(*MBB, injectionPoint, nullptr,
          TII->get(X86::CALLpcrel32)) // call chain_X
      .addExternalSymbol(chainLabel);
  BuildMI(*MBB, injectionPoint, nullptr, TII->get(X86::JMP_1)) // jmp resume_X
      .addExternalSymbol(resumeLabel);
  BuildMI(*MBB, injectionPoint, nullptr,
          TII->get(TargetOpcode::INLINEASM)) // chain_X:
      .addExternalSymbol(chainLabel_C)
      .addImm(0);
  BuildMI(*MBB, injectionPoint, nullptr,
          TII->get(X86::MOV32ri)) // mov ebx, LIBC_BASE_ADDR
      .addReg(X86::EBX)
      .addImm(getBaseAddress());

  /* Pushes each gadget onto the stack in reverse order */
  for (auto gadget = gadgets.rbegin(); gadget != gadgets.rend(); ++gadget) {
    if (gadget->type == IMMEDIATE) {
      /* Pushes the immediate value directly onto the stack, without further
       * computations */
      BuildMI(*MBB, injectionPoint, nullptr,
              TII->get(X86::PUSHi32)) // push $imm
          .addImm(gadget->value);
    } else {
      /* At first it pushes the ebx register (within which the libc base address
       * is located, then it adds the gadget offset to it */
      BuildMI(*MBB, injectionPoint, nullptr, TII->get(X86::PUSH32r)) // push ebx
          .addReg(X86::EBX);
      addDirectMem(
          BuildMI(*MBB, injectionPoint, nullptr, TII->get(X86::ADD32mi)),
          X86::ESP)
          .addImm(gadget->value); // add [esp], $offset
    }
  }

  /* EPILOGUE
  Emits the `ret` instruction which will trigger the chain execution, and a
  label to resume the normal execution flow when the chain has finished. */
  BuildMI(*MBB, injectionPoint, nullptr, TII->get(X86::RETL)); // ret
  BuildMI(*MBB, injectionPoint, nullptr,
          TII->get(TargetOpcode::INLINEASM)) // resume_X:
      .addExternalSymbol(resumeLabel_C)
      .addImm(0);

  for (MachineInstr *MI : instructionsToDelete) {
    MI->eraseFromParent();
  }
}

int ROPChain::addInstruction(MachineInstr &MI) {
  /* Wrapper method: if a correct binding can be found between the original
   * instruction and some gadgets, the original instruction is put in a vector.
   * We keep track of all the instructions to remove in order to defer the
   * actual deletion to the moment in which we'll inject the ROP Chain. We do
   * this because currently MI is just an iterator */
  assert(!finalized && "Attempt to modify a finalized chain!");
  int err = mapBindings(MI);

  if (!err) {
    instructionsToDelete.push_back(&MI);
  }

  return err;
}

int ROPChain::mapBindings(MachineInstr &MI) {
  /* Given a specific MachineInstr it tries to find a series of gadgets that
   * can replace the input instruction maintaining the same semantics.
   * In general we use registers as following:
   *    - EAX: accumulator, storage of computed values
   *    - EBX: pointer to the libc base address
   *    - ECX: storage of immediate values
   *    - EDX: storage of memory addresses */

  unsigned opcode = MI.getOpcode();
  switch (opcode) {
  case X86::ADD32ri8:
  case X86::ADD32ri:
    if (MI.getOperand(0).getReg() == X86::EAX) {
      gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("pop ecx;")));
      gadgets.push_back(ROPGadget(IMMEDIATE, MI.getOperand(2).getImm()));
      gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("add eax, ecx;")));
      return 0;
    } else
      return 1;
  case X86::SUB32ri8:
  case X86::SUB32ri:
    if (MI.getOperand(0).getReg() == X86::EAX) {
      gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("pop ecx;")));
      gadgets.push_back(ROPGadget(IMMEDIATE, -MI.getOperand(2).getImm()));
      gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("add eax, ecx;")));
      return 0;
    } else
      return 1;
  case X86::MOV32ri:
    if (MI.getOperand(0).getReg() == X86::EAX) {
      gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("pop eax;")));
      gadgets.push_back(ROPGadget(IMMEDIATE, MI.getOperand(2).getImm()));
      return 0;
    } else
      return 1;
  case X86::MOV32rm:
    // mov eax, dword ptr [ebp - $displacement]
    if (MI.getOperand(0).getReg() == X86::EAX &&
        MI.getOperand(1).getReg() == X86::EBP) {
      loadEffectiveAddress(MI.getOperand(4).getImm());
      gadgets.push_back(
          ROPGadget(OFFSET, getOffsetByAsm("mov eax, dword ptr [edx];")));
      return 0;
    } else
      return 1;
  case X86::MOV32mr:
    // mov dword ptr [ebp - $displacement], eax
    if (MI.getOperand(0).getReg() == X86::EBP &&
        MI.getOperand(5).getReg() == X86::EAX) {
      loadEffectiveAddress(MI.getOperand(3).getImm());
      gadgets.push_back(
          ROPGadget(OFFSET, getOffsetByAsm("mov dword ptr [edx], eax;")));
      return 0;
    } else
      return 1;
  default:
    return 1;
  }
}

void ROPChain::loadEffectiveAddress(int64_t displacement) {
  /* Loads the effective address of a memory reference of type [ebp +
   * $displacement] in EDX */
  // EAX <- EBP
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("xchg eax, ebp;")));
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("xchg eax, edx;")));
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("mov eax, edx;")));
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("xchg eax, ebp;")));
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("xchg eax, edx;")));
  // EAX = EAX + $displacement
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("pop ecx;")));
  gadgets.push_back(ROPGadget(IMMEDIATE, displacement));
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("add eax, ecx;")));
  // EDX <- EAX
  gadgets.push_back(ROPGadget(OFFSET, getOffsetByAsm("xchg eax, edx;")));
}

void ROPChain::finalize() { finalized = true; }

bool ROPChain::isFinalized() { return finalized; }

bool ROPChain::isEmpty() { return gadgets.empty(); }
