// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Add temporaries, such as for inline nodes
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
// V3Inline's Transformations:
//
// Each module:
//	Look for CELL... PRAGMA INLINE_MODULE
//	    Replicate the cell's module
//	        Convert pins to wires that make assignments
//		Rename vars to include cell name
//	    Insert cell's module statements into the upper module
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <algorithm>
#include <vector>

#include "V3Global.h"
#include "V3Inline.h"
#include "V3Inst.h"
#include "V3Stats.h"
#include "V3Ast.h"

//######################################################################
// Inline state, as a visitor of each AstNode

// CONFIG
static const int INLINE_MODS_SMALLER = 100;	// If a mod is < this # nodes, can always inline it

class InlineVisitor : public AstNVisitor {
private:
    // NODE STATE
    // Cleared entire netlist
    //  Input:
    //   AstNodeModule::user1p()	// bool. True to inline this module (from InlineMarkVisitor)
    // Cleared each cell
    //   AstVar::user2p()	// AstVarRef*/AstConst*  Points to signal this is a direct connect to
    //   AstVar::user3()	// bool    Don't alias the user4, keep it as signal

    // STATE
    AstNodeModule*	m_modp;		// Current module
    AstCell*		m_cellp;	// Cell being cloned
    V3Double0		m_statCells;	// Statistic tracking

    static int debug() {
	static int level = -1;
	if (VL_UNLIKELY(level < 0)) level = v3Global.opt.debugSrcLevel(__FILE__);
	return level;
    }

    // VISITORS
    virtual void visit(AstNetlist* nodep, AstNUser*) {
	// Iterate modules backwards, in bottom-up order.  Required!
	nodep->iterateChildrenBackwards(*this);
    }
    virtual void visit(AstNodeModule* nodep, AstNUser*) {
	if (m_cellp) {
	} else {
	    m_modp = nodep;
	}
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstCellInline* nodep, AstNUser*) {
	// Inlined cell under the inline cell, need to move to avoid conflicts
	if (m_cellp) {
	    nodep->unlinkFrBack();
	    m_modp->addInlinesp(nodep);
	    // Rename
	    string name = m_cellp->name() + "__DOT__" + nodep->name();
	    nodep->name(name);
	    UINFO(6, "    Inline "<<nodep<<endl);
	    // Do CellInlines under this, but don't move them
	    nodep->iterateChildren(*this);
	}
    }
    virtual void visit(AstCell* nodep, AstNUser*) {
	if (m_cellp) {
	    // Cell under the inline cell, need to rename to avoid conflicts
	    string name = m_cellp->name() + "__DOT__" + nodep->name();
	    nodep->name(name);
	    nodep->iterateChildren(*this);
	}
	if (nodep->modp()->user1()) {  // Marked with inline request
	    if (m_cellp) nodep->v3error("Cloning should have already been done bottom-up");
	    UINFO(5," Inline CELL   "<<nodep<<endl);
	    UINFO(5,"   To MOD      "<<m_modp<<endl);
	    ++m_statCells;

	    // Before cloning simplify pin assignments
	    // Better off before, as if module has multiple instantiations
	    // we'll save work, and we can't call pinReconnectSimple in
	    // this loop as it clone()s itself.
	    for (AstPin* pinp = nodep->pinsp(); pinp; pinp=pinp->nextp()->castPin()) {
		if (!pinp->exprp()) continue;
		V3Inst::pinReconnectSimple(pinp, nodep, m_modp, false);
	    }

	    // Clone original module
	    if (debug()>=9) { nodep->dumpTree(cout,"inlcell:"); }
	    //if (debug()>=9) { nodep->modp()->dumpTree(cout,"oldmod:"); }
	    AstNodeModule* newmodp = nodep->modp()->cloneTree(false);
	    if (debug()>=9) { newmodp->dumpTree(cout,"newmod:"); }
	    // Clear var markings
	    AstNode::user2ClearTree();
	    // Create data for dotted variable resolution
	    AstCellInline* inlinep = new AstCellInline(nodep->fileline(),
						       nodep->name(), nodep->modp()->origName());
	    m_modp->addInlinesp(inlinep);  // Must be parsed before any AstCells
	    // Create assignments to the pins
	    for (AstPin* pinp = nodep->pinsp(); pinp; pinp=pinp->nextp()->castPin()) {
		if (!pinp->exprp()) continue;
		UINFO(6,"     Pin change from "<<pinp->modVarp()<<endl);
		// Make new signal; even though we'll optimize the interconnect, we
		// need an alias to trace correctly.  If tracing is disabled, we'll
		// delete it in later optimizations.
		AstVar* pinOldVarp = pinp->modVarp();
		AstVar* pinNewVarp = pinOldVarp->clonep()->castVar();

		AstNode* connectRefp = pinp->exprp();
		if (!connectRefp->castConst() && !connectRefp->castVarRef()) {
		    pinp->v3fatalSrc("Unknown interconnect type; pinReconnectSimple should have cleared up\n");
		}
		if (pinNewVarp->isOutOnly() && connectRefp->castConst()) {
		    pinp->v3error("Output port is connected to a constant pin, electrical short");
		}

		// Propagate any attributes across the interconnect
		pinNewVarp->propagateAttrFrom(pinOldVarp);
		if (connectRefp->castVarRef()) {
		    connectRefp->castVarRef()->varp()->propagateAttrFrom(pinOldVarp);
		}

		// One to one interconnect won't make a temporary variable.
		// This prevents creating a lot of extra wires for clock signals.
		// It will become a tracing alias.
		UINFO(6,"One-to-one "<<connectRefp<<endl);
		UINFO(6,"       -to "<<pinNewVarp<<endl);
		pinNewVarp->user2p(connectRefp);
		// Public output inside the cell must go via an assign rather than alias
		// Else the public logic will set the alias, loosing the value to be propagated up
		// (InOnly isn't a problem as the AssignAlias will create the assignment for us)
		pinNewVarp->user3(pinNewVarp->isSigUserRWPublic() && pinNewVarp->isOutOnly());
	    }
	    // Cleanup var names, etc, to not conflict
	    m_cellp = nodep;
	    newmodp->iterate(*this);   // Not iterateAndNext because newmodp isn't linked; no back
	    m_cellp = NULL;
	    // Move statements to top module
	    if (debug()>=9) { newmodp->dumpTree(cout,"fixmod:"); }
	    AstNode* stmtsp = newmodp->stmtsp();
	    if (stmtsp) stmtsp->unlinkFrBackWithNext();
	    if (stmtsp) m_modp->addStmtp(stmtsp);
	    // Remove the cell
	    newmodp->deleteTree(); newmodp=NULL; // Clear any leftover ports, etc
	    nodep->unlinkFrBack();
	    pushDeletep(nodep); nodep = NULL;
	    if (debug()>=9) { m_modp->dumpTree(cout,"donemod:"); }
	}
    }
    virtual void visit(AstVar* nodep, AstNUser*) {
	if (m_cellp) {
	    if (nodep->user2p()) {
		// Make an assignment, so we'll trace it properly
		// user2p is either a const or a var.
		AstConst*  exprconstp  = nodep->user2p()->castNode()->castConst();
		AstVarRef* exprvarrefp = nodep->user2p()->castNode()->castVarRef();
		UINFO(8,"connectto: "<<nodep->user2p()->castNode()<<endl);
		if (!exprconstp && !exprvarrefp) {
		    nodep->v3fatalSrc("Unknown interconnect type; pinReconnectSimple should have cleared up\n");
		}
		if (exprconstp) {
		    m_modp->addStmtp(new AstAssignW(nodep->fileline(),
						    new AstVarRef(nodep->fileline(), nodep, true),
						    exprconstp->cloneTree(true)));
		} else if (nodep->user3()) {
		    // Public variable at the lower module end - we need to make sure we propagate
		    // the logic changes up and down; if we aliased, we might remove the change detection
		    // on the output variable.
		    UINFO(9,"public pin assign: "<<exprvarrefp<<endl);
		    if (nodep->isInput()) nodep->v3fatalSrc("Outputs only - inputs use AssignAlias");
		    m_modp->addStmtp(new AstAssignW(nodep->fileline(),
						    new AstVarRef(nodep->fileline(), exprvarrefp->varp(), true),
						    new AstVarRef(nodep->fileline(), nodep, false)));
		} else {
		    m_modp->addStmtp(new AstAssignAlias(nodep->fileline(),
							new AstVarRef(nodep->fileline(), nodep, true),
							new AstVarRef(nodep->fileline(), exprvarrefp->varp(), false)));
		    AstNode* nodebp=exprvarrefp->varp();
		    nodep ->fileline()->modifyStateInherit(nodebp->fileline());
		    nodebp->fileline()->modifyStateInherit(nodep ->fileline());
		}
	    }
	    // Variable under the inline cell, need to rename to avoid conflicts
	    // Also clear I/O bits, as it is now local.
	    string name = m_cellp->name() + "__DOT__" + nodep->name();
	    if (!nodep->isFuncLocal()) nodep->inlineAttrReset(name);
	    if (debug()>=9) { nodep->dumpTree(cout,"varchanged:"); }
	    if (debug()>=9) { nodep->valuep()->dumpTree(cout,"varchangei:"); }
	}
	if (nodep) nodep->iterateChildren(*this);
    }
    virtual void visit(AstNodeFTask* nodep, AstNUser*) {
	if (m_cellp) {
	    // Function under the inline cell, need to rename to avoid conflicts
	    nodep->name(m_cellp->name() + "__DOT__" + nodep->name());
	}
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstTypedef* nodep, AstNUser*) {
	if (m_cellp) {
	    // Typedef under the inline cell, need to rename to avoid conflicts
	    nodep->name(m_cellp->name() + "__DOT__" + nodep->name());
	}
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstVarRef* nodep, AstNUser*) {
	if (m_cellp) {
	    if (nodep->varp()->user2p()  // It's being converted to an alias.
		&& !nodep->varp()->user3()
		&& !nodep->backp()->castAssignAlias()) { 	// Don't constant propagate aliases (we just made)
		AstConst*  exprconstp  = nodep->varp()->user2p()->castNode()->castConst();
		AstVarRef* exprvarrefp = nodep->varp()->user2p()->castNode()->castVarRef();
		if (exprconstp) {
		    nodep->replaceWith(exprconstp->cloneTree(true));
		    nodep->deleteTree(); nodep=NULL;
		    return;
		}
		else if (exprvarrefp) {
		    nodep->varp( exprvarrefp->varp() );
		}
		else {
		    nodep->v3fatalSrc("Null connection?\n");
		}
	    }
	    nodep->name(nodep->varp()->name());
	}
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstVarXRef* nodep, AstNUser*) {
	if (m_cellp) {
	    // Track what scope it was originally under so V3LinkDot can resolve it
	    string newname = m_cellp->name();
	    if (nodep->inlinedDots() != "") { newname += "." + nodep->inlinedDots(); }
	    nodep->inlinedDots(newname);
	    UINFO(8,"   "<<nodep<<endl);
	}
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstNodeFTaskRef* nodep, AstNUser*) {
	if (m_cellp) {
	    // Track what scope it was originally under so V3LinkDot can resolve it
	    string newname = m_cellp->name();
	    if (nodep->inlinedDots() != "") { newname += "." + nodep->inlinedDots(); }
	    nodep->inlinedDots(newname);
	    UINFO(8,"   "<<nodep<<endl);
	}
	nodep->iterateChildren(*this);
    }

    // Not needed, as V3LinkDot doesn't care about typedefs
    //virtual void visit(AstRefDType* nodep, AstNUser*) {}

    virtual void visit(AstScopeName* nodep, AstNUser*) {
	// If there's a %m in the display text, we add a special node that will contain the name()
	// Similar code in V3Begin
	if (m_cellp) {
	    // To keep correct visual order, must add before other Text's
	    AstNode* afterp = nodep->scopeAttrp();
	    if (afterp) afterp->unlinkFrBackWithNext();
	    nodep->scopeAttrp(new AstText(nodep->fileline(), (string)"__DOT__"+m_cellp->name()));
	    if (afterp) nodep->scopeAttrp(afterp);
	}
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstCoverDecl* nodep, AstNUser*) {
	// Fix path in coverage statements
	if (m_cellp) {
	    nodep->hier(m_cellp->prettyName()
			+ (nodep->hier()!="" ? ".":"")
			+ nodep->hier());
	}
	nodep->iterateChildren(*this);
    }

    //--------------------
    // Default: Just iterate
    virtual void visit(AstNode* nodep, AstNUser*) {
	nodep->iterateChildren(*this);
    }

public:
    // CONSTUCTORS
    InlineVisitor(AstNode* nodep) {
	m_cellp = NULL;
	m_modp = NULL;
	nodep->accept(*this);
    }
    virtual ~InlineVisitor() {
	V3Stats::addStat("Optimizations, Inlined cells", m_statCells);
    }
};

//######################################################################
// Inline state, as a visitor of each AstNode

class InlineMarkVisitor : public AstNVisitor {
private:
    // NODE STATE
    // Entire netlist
    //  AstNodeModule::user1()	// OUTPUT: bool. User request to inline this module
    //  AstNodeModule::user2()	// bool. Allowed to automatically inline module
    //  AstNodeModule::user3()	// int. Number of cells referencing this module
    AstUser1InUse	m_inuser1;
    AstUser2InUse	m_inuser2;
    AstUser3InUse	m_inuser3;

    // STATE
    AstNodeModule*	m_modp;		// Current module
    int			m_stmtCnt;	// Statements in module

    // METHODS
    void cantInline(const char* reason) {
	if (m_modp->user2()) {
	    UINFO(4,"  No inline: "<<reason<<" "<<m_modp<<endl);
	    m_modp->user2(false);
	}
    }

    // VISITORS
    virtual void visit(AstNodeModule* nodep, AstNUser*) {
	m_stmtCnt = 0;
	m_modp = nodep;
	m_modp->user2(true);  // Allowed = true
	if (m_modp->modPublic()) cantInline("modPublic");
	//
	nodep->iterateChildren(*this);
	//
	bool userinline = nodep->user1();
	bool allowed = nodep->user2();
	int refs = nodep->user3();
	// Should we automatically inline this module?
	// inlineMult = 2000 by default.  If a mod*#instances is < this # nodes, can inline it
	bool doit = (userinline || (allowed && (refs==1
						|| m_stmtCnt < INLINE_MODS_SMALLER
						|| v3Global.opt.inlineMult() < 1
						|| refs*m_stmtCnt < v3Global.opt.inlineMult())));
	// Packages aren't really "under" anything so they confuse this algorithm
	if (nodep->castPackage()) doit = false;
	UINFO(4, " Inline="<<doit<<" Possible="<<allowed<<" Usr="<<userinline<<" Refs="<<refs<<" Stmts="<<m_stmtCnt
	      <<"  "<<nodep<<endl);
	if (doit) {
	    UINFO(4," AutoInline "<<nodep<<endl);
	    nodep->user1(true);
	}
	m_modp = NULL;
    }
    virtual void visit(AstCell* nodep, AstNUser*) {
	nodep->modp()->user3Inc();
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstPragma* nodep, AstNUser*) {
	if (nodep->pragType() == AstPragmaType::INLINE_MODULE) {
	    //UINFO(0,"PRAG MARK "<<m_modp<<endl);
	    if (!m_modp) {
		nodep->v3error("Inline pragma not under a module");
	    } else {
		m_modp->user1(1);
	    }
	    nodep->unlinkFrBack()->deleteTree(); nodep=NULL;  // Remove so don't propagate to upper cell...
	} else if (nodep->pragType() == AstPragmaType::NO_INLINE_MODULE) {
	    if (!m_modp) {
		nodep->v3error("Inline pragma not under a module");
	    } else {
		cantInline("Pragma NO_INLINE_MODULE");
	    }
	    nodep->unlinkFrBack()->deleteTree(); nodep=NULL;  // Remove so don't propagate to upper cell...
	} else {
	    nodep->iterateChildren(*this);
	}
    }
    virtual void visit(AstVarXRef* nodep, AstNUser*) {
	// Cleanup link until V3LinkDot can correct it
	nodep->varp(NULL);
    }
    virtual void visit(AstNodeFTaskRef* nodep, AstNUser*) {
	// Cleanup link until V3LinkDot can correct it
	if (!nodep->packagep()) nodep->taskp(NULL);
	nodep->iterateChildren(*this);
    }
    // Nop's to speed up the loop
    virtual void visit(AstAlways* nodep, AstNUser*) {
	nodep->iterateChildren(*this);
	m_stmtCnt++;
    }
    virtual void visit(AstNodeAssign* nodep, AstNUser*) {
	// Don't count assignments, as they'll likely flatten out
	// Still need to iterate though to nullify VarXRefs
	int oldcnt = m_stmtCnt;
	nodep->iterateChildren(*this);
	m_stmtCnt = oldcnt;
    }
    //--------------------
    // Default: Just iterate
    virtual void visit(AstNode* nodep, AstNUser*) {
	nodep->iterateChildren(*this);
	m_stmtCnt++;
    }

public:
    // CONSTUCTORS
    InlineMarkVisitor(AstNode* nodep) {
	m_modp = NULL;
	m_stmtCnt = 0;
	//VV*****  We reset all userp() on the whole netlist!!!
	AstNode::user1ClearTree();
	AstNode::user2ClearTree();
	AstNode::user3ClearTree();
	nodep->accept(*this);
    }
    virtual ~InlineMarkVisitor() {}
};

//######################################################################
// Inline class functions

void V3Inline::inlineAll(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    InlineMarkVisitor mvisitor (nodep);
    InlineVisitor visitor (nodep);
    // Remove all modules that were inlined
    // V3Dead will also clean them up, but if we have debug on, it's a good
    // idea to avoid dumping the hugely exploded tree.
    AstNodeModule* nextmodp;
    for (AstNodeModule* modp = v3Global.rootp()->modulesp(); modp; modp=nextmodp) {
	nextmodp = modp->nextp()->castNodeModule();
	if (modp->user1()) { // Was inlined
	    modp->unlinkFrBack()->deleteTree(); modp=NULL;
	}
    }
}
