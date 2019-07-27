// ==============================================================================
//   BINARY AUTOPSY
//   part of the ROPfuscator project
// ==============================================================================

#include "BinAutopsy.h"
#include "CapstoneLLVMAdpt.h"
#include "ChainElem.h"
#include "Debug.h"

#include <assert.h>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <time.h>

using namespace std;

BinaryAutopsy::BinaryAutopsy(string path) {
  BinaryPath = new char[path.length() + 1];
  strncpy(BinaryPath, path.c_str(), path.length() + 1);

  ifstream f(path);
  assert(f.good() && "Given file doesn't exist or is invalid!");
  llvm::dbgs() << "USING: " << BinaryPath << "\n";

  // Initialises LibBFD and opens the binary
  bfd_init();
  BfdHandle = bfd_openr(BinaryPath, NULL);
  assert(bfd_check_format(BfdHandle, bfd_object) &&
         "Given file does not look like a valid ELF file");

  // Seeds the PRNG (we'll use it in getRandomSymbol());
  srand(time(nullptr));

  dissect();
}

void BinaryAutopsy::dissect() {
  dumpSections();
  dumpDynamicSymbols();
  dumpGadgets();
  applyGadgetFilters();
  buildXchgGraph();
}

BinaryAutopsy *BinaryAutopsy::instance = 0;

BinaryAutopsy *BinaryAutopsy::getInstance(string path) {
  if (instance == nullptr) {
    instance = new BinaryAutopsy(path);
  }

  return instance;
}

BinaryAutopsy *BinaryAutopsy::getInstance() {
  assert(instance != nullptr && "No pre-existing instance of Binary Autopsy.");
  return instance;
}

void BinaryAutopsy::dumpSections() {
  int flags;
  asection *s;
  uint64_t vma, size;
  const char *sec_name;

  using namespace llvm;
  DEBUG_WITH_TYPE(SECTIONS,
                  dbgs() << "[SECTIONS]\tLooking for CODE sections... \n");
  using namespace std;
  // Iterates through all the sections, picking only the ones that contain
  // executable code
  for (s = BfdHandle->sections; s; s = s->next) {
    flags = bfd_get_section_flags(BfdHandle, s);

    if (flags & SEC_CODE) {
      vma = bfd_section_vma(BfdHandle, s);
      size = bfd_section_size(BfdHandle, s);
      sec_name = bfd_section_name(BfdHandle, s);

      if (!sec_name)
        sec_name = "<unnamed>";

      Sections.push_back(Section(sec_name, vma, size));
      DEBUG_WITH_TYPE(SECTIONS, llvm::dbgs() << "[SECTIONS]\tFound section "
                                             << sec_name << "\n");
    }
  }
}

void BinaryAutopsy::dumpDynamicSymbols() {
  const char *symbolName;
  size_t addr, size, nsym;

  // Dumps sections if it wasn't already done
  if (Sections.empty())
    dumpSections();

  // llvm::dbgs() << "[*] Scanning for symbols... \n";

  // Allocate memory and get the symbol table
  size = bfd_get_dynamic_symtab_upper_bound(BfdHandle);
  auto **asymtab = static_cast<asymbol **>(malloc(size));
  nsym = bfd_canonicalize_dynamic_symtab(BfdHandle, asymtab);

  // Scan for all the symbols
  for (size_t i = 0; i < nsym; i++) {
    asymbol *sym = asymtab[i];

    // Consider only function symbols with global scope
    if ((sym->flags & BSF_FUNCTION) && (sym->flags & BSF_GLOBAL)) {
      symbolName = bfd_asymbol_name(sym);

      // those two symbols are very often subject to aliasing (they can be found
      // in many different libraries loaded in memory), so better avoiding them!
      if (strcmp(symbolName, "_init") == 0 || strcmp(symbolName, "_fini") == 0)
        continue;

      addr = bfd_asymbol_value(sym);

      // Get version string to avoid symbol aliasing
      const char *versionString = nullptr;
      bfd_boolean hidden = false;

      if ((sym->flags & (BSF_SECTION_SYM | BSF_SYNTHETIC)) == 0)
        versionString = bfd_get_symbol_version_string(BfdHandle, sym, &hidden);

      // we cannot use multiple versions of the same symbol, so we discard any
      // duplicate.
      bool alreadyPresent = false;
      for (auto &s : Symbols) {
        if (strcmp(s.Label, symbolName) == 0) {
          alreadyPresent = true;
          break;
        }
      }
      if (!alreadyPresent)
        Symbols.emplace_back(Symbol(symbolName, versionString, addr));
    }
  }

  free(asymtab);
  // llvm::dbgs() << "[*] Found " << Symbols.size() << " symbols\n";

  assert(!Symbols.empty() && "No symbols found!");
}

Symbol *BinaryAutopsy::getRandomSymbol() {
  unsigned long i = rand() % Symbols.size();
  return &(Symbols.at(i));
}

void BinaryAutopsy::dumpGadgets() {
  uint8_t ret[] = "\xc3";

  // capstone stuff
  csh handle;
  cs_insn *instructions;

  // Dumps sections if it wasn't already done
  if (Sections.empty())
    dumpSections();

  // Initizialises capstone engine
  cs_open(CS_ARCH_X86, CS_MODE_32, &handle);
  cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

  ifstream input_file(BinaryPath, ios::binary);
  assert(input_file.good() && "Unable to open given binary file!");

  // Get input size
  input_file.seekg(0, ios::end);
  streamoff input_size = input_file.tellg();

  // Read the whole file
  input_file.seekg(0, ios::beg);
  auto *buf = new uint8_t[input_size];
  input_file.read(reinterpret_cast<char *>(buf), input_size);

  for (auto &s : Sections) {
    int cnt = 0;

    // Scan for RET instructions
    for (uint64_t i = s.Address;
         i < static_cast<uint64_t>(s.Address + s.Length); i++) {

      if (buf[i] == *ret) {
        size_t offset = i + 1;
        uint8_t *cur_pos = buf + offset;

        // Iteratively try to decode starting from (MAXDEPTH to 0)
        // instructions before the actual RET
        for (int depth = MAXDEPTH; depth >= 0; depth--) {

          size_t count = cs_disasm(handle, cur_pos - depth, depth,
                                   offset - depth, depth, &instructions);

          // Valid gadgets must have two instructions, and the
          // last one must be a RET
          if (count == 2 && instructions[count - 1].id == X86_INS_RET) {

            // Each gadget is identified with its mnemonic
            // and operators (ugly but straightforward :P)
            string asm_instr;
            for (size_t j = 0; j < count - 1; j++) {
              asm_instr += instructions[j].mnemonic;
              asm_instr += " ";
              asm_instr += instructions[j].op_str;
              asm_instr += ";";
            }

            if (!gadgetLookup(asm_instr)) {
              Microgadgets.push_back(Microgadget(instructions, asm_instr));

              cnt++;
            }
          }
        }
      }
    }
    // llvm::dbgs() << cnt << " found!\n";
  }
  delete[] buf;
  input_file.close();
}

Microgadget *BinaryAutopsy::gadgetLookup(string asmInstr) {
  // Legacy: lookup by asm_instr string
  if (Microgadgets.empty()) {
    return nullptr;
  }

  for (auto &g : Microgadgets) {
    if (g.asmInstr.compare(asmInstr) == 0)
      return &g;
  }

  return nullptr;
}

std::vector<Microgadget *>
BinaryAutopsy::gadgetLookup(x86_insn insn, x86_op_type op0, x86_op_type op1) {
  std::vector<Microgadget *> res;

  if (Microgadgets.empty()) {
    return res;
  }

  for (auto &gadget : Microgadgets) {
    if (gadget.getID() == insn && op0 == gadget.getOp(0).type &&
        (op1 == 0 || op1 == gadget.getOp(1).type))
      res.push_back(&gadget);
  }

  return res;
}

std::vector<Microgadget *>
BinaryAutopsy::gadgetLookup(x86_insn insn, x86_reg op0, x86_reg op1) {
  std::vector<Microgadget *> res;

  if (Microgadgets.empty()) {
    return res;
  }

  for (auto &gadget : Microgadgets) {
    // do these checks:
    // - instruction opcodes must be the same
    // - op0 must be a register operand, and must be the same register
    // - if op1 is set, it must be a register operand, and must be the same
    // register; otherwise skip this check (we must do this to deal with
    // gadgets with one operand).

    if (gadget.getID() == insn && gadget.getOp(0).type == X86_OP_REG &&
        gadget.getOp(0).reg == op0 &&
        (op1 == X86_REG_INVALID ||
         (gadget.getOp(1).type == X86_OP_REG && gadget.getOp(1).reg == op1)))

      res.push_back(&gadget);
  }

  return res;
}

void BinaryAutopsy::buildXchgGraph() {
  DEBUG_WITH_TYPE("xchg_graph", llvm::dbgs()
                                    << "[XchgGraph]\t"
                                    << "Building the eXCHanGe Graph ...\n");
  xgraph = XchgGraph();

  // search for all the "xchg reg, reg" gadgets
  auto XchgGadgets = gadgetLookup(X86_INS_XCHG, X86_OP_REG, X86_OP_REG);

  if (XchgGadgets.empty()) {
    DEBUG_WITH_TYPE(XCHG_GRAPH, llvm::dbgs()
                                    << "[XchgGraph]\t"
                                    << "[!] Unable to build the eXCHanGe "
                                       "Graph: no XCHG gadgets found\n");
    return;
  }

  for (auto &g : XchgGadgets) {
    xgraph.addEdge(g->getOp(0).reg, g->getOp(1).reg);

    DEBUG_WITH_TYPE(XCHG_GRAPH, llvm::dbgs()
                                    << "[XchgGraph]\t"
                                    << "Added new edge: " << g->getOp(0).reg
                                    << ", " << g->getOp(1).reg << "\n");
  }
}

void BinaryAutopsy::applyGadgetFilters() {
  int excluded = 0;

  for (auto g = Microgadgets.begin(); g != Microgadgets.end();) {
    if
        // gadgets with ESP as operand, since we cannot deal with the
        // stack pointer using just microgadgets.
        (((g->getOp(0).type == X86_OP_REG && g->getOp(0).reg == X86_REG_ESP) ||
          (g->getOp(1).type == X86_OP_REG && g->getOp(1).reg == X86_REG_ESP) ||
          (g->getOp(0).type == X86_OP_MEM &&
           g->getOp(0).mem.base == X86_REG_ESP) ||
          (g->getOp(1).type == X86_OP_MEM &&
           g->getOp(1).mem.base == X86_REG_ESP)) ||

         // gadgets with memory operands having index and segment
         // registers, or invalid base register
         ((g->getOp(0).type == X86_OP_MEM &&
           (g->getOp(0).mem.base == X86_REG_INVALID ||
            g->getOp(0).mem.index != X86_REG_INVALID ||
            g->getOp(0).mem.segment != X86_REG_INVALID)) ||
          (g->getOp(1).type == X86_OP_MEM &&
           (g->getOp(1).mem.base == X86_REG_INVALID ||
            g->getOp(1).mem.index != X86_REG_INVALID ||
            g->getOp(1).mem.segment != X86_REG_INVALID)))) {

      DEBUG_WITH_TYPE(GADGET_FILTER, llvm::dbgs()
                                         << "[GadgetFilter]\texcluded: "
                                         << g->asmInstr << "\n");
      g = Microgadgets.erase(g);
      excluded++;
    } else {
      ++g;
    }
  }

  DEBUG_WITH_TYPE(GADGET_FILTER, llvm::dbgs() << "[GadgetFilter]\t" << excluded
                                              << " gadgets have been excluded!"
                                              << "\n");
}

bool BinaryAutopsy::checkXchgPath(x86_reg a, x86_reg b, x86_reg c) {
  int pred[N_REGS], dist[N_REGS];
  bool visited[N_REGS];

  // if the third param hasn't been set, check only the first two
  if (c == X86_REG_INVALID)
    return xgraph.checkPath(a, b, pred, dist, visited);

  return (xgraph.checkPath(a, b, pred, dist, visited) &&
          xgraph.checkPath(b, c, pred, dist, visited));
}

bool BinaryAutopsy::checkXchgPath(x86_reg a, vector<x86_reg> B) {
  int pred[N_REGS], dist[N_REGS];
  bool visited[N_REGS];

  for (auto &b : B) {
    if (xgraph.checkPath(a, b, pred, dist, visited))
      return true;
  }

  return false;
}

vector<Microgadget *> BinaryAutopsy::getXchgPath(x86_reg a, x86_reg b) {
  vector<Microgadget *> result;
  XchgPath path = xgraph.getPath(a, b);

  for (auto &edge : path) {
    // even if the XCHG instruction doesn't care about the order of operands, we
    // have to find the right gadget with the same operand order as decoded by
    // capstone.
    auto res = gadgetLookup(X86_INS_XCHG, static_cast<x86_reg>(edge.first),
                            static_cast<x86_reg>(edge.second));
    if (res.empty())
      res = gadgetLookup(X86_INS_XCHG, static_cast<x86_reg>(edge.second),
                         static_cast<x86_reg>(edge.first));

    result.push_back(res.front());
  }

  return result;
}

vector<Microgadget *> BinaryAutopsy::undoXchgs() {
  vector<Microgadget *> result;
  XchgPath path = xgraph.reorderRegisters();

  for (auto &edge : path) {
    // even if the XCHG instruction doesn't care about the order of operands, we
    // have to find the right gadget with the same operand order as decoded by
    // capstone.
    auto res = gadgetLookup(X86_INS_XCHG, static_cast<x86_reg>(edge.first),
                            static_cast<x86_reg>(edge.second));
    if (res.empty())
      res = gadgetLookup(X86_INS_XCHG, static_cast<x86_reg>(edge.second),
                         static_cast<x86_reg>(edge.first));

    result.push_back(res.front());
  }

  return result;
}

ROPChain BinaryAutopsy::initReg(x86_reg dst, unsigned val) {
  ROPChain result;

  // search for "pop DST" gadgets
  std::vector<Microgadget *> gadgets = gadgetLookup(X86_INS_POP, dst);
  if (gadgets.empty()) {
    // TODO
  }

  result.emplace_back(ChainElem(gadgets.front()));
  result.emplace_back(val);

  return result;
}

ROPChain BinaryAutopsy::addRegs(x86_reg dst, x86_reg src) {
  ROPChain result;

  // search for "add DST, SRC" gadgets
  std::vector<Microgadget *> gadgets = gadgetLookup(X86_INS_ADD, dst, src);
  if (gadgets.empty()) {
    // TODO
  }

  result.emplace_back(ChainElem(gadgets.front()));

  return result;
}

ROPChain BinaryAutopsy::calcAddr(x86_reg dst, x86_reg src, unsigned displ) {
  ROPChain result, tmp;
  // TODO: execute in reverse order

  tmp = initReg(dst, displ);
  result.insert(result.end(), tmp.begin(), tmp.end());

  tmp = addRegs(dst, src);
  result.insert(result.end(), tmp.begin(), tmp.end());

  return result;
}

ROPChain BinaryAutopsy::load(x86_reg dst, x86_reg src) {
  ROPChain result;

  // search for "mov DST, [SRC]" gadgets
  std::vector<Microgadget *> gadgets =
      gadgetLookup(X86_INS_MOV, X86_OP_REG, X86_OP_MEM);
  if (!gadgets.empty()) {
    for (auto &g : gadgets) {
      auto op1 = g->getOp(1);

      if (op1.mem.segment == 0 && op1.mem.index == 0 && op1.mem.scale == 1)
        // TODO: xchg
        result.emplace_back(ChainElem(g));
    }
  }

  return result;
}

ROPChain BinaryAutopsy::store(x86_reg dst, x86_reg src) {
  ROPChain result;

  // search for "mov DST, [SRC]" gadgets
  std::vector<Microgadget *> gadgets =
      gadgetLookup(X86_INS_MOV, X86_OP_REG, X86_OP_MEM);
  if (!gadgets.empty()) {
    for (auto &g : gadgets) {
      auto op0 = g->getOp(0);

      if (op0.mem.segment == 0 && op0.mem.index == 0 && op0.mem.scale == 1)
        // TODO: xchg
        result.emplace_back(ChainElem(g));
    }
  }

  return result;
}