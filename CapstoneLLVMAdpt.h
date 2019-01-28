// ==============================================================================
//   Capstone - LLVM IR Adapter
//   part of the ROPfuscator project
// ==============================================================================
// This module exposes a series of helper functions to manipulate capstone
// instruction structs in a simpler way, like they were instructions of the LLVM
// IR.

#include <capstone/capstone.h>
#include <capstone/x86.h>

// opValid - checks if the operands has the "type" defined. This is used
// in gadgetLookup() to figure out if the optional parameter has been set.
bool opValid(cs_x86_op op);

// opCompare - compares two operands taking into account their types
bool opCompare(cs_x86_op a, cs_x86_op b);

// opCreate - creates an operand from scratch.
cs_x86_op opCreate(x86_op_type type, unsigned int value);