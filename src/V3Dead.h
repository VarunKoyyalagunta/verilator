// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Dead branch elimination
//
// Code available from: http://www.veripool.org/verilator
//
//*************************************************************************
//
// Copyright 2003-2013 by Wilson Snyder.  This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
//
// Verilator is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//*************************************************************************

#ifndef _V3DEAD_H_
#define _V3DEAD_H_ 1
#include "config_build.h"
#include "verilatedos.h"
#include "V3Error.h"
#include "V3Ast.h"

//============================================================================

class V3Dead {
public:
    // Modules, no vars/dtypes
    static void deadifyModules(AstNetlist* nodep);
    // Modules, Data types
    static void deadifyDTypes(AstNetlist* nodep);
    // Everything that's possible
    static void deadifyAll(AstNetlist* nodep);
};

#endif // Guard
