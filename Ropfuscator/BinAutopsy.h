// ==============================================================================
//   BINARY AUTOPSY
//   part of the ROPfuscator project
// ==============================================================================
// This module extracts useful features from a given binary. We call it
// "autopsy" because we have to analyse an already compiled program, dissecting
// it in many sections and extracting as many features as possible.
//
// More in detail, the module is able to extract:
//      - data about ELF sections
//      - symbols from the .dynsym section
//      - microgadgets from executable sections
//
// Microgadgets are a subset of what are commonly known as ROP Gadgets, with the
// only difference that we grab only the ones composed by a single instruction
// before the ret, e.g.:
//        mov eax, ebx
//        ret
//
// This module offers also a set of helper methods to search for a specific
// microgadget or verify the exchangeability of its operands.

#ifndef BINAUTOPSY_H
#define BINAUTOPSY_H

#define PACKAGE "ropfuscator" /* see https://bugs.gentoo.org/428728 */

#include "ChainElem.h"
#include "Microgadget.h"
#include "Section.h"
#include "Symbol.h"
#include "XchgGraph.h"
#include <bfd.h>
#include <capstone/capstone.h>
#include <capstone/x86.h>
#include <string>
#include <vector>

// Max bytes before the RET to be examined (RET included!)
// see BinaryAutopsy::extractGadgets()
#define MAXDEPTH 4

// BinaryAutopsy - dumps all the data needed by ROPfuscator.
// It provides also methods to look for specific gadgets and performs
// operand exchangeability analyses.
// This class has been designed as singleton to simplify the interaction with
// the ROPChain class. Indeed, we don't want to analyse the same file every time
// that a new ROPChain is instanciated.
class BinaryAutopsy {
private:
  // Singleton
  static BinaryAutopsy *instance;
  BinaryAutopsy(std::string path);

public:
  // XchgGraph instance
  XchgGraph xgraph;

  // Symbols - results from dumpDynamicSymbols() are placed here
  std::vector<Symbol> Symbols;

  // Sections - results from dumpSections() are placed here
  std::vector<Section> Sections;

  // Microgadgets - results from dumpGadgets() are placed here
  std::vector<Microgadget> Microgadgets;

  // BinaryPath - path of the binary file that is being analysed
  char *BinaryPath;

  // BfdHandle - an handle to read ELF headers. Used by dumpSections() and
  // dumpDynamicSymbols()
  bfd *BfdHandle;

  // getInstance - returns an instance of this singleton class
  static BinaryAutopsy *getInstance(std::string path);

  // -----------------------------------------------------------------------------
  //  ANALYSES
  // -----------------------------------------------------------------------------

  // dissect - dumps all the data and performs every analysis.
  void dissect();

  // dumpSections - parses the ELF header using LibBFD to obtain a list of
  // sections that contain executable code, from which the symbol and gadget
  // extraction will take place.
  void dumpSections();

  // dumpDynamicSymbols - extracts symbols from the .dynsym section. It takes
  // into account only function symbols with global scope and used in executable
  // sections.
  void dumpDynamicSymbols();

  // dumpGadgets - extracts every microgadget (i.e., single instructions
  // before a RET) that can be found in executable sections. Each instruction is
  // decoded using capstone-engine.
  void dumpGadgets();

  // buildXchgGraph - creates a new instance of xgraph and feeds it with all the
  // XCHG gadgets that have been found.
  void buildXchgGraph();

  // applyGadgetFilters - removes problematic gadgets from the set of discovered
  // ones, basing on the defined filters.
  void applyGadgetFilters();

  // -----------------------------------------------------------------------------
  //  HELPER METHODS
  // -----------------------------------------------------------------------------

  // getRandomSymbol - returns a random symbol. This is used to reference each
  // gadget in the ROP chain as sum of a random symbol address and the gadget
  // offset from it.
  Symbol *getRandomSymbol();

  // gadgetLookup - set of overloaded methods to look for a specific gadget in
  // the set of the ones that have been previously discovered.
  Microgadget *gadgetLookup(std::string asmInstr);
  std::vector<Microgadget *> gadgetLookup(x86_insn insn, x86_op_type op0,
                                          x86_op_type op1 = x86_op_type());
  std::vector<Microgadget *> gadgetLookup(x86_insn insn, x86_reg op0,
                                          x86_reg op1 = X86_REG_INVALID);
  std::vector<Microgadget *> gadgetLookup(GadgetClass_t Class);

  // canInitReg - tells if a given register can be initialised using a gadget.
  bool canInitReg(unsigned int reg);

  // getInitialisableRegs - returns a vector of registers that can be
  // initialised using appropriate gadgets.
  std::vector<x86_reg> getInitialisableRegs();

  // checkXchgPath - uses XChgGraph to check whether two (or more registers) can
  // be mutually exchanged.
  bool checkXchgPath(x86_reg a, x86_reg b, x86_reg c = X86_REG_INVALID);
  bool checkXchgPath(x86_reg a, std::vector<x86_reg> B);

  // getXchgPath - returns a vector of XCHG gadgets in order to exchange the
  // given two registers.
  std::vector<Microgadget *> getXchgPath(x86_reg a, x86_reg b);

  std::vector<Microgadget *> undoXchgs();

  // getReachableRegs - returns a list of all the nodes that can be reached
  // starting the exploration from the given register. If no other register can
  // be reached, it returns just the src itself.
  std::vector<int> getReachableRegs(int src);

  // -------------------------------------------------------------
  // GADGET PRIMITIVES

  std::vector<ChainElem> initReg(x86_reg dst, unsigned val);
  std::vector<ChainElem> addRegs(x86_reg dst, x86_reg src);
  std::vector<ChainElem> load(x86_reg dst, x86_reg src);
  std::vector<ChainElem> store(x86_reg dst, x86_reg src);
  std::vector<ChainElem> calcAddr(x86_reg dst, x86_reg src, unsigned displ);
};

#endif