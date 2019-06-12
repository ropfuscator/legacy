// ==============================================================================
//   X86 ROP Utils
//   part of the ROPfuscator project
// ==============================================================================
// This is the main module of the whole project.
// It provides high-level reasoning about the actual ROP chain creation by
// mapping each instruction, given as input, to a series of microgadgets that
// have the very same semantic.
// It is also responsible to inject the newly built ROP chain and remove the
// instructions that have been replaced.

#include "RopfuscatorBinAutopsy.h"
#include "RopfuscatorLivenessAnalysis.h"
#include "X86.h"
#include "X86InstrBuilder.h"
#include "X86TargetMachine.h"
#include <tuple>

#ifndef X86ROPUTILS_H
#define X86ROPUTILS_H

#define CHAIN_LABEL_LEN 16

#if __GNUC__
#if __x86_64__ || __ppc64__
#define ARCH_64
const std::string POSSIBLE_LIBC_FOLDERS[] = {"/lib32", "/usr/lib32",
                                             "/usr/local/lib32"};
#else
#define ARCH_32
const std::string POSSIBLE_LIBC_FOLDERS[] = {"/lib", "/usr/lib",
                                             "/usr/local/lib"};
#endif
#endif

enum type_t { GADGET, IMMEDIATE };

struct Stats {
  int processed;
  int replaced;

  Stats() : processed(0), replaced(0){};
};

using namespace std;
using namespace llvm;

// Generic element to be put in the chain.
struct ChainElem {
  // type - it can be a GADGET or an IMMEDIATE value. We need to specify the
  // type because we will use different strategies during the creation of
  // machine instructions to push elements of the chain onto the stack.
  type_t type;

  union {
    // value - immediate value
    int64_t value;

    // pointer to a microgadget
    const Microgadget *microgadget;
  };

  // s - pointer to a symbol.
  // We bind symbols to chain elements because, if we'd do that to actual
  // microgadgets, it would be fairly easy to predict which gadget is referenced
  // with a symbol, since during the chain execution very few gadgets are
  // executed.
  Symbol *symbol;

  // Constructor (type: GADGET)
  ChainElem(Microgadget *g);

  // Constructor (type: IMMEDIATE)
  ChainElem(int64_t value);

  // getRelativeAddress - returns the gadget address relative to the symbol it
  // is anchored to.
  uint64_t getRelativeAddress();
};

// Keeps track of all the instructions to be replaced with the obfuscated
// ones. Handles the injection of auxiliary machine code to guarantee the
// correct chain execution and to resume the non-obfuscated code execution
// afterwards.
class ROPChain {
  // globalChainID - just an incremental ID number for all the chains that will
  // be created.
  static int globalChainID;

  // chainID - chain number.
  int chainID;

  // finalized - this flag tells if the chain has to be closed. This happens
  // when an unsupported instruction is encountered: the chain is closed, the
  // unsupported instruction remains untouched, and possibly a new chain is
  // created as soon as a supported instruction is processed.
  bool finalized = false;

  // instructionsToDelete - keeps track of all the instructions that we want to
  // replace with obfuscated ones
  vector<MachineInstr *> instructionsToDelete;

  // Injection location within the program code
  MachineBasicBlock *MBB;
  MachineFunction *MF;
  MCInstrInfo const *TII;
  MachineInstr &injectionPoint;

  bool handleAddSubIncDec(MachineInstr *);
  bool handleMov32rm(MachineInstr *);
  bool handleMov32mr(MachineInstr *);
  void addToInstrMap(MachineInstr *, ChainElem);

public:
  // Labels for inline asm instructions ("C" = colon)
  char chainLabel[CHAIN_LABEL_LEN];    // chain_X
  char chainLabel_C[CHAIN_LABEL_LEN];  // chain_X:
  char resumeLabel[CHAIN_LABEL_LEN];   // resume_X
  char resumeLabel_C[CHAIN_LABEL_LEN]; // resume_X:

  // chain - holds all the elements of the ROP chain
  vector<ChainElem> chain;

  // SRT - holds data about the available registers that can be used as scratch
  // registers (see LivenessAnalysis).
  ScratchRegTracker &SRT;

  // BA - shared instance of Binary Autopsy.
  static BinaryAutopsy *BA;

  // instruction mapping between MachineInstrs and their gadget counterpart
  map<MachineInstr *, vector<ChainElem>> instrMap;

  // Constructor
  ROPChain(MachineBasicBlock &MBB, MachineInstr &injectionPoint,
           ScratchRegTracker &SRT);

  // Destructor
  ~ROPChain();

  // addInstruction - wrapper method: if a correct binding can be found
  // between the original instruction and some gadgets, the original
  // instruction is put in a vector. We keep track of all the instructions
  // to remove in order to defer the actual deletion to the moment in which
  // we'll inject the ROP Chain. We do this because currently MI is just an
  // iterator
  bool addInstruction(MachineInstr &MI);

  // mapBindings - determines the gadgets to use to replace a single input
  // instruction.
  bool mapBindings(MachineInstr &MI);

  // inject - injects the new instructions to build the ROP chain and removes
  // the original ones.
  void inject();

  // addImmToReg - adds an immediate value (stored into a scratch register) to
  // the given register.
  bool addImmToReg(MachineInstr *MI, x86_reg reg, int immediate,
                   vector<x86_reg> const &scratchRegs);

  // computeAddress - finds the correct set of gadgets such that:
  // the value in "inputReg" is copied in a scratch register, incremented by the
  // value of "displacement", and placed in any register that can be exchanged
  // with "outputReg".
  // The return value is the actual register in which the computed value is
  // saved. This is useful to whom calls this method, in order to create an
  // exchange chain to move the results onto another register.
  x86_reg computeAddress(MachineInstr *MI, x86_reg inputReg, int displacement,
                         x86_reg outputReg, vector<x86_reg> scratchRegs);

  // Xchg - Concatenates a series of XCHG gadget in order to exchange reg a with
  // reg b.
  int Xchg(MachineInstr *, x86_reg a, x86_reg b);

  // DoubleXchg - Concatenates a series of XCHG gadget in order to exchange reg
  // a with reg b, and c with d. This method helps to prevent two exchange
  // chains that have the same operands to undo each other.
  void DoubleXchg(MachineInstr *, x86_reg a, x86_reg b, x86_reg c, x86_reg d);

  void undoXchgs(MachineInstr *MI);
  x86_reg getEffectiveReg(x86_reg reg);

  // Helper methods
  bool isFinalized();
  void finalize();
  bool isEmpty();
};

#endif