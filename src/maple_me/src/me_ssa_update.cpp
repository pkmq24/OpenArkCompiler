/*
 * Copyright (c) [2019] Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
 */
#include "me_ssa_update.h"

// Create or update HSSA representation for variables given by *updateCands;
// for each variable, the mapped bb set gives the bbs that have newly isnerted
// dassign's to the variable.
// If some assignments have been deleted, the current implementation does not
// delete useless phi's, and these useless phi's may end up hving identical
// phi operands.
namespace maple {
// accumulate the BBs that are in the iterated dominance frontiers of bb in
// the set dfSet, visiting each BB only once
void MeSSAUpdate::GetIterDomFrontier(const BB &bb, MapleSet<BBId> &dfSet, std::vector<bool> &visitedMap) {
  CHECK_FATAL(bb.GetBBId().idx < visitedMap.size(), "index out of range in MeSSAUpdate::GetIterDomFrontier");
  if (visitedMap[bb.GetBBId().idx]) {
    return;
  }
  visitedMap[bb.GetBBId().idx] = true;
  for (BBId frontierBBId : dom.GetDomFrontier(bb.GetBBId().idx)) {
    dfSet.insert(frontierBBId);
    BB *frontierBB = func.GetBBFromID(frontierBBId);
    GetIterDomFrontier(*frontierBB, dfSet, visitedMap);
  }
}

void MeSSAUpdate::InsertPhis() {
  MapleMap<OStIdx, MapleSet<BBId>*>::iterator it = updateCands.begin();
  MapleSet<BBId> dfSet(std::less<BBId>(), ssaUpdateAlloc.Adapter());
  for (; it != updateCands.end(); it++) {
    std::vector<bool> visitedMap(func.GetAllBBs().size(), false);
    dfSet.clear();
    for (BBId bbId : *it->second) {
      GetIterDomFrontier(*func.GetBBFromID(bbId), dfSet, visitedMap);
    }
    for (BBId bbId : dfSet) {
      // insert a phi node
      BB *bb = func.GetBBFromID(bbId);
      ASSERT(bb != nullptr, "null ptr check");
      MapleMap<OStIdx, MeVarPhiNode*>::iterator phiListIt = bb->GetMevarPhiList().find(it->first);
      if (phiListIt != bb->GetMevarPhiList().end()) {
        phiListIt->second->SetIsLive(true);
        continue;
      }
      MeVarPhiNode *phiMeNode = irMap.NewInPool<MeVarPhiNode>();
      phiMeNode->SetDefBB(bb);
      phiMeNode->GetOpnds().resize(bb->GetPred().size());
      bb->GetMevarPhiList().insert(std::make_pair(it->first, phiMeNode));
    }
    // initialize its rename stack
    renameStacks[it->first] = ssaUpdateMp.New<MapleStack<VarMeExpr*>>(ssaUpdateAlloc.Adapter());
  }
}

void MeSSAUpdate::RenamePhi(BB &bb) {
  MapleMap<OStIdx, MapleStack<VarMeExpr*>*>::iterator it1 = renameStacks.begin();
  for (; it1 != renameStacks.end(); ++it1) {
    MapleMap<OStIdx, MeVarPhiNode*>::iterator it2 = bb.GetMevarPhiList().find(it1->first);
    if (it2 != bb.GetMevarPhiList().end()) {
      // if there is existing phi result node
      MeVarPhiNode *phi = it2->second;
      phi->SetIsLive(true);  // always make it live, for correctness
      if (phi->GetLHS() == nullptr) {
        // create a new VarMeExpr defined by this phi
        irMap.SetExprID(irMap.GetExprID() + 1);
        VarMeExpr *newVar = irMap.New<VarMeExpr>(&irMap.GetIRMapAlloc(), irMap.GetExprID(), it2->first,
                                                 irMap.GetVerst2MeExprTableSize());
        newVar->InitBase(OP_dread, PTY_ref, 0);
        newVar->SetFieldID(0);
        irMap.PushBackVerst2MeExprTable(newVar);
        CHECK_FATAL(newVar->GetVstIdx() == irMap.GetVerst2MeExprTableSize() - 1,
                    "RenamePhi: vstIdx wrongly initialized");
        phi->UpdateLHS(*newVar);
        it1->second->push(newVar);  // push the stack
      } else {
        it1->second->push(phi->GetLHS());  // push the stack
      }
    }
  }
}

// changed has been initialized to false by caller
MeExpr *MeSSAUpdate::RenameExpr(MeExpr &meExpr, bool &changed) {
  bool needRehash = false;
  switch (meExpr.GetMeOp()) {
    case kMeOpVar: {
      auto &varExpr = static_cast<VarMeExpr&>(meExpr);
      MapleMap<OStIdx, MapleStack<VarMeExpr*>*>::iterator it1 = renameStacks.find(varExpr.GetOStIdx());
      if (it1 == renameStacks.end()) {
        return &meExpr;
      }
      MapleStack<VarMeExpr*> *renameStack = it1->second;
      VarMeExpr *curVar = renameStack->top();
      if (&varExpr == curVar) {
        return &meExpr;
      }
      changed = true;
      return curVar;
    }
    case kMeOpIvar: {
      auto &ivarMeExpr = static_cast<IvarMeExpr&>(meExpr);
      IvarMeExpr newMeExpr(kInvalidExprID);
      newMeExpr.SetBase(RenameExpr(*ivarMeExpr.GetBase(), needRehash));
      if (needRehash) {
        changed = true;
        newMeExpr.SetFieldID(ivarMeExpr.GetFieldID());
        newMeExpr.SetTyIdx(ivarMeExpr.GetTyIdx());
        newMeExpr.InitBase(ivarMeExpr.GetOp(), ivarMeExpr.GetPrimType(), ivarMeExpr.GetNumOpnds());
        newMeExpr.SetMuVal(ivarMeExpr.GetMu());
        return irMap.HashMeExpr(newMeExpr);
      }
      return &meExpr;
    }
    case kMeOpOp: {
      auto &meOpExpr = static_cast<OpMeExpr&>(meExpr);
      OpMeExpr newMeExpr(kInvalidExprID);
      newMeExpr.SetOpnd(0, RenameExpr(*meOpExpr.GetOpnd(0), needRehash));
      if (meOpExpr.GetOpnd(1) != nullptr) {
        newMeExpr.SetOpnd(1, RenameExpr(*meOpExpr.GetOpnd(1), needRehash));
        if (meOpExpr.GetOpnd(2) != nullptr) {
          newMeExpr.SetOpnd(2, RenameExpr(*meOpExpr.GetOpnd(2), needRehash));
        }
      }
      if (needRehash) {
        changed = true;
        newMeExpr.SetOpndType(meOpExpr.GetOpndType());
        newMeExpr.SetBitsOffSet(meOpExpr.GetBitsOffSet());
        newMeExpr.SetBitsSize(meOpExpr.GetBitsSize());
        newMeExpr.SetTyIdx(meOpExpr.GetTyIdx());
        newMeExpr.SetFieldID(meOpExpr.GetFieldID());
        newMeExpr.InitBase(meOpExpr.GetOp(), meOpExpr.GetPrimType(), meOpExpr.GetNumOpnds());
        return irMap.HashMeExpr(newMeExpr);
      }
      return &meExpr;
    }
    case kMeOpNary: {
      auto &naryMeExpr = static_cast<NaryMeExpr&>(meExpr);
      NaryMeExpr newMeExpr(&irMap.GetIRMapAlloc(), kInvalidExprID, naryMeExpr.GetTyIdx(), naryMeExpr.GetIntrinsic(),
                           naryMeExpr.GetBoundCheck());
      for (size_t i = 0; i < naryMeExpr.GetOpnds().size(); i++) {
        newMeExpr.GetOpnds().push_back(RenameExpr(*naryMeExpr.GetOpnd(i), needRehash));
      }
      if (needRehash) {
        changed = true;
        newMeExpr.InitBase(meExpr.GetOp(), meExpr.GetPrimType(), meExpr.GetNumOpnds());
        return irMap.HashMeExpr(newMeExpr);
      }
      return &meExpr;
    }
    default:
      return &meExpr;
  }
}

void MeSSAUpdate::RenameStmts(BB &bb) {
  for (auto &stmt : bb.GetMeStmts()) {
    // rename the expressions
    bool changed = false;
    for (size_t i = 0; i < stmt.NumMeStmtOpnds(); i++) {
      stmt.SetOpnd(i, RenameExpr(*stmt.GetOpnd(i), changed /* dummy */));
    }
    // process mayDef
    MapleMap<OStIdx, ChiMeNode*> *chiList = stmt.GetChiList();
    if (chiList != nullptr) {
      for (auto chi : *chiList) {
        MapleMap<OStIdx, MapleStack<VarMeExpr*>*>::iterator it = renameStacks.find(chi.first);
        if (it != renameStacks.end() && chi.second != nullptr) {
          it->second->push(chi.second->GetLHS());
        }
      }
    }
    // process the LHS
    VarMeExpr *lhsVar = nullptr;
    if (stmt.GetOp() == OP_dassign || stmt.GetOp() == OP_maydassign) {
      lhsVar = stmt.GetVarLHS();
    } else if (kOpcodeInfo.IsCallAssigned(stmt.GetOp())) {
      MapleVector<MustDefMeNode> *mustDefList = stmt.GetMustDefList();
      if (mustDefList->empty() || mustDefList->front().GetLHS()->GetMeOp() != kMeOpVar) {
        continue;
      }
      lhsVar = static_cast<VarMeExpr*>(mustDefList->front().GetLHS());
    } else {
      continue;
    }
    CHECK_FATAL(lhsVar != nullptr, "stmt doesn't have lhs?");
    MapleMap<OStIdx, MapleStack<VarMeExpr*>*>::iterator it = renameStacks.find(lhsVar->GetOStIdx());
    if (it == renameStacks.end()) {
      continue;
    }
    MapleStack<VarMeExpr*> *renameStack = it->second;
    renameStack->push(lhsVar);
  }
}

void MeSSAUpdate::RenamePhiOpndsInSucc(BB &bb) {
  for (BB *succ : bb.GetSucc()) {
    // find index of bb in succ_bb->pred_[]
    size_t index = 0;
    while (index < succ->GetPred().size()) {
      if (succ->GetPred(index) == &bb) {
        break;
      }
      index++;
    }
    CHECK_FATAL(index < succ->GetPred().size(), "RenamePhiOpndsinSucc: cannot find corresponding pred");
    MapleMap<OStIdx, MapleStack<VarMeExpr*>*>::iterator it1 = renameStacks.begin();
    for (; it1 != renameStacks.end(); it1++) {
      MapleMap<OStIdx, MeVarPhiNode*>::iterator it2 = succ->GetMevarPhiList().find(it1->first);
      if (it2 == succ->GetMevarPhiList().end()) {
        continue;
      }
      MeVarPhiNode *phi = it2->second;
      MapleStack<VarMeExpr*> *renameStack = it1->second;
      VarMeExpr *curVar = renameStack->top();
      if (phi->GetOpnd(index) != curVar) {
        phi->SetOpnd(index, curVar);
      }
    }
  }
}

void MeSSAUpdate::RenameBB(BB &bb) {
  // for recording stack height on entering this BB, to pop back to same height
  // when backing up the dominator tree
  std::map<OStIdx, uint32> origStackSize((std::less<OStIdx>()));
  MapleMap<OStIdx, MapleStack<VarMeExpr*>*>::iterator it = renameStacks.begin();
  for (; it != renameStacks.end(); ++it) {
    origStackSize[it->first] = it->second->size();
  }
  RenamePhi(bb);
  RenameStmts(bb);
  RenamePhiOpndsInSucc(bb);
  // recurse down dominator tree in pre-order traversal
  const MapleSet<BBId> &children = dom.GetDomChildren(bb.GetBBId().idx);
  for (const BBId &child : children) {
    RenameBB(*func.GetBBFromID(child));
  }
  // pop stacks back to where they were at entry to this BB
  it = renameStacks.begin();
  for (; it != renameStacks.end(); ++it) {
    while (it->second->size() > origStackSize[it->first]) {
      it->second->pop();
    }
  }
}

void MeSSAUpdate::Run() {
  InsertPhis();
  // push zero-version varmeexpr nodes to rename stacks
  MapleMap<OStIdx, MapleStack<VarMeExpr*>*>::iterator it = renameStacks.begin();
  for (; it != renameStacks.end(); it++) {
    const OriginalSt *ost = ssaTab.GetSymbolOriginalStFromID(it->first);
    VarMeExpr *zeroVersVar = irMap.GetOrCreateZeroVersionVarMeExpr(*ost);
    MapleStack<VarMeExpr*> *renameStack = it->second;
    renameStack->push(zeroVersVar);
  }
  // recurse down dominator tree in pre-order traversal
  const MapleSet<BBId> &children = dom.GetDomChildren(func.GetCommonEntryBB()->GetBBId().idx);
  for (const BBId &child : children) {
    RenameBB(*func.GetBBFromID(child));
  }
}
}  // namespace maple
