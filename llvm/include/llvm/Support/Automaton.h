//===-- Automaton.h - Support for driving TableGen-produced DFAs ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements class that drive and introspect deterministic finite-
// state automata (DFAs) as generated by TableGen's -gen-automata backend.
//
// For a description of how to define an automaton, see
// include/llvm/TableGen/Automaton.td.
//
// One important detail is that these deterministic automata are created from
// (potentially) nondeterministic definitions. Therefore a unique sequence of
// input symbols will produce one path through the DFA but multiple paths
// through the original NFA. An automaton by default only returns "accepted" or
// "not accepted", but frequently we want to analyze what NFA path was taken.
// Finding a path through the NFA states that results in a DFA state can help
// answer *what* the solution to a problem was, not just that there exists a
// solution.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_AUTOMATON_H
#define LLVM_SUPPORT_AUTOMATON_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include <deque>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace llvm {

using NfaPath = SmallVector<uint64_t, 4>;

/// Forward define the pair type used by the automata transition info tables.
///
/// Experimental results with large tables have shown a significant (multiple
/// orders of magnitude) parsing speedup by using a custom struct here with a
/// trivial constructor rather than std::pair<uint64_t, uint64_t>.
struct NfaStatePair {
  uint64_t FromDfaState, ToDfaState;

  bool operator<(const NfaStatePair &Other) const {
    return std::make_tuple(FromDfaState, ToDfaState) <
           std::make_tuple(Other.FromDfaState, Other.ToDfaState);
  }
};

namespace internal {
/// The internal class that maintains all possible paths through an NFA based
/// on a path through the DFA.
class NfaTranscriber {
private:
  /// Cached transition table. This is a table of NfaStatePairs that contains
  /// zero-terminated sequences pointed to by DFA transitions.
  ArrayRef<NfaStatePair> TransitionInfo;

  /// A simple linked-list of traversed states that can have a shared tail. The
  /// traversed path is stored in reverse order with the latest state as the
  /// head.
  struct PathSegment {
    uint64_t State;
    PathSegment *Tail;
  };

  /// We allocate segment objects frequently. Allocate them upfront and dispose
  /// at the end of a traversal rather than hammering the system allocator.
  SpecificBumpPtrAllocator<PathSegment> Allocator;

  /// Heads of each tracked path. These are not ordered.
  std::deque<PathSegment *> Heads;

  /// The returned paths. This is populated during getPaths.
  SmallVector<NfaPath, 4> Paths;

  /// Create a new segment and return it.
  PathSegment *makePathSegment(uint64_t State, PathSegment *Tail) {
    PathSegment *P = Allocator.Allocate();
    *P = {State, Tail};
    return P;
  }

  /// Pairs defines a sequence of possible NFA transitions for a single DFA
  /// transition.
  void transition(ArrayRef<NfaStatePair> Pairs) {
    // Iterate over all existing heads. We will mutate the Heads deque during
    // iteration.
    unsigned NumHeads = Heads.size();
    for (auto HeadI = Heads.begin(), HeadE = std::next(Heads.begin(), NumHeads);
         HeadI != HeadE; ++HeadI) {
      PathSegment *Head = *HeadI;
      // The sequence of pairs is sorted. Select the set of pairs that
      // transition from the current head state.
      auto PI = lower_bound(Pairs, NfaStatePair{Head->State, 0ULL});
      auto PE = upper_bound(Pairs, NfaStatePair{Head->State, INT64_MAX});
      // For every transition from the current head state, add a new path
      // segment.
      for (; PI != PE; ++PI)
        if (PI->FromDfaState == Head->State)
          Heads.push_back(makePathSegment(PI->ToDfaState, Head));
    }
    // Now we've iterated over all the initial heads and added new ones,
    // dispose of the original heads.
    Heads.erase(Heads.begin(), std::next(Heads.begin(), NumHeads));
  }

public:
  NfaTranscriber(ArrayRef<NfaStatePair> TransitionInfo)
      : TransitionInfo(TransitionInfo) {
    reset();
  }

  void reset() {
    Paths.clear();
    Heads.clear();
    Allocator.DestroyAll();
    // The initial NFA state is 0.
    Heads.push_back(makePathSegment(0ULL, nullptr));
  }

  void transition(unsigned TransitionInfoIdx) {
    unsigned EndIdx = TransitionInfoIdx;
    while (TransitionInfo[EndIdx].ToDfaState != 0)
      ++EndIdx;
    ArrayRef<NfaStatePair> Pairs(&TransitionInfo[TransitionInfoIdx],
                                 EndIdx - TransitionInfoIdx);
    transition(Pairs);
  }

  ArrayRef<NfaPath> getPaths() {
    Paths.clear();
    for (auto *Head : Heads) {
      NfaPath P;
      while (Head->State != 0) {
        P.push_back(Head->State);
        Head = Head->Tail;
      }
      std::reverse(P.begin(), P.end());
      Paths.push_back(std::move(P));
    }
    return Paths;
  }
};
} // namespace internal

/// A deterministic finite-state automaton. The automaton is defined in
/// TableGen; this object drives an automaton defined by tblgen-emitted tables.
///
/// An automaton accepts a sequence of input tokens ("actions"). This class is
/// templated on the type of these actions.
template <typename ActionT> class Automaton {
  /// Map from {State, Action} to {NewState, TransitionInfoIdx}.
  /// TransitionInfoIdx is used by the DfaTranscriber to analyze the transition.
  /// FIXME: This uses a std::map because ActionT can be a pair type including
  /// an enum. In particular DenseMapInfo<ActionT> must be defined to use
  /// DenseMap here.
  std::map<std::pair<uint64_t, ActionT>, std::pair<uint64_t, unsigned>> M;
  /// An optional transcription object. This uses much more state than simply
  /// traversing the DFA for acceptance, so is heap allocated.
  std::unique_ptr<internal::NfaTranscriber> Transcriber;
  /// The initial DFA state is 1.
  uint64_t State = 1;

public:
  /// Create an automaton.
  /// \param Transitions The Transitions table as created by TableGen. Note that
  ///                    because the action type differs per automaton, the
  ///                    table type is templated as ArrayRef<InfoT>.
  /// \param TranscriptionTable The TransitionInfo table as created by TableGen.
  ///
  /// Providing the TranscriptionTable argument as non-empty will enable the
  /// use of transcription, which analyzes the possible paths in the original
  /// NFA taken by the DFA. NOTE: This is substantially more work than simply
  /// driving the DFA, so unless you require the getPaths() method leave this
  /// empty.
  template <typename InfoT>
  Automaton(ArrayRef<InfoT> Transitions,
            ArrayRef<NfaStatePair> TranscriptionTable = {}) {
    if (!TranscriptionTable.empty())
      Transcriber =
          std::make_unique<internal::NfaTranscriber>(TranscriptionTable);
    for (const auto &I : Transitions)
      // Greedily read and cache the transition table.
      M.emplace(std::make_pair(I.FromDfaState, I.Action),
                std::make_pair(I.ToDfaState, I.InfoIdx));
  }

  /// Reset the automaton to its initial state.
  void reset() {
    State = 1;
    if (Transcriber)
      Transcriber->reset();
  }

  /// Transition the automaton based on input symbol A. Return true if the
  /// automaton transitioned to a valid state, false if the automaton
  /// transitioned to an invalid state.
  ///
  /// If this function returns false, all methods are undefined until reset() is
  /// called.
  bool add(const ActionT &A) {
    auto I = M.find({State, A});
    if (I == M.end())
      return false;
    if (Transcriber)
      Transcriber->transition(I->second.second);
    State = I->second.first;
    return true;
  }

  /// Obtain a set of possible paths through the input nondeterministic
  /// automaton that could be obtained from the sequence of input actions
  /// presented to this deterministic automaton.
  ArrayRef<NfaPath> getNfaPaths() {
    assert(Transcriber && "Can only obtain NFA paths if transcribing!");
    return Transcriber->getPaths();
  }
};

} // namespace llvm

#endif // LLVM_SUPPORT_AUTOMATON_H
