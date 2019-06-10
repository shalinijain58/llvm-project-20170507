//===---------------- SemaCodeComplete.cpp - Code Completion ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the code-completion semantic actions.
//
//===----------------------------------------------------------------------===//
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/Type.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Sema/SemaInternal.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Path.h"
#include <list>
#include <map>
#include <string>
#include <vector>

using namespace clang;
using namespace sema;

namespace {
/// A container of code-completion results.
class ResultBuilder {
public:
  /// The type of a name-lookup filter, which can be provided to the
  /// name-lookup routines to specify which declarations should be included in
  /// the result set (when it returns true) and which declarations should be
  /// filtered out (returns false).
  typedef bool (ResultBuilder::*LookupFilter)(const NamedDecl *) const;

  typedef CodeCompletionResult Result;

private:
  /// The actual results we have found.
  std::vector<Result> Results;

  /// A record of all of the declarations we have found and placed
  /// into the result set, used to ensure that no declaration ever gets into
  /// the result set twice.
  llvm::SmallPtrSet<const Decl *, 16> AllDeclsFound;

  typedef std::pair<const NamedDecl *, unsigned> DeclIndexPair;

  /// An entry in the shadow map, which is optimized to store
  /// a single (declaration, index) mapping (the common case) but
  /// can also store a list of (declaration, index) mappings.
  class ShadowMapEntry {
    typedef SmallVector<DeclIndexPair, 4> DeclIndexPairVector;

    /// Contains either the solitary NamedDecl * or a vector
    /// of (declaration, index) pairs.
    llvm::PointerUnion<const NamedDecl *, DeclIndexPairVector *> DeclOrVector;

    /// When the entry contains a single declaration, this is
    /// the index associated with that entry.
    unsigned SingleDeclIndex;

  public:
    ShadowMapEntry() : DeclOrVector(), SingleDeclIndex(0) {}
    ShadowMapEntry(const ShadowMapEntry &) = delete;
    ShadowMapEntry(ShadowMapEntry &&Move) { *this = std::move(Move); }
    ShadowMapEntry &operator=(const ShadowMapEntry &) = delete;
    ShadowMapEntry &operator=(ShadowMapEntry &&Move) {
      SingleDeclIndex = Move.SingleDeclIndex;
      DeclOrVector = Move.DeclOrVector;
      Move.DeclOrVector = nullptr;
      return *this;
    }

    void Add(const NamedDecl *ND, unsigned Index) {
      if (DeclOrVector.isNull()) {
        // 0 - > 1 elements: just set the single element information.
        DeclOrVector = ND;
        SingleDeclIndex = Index;
        return;
      }

      if (const NamedDecl *PrevND =
              DeclOrVector.dyn_cast<const NamedDecl *>()) {
        // 1 -> 2 elements: create the vector of results and push in the
        // existing declaration.
        DeclIndexPairVector *Vec = new DeclIndexPairVector;
        Vec->push_back(DeclIndexPair(PrevND, SingleDeclIndex));
        DeclOrVector = Vec;
      }

      // Add the new element to the end of the vector.
      DeclOrVector.get<DeclIndexPairVector *>()->push_back(
          DeclIndexPair(ND, Index));
    }

    ~ShadowMapEntry() {
      if (DeclIndexPairVector *Vec =
              DeclOrVector.dyn_cast<DeclIndexPairVector *>()) {
        delete Vec;
        DeclOrVector = ((NamedDecl *)nullptr);
      }
    }

    // Iteration.
    class iterator;
    iterator begin() const;
    iterator end() const;
  };

  /// A mapping from declaration names to the declarations that have
  /// this name within a particular scope and their index within the list of
  /// results.
  typedef llvm::DenseMap<DeclarationName, ShadowMapEntry> ShadowMap;

  /// The semantic analysis object for which results are being
  /// produced.
  Sema &SemaRef;

  /// The allocator used to allocate new code-completion strings.
  CodeCompletionAllocator &Allocator;

  CodeCompletionTUInfo &CCTUInfo;

  /// If non-NULL, a filter function used to remove any code-completion
  /// results that are not desirable.
  LookupFilter Filter;

  /// Whether we should allow declarations as
  /// nested-name-specifiers that would otherwise be filtered out.
  bool AllowNestedNameSpecifiers;

  /// If set, the type that we would prefer our resulting value
  /// declarations to have.
  ///
  /// Closely matching the preferred type gives a boost to a result's
  /// priority.
  CanQualType PreferredType;

  /// A list of shadow maps, which is used to model name hiding at
  /// different levels of, e.g., the inheritance hierarchy.
  std::list<ShadowMap> ShadowMaps;

  /// Overloaded C++ member functions found by SemaLookup.
  /// Used to determine when one overload is dominated by another.
  llvm::DenseMap<std::pair<DeclContext *, /*Name*/uintptr_t>, ShadowMapEntry>
      OverloadMap;

  /// If we're potentially referring to a C++ member function, the set
  /// of qualifiers applied to the object type.
  Qualifiers ObjectTypeQualifiers;
  /// The kind of the object expression, for rvalue/lvalue overloads.
  ExprValueKind ObjectKind;

  /// Whether the \p ObjectTypeQualifiers field is active.
  bool HasObjectTypeQualifiers;

  /// The selector that we prefer.
  Selector PreferredSelector;

  /// The completion context in which we are gathering results.
  CodeCompletionContext CompletionContext;

  /// If we are in an instance method definition, the \@implementation
  /// object.
  ObjCImplementationDecl *ObjCImplementation;

  void AdjustResultPriorityForDecl(Result &R);

  void MaybeAddConstructorResults(Result R);

public:
  explicit ResultBuilder(Sema &SemaRef, CodeCompletionAllocator &Allocator,
                         CodeCompletionTUInfo &CCTUInfo,
                         const CodeCompletionContext &CompletionContext,
                         LookupFilter Filter = nullptr)
      : SemaRef(SemaRef), Allocator(Allocator), CCTUInfo(CCTUInfo),
        Filter(Filter), AllowNestedNameSpecifiers(false),
        HasObjectTypeQualifiers(false), CompletionContext(CompletionContext),
        ObjCImplementation(nullptr) {
    // If this is an Objective-C instance method definition, dig out the
    // corresponding implementation.
    switch (CompletionContext.getKind()) {
    case CodeCompletionContext::CCC_Expression:
    case CodeCompletionContext::CCC_ObjCMessageReceiver:
    case CodeCompletionContext::CCC_ParenthesizedExpression:
    case CodeCompletionContext::CCC_Statement:
    case CodeCompletionContext::CCC_Recovery:
      if (ObjCMethodDecl *Method = SemaRef.getCurMethodDecl())
        if (Method->isInstanceMethod())
          if (ObjCInterfaceDecl *Interface = Method->getClassInterface())
            ObjCImplementation = Interface->getImplementation();
      break;

    default:
      break;
    }
  }

  /// Determine the priority for a reference to the given declaration.
  unsigned getBasePriority(const NamedDecl *D);

  /// Whether we should include code patterns in the completion
  /// results.
  bool includeCodePatterns() const {
    return SemaRef.CodeCompleter &&
           SemaRef.CodeCompleter->includeCodePatterns();
  }

  /// Set the filter used for code-completion results.
  void setFilter(LookupFilter Filter) { this->Filter = Filter; }

  Result *data() { return Results.empty() ? nullptr : &Results.front(); }
  unsigned size() const { return Results.size(); }
  bool empty() const { return Results.empty(); }

  /// Specify the preferred type.
  void setPreferredType(QualType T) {
    PreferredType = SemaRef.Context.getCanonicalType(T);
  }

  /// Set the cv-qualifiers on the object type, for us in filtering
  /// calls to member functions.
  ///
  /// When there are qualifiers in this set, they will be used to filter
  /// out member functions that aren't available (because there will be a
  /// cv-qualifier mismatch) or prefer functions with an exact qualifier
  /// match.
  void setObjectTypeQualifiers(Qualifiers Quals, ExprValueKind Kind) {
    ObjectTypeQualifiers = Quals;
    ObjectKind = Kind;
    HasObjectTypeQualifiers = true;
  }

  /// Set the preferred selector.
  ///
  /// When an Objective-C method declaration result is added, and that
  /// method's selector matches this preferred selector, we give that method
  /// a slight priority boost.
  void setPreferredSelector(Selector Sel) { PreferredSelector = Sel; }

  /// Retrieve the code-completion context for which results are
  /// being collected.
  const CodeCompletionContext &getCompletionContext() const {
    return CompletionContext;
  }

  /// Specify whether nested-name-specifiers are allowed.
  void allowNestedNameSpecifiers(bool Allow = true) {
    AllowNestedNameSpecifiers = Allow;
  }

  /// Return the semantic analysis object for which we are collecting
  /// code completion results.
  Sema &getSema() const { return SemaRef; }

  /// Retrieve the allocator used to allocate code completion strings.
  CodeCompletionAllocator &getAllocator() const { return Allocator; }

  CodeCompletionTUInfo &getCodeCompletionTUInfo() const { return CCTUInfo; }

  /// Determine whether the given declaration is at all interesting
  /// as a code-completion result.
  ///
  /// \param ND the declaration that we are inspecting.
  ///
  /// \param AsNestedNameSpecifier will be set true if this declaration is
  /// only interesting when it is a nested-name-specifier.
  bool isInterestingDecl(const NamedDecl *ND,
                         bool &AsNestedNameSpecifier) const;

  /// Check whether the result is hidden by the Hiding declaration.
  ///
  /// \returns true if the result is hidden and cannot be found, false if
  /// the hidden result could still be found. When false, \p R may be
  /// modified to describe how the result can be found (e.g., via extra
  /// qualification).
  bool CheckHiddenResult(Result &R, DeclContext *CurContext,
                         const NamedDecl *Hiding);

  /// Add a new result to this result set (if it isn't already in one
  /// of the shadow maps), or replace an existing result (for, e.g., a
  /// redeclaration).
  ///
  /// \param R the result to add (if it is unique).
  ///
  /// \param CurContext the context in which this result will be named.
  void MaybeAddResult(Result R, DeclContext *CurContext = nullptr);

  /// Add a new result to this result set, where we already know
  /// the hiding declaration (if any).
  ///
  /// \param R the result to add (if it is unique).
  ///
  /// \param CurContext the context in which this result will be named.
  ///
  /// \param Hiding the declaration that hides the result.
  ///
  /// \param InBaseClass whether the result was found in a base
  /// class of the searched context.
  void AddResult(Result R, DeclContext *CurContext, NamedDecl *Hiding,
                 bool InBaseClass);

  /// Add a new non-declaration result to this result set.
  void AddResult(Result R);

  /// Enter into a new scope.
  void EnterNewScope();

  /// Exit from the current scope.
  void ExitScope();

  /// Ignore this declaration, if it is seen again.
  void Ignore(const Decl *D) { AllDeclsFound.insert(D->getCanonicalDecl()); }

  /// Add a visited context.
  void addVisitedContext(DeclContext *Ctx) {
    CompletionContext.addVisitedContext(Ctx);
  }

  /// \name Name lookup predicates
  ///
  /// These predicates can be passed to the name lookup functions to filter the
  /// results of name lookup. All of the predicates have the same type, so that
  ///
  //@{
  bool IsOrdinaryName(const NamedDecl *ND) const;
  bool IsOrdinaryNonTypeName(const NamedDecl *ND) const;
  bool IsIntegralConstantValue(const NamedDecl *ND) const;
  bool IsOrdinaryNonValueName(const NamedDecl *ND) const;
  bool IsNestedNameSpecifier(const NamedDecl *ND) const;
  bool IsEnum(const NamedDecl *ND) const;
  bool IsClassOrStruct(const NamedDecl *ND) const;
  bool IsUnion(const NamedDecl *ND) const;
  bool IsNamespace(const NamedDecl *ND) const;
  bool IsNamespaceOrAlias(const NamedDecl *ND) const;
  bool IsType(const NamedDecl *ND) const;
  bool IsMember(const NamedDecl *ND) const;
  bool IsObjCIvar(const NamedDecl *ND) const;
  bool IsObjCMessageReceiver(const NamedDecl *ND) const;
  bool IsObjCMessageReceiverOrLambdaCapture(const NamedDecl *ND) const;
  bool IsObjCCollection(const NamedDecl *ND) const;
  bool IsImpossibleToSatisfy(const NamedDecl *ND) const;
  //@}
};
} // namespace

void PreferredTypeBuilder::enterReturn(Sema &S, SourceLocation Tok) {
  if (isa<BlockDecl>(S.CurContext)) {
    if (sema::BlockScopeInfo *BSI = S.getCurBlock()) {
      ComputeType = nullptr;
      Type = BSI->ReturnType;
      ExpectedLoc = Tok;
    }
  } else if (const auto *Function = dyn_cast<FunctionDecl>(S.CurContext)) {
    ComputeType = nullptr;
    Type = Function->getReturnType();
    ExpectedLoc = Tok;
  } else if (const auto *Method = dyn_cast<ObjCMethodDecl>(S.CurContext)) {
    ComputeType = nullptr;
    Type = Method->getReturnType();
    ExpectedLoc = Tok;
  }
}

void PreferredTypeBuilder::enterVariableInit(SourceLocation Tok, Decl *D) {
  auto *VD = llvm::dyn_cast_or_null<ValueDecl>(D);
  ComputeType = nullptr;
  Type = VD ? VD->getType() : QualType();
  ExpectedLoc = Tok;
}

void PreferredTypeBuilder::enterFunctionArgument(
    SourceLocation Tok, llvm::function_ref<QualType()> ComputeType) {
  this->ComputeType = ComputeType;
  Type = QualType();
  ExpectedLoc = Tok;
}

void PreferredTypeBuilder::enterParenExpr(SourceLocation Tok,
                                          SourceLocation LParLoc) {
  // expected type for parenthesized expression does not change.
  if (ExpectedLoc == LParLoc)
    ExpectedLoc = Tok;
}

static QualType getPreferredTypeOfBinaryRHS(Sema &S, Expr *LHS,
                                            tok::TokenKind Op) {
  if (!LHS)
    return QualType();

  QualType LHSType = LHS->getType();
  if (LHSType->isPointerType()) {
    if (Op == tok::plus || Op == tok::plusequal || Op == tok::minusequal)
      return S.getASTContext().getPointerDiffType();
    // Pointer difference is more common than subtracting an int from a pointer.
    if (Op == tok::minus)
      return LHSType;
  }

  switch (Op) {
  // No way to infer the type of RHS from LHS.
  case tok::comma:
    return QualType();
  // Prefer the type of the left operand for all of these.
  // Arithmetic operations.
  case tok::plus:
  case tok::plusequal:
  case tok::minus:
  case tok::minusequal:
  case tok::percent:
  case tok::percentequal:
  case tok::slash:
  case tok::slashequal:
  case tok::star:
  case tok::starequal:
  // Assignment.
  case tok::equal:
  // Comparison operators.
  case tok::equalequal:
  case tok::exclaimequal:
  case tok::less:
  case tok::lessequal:
  case tok::greater:
  case tok::greaterequal:
  case tok::spaceship:
    return LHS->getType();
  // Binary shifts are often overloaded, so don't try to guess those.
  case tok::greatergreater:
  case tok::greatergreaterequal:
  case tok::lessless:
  case tok::lesslessequal:
    if (LHSType->isIntegralOrEnumerationType())
      return S.getASTContext().IntTy;
    return QualType();
  // Logical operators, assume we want bool.
  case tok::ampamp:
  case tok::pipepipe:
  case tok::caretcaret:
    return S.getASTContext().BoolTy;
  // Operators often used for bit manipulation are typically used with the type
  // of the left argument.
  case tok::pipe:
  case tok::pipeequal:
  case tok::caret:
  case tok::caretequal:
  case tok::amp:
  case tok::ampequal:
    if (LHSType->isIntegralOrEnumerationType())
      return LHSType;
    return QualType();
  // RHS should be a pointer to a member of the 'LHS' type, but we can't give
  // any particular type here.
  case tok::periodstar:
  case tok::arrowstar:
    return QualType();
  default:
    // FIXME(ibiryukov): handle the missing op, re-add the assertion.
    // assert(false && "unhandled binary op");
    return QualType();
  }
}

/// Get preferred type for an argument of an unary expression. \p ContextType is
/// preferred type of the whole unary expression.
static QualType getPreferredTypeOfUnaryArg(Sema &S, QualType ContextType,
                                           tok::TokenKind Op) {
  switch (Op) {
  case tok::exclaim:
    return S.getASTContext().BoolTy;
  case tok::amp:
    if (!ContextType.isNull() && ContextType->isPointerType())
      return ContextType->getPointeeType();
    return QualType();
  case tok::star:
    if (ContextType.isNull())
      return QualType();
    return S.getASTContext().getPointerType(ContextType.getNonReferenceType());
  case tok::plus:
  case tok::minus:
  case tok::tilde:
  case tok::minusminus:
  case tok::plusplus:
    if (ContextType.isNull())
      return S.getASTContext().IntTy;
    // leave as is, these operators typically return the same type.
    return ContextType;
  case tok::kw___real:
  case tok::kw___imag:
    return QualType();
  default:
    assert(false && "unhandled unary op");
    return QualType();
  }
}

void PreferredTypeBuilder::enterBinary(Sema &S, SourceLocation Tok, Expr *LHS,
                                       tok::TokenKind Op) {
  ComputeType = nullptr;
  Type = getPreferredTypeOfBinaryRHS(S, LHS, Op);
  ExpectedLoc = Tok;
}

void PreferredTypeBuilder::enterMemAccess(Sema &S, SourceLocation Tok,
                                          Expr *Base) {
  if (!Base)
    return;
  // Do we have expected type for Base?
  if (ExpectedLoc != Base->getBeginLoc())
    return;
  // Keep the expected type, only update the location.
  ExpectedLoc = Tok;
  return;
}

void PreferredTypeBuilder::enterUnary(Sema &S, SourceLocation Tok,
                                      tok::TokenKind OpKind,
                                      SourceLocation OpLoc) {
  ComputeType = nullptr;
  Type = getPreferredTypeOfUnaryArg(S, this->get(OpLoc), OpKind);
  ExpectedLoc = Tok;
}

void PreferredTypeBuilder::enterSubscript(Sema &S, SourceLocation Tok,
                                          Expr *LHS) {
  ComputeType = nullptr;
  Type = S.getASTContext().IntTy;
  ExpectedLoc = Tok;
}

void PreferredTypeBuilder::enterTypeCast(SourceLocation Tok,
                                         QualType CastType) {
  ComputeType = nullptr;
  Type = !CastType.isNull() ? CastType.getCanonicalType() : QualType();
  ExpectedLoc = Tok;
}

void PreferredTypeBuilder::enterCondition(Sema &S, SourceLocation Tok) {
  ComputeType = nullptr;
  Type = S.getASTContext().BoolTy;
  ExpectedLoc = Tok;
}

class ResultBuilder::ShadowMapEntry::iterator {
  llvm::PointerUnion<const NamedDecl *, const DeclIndexPair *> DeclOrIterator;
  unsigned SingleDeclIndex;

public:
  typedef DeclIndexPair value_type;
  typedef value_type reference;
  typedef std::ptrdiff_t difference_type;
  typedef std::input_iterator_tag iterator_category;

  class pointer {
    DeclIndexPair Value;

  public:
    pointer(const DeclIndexPair &Value) : Value(Value) {}

    const DeclIndexPair *operator->() const { return &Value; }
  };

  iterator() : DeclOrIterator((NamedDecl *)nullptr), SingleDeclIndex(0) {}

  iterator(const NamedDecl *SingleDecl, unsigned Index)
      : DeclOrIterator(SingleDecl), SingleDeclIndex(Index) {}

  iterator(const DeclIndexPair *Iterator)
      : DeclOrIterator(Iterator), SingleDeclIndex(0) {}

  iterator &operator++() {
    if (DeclOrIterator.is<const NamedDecl *>()) {
      DeclOrIterator = (NamedDecl *)nullptr;
      SingleDeclIndex = 0;
      return *this;
    }

    const DeclIndexPair *I = DeclOrIterator.get<const DeclIndexPair *>();
    ++I;
    DeclOrIterator = I;
    return *this;
  }

  /*iterator operator++(int) {
    iterator tmp(*this);
    ++(*this);
    return tmp;
  }*/

  reference operator*() const {
    if (const NamedDecl *ND = DeclOrIterator.dyn_cast<const NamedDecl *>())
      return reference(ND, SingleDeclIndex);

    return *DeclOrIterator.get<const DeclIndexPair *>();
  }

  pointer operator->() const { return pointer(**this); }

  friend bool operator==(const iterator &X, const iterator &Y) {
    return X.DeclOrIterator.getOpaqueValue() ==
               Y.DeclOrIterator.getOpaqueValue() &&
           X.SingleDeclIndex == Y.SingleDeclIndex;
  }

  friend bool operator!=(const iterator &X, const iterator &Y) {
    return !(X == Y);
  }
};

ResultBuilder::ShadowMapEntry::iterator
ResultBuilder::ShadowMapEntry::begin() const {
  if (DeclOrVector.isNull())
    return iterator();

  if (const NamedDecl *ND = DeclOrVector.dyn_cast<const NamedDecl *>())
    return iterator(ND, SingleDeclIndex);

  return iterator(DeclOrVector.get<DeclIndexPairVector *>()->begin());
}

ResultBuilder::ShadowMapEntry::iterator
ResultBuilder::ShadowMapEntry::end() const {
  if (DeclOrVector.is<const NamedDecl *>() || DeclOrVector.isNull())
    return iterator();

  return iterator(DeclOrVector.get<DeclIndexPairVector *>()->end());
}

/// Compute the qualification required to get from the current context
/// (\p CurContext) to the target context (\p TargetContext).
///
/// \param Context the AST context in which the qualification will be used.
///
/// \param CurContext the context where an entity is being named, which is
/// typically based on the current scope.
///
/// \param TargetContext the context in which the named entity actually
/// resides.
///
/// \returns a nested name specifier that refers into the target context, or
/// NULL if no qualification is needed.
static NestedNameSpecifier *
getRequiredQualification(ASTContext &Context, const DeclContext *CurContext,
                         const DeclContext *TargetContext) {
  SmallVector<const DeclContext *, 4> TargetParents;

  for (const DeclContext *CommonAncestor = TargetContext;
       CommonAncestor && !CommonAncestor->Encloses(CurContext);
       CommonAncestor = CommonAncestor->getLookupParent()) {
    if (CommonAncestor->isTransparentContext() ||
        CommonAncestor->isFunctionOrMethod())
      continue;

    TargetParents.push_back(CommonAncestor);
  }

  NestedNameSpecifier *Result = nullptr;
  while (!TargetParents.empty()) {
    const DeclContext *Parent = TargetParents.pop_back_val();

    if (const auto *Namespace = dyn_cast<NamespaceDecl>(Parent)) {
      if (!Namespace->getIdentifier())
        continue;

      Result = NestedNameSpecifier::Create(Context, Result, Namespace);
    } else if (const auto *TD = dyn_cast<TagDecl>(Parent))
      Result = NestedNameSpecifier::Create(
          Context, Result, false, Context.getTypeDeclType(TD).getTypePtr());
  }
  return Result;
}

/// Determine whether \p Id is a name reserved for the implementation (C99
/// 7.1.3, C++ [lib.global.names]).
static bool isReservedName(const IdentifierInfo *Id,
                           bool doubleUnderscoreOnly = false) {
  if (Id->getLength() < 2)
    return false;
  const char *Name = Id->getNameStart();
  return Name[0] == '_' &&
         (Name[1] == '_' ||
          (Name[1] >= 'A' && Name[1] <= 'Z' && !doubleUnderscoreOnly));
}

// Some declarations have reserved names that we don't want to ever show.
// Filter out names reserved for the implementation if they come from a
// system header.
static bool shouldIgnoreDueToReservedName(const NamedDecl *ND, Sema &SemaRef) {
  const IdentifierInfo *Id = ND->getIdentifier();
  if (!Id)
    return false;

  // Ignore reserved names for compiler provided decls.
  if (isReservedName(Id) && ND->getLocation().isInvalid())
    return true;

  // For system headers ignore only double-underscore names.
  // This allows for system headers providing private symbols with a single
  // underscore.
  if (isReservedName(Id, /*doubleUnderscoreOnly=*/true) &&
      SemaRef.SourceMgr.isInSystemHeader(
          SemaRef.SourceMgr.getSpellingLoc(ND->getLocation())))
    return true;

  return false;
}

bool ResultBuilder::isInterestingDecl(const NamedDecl *ND,
                                      bool &AsNestedNameSpecifier) const {
  AsNestedNameSpecifier = false;

  auto *Named = ND;
  ND = ND->getUnderlyingDecl();

  // Skip unnamed entities.
  if (!ND->getDeclName())
    return false;

  // Friend declarations and declarations introduced due to friends are never
  // added as results.
  if (ND->getFriendObjectKind() == Decl::FOK_Undeclared)
    return false;

  // Class template (partial) specializations are never added as results.
  if (isa<ClassTemplateSpecializationDecl>(ND) ||
      isa<ClassTemplatePartialSpecializationDecl>(ND))
    return false;

  // Using declarations themselves are never added as results.
  if (isa<UsingDecl>(ND))
    return false;

  if (shouldIgnoreDueToReservedName(ND, SemaRef))
    return false;

  if (Filter == &ResultBuilder::IsNestedNameSpecifier ||
      (isa<NamespaceDecl>(ND) && Filter != &ResultBuilder::IsNamespace &&
       Filter != &ResultBuilder::IsNamespaceOrAlias && Filter != nullptr))
    AsNestedNameSpecifier = true;

  // Filter out any unwanted results.
  if (Filter && !(this->*Filter)(Named)) {
    // Check whether it is interesting as a nested-name-specifier.
    if (AllowNestedNameSpecifiers && SemaRef.getLangOpts().CPlusPlus &&
        IsNestedNameSpecifier(ND) &&
        (Filter != &ResultBuilder::IsMember ||
         (isa<CXXRecordDecl>(ND) &&
          cast<CXXRecordDecl>(ND)->isInjectedClassName()))) {
      AsNestedNameSpecifier = true;
      return true;
    }

    return false;
  }
  // ... then it must be interesting!
  return true;
}

bool ResultBuilder::CheckHiddenResult(Result &R, DeclContext *CurContext,
                                      const NamedDecl *Hiding) {
  // In C, there is no way to refer to a hidden name.
  // FIXME: This isn't true; we can find a tag name hidden by an ordinary
  // name if we introduce the tag type.
  if (!SemaRef.getLangOpts().CPlusPlus)
    return true;

  const DeclContext *HiddenCtx =
      R.Declaration->getDeclContext()->getRedeclContext();

  // There is no way to qualify a name declared in a function or method.
  if (HiddenCtx->isFunctionOrMethod())
    return true;

  if (HiddenCtx == Hiding->getDeclContext()->getRedeclContext())
    return true;

  // We can refer to the result with the appropriate qualification. Do it.
  R.Hidden = true;
  R.QualifierIsInformative = false;

  if (!R.Qualifier)
    R.Qualifier = getRequiredQualification(SemaRef.Context, CurContext,
                                           R.Declaration->getDeclContext());
  return false;
}

/// A simplified classification of types used to determine whether two
/// types are "similar enough" when adjusting priorities.
SimplifiedTypeClass clang::getSimplifiedTypeClass(CanQualType T) {
  switch (T->getTypeClass()) {
  case Type::Builtin:
    switch (cast<BuiltinType>(T)->getKind()) {
    case BuiltinType::Void:
      return STC_Void;

    case BuiltinType::NullPtr:
      return STC_Pointer;

    case BuiltinType::Overload:
    case BuiltinType::Dependent:
      return STC_Other;

    case BuiltinType::ObjCId:
    case BuiltinType::ObjCClass:
    case BuiltinType::ObjCSel:
      return STC_ObjectiveC;

    default:
      return STC_Arithmetic;
    }

  case Type::Complex:
    return STC_Arithmetic;

  case Type::Pointer:
    return STC_Pointer;

  case Type::BlockPointer:
    return STC_Block;

  case Type::LValueReference:
  case Type::RValueReference:
    return getSimplifiedTypeClass(T->getAs<ReferenceType>()->getPointeeType());

  case Type::ConstantArray:
  case Type::IncompleteArray:
  case Type::VariableArray:
  case Type::DependentSizedArray:
    return STC_Array;

  case Type::DependentSizedExtVector:
  case Type::Vector:
  case Type::ExtVector:
    return STC_Arithmetic;

  case Type::FunctionProto:
  case Type::FunctionNoProto:
    return STC_Function;

  case Type::Record:
    return STC_Record;

  case Type::Enum:
    return STC_Arithmetic;

  case Type::ObjCObject:
  case Type::ObjCInterface:
  case Type::ObjCObjectPointer:
    return STC_ObjectiveC;

  default:
    return STC_Other;
  }
}

/// Get the type that a given expression will have if this declaration
/// is used as an expression in its "typical" code-completion form.
QualType clang::getDeclUsageType(ASTContext &C, const NamedDecl *ND) {
  ND = ND->getUnderlyingDecl();

  if (const auto *Type = dyn_cast<TypeDecl>(ND))
    return C.getTypeDeclType(Type);
  if (const auto *Iface = dyn_cast<ObjCInterfaceDecl>(ND))
    return C.getObjCInterfaceType(Iface);

  QualType T;
  if (const FunctionDecl *Function = ND->getAsFunction())
    T = Function->getCallResultType();
  else if (const auto *Method = dyn_cast<ObjCMethodDecl>(ND))
    T = Method->getSendResultType();
  else if (const auto *Enumerator = dyn_cast<EnumConstantDecl>(ND))
    T = C.getTypeDeclType(cast<EnumDecl>(Enumerator->getDeclContext()));
  else if (const auto *Property = dyn_cast<ObjCPropertyDecl>(ND))
    T = Property->getType();
  else if (const auto *Value = dyn_cast<ValueDecl>(ND))
    T = Value->getType();

  if (T.isNull())
    return QualType();

  // Dig through references, function pointers, and block pointers to
  // get down to the likely type of an expression when the entity is
  // used.
  do {
    if (const auto *Ref = T->getAs<ReferenceType>()) {
      T = Ref->getPointeeType();
      continue;
    }

    if (const auto *Pointer = T->getAs<PointerType>()) {
      if (Pointer->getPointeeType()->isFunctionType()) {
        T = Pointer->getPointeeType();
        continue;
      }

      break;
    }

    if (const auto *Block = T->getAs<BlockPointerType>()) {
      T = Block->getPointeeType();
      continue;
    }

    if (const auto *Function = T->getAs<FunctionType>()) {
      T = Function->getReturnType();
      continue;
    }

    break;
  } while (true);

  return T;
}

unsigned ResultBuilder::getBasePriority(const NamedDecl *ND) {
  if (!ND)
    return CCP_Unlikely;

  // Context-based decisions.
  const DeclContext *LexicalDC = ND->getLexicalDeclContext();
  if (LexicalDC->isFunctionOrMethod()) {
    // _cmd is relatively rare
    if (const auto *ImplicitParam = dyn_cast<ImplicitParamDecl>(ND))
      if (ImplicitParam->getIdentifier() &&
          ImplicitParam->getIdentifier()->isStr("_cmd"))
        return CCP_ObjC_cmd;

    return CCP_LocalDeclaration;
  }

  const DeclContext *DC = ND->getDeclContext()->getRedeclContext();
  if (DC->isRecord() || isa<ObjCContainerDecl>(DC)) {
    // Explicit destructor calls are very rare.
    if (isa<CXXDestructorDecl>(ND))
      return CCP_Unlikely;
    // Explicit operator and conversion function calls are also very rare.
    auto DeclNameKind = ND->getDeclName().getNameKind();
    if (DeclNameKind == DeclarationName::CXXOperatorName ||
        DeclNameKind == DeclarationName::CXXLiteralOperatorName ||
        DeclNameKind == DeclarationName::CXXConversionFunctionName)
      return CCP_Unlikely;
    return CCP_MemberDeclaration;
  }

  // Content-based decisions.
  if (isa<EnumConstantDecl>(ND))
    return CCP_Constant;

  // Use CCP_Type for type declarations unless we're in a statement, Objective-C
  // message receiver, or parenthesized expression context. There, it's as
  // likely that the user will want to write a type as other declarations.
  if ((isa<TypeDecl>(ND) || isa<ObjCInterfaceDecl>(ND)) &&
      !(CompletionContext.getKind() == CodeCompletionContext::CCC_Statement ||
        CompletionContext.getKind() ==
            CodeCompletionContext::CCC_ObjCMessageReceiver ||
        CompletionContext.getKind() ==
            CodeCompletionContext::CCC_ParenthesizedExpression))
    return CCP_Type;

  return CCP_Declaration;
}

void ResultBuilder::AdjustResultPriorityForDecl(Result &R) {
  // If this is an Objective-C method declaration whose selector matches our
  // preferred selector, give it a priority boost.
  if (!PreferredSelector.isNull())
    if (const auto *Method = dyn_cast<ObjCMethodDecl>(R.Declaration))
      if (PreferredSelector == Method->getSelector())
        R.Priority += CCD_SelectorMatch;

  // If we have a preferred type, adjust the priority for results with exactly-
  // matching or nearly-matching types.
  if (!PreferredType.isNull()) {
    QualType T = getDeclUsageType(SemaRef.Context, R.Declaration);
    if (!T.isNull()) {
      CanQualType TC = SemaRef.Context.getCanonicalType(T);
      // Check for exactly-matching types (modulo qualifiers).
      if (SemaRef.Context.hasSameUnqualifiedType(PreferredType, TC))
        R.Priority /= CCF_ExactTypeMatch;
      // Check for nearly-matching types, based on classification of each.
      else if ((getSimplifiedTypeClass(PreferredType) ==
                getSimplifiedTypeClass(TC)) &&
               !(PreferredType->isEnumeralType() && TC->isEnumeralType()))
        R.Priority /= CCF_SimilarTypeMatch;
    }
  }
}

static DeclContext::lookup_result getConstructors(ASTContext &Context,
                                                  const CXXRecordDecl *Record) {
  QualType RecordTy = Context.getTypeDeclType(Record);
  DeclarationName ConstructorName =
      Context.DeclarationNames.getCXXConstructorName(
          Context.getCanonicalType(RecordTy));
  return Record->lookup(ConstructorName);
}

void ResultBuilder::MaybeAddConstructorResults(Result R) {
  if (!SemaRef.getLangOpts().CPlusPlus || !R.Declaration ||
      !CompletionContext.wantConstructorResults())
    return;

  const NamedDecl *D = R.Declaration;
  const CXXRecordDecl *Record = nullptr;
  if (const ClassTemplateDecl *ClassTemplate = dyn_cast<ClassTemplateDecl>(D))
    Record = ClassTemplate->getTemplatedDecl();
  else if ((Record = dyn_cast<CXXRecordDecl>(D))) {
    // Skip specializations and partial specializations.
    if (isa<ClassTemplateSpecializationDecl>(Record))
      return;
  } else {
    // There are no constructors here.
    return;
  }

  Record = Record->getDefinition();
  if (!Record)
    return;

  for (NamedDecl *Ctor : getConstructors(SemaRef.Context, Record)) {
    R.Declaration = Ctor;
    R.CursorKind = getCursorKindForDecl(R.Declaration);
    Results.push_back(R);
  }
}

static bool isConstructor(const Decl *ND) {
  if (const auto *Tmpl = dyn_cast<FunctionTemplateDecl>(ND))
    ND = Tmpl->getTemplatedDecl();
  return isa<CXXConstructorDecl>(ND);
}

void ResultBuilder::MaybeAddResult(Result R, DeclContext *CurContext) {
  assert(!ShadowMaps.empty() && "Must enter into a results scope");

  if (R.Kind != Result::RK_Declaration) {
    // For non-declaration results, just add the result.
    Results.push_back(R);
    return;
  }

  // Look through using declarations.
  if (const UsingShadowDecl *Using = dyn_cast<UsingShadowDecl>(R.Declaration)) {
    CodeCompletionResult Result(Using->getTargetDecl(),
                                getBasePriority(Using->getTargetDecl()),
                                R.Qualifier);
    Result.ShadowDecl = Using;
    MaybeAddResult(Result, CurContext);
    return;
  }

  const Decl *CanonDecl = R.Declaration->getCanonicalDecl();
  unsigned IDNS = CanonDecl->getIdentifierNamespace();

  bool AsNestedNameSpecifier = false;
  if (!isInterestingDecl(R.Declaration, AsNestedNameSpecifier))
    return;

  // C++ constructors are never found by name lookup.
  if (isConstructor(R.Declaration))
    return;

  ShadowMap &SMap = ShadowMaps.back();
  ShadowMapEntry::iterator I, IEnd;
  ShadowMap::iterator NamePos = SMap.find(R.Declaration->getDeclName());
  if (NamePos != SMap.end()) {
    I = NamePos->second.begin();
    IEnd = NamePos->second.end();
  }

  for (; I != IEnd; ++I) {
    const NamedDecl *ND = I->first;
    unsigned Index = I->second;
    if (ND->getCanonicalDecl() == CanonDecl) {
      // This is a redeclaration. Always pick the newer declaration.
      Results[Index].Declaration = R.Declaration;

      // We're done.
      return;
    }
  }

  // This is a new declaration in this scope. However, check whether this
  // declaration name is hidden by a similarly-named declaration in an outer
  // scope.
  std::list<ShadowMap>::iterator SM, SMEnd = ShadowMaps.end();
  --SMEnd;
  for (SM = ShadowMaps.begin(); SM != SMEnd; ++SM) {
    ShadowMapEntry::iterator I, IEnd;
    ShadowMap::iterator NamePos = SM->find(R.Declaration->getDeclName());
    if (NamePos != SM->end()) {
      I = NamePos->second.begin();
      IEnd = NamePos->second.end();
    }
    for (; I != IEnd; ++I) {
      // A tag declaration does not hide a non-tag declaration.
      if (I->first->hasTagIdentifierNamespace() &&
          (IDNS & (Decl::IDNS_Member | Decl::IDNS_Ordinary |
                   Decl::IDNS_LocalExtern | Decl::IDNS_ObjCProtocol)))
        continue;

      // Protocols are in distinct namespaces from everything else.
      if (((I->first->getIdentifierNamespace() & Decl::IDNS_ObjCProtocol) ||
           (IDNS & Decl::IDNS_ObjCProtocol)) &&
          I->first->getIdentifierNamespace() != IDNS)
        continue;

      // The newly-added result is hidden by an entry in the shadow map.
      if (CheckHiddenResult(R, CurContext, I->first))
        return;

      break;
    }
  }

  // Make sure that any given declaration only shows up in the result set once.
  if (!AllDeclsFound.insert(CanonDecl).second)
    return;

  // If the filter is for nested-name-specifiers, then this result starts a
  // nested-name-specifier.
  if (AsNestedNameSpecifier) {
    R.StartsNestedNameSpecifier = true;
    R.Priority = CCP_NestedNameSpecifier;
  } else
    AdjustResultPriorityForDecl(R);

  // If this result is supposed to have an informative qualifier, add one.
  if (R.QualifierIsInformative && !R.Qualifier &&
      !R.StartsNestedNameSpecifier) {
    const DeclContext *Ctx = R.Declaration->getDeclContext();
    if (const NamespaceDecl *Namespace = dyn_cast<NamespaceDecl>(Ctx))
      R.Qualifier =
          NestedNameSpecifier::Create(SemaRef.Context, nullptr, Namespace);
    else if (const TagDecl *Tag = dyn_cast<TagDecl>(Ctx))
      R.Qualifier = NestedNameSpecifier::Create(
          SemaRef.Context, nullptr, false,
          SemaRef.Context.getTypeDeclType(Tag).getTypePtr());
    else
      R.QualifierIsInformative = false;
  }

  // Insert this result into the set of results and into the current shadow
  // map.
  SMap[R.Declaration->getDeclName()].Add(R.Declaration, Results.size());
  Results.push_back(R);

  if (!AsNestedNameSpecifier)
    MaybeAddConstructorResults(R);
}

static void setInBaseClass(ResultBuilder::Result &R) {
  R.Priority += CCD_InBaseClass;
  R.InBaseClass = true;
}

enum class OverloadCompare { BothViable, Dominates, Dominated };
// Will Candidate ever be called on the object, when overloaded with Incumbent?
// Returns Dominates if Candidate is always called, Dominated if Incumbent is
// always called, BothViable if either may be called dependending on arguments.
// Precondition: must actually be overloads!
static OverloadCompare compareOverloads(const CXXMethodDecl &Candidate,
                                        const CXXMethodDecl &Incumbent,
                                        const Qualifiers &ObjectQuals,
                                        ExprValueKind ObjectKind) {
  if (Candidate.isVariadic() != Incumbent.isVariadic() ||
      Candidate.getNumParams() != Incumbent.getNumParams() ||
      Candidate.getMinRequiredArguments() !=
          Incumbent.getMinRequiredArguments())
    return OverloadCompare::BothViable;
  for (unsigned I = 0, E = Candidate.getNumParams(); I != E; ++I)
    if (Candidate.parameters()[I]->getType().getCanonicalType() !=
        Incumbent.parameters()[I]->getType().getCanonicalType())
      return OverloadCompare::BothViable;
  if (!llvm::empty(Candidate.specific_attrs<EnableIfAttr>()) ||
      !llvm::empty(Incumbent.specific_attrs<EnableIfAttr>()))
    return OverloadCompare::BothViable;
  // At this point, we know calls can't pick one or the other based on
  // arguments, so one of the two must win. (Or both fail, handled elsewhere).
  RefQualifierKind CandidateRef = Candidate.getRefQualifier();
  RefQualifierKind IncumbentRef = Incumbent.getRefQualifier();
  if (CandidateRef != IncumbentRef) {
    // If the object kind is LValue/RValue, there's one acceptable ref-qualifier
    // and it can't be mixed with ref-unqualified overloads (in valid code).

    // For xvalue objects, we prefer the rvalue overload even if we have to
    // add qualifiers (which is rare, because const&& is rare).
    if (ObjectKind == clang::VK_XValue)
      return CandidateRef == RQ_RValue ? OverloadCompare::Dominates
                                       : OverloadCompare::Dominated;
  }
  // Now the ref qualifiers are the same (or we're in some invalid state).
  // So make some decision based on the qualifiers.
  Qualifiers CandidateQual = Candidate.getMethodQualifiers();
  Qualifiers IncumbentQual = Incumbent.getMethodQualifiers();
  bool CandidateSuperset = CandidateQual.compatiblyIncludes(IncumbentQual);
  bool IncumbentSuperset = IncumbentQual.compatiblyIncludes(CandidateQual);
  if (CandidateSuperset == IncumbentSuperset)
    return OverloadCompare::BothViable;
  return IncumbentSuperset ? OverloadCompare::Dominates
                           : OverloadCompare::Dominated;
}

void ResultBuilder::AddResult(Result R, DeclContext *CurContext,
                              NamedDecl *Hiding, bool InBaseClass = false) {
  if (R.Kind != Result::RK_Declaration) {
    // For non-declaration results, just add the result.
    Results.push_back(R);
    return;
  }

  // Look through using declarations.
  if (const auto *Using = dyn_cast<UsingShadowDecl>(R.Declaration)) {
    CodeCompletionResult Result(Using->getTargetDecl(),
                                getBasePriority(Using->getTargetDecl()),
                                R.Qualifier);
    Result.ShadowDecl = Using;
    AddResult(Result, CurContext, Hiding);
    return;
  }

  bool AsNestedNameSpecifier = false;
  if (!isInterestingDecl(R.Declaration, AsNestedNameSpecifier))
    return;

  // C++ constructors are never found by name lookup.
  if (isConstructor(R.Declaration))
    return;

  if (Hiding && CheckHiddenResult(R, CurContext, Hiding))
    return;

  // Make sure that any given declaration only shows up in the result set once.
  if (!AllDeclsFound.insert(R.Declaration->getCanonicalDecl()).second)
    return;

  // If the filter is for nested-name-specifiers, then this result starts a
  // nested-name-specifier.
  if (AsNestedNameSpecifier) {
    R.StartsNestedNameSpecifier = true;
    R.Priority = CCP_NestedNameSpecifier;
  } else if (Filter == &ResultBuilder::IsMember && !R.Qualifier &&
             InBaseClass &&
             isa<CXXRecordDecl>(
                 R.Declaration->getDeclContext()->getRedeclContext()))
    R.QualifierIsInformative = true;

  // If this result is supposed to have an informative qualifier, add one.
  if (R.QualifierIsInformative && !R.Qualifier &&
      !R.StartsNestedNameSpecifier) {
    const DeclContext *Ctx = R.Declaration->getDeclContext();
    if (const auto *Namespace = dyn_cast<NamespaceDecl>(Ctx))
      R.Qualifier =
          NestedNameSpecifier::Create(SemaRef.Context, nullptr, Namespace);
    else if (const auto *Tag = dyn_cast<TagDecl>(Ctx))
      R.Qualifier = NestedNameSpecifier::Create(
          SemaRef.Context, nullptr, false,
          SemaRef.Context.getTypeDeclType(Tag).getTypePtr());
    else
      R.QualifierIsInformative = false;
  }

  // Adjust the priority if this result comes from a base class.
  if (InBaseClass)
    setInBaseClass(R);

  AdjustResultPriorityForDecl(R);

  if (HasObjectTypeQualifiers)
    if (const auto *Method = dyn_cast<CXXMethodDecl>(R.Declaration))
      if (Method->isInstance()) {
        Qualifiers MethodQuals = Method->getMethodQualifiers();
        if (ObjectTypeQualifiers == MethodQuals)
          R.Priority += CCD_ObjectQualifierMatch;
        else if (ObjectTypeQualifiers - MethodQuals) {
          // The method cannot be invoked, because doing so would drop
          // qualifiers.
          return;
        }
        // Detect cases where a ref-qualified method cannot be invoked.
        switch (Method->getRefQualifier()) {
          case RQ_LValue:
            if (ObjectKind != VK_LValue && !MethodQuals.hasConst())
              return;
            break;
          case RQ_RValue:
            if (ObjectKind == VK_LValue)
              return;
            break;
          case RQ_None:
            break;
        }

        /// Check whether this dominates another overloaded method, which should
        /// be suppressed (or vice versa).
        /// Motivating case is const_iterator begin() const vs iterator begin().
        auto &OverloadSet = OverloadMap[std::make_pair(
            CurContext, Method->getDeclName().getAsOpaqueInteger())];
        for (const DeclIndexPair& Entry : OverloadSet) {
          Result &Incumbent = Results[Entry.second];
          switch (compareOverloads(*Method,
                                   *cast<CXXMethodDecl>(Incumbent.Declaration),
                                   ObjectTypeQualifiers, ObjectKind)) {
          case OverloadCompare::Dominates:
            // Replace the dominated overload with this one.
            // FIXME: if the overload dominates multiple incumbents then we
            // should remove all. But two overloads is by far the common case.
            Incumbent = std::move(R);
            return;
          case OverloadCompare::Dominated:
            // This overload can't be called, drop it.
            return;
          case OverloadCompare::BothViable:
            break;
          }
        }
        OverloadSet.Add(Method, Results.size());
      }

  // Insert this result into the set of results.
  Results.push_back(R);

  if (!AsNestedNameSpecifier)
    MaybeAddConstructorResults(R);
}

void ResultBuilder::AddResult(Result R) {
  assert(R.Kind != Result::RK_Declaration &&
         "Declaration results need more context");
  Results.push_back(R);
}

/// Enter into a new scope.
void ResultBuilder::EnterNewScope() { ShadowMaps.emplace_back(); }

/// Exit from the current scope.
void ResultBuilder::ExitScope() {
  ShadowMaps.pop_back();
}

/// Determines whether this given declaration will be found by
/// ordinary name lookup.
bool ResultBuilder::IsOrdinaryName(const NamedDecl *ND) const {
  ND = ND->getUnderlyingDecl();

  // If name lookup finds a local extern declaration, then we are in a
  // context where it behaves like an ordinary name.
  unsigned IDNS = Decl::IDNS_Ordinary | Decl::IDNS_LocalExtern;
  if (SemaRef.getLangOpts().CPlusPlus)
    IDNS |= Decl::IDNS_Tag | Decl::IDNS_Namespace | Decl::IDNS_Member;
  else if (SemaRef.getLangOpts().ObjC) {
    if (isa<ObjCIvarDecl>(ND))
      return true;
  }

  return ND->getIdentifierNamespace() & IDNS;
}

/// Determines whether this given declaration will be found by
/// ordinary name lookup but is not a type name.
bool ResultBuilder::IsOrdinaryNonTypeName(const NamedDecl *ND) const {
  ND = ND->getUnderlyingDecl();
  if (isa<TypeDecl>(ND))
    return false;
  // Objective-C interfaces names are not filtered by this method because they
  // can be used in a class property expression. We can still filter out
  // @class declarations though.
  if (const auto *ID = dyn_cast<ObjCInterfaceDecl>(ND)) {
    if (!ID->getDefinition())
      return false;
  }

  unsigned IDNS = Decl::IDNS_Ordinary | Decl::IDNS_LocalExtern;
  if (SemaRef.getLangOpts().CPlusPlus)
    IDNS |= Decl::IDNS_Tag | Decl::IDNS_Namespace | Decl::IDNS_Member;
  else if (SemaRef.getLangOpts().ObjC) {
    if (isa<ObjCIvarDecl>(ND))
      return true;
  }

  return ND->getIdentifierNamespace() & IDNS;
}

bool ResultBuilder::IsIntegralConstantValue(const NamedDecl *ND) const {
  if (!IsOrdinaryNonTypeName(ND))
    return 0;

  if (const auto *VD = dyn_cast<ValueDecl>(ND->getUnderlyingDecl()))
    if (VD->getType()->isIntegralOrEnumerationType())
      return true;

  return false;
}

/// Determines whether this given declaration will be found by
/// ordinary name lookup.
bool ResultBuilder::IsOrdinaryNonValueName(const NamedDecl *ND) const {
  ND = ND->getUnderlyingDecl();

  unsigned IDNS = Decl::IDNS_Ordinary | Decl::IDNS_LocalExtern;
  if (SemaRef.getLangOpts().CPlusPlus)
    IDNS |= Decl::IDNS_Tag | Decl::IDNS_Namespace;

  return (ND->getIdentifierNamespace() & IDNS) && !isa<ValueDecl>(ND) &&
         !isa<FunctionTemplateDecl>(ND) && !isa<ObjCPropertyDecl>(ND);
}

/// Determines whether the given declaration is suitable as the
/// start of a C++ nested-name-specifier, e.g., a class or namespace.
bool ResultBuilder::IsNestedNameSpecifier(const NamedDecl *ND) const {
  // Allow us to find class templates, too.
  if (const auto *ClassTemplate = dyn_cast<ClassTemplateDecl>(ND))
    ND = ClassTemplate->getTemplatedDecl();

  return SemaRef.isAcceptableNestedNameSpecifier(ND);
}

/// Determines whether the given declaration is an enumeration.
bool ResultBuilder::IsEnum(const NamedDecl *ND) const {
  return isa<EnumDecl>(ND);
}

/// Determines whether the given declaration is a class or struct.
bool ResultBuilder::IsClassOrStruct(const NamedDecl *ND) const {
  // Allow us to find class templates, too.
  if (const auto *ClassTemplate = dyn_cast<ClassTemplateDecl>(ND))
    ND = ClassTemplate->getTemplatedDecl();

  // For purposes of this check, interfaces match too.
  if (const auto *RD = dyn_cast<RecordDecl>(ND))
    return RD->getTagKind() == TTK_Class || RD->getTagKind() == TTK_Struct ||
           RD->getTagKind() == TTK_Interface;

  return false;
}

/// Determines whether the given declaration is a union.
bool ResultBuilder::IsUnion(const NamedDecl *ND) const {
  // Allow us to find class templates, too.
  if (const auto *ClassTemplate = dyn_cast<ClassTemplateDecl>(ND))
    ND = ClassTemplate->getTemplatedDecl();

  if (const auto *RD = dyn_cast<RecordDecl>(ND))
    return RD->getTagKind() == TTK_Union;

  return false;
}

/// Determines whether the given declaration is a namespace.
bool ResultBuilder::IsNamespace(const NamedDecl *ND) const {
  return isa<NamespaceDecl>(ND);
}

/// Determines whether the given declaration is a namespace or
/// namespace alias.
bool ResultBuilder::IsNamespaceOrAlias(const NamedDecl *ND) const {
  return isa<NamespaceDecl>(ND->getUnderlyingDecl());
}

/// Determines whether the given declaration is a type.
bool ResultBuilder::IsType(const NamedDecl *ND) const {
  ND = ND->getUnderlyingDecl();
  return isa<TypeDecl>(ND) || isa<ObjCInterfaceDecl>(ND);
}

/// Determines which members of a class should be visible via
/// "." or "->".  Only value declarations, nested name specifiers, and
/// using declarations thereof should show up.
bool ResultBuilder::IsMember(const NamedDecl *ND) const {
  ND = ND->getUnderlyingDecl();
  return isa<ValueDecl>(ND) || isa<FunctionTemplateDecl>(ND) ||
         isa<ObjCPropertyDecl>(ND);
}

static bool isObjCReceiverType(ASTContext &C, QualType T) {
  T = C.getCanonicalType(T);
  switch (T->getTypeClass()) {
  case Type::ObjCObject:
  case Type::ObjCInterface:
  case Type::ObjCObjectPointer:
    return true;

  case Type::Builtin:
    switch (cast<BuiltinType>(T)->getKind()) {
    case BuiltinType::ObjCId:
    case BuiltinType::ObjCClass:
    case BuiltinType::ObjCSel:
      return true;

    default:
      break;
    }
    return false;

  default:
    break;
  }

  if (!C.getLangOpts().CPlusPlus)
    return false;

  // FIXME: We could perform more analysis here to determine whether a
  // particular class type has any conversions to Objective-C types. For now,
  // just accept all class types.
  return T->isDependentType() || T->isRecordType();
}

bool ResultBuilder::IsObjCMessageReceiver(const NamedDecl *ND) const {
  QualType T = getDeclUsageType(SemaRef.Context, ND);
  if (T.isNull())
    return false;

  T = SemaRef.Context.getBaseElementType(T);
  return isObjCReceiverType(SemaRef.Context, T);
}

bool ResultBuilder::IsObjCMessageReceiverOrLambdaCapture(
    const NamedDecl *ND) const {
  if (IsObjCMessageReceiver(ND))
    return true;

  const auto *Var = dyn_cast<VarDecl>(ND);
  if (!Var)
    return false;

  return Var->hasLocalStorage() && !Var->hasAttr<BlocksAttr>();
}

bool ResultBuilder::IsObjCCollection(const NamedDecl *ND) const {
  if ((SemaRef.getLangOpts().CPlusPlus && !IsOrdinaryName(ND)) ||
      (!SemaRef.getLangOpts().CPlusPlus && !IsOrdinaryNonTypeName(ND)))
    return false;

  QualType T = getDeclUsageType(SemaRef.Context, ND);
  if (T.isNull())
    return false;

  T = SemaRef.Context.getBaseElementType(T);
  return T->isObjCObjectType() || T->isObjCObjectPointerType() ||
         T->isObjCIdType() ||
         (SemaRef.getLangOpts().CPlusPlus && T->isRecordType());
}

bool ResultBuilder::IsImpossibleToSatisfy(const NamedDecl *ND) const {
  return false;
}

/// Determines whether the given declaration is an Objective-C
/// instance variable.
bool ResultBuilder::IsObjCIvar(const NamedDecl *ND) const {
  return isa<ObjCIvarDecl>(ND);
}

namespace {

/// Visible declaration consumer that adds a code-completion result
/// for each visible declaration.
class CodeCompletionDeclConsumer : public VisibleDeclConsumer {
  ResultBuilder &Results;
  DeclContext *InitialLookupCtx;
  // NamingClass and BaseType are used for access-checking. See
  // Sema::IsSimplyAccessible for details.
  CXXRecordDecl *NamingClass;
  QualType BaseType;
  std::vector<FixItHint> FixIts;

public:
  CodeCompletionDeclConsumer(
      ResultBuilder &Results, DeclContext *InitialLookupCtx,
      QualType BaseType = QualType(),
      std::vector<FixItHint> FixIts = std::vector<FixItHint>())
      : Results(Results), InitialLookupCtx(InitialLookupCtx),
        FixIts(std::move(FixIts)) {
    NamingClass = llvm::dyn_cast<CXXRecordDecl>(InitialLookupCtx);
    // If BaseType was not provided explicitly, emulate implicit 'this->'.
    if (BaseType.isNull()) {
      auto ThisType = Results.getSema().getCurrentThisType();
      if (!ThisType.isNull()) {
        assert(ThisType->isPointerType());
        BaseType = ThisType->getPointeeType();
        if (!NamingClass)
          NamingClass = BaseType->getAsCXXRecordDecl();
      }
    }
    this->BaseType = BaseType;
  }

  void FoundDecl(NamedDecl *ND, NamedDecl *Hiding, DeclContext *Ctx,
                 bool InBaseClass) override {
    ResultBuilder::Result Result(ND, Results.getBasePriority(ND), nullptr,
                                 false, IsAccessible(ND, Ctx), FixIts);
    Results.AddResult(Result, InitialLookupCtx, Hiding, InBaseClass);
  }

  void EnteredContext(DeclContext *Ctx) override {
    Results.addVisitedContext(Ctx);
  }

private:
  bool IsAccessible(NamedDecl *ND, DeclContext *Ctx) {
    // Naming class to use for access check. In most cases it was provided
    // explicitly (e.g. member access (lhs.foo) or qualified lookup (X::)),
    // for unqualified lookup we fallback to the \p Ctx in which we found the
    // member.
    auto *NamingClass = this->NamingClass;
    QualType BaseType = this->BaseType;
    if (auto *Cls = llvm::dyn_cast_or_null<CXXRecordDecl>(Ctx)) {
      if (!NamingClass)
        NamingClass = Cls;
      // When we emulate implicit 'this->' in an unqualified lookup, we might
      // end up with an invalid naming class. In that case, we avoid emulating
      // 'this->' qualifier to satisfy preconditions of the access checking.
      if (NamingClass->getCanonicalDecl() != Cls->getCanonicalDecl() &&
          !NamingClass->isDerivedFrom(Cls)) {
        NamingClass = Cls;
        BaseType = QualType();
      }
    } else {
      // The decl was found outside the C++ class, so only ObjC access checks
      // apply. Those do not rely on NamingClass and BaseType, so we clear them
      // out.
      NamingClass = nullptr;
      BaseType = QualType();
    }
    return Results.getSema().IsSimplyAccessible(ND, NamingClass, BaseType);
  }
};
} // namespace

/// Add type specifiers for the current language as keyword results.
static void AddTypeSpecifierResults(const LangOptions &LangOpts,
                                    ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  Results.AddResult(Result("short", CCP_Type));
  Results.AddResult(Result("long", CCP_Type));
  Results.AddResult(Result("signed", CCP_Type));
  Results.AddResult(Result("unsigned", CCP_Type));
  Results.AddResult(Result("void", CCP_Type));
  Results.AddResult(Result("char", CCP_Type));
  Results.AddResult(Result("int", CCP_Type));
  Results.AddResult(Result("float", CCP_Type));
  Results.AddResult(Result("double", CCP_Type));
  Results.AddResult(Result("enum", CCP_Type));
  Results.AddResult(Result("struct", CCP_Type));
  Results.AddResult(Result("union", CCP_Type));
  Results.AddResult(Result("const", CCP_Type));
  Results.AddResult(Result("volatile", CCP_Type));

  if (LangOpts.C99) {
    // C99-specific
    Results.AddResult(Result("_Complex", CCP_Type));
    Results.AddResult(Result("_Imaginary", CCP_Type));
    Results.AddResult(Result("_Bool", CCP_Type));
    Results.AddResult(Result("restrict", CCP_Type));
  }

  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());
  if (LangOpts.CPlusPlus) {
    // C++-specific
    Results.AddResult(
        Result("bool", CCP_Type + (LangOpts.ObjC ? CCD_bool_in_ObjC : 0)));
    Results.AddResult(Result("class", CCP_Type));
    Results.AddResult(Result("wchar_t", CCP_Type));

    // typename qualified-id
    Builder.AddTypedTextChunk("typename");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("qualifier");
    Builder.AddTextChunk("::");
    Builder.AddPlaceholderChunk("name");
    Results.AddResult(Result(Builder.TakeString()));

    if (LangOpts.CPlusPlus11) {
      Results.AddResult(Result("auto", CCP_Type));
      Results.AddResult(Result("char16_t", CCP_Type));
      Results.AddResult(Result("char32_t", CCP_Type));

      Builder.AddTypedTextChunk("decltype");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));
    }
  } else
    Results.AddResult(Result("__auto_type", CCP_Type));

  // GNU keywords
  if (LangOpts.GNUKeywords) {
    // FIXME: Enable when we actually support decimal floating point.
    //    Results.AddResult(Result("_Decimal32"));
    //    Results.AddResult(Result("_Decimal64"));
    //    Results.AddResult(Result("_Decimal128"));

    Builder.AddTypedTextChunk("typeof");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("expression");
    Results.AddResult(Result(Builder.TakeString()));

    Builder.AddTypedTextChunk("typeof");
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("type");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(Result(Builder.TakeString()));
  }

  // Nullability
  Results.AddResult(Result("_Nonnull", CCP_Type));
  Results.AddResult(Result("_Null_unspecified", CCP_Type));
  Results.AddResult(Result("_Nullable", CCP_Type));
}

static void AddStorageSpecifiers(Sema::ParserCompletionContext CCC,
                                 const LangOptions &LangOpts,
                                 ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  // Note: we don't suggest either "auto" or "register", because both
  // are pointless as storage specifiers. Elsewhere, we suggest "auto"
  // in C++0x as a type specifier.
  Results.AddResult(Result("extern"));
  Results.AddResult(Result("static"));

  if (LangOpts.CPlusPlus11) {
    CodeCompletionAllocator &Allocator = Results.getAllocator();
    CodeCompletionBuilder Builder(Allocator, Results.getCodeCompletionTUInfo());

    // alignas
    Builder.AddTypedTextChunk("alignas");
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("expression");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(Result(Builder.TakeString()));

    Results.AddResult(Result("constexpr"));
    Results.AddResult(Result("thread_local"));
  }
}

static void AddFunctionSpecifiers(Sema::ParserCompletionContext CCC,
                                  const LangOptions &LangOpts,
                                  ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  switch (CCC) {
  case Sema::PCC_Class:
  case Sema::PCC_MemberTemplate:
    if (LangOpts.CPlusPlus) {
      Results.AddResult(Result("explicit"));
      Results.AddResult(Result("friend"));
      Results.AddResult(Result("mutable"));
      Results.AddResult(Result("virtual"));
    }
    LLVM_FALLTHROUGH;

  case Sema::PCC_ObjCInterface:
  case Sema::PCC_ObjCImplementation:
  case Sema::PCC_Namespace:
  case Sema::PCC_Template:
    if (LangOpts.CPlusPlus || LangOpts.C99)
      Results.AddResult(Result("inline"));
    break;

  case Sema::PCC_ObjCInstanceVariableList:
  case Sema::PCC_Expression:
  case Sema::PCC_Statement:
  case Sema::PCC_ForInit:
  case Sema::PCC_Condition:
  case Sema::PCC_RecoveryInFunction:
  case Sema::PCC_Type:
  case Sema::PCC_ParenthesizedExpression:
  case Sema::PCC_LocalDeclarationSpecifiers:
    break;
  }
}

static void AddObjCExpressionResults(ResultBuilder &Results, bool NeedAt);
static void AddObjCStatementResults(ResultBuilder &Results, bool NeedAt);
static void AddObjCVisibilityResults(const LangOptions &LangOpts,
                                     ResultBuilder &Results, bool NeedAt);
static void AddObjCImplementationResults(const LangOptions &LangOpts,
                                         ResultBuilder &Results, bool NeedAt);
static void AddObjCInterfaceResults(const LangOptions &LangOpts,
                                    ResultBuilder &Results, bool NeedAt);
static void AddObjCTopLevelResults(ResultBuilder &Results, bool NeedAt);

static void AddTypedefResult(ResultBuilder &Results) {
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());
  Builder.AddTypedTextChunk("typedef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("type");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("name");
  Builder.AddChunk(CodeCompletionString::CK_SemiColon);
  Results.AddResult(CodeCompletionResult(Builder.TakeString()));
}

static bool WantTypesInContext(Sema::ParserCompletionContext CCC,
                               const LangOptions &LangOpts) {
  switch (CCC) {
  case Sema::PCC_Namespace:
  case Sema::PCC_Class:
  case Sema::PCC_ObjCInstanceVariableList:
  case Sema::PCC_Template:
  case Sema::PCC_MemberTemplate:
  case Sema::PCC_Statement:
  case Sema::PCC_RecoveryInFunction:
  case Sema::PCC_Type:
  case Sema::PCC_ParenthesizedExpression:
  case Sema::PCC_LocalDeclarationSpecifiers:
    return true;

  case Sema::PCC_Expression:
  case Sema::PCC_Condition:
    return LangOpts.CPlusPlus;

  case Sema::PCC_ObjCInterface:
  case Sema::PCC_ObjCImplementation:
    return false;

  case Sema::PCC_ForInit:
    return LangOpts.CPlusPlus || LangOpts.ObjC || LangOpts.C99;
  }

  llvm_unreachable("Invalid ParserCompletionContext!");
}

static PrintingPolicy getCompletionPrintingPolicy(const ASTContext &Context,
                                                  const Preprocessor &PP) {
  PrintingPolicy Policy = Sema::getPrintingPolicy(Context, PP);
  Policy.AnonymousTagLocations = false;
  Policy.SuppressStrongLifetime = true;
  Policy.SuppressUnwrittenScope = true;
  Policy.SuppressScope = true;
  return Policy;
}

/// Retrieve a printing policy suitable for code completion.
static PrintingPolicy getCompletionPrintingPolicy(Sema &S) {
  return getCompletionPrintingPolicy(S.Context, S.PP);
}

/// Retrieve the string representation of the given type as a string
/// that has the appropriate lifetime for code completion.
///
/// This routine provides a fast path where we provide constant strings for
/// common type names.
static const char *GetCompletionTypeString(QualType T, ASTContext &Context,
                                           const PrintingPolicy &Policy,
                                           CodeCompletionAllocator &Allocator) {
  if (!T.getLocalQualifiers()) {
    // Built-in type names are constant strings.
    if (const BuiltinType *BT = dyn_cast<BuiltinType>(T))
      return BT->getNameAsCString(Policy);

    // Anonymous tag types are constant strings.
    if (const TagType *TagT = dyn_cast<TagType>(T))
      if (TagDecl *Tag = TagT->getDecl())
        if (!Tag->hasNameForLinkage()) {
          switch (Tag->getTagKind()) {
          case TTK_Struct:
            return "struct <anonymous>";
          case TTK_Interface:
            return "__interface <anonymous>";
          case TTK_Class:
            return "class <anonymous>";
          case TTK_Union:
            return "union <anonymous>";
          case TTK_Enum:
            return "enum <anonymous>";
          }
        }
  }

  // Slow path: format the type as a string.
  std::string Result;
  T.getAsStringInternal(Result, Policy);
  return Allocator.CopyString(Result);
}

/// Add a completion for "this", if we're in a member function.
static void addThisCompletion(Sema &S, ResultBuilder &Results) {
  QualType ThisTy = S.getCurrentThisType();
  if (ThisTy.isNull())
    return;

  CodeCompletionAllocator &Allocator = Results.getAllocator();
  CodeCompletionBuilder Builder(Allocator, Results.getCodeCompletionTUInfo());
  PrintingPolicy Policy = getCompletionPrintingPolicy(S);
  Builder.AddResultTypeChunk(
      GetCompletionTypeString(ThisTy, S.Context, Policy, Allocator));
  Builder.AddTypedTextChunk("this");
  Results.AddResult(CodeCompletionResult(Builder.TakeString()));
}

static void AddStaticAssertResult(CodeCompletionBuilder &Builder,
                                  ResultBuilder &Results,
                                  const LangOptions &LangOpts) {
  if (!LangOpts.CPlusPlus11)
    return;

  Builder.AddTypedTextChunk("static_assert");
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("expression");
  Builder.AddChunk(CodeCompletionString::CK_Comma);
  Builder.AddPlaceholderChunk("message");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Builder.AddChunk(CodeCompletionString::CK_SemiColon);
  Results.AddResult(CodeCompletionResult(Builder.TakeString()));
}

static void AddOverrideResults(ResultBuilder &Results,
                               const CodeCompletionContext &CCContext,
                               CodeCompletionBuilder &Builder) {
  Sema &S = Results.getSema();
  const auto *CR = llvm::dyn_cast<CXXRecordDecl>(S.CurContext);
  // If not inside a class/struct/union return empty.
  if (!CR)
    return;
  // First store overrides within current class.
  // These are stored by name to make querying fast in the later step.
  llvm::StringMap<std::vector<FunctionDecl *>> Overrides;
  for (auto *Method : CR->methods()) {
    if (!Method->isVirtual() || !Method->getIdentifier())
      continue;
    Overrides[Method->getName()].push_back(Method);
  }

  for (const auto &Base : CR->bases()) {
    const auto *BR = Base.getType().getTypePtr()->getAsCXXRecordDecl();
    if (!BR)
      continue;
    for (auto *Method : BR->methods()) {
      if (!Method->isVirtual() || !Method->getIdentifier())
        continue;
      const auto it = Overrides.find(Method->getName());
      bool IsOverriden = false;
      if (it != Overrides.end()) {
        for (auto *MD : it->second) {
          // If the method in current body is not an overload of this virtual
          // function, then it overrides this one.
          if (!S.IsOverload(MD, Method, false)) {
            IsOverriden = true;
            break;
          }
        }
      }
      if (!IsOverriden) {
        // Generates a new CodeCompletionResult by taking this function and
        // converting it into an override declaration with only one chunk in the
        // final CodeCompletionString as a TypedTextChunk.
        std::string OverrideSignature;
        llvm::raw_string_ostream OS(OverrideSignature);
        CodeCompletionResult CCR(Method, 0);
        PrintingPolicy Policy =
            getCompletionPrintingPolicy(S.getASTContext(), S.getPreprocessor());
        auto *CCS = CCR.createCodeCompletionStringForOverride(
            S.getPreprocessor(), S.getASTContext(), Builder,
            /*IncludeBriefComments=*/false, CCContext, Policy);
        Results.AddResult(CodeCompletionResult(CCS, Method, CCP_CodePattern));
      }
    }
  }
}

/// Add language constructs that show up for "ordinary" names.
static void AddOrdinaryNameResults(Sema::ParserCompletionContext CCC, Scope *S,
                                   Sema &SemaRef, ResultBuilder &Results) {
  CodeCompletionAllocator &Allocator = Results.getAllocator();
  CodeCompletionBuilder Builder(Allocator, Results.getCodeCompletionTUInfo());

  typedef CodeCompletionResult Result;
  switch (CCC) {
  case Sema::PCC_Namespace:
    if (SemaRef.getLangOpts().CPlusPlus) {
      if (Results.includeCodePatterns()) {
        // namespace <identifier> { declarations }
        Builder.AddTypedTextChunk("namespace");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("identifier");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
        Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
        Builder.AddPlaceholderChunk("declarations");
        Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
        Builder.AddChunk(CodeCompletionString::CK_RightBrace);
        Results.AddResult(Result(Builder.TakeString()));
      }

      // namespace identifier = identifier ;
      Builder.AddTypedTextChunk("namespace");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("name");
      Builder.AddChunk(CodeCompletionString::CK_Equal);
      Builder.AddPlaceholderChunk("namespace");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Results.AddResult(Result(Builder.TakeString()));

      // Using directives
      Builder.AddTypedTextChunk("using namespace");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("identifier");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Results.AddResult(Result(Builder.TakeString()));

      // asm(string-literal)
      Builder.AddTypedTextChunk("asm");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("string-literal");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      if (Results.includeCodePatterns()) {
        // Explicit template instantiation
        Builder.AddTypedTextChunk("template");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("declaration");
        Results.AddResult(Result(Builder.TakeString()));
      } else {
        Results.AddResult(Result("template", CodeCompletionResult::RK_Keyword));
      }
    }

    if (SemaRef.getLangOpts().ObjC)
      AddObjCTopLevelResults(Results, true);

    AddTypedefResult(Results);
    LLVM_FALLTHROUGH;

  case Sema::PCC_Class:
    if (SemaRef.getLangOpts().CPlusPlus) {
      // Using declaration
      Builder.AddTypedTextChunk("using");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("qualifier");
      Builder.AddTextChunk("::");
      Builder.AddPlaceholderChunk("name");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Results.AddResult(Result(Builder.TakeString()));

      // using typename qualifier::name (only in a dependent context)
      if (SemaRef.CurContext->isDependentContext()) {
        Builder.AddTypedTextChunk("using typename");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("qualifier");
        Builder.AddTextChunk("::");
        Builder.AddPlaceholderChunk("name");
        Builder.AddChunk(CodeCompletionString::CK_SemiColon);
        Results.AddResult(Result(Builder.TakeString()));
      }

      AddStaticAssertResult(Builder, Results, SemaRef.getLangOpts());

      if (CCC == Sema::PCC_Class) {
        AddTypedefResult(Results);

        bool IsNotInheritanceScope =
            !(S->getFlags() & Scope::ClassInheritanceScope);
        // public:
        Builder.AddTypedTextChunk("public");
        if (IsNotInheritanceScope && Results.includeCodePatterns())
          Builder.AddChunk(CodeCompletionString::CK_Colon);
        Results.AddResult(Result(Builder.TakeString()));

        // protected:
        Builder.AddTypedTextChunk("protected");
        if (IsNotInheritanceScope && Results.includeCodePatterns())
          Builder.AddChunk(CodeCompletionString::CK_Colon);
        Results.AddResult(Result(Builder.TakeString()));

        // private:
        Builder.AddTypedTextChunk("private");
        if (IsNotInheritanceScope && Results.includeCodePatterns())
          Builder.AddChunk(CodeCompletionString::CK_Colon);
        Results.AddResult(Result(Builder.TakeString()));

        // FIXME: This adds override results only if we are at the first word of
        // the declaration/definition. Also call this from other sides to have
        // more use-cases.
        AddOverrideResults(Results, CodeCompletionContext::CCC_ClassStructUnion,
                           Builder);
      }
    }
    LLVM_FALLTHROUGH;

  case Sema::PCC_Template:
  case Sema::PCC_MemberTemplate:
    if (SemaRef.getLangOpts().CPlusPlus && Results.includeCodePatterns()) {
      // template < parameters >
      Builder.AddTypedTextChunk("template");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("parameters");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Results.AddResult(Result(Builder.TakeString()));
    } else {
      Results.AddResult(Result("template", CodeCompletionResult::RK_Keyword));
    }

    AddStorageSpecifiers(CCC, SemaRef.getLangOpts(), Results);
    AddFunctionSpecifiers(CCC, SemaRef.getLangOpts(), Results);
    break;

  case Sema::PCC_ObjCInterface:
    AddObjCInterfaceResults(SemaRef.getLangOpts(), Results, true);
    AddStorageSpecifiers(CCC, SemaRef.getLangOpts(), Results);
    AddFunctionSpecifiers(CCC, SemaRef.getLangOpts(), Results);
    break;

  case Sema::PCC_ObjCImplementation:
    AddObjCImplementationResults(SemaRef.getLangOpts(), Results, true);
    AddStorageSpecifiers(CCC, SemaRef.getLangOpts(), Results);
    AddFunctionSpecifiers(CCC, SemaRef.getLangOpts(), Results);
    break;

  case Sema::PCC_ObjCInstanceVariableList:
    AddObjCVisibilityResults(SemaRef.getLangOpts(), Results, true);
    break;

  case Sema::PCC_RecoveryInFunction:
  case Sema::PCC_Statement: {
    AddTypedefResult(Results);

    if (SemaRef.getLangOpts().CPlusPlus && Results.includeCodePatterns() &&
        SemaRef.getLangOpts().CXXExceptions) {
      Builder.AddTypedTextChunk("try");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTextChunk("catch");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("declaration");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));
    }
    if (SemaRef.getLangOpts().ObjC)
      AddObjCStatementResults(Results, true);

    if (Results.includeCodePatterns()) {
      // if (condition) { statements }
      Builder.AddTypedTextChunk("if");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOpts().CPlusPlus)
        Builder.AddPlaceholderChunk("condition");
      else
        Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));

      // switch (condition) { }
      Builder.AddTypedTextChunk("switch");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOpts().CPlusPlus)
        Builder.AddPlaceholderChunk("condition");
      else
        Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("cases");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));
    }

    // Switch-specific statements.
    if (SemaRef.getCurFunction() &&
        !SemaRef.getCurFunction()->SwitchStack.empty()) {
      // case expression:
      Builder.AddTypedTextChunk("case");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_Colon);
      Results.AddResult(Result(Builder.TakeString()));

      // default:
      Builder.AddTypedTextChunk("default");
      Builder.AddChunk(CodeCompletionString::CK_Colon);
      Results.AddResult(Result(Builder.TakeString()));
    }

    if (Results.includeCodePatterns()) {
      /// while (condition) { statements }
      Builder.AddTypedTextChunk("while");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOpts().CPlusPlus)
        Builder.AddPlaceholderChunk("condition");
      else
        Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));

      // do { statements } while ( expression );
      Builder.AddTypedTextChunk("do");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Builder.AddTextChunk("while");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      // for ( for-init-statement ; condition ; expression ) { statements }
      Builder.AddTypedTextChunk("for");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (SemaRef.getLangOpts().CPlusPlus || SemaRef.getLangOpts().C99)
        Builder.AddPlaceholderChunk("init-statement");
      else
        Builder.AddPlaceholderChunk("init-expression");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("condition");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("inc-expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddPlaceholderChunk("statements");
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
      Results.AddResult(Result(Builder.TakeString()));
    }

    if (S->getContinueParent()) {
      // continue ;
      Builder.AddTypedTextChunk("continue");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Results.AddResult(Result(Builder.TakeString()));
    }

    if (S->getBreakParent()) {
      // break ;
      Builder.AddTypedTextChunk("break");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Results.AddResult(Result(Builder.TakeString()));
    }

    // "return expression ;" or "return ;", depending on the return type.
    QualType ReturnType;
    if (const auto *Function = dyn_cast<FunctionDecl>(SemaRef.CurContext))
      ReturnType = Function->getReturnType();
    else if (const auto *Method = dyn_cast<ObjCMethodDecl>(SemaRef.CurContext))
      ReturnType = Method->getReturnType();
    else if (SemaRef.getCurBlock() &&
             !SemaRef.getCurBlock()->ReturnType.isNull())
      ReturnType = SemaRef.getCurBlock()->ReturnType;;
    if (ReturnType.isNull() || ReturnType->isVoidType()) {
      Builder.AddTypedTextChunk("return");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Results.AddResult(Result(Builder.TakeString()));
    } else {
      assert(!ReturnType.isNull());
      // "return expression ;"
      Builder.AddTypedTextChunk("return");
      Builder.AddChunk(clang::CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      Results.AddResult(Result(Builder.TakeString()));
      // When boolean, also add 'return true;' and 'return false;'.
      if (ReturnType->isBooleanType()) {
        Builder.AddTypedTextChunk("return true");
        Builder.AddChunk(CodeCompletionString::CK_SemiColon);
        Results.AddResult(Result(Builder.TakeString()));

        Builder.AddTypedTextChunk("return false");
        Builder.AddChunk(CodeCompletionString::CK_SemiColon);
        Results.AddResult(Result(Builder.TakeString()));
      }
    }

    // goto identifier ;
    Builder.AddTypedTextChunk("goto");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("label");
    Builder.AddChunk(CodeCompletionString::CK_SemiColon);
    Results.AddResult(Result(Builder.TakeString()));

    // Using directives
    Builder.AddTypedTextChunk("using namespace");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("identifier");
    Builder.AddChunk(CodeCompletionString::CK_SemiColon);
    Results.AddResult(Result(Builder.TakeString()));

    AddStaticAssertResult(Builder, Results, SemaRef.getLangOpts());
  }
    LLVM_FALLTHROUGH;

  // Fall through (for statement expressions).
  case Sema::PCC_ForInit:
  case Sema::PCC_Condition:
    AddStorageSpecifiers(CCC, SemaRef.getLangOpts(), Results);
    // Fall through: conditions and statements can have expressions.
    LLVM_FALLTHROUGH;

  case Sema::PCC_ParenthesizedExpression:
    if (SemaRef.getLangOpts().ObjCAutoRefCount &&
        CCC == Sema::PCC_ParenthesizedExpression) {
      // (__bridge <type>)<expression>
      Builder.AddTypedTextChunk("__bridge");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));

      // (__bridge_transfer <Objective-C type>)<expression>
      Builder.AddTypedTextChunk("__bridge_transfer");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("Objective-C type");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));

      // (__bridge_retained <CF type>)<expression>
      Builder.AddTypedTextChunk("__bridge_retained");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("CF type");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));
    }
    // Fall through
    LLVM_FALLTHROUGH;

  case Sema::PCC_Expression: {
    if (SemaRef.getLangOpts().CPlusPlus) {
      // 'this', if we're in a non-static member function.
      addThisCompletion(SemaRef, Results);

      // true
      Builder.AddResultTypeChunk("bool");
      Builder.AddTypedTextChunk("true");
      Results.AddResult(Result(Builder.TakeString()));

      // false
      Builder.AddResultTypeChunk("bool");
      Builder.AddTypedTextChunk("false");
      Results.AddResult(Result(Builder.TakeString()));

      if (SemaRef.getLangOpts().RTTI) {
        // dynamic_cast < type-id > ( expression )
        Builder.AddTypedTextChunk("dynamic_cast");
        Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
        Builder.AddPlaceholderChunk("type");
        Builder.AddChunk(CodeCompletionString::CK_RightAngle);
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("expression");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));
      }

      // static_cast < type-id > ( expression )
      Builder.AddTypedTextChunk("static_cast");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      // reinterpret_cast < type-id > ( expression )
      Builder.AddTypedTextChunk("reinterpret_cast");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      // const_cast < type-id > ( expression )
      Builder.AddTypedTextChunk("const_cast");
      Builder.AddChunk(CodeCompletionString::CK_LeftAngle);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightAngle);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expression");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      if (SemaRef.getLangOpts().RTTI) {
        // typeid ( expression-or-type )
        Builder.AddResultTypeChunk("std::type_info");
        Builder.AddTypedTextChunk("typeid");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("expression-or-type");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));
      }

      // new T ( ... )
      Builder.AddTypedTextChunk("new");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expressions");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      // new T [ ] ( ... )
      Builder.AddTypedTextChunk("new");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_LeftBracket);
      Builder.AddPlaceholderChunk("size");
      Builder.AddChunk(CodeCompletionString::CK_RightBracket);
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("expressions");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));

      // delete expression
      Builder.AddResultTypeChunk("void");
      Builder.AddTypedTextChunk("delete");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));

      // delete [] expression
      Builder.AddResultTypeChunk("void");
      Builder.AddTypedTextChunk("delete");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBracket);
      Builder.AddChunk(CodeCompletionString::CK_RightBracket);
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddPlaceholderChunk("expression");
      Results.AddResult(Result(Builder.TakeString()));

      if (SemaRef.getLangOpts().CXXExceptions) {
        // throw expression
        Builder.AddResultTypeChunk("void");
        Builder.AddTypedTextChunk("throw");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("expression");
        Results.AddResult(Result(Builder.TakeString()));
      }

      // FIXME: Rethrow?

      if (SemaRef.getLangOpts().CPlusPlus11) {
        // nullptr
        Builder.AddResultTypeChunk("std::nullptr_t");
        Builder.AddTypedTextChunk("nullptr");
        Results.AddResult(Result(Builder.TakeString()));

        // alignof
        Builder.AddResultTypeChunk("size_t");
        Builder.AddTypedTextChunk("alignof");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("type");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));

        // noexcept
        Builder.AddResultTypeChunk("bool");
        Builder.AddTypedTextChunk("noexcept");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("expression");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));

        // sizeof... expression
        Builder.AddResultTypeChunk("size_t");
        Builder.AddTypedTextChunk("sizeof...");
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("parameter-pack");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
        Results.AddResult(Result(Builder.TakeString()));
      }
    }

    if (SemaRef.getLangOpts().ObjC) {
      // Add "super", if we're in an Objective-C class with a superclass.
      if (ObjCMethodDecl *Method = SemaRef.getCurMethodDecl()) {
        // The interface can be NULL.
        if (ObjCInterfaceDecl *ID = Method->getClassInterface())
          if (ID->getSuperClass()) {
            std::string SuperType;
            SuperType = ID->getSuperClass()->getNameAsString();
            if (Method->isInstanceMethod())
              SuperType += " *";

            Builder.AddResultTypeChunk(Allocator.CopyString(SuperType));
            Builder.AddTypedTextChunk("super");
            Results.AddResult(Result(Builder.TakeString()));
          }
      }

      AddObjCExpressionResults(Results, true);
    }

    if (SemaRef.getLangOpts().C11) {
      // _Alignof
      Builder.AddResultTypeChunk("size_t");
      if (SemaRef.PP.isMacroDefined("alignof"))
        Builder.AddTypedTextChunk("alignof");
      else
        Builder.AddTypedTextChunk("_Alignof");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("type");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Results.AddResult(Result(Builder.TakeString()));
    }

    // sizeof expression
    Builder.AddResultTypeChunk("size_t");
    Builder.AddTypedTextChunk("sizeof");
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("expression-or-type");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(Result(Builder.TakeString()));
    break;
  }

  case Sema::PCC_Type:
  case Sema::PCC_LocalDeclarationSpecifiers:
    break;
  }

  if (WantTypesInContext(CCC, SemaRef.getLangOpts()))
    AddTypeSpecifierResults(SemaRef.getLangOpts(), Results);

  if (SemaRef.getLangOpts().CPlusPlus && CCC != Sema::PCC_Type)
    Results.AddResult(Result("operator"));
}

/// If the given declaration has an associated type, add it as a result
/// type chunk.
static void AddResultTypeChunk(ASTContext &Context,
                               const PrintingPolicy &Policy,
                               const NamedDecl *ND, QualType BaseType,
                               CodeCompletionBuilder &Result) {
  if (!ND)
    return;

  // Skip constructors and conversion functions, which have their return types
  // built into their names.
  if (isConstructor(ND) || isa<CXXConversionDecl>(ND))
    return;

  // Determine the type of the declaration (if it has a type).
  QualType T;
  if (const FunctionDecl *Function = ND->getAsFunction())
    T = Function->getReturnType();
  else if (const auto *Method = dyn_cast<ObjCMethodDecl>(ND)) {
    if (!BaseType.isNull())
      T = Method->getSendResultType(BaseType);
    else
      T = Method->getReturnType();
  } else if (const auto *Enumerator = dyn_cast<EnumConstantDecl>(ND)) {
    T = Context.getTypeDeclType(cast<TypeDecl>(Enumerator->getDeclContext()));
    T = clang::TypeName::getFullyQualifiedType(T, Context);
  } else if (isa<UnresolvedUsingValueDecl>(ND)) {
    /* Do nothing: ignore unresolved using declarations*/
  } else if (const auto *Ivar = dyn_cast<ObjCIvarDecl>(ND)) {
    if (!BaseType.isNull())
      T = Ivar->getUsageType(BaseType);
    else
      T = Ivar->getType();
  } else if (const auto *Value = dyn_cast<ValueDecl>(ND)) {
    T = Value->getType();
  } else if (const auto *Property = dyn_cast<ObjCPropertyDecl>(ND)) {
    if (!BaseType.isNull())
      T = Property->getUsageType(BaseType);
    else
      T = Property->getType();
  }

  if (T.isNull() || Context.hasSameType(T, Context.DependentTy))
    return;

  Result.AddResultTypeChunk(
      GetCompletionTypeString(T, Context, Policy, Result.getAllocator()));
}

static void MaybeAddSentinel(Preprocessor &PP,
                             const NamedDecl *FunctionOrMethod,
                             CodeCompletionBuilder &Result) {
  if (SentinelAttr *Sentinel = FunctionOrMethod->getAttr<SentinelAttr>())
    if (Sentinel->getSentinel() == 0) {
      if (PP.getLangOpts().ObjC && PP.isMacroDefined("nil"))
        Result.AddTextChunk(", nil");
      else if (PP.isMacroDefined("NULL"))
        Result.AddTextChunk(", NULL");
      else
        Result.AddTextChunk(", (void*)0");
    }
}

static std::string formatObjCParamQualifiers(unsigned ObjCQuals,
                                             QualType &Type) {
  std::string Result;
  if (ObjCQuals & Decl::OBJC_TQ_In)
    Result += "in ";
  else if (ObjCQuals & Decl::OBJC_TQ_Inout)
    Result += "inout ";
  else if (ObjCQuals & Decl::OBJC_TQ_Out)
    Result += "out ";
  if (ObjCQuals & Decl::OBJC_TQ_Bycopy)
    Result += "bycopy ";
  else if (ObjCQuals & Decl::OBJC_TQ_Byref)
    Result += "byref ";
  if (ObjCQuals & Decl::OBJC_TQ_Oneway)
    Result += "oneway ";
  if (ObjCQuals & Decl::OBJC_TQ_CSNullability) {
    if (auto nullability = AttributedType::stripOuterNullability(Type)) {
      switch (*nullability) {
      case NullabilityKind::NonNull:
        Result += "nonnull ";
        break;

      case NullabilityKind::Nullable:
        Result += "nullable ";
        break;

      case NullabilityKind::Unspecified:
        Result += "null_unspecified ";
        break;
      }
    }
  }
  return Result;
}

/// Tries to find the most appropriate type location for an Objective-C
/// block placeholder.
///
/// This function ignores things like typedefs and qualifiers in order to
/// present the most relevant and accurate block placeholders in code completion
/// results.
static void findTypeLocationForBlockDecl(const TypeSourceInfo *TSInfo,
                                         FunctionTypeLoc &Block,
                                         FunctionProtoTypeLoc &BlockProto,
                                         bool SuppressBlock = false) {
  if (!TSInfo)
    return;
  TypeLoc TL = TSInfo->getTypeLoc().getUnqualifiedLoc();
  while (true) {
    // Look through typedefs.
    if (!SuppressBlock) {
      if (TypedefTypeLoc TypedefTL = TL.getAs<TypedefTypeLoc>()) {
        if (TypeSourceInfo *InnerTSInfo =
                TypedefTL.getTypedefNameDecl()->getTypeSourceInfo()) {
          TL = InnerTSInfo->getTypeLoc().getUnqualifiedLoc();
          continue;
        }
      }

      // Look through qualified types
      if (QualifiedTypeLoc QualifiedTL = TL.getAs<QualifiedTypeLoc>()) {
        TL = QualifiedTL.getUnqualifiedLoc();
        continue;
      }

      if (AttributedTypeLoc AttrTL = TL.getAs<AttributedTypeLoc>()) {
        TL = AttrTL.getModifiedLoc();
        continue;
      }
    }

    // Try to get the function prototype behind the block pointer type,
    // then we're done.
    if (BlockPointerTypeLoc BlockPtr = TL.getAs<BlockPointerTypeLoc>()) {
      TL = BlockPtr.getPointeeLoc().IgnoreParens();
      Block = TL.getAs<FunctionTypeLoc>();
      BlockProto = TL.getAs<FunctionProtoTypeLoc>();
    }
    break;
  }
}

static std::string
formatBlockPlaceholder(const PrintingPolicy &Policy, const NamedDecl *BlockDecl,
                       FunctionTypeLoc &Block, FunctionProtoTypeLoc &BlockProto,
                       bool SuppressBlockName = false,
                       bool SuppressBlock = false,
                       Optional<ArrayRef<QualType>> ObjCSubsts = None);

static std::string
FormatFunctionParameter(const PrintingPolicy &Policy, const ParmVarDecl *Param,
                        bool SuppressName = false, bool SuppressBlock = false,
                        Optional<ArrayRef<QualType>> ObjCSubsts = None) {
  // Params are unavailable in FunctionTypeLoc if the FunctionType is invalid.
  // It would be better to pass in the param Type, which is usually avaliable.
  // But this case is rare, so just pretend we fell back to int as elsewhere.
  if (!Param)
    return "int";
  bool ObjCMethodParam = isa<ObjCMethodDecl>(Param->getDeclContext());
  if (Param->getType()->isDependentType() ||
      !Param->getType()->isBlockPointerType()) {
    // The argument for a dependent or non-block parameter is a placeholder
    // containing that parameter's type.
    std::string Result;

    if (Param->getIdentifier() && !ObjCMethodParam && !SuppressName)
      Result = Param->getIdentifier()->getName();

    QualType Type = Param->getType();
    if (ObjCSubsts)
      Type = Type.substObjCTypeArgs(Param->getASTContext(), *ObjCSubsts,
                                    ObjCSubstitutionContext::Parameter);
    if (ObjCMethodParam) {
      Result =
          "(" + formatObjCParamQualifiers(Param->getObjCDeclQualifier(), Type);
      Result += Type.getAsString(Policy) + ")";
      if (Param->getIdentifier() && !SuppressName)
        Result += Param->getIdentifier()->getName();
    } else {
      Type.getAsStringInternal(Result, Policy);
    }
    return Result;
  }

  // The argument for a block pointer parameter is a block literal with
  // the appropriate type.
  FunctionTypeLoc Block;
  FunctionProtoTypeLoc BlockProto;
  findTypeLocationForBlockDecl(Param->getTypeSourceInfo(), Block, BlockProto,
                               SuppressBlock);
  // Try to retrieve the block type information from the property if this is a
  // parameter in a setter.
  if (!Block && ObjCMethodParam &&
      cast<ObjCMethodDecl>(Param->getDeclContext())->isPropertyAccessor()) {
    if (const auto *PD = cast<ObjCMethodDecl>(Param->getDeclContext())
                             ->findPropertyDecl(/*CheckOverrides=*/false))
      findTypeLocationForBlockDecl(PD->getTypeSourceInfo(), Block, BlockProto,
                                   SuppressBlock);
  }

  if (!Block) {
    // We were unable to find a FunctionProtoTypeLoc with parameter names
    // for the block; just use the parameter type as a placeholder.
    std::string Result;
    if (!ObjCMethodParam && Param->getIdentifier())
      Result = Param->getIdentifier()->getName();

    QualType Type = Param->getType().getUnqualifiedType();

    if (ObjCMethodParam) {
      Result = Type.getAsString(Policy);
      std::string Quals =
          formatObjCParamQualifiers(Param->getObjCDeclQualifier(), Type);
      if (!Quals.empty())
        Result = "(" + Quals + " " + Result + ")";
      if (Result.back() != ')')
        Result += " ";
      if (Param->getIdentifier())
        Result += Param->getIdentifier()->getName();
    } else {
      Type.getAsStringInternal(Result, Policy);
    }

    return Result;
  }

  // We have the function prototype behind the block pointer type, as it was
  // written in the source.
  return formatBlockPlaceholder(Policy, Param, Block, BlockProto,
                                /*SuppressBlockName=*/false, SuppressBlock,
                                ObjCSubsts);
}

/// Returns a placeholder string that corresponds to an Objective-C block
/// declaration.
///
/// \param BlockDecl A declaration with an Objective-C block type.
///
/// \param Block The most relevant type location for that block type.
///
/// \param SuppressBlockName Determines whether or not the name of the block
/// declaration is included in the resulting string.
static std::string
formatBlockPlaceholder(const PrintingPolicy &Policy, const NamedDecl *BlockDecl,
                       FunctionTypeLoc &Block, FunctionProtoTypeLoc &BlockProto,
                       bool SuppressBlockName, bool SuppressBlock,
                       Optional<ArrayRef<QualType>> ObjCSubsts) {
  std::string Result;
  QualType ResultType = Block.getTypePtr()->getReturnType();
  if (ObjCSubsts)
    ResultType =
        ResultType.substObjCTypeArgs(BlockDecl->getASTContext(), *ObjCSubsts,
                                     ObjCSubstitutionContext::Result);
  if (!ResultType->isVoidType() || SuppressBlock)
    ResultType.getAsStringInternal(Result, Policy);

  // Format the parameter list.
  std::string Params;
  if (!BlockProto || Block.getNumParams() == 0) {
    if (BlockProto && BlockProto.getTypePtr()->isVariadic())
      Params = "(...)";
    else
      Params = "(void)";
  } else {
    Params += "(";
    for (unsigned I = 0, N = Block.getNumParams(); I != N; ++I) {
      if (I)
        Params += ", ";
      Params += FormatFunctionParameter(Policy, Block.getParam(I),
                                        /*SuppressName=*/false,
                                        /*SuppressBlock=*/true, ObjCSubsts);

      if (I == N - 1 && BlockProto.getTypePtr()->isVariadic())
        Params += ", ...";
    }
    Params += ")";
  }

  if (SuppressBlock) {
    // Format as a parameter.
    Result = Result + " (^";
    if (!SuppressBlockName && BlockDecl->getIdentifier())
      Result += BlockDecl->getIdentifier()->getName();
    Result += ")";
    Result += Params;
  } else {
    // Format as a block literal argument.
    Result = '^' + Result;
    Result += Params;

    if (!SuppressBlockName && BlockDecl->getIdentifier())
      Result += BlockDecl->getIdentifier()->getName();
  }

  return Result;
}

static std::string GetDefaultValueString(const ParmVarDecl *Param,
                                         const SourceManager &SM,
                                         const LangOptions &LangOpts) {
  const SourceRange SrcRange = Param->getDefaultArgRange();
  CharSourceRange CharSrcRange = CharSourceRange::getTokenRange(SrcRange);
  bool Invalid = CharSrcRange.isInvalid();
  if (Invalid)
    return "";
  StringRef srcText =
      Lexer::getSourceText(CharSrcRange, SM, LangOpts, &Invalid);
  if (Invalid)
    return "";

  if (srcText.empty() || srcText == "=") {
    // Lexer can't determine the value.
    // This happens if the code is incorrect (for example class is forward
    // declared).
    return "";
  }
  std::string DefValue(srcText.str());
  // FIXME: remove this check if the Lexer::getSourceText value is fixed and
  // this value always has (or always does not have) '=' in front of it
  if (DefValue.at(0) != '=') {
    // If we don't have '=' in front of value.
    // Lexer returns built-in types values without '=' and user-defined types
    // values with it.
    return " = " + DefValue;
  }
  return " " + DefValue;
}

/// Add function parameter chunks to the given code completion string.
static void AddFunctionParameterChunks(Preprocessor &PP,
                                       const PrintingPolicy &Policy,
                                       const FunctionDecl *Function,
                                       CodeCompletionBuilder &Result,
                                       unsigned Start = 0,
                                       bool InOptional = false) {
  bool FirstParameter = true;

  for (unsigned P = Start, N = Function->getNumParams(); P != N; ++P) {
    const ParmVarDecl *Param = Function->getParamDecl(P);

    if (Param->hasDefaultArg() && !InOptional) {
      // When we see an optional default argument, put that argument and
      // the remaining default arguments into a new, optional string.
      CodeCompletionBuilder Opt(Result.getAllocator(),
                                Result.getCodeCompletionTUInfo());
      if (!FirstParameter)
        Opt.AddChunk(CodeCompletionString::CK_Comma);
      AddFunctionParameterChunks(PP, Policy, Function, Opt, P, true);
      Result.AddOptionalChunk(Opt.TakeString());
      break;
    }

    if (FirstParameter)
      FirstParameter = false;
    else
      Result.AddChunk(CodeCompletionString::CK_Comma);

    InOptional = false;

    // Format the placeholder string.
    std::string PlaceholderStr = FormatFunctionParameter(Policy, Param);
    if (Param->hasDefaultArg())
      PlaceholderStr +=
          GetDefaultValueString(Param, PP.getSourceManager(), PP.getLangOpts());

    if (Function->isVariadic() && P == N - 1)
      PlaceholderStr += ", ...";

    // Add the placeholder string.
    Result.AddPlaceholderChunk(
        Result.getAllocator().CopyString(PlaceholderStr));
  }

  if (const auto *Proto = Function->getType()->getAs<FunctionProtoType>())
    if (Proto->isVariadic()) {
      if (Proto->getNumParams() == 0)
        Result.AddPlaceholderChunk("...");

      MaybeAddSentinel(PP, Function, Result);
    }
}

/// Add template parameter chunks to the given code completion string.
static void AddTemplateParameterChunks(
    ASTContext &Context, const PrintingPolicy &Policy,
    const TemplateDecl *Template, CodeCompletionBuilder &Result,
    unsigned MaxParameters = 0, unsigned Start = 0, bool InDefaultArg = false) {
  bool FirstParameter = true;

  // Prefer to take the template parameter names from the first declaration of
  // the template.
  Template = cast<TemplateDecl>(Template->getCanonicalDecl());

  TemplateParameterList *Params = Template->getTemplateParameters();
  TemplateParameterList::iterator PEnd = Params->end();
  if (MaxParameters)
    PEnd = Params->begin() + MaxParameters;
  for (TemplateParameterList::iterator P = Params->begin() + Start; P != PEnd;
       ++P) {
    bool HasDefaultArg = false;
    std::string PlaceholderStr;
    if (TemplateTypeParmDecl *TTP = dyn_cast<TemplateTypeParmDecl>(*P)) {
      if (TTP->wasDeclaredWithTypename())
        PlaceholderStr = "typename";
      else
        PlaceholderStr = "class";

      if (TTP->getIdentifier()) {
        PlaceholderStr += ' ';
        PlaceholderStr += TTP->getIdentifier()->getName();
      }

      HasDefaultArg = TTP->hasDefaultArgument();
    } else if (NonTypeTemplateParmDecl *NTTP =
                   dyn_cast<NonTypeTemplateParmDecl>(*P)) {
      if (NTTP->getIdentifier())
        PlaceholderStr = NTTP->getIdentifier()->getName();
      NTTP->getType().getAsStringInternal(PlaceholderStr, Policy);
      HasDefaultArg = NTTP->hasDefaultArgument();
    } else {
      assert(isa<TemplateTemplateParmDecl>(*P));
      TemplateTemplateParmDecl *TTP = cast<TemplateTemplateParmDecl>(*P);

      // Since putting the template argument list into the placeholder would
      // be very, very long, we just use an abbreviation.
      PlaceholderStr = "template<...> class";
      if (TTP->getIdentifier()) {
        PlaceholderStr += ' ';
        PlaceholderStr += TTP->getIdentifier()->getName();
      }

      HasDefaultArg = TTP->hasDefaultArgument();
    }

    if (HasDefaultArg && !InDefaultArg) {
      // When we see an optional default argument, put that argument and
      // the remaining default arguments into a new, optional string.
      CodeCompletionBuilder Opt(Result.getAllocator(),
                                Result.getCodeCompletionTUInfo());
      if (!FirstParameter)
        Opt.AddChunk(CodeCompletionString::CK_Comma);
      AddTemplateParameterChunks(Context, Policy, Template, Opt, MaxParameters,
                                 P - Params->begin(), true);
      Result.AddOptionalChunk(Opt.TakeString());
      break;
    }

    InDefaultArg = false;

    if (FirstParameter)
      FirstParameter = false;
    else
      Result.AddChunk(CodeCompletionString::CK_Comma);

    // Add the placeholder string.
    Result.AddPlaceholderChunk(
        Result.getAllocator().CopyString(PlaceholderStr));
  }
}

/// Add a qualifier to the given code-completion string, if the
/// provided nested-name-specifier is non-NULL.
static void AddQualifierToCompletionString(CodeCompletionBuilder &Result,
                                           NestedNameSpecifier *Qualifier,
                                           bool QualifierIsInformative,
                                           ASTContext &Context,
                                           const PrintingPolicy &Policy) {
  if (!Qualifier)
    return;

  std::string PrintedNNS;
  {
    llvm::raw_string_ostream OS(PrintedNNS);
    Qualifier->print(OS, Policy);
  }
  if (QualifierIsInformative)
    Result.AddInformativeChunk(Result.getAllocator().CopyString(PrintedNNS));
  else
    Result.AddTextChunk(Result.getAllocator().CopyString(PrintedNNS));
}

static void
AddFunctionTypeQualsToCompletionString(CodeCompletionBuilder &Result,
                                       const FunctionDecl *Function) {
  const auto *Proto = Function->getType()->getAs<FunctionProtoType>();
  if (!Proto || !Proto->getMethodQuals())
    return;

  // FIXME: Add ref-qualifier!

  // Handle single qualifiers without copying
  if (Proto->getMethodQuals().hasOnlyConst()) {
    Result.AddInformativeChunk(" const");
    return;
  }

  if (Proto->getMethodQuals().hasOnlyVolatile()) {
    Result.AddInformativeChunk(" volatile");
    return;
  }

  if (Proto->getMethodQuals().hasOnlyRestrict()) {
    Result.AddInformativeChunk(" restrict");
    return;
  }

  // Handle multiple qualifiers.
  std::string QualsStr;
  if (Proto->isConst())
    QualsStr += " const";
  if (Proto->isVolatile())
    QualsStr += " volatile";
  if (Proto->isRestrict())
    QualsStr += " restrict";
  Result.AddInformativeChunk(Result.getAllocator().CopyString(QualsStr));
}

/// Add the name of the given declaration
static void AddTypedNameChunk(ASTContext &Context, const PrintingPolicy &Policy,
                              const NamedDecl *ND,
                              CodeCompletionBuilder &Result) {
  DeclarationName Name = ND->getDeclName();
  if (!Name)
    return;

  switch (Name.getNameKind()) {
  case DeclarationName::CXXOperatorName: {
    const char *OperatorName = nullptr;
    switch (Name.getCXXOverloadedOperator()) {
    case OO_None:
    case OO_Conditional:
    case NUM_OVERLOADED_OPERATORS:
      OperatorName = "operator";
      break;

#define OVERLOADED_OPERATOR(Name, Spelling, Token, Unary, Binary, MemberOnly)  \
  case OO_##Name:                                                              \
    OperatorName = "operator" Spelling;                                        \
    break;
#define OVERLOADED_OPERATOR_MULTI(Name, Spelling, Unary, Binary, MemberOnly)
#include "clang/Basic/OperatorKinds.def"

    case OO_New:
      OperatorName = "operator new";
      break;
    case OO_Delete:
      OperatorName = "operator delete";
      break;
    case OO_Array_New:
      OperatorName = "operator new[]";
      break;
    case OO_Array_Delete:
      OperatorName = "operator delete[]";
      break;
    case OO_Call:
      OperatorName = "operator()";
      break;
    case OO_Subscript:
      OperatorName = "operator[]";
      break;
    }
    Result.AddTypedTextChunk(OperatorName);
    break;
  }

  case DeclarationName::Identifier:
  case DeclarationName::CXXConversionFunctionName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXLiteralOperatorName:
    Result.AddTypedTextChunk(
        Result.getAllocator().CopyString(ND->getNameAsString()));
    break;

  case DeclarationName::CXXDeductionGuideName:
  case DeclarationName::CXXUsingDirective:
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
    break;

  case DeclarationName::CXXConstructorName: {
    CXXRecordDecl *Record = nullptr;
    QualType Ty = Name.getCXXNameType();
    if (const auto *RecordTy = Ty->getAs<RecordType>())
      Record = cast<CXXRecordDecl>(RecordTy->getDecl());
    else if (const auto *InjectedTy = Ty->getAs<InjectedClassNameType>())
      Record = InjectedTy->getDecl();
    else {
      Result.AddTypedTextChunk(
          Result.getAllocator().CopyString(ND->getNameAsString()));
      break;
    }

    Result.AddTypedTextChunk(
        Result.getAllocator().CopyString(Record->getNameAsString()));
    if (ClassTemplateDecl *Template = Record->getDescribedClassTemplate()) {
      Result.AddChunk(CodeCompletionString::CK_LeftAngle);
      AddTemplateParameterChunks(Context, Policy, Template, Result);
      Result.AddChunk(CodeCompletionString::CK_RightAngle);
    }
    break;
  }
  }
}

CodeCompletionString *CodeCompletionResult::CreateCodeCompletionString(
    Sema &S, const CodeCompletionContext &CCContext,
    CodeCompletionAllocator &Allocator, CodeCompletionTUInfo &CCTUInfo,
    bool IncludeBriefComments) {
  return CreateCodeCompletionString(S.Context, S.PP, CCContext, Allocator,
                                    CCTUInfo, IncludeBriefComments);
}

CodeCompletionString *CodeCompletionResult::CreateCodeCompletionStringForMacro(
    Preprocessor &PP, CodeCompletionAllocator &Allocator,
    CodeCompletionTUInfo &CCTUInfo) {
  assert(Kind == RK_Macro);
  CodeCompletionBuilder Result(Allocator, CCTUInfo, Priority, Availability);
  const MacroInfo *MI = PP.getMacroInfo(Macro);
  Result.AddTypedTextChunk(Result.getAllocator().CopyString(Macro->getName()));

  if (!MI || !MI->isFunctionLike())
    return Result.TakeString();

  // Format a function-like macro with placeholders for the arguments.
  Result.AddChunk(CodeCompletionString::CK_LeftParen);
  MacroInfo::param_iterator A = MI->param_begin(), AEnd = MI->param_end();

  // C99 variadic macros add __VA_ARGS__ at the end. Skip it.
  if (MI->isC99Varargs()) {
    --AEnd;

    if (A == AEnd) {
      Result.AddPlaceholderChunk("...");
    }
  }

  for (MacroInfo::param_iterator A = MI->param_begin(); A != AEnd; ++A) {
    if (A != MI->param_begin())
      Result.AddChunk(CodeCompletionString::CK_Comma);

    if (MI->isVariadic() && (A + 1) == AEnd) {
      SmallString<32> Arg = (*A)->getName();
      if (MI->isC99Varargs())
        Arg += ", ...";
      else
        Arg += "...";
      Result.AddPlaceholderChunk(Result.getAllocator().CopyString(Arg));
      break;
    }

    // Non-variadic macros are simple.
    Result.AddPlaceholderChunk(
        Result.getAllocator().CopyString((*A)->getName()));
  }
  Result.AddChunk(CodeCompletionString::CK_RightParen);
  return Result.TakeString();
}

/// If possible, create a new code completion string for the given
/// result.
///
/// \returns Either a new, heap-allocated code completion string describing
/// how to use this result, or NULL to indicate that the string or name of the
/// result is all that is needed.
CodeCompletionString *CodeCompletionResult::CreateCodeCompletionString(
    ASTContext &Ctx, Preprocessor &PP, const CodeCompletionContext &CCContext,
    CodeCompletionAllocator &Allocator, CodeCompletionTUInfo &CCTUInfo,
    bool IncludeBriefComments) {
  if (Kind == RK_Macro)
    return CreateCodeCompletionStringForMacro(PP, Allocator, CCTUInfo);

  CodeCompletionBuilder Result(Allocator, CCTUInfo, Priority, Availability);

  PrintingPolicy Policy = getCompletionPrintingPolicy(Ctx, PP);
  if (Kind == RK_Pattern) {
    Pattern->Priority = Priority;
    Pattern->Availability = Availability;

    if (Declaration) {
      Result.addParentContext(Declaration->getDeclContext());
      Pattern->ParentName = Result.getParentName();
      if (const RawComment *RC =
              getPatternCompletionComment(Ctx, Declaration)) {
        Result.addBriefComment(RC->getBriefText(Ctx));
        Pattern->BriefComment = Result.getBriefComment();
      }
    }

    return Pattern;
  }

  if (Kind == RK_Keyword) {
    Result.AddTypedTextChunk(Keyword);
    return Result.TakeString();
  }
  assert(Kind == RK_Declaration && "Missed a result kind?");
  return createCodeCompletionStringForDecl(
      PP, Ctx, Result, IncludeBriefComments, CCContext, Policy);
}

static void printOverrideString(const CodeCompletionString &CCS,
                                std::string &BeforeName,
                                std::string &NameAndSignature) {
  bool SeenTypedChunk = false;
  for (auto &Chunk : CCS) {
    if (Chunk.Kind == CodeCompletionString::CK_Optional) {
      assert(SeenTypedChunk && "optional parameter before name");
      // Note that we put all chunks inside into NameAndSignature.
      printOverrideString(*Chunk.Optional, NameAndSignature, NameAndSignature);
      continue;
    }
    SeenTypedChunk |= Chunk.Kind == CodeCompletionString::CK_TypedText;
    if (SeenTypedChunk)
      NameAndSignature += Chunk.Text;
    else
      BeforeName += Chunk.Text;
  }
}

CodeCompletionString *
CodeCompletionResult::createCodeCompletionStringForOverride(
    Preprocessor &PP, ASTContext &Ctx, CodeCompletionBuilder &Result,
    bool IncludeBriefComments, const CodeCompletionContext &CCContext,
    PrintingPolicy &Policy) {
  auto *CCS = createCodeCompletionStringForDecl(PP, Ctx, Result,
                                                /*IncludeBriefComments=*/false,
                                                CCContext, Policy);
  std::string BeforeName;
  std::string NameAndSignature;
  // For overrides all chunks go into the result, none are informative.
  printOverrideString(*CCS, BeforeName, NameAndSignature);
  NameAndSignature += " override";

  Result.AddTextChunk(Result.getAllocator().CopyString(BeforeName));
  Result.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Result.AddTypedTextChunk(Result.getAllocator().CopyString(NameAndSignature));
  return Result.TakeString();
}

CodeCompletionString *CodeCompletionResult::createCodeCompletionStringForDecl(
    Preprocessor &PP, ASTContext &Ctx, CodeCompletionBuilder &Result,
    bool IncludeBriefComments, const CodeCompletionContext &CCContext,
    PrintingPolicy &Policy) {
  const NamedDecl *ND = Declaration;
  Result.addParentContext(ND->getDeclContext());

  if (IncludeBriefComments) {
    // Add documentation comment, if it exists.
    if (const RawComment *RC = getCompletionComment(Ctx, Declaration)) {
      Result.addBriefComment(RC->getBriefText(Ctx));
    }
  }

  if (StartsNestedNameSpecifier) {
    Result.AddTypedTextChunk(
        Result.getAllocator().CopyString(ND->getNameAsString()));
    Result.AddTextChunk("::");
    return Result.TakeString();
  }

  for (const auto *I : ND->specific_attrs<AnnotateAttr>())
    Result.AddAnnotation(Result.getAllocator().CopyString(I->getAnnotation()));

  AddResultTypeChunk(Ctx, Policy, ND, CCContext.getBaseType(), Result);

  if (const auto *Function = dyn_cast<FunctionDecl>(ND)) {
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative,
                                   Ctx, Policy);
    AddTypedNameChunk(Ctx, Policy, ND, Result);
    Result.AddChunk(CodeCompletionString::CK_LeftParen);
    AddFunctionParameterChunks(PP, Policy, Function, Result);
    Result.AddChunk(CodeCompletionString::CK_RightParen);
    AddFunctionTypeQualsToCompletionString(Result, Function);
    return Result.TakeString();
  }

  if (const FunctionTemplateDecl *FunTmpl =
          dyn_cast<FunctionTemplateDecl>(ND)) {
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative,
                                   Ctx, Policy);
    FunctionDecl *Function = FunTmpl->getTemplatedDecl();
    AddTypedNameChunk(Ctx, Policy, Function, Result);

    // Figure out which template parameters are deduced (or have default
    // arguments).
    llvm::SmallBitVector Deduced;
    Sema::MarkDeducedTemplateParameters(Ctx, FunTmpl, Deduced);
    unsigned LastDeducibleArgument;
    for (LastDeducibleArgument = Deduced.size(); LastDeducibleArgument > 0;
         --LastDeducibleArgument) {
      if (!Deduced[LastDeducibleArgument - 1]) {
        // C++0x: Figure out if the template argument has a default. If so,
        // the user doesn't need to type this argument.
        // FIXME: We need to abstract template parameters better!
        bool HasDefaultArg = false;
        NamedDecl *Param = FunTmpl->getTemplateParameters()->getParam(
            LastDeducibleArgument - 1);
        if (TemplateTypeParmDecl *TTP = dyn_cast<TemplateTypeParmDecl>(Param))
          HasDefaultArg = TTP->hasDefaultArgument();
        else if (NonTypeTemplateParmDecl *NTTP =
                     dyn_cast<NonTypeTemplateParmDecl>(Param))
          HasDefaultArg = NTTP->hasDefaultArgument();
        else {
          assert(isa<TemplateTemplateParmDecl>(Param));
          HasDefaultArg =
              cast<TemplateTemplateParmDecl>(Param)->hasDefaultArgument();
        }

        if (!HasDefaultArg)
          break;
      }
    }

    if (LastDeducibleArgument) {
      // Some of the function template arguments cannot be deduced from a
      // function call, so we introduce an explicit template argument list
      // containing all of the arguments up to the first deducible argument.
      Result.AddChunk(CodeCompletionString::CK_LeftAngle);
      AddTemplateParameterChunks(Ctx, Policy, FunTmpl, Result,
                                 LastDeducibleArgument);
      Result.AddChunk(CodeCompletionString::CK_RightAngle);
    }

    // Add the function parameters
    Result.AddChunk(CodeCompletionString::CK_LeftParen);
    AddFunctionParameterChunks(PP, Policy, Function, Result);
    Result.AddChunk(CodeCompletionString::CK_RightParen);
    AddFunctionTypeQualsToCompletionString(Result, Function);
    return Result.TakeString();
  }

  if (const auto *Template = dyn_cast<TemplateDecl>(ND)) {
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative,
                                   Ctx, Policy);
    Result.AddTypedTextChunk(
        Result.getAllocator().CopyString(Template->getNameAsString()));
    Result.AddChunk(CodeCompletionString::CK_LeftAngle);
    AddTemplateParameterChunks(Ctx, Policy, Template, Result);
    Result.AddChunk(CodeCompletionString::CK_RightAngle);
    return Result.TakeString();
  }
  if (const auto *Method = dyn_cast<ObjCMethodDecl>(ND)) {
    Selector Sel = Method->getSelector();
    if (Sel.isUnarySelector()) {
      Result.AddTypedTextChunk(
          Result.getAllocator().CopyString(Sel.getNameForSlot(0)));
      return Result.TakeString();
    }

    std::string SelName = Sel.getNameForSlot(0).str();
    SelName += ':';
    if (StartParameter == 0)
      Result.AddTypedTextChunk(Result.getAllocator().CopyString(SelName));
    else {
      Result.AddInformativeChunk(Result.getAllocator().CopyString(SelName));

      // If there is only one parameter, and we're past it, add an empty
      // typed-text chunk since there is nothing to type.
      if (Method->param_size() == 1)
        Result.AddTypedTextChunk("");
    }
    unsigned Idx = 0;
    for (ObjCMethodDecl::param_const_iterator P = Method->param_begin(),
                                              PEnd = Method->param_end();
         P != PEnd; (void)++P, ++Idx) {
      if (Idx > 0) {
        std::string Keyword;
        if (Idx > StartParameter)
          Result.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        if (IdentifierInfo *II = Sel.getIdentifierInfoForSlot(Idx))
          Keyword += II->getName();
        Keyword += ":";
        if (Idx < StartParameter || AllParametersAreInformative)
          Result.AddInformativeChunk(Result.getAllocator().CopyString(Keyword));
        else
          Result.AddTypedTextChunk(Result.getAllocator().CopyString(Keyword));
      }

      // If we're before the starting parameter, skip the placeholder.
      if (Idx < StartParameter)
        continue;

      std::string Arg;
      QualType ParamType = (*P)->getType();
      Optional<ArrayRef<QualType>> ObjCSubsts;
      if (!CCContext.getBaseType().isNull())
        ObjCSubsts = CCContext.getBaseType()->getObjCSubstitutions(Method);

      if (ParamType->isBlockPointerType() && !DeclaringEntity)
        Arg = FormatFunctionParameter(Policy, *P, true,
                                      /*SuppressBlock=*/false, ObjCSubsts);
      else {
        if (ObjCSubsts)
          ParamType = ParamType.substObjCTypeArgs(
              Ctx, *ObjCSubsts, ObjCSubstitutionContext::Parameter);
        Arg = "(" + formatObjCParamQualifiers((*P)->getObjCDeclQualifier(),
                                              ParamType);
        Arg += ParamType.getAsString(Policy) + ")";
        if (IdentifierInfo *II = (*P)->getIdentifier())
          if (DeclaringEntity || AllParametersAreInformative)
            Arg += II->getName();
      }

      if (Method->isVariadic() && (P + 1) == PEnd)
        Arg += ", ...";

      if (DeclaringEntity)
        Result.AddTextChunk(Result.getAllocator().CopyString(Arg));
      else if (AllParametersAreInformative)
        Result.AddInformativeChunk(Result.getAllocator().CopyString(Arg));
      else
        Result.AddPlaceholderChunk(Result.getAllocator().CopyString(Arg));
    }

    if (Method->isVariadic()) {
      if (Method->param_size() == 0) {
        if (DeclaringEntity)
          Result.AddTextChunk(", ...");
        else if (AllParametersAreInformative)
          Result.AddInformativeChunk(", ...");
        else
          Result.AddPlaceholderChunk(", ...");
      }

      MaybeAddSentinel(PP, Method, Result);
    }

    return Result.TakeString();
  }

  if (Qualifier)
    AddQualifierToCompletionString(Result, Qualifier, QualifierIsInformative,
                                   Ctx, Policy);

  Result.AddTypedTextChunk(
      Result.getAllocator().CopyString(ND->getNameAsString()));
  return Result.TakeString();
}

const RawComment *clang::getCompletionComment(const ASTContext &Ctx,
                                              const NamedDecl *ND) {
  if (!ND)
    return nullptr;
  if (auto *RC = Ctx.getRawCommentForAnyRedecl(ND))
    return RC;

  // Try to find comment from a property for ObjC methods.
  const auto *M = dyn_cast<ObjCMethodDecl>(ND);
  if (!M)
    return nullptr;
  const ObjCPropertyDecl *PDecl = M->findPropertyDecl();
  if (!PDecl)
    return nullptr;

  return Ctx.getRawCommentForAnyRedecl(PDecl);
}

const RawComment *clang::getPatternCompletionComment(const ASTContext &Ctx,
                                                     const NamedDecl *ND) {
  const auto *M = dyn_cast_or_null<ObjCMethodDecl>(ND);
  if (!M || !M->isPropertyAccessor())
    return nullptr;

  // Provide code completion comment for self.GetterName where
  // GetterName is the getter method for a property with name
  // different from the property name (declared via a property
  // getter attribute.
  const ObjCPropertyDecl *PDecl = M->findPropertyDecl();
  if (!PDecl)
    return nullptr;
  if (PDecl->getGetterName() == M->getSelector() &&
      PDecl->getIdentifier() != M->getIdentifier()) {
    if (auto *RC = Ctx.getRawCommentForAnyRedecl(M))
      return RC;
    if (auto *RC = Ctx.getRawCommentForAnyRedecl(PDecl))
      return RC;
  }
  return nullptr;
}

const RawComment *clang::getParameterComment(
    const ASTContext &Ctx,
    const CodeCompleteConsumer::OverloadCandidate &Result, unsigned ArgIndex) {
  auto FDecl = Result.getFunction();
  if (!FDecl)
    return nullptr;
  if (ArgIndex < FDecl->getNumParams())
    return Ctx.getRawCommentForAnyRedecl(FDecl->getParamDecl(ArgIndex));
  return nullptr;
}

/// Add function overload parameter chunks to the given code completion
/// string.
static void AddOverloadParameterChunks(ASTContext &Context,
                                       const PrintingPolicy &Policy,
                                       const FunctionDecl *Function,
                                       const FunctionProtoType *Prototype,
                                       CodeCompletionBuilder &Result,
                                       unsigned CurrentArg, unsigned Start = 0,
                                       bool InOptional = false) {
  bool FirstParameter = true;
  unsigned NumParams =
      Function ? Function->getNumParams() : Prototype->getNumParams();

  for (unsigned P = Start; P != NumParams; ++P) {
    if (Function && Function->getParamDecl(P)->hasDefaultArg() && !InOptional) {
      // When we see an optional default argument, put that argument and
      // the remaining default arguments into a new, optional string.
      CodeCompletionBuilder Opt(Result.getAllocator(),
                                Result.getCodeCompletionTUInfo());
      if (!FirstParameter)
        Opt.AddChunk(CodeCompletionString::CK_Comma);
      // Optional sections are nested.
      AddOverloadParameterChunks(Context, Policy, Function, Prototype, Opt,
                                 CurrentArg, P, /*InOptional=*/true);
      Result.AddOptionalChunk(Opt.TakeString());
      return;
    }

    if (FirstParameter)
      FirstParameter = false;
    else
      Result.AddChunk(CodeCompletionString::CK_Comma);

    InOptional = false;

    // Format the placeholder string.
    std::string Placeholder;
    if (Function) {
      const ParmVarDecl *Param = Function->getParamDecl(P);
      Placeholder = FormatFunctionParameter(Policy, Param);
      if (Param->hasDefaultArg())
        Placeholder += GetDefaultValueString(Param, Context.getSourceManager(),
                                             Context.getLangOpts());
    } else {
      Placeholder = Prototype->getParamType(P).getAsString(Policy);
    }

    if (P == CurrentArg)
      Result.AddCurrentParameterChunk(
          Result.getAllocator().CopyString(Placeholder));
    else
      Result.AddPlaceholderChunk(Result.getAllocator().CopyString(Placeholder));
  }

  if (Prototype && Prototype->isVariadic()) {
    CodeCompletionBuilder Opt(Result.getAllocator(),
                              Result.getCodeCompletionTUInfo());
    if (!FirstParameter)
      Opt.AddChunk(CodeCompletionString::CK_Comma);

    if (CurrentArg < NumParams)
      Opt.AddPlaceholderChunk("...");
    else
      Opt.AddCurrentParameterChunk("...");

    Result.AddOptionalChunk(Opt.TakeString());
  }
}

CodeCompletionString *
CodeCompleteConsumer::OverloadCandidate::CreateSignatureString(
    unsigned CurrentArg, Sema &S, CodeCompletionAllocator &Allocator,
    CodeCompletionTUInfo &CCTUInfo, bool IncludeBriefComments) const {
  PrintingPolicy Policy = getCompletionPrintingPolicy(S);

  // FIXME: Set priority, availability appropriately.
  CodeCompletionBuilder Result(Allocator, CCTUInfo, 1,
                               CXAvailability_Available);
  FunctionDecl *FDecl = getFunction();
  const FunctionProtoType *Proto =
      dyn_cast<FunctionProtoType>(getFunctionType());
  if (!FDecl && !Proto) {
    // Function without a prototype. Just give the return type and a
    // highlighted ellipsis.
    const FunctionType *FT = getFunctionType();
    Result.AddResultTypeChunk(Result.getAllocator().CopyString(
        FT->getReturnType().getAsString(Policy)));
    Result.AddChunk(CodeCompletionString::CK_LeftParen);
    Result.AddChunk(CodeCompletionString::CK_CurrentParameter, "...");
    Result.AddChunk(CodeCompletionString::CK_RightParen);
    return Result.TakeString();
  }

  if (FDecl) {
    if (IncludeBriefComments) {
      if (auto RC = getParameterComment(S.getASTContext(), *this, CurrentArg))
        Result.addBriefComment(RC->getBriefText(S.getASTContext()));
    }
    AddResultTypeChunk(S.Context, Policy, FDecl, QualType(), Result);
    Result.AddTextChunk(
        Result.getAllocator().CopyString(FDecl->getNameAsString()));
  } else {
    Result.AddResultTypeChunk(Result.getAllocator().CopyString(
        Proto->getReturnType().getAsString(Policy)));
  }

  Result.AddChunk(CodeCompletionString::CK_LeftParen);
  AddOverloadParameterChunks(S.getASTContext(), Policy, FDecl, Proto, Result,
                             CurrentArg);
  Result.AddChunk(CodeCompletionString::CK_RightParen);

  return Result.TakeString();
}

unsigned clang::getMacroUsagePriority(StringRef MacroName,
                                      const LangOptions &LangOpts,
                                      bool PreferredTypeIsPointer) {
  unsigned Priority = CCP_Macro;

  // Treat the "nil", "Nil" and "NULL" macros as null pointer constants.
  if (MacroName.equals("nil") || MacroName.equals("NULL") ||
      MacroName.equals("Nil")) {
    Priority = CCP_Constant;
    if (PreferredTypeIsPointer)
      Priority = Priority / CCF_SimilarTypeMatch;
  }
  // Treat "YES", "NO", "true", and "false" as constants.
  else if (MacroName.equals("YES") || MacroName.equals("NO") ||
           MacroName.equals("true") || MacroName.equals("false"))
    Priority = CCP_Constant;
  // Treat "bool" as a type.
  else if (MacroName.equals("bool"))
    Priority = CCP_Type + (LangOpts.ObjC ? CCD_bool_in_ObjC : 0);

  return Priority;
}

CXCursorKind clang::getCursorKindForDecl(const Decl *D) {
  if (!D)
    return CXCursor_UnexposedDecl;

  switch (D->getKind()) {
  case Decl::Enum:
    return CXCursor_EnumDecl;
  case Decl::EnumConstant:
    return CXCursor_EnumConstantDecl;
  case Decl::Field:
    return CXCursor_FieldDecl;
  case Decl::Function:
    return CXCursor_FunctionDecl;
  case Decl::ObjCCategory:
    return CXCursor_ObjCCategoryDecl;
  case Decl::ObjCCategoryImpl:
    return CXCursor_ObjCCategoryImplDecl;
  case Decl::ObjCImplementation:
    return CXCursor_ObjCImplementationDecl;

  case Decl::ObjCInterface:
    return CXCursor_ObjCInterfaceDecl;
  case Decl::ObjCIvar:
    return CXCursor_ObjCIvarDecl;
  case Decl::ObjCMethod:
    return cast<ObjCMethodDecl>(D)->isInstanceMethod()
               ? CXCursor_ObjCInstanceMethodDecl
               : CXCursor_ObjCClassMethodDecl;
  case Decl::CXXMethod:
    return CXCursor_CXXMethod;
  case Decl::CXXConstructor:
    return CXCursor_Constructor;
  case Decl::CXXDestructor:
    return CXCursor_Destructor;
  case Decl::CXXConversion:
    return CXCursor_ConversionFunction;
  case Decl::ObjCProperty:
    return CXCursor_ObjCPropertyDecl;
  case Decl::ObjCProtocol:
    return CXCursor_ObjCProtocolDecl;
  case Decl::ParmVar:
    return CXCursor_ParmDecl;
  case Decl::Typedef:
    return CXCursor_TypedefDecl;
  case Decl::TypeAlias:
    return CXCursor_TypeAliasDecl;
  case Decl::TypeAliasTemplate:
    return CXCursor_TypeAliasTemplateDecl;
  case Decl::Var:
    return CXCursor_VarDecl;
  case Decl::Namespace:
    return CXCursor_Namespace;
  case Decl::NamespaceAlias:
    return CXCursor_NamespaceAlias;
  case Decl::TemplateTypeParm:
    return CXCursor_TemplateTypeParameter;
  case Decl::NonTypeTemplateParm:
    return CXCursor_NonTypeTemplateParameter;
  case Decl::TemplateTemplateParm:
    return CXCursor_TemplateTemplateParameter;
  case Decl::FunctionTemplate:
    return CXCursor_FunctionTemplate;
  case Decl::ClassTemplate:
    return CXCursor_ClassTemplate;
  case Decl::AccessSpec:
    return CXCursor_CXXAccessSpecifier;
  case Decl::ClassTemplatePartialSpecialization:
    return CXCursor_ClassTemplatePartialSpecialization;
  case Decl::UsingDirective:
    return CXCursor_UsingDirective;
  case Decl::StaticAssert:
    return CXCursor_StaticAssert;
  case Decl::Friend:
    return CXCursor_FriendDecl;
  case Decl::TranslationUnit:
    return CXCursor_TranslationUnit;

  case Decl::Using:
  case Decl::UnresolvedUsingValue:
  case Decl::UnresolvedUsingTypename:
    return CXCursor_UsingDeclaration;

  case Decl::ObjCPropertyImpl:
    switch (cast<ObjCPropertyImplDecl>(D)->getPropertyImplementation()) {
    case ObjCPropertyImplDecl::Dynamic:
      return CXCursor_ObjCDynamicDecl;

    case ObjCPropertyImplDecl::Synthesize:
      return CXCursor_ObjCSynthesizeDecl;
    }
    llvm_unreachable("Unexpected Kind!");

  case Decl::Import:
    return CXCursor_ModuleImportDecl;

  case Decl::ObjCTypeParam:
    return CXCursor_TemplateTypeParameter;

  default:
    if (const auto *TD = dyn_cast<TagDecl>(D)) {
      switch (TD->getTagKind()) {
      case TTK_Interface: // fall through
      case TTK_Struct:
        return CXCursor_StructDecl;
      case TTK_Class:
        return CXCursor_ClassDecl;
      case TTK_Union:
        return CXCursor_UnionDecl;
      case TTK_Enum:
        return CXCursor_EnumDecl;
      }
    }
  }

  return CXCursor_UnexposedDecl;
}

static void AddMacroResults(Preprocessor &PP, ResultBuilder &Results,
                            bool LoadExternal, bool IncludeUndefined,
                            bool TargetTypeIsPointer = false) {
  typedef CodeCompletionResult Result;

  Results.EnterNewScope();

  for (Preprocessor::macro_iterator M = PP.macro_begin(LoadExternal),
                                    MEnd = PP.macro_end(LoadExternal);
       M != MEnd; ++M) {
    auto MD = PP.getMacroDefinition(M->first);
    if (IncludeUndefined || MD) {
      MacroInfo *MI = MD.getMacroInfo();
      if (MI && MI->isUsedForHeaderGuard())
        continue;

      Results.AddResult(
          Result(M->first, MI,
                 getMacroUsagePriority(M->first->getName(), PP.getLangOpts(),
                                       TargetTypeIsPointer)));
    }
  }

  Results.ExitScope();
}

static void AddPrettyFunctionResults(const LangOptions &LangOpts,
                                     ResultBuilder &Results) {
  typedef CodeCompletionResult Result;

  Results.EnterNewScope();

  Results.AddResult(Result("__PRETTY_FUNCTION__", CCP_Constant));
  Results.AddResult(Result("__FUNCTION__", CCP_Constant));
  if (LangOpts.C99 || LangOpts.CPlusPlus11)
    Results.AddResult(Result("__func__", CCP_Constant));
  Results.ExitScope();
}

static void HandleCodeCompleteResults(Sema *S,
                                      CodeCompleteConsumer *CodeCompleter,
                                      CodeCompletionContext Context,
                                      CodeCompletionResult *Results,
                                      unsigned NumResults) {
  if (CodeCompleter)
    CodeCompleter->ProcessCodeCompleteResults(*S, Context, Results, NumResults);
}

static CodeCompletionContext
mapCodeCompletionContext(Sema &S, Sema::ParserCompletionContext PCC) {
  switch (PCC) {
  case Sema::PCC_Namespace:
    return CodeCompletionContext::CCC_TopLevel;

  case Sema::PCC_Class:
    return CodeCompletionContext::CCC_ClassStructUnion;

  case Sema::PCC_ObjCInterface:
    return CodeCompletionContext::CCC_ObjCInterface;

  case Sema::PCC_ObjCImplementation:
    return CodeCompletionContext::CCC_ObjCImplementation;

  case Sema::PCC_ObjCInstanceVariableList:
    return CodeCompletionContext::CCC_ObjCIvarList;

  case Sema::PCC_Template:
  case Sema::PCC_MemberTemplate:
    if (S.CurContext->isFileContext())
      return CodeCompletionContext::CCC_TopLevel;
    if (S.CurContext->isRecord())
      return CodeCompletionContext::CCC_ClassStructUnion;
    return CodeCompletionContext::CCC_Other;

  case Sema::PCC_RecoveryInFunction:
    return CodeCompletionContext::CCC_Recovery;

  case Sema::PCC_ForInit:
    if (S.getLangOpts().CPlusPlus || S.getLangOpts().C99 ||
        S.getLangOpts().ObjC)
      return CodeCompletionContext::CCC_ParenthesizedExpression;
    else
      return CodeCompletionContext::CCC_Expression;

  case Sema::PCC_Expression:
    return CodeCompletionContext::CCC_Expression;
  case Sema::PCC_Condition:
    return CodeCompletionContext(CodeCompletionContext::CCC_Expression,
                                 S.getASTContext().BoolTy);

  case Sema::PCC_Statement:
    return CodeCompletionContext::CCC_Statement;

  case Sema::PCC_Type:
    return CodeCompletionContext::CCC_Type;

  case Sema::PCC_ParenthesizedExpression:
    return CodeCompletionContext::CCC_ParenthesizedExpression;

  case Sema::PCC_LocalDeclarationSpecifiers:
    return CodeCompletionContext::CCC_Type;
  }

  llvm_unreachable("Invalid ParserCompletionContext!");
}

/// If we're in a C++ virtual member function, add completion results
/// that invoke the functions we override, since it's common to invoke the
/// overridden function as well as adding new functionality.
///
/// \param S The semantic analysis object for which we are generating results.
///
/// \param InContext This context in which the nested-name-specifier preceding
/// the code-completion point
static void MaybeAddOverrideCalls(Sema &S, DeclContext *InContext,
                                  ResultBuilder &Results) {
  // Look through blocks.
  DeclContext *CurContext = S.CurContext;
  while (isa<BlockDecl>(CurContext))
    CurContext = CurContext->getParent();

  CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(CurContext);
  if (!Method || !Method->isVirtual())
    return;

  // We need to have names for all of the parameters, if we're going to
  // generate a forwarding call.
  for (auto P : Method->parameters())
    if (!P->getDeclName())
      return;

  PrintingPolicy Policy = getCompletionPrintingPolicy(S);
  for (const CXXMethodDecl *Overridden : Method->overridden_methods()) {
    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo());
    if (Overridden->getCanonicalDecl() == Method->getCanonicalDecl())
      continue;

    // If we need a nested-name-specifier, add one now.
    if (!InContext) {
      NestedNameSpecifier *NNS = getRequiredQualification(
          S.Context, CurContext, Overridden->getDeclContext());
      if (NNS) {
        std::string Str;
        llvm::raw_string_ostream OS(Str);
        NNS->print(OS, Policy);
        Builder.AddTextChunk(Results.getAllocator().CopyString(OS.str()));
      }
    } else if (!InContext->Equals(Overridden->getDeclContext()))
      continue;

    Builder.AddTypedTextChunk(
        Results.getAllocator().CopyString(Overridden->getNameAsString()));
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    bool FirstParam = true;
    for (auto P : Method->parameters()) {
      if (FirstParam)
        FirstParam = false;
      else
        Builder.AddChunk(CodeCompletionString::CK_Comma);

      Builder.AddPlaceholderChunk(
          Results.getAllocator().CopyString(P->getIdentifier()->getName()));
    }
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Results.AddResult(CodeCompletionResult(
        Builder.TakeString(), CCP_SuperCompletion, CXCursor_CXXMethod,
        CXAvailability_Available, Overridden));
    Results.Ignore(Overridden);
  }
}

void Sema::CodeCompleteModuleImport(SourceLocation ImportLoc,
                                    ModuleIdPath Path) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();

  CodeCompletionAllocator &Allocator = Results.getAllocator();
  CodeCompletionBuilder Builder(Allocator, Results.getCodeCompletionTUInfo());
  typedef CodeCompletionResult Result;
  if (Path.empty()) {
    // Enumerate all top-level modules.
    SmallVector<Module *, 8> Modules;
    PP.getHeaderSearchInfo().collectAllModules(Modules);
    for (unsigned I = 0, N = Modules.size(); I != N; ++I) {
      Builder.AddTypedTextChunk(
          Builder.getAllocator().CopyString(Modules[I]->Name));
      Results.AddResult(Result(
          Builder.TakeString(), CCP_Declaration, CXCursor_ModuleImportDecl,
          Modules[I]->isAvailable() ? CXAvailability_Available
                                    : CXAvailability_NotAvailable));
    }
  } else if (getLangOpts().Modules) {
    // Load the named module.
    Module *Mod =
        PP.getModuleLoader().loadModule(ImportLoc, Path, Module::AllVisible,
                                        /*IsInclusionDirective=*/false);
    // Enumerate submodules.
    if (Mod) {
      for (Module::submodule_iterator Sub = Mod->submodule_begin(),
                                      SubEnd = Mod->submodule_end();
           Sub != SubEnd; ++Sub) {

        Builder.AddTypedTextChunk(
            Builder.getAllocator().CopyString((*Sub)->Name));
        Results.AddResult(Result(
            Builder.TakeString(), CCP_Declaration, CXCursor_ModuleImportDecl,
            (*Sub)->isAvailable() ? CXAvailability_Available
                                  : CXAvailability_NotAvailable));
      }
    }
  }
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteOrdinaryName(Scope *S,
                                    ParserCompletionContext CompletionContext) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        mapCodeCompletionContext(*this, CompletionContext));
  Results.EnterNewScope();

  // Determine how to filter results, e.g., so that the names of
  // values (functions, enumerators, function templates, etc.) are
  // only allowed where we can have an expression.
  switch (CompletionContext) {
  case PCC_Namespace:
  case PCC_Class:
  case PCC_ObjCInterface:
  case PCC_ObjCImplementation:
  case PCC_ObjCInstanceVariableList:
  case PCC_Template:
  case PCC_MemberTemplate:
  case PCC_Type:
  case PCC_LocalDeclarationSpecifiers:
    Results.setFilter(&ResultBuilder::IsOrdinaryNonValueName);
    break;

  case PCC_Statement:
  case PCC_ParenthesizedExpression:
  case PCC_Expression:
  case PCC_ForInit:
  case PCC_Condition:
    if (WantTypesInContext(CompletionContext, getLangOpts()))
      Results.setFilter(&ResultBuilder::IsOrdinaryName);
    else
      Results.setFilter(&ResultBuilder::IsOrdinaryNonTypeName);

    if (getLangOpts().CPlusPlus)
      MaybeAddOverrideCalls(*this, /*InContext=*/nullptr, Results);
    break;

  case PCC_RecoveryInFunction:
    // Unfiltered
    break;
  }

  // If we are in a C++ non-static member function, check the qualifiers on
  // the member function to filter/prioritize the results list.
  auto ThisType = getCurrentThisType();
  if (!ThisType.isNull())
    Results.setObjectTypeQualifiers(ThisType->getPointeeType().getQualifiers(),
                                    VK_LValue);

  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());

  AddOrdinaryNameResults(CompletionContext, S, *this, Results);
  Results.ExitScope();

  switch (CompletionContext) {
  case PCC_ParenthesizedExpression:
  case PCC_Expression:
  case PCC_Statement:
  case PCC_RecoveryInFunction:
    if (S->getFnParent())
      AddPrettyFunctionResults(getLangOpts(), Results);
    break;

  case PCC_Namespace:
  case PCC_Class:
  case PCC_ObjCInterface:
  case PCC_ObjCImplementation:
  case PCC_ObjCInstanceVariableList:
  case PCC_Template:
  case PCC_MemberTemplate:
  case PCC_ForInit:
  case PCC_Condition:
  case PCC_Type:
  case PCC_LocalDeclarationSpecifiers:
    break;
  }

  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results, CodeCompleter->loadExternal(), false);

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

static void AddClassMessageCompletions(Sema &SemaRef, Scope *S,
                                       ParsedType Receiver,
                                       ArrayRef<IdentifierInfo *> SelIdents,
                                       bool AtArgumentExpression, bool IsSuper,
                                       ResultBuilder &Results);

void Sema::CodeCompleteDeclSpec(Scope *S, DeclSpec &DS,
                                bool AllowNonIdentifiers,
                                bool AllowNestedNameSpecifiers) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(
      *this, CodeCompleter->getAllocator(),
      CodeCompleter->getCodeCompletionTUInfo(),
      AllowNestedNameSpecifiers
          // FIXME: Try to separate codepath leading here to deduce whether we
          // need an existing symbol or a new one.
          ? CodeCompletionContext::CCC_SymbolOrNewName
          : CodeCompletionContext::CCC_NewName);
  Results.EnterNewScope();

  // Type qualifiers can come after names.
  Results.AddResult(Result("const"));
  Results.AddResult(Result("volatile"));
  if (getLangOpts().C99)
    Results.AddResult(Result("restrict"));

  if (getLangOpts().CPlusPlus) {
    if (getLangOpts().CPlusPlus11 &&
        (DS.getTypeSpecType() == DeclSpec::TST_class ||
         DS.getTypeSpecType() == DeclSpec::TST_struct))
      Results.AddResult("final");

    if (AllowNonIdentifiers) {
      Results.AddResult(Result("operator"));
    }

    // Add nested-name-specifiers.
    if (AllowNestedNameSpecifiers) {
      Results.allowNestedNameSpecifiers();
      Results.setFilter(&ResultBuilder::IsImpossibleToSatisfy);
      CodeCompletionDeclConsumer Consumer(Results, CurContext);
      LookupVisibleDecls(S, LookupNestedNameSpecifierName, Consumer,
                         CodeCompleter->includeGlobals(),
                         CodeCompleter->loadExternal());
      Results.setFilter(nullptr);
    }
  }
  Results.ExitScope();

  // If we're in a context where we might have an expression (rather than a
  // declaration), and what we've seen so far is an Objective-C type that could
  // be a receiver of a class message, this may be a class message send with
  // the initial opening bracket '[' missing. Add appropriate completions.
  if (AllowNonIdentifiers && !AllowNestedNameSpecifiers &&
      DS.getParsedSpecifiers() == DeclSpec::PQ_TypeSpecifier &&
      DS.getTypeSpecType() == DeclSpec::TST_typename &&
      DS.getTypeSpecComplex() == DeclSpec::TSC_unspecified &&
      DS.getTypeSpecSign() == DeclSpec::TSS_unspecified &&
      !DS.isTypeAltiVecVector() && S &&
      (S->getFlags() & Scope::DeclScope) != 0 &&
      (S->getFlags() & (Scope::ClassScope | Scope::TemplateParamScope |
                        Scope::FunctionPrototypeScope | Scope::AtCatchScope)) ==
          0) {
    ParsedType T = DS.getRepAsType();
    if (!T.get().isNull() && T.get()->isObjCObjectOrInterfaceType())
      AddClassMessageCompletions(*this, S, T, None, false, false, Results);
  }

  // Note that we intentionally suppress macro results here, since we do not
  // encourage using macros to produce the names of entities.

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

struct Sema::CodeCompleteExpressionData {
  CodeCompleteExpressionData(QualType PreferredType = QualType(),
                             bool IsParenthesized = false)
      : PreferredType(PreferredType), IntegralConstantExpression(false),
        ObjCCollection(false), IsParenthesized(IsParenthesized) {}

  QualType PreferredType;
  bool IntegralConstantExpression;
  bool ObjCCollection;
  bool IsParenthesized;
  SmallVector<Decl *, 4> IgnoreDecls;
};

namespace {
/// Information that allows to avoid completing redundant enumerators.
struct CoveredEnumerators {
  llvm::SmallPtrSet<EnumConstantDecl *, 8> Seen;
  NestedNameSpecifier *SuggestedQualifier = nullptr;
};
} // namespace

static void AddEnumerators(ResultBuilder &Results, ASTContext &Context,
                           EnumDecl *Enum, DeclContext *CurContext,
                           const CoveredEnumerators &Enumerators) {
  NestedNameSpecifier *Qualifier = Enumerators.SuggestedQualifier;
  if (Context.getLangOpts().CPlusPlus && !Qualifier && Enumerators.Seen.empty()) {
    // If there are no prior enumerators in C++, check whether we have to
    // qualify the names of the enumerators that we suggest, because they
    // may not be visible in this scope.
    Qualifier = getRequiredQualification(Context, CurContext, Enum);
  }

  Results.EnterNewScope();
  for (auto *E : Enum->enumerators()) {
    if (Enumerators.Seen.count(E))
      continue;

    CodeCompletionResult R(E, CCP_EnumInCase, Qualifier);
    Results.AddResult(R, CurContext, nullptr, false);
  }
  Results.ExitScope();
}

/// Try to find a corresponding FunctionProtoType for function-like types (e.g.
/// function pointers, std::function, etc).
static const FunctionProtoType *TryDeconstructFunctionLike(QualType T) {
  assert(!T.isNull());
  // Try to extract first template argument from std::function<> and similar.
  // Note we only handle the sugared types, they closely match what users wrote.
  // We explicitly choose to not handle ClassTemplateSpecializationDecl.
  if (auto *Specialization = T->getAs<TemplateSpecializationType>()) {
    if (Specialization->getNumArgs() != 1)
      return nullptr;
    const TemplateArgument &Argument = Specialization->getArg(0);
    if (Argument.getKind() != TemplateArgument::Type)
      return nullptr;
    return Argument.getAsType()->getAs<FunctionProtoType>();
  }
  // Handle other cases.
  if (T->isPointerType())
    T = T->getPointeeType();
  return T->getAs<FunctionProtoType>();
}

/// Adds a pattern completion for a lambda expression with the specified
/// parameter types and placeholders for parameter names.
static void AddLambdaCompletion(ResultBuilder &Results,
                                llvm::ArrayRef<QualType> Parameters,
                                const LangOptions &LangOpts) {
  if (!Results.includeCodePatterns())
    return;
  CodeCompletionBuilder Completion(Results.getAllocator(),
                                   Results.getCodeCompletionTUInfo());
  // [](<parameters>) {}
  Completion.AddChunk(CodeCompletionString::CK_LeftBracket);
  Completion.AddPlaceholderChunk("=");
  Completion.AddChunk(CodeCompletionString::CK_RightBracket);
  if (!Parameters.empty()) {
    Completion.AddChunk(CodeCompletionString::CK_LeftParen);
    bool First = true;
    for (auto Parameter : Parameters) {
      if (!First)
        Completion.AddChunk(CodeCompletionString::ChunkKind::CK_Comma);
      else
        First = false;

      constexpr llvm::StringLiteral NamePlaceholder = "!#!NAME_GOES_HERE!#!";
      std::string Type = NamePlaceholder;
      Parameter.getAsStringInternal(Type, PrintingPolicy(LangOpts));
      llvm::StringRef Prefix, Suffix;
      std::tie(Prefix, Suffix) = llvm::StringRef(Type).split(NamePlaceholder);
      Prefix = Prefix.rtrim();
      Suffix = Suffix.ltrim();

      Completion.AddTextChunk(Completion.getAllocator().CopyString(Prefix));
      Completion.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Completion.AddPlaceholderChunk("parameter");
      Completion.AddTextChunk(Completion.getAllocator().CopyString(Suffix));
    };
    Completion.AddChunk(CodeCompletionString::CK_RightParen);
  }
  Completion.AddChunk(clang::CodeCompletionString::CK_HorizontalSpace);
  Completion.AddChunk(CodeCompletionString::CK_LeftBrace);
  Completion.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Completion.AddPlaceholderChunk("body");
  Completion.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Completion.AddChunk(CodeCompletionString::CK_RightBrace);

  Results.AddResult(Completion.TakeString());
}

/// Perform code-completion in an expression context when we know what
/// type we're looking for.
void Sema::CodeCompleteExpression(Scope *S,
                                  const CodeCompleteExpressionData &Data) {
  ResultBuilder Results(
      *this, CodeCompleter->getAllocator(),
      CodeCompleter->getCodeCompletionTUInfo(),
      CodeCompletionContext(
          Data.IsParenthesized
              ? CodeCompletionContext::CCC_ParenthesizedExpression
              : CodeCompletionContext::CCC_Expression,
          Data.PreferredType));
  auto PCC =
      Data.IsParenthesized ? PCC_ParenthesizedExpression : PCC_Expression;
  if (Data.ObjCCollection)
    Results.setFilter(&ResultBuilder::IsObjCCollection);
  else if (Data.IntegralConstantExpression)
    Results.setFilter(&ResultBuilder::IsIntegralConstantValue);
  else if (WantTypesInContext(PCC, getLangOpts()))
    Results.setFilter(&ResultBuilder::IsOrdinaryName);
  else
    Results.setFilter(&ResultBuilder::IsOrdinaryNonTypeName);

  if (!Data.PreferredType.isNull())
    Results.setPreferredType(Data.PreferredType.getNonReferenceType());

  // Ignore any declarations that we were told that we don't care about.
  for (unsigned I = 0, N = Data.IgnoreDecls.size(); I != N; ++I)
    Results.Ignore(Data.IgnoreDecls[I]);

  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());

  Results.EnterNewScope();
  AddOrdinaryNameResults(PCC, S, *this, Results);
  Results.ExitScope();

  bool PreferredTypeIsPointer = false;
  if (!Data.PreferredType.isNull()) {
    PreferredTypeIsPointer = Data.PreferredType->isAnyPointerType() ||
                             Data.PreferredType->isMemberPointerType() ||
                             Data.PreferredType->isBlockPointerType();
    if (Data.PreferredType->isEnumeralType()) {
      EnumDecl *Enum = Data.PreferredType->castAs<EnumType>()->getDecl();
      if (auto *Def = Enum->getDefinition())
        Enum = Def;
      // FIXME: collect covered enumerators in cases like:
      //        if (x == my_enum::one) { ... } else if (x == ^) {}
      AddEnumerators(Results, Context, Enum, CurContext, CoveredEnumerators());
    }
  }

  if (S->getFnParent() && !Data.ObjCCollection &&
      !Data.IntegralConstantExpression)
    AddPrettyFunctionResults(getLangOpts(), Results);

  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results, CodeCompleter->loadExternal(), false,
                    PreferredTypeIsPointer);

  // Complete a lambda expression when preferred type is a function.
  if (!Data.PreferredType.isNull() && getLangOpts().CPlusPlus11) {
    if (const FunctionProtoType *F =
            TryDeconstructFunctionLike(Data.PreferredType))
      AddLambdaCompletion(Results, F->getParamTypes(), getLangOpts());
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteExpression(Scope *S, QualType PreferredType,
                                  bool IsParenthesized) {
  return CodeCompleteExpression(
      S, CodeCompleteExpressionData(PreferredType, IsParenthesized));
}

void Sema::CodeCompletePostfixExpression(Scope *S, ExprResult E,
                                         QualType PreferredType) {
  if (E.isInvalid())
    CodeCompleteExpression(S, PreferredType);
  else if (getLangOpts().ObjC)
    CodeCompleteObjCInstanceMessage(S, E.get(), None, false);
}

/// The set of properties that have already been added, referenced by
/// property name.
typedef llvm::SmallPtrSet<IdentifierInfo *, 16> AddedPropertiesSet;

/// Retrieve the container definition, if any?
static ObjCContainerDecl *getContainerDef(ObjCContainerDecl *Container) {
  if (ObjCInterfaceDecl *Interface = dyn_cast<ObjCInterfaceDecl>(Container)) {
    if (Interface->hasDefinition())
      return Interface->getDefinition();

    return Interface;
  }

  if (ObjCProtocolDecl *Protocol = dyn_cast<ObjCProtocolDecl>(Container)) {
    if (Protocol->hasDefinition())
      return Protocol->getDefinition();

    return Protocol;
  }
  return Container;
}

/// Adds a block invocation code completion result for the given block
/// declaration \p BD.
static void AddObjCBlockCall(ASTContext &Context, const PrintingPolicy &Policy,
                             CodeCompletionBuilder &Builder,
                             const NamedDecl *BD,
                             const FunctionTypeLoc &BlockLoc,
                             const FunctionProtoTypeLoc &BlockProtoLoc) {
  Builder.AddResultTypeChunk(
      GetCompletionTypeString(BlockLoc.getReturnLoc().getType(), Context,
                              Policy, Builder.getAllocator()));

  AddTypedNameChunk(Context, Policy, BD, Builder);
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);

  if (BlockProtoLoc && BlockProtoLoc.getTypePtr()->isVariadic()) {
    Builder.AddPlaceholderChunk("...");
  } else {
    for (unsigned I = 0, N = BlockLoc.getNumParams(); I != N; ++I) {
      if (I)
        Builder.AddChunk(CodeCompletionString::CK_Comma);

      // Format the placeholder string.
      std::string PlaceholderStr =
          FormatFunctionParameter(Policy, BlockLoc.getParam(I));

      if (I == N - 1 && BlockProtoLoc &&
          BlockProtoLoc.getTypePtr()->isVariadic())
        PlaceholderStr += ", ...";

      // Add the placeholder string.
      Builder.AddPlaceholderChunk(
          Builder.getAllocator().CopyString(PlaceholderStr));
    }
  }

  Builder.AddChunk(CodeCompletionString::CK_RightParen);
}

static void
AddObjCProperties(const CodeCompletionContext &CCContext,
                  ObjCContainerDecl *Container, bool AllowCategories,
                  bool AllowNullaryMethods, DeclContext *CurContext,
                  AddedPropertiesSet &AddedProperties, ResultBuilder &Results,
                  bool IsBaseExprStatement = false,
                  bool IsClassProperty = false, bool InOriginalClass = true) {
  typedef CodeCompletionResult Result;

  // Retrieve the definition.
  Container = getContainerDef(Container);

  // Add properties in this container.
  const auto AddProperty = [&](const ObjCPropertyDecl *P) {
    if (!AddedProperties.insert(P->getIdentifier()).second)
      return;

    // FIXME: Provide block invocation completion for non-statement
    // expressions.
    if (!P->getType().getTypePtr()->isBlockPointerType() ||
        !IsBaseExprStatement) {
      Result R = Result(P, Results.getBasePriority(P), nullptr);
      if (!InOriginalClass)
        setInBaseClass(R);
      Results.MaybeAddResult(R, CurContext);
      return;
    }

    // Block setter and invocation completion is provided only when we are able
    // to find the FunctionProtoTypeLoc with parameter names for the block.
    FunctionTypeLoc BlockLoc;
    FunctionProtoTypeLoc BlockProtoLoc;
    findTypeLocationForBlockDecl(P->getTypeSourceInfo(), BlockLoc,
                                 BlockProtoLoc);
    if (!BlockLoc) {
      Result R = Result(P, Results.getBasePriority(P), nullptr);
      if (!InOriginalClass)
        setInBaseClass(R);
      Results.MaybeAddResult(R, CurContext);
      return;
    }

    // The default completion result for block properties should be the block
    // invocation completion when the base expression is a statement.
    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo());
    AddObjCBlockCall(Container->getASTContext(),
                     getCompletionPrintingPolicy(Results.getSema()), Builder, P,
                     BlockLoc, BlockProtoLoc);
    Result R = Result(Builder.TakeString(), P, Results.getBasePriority(P));
    if (!InOriginalClass)
      setInBaseClass(R);
    Results.MaybeAddResult(R, CurContext);

    // Provide additional block setter completion iff the base expression is a
    // statement and the block property is mutable.
    if (!P->isReadOnly()) {
      CodeCompletionBuilder Builder(Results.getAllocator(),
                                    Results.getCodeCompletionTUInfo());
      AddResultTypeChunk(Container->getASTContext(),
                         getCompletionPrintingPolicy(Results.getSema()), P,
                         CCContext.getBaseType(), Builder);
      Builder.AddTypedTextChunk(
          Results.getAllocator().CopyString(P->getName()));
      Builder.AddChunk(CodeCompletionString::CK_Equal);

      std::string PlaceholderStr = formatBlockPlaceholder(
          getCompletionPrintingPolicy(Results.getSema()), P, BlockLoc,
          BlockProtoLoc, /*SuppressBlockName=*/true);
      // Add the placeholder string.
      Builder.AddPlaceholderChunk(
          Builder.getAllocator().CopyString(PlaceholderStr));

      // When completing blocks properties that return void the default
      // property completion result should show up before the setter,
      // otherwise the setter completion should show up before the default
      // property completion, as we normally want to use the result of the
      // call.
      Result R =
          Result(Builder.TakeString(), P,
                 Results.getBasePriority(P) +
                     (BlockLoc.getTypePtr()->getReturnType()->isVoidType()
                          ? CCD_BlockPropertySetter
                          : -CCD_BlockPropertySetter));
      if (!InOriginalClass)
        setInBaseClass(R);
      Results.MaybeAddResult(R, CurContext);
    }
  };

  if (IsClassProperty) {
    for (const auto *P : Container->class_properties())
      AddProperty(P);
  } else {
    for (const auto *P : Container->instance_properties())
      AddProperty(P);
  }

  // Add nullary methods or implicit class properties
  if (AllowNullaryMethods) {
    ASTContext &Context = Container->getASTContext();
    PrintingPolicy Policy = getCompletionPrintingPolicy(Results.getSema());
    // Adds a method result
    const auto AddMethod = [&](const ObjCMethodDecl *M) {
      IdentifierInfo *Name = M->getSelector().getIdentifierInfoForSlot(0);
      if (!Name)
        return;
      if (!AddedProperties.insert(Name).second)
        return;
      CodeCompletionBuilder Builder(Results.getAllocator(),
                                    Results.getCodeCompletionTUInfo());
      AddResultTypeChunk(Context, Policy, M, CCContext.getBaseType(), Builder);
      Builder.AddTypedTextChunk(
          Results.getAllocator().CopyString(Name->getName()));
      Result R = Result(Builder.TakeString(), M,
                        CCP_MemberDeclaration + CCD_MethodAsProperty);
      if (!InOriginalClass)
        setInBaseClass(R);
      Results.MaybeAddResult(R, CurContext);
    };

    if (IsClassProperty) {
      for (const auto *M : Container->methods()) {
        // Gather the class method that can be used as implicit property
        // getters. Methods with arguments or methods that return void aren't
        // added to the results as they can't be used as a getter.
        if (!M->getSelector().isUnarySelector() ||
            M->getReturnType()->isVoidType() || M->isInstanceMethod())
          continue;
        AddMethod(M);
      }
    } else {
      for (auto *M : Container->methods()) {
        if (M->getSelector().isUnarySelector())
          AddMethod(M);
      }
    }
  }

  // Add properties in referenced protocols.
  if (ObjCProtocolDecl *Protocol = dyn_cast<ObjCProtocolDecl>(Container)) {
    for (auto *P : Protocol->protocols())
      AddObjCProperties(CCContext, P, AllowCategories, AllowNullaryMethods,
                        CurContext, AddedProperties, Results,
                        IsBaseExprStatement, IsClassProperty,
                        /*InOriginalClass*/ false);
  } else if (ObjCInterfaceDecl *IFace =
                 dyn_cast<ObjCInterfaceDecl>(Container)) {
    if (AllowCategories) {
      // Look through categories.
      for (auto *Cat : IFace->known_categories())
        AddObjCProperties(CCContext, Cat, AllowCategories, AllowNullaryMethods,
                          CurContext, AddedProperties, Results,
                          IsBaseExprStatement, IsClassProperty,
                          InOriginalClass);
    }

    // Look through protocols.
    for (auto *I : IFace->all_referenced_protocols())
      AddObjCProperties(CCContext, I, AllowCategories, AllowNullaryMethods,
                        CurContext, AddedProperties, Results,
                        IsBaseExprStatement, IsClassProperty,
                        /*InOriginalClass*/ false);

    // Look in the superclass.
    if (IFace->getSuperClass())
      AddObjCProperties(CCContext, IFace->getSuperClass(), AllowCategories,
                        AllowNullaryMethods, CurContext, AddedProperties,
                        Results, IsBaseExprStatement, IsClassProperty,
                        /*InOriginalClass*/ false);
  } else if (const auto *Category =
                 dyn_cast<ObjCCategoryDecl>(Container)) {
    // Look through protocols.
    for (auto *P : Category->protocols())
      AddObjCProperties(CCContext, P, AllowCategories, AllowNullaryMethods,
                        CurContext, AddedProperties, Results,
                        IsBaseExprStatement, IsClassProperty,
                        /*InOriginalClass*/ false);
  }
}

static void AddRecordMembersCompletionResults(
    Sema &SemaRef, ResultBuilder &Results, Scope *S, QualType BaseType,
    ExprValueKind BaseKind, RecordDecl *RD, Optional<FixItHint> AccessOpFixIt) {
  // Indicate that we are performing a member access, and the cv-qualifiers
  // for the base object type.
  Results.setObjectTypeQualifiers(BaseType.getQualifiers(), BaseKind);

  // Access to a C/C++ class, struct, or union.
  Results.allowNestedNameSpecifiers();
  std::vector<FixItHint> FixIts;
  if (AccessOpFixIt)
    FixIts.emplace_back(AccessOpFixIt.getValue());
  CodeCompletionDeclConsumer Consumer(Results, RD, BaseType, std::move(FixIts));
  SemaRef.LookupVisibleDecls(RD, Sema::LookupMemberName, Consumer,
                             SemaRef.CodeCompleter->includeGlobals(),
                             /*IncludeDependentBases=*/true,
                             SemaRef.CodeCompleter->loadExternal());

  if (SemaRef.getLangOpts().CPlusPlus) {
    if (!Results.empty()) {
      // The "template" keyword can follow "->" or "." in the grammar.
      // However, we only want to suggest the template keyword if something
      // is dependent.
      bool IsDependent = BaseType->isDependentType();
      if (!IsDependent) {
        for (Scope *DepScope = S; DepScope; DepScope = DepScope->getParent())
          if (DeclContext *Ctx = DepScope->getEntity()) {
            IsDependent = Ctx->isDependentContext();
            break;
          }
      }

      if (IsDependent)
        Results.AddResult(CodeCompletionResult("template"));
    }
  }
}

void Sema::CodeCompleteMemberReferenceExpr(Scope *S, Expr *Base,
                                           Expr *OtherOpBase,
                                           SourceLocation OpLoc, bool IsArrow,
                                           bool IsBaseExprStatement,
                                           QualType PreferredType) {
  if (!Base || !CodeCompleter)
    return;

  ExprResult ConvertedBase = PerformMemberExprBaseConversion(Base, IsArrow);
  if (ConvertedBase.isInvalid())
    return;
  QualType ConvertedBaseType = ConvertedBase.get()->getType();

  enum CodeCompletionContext::Kind contextKind;

  if (IsArrow) {
    if (const auto *Ptr = ConvertedBaseType->getAs<PointerType>())
      ConvertedBaseType = Ptr->getPointeeType();
  }

  if (IsArrow) {
    contextKind = CodeCompletionContext::CCC_ArrowMemberAccess;
  } else {
    if (ConvertedBaseType->isObjCObjectPointerType() ||
        ConvertedBaseType->isObjCObjectOrInterfaceType()) {
      contextKind = CodeCompletionContext::CCC_ObjCPropertyAccess;
    } else {
      contextKind = CodeCompletionContext::CCC_DotMemberAccess;
    }
  }

  CodeCompletionContext CCContext(contextKind, ConvertedBaseType);
  CCContext.setPreferredType(PreferredType);
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(), CCContext,
                        &ResultBuilder::IsMember);

  auto DoCompletion = [&](Expr *Base, bool IsArrow,
                          Optional<FixItHint> AccessOpFixIt) -> bool {
    if (!Base)
      return false;

    ExprResult ConvertedBase = PerformMemberExprBaseConversion(Base, IsArrow);
    if (ConvertedBase.isInvalid())
      return false;
    Base = ConvertedBase.get();

    QualType BaseType = Base->getType();
    ExprValueKind BaseKind = Base->getValueKind();

    if (IsArrow) {
      if (const PointerType *Ptr = BaseType->getAs<PointerType>()) {
        BaseType = Ptr->getPointeeType();
        BaseKind = VK_LValue;
      } else if (BaseType->isObjCObjectPointerType())
        /*Do nothing*/;
      else
        return false;
    }

    if (const RecordType *Record = BaseType->getAs<RecordType>()) {
      AddRecordMembersCompletionResults(*this, Results, S, BaseType, BaseKind,
                                        Record->getDecl(),
                                        std::move(AccessOpFixIt));
    } else if (const auto *TST =
                   BaseType->getAs<TemplateSpecializationType>()) {
      TemplateName TN = TST->getTemplateName();
      if (const auto *TD =
              dyn_cast_or_null<ClassTemplateDecl>(TN.getAsTemplateDecl())) {
        CXXRecordDecl *RD = TD->getTemplatedDecl();
        AddRecordMembersCompletionResults(*this, Results, S, BaseType, BaseKind,
                                          RD, std::move(AccessOpFixIt));
      }
    } else if (const auto *ICNT = BaseType->getAs<InjectedClassNameType>()) {
      if (auto *RD = ICNT->getDecl())
        AddRecordMembersCompletionResults(*this, Results, S, BaseType, BaseKind,
                                          RD, std::move(AccessOpFixIt));
    } else if (!IsArrow && BaseType->isObjCObjectPointerType()) {
      // Objective-C property reference.
      AddedPropertiesSet AddedProperties;

      if (const ObjCObjectPointerType *ObjCPtr =
              BaseType->getAsObjCInterfacePointerType()) {
        // Add property results based on our interface.
        assert(ObjCPtr && "Non-NULL pointer guaranteed above!");
        AddObjCProperties(CCContext, ObjCPtr->getInterfaceDecl(), true,
                          /*AllowNullaryMethods=*/true, CurContext,
                          AddedProperties, Results, IsBaseExprStatement);
      }

      // Add properties from the protocols in a qualified interface.
      for (auto *I : BaseType->getAs<ObjCObjectPointerType>()->quals())
        AddObjCProperties(CCContext, I, true, /*AllowNullaryMethods=*/true,
                          CurContext, AddedProperties, Results,
                          IsBaseExprStatement, /*IsClassProperty*/ false,
                          /*InOriginalClass*/ false);
    } else if ((IsArrow && BaseType->isObjCObjectPointerType()) ||
               (!IsArrow && BaseType->isObjCObjectType())) {
      // Objective-C instance variable access.
      ObjCInterfaceDecl *Class = nullptr;
      if (const ObjCObjectPointerType *ObjCPtr =
              BaseType->getAs<ObjCObjectPointerType>())
        Class = ObjCPtr->getInterfaceDecl();
      else
        Class = BaseType->getAs<ObjCObjectType>()->getInterface();

      // Add all ivars from this class and its superclasses.
      if (Class) {
        CodeCompletionDeclConsumer Consumer(Results, Class, BaseType);
        Results.setFilter(&ResultBuilder::IsObjCIvar);
        LookupVisibleDecls(
            Class, LookupMemberName, Consumer, CodeCompleter->includeGlobals(),
            /*IncludeDependentBases=*/false, CodeCompleter->loadExternal());
      }
    }

    // FIXME: How do we cope with isa?
    return true;
  };

  Results.EnterNewScope();

  bool CompletionSucceded = DoCompletion(Base, IsArrow, None);
  if (CodeCompleter->includeFixIts()) {
    const CharSourceRange OpRange =
        CharSourceRange::getTokenRange(OpLoc, OpLoc);
    CompletionSucceded |= DoCompletion(
        OtherOpBase, !IsArrow,
        FixItHint::CreateReplacement(OpRange, IsArrow ? "." : "->"));
  }

  Results.ExitScope();

  if (!CompletionSucceded)
    return;

  // Hand off the results found for code completion.
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCClassPropertyRefExpr(Scope *S,
                                                IdentifierInfo &ClassName,
                                                SourceLocation ClassNameLoc,
                                                bool IsBaseExprStatement) {
  IdentifierInfo *ClassNamePtr = &ClassName;
  ObjCInterfaceDecl *IFace = getObjCInterfaceDecl(ClassNamePtr, ClassNameLoc);
  if (!IFace)
    return;
  CodeCompletionContext CCContext(
      CodeCompletionContext::CCC_ObjCPropertyAccess);
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(), CCContext,
                        &ResultBuilder::IsMember);
  Results.EnterNewScope();
  AddedPropertiesSet AddedProperties;
  AddObjCProperties(CCContext, IFace, true,
                    /*AllowNullaryMethods=*/true, CurContext, AddedProperties,
                    Results, IsBaseExprStatement,
                    /*IsClassProperty=*/true);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteTag(Scope *S, unsigned TagSpec) {
  if (!CodeCompleter)
    return;

  ResultBuilder::LookupFilter Filter = nullptr;
  enum CodeCompletionContext::Kind ContextKind =
      CodeCompletionContext::CCC_Other;
  switch ((DeclSpec::TST)TagSpec) {
  case DeclSpec::TST_enum:
    Filter = &ResultBuilder::IsEnum;
    ContextKind = CodeCompletionContext::CCC_EnumTag;
    break;

  case DeclSpec::TST_union:
    Filter = &ResultBuilder::IsUnion;
    ContextKind = CodeCompletionContext::CCC_UnionTag;
    break;

  case DeclSpec::TST_struct:
  case DeclSpec::TST_class:
  case DeclSpec::TST_interface:
    Filter = &ResultBuilder::IsClassOrStruct;
    ContextKind = CodeCompletionContext::CCC_ClassOrStructTag;
    break;

  default:
    llvm_unreachable("Unknown type specifier kind in CodeCompleteTag");
  }

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(), ContextKind);
  CodeCompletionDeclConsumer Consumer(Results, CurContext);

  // First pass: look for tags.
  Results.setFilter(Filter);
  LookupVisibleDecls(S, LookupTagName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());

  if (CodeCompleter->includeGlobals()) {
    // Second pass: look for nested name specifiers.
    Results.setFilter(&ResultBuilder::IsNestedNameSpecifier);
    LookupVisibleDecls(S, LookupNestedNameSpecifierName, Consumer,
                       CodeCompleter->includeGlobals(),
                       CodeCompleter->loadExternal());
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

static void AddTypeQualifierResults(DeclSpec &DS, ResultBuilder &Results,
                                    const LangOptions &LangOpts) {
  if (!(DS.getTypeQualifiers() & DeclSpec::TQ_const))
    Results.AddResult("const");
  if (!(DS.getTypeQualifiers() & DeclSpec::TQ_volatile))
    Results.AddResult("volatile");
  if (LangOpts.C99 && !(DS.getTypeQualifiers() & DeclSpec::TQ_restrict))
    Results.AddResult("restrict");
  if (LangOpts.C11 && !(DS.getTypeQualifiers() & DeclSpec::TQ_atomic))
    Results.AddResult("_Atomic");
  if (LangOpts.MSVCCompat && !(DS.getTypeQualifiers() & DeclSpec::TQ_unaligned))
    Results.AddResult("__unaligned");
}

void Sema::CodeCompleteTypeQualifiers(DeclSpec &DS) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_TypeQualifiers);
  Results.EnterNewScope();
  AddTypeQualifierResults(DS, Results, LangOpts);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteFunctionQualifiers(DeclSpec &DS, Declarator &D,
                                          const VirtSpecifiers *VS) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_TypeQualifiers);
  Results.EnterNewScope();
  AddTypeQualifierResults(DS, Results, LangOpts);
  if (LangOpts.CPlusPlus11) {
    Results.AddResult("noexcept");
    if (D.getContext() == DeclaratorContext::MemberContext &&
        !D.isCtorOrDtor() && !D.isStaticMember()) {
      if (!VS || !VS->isFinalSpecified())
        Results.AddResult("final");
      if (!VS || !VS->isOverrideSpecified())
        Results.AddResult("override");
    }
  }
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteBracketDeclarator(Scope *S) {
  CodeCompleteExpression(S, QualType(getASTContext().getSizeType()));
}

void Sema::CodeCompleteCase(Scope *S) {
  if (getCurFunction()->SwitchStack.empty() || !CodeCompleter)
    return;

  SwitchStmt *Switch = getCurFunction()->SwitchStack.back().getPointer();
  // Condition expression might be invalid, do not continue in this case.
  if (!Switch->getCond())
    return;
  QualType type = Switch->getCond()->IgnoreImplicit()->getType();
  if (!type->isEnumeralType()) {
    CodeCompleteExpressionData Data(type);
    Data.IntegralConstantExpression = true;
    CodeCompleteExpression(S, Data);
    return;
  }

  // Code-complete the cases of a switch statement over an enumeration type
  // by providing the list of
  EnumDecl *Enum = type->castAs<EnumType>()->getDecl();
  if (EnumDecl *Def = Enum->getDefinition())
    Enum = Def;

  // Determine which enumerators we have already seen in the switch statement.
  // FIXME: Ideally, we would also be able to look *past* the code-completion
  // token, in case we are code-completing in the middle of the switch and not
  // at the end. However, we aren't able to do so at the moment.
  CoveredEnumerators Enumerators;
  for (SwitchCase *SC = Switch->getSwitchCaseList(); SC;
       SC = SC->getNextSwitchCase()) {
    CaseStmt *Case = dyn_cast<CaseStmt>(SC);
    if (!Case)
      continue;

    Expr *CaseVal = Case->getLHS()->IgnoreParenCasts();
    if (auto *DRE = dyn_cast<DeclRefExpr>(CaseVal))
      if (auto *Enumerator =
              dyn_cast<EnumConstantDecl>(DRE->getDecl())) {
        // We look into the AST of the case statement to determine which
        // enumerator was named. Alternatively, we could compute the value of
        // the integral constant expression, then compare it against the
        // values of each enumerator. However, value-based approach would not
        // work as well with C++ templates where enumerators declared within a
        // template are type- and value-dependent.
        Enumerators.Seen.insert(Enumerator);

        // If this is a qualified-id, keep track of the nested-name-specifier
        // so that we can reproduce it as part of code completion, e.g.,
        //
        //   switch (TagD.getKind()) {
        //     case TagDecl::TK_enum:
        //       break;
        //     case XXX
        //
        // At the XXX, our completions are TagDecl::TK_union,
        // TagDecl::TK_struct, and TagDecl::TK_class, rather than TK_union,
        // TK_struct, and TK_class.
        Enumerators.SuggestedQualifier = DRE->getQualifier();
      }
  }

  // Add any enumerators that have not yet been mentioned.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Expression);
  AddEnumerators(Results, Context, Enum, CurContext, Enumerators);

  if (CodeCompleter->includeMacros()) {
    AddMacroResults(PP, Results, CodeCompleter->loadExternal(), false);
  }
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

static bool anyNullArguments(ArrayRef<Expr *> Args) {
  if (Args.size() && !Args.data())
    return true;

  for (unsigned I = 0; I != Args.size(); ++I)
    if (!Args[I])
      return true;

  return false;
}

typedef CodeCompleteConsumer::OverloadCandidate ResultCandidate;

static void mergeCandidatesWithResults(
    Sema &SemaRef, SmallVectorImpl<ResultCandidate> &Results,
    OverloadCandidateSet &CandidateSet, SourceLocation Loc) {
  // Sort the overload candidate set by placing the best overloads first.
  llvm::stable_sort(CandidateSet, [&](const OverloadCandidate &X,
                                      const OverloadCandidate &Y) {
    return isBetterOverloadCandidate(SemaRef, X, Y, Loc,
                                     CandidateSet.getKind());
  });

  // Add the remaining viable overload candidates as code-completion results.
  for (OverloadCandidate &Candidate : CandidateSet) {
    if (Candidate.Function && Candidate.Function->isDeleted())
      continue;
    if (Candidate.Viable)
      Results.push_back(ResultCandidate(Candidate.Function));
  }
}

/// Get the type of the Nth parameter from a given set of overload
/// candidates.
static QualType getParamType(Sema &SemaRef,
                             ArrayRef<ResultCandidate> Candidates, unsigned N) {

  // Given the overloads 'Candidates' for a function call matching all arguments
  // up to N, return the type of the Nth parameter if it is the same for all
  // overload candidates.
  QualType ParamType;
  for (auto &Candidate : Candidates) {
    if (const auto *FType = Candidate.getFunctionType())
      if (const auto *Proto = dyn_cast<FunctionProtoType>(FType))
        if (N < Proto->getNumParams()) {
          if (ParamType.isNull())
            ParamType = Proto->getParamType(N);
          else if (!SemaRef.Context.hasSameUnqualifiedType(
                       ParamType.getNonReferenceType(),
                       Proto->getParamType(N).getNonReferenceType()))
            // Otherwise return a default-constructed QualType.
            return QualType();
        }
  }

  return ParamType;
}

static QualType
ProduceSignatureHelp(Sema &SemaRef, Scope *S,
                     MutableArrayRef<ResultCandidate> Candidates,
                     unsigned CurrentArg, SourceLocation OpenParLoc) {
  if (Candidates.empty())
    return QualType();
  SemaRef.CodeCompleter->ProcessOverloadCandidates(
      SemaRef, CurrentArg, Candidates.data(), Candidates.size(), OpenParLoc);
  return getParamType(SemaRef, Candidates, CurrentArg);
}

QualType Sema::ProduceCallSignatureHelp(Scope *S, Expr *Fn,
                                        ArrayRef<Expr *> Args,
                                        SourceLocation OpenParLoc) {
  if (!CodeCompleter)
    return QualType();

  // FIXME: Provide support for variadic template functions.
  // Ignore type-dependent call expressions entirely.
  if (!Fn || Fn->isTypeDependent() || anyNullArguments(Args) ||
      Expr::hasAnyTypeDependentArguments(Args)) {
    return QualType();
  }

  // Build an overload candidate set based on the functions we find.
  SourceLocation Loc = Fn->getExprLoc();
  OverloadCandidateSet CandidateSet(Loc, OverloadCandidateSet::CSK_Normal);

  SmallVector<ResultCandidate, 8> Results;

  Expr *NakedFn = Fn->IgnoreParenCasts();
  if (auto ULE = dyn_cast<UnresolvedLookupExpr>(NakedFn))
    AddOverloadedCallCandidates(ULE, Args, CandidateSet,
                                /*PartialOverloading=*/true);
  else if (auto UME = dyn_cast<UnresolvedMemberExpr>(NakedFn)) {
    TemplateArgumentListInfo TemplateArgsBuffer, *TemplateArgs = nullptr;
    if (UME->hasExplicitTemplateArgs()) {
      UME->copyTemplateArgumentsInto(TemplateArgsBuffer);
      TemplateArgs = &TemplateArgsBuffer;
    }

    // Add the base as first argument (use a nullptr if the base is implicit).
    SmallVector<Expr *, 12> ArgExprs(
        1, UME->isImplicitAccess() ? nullptr : UME->getBase());
    ArgExprs.append(Args.begin(), Args.end());
    UnresolvedSet<8> Decls;
    Decls.append(UME->decls_begin(), UME->decls_end());
    const bool FirstArgumentIsBase = !UME->isImplicitAccess() && UME->getBase();
    AddFunctionCandidates(Decls, ArgExprs, CandidateSet, TemplateArgs,
                          /*SuppressUsedConversions=*/false,
                          /*PartialOverloading=*/true, FirstArgumentIsBase);
  } else {
    FunctionDecl *FD = nullptr;
    if (auto *MCE = dyn_cast<MemberExpr>(NakedFn))
      FD = dyn_cast<FunctionDecl>(MCE->getMemberDecl());
    else if (auto *DRE = dyn_cast<DeclRefExpr>(NakedFn))
      FD = dyn_cast<FunctionDecl>(DRE->getDecl());
    if (FD) { // We check whether it's a resolved function declaration.
      if (!getLangOpts().CPlusPlus ||
          !FD->getType()->getAs<FunctionProtoType>())
        Results.push_back(ResultCandidate(FD));
      else
        AddOverloadCandidate(FD, DeclAccessPair::make(FD, FD->getAccess()),
                             Args, CandidateSet,
                             /*SuppressUsedConversions=*/false,
                             /*PartialOverloading=*/true);

    } else if (auto DC = NakedFn->getType()->getAsCXXRecordDecl()) {
      // If expression's type is CXXRecordDecl, it may overload the function
      // call operator, so we check if it does and add them as candidates.
      // A complete type is needed to lookup for member function call operators.
      if (isCompleteType(Loc, NakedFn->getType())) {
        DeclarationName OpName =
            Context.DeclarationNames.getCXXOperatorName(OO_Call);
        LookupResult R(*this, OpName, Loc, LookupOrdinaryName);
        LookupQualifiedName(R, DC);
        R.suppressDiagnostics();
        SmallVector<Expr *, 12> ArgExprs(1, NakedFn);
        ArgExprs.append(Args.begin(), Args.end());
        AddFunctionCandidates(R.asUnresolvedSet(), ArgExprs, CandidateSet,
                              /*ExplicitArgs=*/nullptr,
                              /*SuppressUsedConversions=*/false,
                              /*PartialOverloading=*/true);
      }
    } else {
      // Lastly we check whether expression's type is function pointer or
      // function.
      QualType T = NakedFn->getType();
      if (!T->getPointeeType().isNull())
        T = T->getPointeeType();

      if (auto FP = T->getAs<FunctionProtoType>()) {
        if (!TooManyArguments(FP->getNumParams(), Args.size(),
                              /*PartialOverloading=*/true) ||
            FP->isVariadic())
          Results.push_back(ResultCandidate(FP));
      } else if (auto FT = T->getAs<FunctionType>())
        // No prototype and declaration, it may be a K & R style function.
        Results.push_back(ResultCandidate(FT));
    }
  }
  mergeCandidatesWithResults(*this, Results, CandidateSet, Loc);
  QualType ParamType =
      ProduceSignatureHelp(*this, S, Results, Args.size(), OpenParLoc);
  return !CandidateSet.empty() ? ParamType : QualType();
}

QualType Sema::ProduceConstructorSignatureHelp(Scope *S, QualType Type,
                                               SourceLocation Loc,
                                               ArrayRef<Expr *> Args,
                                               SourceLocation OpenParLoc) {
  if (!CodeCompleter)
    return QualType();

  // A complete type is needed to lookup for constructors.
  CXXRecordDecl *RD =
      isCompleteType(Loc, Type) ? Type->getAsCXXRecordDecl() : nullptr;
  if (!RD)
    return Type;

  // FIXME: Provide support for member initializers.
  // FIXME: Provide support for variadic template constructors.

  OverloadCandidateSet CandidateSet(Loc, OverloadCandidateSet::CSK_Normal);

  for (NamedDecl *C : LookupConstructors(RD)) {
    if (auto *FD = dyn_cast<FunctionDecl>(C)) {
      AddOverloadCandidate(FD, DeclAccessPair::make(FD, C->getAccess()), Args,
                           CandidateSet,
                           /*SuppressUsedConversions=*/false,
                           /*PartialOverloading=*/true,
                           /*AllowExplicit*/ true);
    } else if (auto *FTD = dyn_cast<FunctionTemplateDecl>(C)) {
      AddTemplateOverloadCandidate(
          FTD, DeclAccessPair::make(FTD, C->getAccess()),
          /*ExplicitTemplateArgs=*/nullptr, Args, CandidateSet,
          /*SuppressUsedConversions=*/false,
          /*PartialOverloading=*/true);
    }
  }

  SmallVector<ResultCandidate, 8> Results;
  mergeCandidatesWithResults(*this, Results, CandidateSet, Loc);
  return ProduceSignatureHelp(*this, S, Results, Args.size(), OpenParLoc);
}

QualType Sema::ProduceCtorInitMemberSignatureHelp(
    Scope *S, Decl *ConstructorDecl, CXXScopeSpec SS, ParsedType TemplateTypeTy,
    ArrayRef<Expr *> ArgExprs, IdentifierInfo *II, SourceLocation OpenParLoc) {
  if (!CodeCompleter)
    return QualType();

  CXXConstructorDecl *Constructor =
      dyn_cast<CXXConstructorDecl>(ConstructorDecl);
  if (!Constructor)
    return QualType();
  // FIXME: Add support for Base class constructors as well.
  if (ValueDecl *MemberDecl = tryLookupCtorInitMemberDecl(
          Constructor->getParent(), SS, TemplateTypeTy, II))
    return ProduceConstructorSignatureHelp(getCurScope(), MemberDecl->getType(),
                                           MemberDecl->getLocation(), ArgExprs,
                                           OpenParLoc);
  return QualType();
}

void Sema::CodeCompleteInitializer(Scope *S, Decl *D) {
  ValueDecl *VD = dyn_cast_or_null<ValueDecl>(D);
  if (!VD) {
    CodeCompleteOrdinaryName(S, PCC_Expression);
    return;
  }

  CodeCompleteExpressionData Data;
  Data.PreferredType = VD->getType();
  // Ignore VD to avoid completing the variable itself, e.g. in 'int foo = ^'.
  Data.IgnoreDecls.push_back(VD);

  CodeCompleteExpression(S, Data);
}

void Sema::CodeCompleteAfterIf(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        mapCodeCompletionContext(*this, PCC_Statement));
  Results.setFilter(&ResultBuilder::IsOrdinaryName);
  Results.EnterNewScope();

  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());

  AddOrdinaryNameResults(PCC_Statement, S, *this, Results);

  // "else" block
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());
  Builder.AddTypedTextChunk("else");
  if (Results.includeCodePatterns()) {
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
  }
  Results.AddResult(Builder.TakeString());

  // "else if" block
  Builder.AddTypedTextChunk("else if");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  if (getLangOpts().CPlusPlus)
    Builder.AddPlaceholderChunk("condition");
  else
    Builder.AddPlaceholderChunk("expression");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  if (Results.includeCodePatterns()) {
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
  }
  Results.AddResult(Builder.TakeString());

  Results.ExitScope();

  if (S->getFnParent())
    AddPrettyFunctionResults(getLangOpts(), Results);

  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results, CodeCompleter->loadExternal(), false);

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteQualifiedId(Scope *S, CXXScopeSpec &SS,
                                   bool EnteringContext, QualType BaseType,
                                   QualType PreferredType) {
  if (SS.isEmpty() || !CodeCompleter)
    return;

  // We want to keep the scope specifier even if it's invalid (e.g. the scope
  // "a::b::" is not corresponding to any context/namespace in the AST), since
  // it can be useful for global code completion which have information about
  // contexts/symbols that are not in the AST.
  if (SS.isInvalid()) {
    CodeCompletionContext CC(CodeCompletionContext::CCC_Symbol, PreferredType);
    CC.setCXXScopeSpecifier(SS);
    // As SS is invalid, we try to collect accessible contexts from the current
    // scope with a dummy lookup so that the completion consumer can try to
    // guess what the specified scope is.
    ResultBuilder DummyResults(*this, CodeCompleter->getAllocator(),
                               CodeCompleter->getCodeCompletionTUInfo(), CC);
    if (!PreferredType.isNull())
      DummyResults.setPreferredType(PreferredType);
    if (S->getEntity()) {
      CodeCompletionDeclConsumer Consumer(DummyResults, S->getEntity(),
                                          BaseType);
      LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                         /*IncludeGlobalScope=*/false,
                         /*LoadExternal=*/false);
    }
    HandleCodeCompleteResults(this, CodeCompleter,
                              DummyResults.getCompletionContext(), nullptr, 0);
    return;
  }
  // Always pretend to enter a context to ensure that a dependent type
  // resolves to a dependent record.
  DeclContext *Ctx = computeDeclContext(SS, /*EnteringContext=*/true);
  if (!Ctx)
    return;

  // Try to instantiate any non-dependent declaration contexts before
  // we look in them.
  if (!isDependentScopeSpecifier(SS) && RequireCompleteDeclContext(SS, Ctx))
    return;

  ResultBuilder Results(
      *this, CodeCompleter->getAllocator(),
      CodeCompleter->getCodeCompletionTUInfo(),
      CodeCompletionContext(CodeCompletionContext::CCC_Symbol, PreferredType));
  if (!PreferredType.isNull())
    Results.setPreferredType(PreferredType);
  Results.EnterNewScope();

  // The "template" keyword can follow "::" in the grammar, but only
  // put it into the grammar if the nested-name-specifier is dependent.
  NestedNameSpecifier *NNS = SS.getScopeRep();
  if (!Results.empty() && NNS->isDependent())
    Results.AddResult("template");

  // Add calls to overridden virtual functions, if there are any.
  //
  // FIXME: This isn't wonderful, because we don't know whether we're actually
  // in a context that permits expressions. This is a general issue with
  // qualified-id completions.
  if (!EnteringContext)
    MaybeAddOverrideCalls(*this, Ctx, Results);
  Results.ExitScope();

  if (CodeCompleter->includeNamespaceLevelDecls() ||
      (!Ctx->isNamespace() && !Ctx->isTranslationUnit())) {
    CodeCompletionDeclConsumer Consumer(Results, Ctx, BaseType);
    LookupVisibleDecls(Ctx, LookupOrdinaryName, Consumer,
                       /*IncludeGlobalScope=*/true,
                       /*IncludeDependentBases=*/true,
                       CodeCompleter->loadExternal());
  }

  auto CC = Results.getCompletionContext();
  CC.setCXXScopeSpecifier(SS);

  HandleCodeCompleteResults(this, CodeCompleter, CC, Results.data(),
                            Results.size());
}

void Sema::CodeCompleteUsing(Scope *S) {
  if (!CodeCompleter)
    return;

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        // This can be both a using alias or using
                        // declaration, in the former we expect a new name and a
                        // symbol in the latter case.
                        CodeCompletionContext::CCC_SymbolOrNewName,
                        &ResultBuilder::IsNestedNameSpecifier);
  Results.EnterNewScope();

  // If we aren't in class scope, we could see the "namespace" keyword.
  if (!S->isClassScope())
    Results.AddResult(CodeCompletionResult("namespace"));

  // After "using", we can see anything that would start a
  // nested-name-specifier.
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteUsingDirective(Scope *S) {
  if (!CodeCompleter)
    return;

  // After "using namespace", we expect to see a namespace name or namespace
  // alias.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Namespace,
                        &ResultBuilder::IsNamespaceOrAlias);
  Results.EnterNewScope();
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteNamespaceDecl(Scope *S) {
  if (!CodeCompleter)
    return;

  DeclContext *Ctx = S->getEntity();
  if (!S->getParent())
    Ctx = Context.getTranslationUnitDecl();

  bool SuppressedGlobalResults =
      Ctx && !CodeCompleter->includeGlobals() && isa<TranslationUnitDecl>(Ctx);

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        SuppressedGlobalResults
                            ? CodeCompletionContext::CCC_Namespace
                            : CodeCompletionContext::CCC_Other,
                        &ResultBuilder::IsNamespace);

  if (Ctx && Ctx->isFileContext() && !SuppressedGlobalResults) {
    // We only want to see those namespaces that have already been defined
    // within this scope, because its likely that the user is creating an
    // extended namespace declaration. Keep track of the most recent
    // definition of each namespace.
    std::map<NamespaceDecl *, NamespaceDecl *> OrigToLatest;
    for (DeclContext::specific_decl_iterator<NamespaceDecl>
             NS(Ctx->decls_begin()),
         NSEnd(Ctx->decls_end());
         NS != NSEnd; ++NS)
      OrigToLatest[NS->getOriginalNamespace()] = *NS;

    // Add the most recent definition (or extended definition) of each
    // namespace to the list of results.
    Results.EnterNewScope();
    for (std::map<NamespaceDecl *, NamespaceDecl *>::iterator
             NS = OrigToLatest.begin(),
             NSEnd = OrigToLatest.end();
         NS != NSEnd; ++NS)
      Results.AddResult(
          CodeCompletionResult(NS->second, Results.getBasePriority(NS->second),
                               nullptr),
          CurContext, nullptr, false);
    Results.ExitScope();
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteNamespaceAliasDecl(Scope *S) {
  if (!CodeCompleter)
    return;

  // After "namespace", we expect to see a namespace or alias.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Namespace,
                        &ResultBuilder::IsNamespaceOrAlias);
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteOperatorName(Scope *S) {
  if (!CodeCompleter)
    return;

  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Type,
                        &ResultBuilder::IsType);
  Results.EnterNewScope();

  // Add the names of overloadable operators.
#define OVERLOADED_OPERATOR(Name, Spelling, Token, Unary, Binary, MemberOnly)  \
  if (std::strcmp(Spelling, "?"))                                              \
    Results.AddResult(Result(Spelling));
#include "clang/Basic/OperatorKinds.def"

  // Add any type names visible from the current scope
  Results.allowNestedNameSpecifiers();
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());

  // Add any type specifiers
  AddTypeSpecifierResults(getLangOpts(), Results);
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteConstructorInitializer(
    Decl *ConstructorD, ArrayRef<CXXCtorInitializer *> Initializers) {
  if (!ConstructorD)
    return;

  AdjustDeclIfTemplate(ConstructorD);

  auto *Constructor = dyn_cast<CXXConstructorDecl>(ConstructorD);
  if (!Constructor)
    return;

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Symbol);
  Results.EnterNewScope();

  // Fill in any already-initialized fields or base classes.
  llvm::SmallPtrSet<FieldDecl *, 4> InitializedFields;
  llvm::SmallPtrSet<CanQualType, 4> InitializedBases;
  for (unsigned I = 0, E = Initializers.size(); I != E; ++I) {
    if (Initializers[I]->isBaseInitializer())
      InitializedBases.insert(Context.getCanonicalType(
          QualType(Initializers[I]->getBaseClass(), 0)));
    else
      InitializedFields.insert(
          cast<FieldDecl>(Initializers[I]->getAnyMember()));
  }

  // Add completions for base classes.
  PrintingPolicy Policy = getCompletionPrintingPolicy(*this);
  bool SawLastInitializer = Initializers.empty();
  CXXRecordDecl *ClassDecl = Constructor->getParent();

  auto GenerateCCS = [&](const NamedDecl *ND, const char *Name) {
    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo());
    Builder.AddTypedTextChunk(Name);
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    if (const auto *Function = dyn_cast<FunctionDecl>(ND))
      AddFunctionParameterChunks(PP, Policy, Function, Builder);
    else if (const auto *FunTemplDecl = dyn_cast<FunctionTemplateDecl>(ND))
      AddFunctionParameterChunks(PP, Policy, FunTemplDecl->getTemplatedDecl(),
                                 Builder);
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    return Builder.TakeString();
  };
  auto AddDefaultCtorInit = [&](const char *Name, const char *Type,
                                const NamedDecl *ND) {
    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo());
    Builder.AddTypedTextChunk(Name);
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk(Type);
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    if (ND) {
      auto CCR = CodeCompletionResult(
          Builder.TakeString(), ND,
          SawLastInitializer ? CCP_NextInitializer : CCP_MemberDeclaration);
      if (isa<FieldDecl>(ND))
        CCR.CursorKind = CXCursor_MemberRef;
      return Results.AddResult(CCR);
    }
    return Results.AddResult(CodeCompletionResult(
        Builder.TakeString(),
        SawLastInitializer ? CCP_NextInitializer : CCP_MemberDeclaration));
  };
  auto AddCtorsWithName = [&](const CXXRecordDecl *RD, unsigned int Priority,
                              const char *Name, const FieldDecl *FD) {
    if (!RD)
      return AddDefaultCtorInit(Name,
                                FD ? Results.getAllocator().CopyString(
                                         FD->getType().getAsString(Policy))
                                   : Name,
                                FD);
    auto Ctors = getConstructors(Context, RD);
    if (Ctors.begin() == Ctors.end())
      return AddDefaultCtorInit(Name, Name, RD);
    for (const NamedDecl *Ctor : Ctors) {
      auto CCR = CodeCompletionResult(GenerateCCS(Ctor, Name), RD, Priority);
      CCR.CursorKind = getCursorKindForDecl(Ctor);
      Results.AddResult(CCR);
    }
  };
  auto AddBase = [&](const CXXBaseSpecifier &Base) {
    const char *BaseName =
        Results.getAllocator().CopyString(Base.getType().getAsString(Policy));
    const auto *RD = Base.getType()->getAsCXXRecordDecl();
    AddCtorsWithName(
        RD, SawLastInitializer ? CCP_NextInitializer : CCP_MemberDeclaration,
        BaseName, nullptr);
  };
  auto AddField = [&](const FieldDecl *FD) {
    const char *FieldName =
        Results.getAllocator().CopyString(FD->getIdentifier()->getName());
    const CXXRecordDecl *RD = FD->getType()->getAsCXXRecordDecl();
    AddCtorsWithName(
        RD, SawLastInitializer ? CCP_NextInitializer : CCP_MemberDeclaration,
        FieldName, FD);
  };

  for (const auto &Base : ClassDecl->bases()) {
    if (!InitializedBases.insert(Context.getCanonicalType(Base.getType()))
             .second) {
      SawLastInitializer =
          !Initializers.empty() && Initializers.back()->isBaseInitializer() &&
          Context.hasSameUnqualifiedType(
              Base.getType(), QualType(Initializers.back()->getBaseClass(), 0));
      continue;
    }

    AddBase(Base);
    SawLastInitializer = false;
  }

  // Add completions for virtual base classes.
  for (const auto &Base : ClassDecl->vbases()) {
    if (!InitializedBases.insert(Context.getCanonicalType(Base.getType()))
             .second) {
      SawLastInitializer =
          !Initializers.empty() && Initializers.back()->isBaseInitializer() &&
          Context.hasSameUnqualifiedType(
              Base.getType(), QualType(Initializers.back()->getBaseClass(), 0));
      continue;
    }

    AddBase(Base);
    SawLastInitializer = false;
  }

  // Add completions for members.
  for (auto *Field : ClassDecl->fields()) {
    if (!InitializedFields.insert(cast<FieldDecl>(Field->getCanonicalDecl()))
             .second) {
      SawLastInitializer = !Initializers.empty() &&
                           Initializers.back()->isAnyMemberInitializer() &&
                           Initializers.back()->getAnyMember() == Field;
      continue;
    }

    if (!Field->getDeclName())
      continue;

    AddField(Field);
    SawLastInitializer = false;
  }
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

/// Determine whether this scope denotes a namespace.
static bool isNamespaceScope(Scope *S) {
  DeclContext *DC = S->getEntity();
  if (!DC)
    return false;

  return DC->isFileContext();
}

void Sema::CodeCompleteLambdaIntroducer(Scope *S, LambdaIntroducer &Intro,
                                        bool AfterAmpersand) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();

  // Note what has already been captured.
  llvm::SmallPtrSet<IdentifierInfo *, 4> Known;
  bool IncludedThis = false;
  for (const auto &C : Intro.Captures) {
    if (C.Kind == LCK_This) {
      IncludedThis = true;
      continue;
    }

    Known.insert(C.Id);
  }

  // Look for other capturable variables.
  for (; S && !isNamespaceScope(S); S = S->getParent()) {
    for (const auto *D : S->decls()) {
      const auto *Var = dyn_cast<VarDecl>(D);
      if (!Var || !Var->hasLocalStorage() || Var->hasAttr<BlocksAttr>())
        continue;

      if (Known.insert(Var->getIdentifier()).second)
        Results.AddResult(CodeCompletionResult(Var, CCP_LocalDeclaration),
                          CurContext, nullptr, false);
    }
  }

  // Add 'this', if it would be valid.
  if (!IncludedThis && !AfterAmpersand && Intro.Default != LCD_ByCopy)
    addThisCompletion(*this, Results);

  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

/// Macro that optionally prepends an "@" to the string literal passed in via
/// Keyword, depending on whether NeedAt is true or false.
#define OBJC_AT_KEYWORD_NAME(NeedAt, Keyword) ((NeedAt) ? "@" Keyword : Keyword)

static void AddObjCImplementationResults(const LangOptions &LangOpts,
                                         ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  // Since we have an implementation, we can end it.
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "end")));

  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());
  if (LangOpts.ObjC) {
    // @dynamic
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "dynamic"));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("property");
    Results.AddResult(Result(Builder.TakeString()));

    // @synthesize
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "synthesize"));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("property");
    Results.AddResult(Result(Builder.TakeString()));
  }
}

static void AddObjCInterfaceResults(const LangOptions &LangOpts,
                                    ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;

  // Since we have an interface or protocol, we can end it.
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "end")));

  if (LangOpts.ObjC) {
    // @property
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "property")));

    // @required
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "required")));

    // @optional
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "optional")));
  }
}

static void AddObjCTopLevelResults(ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());

  // @class name ;
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "class"));
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("name");
  Results.AddResult(Result(Builder.TakeString()));

  if (Results.includeCodePatterns()) {
    // @interface name
    // FIXME: Could introduce the whole pattern, including superclasses and
    // such.
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "interface"));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("class");
    Results.AddResult(Result(Builder.TakeString()));

    // @protocol name
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "protocol"));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("protocol");
    Results.AddResult(Result(Builder.TakeString()));

    // @implementation name
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "implementation"));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("class");
    Results.AddResult(Result(Builder.TakeString()));
  }

  // @compatibility_alias name
  Builder.AddTypedTextChunk(
      OBJC_AT_KEYWORD_NAME(NeedAt, "compatibility_alias"));
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("alias");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("class");
  Results.AddResult(Result(Builder.TakeString()));

  if (Results.getSema().getLangOpts().Modules) {
    // @import name
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "import"));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("module");
    Results.AddResult(Result(Builder.TakeString()));
  }
}

void Sema::CodeCompleteObjCAtDirective(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  if (isa<ObjCImplDecl>(CurContext))
    AddObjCImplementationResults(getLangOpts(), Results, false);
  else if (CurContext->isObjCContainer())
    AddObjCInterfaceResults(getLangOpts(), Results, false);
  else
    AddObjCTopLevelResults(Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

static void AddObjCExpressionResults(ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());

  // @encode ( type-name )
  const char *EncodeType = "char[]";
  if (Results.getSema().getLangOpts().CPlusPlus ||
      Results.getSema().getLangOpts().ConstStrings)
    EncodeType = "const char[]";
  Builder.AddResultTypeChunk(EncodeType);
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "encode"));
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("type-name");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Result(Builder.TakeString()));

  // @protocol ( protocol-name )
  Builder.AddResultTypeChunk("Protocol *");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "protocol"));
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("protocol-name");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Result(Builder.TakeString()));

  // @selector ( selector )
  Builder.AddResultTypeChunk("SEL");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "selector"));
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("selector");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Result(Builder.TakeString()));

  // @"string"
  Builder.AddResultTypeChunk("NSString *");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "\""));
  Builder.AddPlaceholderChunk("string");
  Builder.AddTextChunk("\"");
  Results.AddResult(Result(Builder.TakeString()));

  // @[objects, ...]
  Builder.AddResultTypeChunk("NSArray *");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "["));
  Builder.AddPlaceholderChunk("objects, ...");
  Builder.AddChunk(CodeCompletionString::CK_RightBracket);
  Results.AddResult(Result(Builder.TakeString()));

  // @{key : object, ...}
  Builder.AddResultTypeChunk("NSDictionary *");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "{"));
  Builder.AddPlaceholderChunk("key");
  Builder.AddChunk(CodeCompletionString::CK_Colon);
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("object, ...");
  Builder.AddChunk(CodeCompletionString::CK_RightBrace);
  Results.AddResult(Result(Builder.TakeString()));

  // @(expression)
  Builder.AddResultTypeChunk("id");
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "("));
  Builder.AddPlaceholderChunk("expression");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Result(Builder.TakeString()));
}

static void AddObjCStatementResults(ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());

  if (Results.includeCodePatterns()) {
    // @try { statements } @catch ( declaration ) { statements } @finally
    //   { statements }
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "try"));
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Builder.AddTextChunk("@catch");
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("parameter");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Builder.AddTextChunk("@finally");
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Results.AddResult(Result(Builder.TakeString()));
  }

  // @throw
  Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "throw"));
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("expression");
  Results.AddResult(Result(Builder.TakeString()));

  if (Results.includeCodePatterns()) {
    // @synchronized ( expression ) { statements }
    Builder.AddTypedTextChunk(OBJC_AT_KEYWORD_NAME(NeedAt, "synchronized"));
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddPlaceholderChunk("expression");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
    Builder.AddPlaceholderChunk("statements");
    Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    Results.AddResult(Result(Builder.TakeString()));
  }
}

static void AddObjCVisibilityResults(const LangOptions &LangOpts,
                                     ResultBuilder &Results, bool NeedAt) {
  typedef CodeCompletionResult Result;
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "private")));
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "protected")));
  Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "public")));
  if (LangOpts.ObjC)
    Results.AddResult(Result(OBJC_AT_KEYWORD_NAME(NeedAt, "package")));
}

void Sema::CodeCompleteObjCAtVisibility(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  AddObjCVisibilityResults(getLangOpts(), Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCAtStatement(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  AddObjCStatementResults(Results, false);
  AddObjCExpressionResults(Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCAtExpression(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  AddObjCExpressionResults(Results, false);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

/// Determine whether the addition of the given flag to an Objective-C
/// property's attributes will cause a conflict.
static bool ObjCPropertyFlagConflicts(unsigned Attributes, unsigned NewFlag) {
  // Check if we've already added this flag.
  if (Attributes & NewFlag)
    return true;

  Attributes |= NewFlag;

  // Check for collisions with "readonly".
  if ((Attributes & ObjCDeclSpec::DQ_PR_readonly) &&
      (Attributes & ObjCDeclSpec::DQ_PR_readwrite))
    return true;

  // Check for more than one of { assign, copy, retain, strong, weak }.
  unsigned AssignCopyRetMask =
      Attributes &
      (ObjCDeclSpec::DQ_PR_assign | ObjCDeclSpec::DQ_PR_unsafe_unretained |
       ObjCDeclSpec::DQ_PR_copy | ObjCDeclSpec::DQ_PR_retain |
       ObjCDeclSpec::DQ_PR_strong | ObjCDeclSpec::DQ_PR_weak);
  if (AssignCopyRetMask && AssignCopyRetMask != ObjCDeclSpec::DQ_PR_assign &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_unsafe_unretained &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_copy &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_retain &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_strong &&
      AssignCopyRetMask != ObjCDeclSpec::DQ_PR_weak)
    return true;

  return false;
}

void Sema::CodeCompleteObjCPropertyFlags(Scope *S, ObjCDeclSpec &ODS) {
  if (!CodeCompleter)
    return;

  unsigned Attributes = ODS.getPropertyAttributes();

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_readonly))
    Results.AddResult(CodeCompletionResult("readonly"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_assign))
    Results.AddResult(CodeCompletionResult("assign"));
  if (!ObjCPropertyFlagConflicts(Attributes,
                                 ObjCDeclSpec::DQ_PR_unsafe_unretained))
    Results.AddResult(CodeCompletionResult("unsafe_unretained"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_readwrite))
    Results.AddResult(CodeCompletionResult("readwrite"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_retain))
    Results.AddResult(CodeCompletionResult("retain"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_strong))
    Results.AddResult(CodeCompletionResult("strong"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_copy))
    Results.AddResult(CodeCompletionResult("copy"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_nonatomic))
    Results.AddResult(CodeCompletionResult("nonatomic"));
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_atomic))
    Results.AddResult(CodeCompletionResult("atomic"));

  // Only suggest "weak" if we're compiling for ARC-with-weak-references or GC.
  if (getLangOpts().ObjCWeak || getLangOpts().getGC() != LangOptions::NonGC)
    if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_weak))
      Results.AddResult(CodeCompletionResult("weak"));

  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_setter)) {
    CodeCompletionBuilder Setter(Results.getAllocator(),
                                 Results.getCodeCompletionTUInfo());
    Setter.AddTypedTextChunk("setter");
    Setter.AddTextChunk("=");
    Setter.AddPlaceholderChunk("method");
    Results.AddResult(CodeCompletionResult(Setter.TakeString()));
  }
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_getter)) {
    CodeCompletionBuilder Getter(Results.getAllocator(),
                                 Results.getCodeCompletionTUInfo());
    Getter.AddTypedTextChunk("getter");
    Getter.AddTextChunk("=");
    Getter.AddPlaceholderChunk("method");
    Results.AddResult(CodeCompletionResult(Getter.TakeString()));
  }
  if (!ObjCPropertyFlagConflicts(Attributes, ObjCDeclSpec::DQ_PR_nullability)) {
    Results.AddResult(CodeCompletionResult("nonnull"));
    Results.AddResult(CodeCompletionResult("nullable"));
    Results.AddResult(CodeCompletionResult("null_unspecified"));
    Results.AddResult(CodeCompletionResult("null_resettable"));
  }
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

/// Describes the kind of Objective-C method that we want to find
/// via code completion.
enum ObjCMethodKind {
  MK_Any, ///< Any kind of method, provided it means other specified criteria.
  MK_ZeroArgSelector, ///< Zero-argument (unary) selector.
  MK_OneArgSelector   ///< One-argument selector.
};

static bool isAcceptableObjCSelector(Selector Sel, ObjCMethodKind WantKind,
                                     ArrayRef<IdentifierInfo *> SelIdents,
                                     bool AllowSameLength = true) {
  unsigned NumSelIdents = SelIdents.size();
  if (NumSelIdents > Sel.getNumArgs())
    return false;

  switch (WantKind) {
  case MK_Any:
    break;
  case MK_ZeroArgSelector:
    return Sel.isUnarySelector();
  case MK_OneArgSelector:
    return Sel.getNumArgs() == 1;
  }

  if (!AllowSameLength && NumSelIdents && NumSelIdents == Sel.getNumArgs())
    return false;

  for (unsigned I = 0; I != NumSelIdents; ++I)
    if (SelIdents[I] != Sel.getIdentifierInfoForSlot(I))
      return false;

  return true;
}

static bool isAcceptableObjCMethod(ObjCMethodDecl *Method,
                                   ObjCMethodKind WantKind,
                                   ArrayRef<IdentifierInfo *> SelIdents,
                                   bool AllowSameLength = true) {
  return isAcceptableObjCSelector(Method->getSelector(), WantKind, SelIdents,
                                  AllowSameLength);
}

/// A set of selectors, which is used to avoid introducing multiple
/// completions with the same selector into the result set.
typedef llvm::SmallPtrSet<Selector, 16> VisitedSelectorSet;

/// Add all of the Objective-C methods in the given Objective-C
/// container to the set of results.
///
/// The container will be a class, protocol, category, or implementation of
/// any of the above. This mether will recurse to include methods from
/// the superclasses of classes along with their categories, protocols, and
/// implementations.
///
/// \param Container the container in which we'll look to find methods.
///
/// \param WantInstanceMethods Whether to add instance methods (only); if
/// false, this routine will add factory methods (only).
///
/// \param CurContext the context in which we're performing the lookup that
/// finds methods.
///
/// \param AllowSameLength Whether we allow a method to be added to the list
/// when it has the same number of parameters as we have selector identifiers.
///
/// \param Results the structure into which we'll add results.
static void AddObjCMethods(ObjCContainerDecl *Container,
                           bool WantInstanceMethods, ObjCMethodKind WantKind,
                           ArrayRef<IdentifierInfo *> SelIdents,
                           DeclContext *CurContext,
                           VisitedSelectorSet &Selectors, bool AllowSameLength,
                           ResultBuilder &Results, bool InOriginalClass = true,
                           bool IsRootClass = false) {
  typedef CodeCompletionResult Result;
  Container = getContainerDef(Container);
  ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>(Container);
  IsRootClass = IsRootClass || (IFace && !IFace->getSuperClass());
  for (ObjCMethodDecl *M : Container->methods()) {
    // The instance methods on the root class can be messaged via the
    // metaclass.
    if (M->isInstanceMethod() == WantInstanceMethods ||
        (IsRootClass && !WantInstanceMethods)) {
      // Check whether the selector identifiers we've been given are a
      // subset of the identifiers for this particular method.
      if (!isAcceptableObjCMethod(M, WantKind, SelIdents, AllowSameLength))
        continue;

      if (!Selectors.insert(M->getSelector()).second)
        continue;

      Result R = Result(M, Results.getBasePriority(M), nullptr);
      R.StartParameter = SelIdents.size();
      R.AllParametersAreInformative = (WantKind != MK_Any);
      if (!InOriginalClass)
        setInBaseClass(R);
      Results.MaybeAddResult(R, CurContext);
    }
  }

  // Visit the protocols of protocols.
  if (const auto *Protocol = dyn_cast<ObjCProtocolDecl>(Container)) {
    if (Protocol->hasDefinition()) {
      const ObjCList<ObjCProtocolDecl> &Protocols =
          Protocol->getReferencedProtocols();
      for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                                E = Protocols.end();
           I != E; ++I)
        AddObjCMethods(*I, WantInstanceMethods, WantKind, SelIdents, CurContext,
                       Selectors, AllowSameLength, Results, false, IsRootClass);
    }
  }

  if (!IFace || !IFace->hasDefinition())
    return;

  // Add methods in protocols.
  for (ObjCProtocolDecl *I : IFace->protocols())
    AddObjCMethods(I, WantInstanceMethods, WantKind, SelIdents, CurContext,
                   Selectors, AllowSameLength, Results, false, IsRootClass);

  // Add methods in categories.
  for (ObjCCategoryDecl *CatDecl : IFace->known_categories()) {
    AddObjCMethods(CatDecl, WantInstanceMethods, WantKind, SelIdents,
                   CurContext, Selectors, AllowSameLength, Results,
                   InOriginalClass, IsRootClass);

    // Add a categories protocol methods.
    const ObjCList<ObjCProtocolDecl> &Protocols =
        CatDecl->getReferencedProtocols();
    for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                              E = Protocols.end();
         I != E; ++I)
      AddObjCMethods(*I, WantInstanceMethods, WantKind, SelIdents, CurContext,
                     Selectors, AllowSameLength, Results, false, IsRootClass);

    // Add methods in category implementations.
    if (ObjCCategoryImplDecl *Impl = CatDecl->getImplementation())
      AddObjCMethods(Impl, WantInstanceMethods, WantKind, SelIdents, CurContext,
                     Selectors, AllowSameLength, Results, InOriginalClass,
                     IsRootClass);
  }

  // Add methods in superclass.
  // Avoid passing in IsRootClass since root classes won't have super classes.
  if (IFace->getSuperClass())
    AddObjCMethods(IFace->getSuperClass(), WantInstanceMethods, WantKind,
                   SelIdents, CurContext, Selectors, AllowSameLength, Results,
                   /*IsRootClass=*/false);

  // Add methods in our implementation, if any.
  if (ObjCImplementationDecl *Impl = IFace->getImplementation())
    AddObjCMethods(Impl, WantInstanceMethods, WantKind, SelIdents, CurContext,
                   Selectors, AllowSameLength, Results, InOriginalClass,
                   IsRootClass);
}

void Sema::CodeCompleteObjCPropertyGetter(Scope *S) {
  // Try to find the interface where getters might live.
  ObjCInterfaceDecl *Class = dyn_cast_or_null<ObjCInterfaceDecl>(CurContext);
  if (!Class) {
    if (ObjCCategoryDecl *Category =
            dyn_cast_or_null<ObjCCategoryDecl>(CurContext))
      Class = Category->getClassInterface();

    if (!Class)
      return;
  }

  // Find all of the potential getters.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();

  VisitedSelectorSet Selectors;
  AddObjCMethods(Class, true, MK_ZeroArgSelector, None, CurContext, Selectors,
                 /*AllowSameLength=*/true, Results);
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCPropertySetter(Scope *S) {
  // Try to find the interface where setters might live.
  ObjCInterfaceDecl *Class = dyn_cast_or_null<ObjCInterfaceDecl>(CurContext);
  if (!Class) {
    if (ObjCCategoryDecl *Category =
            dyn_cast_or_null<ObjCCategoryDecl>(CurContext))
      Class = Category->getClassInterface();

    if (!Class)
      return;
  }

  // Find all of the potential getters.
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();

  VisitedSelectorSet Selectors;
  AddObjCMethods(Class, true, MK_OneArgSelector, None, CurContext, Selectors,
                 /*AllowSameLength=*/true, Results);

  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCPassingType(Scope *S, ObjCDeclSpec &DS,
                                       bool IsParameter) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Type);
  Results.EnterNewScope();

  // Add context-sensitive, Objective-C parameter-passing keywords.
  bool AddedInOut = false;
  if ((DS.getObjCDeclQualifier() &
       (ObjCDeclSpec::DQ_In | ObjCDeclSpec::DQ_Inout)) == 0) {
    Results.AddResult("in");
    Results.AddResult("inout");
    AddedInOut = true;
  }
  if ((DS.getObjCDeclQualifier() &
       (ObjCDeclSpec::DQ_Out | ObjCDeclSpec::DQ_Inout)) == 0) {
    Results.AddResult("out");
    if (!AddedInOut)
      Results.AddResult("inout");
  }
  if ((DS.getObjCDeclQualifier() &
       (ObjCDeclSpec::DQ_Bycopy | ObjCDeclSpec::DQ_Byref |
        ObjCDeclSpec::DQ_Oneway)) == 0) {
    Results.AddResult("bycopy");
    Results.AddResult("byref");
    Results.AddResult("oneway");
  }
  if ((DS.getObjCDeclQualifier() & ObjCDeclSpec::DQ_CSNullability) == 0) {
    Results.AddResult("nonnull");
    Results.AddResult("nullable");
    Results.AddResult("null_unspecified");
  }

  // If we're completing the return type of an Objective-C method and the
  // identifier IBAction refers to a macro, provide a completion item for
  // an action, e.g.,
  //   IBAction)<#selector#>:(id)sender
  if (DS.getObjCDeclQualifier() == 0 && !IsParameter &&
      PP.isMacroDefined("IBAction")) {
    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo(),
                                  CCP_CodePattern, CXAvailability_Available);
    Builder.AddTypedTextChunk("IBAction");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Builder.AddPlaceholderChunk("selector");
    Builder.AddChunk(CodeCompletionString::CK_Colon);
    Builder.AddChunk(CodeCompletionString::CK_LeftParen);
    Builder.AddTextChunk("id");
    Builder.AddChunk(CodeCompletionString::CK_RightParen);
    Builder.AddTextChunk("sender");
    Results.AddResult(CodeCompletionResult(Builder.TakeString()));
  }

  // If we're completing the return type, provide 'instancetype'.
  if (!IsParameter) {
    Results.AddResult(CodeCompletionResult("instancetype"));
  }

  // Add various builtin type names and specifiers.
  AddOrdinaryNameResults(PCC_Type, S, *this, Results);
  Results.ExitScope();

  // Add the various type names
  Results.setFilter(&ResultBuilder::IsOrdinaryNonValueName);
  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());

  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results, CodeCompleter->loadExternal(), false);

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

/// When we have an expression with type "id", we may assume
/// that it has some more-specific class type based on knowledge of
/// common uses of Objective-C. This routine returns that class type,
/// or NULL if no better result could be determined.
static ObjCInterfaceDecl *GetAssumedMessageSendExprType(Expr *E) {
  auto *Msg = dyn_cast_or_null<ObjCMessageExpr>(E);
  if (!Msg)
    return nullptr;

  Selector Sel = Msg->getSelector();
  if (Sel.isNull())
    return nullptr;

  IdentifierInfo *Id = Sel.getIdentifierInfoForSlot(0);
  if (!Id)
    return nullptr;

  ObjCMethodDecl *Method = Msg->getMethodDecl();
  if (!Method)
    return nullptr;

  // Determine the class that we're sending the message to.
  ObjCInterfaceDecl *IFace = nullptr;
  switch (Msg->getReceiverKind()) {
  case ObjCMessageExpr::Class:
    if (const ObjCObjectType *ObjType =
            Msg->getClassReceiver()->getAs<ObjCObjectType>())
      IFace = ObjType->getInterface();
    break;

  case ObjCMessageExpr::Instance: {
    QualType T = Msg->getInstanceReceiver()->getType();
    if (const ObjCObjectPointerType *Ptr = T->getAs<ObjCObjectPointerType>())
      IFace = Ptr->getInterfaceDecl();
    break;
  }

  case ObjCMessageExpr::SuperInstance:
  case ObjCMessageExpr::SuperClass:
    break;
  }

  if (!IFace)
    return nullptr;

  ObjCInterfaceDecl *Super = IFace->getSuperClass();
  if (Method->isInstanceMethod())
    return llvm::StringSwitch<ObjCInterfaceDecl *>(Id->getName())
        .Case("retain", IFace)
        .Case("strong", IFace)
        .Case("autorelease", IFace)
        .Case("copy", IFace)
        .Case("copyWithZone", IFace)
        .Case("mutableCopy", IFace)
        .Case("mutableCopyWithZone", IFace)
        .Case("awakeFromCoder", IFace)
        .Case("replacementObjectFromCoder", IFace)
        .Case("class", IFace)
        .Case("classForCoder", IFace)
        .Case("superclass", Super)
        .Default(nullptr);

  return llvm::StringSwitch<ObjCInterfaceDecl *>(Id->getName())
      .Case("new", IFace)
      .Case("alloc", IFace)
      .Case("allocWithZone", IFace)
      .Case("class", IFace)
      .Case("superclass", Super)
      .Default(nullptr);
}

// Add a special completion for a message send to "super", which fills in the
// most likely case of forwarding all of our arguments to the superclass
// function.
///
/// \param S The semantic analysis object.
///
/// \param NeedSuperKeyword Whether we need to prefix this completion with
/// the "super" keyword. Otherwise, we just need to provide the arguments.
///
/// \param SelIdents The identifiers in the selector that have already been
/// provided as arguments for a send to "super".
///
/// \param Results The set of results to augment.
///
/// \returns the Objective-C method declaration that would be invoked by
/// this "super" completion. If NULL, no completion was added.
static ObjCMethodDecl *
AddSuperSendCompletion(Sema &S, bool NeedSuperKeyword,
                       ArrayRef<IdentifierInfo *> SelIdents,
                       ResultBuilder &Results) {
  ObjCMethodDecl *CurMethod = S.getCurMethodDecl();
  if (!CurMethod)
    return nullptr;

  ObjCInterfaceDecl *Class = CurMethod->getClassInterface();
  if (!Class)
    return nullptr;

  // Try to find a superclass method with the same selector.
  ObjCMethodDecl *SuperMethod = nullptr;
  while ((Class = Class->getSuperClass()) && !SuperMethod) {
    // Check in the class
    SuperMethod = Class->getMethod(CurMethod->getSelector(),
                                   CurMethod->isInstanceMethod());

    // Check in categories or class extensions.
    if (!SuperMethod) {
      for (const auto *Cat : Class->known_categories()) {
        if ((SuperMethod = Cat->getMethod(CurMethod->getSelector(),
                                          CurMethod->isInstanceMethod())))
          break;
      }
    }
  }

  if (!SuperMethod)
    return nullptr;

  // Check whether the superclass method has the same signature.
  if (CurMethod->param_size() != SuperMethod->param_size() ||
      CurMethod->isVariadic() != SuperMethod->isVariadic())
    return nullptr;

  for (ObjCMethodDecl::param_iterator CurP = CurMethod->param_begin(),
                                      CurPEnd = CurMethod->param_end(),
                                      SuperP = SuperMethod->param_begin();
       CurP != CurPEnd; ++CurP, ++SuperP) {
    // Make sure the parameter types are compatible.
    if (!S.Context.hasSameUnqualifiedType((*CurP)->getType(),
                                          (*SuperP)->getType()))
      return nullptr;

    // Make sure we have a parameter name to forward!
    if (!(*CurP)->getIdentifier())
      return nullptr;
  }

  // We have a superclass method. Now, form the send-to-super completion.
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());

  // Give this completion a return type.
  AddResultTypeChunk(S.Context, getCompletionPrintingPolicy(S), SuperMethod,
                     Results.getCompletionContext().getBaseType(), Builder);

  // If we need the "super" keyword, add it (plus some spacing).
  if (NeedSuperKeyword) {
    Builder.AddTypedTextChunk("super");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  }

  Selector Sel = CurMethod->getSelector();
  if (Sel.isUnarySelector()) {
    if (NeedSuperKeyword)
      Builder.AddTextChunk(
          Builder.getAllocator().CopyString(Sel.getNameForSlot(0)));
    else
      Builder.AddTypedTextChunk(
          Builder.getAllocator().CopyString(Sel.getNameForSlot(0)));
  } else {
    ObjCMethodDecl::param_iterator CurP = CurMethod->param_begin();
    for (unsigned I = 0, N = Sel.getNumArgs(); I != N; ++I, ++CurP) {
      if (I > SelIdents.size())
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);

      if (I < SelIdents.size())
        Builder.AddInformativeChunk(
            Builder.getAllocator().CopyString(Sel.getNameForSlot(I) + ":"));
      else if (NeedSuperKeyword || I > SelIdents.size()) {
        Builder.AddTextChunk(
            Builder.getAllocator().CopyString(Sel.getNameForSlot(I) + ":"));
        Builder.AddPlaceholderChunk(Builder.getAllocator().CopyString(
            (*CurP)->getIdentifier()->getName()));
      } else {
        Builder.AddTypedTextChunk(
            Builder.getAllocator().CopyString(Sel.getNameForSlot(I) + ":"));
        Builder.AddPlaceholderChunk(Builder.getAllocator().CopyString(
            (*CurP)->getIdentifier()->getName()));
      }
    }
  }

  Results.AddResult(CodeCompletionResult(Builder.TakeString(), SuperMethod,
                                         CCP_SuperCompletion));
  return SuperMethod;
}

void Sema::CodeCompleteObjCMessageReceiver(Scope *S) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(
      *this, CodeCompleter->getAllocator(),
      CodeCompleter->getCodeCompletionTUInfo(),
      CodeCompletionContext::CCC_ObjCMessageReceiver,
      getLangOpts().CPlusPlus11
          ? &ResultBuilder::IsObjCMessageReceiverOrLambdaCapture
          : &ResultBuilder::IsObjCMessageReceiver);

  CodeCompletionDeclConsumer Consumer(Results, CurContext);
  Results.EnterNewScope();
  LookupVisibleDecls(S, LookupOrdinaryName, Consumer,
                     CodeCompleter->includeGlobals(),
                     CodeCompleter->loadExternal());

  // If we are in an Objective-C method inside a class that has a superclass,
  // add "super" as an option.
  if (ObjCMethodDecl *Method = getCurMethodDecl())
    if (ObjCInterfaceDecl *Iface = Method->getClassInterface())
      if (Iface->getSuperClass()) {
        Results.AddResult(Result("super"));

        AddSuperSendCompletion(*this, /*NeedSuperKeyword=*/true, None, Results);
      }

  if (getLangOpts().CPlusPlus11)
    addThisCompletion(*this, Results);

  Results.ExitScope();

  if (CodeCompleter->includeMacros())
    AddMacroResults(PP, Results, CodeCompleter->loadExternal(), false);
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCSuperMessage(Scope *S, SourceLocation SuperLoc,
                                        ArrayRef<IdentifierInfo *> SelIdents,
                                        bool AtArgumentExpression) {
  ObjCInterfaceDecl *CDecl = nullptr;
  if (ObjCMethodDecl *CurMethod = getCurMethodDecl()) {
    // Figure out which interface we're in.
    CDecl = CurMethod->getClassInterface();
    if (!CDecl)
      return;

    // Find the superclass of this class.
    CDecl = CDecl->getSuperClass();
    if (!CDecl)
      return;

    if (CurMethod->isInstanceMethod()) {
      // We are inside an instance method, which means that the message
      // send [super ...] is actually calling an instance method on the
      // current object.
      return CodeCompleteObjCInstanceMessage(S, nullptr, SelIdents,
                                             AtArgumentExpression, CDecl);
    }

    // Fall through to send to the superclass in CDecl.
  } else {
    // "super" may be the name of a type or variable. Figure out which
    // it is.
    IdentifierInfo *Super = getSuperIdentifier();
    NamedDecl *ND = LookupSingleName(S, Super, SuperLoc, LookupOrdinaryName);
    if ((CDecl = dyn_cast_or_null<ObjCInterfaceDecl>(ND))) {
      // "super" names an interface. Use it.
    } else if (TypeDecl *TD = dyn_cast_or_null<TypeDecl>(ND)) {
      if (const ObjCObjectType *Iface =
              Context.getTypeDeclType(TD)->getAs<ObjCObjectType>())
        CDecl = Iface->getInterface();
    } else if (ND && isa<UnresolvedUsingTypenameDecl>(ND)) {
      // "super" names an unresolved type; we can't be more specific.
    } else {
      // Assume that "super" names some kind of value and parse that way.
      CXXScopeSpec SS;
      SourceLocation TemplateKWLoc;
      UnqualifiedId id;
      id.setIdentifier(Super, SuperLoc);
      ExprResult SuperExpr = ActOnIdExpression(S, SS, TemplateKWLoc, id,
                                               /*HasTrailingLParen=*/false,
                                               /*IsAddressOfOperand=*/false);
      return CodeCompleteObjCInstanceMessage(S, (Expr *)SuperExpr.get(),
                                             SelIdents, AtArgumentExpression);
    }

    // Fall through
  }

  ParsedType Receiver;
  if (CDecl)
    Receiver = ParsedType::make(Context.getObjCInterfaceType(CDecl));
  return CodeCompleteObjCClassMessage(S, Receiver, SelIdents,
                                      AtArgumentExpression,
                                      /*IsSuper=*/true);
}

/// Given a set of code-completion results for the argument of a message
/// send, determine the preferred type (if any) for that argument expression.
static QualType getPreferredArgumentTypeForMessageSend(ResultBuilder &Results,
                                                       unsigned NumSelIdents) {
  typedef CodeCompletionResult Result;
  ASTContext &Context = Results.getSema().Context;

  QualType PreferredType;
  unsigned BestPriority = CCP_Unlikely * 2;
  Result *ResultsData = Results.data();
  for (unsigned I = 0, N = Results.size(); I != N; ++I) {
    Result &R = ResultsData[I];
    if (R.Kind == Result::RK_Declaration &&
        isa<ObjCMethodDecl>(R.Declaration)) {
      if (R.Priority <= BestPriority) {
        const ObjCMethodDecl *Method = cast<ObjCMethodDecl>(R.Declaration);
        if (NumSelIdents <= Method->param_size()) {
          QualType MyPreferredType =
              Method->parameters()[NumSelIdents - 1]->getType();
          if (R.Priority < BestPriority || PreferredType.isNull()) {
            BestPriority = R.Priority;
            PreferredType = MyPreferredType;
          } else if (!Context.hasSameUnqualifiedType(PreferredType,
                                                     MyPreferredType)) {
            PreferredType = QualType();
          }
        }
      }
    }
  }

  return PreferredType;
}

static void AddClassMessageCompletions(Sema &SemaRef, Scope *S,
                                       ParsedType Receiver,
                                       ArrayRef<IdentifierInfo *> SelIdents,
                                       bool AtArgumentExpression, bool IsSuper,
                                       ResultBuilder &Results) {
  typedef CodeCompletionResult Result;
  ObjCInterfaceDecl *CDecl = nullptr;

  // If the given name refers to an interface type, retrieve the
  // corresponding declaration.
  if (Receiver) {
    QualType T = SemaRef.GetTypeFromParser(Receiver, nullptr);
    if (!T.isNull())
      if (const ObjCObjectType *Interface = T->getAs<ObjCObjectType>())
        CDecl = Interface->getInterface();
  }

  // Add all of the factory methods in this Objective-C class, its protocols,
  // superclasses, categories, implementation, etc.
  Results.EnterNewScope();

  // If this is a send-to-super, try to add the special "super" send
  // completion.
  if (IsSuper) {
    if (ObjCMethodDecl *SuperMethod =
            AddSuperSendCompletion(SemaRef, false, SelIdents, Results))
      Results.Ignore(SuperMethod);
  }

  // If we're inside an Objective-C method definition, prefer its selector to
  // others.
  if (ObjCMethodDecl *CurMethod = SemaRef.getCurMethodDecl())
    Results.setPreferredSelector(CurMethod->getSelector());

  VisitedSelectorSet Selectors;
  if (CDecl)
    AddObjCMethods(CDecl, false, MK_Any, SelIdents, SemaRef.CurContext,
                   Selectors, AtArgumentExpression, Results);
  else {
    // We're messaging "id" as a type; provide all class/factory methods.

    // If we have an external source, load the entire class method
    // pool from the AST file.
    if (SemaRef.getExternalSource()) {
      for (uint32_t I = 0,
                    N = SemaRef.getExternalSource()->GetNumExternalSelectors();
           I != N; ++I) {
        Selector Sel = SemaRef.getExternalSource()->GetExternalSelector(I);
        if (Sel.isNull() || SemaRef.MethodPool.count(Sel))
          continue;

        SemaRef.ReadMethodPool(Sel);
      }
    }

    for (Sema::GlobalMethodPool::iterator M = SemaRef.MethodPool.begin(),
                                          MEnd = SemaRef.MethodPool.end();
         M != MEnd; ++M) {
      for (ObjCMethodList *MethList = &M->second.second;
           MethList && MethList->getMethod(); MethList = MethList->getNext()) {
        if (!isAcceptableObjCMethod(MethList->getMethod(), MK_Any, SelIdents))
          continue;

        Result R(MethList->getMethod(),
                 Results.getBasePriority(MethList->getMethod()), nullptr);
        R.StartParameter = SelIdents.size();
        R.AllParametersAreInformative = false;
        Results.MaybeAddResult(R, SemaRef.CurContext);
      }
    }
  }

  Results.ExitScope();
}

void Sema::CodeCompleteObjCClassMessage(Scope *S, ParsedType Receiver,
                                        ArrayRef<IdentifierInfo *> SelIdents,
                                        bool AtArgumentExpression,
                                        bool IsSuper) {

  QualType T = this->GetTypeFromParser(Receiver);

  ResultBuilder Results(
      *this, CodeCompleter->getAllocator(),
      CodeCompleter->getCodeCompletionTUInfo(),
      CodeCompletionContext(CodeCompletionContext::CCC_ObjCClassMessage, T,
                            SelIdents));

  AddClassMessageCompletions(*this, S, Receiver, SelIdents,
                             AtArgumentExpression, IsSuper, Results);

  // If we're actually at the argument expression (rather than prior to the
  // selector), we're actually performing code completion for an expression.
  // Determine whether we have a single, best method. If so, we can
  // code-complete the expression using the corresponding parameter type as
  // our preferred type, improving completion results.
  if (AtArgumentExpression) {
    QualType PreferredType =
        getPreferredArgumentTypeForMessageSend(Results, SelIdents.size());
    if (PreferredType.isNull())
      CodeCompleteOrdinaryName(S, PCC_Expression);
    else
      CodeCompleteExpression(S, PreferredType);
    return;
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCInstanceMessage(Scope *S, Expr *Receiver,
                                           ArrayRef<IdentifierInfo *> SelIdents,
                                           bool AtArgumentExpression,
                                           ObjCInterfaceDecl *Super) {
  typedef CodeCompletionResult Result;

  Expr *RecExpr = static_cast<Expr *>(Receiver);

  // If necessary, apply function/array conversion to the receiver.
  // C99 6.7.5.3p[7,8].
  if (RecExpr) {
    ExprResult Conv = DefaultFunctionArrayLvalueConversion(RecExpr);
    if (Conv.isInvalid()) // conversion failed. bail.
      return;
    RecExpr = Conv.get();
  }
  QualType ReceiverType = RecExpr
                              ? RecExpr->getType()
                              : Super ? Context.getObjCObjectPointerType(
                                            Context.getObjCInterfaceType(Super))
                                      : Context.getObjCIdType();

  // If we're messaging an expression with type "id" or "Class", check
  // whether we know something special about the receiver that allows
  // us to assume a more-specific receiver type.
  if (ReceiverType->isObjCIdType() || ReceiverType->isObjCClassType()) {
    if (ObjCInterfaceDecl *IFace = GetAssumedMessageSendExprType(RecExpr)) {
      if (ReceiverType->isObjCClassType())
        return CodeCompleteObjCClassMessage(
            S, ParsedType::make(Context.getObjCInterfaceType(IFace)), SelIdents,
            AtArgumentExpression, Super);

      ReceiverType =
          Context.getObjCObjectPointerType(Context.getObjCInterfaceType(IFace));
    }
  } else if (RecExpr && getLangOpts().CPlusPlus) {
    ExprResult Conv = PerformContextuallyConvertToObjCPointer(RecExpr);
    if (Conv.isUsable()) {
      RecExpr = Conv.get();
      ReceiverType = RecExpr->getType();
    }
  }

  // Build the set of methods we can see.
  ResultBuilder Results(
      *this, CodeCompleter->getAllocator(),
      CodeCompleter->getCodeCompletionTUInfo(),
      CodeCompletionContext(CodeCompletionContext::CCC_ObjCInstanceMessage,
                            ReceiverType, SelIdents));

  Results.EnterNewScope();

  // If this is a send-to-super, try to add the special "super" send
  // completion.
  if (Super) {
    if (ObjCMethodDecl *SuperMethod =
            AddSuperSendCompletion(*this, false, SelIdents, Results))
      Results.Ignore(SuperMethod);
  }

  // If we're inside an Objective-C method definition, prefer its selector to
  // others.
  if (ObjCMethodDecl *CurMethod = getCurMethodDecl())
    Results.setPreferredSelector(CurMethod->getSelector());

  // Keep track of the selectors we've already added.
  VisitedSelectorSet Selectors;

  // Handle messages to Class. This really isn't a message to an instance
  // method, so we treat it the same way we would treat a message send to a
  // class method.
  if (ReceiverType->isObjCClassType() ||
      ReceiverType->isObjCQualifiedClassType()) {
    if (ObjCMethodDecl *CurMethod = getCurMethodDecl()) {
      if (ObjCInterfaceDecl *ClassDecl = CurMethod->getClassInterface())
        AddObjCMethods(ClassDecl, false, MK_Any, SelIdents, CurContext,
                       Selectors, AtArgumentExpression, Results);
    }
  }
  // Handle messages to a qualified ID ("id<foo>").
  else if (const ObjCObjectPointerType *QualID =
               ReceiverType->getAsObjCQualifiedIdType()) {
    // Search protocols for instance methods.
    for (auto *I : QualID->quals())
      AddObjCMethods(I, true, MK_Any, SelIdents, CurContext, Selectors,
                     AtArgumentExpression, Results);
  }
  // Handle messages to a pointer to interface type.
  else if (const ObjCObjectPointerType *IFacePtr =
               ReceiverType->getAsObjCInterfacePointerType()) {
    // Search the class, its superclasses, etc., for instance methods.
    AddObjCMethods(IFacePtr->getInterfaceDecl(), true, MK_Any, SelIdents,
                   CurContext, Selectors, AtArgumentExpression, Results);

    // Search protocols for instance methods.
    for (auto *I : IFacePtr->quals())
      AddObjCMethods(I, true, MK_Any, SelIdents, CurContext, Selectors,
                     AtArgumentExpression, Results);
  }
  // Handle messages to "id".
  else if (ReceiverType->isObjCIdType()) {
    // We're messaging "id", so provide all instance methods we know
    // about as code-completion results.

    // If we have an external source, load the entire class method
    // pool from the AST file.
    if (ExternalSource) {
      for (uint32_t I = 0, N = ExternalSource->GetNumExternalSelectors();
           I != N; ++I) {
        Selector Sel = ExternalSource->GetExternalSelector(I);
        if (Sel.isNull() || MethodPool.count(Sel))
          continue;

        ReadMethodPool(Sel);
      }
    }

    for (GlobalMethodPool::iterator M = MethodPool.begin(),
                                    MEnd = MethodPool.end();
         M != MEnd; ++M) {
      for (ObjCMethodList *MethList = &M->second.first;
           MethList && MethList->getMethod(); MethList = MethList->getNext()) {
        if (!isAcceptableObjCMethod(MethList->getMethod(), MK_Any, SelIdents))
          continue;

        if (!Selectors.insert(MethList->getMethod()->getSelector()).second)
          continue;

        Result R(MethList->getMethod(),
                 Results.getBasePriority(MethList->getMethod()), nullptr);
        R.StartParameter = SelIdents.size();
        R.AllParametersAreInformative = false;
        Results.MaybeAddResult(R, CurContext);
      }
    }
  }
  Results.ExitScope();

  // If we're actually at the argument expression (rather than prior to the
  // selector), we're actually performing code completion for an expression.
  // Determine whether we have a single, best method. If so, we can
  // code-complete the expression using the corresponding parameter type as
  // our preferred type, improving completion results.
  if (AtArgumentExpression) {
    QualType PreferredType =
        getPreferredArgumentTypeForMessageSend(Results, SelIdents.size());
    if (PreferredType.isNull())
      CodeCompleteOrdinaryName(S, PCC_Expression);
    else
      CodeCompleteExpression(S, PreferredType);
    return;
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCForCollection(Scope *S,
                                         DeclGroupPtrTy IterationVar) {
  CodeCompleteExpressionData Data;
  Data.ObjCCollection = true;

  if (IterationVar.getAsOpaquePtr()) {
    DeclGroupRef DG = IterationVar.get();
    for (DeclGroupRef::iterator I = DG.begin(), End = DG.end(); I != End; ++I) {
      if (*I)
        Data.IgnoreDecls.push_back(*I);
    }
  }

  CodeCompleteExpression(S, Data);
}

void Sema::CodeCompleteObjCSelector(Scope *S,
                                    ArrayRef<IdentifierInfo *> SelIdents) {
  // If we have an external source, load the entire class method
  // pool from the AST file.
  if (ExternalSource) {
    for (uint32_t I = 0, N = ExternalSource->GetNumExternalSelectors(); I != N;
         ++I) {
      Selector Sel = ExternalSource->GetExternalSelector(I);
      if (Sel.isNull() || MethodPool.count(Sel))
        continue;

      ReadMethodPool(Sel);
    }
  }

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_SelectorName);
  Results.EnterNewScope();
  for (GlobalMethodPool::iterator M = MethodPool.begin(),
                                  MEnd = MethodPool.end();
       M != MEnd; ++M) {

    Selector Sel = M->first;
    if (!isAcceptableObjCSelector(Sel, MK_Any, SelIdents))
      continue;

    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo());
    if (Sel.isUnarySelector()) {
      Builder.AddTypedTextChunk(
          Builder.getAllocator().CopyString(Sel.getNameForSlot(0)));
      Results.AddResult(Builder.TakeString());
      continue;
    }

    std::string Accumulator;
    for (unsigned I = 0, N = Sel.getNumArgs(); I != N; ++I) {
      if (I == SelIdents.size()) {
        if (!Accumulator.empty()) {
          Builder.AddInformativeChunk(
              Builder.getAllocator().CopyString(Accumulator));
          Accumulator.clear();
        }
      }

      Accumulator += Sel.getNameForSlot(I);
      Accumulator += ':';
    }
    Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(Accumulator));
    Results.AddResult(Builder.TakeString());
  }
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

/// Add all of the protocol declarations that we find in the given
/// (translation unit) context.
static void AddProtocolResults(DeclContext *Ctx, DeclContext *CurContext,
                               bool OnlyForwardDeclarations,
                               ResultBuilder &Results) {
  typedef CodeCompletionResult Result;

  for (const auto *D : Ctx->decls()) {
    // Record any protocols we find.
    if (const auto *Proto = dyn_cast<ObjCProtocolDecl>(D))
      if (!OnlyForwardDeclarations || !Proto->hasDefinition())
        Results.AddResult(
            Result(Proto, Results.getBasePriority(Proto), nullptr), CurContext,
            nullptr, false);
  }
}

void Sema::CodeCompleteObjCProtocolReferences(
    ArrayRef<IdentifierLocPair> Protocols) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_ObjCProtocolName);

  if (CodeCompleter->includeGlobals()) {
    Results.EnterNewScope();

    // Tell the result set to ignore all of the protocols we have
    // already seen.
    // FIXME: This doesn't work when caching code-completion results.
    for (const IdentifierLocPair &Pair : Protocols)
      if (ObjCProtocolDecl *Protocol = LookupProtocol(Pair.first, Pair.second))
        Results.Ignore(Protocol);

    // Add all protocols.
    AddProtocolResults(Context.getTranslationUnitDecl(), CurContext, false,
                       Results);

    Results.ExitScope();
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCProtocolDecl(Scope *) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_ObjCProtocolName);

  if (CodeCompleter->includeGlobals()) {
    Results.EnterNewScope();

    // Add all protocols.
    AddProtocolResults(Context.getTranslationUnitDecl(), CurContext, true,
                       Results);

    Results.ExitScope();
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

/// Add all of the Objective-C interface declarations that we find in
/// the given (translation unit) context.
static void AddInterfaceResults(DeclContext *Ctx, DeclContext *CurContext,
                                bool OnlyForwardDeclarations,
                                bool OnlyUnimplemented,
                                ResultBuilder &Results) {
  typedef CodeCompletionResult Result;

  for (const auto *D : Ctx->decls()) {
    // Record any interfaces we find.
    if (const auto *Class = dyn_cast<ObjCInterfaceDecl>(D))
      if ((!OnlyForwardDeclarations || !Class->hasDefinition()) &&
          (!OnlyUnimplemented || !Class->getImplementation()))
        Results.AddResult(
            Result(Class, Results.getBasePriority(Class), nullptr), CurContext,
            nullptr, false);
  }
}

void Sema::CodeCompleteObjCInterfaceDecl(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_ObjCInterfaceName);
  Results.EnterNewScope();

  if (CodeCompleter->includeGlobals()) {
    // Add all classes.
    AddInterfaceResults(Context.getTranslationUnitDecl(), CurContext, false,
                        false, Results);
  }

  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCSuperclass(Scope *S, IdentifierInfo *ClassName,
                                      SourceLocation ClassNameLoc) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_ObjCInterfaceName);
  Results.EnterNewScope();

  // Make sure that we ignore the class we're currently defining.
  NamedDecl *CurClass =
      LookupSingleName(TUScope, ClassName, ClassNameLoc, LookupOrdinaryName);
  if (CurClass && isa<ObjCInterfaceDecl>(CurClass))
    Results.Ignore(CurClass);

  if (CodeCompleter->includeGlobals()) {
    // Add all classes.
    AddInterfaceResults(Context.getTranslationUnitDecl(), CurContext, false,
                        false, Results);
  }

  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCImplementationDecl(Scope *S) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_ObjCImplementation);
  Results.EnterNewScope();

  if (CodeCompleter->includeGlobals()) {
    // Add all unimplemented classes.
    AddInterfaceResults(Context.getTranslationUnitDecl(), CurContext, false,
                        true, Results);
  }

  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCInterfaceCategory(Scope *S,
                                             IdentifierInfo *ClassName,
                                             SourceLocation ClassNameLoc) {
  typedef CodeCompletionResult Result;

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_ObjCCategoryName);

  // Ignore any categories we find that have already been implemented by this
  // interface.
  llvm::SmallPtrSet<IdentifierInfo *, 16> CategoryNames;
  NamedDecl *CurClass =
      LookupSingleName(TUScope, ClassName, ClassNameLoc, LookupOrdinaryName);
  if (ObjCInterfaceDecl *Class =
          dyn_cast_or_null<ObjCInterfaceDecl>(CurClass)) {
    for (const auto *Cat : Class->visible_categories())
      CategoryNames.insert(Cat->getIdentifier());
  }

  // Add all of the categories we know about.
  Results.EnterNewScope();
  TranslationUnitDecl *TU = Context.getTranslationUnitDecl();
  for (const auto *D : TU->decls())
    if (const auto *Category = dyn_cast<ObjCCategoryDecl>(D))
      if (CategoryNames.insert(Category->getIdentifier()).second)
        Results.AddResult(
            Result(Category, Results.getBasePriority(Category), nullptr),
            CurContext, nullptr, false);
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCImplementationCategory(Scope *S,
                                                  IdentifierInfo *ClassName,
                                                  SourceLocation ClassNameLoc) {
  typedef CodeCompletionResult Result;

  // Find the corresponding interface. If we couldn't find the interface, the
  // program itself is ill-formed. However, we'll try to be helpful still by
  // providing the list of all of the categories we know about.
  NamedDecl *CurClass =
      LookupSingleName(TUScope, ClassName, ClassNameLoc, LookupOrdinaryName);
  ObjCInterfaceDecl *Class = dyn_cast_or_null<ObjCInterfaceDecl>(CurClass);
  if (!Class)
    return CodeCompleteObjCInterfaceCategory(S, ClassName, ClassNameLoc);

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_ObjCCategoryName);

  // Add all of the categories that have have corresponding interface
  // declarations in this class and any of its superclasses, except for
  // already-implemented categories in the class itself.
  llvm::SmallPtrSet<IdentifierInfo *, 16> CategoryNames;
  Results.EnterNewScope();
  bool IgnoreImplemented = true;
  while (Class) {
    for (const auto *Cat : Class->visible_categories()) {
      if ((!IgnoreImplemented || !Cat->getImplementation()) &&
          CategoryNames.insert(Cat->getIdentifier()).second)
        Results.AddResult(Result(Cat, Results.getBasePriority(Cat), nullptr),
                          CurContext, nullptr, false);
    }

    Class = Class->getSuperClass();
    IgnoreImplemented = false;
  }
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCPropertyDefinition(Scope *S) {
  CodeCompletionContext CCContext(CodeCompletionContext::CCC_Other);
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(), CCContext);

  // Figure out where this @synthesize lives.
  ObjCContainerDecl *Container =
      dyn_cast_or_null<ObjCContainerDecl>(CurContext);
  if (!Container || (!isa<ObjCImplementationDecl>(Container) &&
                     !isa<ObjCCategoryImplDecl>(Container)))
    return;

  // Ignore any properties that have already been implemented.
  Container = getContainerDef(Container);
  for (const auto *D : Container->decls())
    if (const auto *PropertyImpl = dyn_cast<ObjCPropertyImplDecl>(D))
      Results.Ignore(PropertyImpl->getPropertyDecl());

  // Add any properties that we find.
  AddedPropertiesSet AddedProperties;
  Results.EnterNewScope();
  if (ObjCImplementationDecl *ClassImpl =
          dyn_cast<ObjCImplementationDecl>(Container))
    AddObjCProperties(CCContext, ClassImpl->getClassInterface(), false,
                      /*AllowNullaryMethods=*/false, CurContext,
                      AddedProperties, Results);
  else
    AddObjCProperties(CCContext,
                      cast<ObjCCategoryImplDecl>(Container)->getCategoryDecl(),
                      false, /*AllowNullaryMethods=*/false, CurContext,
                      AddedProperties, Results);
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCPropertySynthesizeIvar(
    Scope *S, IdentifierInfo *PropertyName) {
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);

  // Figure out where this @synthesize lives.
  ObjCContainerDecl *Container =
      dyn_cast_or_null<ObjCContainerDecl>(CurContext);
  if (!Container || (!isa<ObjCImplementationDecl>(Container) &&
                     !isa<ObjCCategoryImplDecl>(Container)))
    return;

  // Figure out which interface we're looking into.
  ObjCInterfaceDecl *Class = nullptr;
  if (ObjCImplementationDecl *ClassImpl =
          dyn_cast<ObjCImplementationDecl>(Container))
    Class = ClassImpl->getClassInterface();
  else
    Class = cast<ObjCCategoryImplDecl>(Container)
                ->getCategoryDecl()
                ->getClassInterface();

  // Determine the type of the property we're synthesizing.
  QualType PropertyType = Context.getObjCIdType();
  if (Class) {
    if (ObjCPropertyDecl *Property = Class->FindPropertyDeclaration(
            PropertyName, ObjCPropertyQueryKind::OBJC_PR_query_instance)) {
      PropertyType =
          Property->getType().getNonReferenceType().getUnqualifiedType();

      // Give preference to ivars
      Results.setPreferredType(PropertyType);
    }
  }

  // Add all of the instance variables in this class and its superclasses.
  Results.EnterNewScope();
  bool SawSimilarlyNamedIvar = false;
  std::string NameWithPrefix;
  NameWithPrefix += '_';
  NameWithPrefix += PropertyName->getName();
  std::string NameWithSuffix = PropertyName->getName().str();
  NameWithSuffix += '_';
  for (; Class; Class = Class->getSuperClass()) {
    for (ObjCIvarDecl *Ivar = Class->all_declared_ivar_begin(); Ivar;
         Ivar = Ivar->getNextIvar()) {
      Results.AddResult(Result(Ivar, Results.getBasePriority(Ivar), nullptr),
                        CurContext, nullptr, false);

      // Determine whether we've seen an ivar with a name similar to the
      // property.
      if ((PropertyName == Ivar->getIdentifier() ||
           NameWithPrefix == Ivar->getName() ||
           NameWithSuffix == Ivar->getName())) {
        SawSimilarlyNamedIvar = true;

        // Reduce the priority of this result by one, to give it a slight
        // advantage over other results whose names don't match so closely.
        if (Results.size() &&
            Results.data()[Results.size() - 1].Kind ==
                CodeCompletionResult::RK_Declaration &&
            Results.data()[Results.size() - 1].Declaration == Ivar)
          Results.data()[Results.size() - 1].Priority--;
      }
    }
  }

  if (!SawSimilarlyNamedIvar) {
    // Create ivar result _propName, that the user can use to synthesize
    // an ivar of the appropriate type.
    unsigned Priority = CCP_MemberDeclaration + 1;
    typedef CodeCompletionResult Result;
    CodeCompletionAllocator &Allocator = Results.getAllocator();
    CodeCompletionBuilder Builder(Allocator, Results.getCodeCompletionTUInfo(),
                                  Priority, CXAvailability_Available);

    PrintingPolicy Policy = getCompletionPrintingPolicy(*this);
    Builder.AddResultTypeChunk(
        GetCompletionTypeString(PropertyType, Context, Policy, Allocator));
    Builder.AddTypedTextChunk(Allocator.CopyString(NameWithPrefix));
    Results.AddResult(
        Result(Builder.TakeString(), Priority, CXCursor_ObjCIvarDecl));
  }

  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

// Mapping from selectors to the methods that implement that selector, along
// with the "in original class" flag.
typedef llvm::DenseMap<Selector,
                       llvm::PointerIntPair<ObjCMethodDecl *, 1, bool>>
    KnownMethodsMap;

/// Find all of the methods that reside in the given container
/// (and its superclasses, protocols, etc.) that meet the given
/// criteria. Insert those methods into the map of known methods,
/// indexed by selector so they can be easily found.
static void FindImplementableMethods(ASTContext &Context,
                                     ObjCContainerDecl *Container,
                                     Optional<bool> WantInstanceMethods,
                                     QualType ReturnType,
                                     KnownMethodsMap &KnownMethods,
                                     bool InOriginalClass = true) {
  if (ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>(Container)) {
    // Make sure we have a definition; that's what we'll walk.
    if (!IFace->hasDefinition())
      return;

    IFace = IFace->getDefinition();
    Container = IFace;

    const ObjCList<ObjCProtocolDecl> &Protocols =
        IFace->getReferencedProtocols();
    for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                              E = Protocols.end();
         I != E; ++I)
      FindImplementableMethods(Context, *I, WantInstanceMethods, ReturnType,
                               KnownMethods, InOriginalClass);

    // Add methods from any class extensions and categories.
    for (auto *Cat : IFace->visible_categories()) {
      FindImplementableMethods(Context, Cat, WantInstanceMethods, ReturnType,
                               KnownMethods, false);
    }

    // Visit the superclass.
    if (IFace->getSuperClass())
      FindImplementableMethods(Context, IFace->getSuperClass(),
                               WantInstanceMethods, ReturnType, KnownMethods,
                               false);
  }

  if (ObjCCategoryDecl *Category = dyn_cast<ObjCCategoryDecl>(Container)) {
    // Recurse into protocols.
    const ObjCList<ObjCProtocolDecl> &Protocols =
        Category->getReferencedProtocols();
    for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                              E = Protocols.end();
         I != E; ++I)
      FindImplementableMethods(Context, *I, WantInstanceMethods, ReturnType,
                               KnownMethods, InOriginalClass);

    // If this category is the original class, jump to the interface.
    if (InOriginalClass && Category->getClassInterface())
      FindImplementableMethods(Context, Category->getClassInterface(),
                               WantInstanceMethods, ReturnType, KnownMethods,
                               false);
  }

  if (ObjCProtocolDecl *Protocol = dyn_cast<ObjCProtocolDecl>(Container)) {
    // Make sure we have a definition; that's what we'll walk.
    if (!Protocol->hasDefinition())
      return;
    Protocol = Protocol->getDefinition();
    Container = Protocol;

    // Recurse into protocols.
    const ObjCList<ObjCProtocolDecl> &Protocols =
        Protocol->getReferencedProtocols();
    for (ObjCList<ObjCProtocolDecl>::iterator I = Protocols.begin(),
                                              E = Protocols.end();
         I != E; ++I)
      FindImplementableMethods(Context, *I, WantInstanceMethods, ReturnType,
                               KnownMethods, false);
  }

  // Add methods in this container. This operation occurs last because
  // we want the methods from this container to override any methods
  // we've previously seen with the same selector.
  for (auto *M : Container->methods()) {
    if (!WantInstanceMethods || M->isInstanceMethod() == *WantInstanceMethods) {
      if (!ReturnType.isNull() &&
          !Context.hasSameUnqualifiedType(ReturnType, M->getReturnType()))
        continue;

      KnownMethods[M->getSelector()] =
          KnownMethodsMap::mapped_type(M, InOriginalClass);
    }
  }
}

/// Add the parenthesized return or parameter type chunk to a code
/// completion string.
static void AddObjCPassingTypeChunk(QualType Type, unsigned ObjCDeclQuals,
                                    ASTContext &Context,
                                    const PrintingPolicy &Policy,
                                    CodeCompletionBuilder &Builder) {
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  std::string Quals = formatObjCParamQualifiers(ObjCDeclQuals, Type);
  if (!Quals.empty())
    Builder.AddTextChunk(Builder.getAllocator().CopyString(Quals));
  Builder.AddTextChunk(
      GetCompletionTypeString(Type, Context, Policy, Builder.getAllocator()));
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
}

/// Determine whether the given class is or inherits from a class by
/// the given name.
static bool InheritsFromClassNamed(ObjCInterfaceDecl *Class, StringRef Name) {
  if (!Class)
    return false;

  if (Class->getIdentifier() && Class->getIdentifier()->getName() == Name)
    return true;

  return InheritsFromClassNamed(Class->getSuperClass(), Name);
}

/// Add code completions for Objective-C Key-Value Coding (KVC) and
/// Key-Value Observing (KVO).
static void AddObjCKeyValueCompletions(ObjCPropertyDecl *Property,
                                       bool IsInstanceMethod,
                                       QualType ReturnType, ASTContext &Context,
                                       VisitedSelectorSet &KnownSelectors,
                                       ResultBuilder &Results) {
  IdentifierInfo *PropName = Property->getIdentifier();
  if (!PropName || PropName->getLength() == 0)
    return;

  PrintingPolicy Policy = getCompletionPrintingPolicy(Results.getSema());

  // Builder that will create each code completion.
  typedef CodeCompletionResult Result;
  CodeCompletionAllocator &Allocator = Results.getAllocator();
  CodeCompletionBuilder Builder(Allocator, Results.getCodeCompletionTUInfo());

  // The selector table.
  SelectorTable &Selectors = Context.Selectors;

  // The property name, copied into the code completion allocation region
  // on demand.
  struct KeyHolder {
    CodeCompletionAllocator &Allocator;
    StringRef Key;
    const char *CopiedKey;

    KeyHolder(CodeCompletionAllocator &Allocator, StringRef Key)
        : Allocator(Allocator), Key(Key), CopiedKey(nullptr) {}

    operator const char *() {
      if (CopiedKey)
        return CopiedKey;

      return CopiedKey = Allocator.CopyString(Key);
    }
  } Key(Allocator, PropName->getName());

  // The uppercased name of the property name.
  std::string UpperKey = PropName->getName();
  if (!UpperKey.empty())
    UpperKey[0] = toUppercase(UpperKey[0]);

  bool ReturnTypeMatchesProperty =
      ReturnType.isNull() ||
      Context.hasSameUnqualifiedType(ReturnType.getNonReferenceType(),
                                     Property->getType());
  bool ReturnTypeMatchesVoid = ReturnType.isNull() || ReturnType->isVoidType();

  // Add the normal accessor -(type)key.
  if (IsInstanceMethod &&
      KnownSelectors.insert(Selectors.getNullarySelector(PropName)).second &&
      ReturnTypeMatchesProperty && !Property->getGetterMethodDecl()) {
    if (ReturnType.isNull())
      AddObjCPassingTypeChunk(Property->getType(), /*Quals=*/0, Context, Policy,
                              Builder);

    Builder.AddTypedTextChunk(Key);
    Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern,
                             CXCursor_ObjCInstanceMethodDecl));
  }

  // If we have an integral or boolean property (or the user has provided
  // an integral or boolean return type), add the accessor -(type)isKey.
  if (IsInstanceMethod &&
      ((!ReturnType.isNull() &&
        (ReturnType->isIntegerType() || ReturnType->isBooleanType())) ||
       (ReturnType.isNull() && (Property->getType()->isIntegerType() ||
                                Property->getType()->isBooleanType())))) {
    std::string SelectorName = (Twine("is") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))
            .second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("BOOL");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorId->getName()));
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Add the normal mutator.
  if (IsInstanceMethod && ReturnTypeMatchesVoid &&
      !Property->getSetterMethodDecl()) {
    std::string SelectorName = (Twine("set") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorId->getName()));
      Builder.AddTypedTextChunk(":");
      AddObjCPassingTypeChunk(Property->getType(), /*Quals=*/0, Context, Policy,
                              Builder);
      Builder.AddTextChunk(Key);
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Indexed and unordered accessors
  unsigned IndexedGetterPriority = CCP_CodePattern;
  unsigned IndexedSetterPriority = CCP_CodePattern;
  unsigned UnorderedGetterPriority = CCP_CodePattern;
  unsigned UnorderedSetterPriority = CCP_CodePattern;
  if (const auto *ObjCPointer =
          Property->getType()->getAs<ObjCObjectPointerType>()) {
    if (ObjCInterfaceDecl *IFace = ObjCPointer->getInterfaceDecl()) {
      // If this interface type is not provably derived from a known
      // collection, penalize the corresponding completions.
      if (!InheritsFromClassNamed(IFace, "NSMutableArray")) {
        IndexedSetterPriority += CCD_ProbablyNotObjCCollection;
        if (!InheritsFromClassNamed(IFace, "NSArray"))
          IndexedGetterPriority += CCD_ProbablyNotObjCCollection;
      }

      if (!InheritsFromClassNamed(IFace, "NSMutableSet")) {
        UnorderedSetterPriority += CCD_ProbablyNotObjCCollection;
        if (!InheritsFromClassNamed(IFace, "NSSet"))
          UnorderedGetterPriority += CCD_ProbablyNotObjCCollection;
      }
    }
  } else {
    IndexedGetterPriority += CCD_ProbablyNotObjCCollection;
    IndexedSetterPriority += CCD_ProbablyNotObjCCollection;
    UnorderedGetterPriority += CCD_ProbablyNotObjCCollection;
    UnorderedSetterPriority += CCD_ProbablyNotObjCCollection;
  }

  // Add -(NSUInteger)countOf<key>
  if (IsInstanceMethod &&
      (ReturnType.isNull() || ReturnType->isIntegerType())) {
    std::string SelectorName = (Twine("countOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))
            .second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSUInteger");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorId->getName()));
      Results.AddResult(
          Result(Builder.TakeString(),
                 std::min(IndexedGetterPriority, UnorderedGetterPriority),
                 CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Indexed getters
  // Add -(id)objectInKeyAtIndex:(NSUInteger)index
  if (IsInstanceMethod &&
      (ReturnType.isNull() || ReturnType->isObjCObjectPointerType())) {
    std::string SelectorName = (Twine("objectIn") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("id");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Results.AddResult(Result(Builder.TakeString(), IndexedGetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Add -(NSArray *)keyAtIndexes:(NSIndexSet *)indexes
  if (IsInstanceMethod &&
      (ReturnType.isNull() ||
       (ReturnType->isObjCObjectPointerType() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl() &&
        ReturnType->getAs<ObjCObjectPointerType>()
                ->getInterfaceDecl()
                ->getName() == "NSArray"))) {
    std::string SelectorName = (Twine(Property->getName()) + "AtIndexes").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSArray *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Results.AddResult(Result(Builder.TakeString(), IndexedGetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Add -(void)getKey:(type **)buffer range:(NSRange)inRange
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("get") + UpperKey).str();
    IdentifierInfo *SelectorIds[2] = {&Context.Idents.get(SelectorName),
                                      &Context.Idents.get("range")};

    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" **");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("buffer");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk("range:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSRange");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("inRange");
      Results.AddResult(Result(Builder.TakeString(), IndexedGetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Mutable indexed accessors

  // - (void)insertObject:(type *)object inKeyAtIndex:(NSUInteger)index
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("in") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorIds[2] = {&Context.Idents.get("insertObject"),
                                      &Context.Idents.get(SelectorName)};

    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk("insertObject:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (void)insertKey:(NSArray *)array atIndexes:(NSIndexSet *)indexes
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("insert") + UpperKey).str();
    IdentifierInfo *SelectorIds[2] = {&Context.Idents.get(SelectorName),
                                      &Context.Idents.get("atIndexes")};

    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSArray *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("array");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk("atIndexes:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // -(void)removeObjectFromKeyAtIndex:(NSUInteger)index
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName =
        (Twine("removeObjectFrom") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // -(void)removeKeyAtIndexes:(NSIndexSet *)indexes
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("remove") + UpperKey + "AtIndexes").str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (void)replaceObjectInKeyAtIndex:(NSUInteger)index withObject:(id)object
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName =
        (Twine("replaceObjectIn") + UpperKey + "AtIndex").str();
    IdentifierInfo *SelectorIds[2] = {&Context.Idents.get(SelectorName),
                                      &Context.Idents.get("withObject")};

    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSUInteger");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("index");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk("withObject:");
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("id");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (void)replaceKeyAtIndexes:(NSIndexSet *)indexes withKey:(NSArray *)array
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName1 =
        (Twine("replace") + UpperKey + "AtIndexes").str();
    std::string SelectorName2 = (Twine("with") + UpperKey).str();
    IdentifierInfo *SelectorIds[2] = {&Context.Idents.get(SelectorName1),
                                      &Context.Idents.get(SelectorName2)};

    if (KnownSelectors.insert(Selectors.getSelector(2, SelectorIds)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName1 + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("NSIndexSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("indexes");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName2 + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSArray *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("array");
      Results.AddResult(Result(Builder.TakeString(), IndexedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Unordered getters
  // - (NSEnumerator *)enumeratorOfKey
  if (IsInstanceMethod &&
      (ReturnType.isNull() ||
       (ReturnType->isObjCObjectPointerType() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl() &&
        ReturnType->getAs<ObjCObjectPointerType>()
                ->getInterfaceDecl()
                ->getName() == "NSEnumerator"))) {
    std::string SelectorName = (Twine("enumeratorOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))
            .second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSEnumerator *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName));
      Results.AddResult(Result(Builder.TakeString(), UnorderedGetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (type *)memberOfKey:(type *)object
  if (IsInstanceMethod &&
      (ReturnType.isNull() || ReturnType->isObjCObjectPointerType())) {
    std::string SelectorName = (Twine("memberOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddPlaceholderChunk("object-type");
        Builder.AddTextChunk(" *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      if (ReturnType.isNull()) {
        Builder.AddPlaceholderChunk("object-type");
        Builder.AddTextChunk(" *");
      } else {
        Builder.AddTextChunk(GetCompletionTypeString(
            ReturnType, Context, Policy, Builder.getAllocator()));
      }
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), UnorderedGetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Mutable unordered accessors
  // - (void)addKeyObject:(type *)object
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName =
        (Twine("add") + UpperKey + Twine("Object")).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (void)addKey:(NSSet *)objects
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("add") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("objects");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (void)removeKeyObject:(type *)object
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName =
        (Twine("remove") + UpperKey + Twine("Object")).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddPlaceholderChunk("object-type");
      Builder.AddTextChunk(" *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("object");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (void)removeKey:(NSSet *)objects
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("remove") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("objects");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // - (void)intersectKey:(NSSet *)objects
  if (IsInstanceMethod && ReturnTypeMatchesVoid) {
    std::string SelectorName = (Twine("intersect") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getUnarySelector(SelectorId)).second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("void");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName + ":"));
      Builder.AddChunk(CodeCompletionString::CK_LeftParen);
      Builder.AddTextChunk("NSSet *");
      Builder.AddChunk(CodeCompletionString::CK_RightParen);
      Builder.AddTextChunk("objects");
      Results.AddResult(Result(Builder.TakeString(), UnorderedSetterPriority,
                               CXCursor_ObjCInstanceMethodDecl));
    }
  }

  // Key-Value Observing
  // + (NSSet *)keyPathsForValuesAffectingKey
  if (!IsInstanceMethod &&
      (ReturnType.isNull() ||
       (ReturnType->isObjCObjectPointerType() &&
        ReturnType->getAs<ObjCObjectPointerType>()->getInterfaceDecl() &&
        ReturnType->getAs<ObjCObjectPointerType>()
                ->getInterfaceDecl()
                ->getName() == "NSSet"))) {
    std::string SelectorName =
        (Twine("keyPathsForValuesAffecting") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))
            .second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("NSSet<NSString *> *");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName));
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern,
                               CXCursor_ObjCClassMethodDecl));
    }
  }

  // + (BOOL)automaticallyNotifiesObserversForKey
  if (!IsInstanceMethod &&
      (ReturnType.isNull() || ReturnType->isIntegerType() ||
       ReturnType->isBooleanType())) {
    std::string SelectorName =
        (Twine("automaticallyNotifiesObserversOf") + UpperKey).str();
    IdentifierInfo *SelectorId = &Context.Idents.get(SelectorName);
    if (KnownSelectors.insert(Selectors.getNullarySelector(SelectorId))
            .second) {
      if (ReturnType.isNull()) {
        Builder.AddChunk(CodeCompletionString::CK_LeftParen);
        Builder.AddTextChunk("BOOL");
        Builder.AddChunk(CodeCompletionString::CK_RightParen);
      }

      Builder.AddTypedTextChunk(Allocator.CopyString(SelectorName));
      Results.AddResult(Result(Builder.TakeString(), CCP_CodePattern,
                               CXCursor_ObjCClassMethodDecl));
    }
  }
}

void Sema::CodeCompleteObjCMethodDecl(Scope *S, Optional<bool> IsInstanceMethod,
                                      ParsedType ReturnTy) {
  // Determine the return type of the method we're declaring, if
  // provided.
  QualType ReturnType = GetTypeFromParser(ReturnTy);
  Decl *IDecl = nullptr;
  if (CurContext->isObjCContainer()) {
    ObjCContainerDecl *OCD = dyn_cast<ObjCContainerDecl>(CurContext);
    IDecl = OCD;
  }
  // Determine where we should start searching for methods.
  ObjCContainerDecl *SearchDecl = nullptr;
  bool IsInImplementation = false;
  if (Decl *D = IDecl) {
    if (ObjCImplementationDecl *Impl = dyn_cast<ObjCImplementationDecl>(D)) {
      SearchDecl = Impl->getClassInterface();
      IsInImplementation = true;
    } else if (ObjCCategoryImplDecl *CatImpl =
                   dyn_cast<ObjCCategoryImplDecl>(D)) {
      SearchDecl = CatImpl->getCategoryDecl();
      IsInImplementation = true;
    } else
      SearchDecl = dyn_cast<ObjCContainerDecl>(D);
  }

  if (!SearchDecl && S) {
    if (DeclContext *DC = S->getEntity())
      SearchDecl = dyn_cast<ObjCContainerDecl>(DC);
  }

  if (!SearchDecl) {
    HandleCodeCompleteResults(this, CodeCompleter,
                              CodeCompletionContext::CCC_Other, nullptr, 0);
    return;
  }

  // Find all of the methods that we could declare/implement here.
  KnownMethodsMap KnownMethods;
  FindImplementableMethods(Context, SearchDecl, IsInstanceMethod, ReturnType,
                           KnownMethods);

  // Add declarations or definitions for each of the known methods.
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  PrintingPolicy Policy = getCompletionPrintingPolicy(*this);
  for (KnownMethodsMap::iterator M = KnownMethods.begin(),
                                 MEnd = KnownMethods.end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = M->second.getPointer();
    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo());

    // Add the '-'/'+' prefix if it wasn't provided yet.
    if (!IsInstanceMethod) {
      Builder.AddTextChunk(Method->isInstanceMethod() ? "-" : "+");
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    }

    // If the result type was not already provided, add it to the
    // pattern as (type).
    if (ReturnType.isNull()) {
      QualType ResTy = Method->getSendResultType().stripObjCKindOfType(Context);
      AttributedType::stripOuterNullability(ResTy);
      AddObjCPassingTypeChunk(ResTy, Method->getObjCDeclQualifier(), Context,
                              Policy, Builder);
    }

    Selector Sel = Method->getSelector();

    // Add the first part of the selector to the pattern.
    Builder.AddTypedTextChunk(
        Builder.getAllocator().CopyString(Sel.getNameForSlot(0)));

    // Add parameters to the pattern.
    unsigned I = 0;
    for (ObjCMethodDecl::param_iterator P = Method->param_begin(),
                                        PEnd = Method->param_end();
         P != PEnd; (void)++P, ++I) {
      // Add the part of the selector name.
      if (I == 0)
        Builder.AddTypedTextChunk(":");
      else if (I < Sel.getNumArgs()) {
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddTypedTextChunk(
            Builder.getAllocator().CopyString(Sel.getNameForSlot(I) + ":"));
      } else
        break;

      // Add the parameter type.
      QualType ParamType;
      if ((*P)->getObjCDeclQualifier() & Decl::OBJC_TQ_CSNullability)
        ParamType = (*P)->getType();
      else
        ParamType = (*P)->getOriginalType();
      ParamType = ParamType.substObjCTypeArgs(
          Context, {}, ObjCSubstitutionContext::Parameter);
      AttributedType::stripOuterNullability(ParamType);
      AddObjCPassingTypeChunk(ParamType, (*P)->getObjCDeclQualifier(), Context,
                              Policy, Builder);

      if (IdentifierInfo *Id = (*P)->getIdentifier())
        Builder.AddTextChunk(Builder.getAllocator().CopyString(Id->getName()));
    }

    if (Method->isVariadic()) {
      if (Method->param_size() > 0)
        Builder.AddChunk(CodeCompletionString::CK_Comma);
      Builder.AddTextChunk("...");
    }

    if (IsInImplementation && Results.includeCodePatterns()) {
      // We will be defining the method here, so add a compound statement.
      Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
      Builder.AddChunk(CodeCompletionString::CK_LeftBrace);
      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      if (!Method->getReturnType()->isVoidType()) {
        // If the result type is not void, add a return clause.
        Builder.AddTextChunk("return");
        Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
        Builder.AddPlaceholderChunk("expression");
        Builder.AddChunk(CodeCompletionString::CK_SemiColon);
      } else
        Builder.AddPlaceholderChunk("statements");

      Builder.AddChunk(CodeCompletionString::CK_VerticalSpace);
      Builder.AddChunk(CodeCompletionString::CK_RightBrace);
    }

    unsigned Priority = CCP_CodePattern;
    auto R = Result(Builder.TakeString(), Method, Priority);
    if (!M->second.getInt())
      setInBaseClass(R);
    Results.AddResult(std::move(R));
  }

  // Add Key-Value-Coding and Key-Value-Observing accessor methods for all of
  // the properties in this class and its categories.
  if (Context.getLangOpts().ObjC) {
    SmallVector<ObjCContainerDecl *, 4> Containers;
    Containers.push_back(SearchDecl);

    VisitedSelectorSet KnownSelectors;
    for (KnownMethodsMap::iterator M = KnownMethods.begin(),
                                   MEnd = KnownMethods.end();
         M != MEnd; ++M)
      KnownSelectors.insert(M->first);

    ObjCInterfaceDecl *IFace = dyn_cast<ObjCInterfaceDecl>(SearchDecl);
    if (!IFace)
      if (ObjCCategoryDecl *Category = dyn_cast<ObjCCategoryDecl>(SearchDecl))
        IFace = Category->getClassInterface();

    if (IFace)
      for (auto *Cat : IFace->visible_categories())
        Containers.push_back(Cat);

    if (IsInstanceMethod) {
      for (unsigned I = 0, N = Containers.size(); I != N; ++I)
        for (auto *P : Containers[I]->instance_properties())
          AddObjCKeyValueCompletions(P, *IsInstanceMethod, ReturnType, Context,
                                     KnownSelectors, Results);
    }
  }

  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteObjCMethodDeclSelector(
    Scope *S, bool IsInstanceMethod, bool AtParameterName, ParsedType ReturnTy,
    ArrayRef<IdentifierInfo *> SelIdents) {
  // If we have an external source, load the entire class method
  // pool from the AST file.
  if (ExternalSource) {
    for (uint32_t I = 0, N = ExternalSource->GetNumExternalSelectors(); I != N;
         ++I) {
      Selector Sel = ExternalSource->GetExternalSelector(I);
      if (Sel.isNull() || MethodPool.count(Sel))
        continue;

      ReadMethodPool(Sel);
    }
  }

  // Build the set of methods we can see.
  typedef CodeCompletionResult Result;
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);

  if (ReturnTy)
    Results.setPreferredType(GetTypeFromParser(ReturnTy).getNonReferenceType());

  Results.EnterNewScope();
  for (GlobalMethodPool::iterator M = MethodPool.begin(),
                                  MEnd = MethodPool.end();
       M != MEnd; ++M) {
    for (ObjCMethodList *MethList = IsInstanceMethod ? &M->second.first
                                                     : &M->second.second;
         MethList && MethList->getMethod(); MethList = MethList->getNext()) {
      if (!isAcceptableObjCMethod(MethList->getMethod(), MK_Any, SelIdents))
        continue;

      if (AtParameterName) {
        // Suggest parameter names we've seen before.
        unsigned NumSelIdents = SelIdents.size();
        if (NumSelIdents &&
            NumSelIdents <= MethList->getMethod()->param_size()) {
          ParmVarDecl *Param =
              MethList->getMethod()->parameters()[NumSelIdents - 1];
          if (Param->getIdentifier()) {
            CodeCompletionBuilder Builder(Results.getAllocator(),
                                          Results.getCodeCompletionTUInfo());
            Builder.AddTypedTextChunk(Builder.getAllocator().CopyString(
                Param->getIdentifier()->getName()));
            Results.AddResult(Builder.TakeString());
          }
        }

        continue;
      }

      Result R(MethList->getMethod(),
               Results.getBasePriority(MethList->getMethod()), nullptr);
      R.StartParameter = SelIdents.size();
      R.AllParametersAreInformative = false;
      R.DeclaringEntity = true;
      Results.MaybeAddResult(R, CurContext);
    }
  }

  Results.ExitScope();

  if (!AtParameterName && !SelIdents.empty() &&
      SelIdents.front()->getName().startswith("init")) {
    for (const auto &M : PP.macros()) {
      if (M.first->getName() != "NS_DESIGNATED_INITIALIZER")
        continue;
      Results.EnterNewScope();
      CodeCompletionBuilder Builder(Results.getAllocator(),
                                    Results.getCodeCompletionTUInfo());
      Builder.AddTypedTextChunk(
          Builder.getAllocator().CopyString(M.first->getName()));
      Results.AddResult(CodeCompletionResult(Builder.TakeString(), CCP_Macro,
                                             CXCursor_MacroDefinition));
      Results.ExitScope();
    }
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompletePreprocessorDirective(bool InConditional) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_PreprocessorDirective);
  Results.EnterNewScope();

  // #if <condition>
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());
  Builder.AddTypedTextChunk("if");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("condition");
  Results.AddResult(Builder.TakeString());

  // #ifdef <macro>
  Builder.AddTypedTextChunk("ifdef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());

  // #ifndef <macro>
  Builder.AddTypedTextChunk("ifndef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());

  if (InConditional) {
    // #elif <condition>
    Builder.AddTypedTextChunk("elif");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddPlaceholderChunk("condition");
    Results.AddResult(Builder.TakeString());

    // #else
    Builder.AddTypedTextChunk("else");
    Results.AddResult(Builder.TakeString());

    // #endif
    Builder.AddTypedTextChunk("endif");
    Results.AddResult(Builder.TakeString());
  }

  // #include "header"
  Builder.AddTypedTextChunk("include");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("\"");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk("\"");
  Results.AddResult(Builder.TakeString());

  // #include <header>
  Builder.AddTypedTextChunk("include");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("<");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk(">");
  Results.AddResult(Builder.TakeString());

  // #define <macro>
  Builder.AddTypedTextChunk("define");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());

  // #define <macro>(<args>)
  Builder.AddTypedTextChunk("define");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("args");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Builder.TakeString());

  // #undef <macro>
  Builder.AddTypedTextChunk("undef");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("macro");
  Results.AddResult(Builder.TakeString());

  // #line <number>
  Builder.AddTypedTextChunk("line");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("number");
  Results.AddResult(Builder.TakeString());

  // #line <number> "filename"
  Builder.AddTypedTextChunk("line");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("number");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("\"");
  Builder.AddPlaceholderChunk("filename");
  Builder.AddTextChunk("\"");
  Results.AddResult(Builder.TakeString());

  // #error <message>
  Builder.AddTypedTextChunk("error");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("message");
  Results.AddResult(Builder.TakeString());

  // #pragma <arguments>
  Builder.AddTypedTextChunk("pragma");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("arguments");
  Results.AddResult(Builder.TakeString());

  if (getLangOpts().ObjC) {
    // #import "header"
    Builder.AddTypedTextChunk("import");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddTextChunk("\"");
    Builder.AddPlaceholderChunk("header");
    Builder.AddTextChunk("\"");
    Results.AddResult(Builder.TakeString());

    // #import <header>
    Builder.AddTypedTextChunk("import");
    Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
    Builder.AddTextChunk("<");
    Builder.AddPlaceholderChunk("header");
    Builder.AddTextChunk(">");
    Results.AddResult(Builder.TakeString());
  }

  // #include_next "header"
  Builder.AddTypedTextChunk("include_next");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("\"");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk("\"");
  Results.AddResult(Builder.TakeString());

  // #include_next <header>
  Builder.AddTypedTextChunk("include_next");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddTextChunk("<");
  Builder.AddPlaceholderChunk("header");
  Builder.AddTextChunk(">");
  Results.AddResult(Builder.TakeString());

  // #warning <message>
  Builder.AddTypedTextChunk("warning");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddPlaceholderChunk("message");
  Results.AddResult(Builder.TakeString());

  // Note: #ident and #sccs are such crazy anachronisms that we don't provide
  // completions for them. And __include_macros is a Clang-internal extension
  // that we don't want to encourage anyone to use.

  // FIXME: we don't support #assert or #unassert, so don't suggest them.
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteInPreprocessorConditionalExclusion(Scope *S) {
  CodeCompleteOrdinaryName(S, S->getFnParent() ? Sema::PCC_RecoveryInFunction
                                               : Sema::PCC_Namespace);
}

void Sema::CodeCompletePreprocessorMacroName(bool IsDefinition) {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        IsDefinition ? CodeCompletionContext::CCC_MacroName
                                     : CodeCompletionContext::CCC_MacroNameUse);
  if (!IsDefinition && (!CodeCompleter || CodeCompleter->includeMacros())) {
    // Add just the names of macros, not their arguments.
    CodeCompletionBuilder Builder(Results.getAllocator(),
                                  Results.getCodeCompletionTUInfo());
    Results.EnterNewScope();
    for (Preprocessor::macro_iterator M = PP.macro_begin(),
                                      MEnd = PP.macro_end();
         M != MEnd; ++M) {
      Builder.AddTypedTextChunk(
          Builder.getAllocator().CopyString(M->first->getName()));
      Results.AddResult(CodeCompletionResult(
          Builder.TakeString(), CCP_CodePattern, CXCursor_MacroDefinition));
    }
    Results.ExitScope();
  } else if (IsDefinition) {
    // FIXME: Can we detect when the user just wrote an include guard above?
  }

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompletePreprocessorExpression() {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_PreprocessorExpression);

  if (!CodeCompleter || CodeCompleter->includeMacros())
    AddMacroResults(PP, Results,
                    CodeCompleter ? CodeCompleter->loadExternal() : false,
                    true);

  // defined (<macro>)
  Results.EnterNewScope();
  CodeCompletionBuilder Builder(Results.getAllocator(),
                                Results.getCodeCompletionTUInfo());
  Builder.AddTypedTextChunk("defined");
  Builder.AddChunk(CodeCompletionString::CK_HorizontalSpace);
  Builder.AddChunk(CodeCompletionString::CK_LeftParen);
  Builder.AddPlaceholderChunk("macro");
  Builder.AddChunk(CodeCompletionString::CK_RightParen);
  Results.AddResult(Builder.TakeString());
  Results.ExitScope();

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompletePreprocessorMacroArgument(Scope *S,
                                                 IdentifierInfo *Macro,
                                                 MacroInfo *MacroInfo,
                                                 unsigned Argument) {
  // FIXME: In the future, we could provide "overload" results, much like we
  // do for function calls.

  // Now just ignore this. There will be another code-completion callback
  // for the expanded tokens.
}

// This handles completion inside an #include filename, e.g. #include <foo/ba
// We look for the directory "foo" under each directory on the include path,
// list its files, and reassemble the appropriate #include.
void Sema::CodeCompleteIncludedFile(llvm::StringRef Dir, bool Angled) {
  // RelDir should use /, but unescaped \ is possible on windows!
  // Our completions will normalize to / for simplicity, this case is rare.
  std::string RelDir = llvm::sys::path::convert_to_slash(Dir);
  // We need the native slashes for the actual file system interactions.
  SmallString<128> NativeRelDir = StringRef(RelDir);
  llvm::sys::path::native(NativeRelDir);
  llvm::vfs::FileSystem &FS =
      getSourceManager().getFileManager().getVirtualFileSystem();

  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_IncludedFile);
  llvm::DenseSet<StringRef> SeenResults; // To deduplicate results.

  // Helper: adds one file or directory completion result.
  auto AddCompletion = [&](StringRef Filename, bool IsDirectory) {
    SmallString<64> TypedChunk = Filename;
    // Directory completion is up to the slash, e.g. <sys/
    TypedChunk.push_back(IsDirectory ? '/' : Angled ? '>' : '"');
    auto R = SeenResults.insert(TypedChunk);
    if (R.second) { // New completion
      const char *InternedTyped = Results.getAllocator().CopyString(TypedChunk);
      *R.first = InternedTyped; // Avoid dangling StringRef.
      CodeCompletionBuilder Builder(CodeCompleter->getAllocator(),
                                    CodeCompleter->getCodeCompletionTUInfo());
      Builder.AddTypedTextChunk(InternedTyped);
      // The result is a "Pattern", which is pretty opaque.
      // We may want to include the real filename to allow smart ranking.
      Results.AddResult(CodeCompletionResult(Builder.TakeString()));
    }
  };

  // Helper: scans IncludeDir for nice files, and adds results for each.
  auto AddFilesFromIncludeDir = [&](StringRef IncludeDir,
                                    bool IsSystem,
                                    DirectoryLookup::LookupType_t LookupType) {
    llvm::SmallString<128> Dir = IncludeDir;
    if (!NativeRelDir.empty()) {
      if (LookupType == DirectoryLookup::LT_Framework) {
        // For a framework dir, #include <Foo/Bar/> actually maps to
        // a path of Foo.framework/Headers/Bar/.
        auto Begin = llvm::sys::path::begin(NativeRelDir);
        auto End = llvm::sys::path::end(NativeRelDir);

        llvm::sys::path::append(Dir, *Begin + ".framework", "Headers");
        llvm::sys::path::append(Dir, ++Begin, End);
      } else {
        llvm::sys::path::append(Dir, NativeRelDir);
      }
    }

    std::error_code EC;
    unsigned Count = 0;
    for (auto It = FS.dir_begin(Dir, EC);
         !EC && It != llvm::vfs::directory_iterator(); It.increment(EC)) {
      if (++Count == 2500) // If we happen to hit a huge directory,
        break;             // bail out early so we're not too slow.
      StringRef Filename = llvm::sys::path::filename(It->path());
      switch (It->type()) {
      case llvm::sys::fs::file_type::directory_file:
        // All entries in a framework directory must have a ".framework" suffix,
        // but the suffix does not appear in the source code's include/import.
        if (LookupType == DirectoryLookup::LT_Framework &&
            NativeRelDir.empty() && !Filename.consume_back(".framework"))
          break;

        AddCompletion(Filename, /*IsDirectory=*/true);
        break;
      case llvm::sys::fs::file_type::regular_file:
        // Only files that really look like headers. (Except in system dirs).
        if (!IsSystem) {
          // Header extensions from Types.def, which we can't depend on here.
          if (!(Filename.endswith_lower(".h") ||
                Filename.endswith_lower(".hh") ||
                Filename.endswith_lower(".hpp") ||
                Filename.endswith_lower(".inc")))
            break;
        }
        AddCompletion(Filename, /*IsDirectory=*/false);
        break;
      default:
        break;
      }
    }
  };

  // Helper: adds results relative to IncludeDir, if possible.
  auto AddFilesFromDirLookup = [&](const DirectoryLookup &IncludeDir,
                                   bool IsSystem) {
    switch (IncludeDir.getLookupType()) {
    case DirectoryLookup::LT_HeaderMap:
      // header maps are not (currently) enumerable.
      break;
    case DirectoryLookup::LT_NormalDir:
      AddFilesFromIncludeDir(IncludeDir.getDir()->getName(), IsSystem,
                             DirectoryLookup::LT_NormalDir);
      break;
    case DirectoryLookup::LT_Framework:
      AddFilesFromIncludeDir(IncludeDir.getFrameworkDir()->getName(), IsSystem,
                             DirectoryLookup::LT_Framework);
      break;
    }
  };

  // Finally with all our helpers, we can scan the include path.
  // Do this in standard order so deduplication keeps the right file.
  // (In case we decide to add more details to the results later).
  const auto &S = PP.getHeaderSearchInfo();
  using llvm::make_range;
  if (!Angled) {
    // The current directory is on the include path for "quoted" includes.
    auto *CurFile = PP.getCurrentFileLexer()->getFileEntry();
    if (CurFile && CurFile->getDir())
      AddFilesFromIncludeDir(CurFile->getDir()->getName(), false,
                             DirectoryLookup::LT_NormalDir);
    for (const auto &D : make_range(S.quoted_dir_begin(), S.quoted_dir_end()))
      AddFilesFromDirLookup(D, false);
  }
  for (const auto &D : make_range(S.angled_dir_begin(), S.angled_dir_end()))
    AddFilesFromDirLookup(D, false);
  for (const auto &D : make_range(S.system_dir_begin(), S.system_dir_end()))
    AddFilesFromDirLookup(D, true);

  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::CodeCompleteNaturalLanguage() {
  HandleCodeCompleteResults(this, CodeCompleter,
                            CodeCompletionContext::CCC_NaturalLanguage, nullptr,
                            0);
}

void Sema::CodeCompleteAvailabilityPlatformName() {
  ResultBuilder Results(*this, CodeCompleter->getAllocator(),
                        CodeCompleter->getCodeCompletionTUInfo(),
                        CodeCompletionContext::CCC_Other);
  Results.EnterNewScope();
  static const char *Platforms[] = {"macOS", "iOS", "watchOS", "tvOS"};
  for (const char *Platform : llvm::makeArrayRef(Platforms)) {
    Results.AddResult(CodeCompletionResult(Platform));
    Results.AddResult(CodeCompletionResult(Results.getAllocator().CopyString(
        Twine(Platform) + "ApplicationExtension")));
  }
  Results.ExitScope();
  HandleCodeCompleteResults(this, CodeCompleter, Results.getCompletionContext(),
                            Results.data(), Results.size());
}

void Sema::GatherGlobalCodeCompletions(
    CodeCompletionAllocator &Allocator, CodeCompletionTUInfo &CCTUInfo,
    SmallVectorImpl<CodeCompletionResult> &Results) {
  ResultBuilder Builder(*this, Allocator, CCTUInfo,
                        CodeCompletionContext::CCC_Recovery);
  if (!CodeCompleter || CodeCompleter->includeGlobals()) {
    CodeCompletionDeclConsumer Consumer(Builder,
                                        Context.getTranslationUnitDecl());
    LookupVisibleDecls(Context.getTranslationUnitDecl(), LookupAnyName,
                       Consumer,
                       !CodeCompleter || CodeCompleter->loadExternal());
  }

  if (!CodeCompleter || CodeCompleter->includeMacros())
    AddMacroResults(PP, Builder,
                    CodeCompleter ? CodeCompleter->loadExternal() : false,
                    true);

  Results.clear();
  Results.insert(Results.end(), Builder.data(),
                 Builder.data() + Builder.size());
}
