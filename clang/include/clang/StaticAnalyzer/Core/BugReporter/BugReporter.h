//===- BugReporter.h - Generate PathDiagnostics -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines BugReporter, a utility class for generating
//  PathDiagnostics for analyses based on ProgramState.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_STATICANALYZER_CORE_BUGREPORTER_BUGREPORTER_H
#define LLVM_CLANG_STATICANALYZER_CORE_BUGREPORTER_BUGREPORTER_H

#include "clang/Basic/LLVM.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporterVisitors.h"
#include "clang/StaticAnalyzer/Core/BugReporter/PathDiagnostic.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExplodedGraph.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/ImmutableSet.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/iterator_range.h"
#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clang {

class AnalyzerOptions;
class ASTContext;
class Decl;
class DiagnosticsEngine;
class LocationContext;
class SourceManager;
class Stmt;

namespace ento {

class BugType;
class CheckerBase;
class ExplodedGraph;
class ExplodedNode;
class ExprEngine;
class MemRegion;
class SValBuilder;

//===----------------------------------------------------------------------===//
// Interface for individual bug reports.
//===----------------------------------------------------------------------===//

/// A mapping from diagnostic consumers to the diagnostics they should
/// consume.
using DiagnosticForConsumerMapTy =
    llvm::DenseMap<PathDiagnosticConsumer *, std::unique_ptr<PathDiagnostic>>;

/// This class provides an interface through which checkers can create
/// individual bug reports.
class BugReport : public llvm::ilist_node<BugReport> {
public:
  using ranges_iterator = const SourceRange *;
  using VisitorList = SmallVector<std::unique_ptr<BugReporterVisitor>, 8>;
  using visitor_iterator = VisitorList::iterator;
  using visitor_range = llvm::iterator_range<visitor_iterator>;
  using NoteList = SmallVector<std::shared_ptr<PathDiagnosticNotePiece>, 4>;

protected:
  friend class BugReportEquivClass;
  friend class BugReporter;

  const BugType& BT;
  const Decl *DeclWithIssue = nullptr;
  std::string ShortDescription;
  std::string Description;
  PathDiagnosticLocation Location;
  PathDiagnosticLocation UniqueingLocation;
  const Decl *UniqueingDecl;

  const ExplodedNode *ErrorNode = nullptr;
  SmallVector<SourceRange, 4> Ranges;
  const SourceRange ErrorNodeRange;
  NoteList Notes;
  SmallVector<FixItHint, 4> Fixits;

  /// A (stack of) a set of symbols that are registered with this
  /// report as being "interesting", and thus used to help decide which
  /// diagnostics to include when constructing the final path diagnostic.
  /// The stack is largely used by BugReporter when generating PathDiagnostics
  /// for multiple PathDiagnosticConsumers.
  llvm::DenseMap<SymbolRef, bugreporter::TrackingKind> InterestingSymbols;

  /// A (stack of) set of regions that are registered with this report as being
  /// "interesting", and thus used to help decide which diagnostics
  /// to include when constructing the final path diagnostic.
  /// The stack is largely used by BugReporter when generating PathDiagnostics
  /// for multiple PathDiagnosticConsumers.
  llvm::DenseMap<const MemRegion *, bugreporter::TrackingKind>
      InterestingRegions;

  /// A set of location contexts that correspoind to call sites which should be
  /// considered "interesting".
  llvm::SmallSet<const LocationContext *, 2> InterestingLocationContexts;

  /// A set of custom visitors which generate "event" diagnostics at
  /// interesting points in the path.
  VisitorList Callbacks;

  /// Used for ensuring the visitors are only added once.
  llvm::FoldingSet<BugReporterVisitor> CallbacksSet;

  /// When set, this flag disables all callstack pruning from a diagnostic
  /// path.  This is useful for some reports that want maximum fidelty
  /// when reporting an issue.
  bool DoNotPrunePath = false;

  /// Used to track unique reasons why a bug report might be invalid.
  ///
  /// \sa markInvalid
  /// \sa removeInvalidation
  using InvalidationRecord = std::pair<const void *, const void *>;

  /// If non-empty, this bug report is likely a false positive and should not be
  /// shown to the user.
  ///
  /// \sa markInvalid
  /// \sa removeInvalidation
  llvm::SmallSet<InvalidationRecord, 4> Invalidations;

  /// Conditions we're already tracking.
  llvm::SmallSet<const ExplodedNode *, 4> TrackedConditions;

public:
  BugReport(const BugType &bt, StringRef desc, const ExplodedNode *errornode)
      : BT(bt), Description(desc), ErrorNode(errornode),
        ErrorNodeRange(getStmt() ? getStmt()->getSourceRange()
                                 : SourceRange()) {}

  BugReport(const BugType &bt, StringRef shortDesc, StringRef desc,
            const ExplodedNode *errornode)
      : BT(bt), ShortDescription(shortDesc), Description(desc),
        ErrorNode(errornode),
        ErrorNodeRange(getStmt() ? getStmt()->getSourceRange()
                                 : SourceRange()) {}

  BugReport(const BugType &bt, StringRef desc, PathDiagnosticLocation l)
      : BT(bt), Description(desc), Location(l),
        ErrorNodeRange(getStmt() ? getStmt()->getSourceRange()
                                 : SourceRange()) {}

  /// Create a BugReport with a custom uniqueing location.
  ///
  /// The reports that have the same report location, description, bug type, and
  /// ranges are uniqued - only one of the equivalent reports will be presented
  /// to the user. This method allows to rest the location which should be used
  /// for uniquing reports. For example, memory leaks checker, could set this to
  /// the allocation site, rather then the location where the bug is reported.
  BugReport(BugType &bt, StringRef desc, const ExplodedNode *errornode,
            PathDiagnosticLocation LocationToUnique, const Decl *DeclToUnique)
      : BT(bt), Description(desc), UniqueingLocation(LocationToUnique),
        UniqueingDecl(DeclToUnique), ErrorNode(errornode),
        ErrorNodeRange(getStmt() ? getStmt()->getSourceRange()
                                 : SourceRange()) {}

  virtual ~BugReport() = default;

  const BugType& getBugType() const { return BT; }
  //BugType& getBugType() { return BT; }

  /// True when the report has an execution path associated with it.
  ///
  /// A report is said to be path-sensitive if it was thrown against a
  /// particular exploded node in the path-sensitive analysis graph.
  /// Path-sensitive reports have their intermediate path diagnostics
  /// auto-generated, perhaps with the help of checker-defined visitors,
  /// and may contain extra notes.
  /// Path-insensitive reports consist only of a single warning message
  /// in a specific location, and perhaps extra notes.
  /// Path-sensitive checkers are allowed to throw path-insensitive reports.
  bool isPathSensitive() const { return ErrorNode != nullptr; }

  const ExplodedNode *getErrorNode() const { return ErrorNode; }

  StringRef getDescription() const { return Description; }

  StringRef getShortDescription(bool UseFallback = true) const {
    if (ShortDescription.empty() && UseFallback)
      return Description;
    return ShortDescription;
  }

  /// Indicates whether or not any path pruning should take place
  /// when generating a PathDiagnostic from this BugReport.
  bool shouldPrunePath() const { return !DoNotPrunePath; }

  /// Disable all path pruning when generating a PathDiagnostic.
  void disablePathPruning() { DoNotPrunePath = true; }

  /// Marks a symbol as interesting. Different kinds of interestingness will
  /// be processed differently by visitors (e.g. if the tracking kind is
  /// condition, will append "will be used as a condition" to the message).
  void markInteresting(SymbolRef sym, bugreporter::TrackingKind TKind =
                                          bugreporter::TrackingKind::Thorough);

  /// Marks a region as interesting. Different kinds of interestingness will
  /// be processed differently by visitors (e.g. if the tracking kind is
  /// condition, will append "will be used as a condition" to the message).
  void markInteresting(
      const MemRegion *R,
      bugreporter::TrackingKind TKind = bugreporter::TrackingKind::Thorough);

  /// Marks a symbolic value as interesting. Different kinds of interestingness
  /// will be processed differently by visitors (e.g. if the tracking kind is
  /// condition, will append "will be used as a condition" to the message).
  void markInteresting(SVal V, bugreporter::TrackingKind TKind =
                                   bugreporter::TrackingKind::Thorough);
  void markInteresting(const LocationContext *LC);

  bool isInteresting(SymbolRef sym) const;
  bool isInteresting(const MemRegion *R) const;
  bool isInteresting(SVal V) const;
  bool isInteresting(const LocationContext *LC) const;

  Optional<bugreporter::TrackingKind>
  getInterestingnessKind(SymbolRef sym) const;

  Optional<bugreporter::TrackingKind>
  getInterestingnessKind(const MemRegion *R) const;

  Optional<bugreporter::TrackingKind> getInterestingnessKind(SVal V) const;

  /// Returns whether or not this report should be considered valid.
  ///
  /// Invalid reports are those that have been classified as likely false
  /// positives after the fact.
  bool isValid() const {
    return Invalidations.empty();
  }

  /// Marks the current report as invalid, meaning that it is probably a false
  /// positive and should not be reported to the user.
  ///
  /// The \p Tag and \p Data arguments are intended to be opaque identifiers for
  /// this particular invalidation, where \p Tag represents the visitor
  /// responsible for invalidation, and \p Data represents the reason this
  /// visitor decided to invalidate the bug report.
  ///
  /// \sa removeInvalidation
  void markInvalid(const void *Tag, const void *Data) {
    Invalidations.insert(std::make_pair(Tag, Data));
  }

  /// Return the canonical declaration, be it a method or class, where
  /// this issue semantically occurred.
  const Decl *getDeclWithIssue() const;

  /// Specifically set the Decl where an issue occurred.  This isn't necessary
  /// for BugReports that cover a path as it will be automatically inferred.
  void setDeclWithIssue(const Decl *declWithIssue) {
    DeclWithIssue = declWithIssue;
  }

  /// Add new item to the list of additional notes that need to be attached to
  /// this path-insensitive report. If you want to add extra notes to a
  /// path-sensitive report, you need to use a BugReporterVisitor because it
  /// allows you to specify where exactly in the auto-generated path diagnostic
  /// the extra note should appear.
  void addNote(StringRef Msg, const PathDiagnosticLocation &Pos,
               ArrayRef<SourceRange> Ranges = {},
               ArrayRef<FixItHint> Fixits = {}) {
    auto P = std::make_shared<PathDiagnosticNotePiece>(Pos, Msg);

    for (const auto &R : Ranges)
      P->addRange(R);

    for (const auto &F : Fixits)
      P->addFixit(F);

    Notes.push_back(std::move(P));
  }

  virtual const NoteList &getNotes() {
    return Notes;
  }

  /// Return the "definitive" location of the reported bug.
  ///
  ///  While a bug can span an entire path, usually there is a specific
  ///  location that can be used to identify where the key issue occurred.
  ///  This location is used by clients rendering diagnostics.
  virtual PathDiagnosticLocation getLocation(const SourceManager &SM) const;

  /// Get the location on which the report should be uniqued.
  PathDiagnosticLocation getUniqueingLocation() const {
    return UniqueingLocation;
  }

  /// Get the declaration containing the uniqueing location.
  const Decl *getUniqueingDecl() const {
    return UniqueingDecl;
  }

  const Stmt *getStmt() const;

  /// Add a range to the bug report.
  ///
  /// Ranges are used to highlight regions of interest in the source code.
  /// They should be at the same source code line as the BugReport location.
  /// By default, the source range of the statement corresponding to the error
  /// node will be used; add a single invalid range to specify absence of
  /// ranges.
  void addRange(SourceRange R) {
    assert((R.isValid() || Ranges.empty()) && "Invalid range can only be used "
                           "to specify that the report does not have a range.");
    Ranges.push_back(R);
  }

  /// Get the SourceRanges associated with the report.
  virtual llvm::iterator_range<ranges_iterator> getRanges() const;

  /// Add a fix-it hint to the warning message of the bug report.
  ///
  /// Fix-it hints are the suggested edits to the code that would resolve
  /// the problem explained by the bug report. Fix-it hints should be
  /// as conservative as possible because it is not uncommon for the user
  /// to blindly apply all fixits to their project. It usually is very hard
  /// to produce a good fix-it hint for most path-sensitive warnings.
  /// Fix-it hints can also be added to notes through the addNote() interface.
  void addFixItHint(const FixItHint &F) {
    Fixits.push_back(F);
  }

  ArrayRef<FixItHint> getFixits() const { return Fixits; }

  /// Add custom or predefined bug report visitors to this report.
  ///
  /// The visitors should be used when the default trace is not sufficient.
  /// For example, they allow constructing a more elaborate trace.
  /// \sa registerConditionVisitor(), registerTrackNullOrUndefValue(),
  /// registerFindLastStore(), registerNilReceiverVisitor(), and
  /// registerVarDeclsLastStore().
  void addVisitor(std::unique_ptr<BugReporterVisitor> visitor);

  /// Remove all visitors attached to this bug report.
  void clearVisitors();

  /// Iterators through the custom diagnostic visitors.
  visitor_iterator visitor_begin() { return Callbacks.begin(); }
  visitor_iterator visitor_end() { return Callbacks.end(); }
  visitor_range visitors() { return {visitor_begin(), visitor_end()}; }

  /// Notes that the condition of the CFGBlock associated with \p Cond is
  /// being tracked.
  /// \returns false if the condition is already being tracked.
  bool addTrackedCondition(const ExplodedNode *Cond) {
    return TrackedConditions.insert(Cond).second;
  }

  /// Profile to identify equivalent bug reports for error report coalescing.
  /// Reports are uniqued to ensure that we do not emit multiple diagnostics
  /// for each bug.
  virtual void Profile(llvm::FoldingSetNodeID& hash) const;
};

//===----------------------------------------------------------------------===//
// BugTypes (collections of related reports).
//===----------------------------------------------------------------------===//

class BugReportEquivClass : public llvm::FoldingSetNode {
  friend class BugReporter;

  /// List of *owned* BugReport objects.
  llvm::ilist<BugReport> Reports;

  void AddReport(std::unique_ptr<BugReport> R) {
    Reports.push_back(R.release());
  }

public:
  BugReportEquivClass(std::unique_ptr<BugReport> R) { AddReport(std::move(R)); }

  void Profile(llvm::FoldingSetNodeID& ID) const {
    assert(!Reports.empty());
    Reports.front().Profile(ID);
  }

  using iterator = llvm::ilist<BugReport>::iterator;
  using const_iterator = llvm::ilist<BugReport>::const_iterator;

  iterator begin() { return Reports.begin(); }
  iterator end() { return Reports.end(); }

  const_iterator begin() const { return Reports.begin(); }
  const_iterator end() const { return Reports.end(); }
};

//===----------------------------------------------------------------------===//
// BugReporter and friends.
//===----------------------------------------------------------------------===//

class BugReporterData {
public:
  virtual ~BugReporterData() = default;

  virtual ArrayRef<PathDiagnosticConsumer*> getPathDiagnosticConsumers() = 0;
  virtual ASTContext &getASTContext() = 0;
  virtual SourceManager &getSourceManager() = 0;
  virtual AnalyzerOptions &getAnalyzerOptions() = 0;
};

/// BugReporter is a utility class for generating PathDiagnostics for analysis.
/// It collects the BugReports and BugTypes and knows how to generate
/// and flush the corresponding diagnostics.
///
/// The base class is used for generating path-insensitive
class BugReporter {
private:
  BugReporterData& D;

  /// Generate and flush the diagnostics for the given bug report.
  void FlushReport(BugReportEquivClass& EQ);

  /// Generate the diagnostics for the given bug report.
  std::unique_ptr<DiagnosticForConsumerMapTy>
  generateDiagnosticForConsumerMap(BugReport *exampleReport,
                                   ArrayRef<PathDiagnosticConsumer *> consumers,
                                   ArrayRef<BugReport *> bugReports);

  /// The set of bug reports tracked by the BugReporter.
  llvm::FoldingSet<BugReportEquivClass> EQClasses;

  /// A vector of BugReports for tracking the allocated pointers and cleanup.
  std::vector<BugReportEquivClass *> EQClassesVector;

public:
  BugReporter(BugReporterData &d) : D(d) {}
  virtual ~BugReporter();

  /// Generate and flush diagnostics for all bug reports.
  void FlushReports();

  ArrayRef<PathDiagnosticConsumer*> getPathDiagnosticConsumers() {
    return D.getPathDiagnosticConsumers();
  }

  /// Iterator over the set of BugReports tracked by the BugReporter.
  using EQClasses_iterator = llvm::FoldingSet<BugReportEquivClass>::iterator;
  EQClasses_iterator EQClasses_begin() { return EQClasses.begin(); }
  EQClasses_iterator EQClasses_end() { return EQClasses.end(); }

  ASTContext &getContext() { return D.getASTContext(); }

  const SourceManager &getSourceManager() { return D.getSourceManager(); }

  const AnalyzerOptions &getAnalyzerOptions() { return D.getAnalyzerOptions(); }

  virtual std::unique_ptr<DiagnosticForConsumerMapTy>
  generatePathDiagnostics(ArrayRef<PathDiagnosticConsumer *> consumers,
                          ArrayRef<BugReport *> &bugReports) {
    return {};
  }

  /// Add the given report to the set of reports tracked by BugReporter.
  ///
  /// The reports are usually generated by the checkers. Further, they are
  /// folded based on the profile value, which is done to coalesce similar
  /// reports.
  void emitReport(std::unique_ptr<BugReport> R);

  void EmitBasicReport(const Decl *DeclWithIssue, const CheckerBase *Checker,
                       StringRef BugName, StringRef BugCategory,
                       StringRef BugStr, PathDiagnosticLocation Loc,
                       ArrayRef<SourceRange> Ranges = None,
                       ArrayRef<FixItHint> Fixits = None);

  void EmitBasicReport(const Decl *DeclWithIssue, CheckName CheckName,
                       StringRef BugName, StringRef BugCategory,
                       StringRef BugStr, PathDiagnosticLocation Loc,
                       ArrayRef<SourceRange> Ranges = None,
                       ArrayRef<FixItHint> Fixits = None);

private:
  llvm::StringMap<BugType *> StrBugTypes;

  /// Returns a BugType that is associated with the given name and
  /// category.
  BugType *getBugTypeForName(CheckName CheckName, StringRef name,
                             StringRef category);
};

/// GRBugReporter is used for generating path-sensitive reports.
class PathSensitiveBugReporter : public BugReporter {
  ExprEngine& Eng;

public:
  PathSensitiveBugReporter(BugReporterData& d, ExprEngine& eng)
      : BugReporter(d), Eng(eng) {}

  /// getGraph - Get the exploded graph created by the analysis engine
  ///  for the analyzed method or function.
  const ExplodedGraph &getGraph() const;

  /// getStateManager - Return the state manager used by the analysis
  ///  engine.
  ProgramStateManager &getStateManager() const;

  /// \p bugReports A set of bug reports within a *single* equivalence class
  ///
  /// \return A mapping from consumers to the corresponding diagnostics.
  /// Iterates through the bug reports within a single equivalence class,
  /// stops at a first non-invalidated report.
  std::unique_ptr<DiagnosticForConsumerMapTy>
  generatePathDiagnostics(ArrayRef<PathDiagnosticConsumer *> consumers,
                          ArrayRef<BugReport *> &bugReports) override;
};


class BugReporterContext {
  PathSensitiveBugReporter &BR;

  virtual void anchor();

public:
  BugReporterContext(PathSensitiveBugReporter &br) : BR(br) {}

  virtual ~BugReporterContext() = default;

  PathSensitiveBugReporter& getBugReporter() { return BR; }

  ProgramStateManager& getStateManager() const {
    return BR.getStateManager();
  }

  ASTContext &getASTContext() const {
    return BR.getContext();
  }

  const SourceManager& getSourceManager() const {
    return BR.getSourceManager();
  }

  const AnalyzerOptions &getAnalyzerOptions() const {
    return BR.getAnalyzerOptions();
  }
};


/// The tag upon which the TagVisitor reacts. Add these in order to display
/// additional PathDiagnosticEventPieces along the path.
class NoteTag : public ProgramPointTag {
public:
  using Callback =
      std::function<std::string(BugReporterContext &, BugReport &)>;

private:
  static int Kind;

  const Callback Cb;
  const bool IsPrunable;

  NoteTag(Callback &&Cb, bool IsPrunable)
      : ProgramPointTag(&Kind), Cb(std::move(Cb)), IsPrunable(IsPrunable) {}

public:
  static bool classof(const ProgramPointTag *T) {
    return T->getTagKind() == &Kind;
  }

  Optional<std::string> generateMessage(BugReporterContext &BRC,
                                        BugReport &R) const {
    std::string Msg = Cb(BRC, R);
    if (Msg.empty())
      return None;

    return std::move(Msg);
  }

  StringRef getTagDescription() const override {
    // TODO: Remember a few examples of generated messages
    // and display them in the ExplodedGraph dump by
    // returning them from this function.
    return "Note Tag";
  }

  bool isPrunable() const { return IsPrunable; }

  // Manage memory for NoteTag objects.
  class Factory {
    std::vector<std::unique_ptr<NoteTag>> Tags;

  public:
    const NoteTag *makeNoteTag(Callback &&Cb, bool IsPrunable = false) {
      // We cannot use std::make_unique because we cannot access the private
      // constructor from inside it.
      std::unique_ptr<NoteTag> T(new NoteTag(std::move(Cb), IsPrunable));
      Tags.push_back(std::move(T));
      return Tags.back().get();
    }
  };

  friend class TagVisitor;
};

} // namespace ento

} // namespace clang

#endif // LLVM_CLANG_STATICANALYZER_CORE_BUGREPORTER_BUGREPORTER_H
