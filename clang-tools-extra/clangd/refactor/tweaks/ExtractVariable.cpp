//===--- ExtractVariable.cpp ------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "AST.h"
#include "ParsedAST.h"
#include "Protocol.h"
#include "Selection.h"
#include "SourceCode.h"
#include "refactor/Tweak.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/LambdaCapture.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {
namespace clangd {
namespace {
// information regarding the Expr that is being extracted
class ExtractionContext {
public:
  ExtractionContext(const SelectionTree::Node *Node, const SourceManager &SM,
                    const ASTContext &Ctx);
  const clang::Expr *getExpr() const { return Expr; }
  const SelectionTree::Node *getExprNode() const { return ExprNode; }
  bool isExtractable() const { return Extractable; }
  // The half-open range for the expression to be extracted.
  SourceRange getExtractionChars() const;
  // Generate Replacement for replacing selected expression with given VarName
  tooling::Replacement replaceWithVar(SourceRange Chars,
                                      llvm::StringRef VarName) const;
  // Generate Replacement for declaring the selected Expr as a new variable
  tooling::Replacement insertDeclaration(llvm::StringRef VarName,
                                         SourceRange InitChars,
                                         bool AddSemicolon) const;

private:
  bool Extractable = false;
  const clang::Expr *Expr;
  QualType VarType;
  const SelectionTree::Node *ExprNode;
  // Stmt before which we will extract
  const clang::Stmt *InsertionPoint = nullptr;
  const SourceManager &SM;
  const ASTContext &Ctx;
  // Decls referenced in the Expr
  std::vector<clang::Decl *> ReferencedDecls;
  // returns true if the Expr doesn't reference any variable declared in scope
  bool exprIsValidOutside(const clang::Stmt *Scope) const;
  // computes the Stmt before which we will extract out Expr
  const clang::Stmt *computeInsertionPoint() const;
};

// Returns all the Decls referenced inside the given Expr
static std::vector<clang::Decl *>
computeReferencedDecls(const clang::Expr *Expr) {
  // RAV subclass to find all DeclRefs in a given Stmt
  class FindDeclRefsVisitor
      : public clang::RecursiveASTVisitor<FindDeclRefsVisitor> {
  public:
    std::vector<Decl *> ReferencedDecls;
    bool VisitDeclRefExpr(DeclRefExpr *DeclRef) { // NOLINT
      // Stop the call operator of lambdas from being marked as a referenced
      // DeclRefExpr in immediately invoked lambdas.
      if (const auto *const Method =
              llvm::dyn_cast<CXXMethodDecl>(DeclRef->getDecl());
          Method != nullptr && Method->getParent()->isLambda()) {
        return true;
      }
      ReferencedDecls.push_back(DeclRef->getDecl());
      return true;
    }

    // Local variables declared inside of the selected lambda cannot go out of
    // scope. The DeclRefExprs that are important are the variables captured,
    // the DeclRefExprs inside the initializers of init-capture variables,
    // variables mentioned in trailing return types, constraints and explicit
    // defaulted template parameters.
    bool TraverseLambdaExpr(LambdaExpr *LExpr) {
      for (const auto &[Capture, Initializer] :
           llvm::zip(LExpr->captures(), LExpr->capture_inits())) {
        TraverseLambdaCapture(LExpr, &Capture, Initializer);
      }

      if (const clang::Expr *RequiresClause =
              LExpr->getTrailingRequiresClause().ConstraintExpr) {
        TraverseStmt(const_cast<clang::Expr *>(RequiresClause));
      }

      for (auto *const TemplateParam : LExpr->getExplicitTemplateParameters())
        TraverseDecl(TemplateParam);

      if (auto *const CallOperator = LExpr->getCallOperator()) {
        TraverseType(CallOperator->getDeclaredReturnType());

        for (auto *const Param : CallOperator->parameters()) {
          TraverseParmVarDecl(Param);
        }

        for (auto *const Attr : CallOperator->attrs()) {
          TraverseAttr(Attr);
        }
      }

      return true;
    }
  };

  FindDeclRefsVisitor Visitor;
  Visitor.TraverseStmt(const_cast<Stmt *>(cast<Stmt>(Expr)));
  return Visitor.ReferencedDecls;
}

static QualType computeVariableType(const Expr *Expr, const ASTContext &Ctx) {
  if (Ctx.getLangOpts().CPlusPlus11)
    return Ctx.getAutoDeductType();

  if (Expr->hasPlaceholderType(BuiltinType::PseudoObject)) {
    if (const auto *PR = dyn_cast<ObjCPropertyRefExpr>(Expr)) {
      if (PR->isMessagingSetter()) {
        // Don't support extracting a compound reference like `self.prop += 1`
        // since the meaning changes after extraction since we'll no longer call
        // the setter. Non compound access like `self.prop = 1` is invalid since
        // it returns nil (setter method must have a void return type).
        return QualType();
      } else if (PR->isMessagingGetter()) {
        if (PR->isExplicitProperty())
          return PR->getExplicitProperty()->getType();
        else
          return PR->getImplicitPropertyGetter()->getReturnType();
      }
    } else {
      return QualType();
    }
  }
  return Expr->getType();
}

ExtractionContext::ExtractionContext(const SelectionTree::Node *Node,
                                     const SourceManager &SM,
                                     const ASTContext &Ctx)
    : ExprNode(Node), SM(SM), Ctx(Ctx) {
  Expr = Node->ASTNode.get<clang::Expr>();
  ReferencedDecls = computeReferencedDecls(Expr);
  InsertionPoint = computeInsertionPoint();
  if (InsertionPoint)
    Extractable = true;
  VarType = computeVariableType(Expr, Ctx);
  if (VarType.isNull())
    Extractable = false;
  else
    // Strip the outer nullability since it's not common for local variables.
    AttributedType::stripOuterNullability(VarType);
}

// checks whether extracting before InsertionPoint will take a
// variable reference out of scope
bool ExtractionContext::exprIsValidOutside(const clang::Stmt *Scope) const {
  SourceLocation ScopeBegin = Scope->getBeginLoc();
  SourceLocation ScopeEnd = Scope->getEndLoc();
  for (const Decl *ReferencedDecl : ReferencedDecls) {
    if (ReferencedDecl->getBeginLoc().isValid() &&
        SM.isPointWithin(ReferencedDecl->getBeginLoc(), ScopeBegin, ScopeEnd) &&
        SM.isPointWithin(ReferencedDecl->getEndLoc(), ScopeBegin, ScopeEnd))
      return false;
  }
  return true;
}

// Return the Stmt before which we need to insert the extraction.
// To find the Stmt, we go up the AST Tree and if the Parent of the current
// Stmt is a CompoundStmt, we can extract inside this CompoundStmt just before
// the current Stmt. We ALWAYS insert before a Stmt whose parent is a
// CompoundStmt
//
// FIXME: Extraction from label, switch and case statements
// FIXME: Doens't work for FoldExpr
// FIXME: Ensure extraction from loops doesn't change semantics.
const clang::Stmt *ExtractionContext::computeInsertionPoint() const {
  // returns true if we can extract before InsertionPoint
  auto CanExtractOutside =
      [](const SelectionTree::Node *InsertionPoint) -> bool {
    if (const clang::Stmt *Stmt = InsertionPoint->ASTNode.get<clang::Stmt>()) {
      if (isa<clang::Expr>(Stmt)) {
        // Do not allow extraction from the initializer of a defaulted parameter
        // to a local variable (e.g. a function-local lambda).
        if (InsertionPoint->Parent->ASTNode.get<ParmVarDecl>() != nullptr) {
          return false;
        }

        return true;
      }

      // We don't yet allow extraction from switch/case stmt as we would need to
      // jump over the switch stmt even if there is a CompoundStmt inside the
      // switch. And there are other Stmts which we don't care about (e.g.
      // continue and break) as there can never be anything to extract from
      // them.
      return isa<AttributedStmt>(Stmt) || isa<CompoundStmt>(Stmt) ||
             isa<CXXForRangeStmt>(Stmt) || isa<DeclStmt>(Stmt) ||
             isa<DoStmt>(Stmt) || isa<ForStmt>(Stmt) || isa<IfStmt>(Stmt) ||
             isa<ReturnStmt>(Stmt) || isa<WhileStmt>(Stmt);
    }
    if (InsertionPoint->ASTNode.get<VarDecl>())
      return true;
    return false;
  };
  for (const SelectionTree::Node *CurNode = getExprNode();
       CurNode->Parent && CanExtractOutside(CurNode);
       CurNode = CurNode->Parent) {
    const clang::Stmt *CurInsertionPoint = CurNode->ASTNode.get<Stmt>();
    // give up if extraction will take a variable out of scope
    if (CurInsertionPoint && !exprIsValidOutside(CurInsertionPoint))
      break;
    if (const clang::Stmt *CurParent = CurNode->Parent->ASTNode.get<Stmt>()) {
      if (isa<CompoundStmt>(CurParent)) {
        // Ensure we don't write inside a macro.
        if (CurParent->getBeginLoc().isMacroID())
          continue;
        return CurInsertionPoint;
      }
    }
  }
  return nullptr;
}

// returns the replacement for substituting the extraction with VarName
tooling::Replacement
ExtractionContext::replaceWithVar(SourceRange Chars,
                                  llvm::StringRef VarName) const {
  unsigned ExtractionLength =
      SM.getFileOffset(Chars.getEnd()) - SM.getFileOffset(Chars.getBegin());
  return tooling::Replacement(SM, Chars.getBegin(), ExtractionLength, VarName);
}
// returns the Replacement for declaring a new variable storing the extraction
tooling::Replacement
ExtractionContext::insertDeclaration(llvm::StringRef VarName,
                                     SourceRange InitializerChars,
                                     bool AddSemicolon) const {
  llvm::StringRef ExtractionCode = toSourceCode(SM, InitializerChars);
  const SourceLocation InsertionLoc =
      toHalfOpenFileRange(SM, Ctx.getLangOpts(),
                          InsertionPoint->getSourceRange())
          ->getBegin();
  std::string ExtractedVarDecl =
      printType(VarType, ExprNode->getDeclContext(), VarName) + " = " +
      ExtractionCode.str();
  if (AddSemicolon)
    ExtractedVarDecl += "; ";
  return tooling::Replacement(SM, InsertionLoc, 0, ExtractedVarDecl);
}

// Helpers for handling "binary subexpressions" like a + [[b + c]] + d.
//
// These are special, because the formal AST doesn't match what users expect:
// - the AST is ((a + b) + c) + d, so the ancestor expression is `a + b + c`.
// - but extracting `b + c` is reasonable, as + is (mathematically) associative.
//
// So we try to support these cases with some restrictions:
//  - the operator must be associative
//  - no mixing of operators is allowed
//  - we don't look inside macro expansions in the subexpressions
//  - we only adjust the extracted range, so references in the unselected parts
//    of the AST expression (e.g. `a`) are still considered referenced for
//    the purposes of calculating the insertion point.
//    FIXME: it would be nice to exclude these references, by micromanaging
//    the computeReferencedDecls() calls around the binary operator tree.

// Information extracted about a binary operator encounted in a SelectionTree.
// It can represent either an overloaded or built-in operator.
struct ParsedBinaryOperator {
  BinaryOperatorKind Kind;
  SourceLocation ExprLoc;
  llvm::SmallVector<const SelectionTree::Node *> SelectedOperands;

  // If N is a binary operator, populate this and return true.
  bool parse(const SelectionTree::Node &N) {
    SelectedOperands.clear();

    if (const BinaryOperator *Op =
            llvm::dyn_cast_or_null<BinaryOperator>(N.ASTNode.get<Expr>())) {
      Kind = Op->getOpcode();
      ExprLoc = Op->getExprLoc();
      SelectedOperands = N.Children;
      return true;
    }
    if (const CXXOperatorCallExpr *Op =
            llvm::dyn_cast_or_null<CXXOperatorCallExpr>(
                N.ASTNode.get<Expr>())) {
      if (!Op->isInfixBinaryOp())
        return false;

      Kind = BinaryOperator::getOverloadedOpcode(Op->getOperator());
      ExprLoc = Op->getExprLoc();
      // Not all children are args, there's also the callee (operator).
      for (const auto *Child : N.Children) {
        const Expr *E = Child->ASTNode.get<Expr>();
        assert(E && "callee and args should be Exprs!");
        if (E == Op->getArg(0) || E == Op->getArg(1))
          SelectedOperands.push_back(Child);
      }
      return true;
    }
    return false;
  }

  bool associative() const {
    // Must also be left-associative, or update getBinaryOperatorRange()!
    switch (Kind) {
    case BO_Add:
    case BO_Mul:
    case BO_And:
    case BO_Or:
    case BO_Xor:
    case BO_LAnd:
    case BO_LOr:
      return true;
    default:
      return false;
    }
  }

  bool crossesMacroBoundary(const SourceManager &SM) {
    FileID F = SM.getFileID(ExprLoc);
    for (const SelectionTree::Node *Child : SelectedOperands)
      if (SM.getFileID(Child->ASTNode.get<Expr>()->getExprLoc()) != F)
        return true;
    return false;
  }
};

// If have an associative operator at the top level, then we must find
// the start point (rightmost in LHS) and end point (leftmost in RHS).
// We can only descend into subtrees where the operator matches.
//
// e.g. for a + [[b + c]] + d
//        +
//       / \
//  N-> +   d
//     / \
//    +   c <- End
//   / \
//  a   b <- Start
SourceRange getBinaryOperatorRange(const SelectionTree::Node &N,
                                   const SourceManager &SM,
                                   const LangOptions &LangOpts) {
  // If N is not a suitable binary operator, bail out.
  ParsedBinaryOperator Op;
  if (!Op.parse(N.ignoreImplicit()) || !Op.associative() ||
      Op.crossesMacroBoundary(SM) || Op.SelectedOperands.size() != 2)
    return SourceRange();
  BinaryOperatorKind OuterOp = Op.Kind;

  // Because the tree we're interested in contains only one operator type, and
  // all eligible operators are left-associative, the shape of the tree is
  // very restricted: it's a linked list along the left edges.
  // This simplifies our implementation.
  const SelectionTree::Node *Start = Op.SelectedOperands.front(); // LHS
  const SelectionTree::Node *End = Op.SelectedOperands.back();    // RHS
  // End is already correct: it can't be an OuterOp (as it's left-associative).
  // Start needs to be pushed down int the subtree to the right spot.
  while (Op.parse(Start->ignoreImplicit()) && Op.Kind == OuterOp &&
         !Op.crossesMacroBoundary(SM)) {
    assert(!Op.SelectedOperands.empty() && "got only operator on one side!");
    if (Op.SelectedOperands.size() == 1) { // Only Op.RHS selected
      Start = Op.SelectedOperands.back();
      break;
    }
    // Op.LHS is (at least partially) selected, so descend into it.
    Start = Op.SelectedOperands.front();
  }

  return SourceRange(
      toHalfOpenFileRange(SM, LangOpts, Start->ASTNode.getSourceRange())
          ->getBegin(),
      toHalfOpenFileRange(SM, LangOpts, End->ASTNode.getSourceRange())
          ->getEnd());
}

SourceRange ExtractionContext::getExtractionChars() const {
  // Special case: we're extracting an associative binary subexpression.
  SourceRange BinaryOperatorRange =
      getBinaryOperatorRange(*ExprNode, SM, Ctx.getLangOpts());
  if (BinaryOperatorRange.isValid())
    return BinaryOperatorRange;

  // Usual case: we're extracting the whole expression.
  return *toHalfOpenFileRange(SM, Ctx.getLangOpts(), Expr->getSourceRange());
}

// Find the CallExpr whose callee is the (possibly wrapped) DeclRef
const SelectionTree::Node *getCallExpr(const SelectionTree::Node *DeclRef) {
  const SelectionTree::Node &MaybeCallee = DeclRef->outerImplicit();
  const SelectionTree::Node *MaybeCall = MaybeCallee.Parent;
  if (!MaybeCall)
    return nullptr;
  const CallExpr *CE =
      llvm::dyn_cast_or_null<CallExpr>(MaybeCall->ASTNode.get<Expr>());
  if (!CE)
    return nullptr;
  if (CE->getCallee() != MaybeCallee.ASTNode.get<Expr>())
    return nullptr;
  return MaybeCall;
}

// Returns true if Inner (which is a direct child of Outer) is appearing as
// a statement rather than an expression whose value can be used.
bool childExprIsDisallowedStmt(const Stmt *Outer, const Expr *Inner) {
  if (!Outer || !Inner)
    return false;
  // Exclude the most common places where an expr can appear but be unused.
  if (llvm::isa<SwitchCase>(Outer))
    return true;
  // Control flow statements use condition etc, but not the body.
  if (const auto *WS = llvm::dyn_cast<WhileStmt>(Outer))
    return Inner == WS->getBody();
  if (const auto *DS = llvm::dyn_cast<DoStmt>(Outer))
    return Inner == DS->getBody();
  if (const auto *FS = llvm::dyn_cast<ForStmt>(Outer))
    return Inner == FS->getBody();
  if (const auto *FS = llvm::dyn_cast<CXXForRangeStmt>(Outer))
    return Inner == FS->getBody();
  if (const auto *IS = llvm::dyn_cast<IfStmt>(Outer))
    return Inner == IS->getThen() || Inner == IS->getElse();
  // Assume all other cases may be actual expressions.
  // This includes the important case of subexpressions (where Outer is Expr).
  return false;
}

// check if N can and should be extracted (e.g. is not void-typed).
bool eligibleForExtraction(const SelectionTree::Node *N) {
  const Expr *E = N->ASTNode.get<Expr>();
  if (!E)
    return false;

  // Void expressions can't be assigned to variables.
  const Type *ExprType = E->getType().getTypePtrOrNull();
  if (!ExprType || ExprType->isVoidType())
    return false;

  // A plain reference to a name (e.g. variable) isn't  worth extracting.
  // FIXME: really? What if it's e.g. `std::is_same<void, void>::value`?
  if (llvm::isa<DeclRefExpr>(E))
    return false;

  // Similarly disallow extraction for member exprs with an implicit `this`.
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    if (const auto *TE = dyn_cast<CXXThisExpr>(ME->getBase()->IgnoreImpCasts()))
      if (TE->isImplicit())
        return false;

  // Extracting Exprs like a = 1 gives placeholder = a = 1 which isn't useful.
  // FIXME: we could still hoist the assignment, and leave the variable there?
  ParsedBinaryOperator BinOp;
  bool IsBinOp = BinOp.parse(*N);
  if (IsBinOp && BinaryOperator::isAssignmentOp(BinOp.Kind))
    return false;

  const SelectionTree::Node &OuterImplicit = N->outerImplicit();
  const auto *Parent = OuterImplicit.Parent;
  if (!Parent)
    return false;
  // Filter non-applicable expression statements.
  if (childExprIsDisallowedStmt(Parent->ASTNode.get<Stmt>(),
                                OuterImplicit.ASTNode.get<Expr>()))
    return false;

  std::function<bool(const SelectionTree::Node *)> IsFullySelected =
      [&](const SelectionTree::Node *N) {
        if (N->ASTNode.getSourceRange().isValid() &&
            N->Selected != SelectionTree::Complete)
          return false;
        for (const auto *Child : N->Children) {
          if (!IsFullySelected(Child))
            return false;
        }
        return true;
      };
  auto ExprIsFullySelectedTargetNode = [&](const Expr *E) {
    if (E != OuterImplicit.ASTNode.get<Expr>())
      return false;

    // The above condition is the only relevant one except for binary operators.
    // Without the following code, we would fail to offer extraction for e.g.:
    //   int x = 1 + 2 + [[3 + 4 + 5]];
    // See the documentation of ParsedBinaryOperator for further details.
    if (!IsBinOp)
      return true;
    return IsFullySelected(N);
  };

  // Disable extraction of full RHS on assignment operations, e.g:
  // x = [[RHS_EXPR]];
  // This would just result in duplicating the code.
  if (const auto *BO = Parent->ASTNode.get<BinaryOperator>()) {
    if (BO->isAssignmentOp() && ExprIsFullySelectedTargetNode(BO->getRHS()))
      return false;
  }

  // If e.g. a capture clause was selected, the target node is the lambda
  // expression. We only want to offer the extraction if the entire lambda
  // expression was selected.
  if (llvm::isa<LambdaExpr>(E))
    return N->Selected == SelectionTree::Complete;

  // The same logic as for assignments applies to initializations.
  // However, we do allow extracting the RHS of an init capture, as it is
  // a valid use case to move non-trivial expressions out of the capture clause.
  // FIXME: In that case, the extracted variable should be captured directly,
  //        rather than an explicit copy.
  if (const auto *Decl = Parent->ASTNode.get<VarDecl>()) {
    if (!Decl->isInitCapture() &&
        ExprIsFullySelectedTargetNode(Decl->getInit())) {
      return false;
    }
  }

  return true;
}

// Find the Expr node that we're going to extract.
// We don't want to trigger for assignment expressions and variable/field
// DeclRefs. For function/member function, we want to extract the entire
// function call.
const SelectionTree::Node *computeExtractedExpr(const SelectionTree::Node *N) {
  if (!N)
    return nullptr;
  const SelectionTree::Node *TargetNode = N;
  const clang::Expr *SelectedExpr = N->ASTNode.get<clang::Expr>();
  if (!SelectedExpr)
    return nullptr;
  // For function and member function DeclRefs, extract the whole call.
  if (llvm::isa<DeclRefExpr>(SelectedExpr) ||
      llvm::isa<MemberExpr>(SelectedExpr))
    if (const SelectionTree::Node *Call = getCallExpr(N))
      TargetNode = Call;
  // Extracting Exprs like a = 1 gives placeholder = a = 1 which isn't useful.
  if (const BinaryOperator *BinOpExpr =
          dyn_cast_or_null<BinaryOperator>(SelectedExpr)) {
    if (BinOpExpr->getOpcode() == BinaryOperatorKind::BO_Assign)
      return nullptr;
  }
  if (!TargetNode || !eligibleForExtraction(TargetNode))
    return nullptr;
  return TargetNode;
}

/// Extracts an expression to the variable placeholder
/// Before:
/// int x = 5 + 4 * 3;
///         ^^^^^
/// After:
/// auto placeholder = 5 + 4;
/// int x = placeholder * 3;
class ExtractVariable : public Tweak {
public:
  const char *id() const final;
  bool prepare(const Selection &Inputs) override;
  Expected<Effect> apply(const Selection &Inputs) override;
  std::string title() const override {
    return "Extract subexpression to variable";
  }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }

private:
  // the expression to extract
  std::unique_ptr<ExtractionContext> Target;
};
REGISTER_TWEAK(ExtractVariable)
bool ExtractVariable::prepare(const Selection &Inputs) {
  // we don't trigger on empty selections for now
  if (Inputs.SelectionBegin == Inputs.SelectionEnd)
    return false;
  const ASTContext &Ctx = Inputs.AST->getASTContext();
  const SourceManager &SM = Inputs.AST->getSourceManager();
  if (const SelectionTree::Node *N =
          computeExtractedExpr(Inputs.ASTSelection.commonAncestor()))
    Target = std::make_unique<ExtractionContext>(N, SM, Ctx);
  return Target && Target->isExtractable();
}

Expected<Tweak::Effect> ExtractVariable::apply(const Selection &Inputs) {
  tooling::Replacements Result;
  // FIXME: get variable name from user or suggest based on type
  std::string VarName = "placeholder";
  SourceRange Range = Target->getExtractionChars();

  const SelectionTree::Node &OuterImplicit =
      Target->getExprNode()->outerImplicit();
  assert(OuterImplicit.Parent);
  bool IsExprStmt = llvm::isa_and_nonnull<CompoundStmt>(
      OuterImplicit.Parent->ASTNode.get<Stmt>());

  // insert new variable declaration. add a semicolon if and only if
  // we are not dealing with an expression statement, which already has
  // a semicolon that stays where it is, as it's not part of the range.
  if (auto Err =
          Result.add(Target->insertDeclaration(VarName, Range, !IsExprStmt)))
    return std::move(Err);

  // replace expression with variable name, unless it's an expression statement,
  // in which case we remove it.
  if (IsExprStmt)
    VarName.clear();
  if (auto Err = Result.add(Target->replaceWithVar(Range, VarName)))
    return std::move(Err);
  return Effect::mainFileEdit(Inputs.AST->getSourceManager(),
                              std::move(Result));
}

} // namespace
} // namespace clangd
} // namespace clang
