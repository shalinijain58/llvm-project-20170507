; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -indvars -dce -S | FileCheck %
; This is a collection of tests specifically for LFTR of multiple exit loops.
; The actual LFTR performed is trivial so as to focus on the loop structure
; aspects.

; Provide legal integer types.
target datalayout = "n8:16:32:64"

@A = external global i32

define void @analyzeable_early_exit(i32 %n) {
; CHECK-LABEL: @analyzeable_early_exit(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[EARLYCND:%.*]] = icmp ult i32 [[IV]], [[N:%.*]]
; CHECK-NEXT:    br i1 [[EARLYCND]], label [[LATCH]], label [[EXIT:%.*]]
; CHECK:       latch:
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i32 [[IV]], 1
; CHECK-NEXT:    store i32 [[IV]], i32* @A
; CHECK-NEXT:    [[C:%.*]] = icmp ult i32 [[IV_NEXT]], 1000
; CHECK-NEXT:    br i1 [[C]], label [[LOOP]], label [[EXIT]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry], [ %iv.next, %latch]
  %earlycnd = icmp ult i32 %iv, %n
  br i1 %earlycnd, label %latch, label %exit

latch:
  %iv.next = add i32 %iv, 1
  store i32 %iv, i32* @A
  %c = icmp ult i32 %iv.next, 1000
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

define void @unanalyzeable_early_exit() {
; CHECK-LABEL: @unanalyzeable_early_exit(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[VOL:%.*]] = load volatile i32, i32* @A
; CHECK-NEXT:    [[EARLYCND:%.*]] = icmp ne i32 [[VOL]], 0
; CHECK-NEXT:    br i1 [[EARLYCND]], label [[LATCH]], label [[EXIT:%.*]]
; CHECK:       latch:
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i32 [[IV]], 1
; CHECK-NEXT:    store i32 [[IV]], i32* @A
; CHECK-NEXT:    [[C:%.*]] = icmp ult i32 [[IV_NEXT]], 1000
; CHECK-NEXT:    br i1 [[C]], label [[LOOP]], label [[EXIT]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry], [ %iv.next, %latch]
  %vol = load volatile i32, i32* @A
  %earlycnd = icmp ne i32 %vol, 0
  br i1 %earlycnd, label %latch, label %exit

latch:
  %iv.next = add i32 %iv, 1
  store i32 %iv, i32* @A
  %c = icmp ult i32 %iv.next, 1000
  br i1 %c, label %loop, label %exit

exit:
  ret void
}


define void @multiple_early_exits(i32 %n, i32 %m) {
; CHECK-LABEL: @multiple_early_exits(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[EARLYCND:%.*]] = icmp ult i32 [[IV]], [[N:%.*]]
; CHECK-NEXT:    br i1 [[EARLYCND]], label [[CONTINUE:%.*]], label [[EXIT:%.*]]
; CHECK:       continue:
; CHECK-NEXT:    store volatile i32 [[IV]], i32* @A
; CHECK-NEXT:    [[EARLYCND2:%.*]] = icmp ult i32 [[IV]], [[M:%.*]]
; CHECK-NEXT:    br i1 [[EARLYCND2]], label [[LATCH]], label [[EXIT]]
; CHECK:       latch:
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i32 [[IV]], 1
; CHECK-NEXT:    store volatile i32 [[IV]], i32* @A
; CHECK-NEXT:    [[C:%.*]] = icmp ult i32 [[IV_NEXT]], 1000
; CHECK-NEXT:    br i1 [[C]], label [[LOOP]], label [[EXIT]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry], [ %iv.next, %latch]
  %earlycnd = icmp ult i32 %iv, %n
  br i1 %earlycnd, label %continue, label %exit

continue:
  store volatile i32 %iv, i32* @A
  %earlycnd2 = icmp ult i32 %iv, %m
  br i1 %earlycnd2, label %latch, label %exit

latch:
  %iv.next = add i32 %iv, 1
  store volatile i32 %iv, i32* @A
  %c = icmp ult i32 %iv.next, 1000
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; Note: This slightly odd form is what indvars itself produces for multiple
; exits without a side effect between them.
define void @compound_early_exit(i32 %n, i32 %m) {
; CHECK-LABEL: @compound_early_exit(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[EARLYCND:%.*]] = icmp ult i32 [[IV]], [[N:%.*]]
; CHECK-NEXT:    [[EARLYCND2:%.*]] = icmp ult i32 [[IV]], [[M:%.*]]
; CHECK-NEXT:    [[AND:%.*]] = and i1 [[EARLYCND]], [[EARLYCND2]]
; CHECK-NEXT:    br i1 [[AND]], label [[LATCH]], label [[EXIT:%.*]]
; CHECK:       latch:
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i32 [[IV]], 1
; CHECK-NEXT:    store volatile i32 [[IV]], i32* @A
; CHECK-NEXT:    [[C:%.*]] = icmp ult i32 [[IV_NEXT]], 1000
; CHECK-NEXT:    br i1 [[C]], label [[LOOP]], label [[EXIT]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry], [ %iv.next, %latch]
  %earlycnd = icmp ult i32 %iv, %n
  %earlycnd2 = icmp ult i32 %iv, %m
  %and = and i1 %earlycnd, %earlycnd2
  br i1 %and, label %latch, label %exit

latch:
  %iv.next = add i32 %iv, 1
  store volatile i32 %iv, i32* @A
  %c = icmp ult i32 %iv.next, 1000
  br i1 %c, label %loop, label %exit

exit:
  ret void
}


define void @unanalyzeable_latch(i32 %n) {
; CHECK-LABEL: @unanalyzeable_latch(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[EARLYCND:%.*]] = icmp ult i32 [[IV]], [[N:%.*]]
; CHECK-NEXT:    br i1 [[EARLYCND]], label [[LATCH]], label [[EXIT:%.*]]
; CHECK:       latch:
; CHECK-NEXT:    [[IV_NEXT]] = add i32 [[IV]], 1
; CHECK-NEXT:    store i32 [[IV]], i32* @A
; CHECK-NEXT:    [[VOL:%.*]] = load volatile i32, i32* @A
; CHECK-NEXT:    [[C:%.*]] = icmp ult i32 [[VOL]], 1000
; CHECK-NEXT:    br i1 [[C]], label [[LOOP]], label [[EXIT]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry], [ %iv.next, %latch]
  %earlycnd = icmp ult i32 %iv, %n
  br i1 %earlycnd, label %latch, label %exit

latch:
  %iv.next = add i32 %iv, 1
  store i32 %iv, i32* @A
  %vol = load volatile i32, i32* @A
  %c = icmp ult i32 %vol, 1000
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

define void @single_exit_no_latch(i32 %n) {
; CHECK-LABEL: @single_exit_no_latch(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[EXITCOND:%.*]] = icmp ne i32 [[IV]], [[N:%.*]]
; CHECK-NEXT:    br i1 [[EXITCOND]], label [[LATCH]], label [[EXIT:%.*]]
; CHECK:       latch:
; CHECK-NEXT:    [[IV_NEXT]] = add i32 [[IV]], 1
; CHECK-NEXT:    store i32 [[IV]], i32* @A
; CHECK-NEXT:    br label [[LOOP]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry], [ %iv.next, %latch]
  %earlycnd = icmp ult i32 %iv, %n
  br i1 %earlycnd, label %latch, label %exit

latch:
  %iv.next = add i32 %iv, 1
  store i32 %iv, i32* @A
  br label %loop

exit:
  ret void
}

; Multiple exits which could be LFTRed, but the latch itself is not an
; exiting block.
define void @no_latch_exit(i32 %n, i32 %m) {
; CHECK-LABEL: @no_latch_exit(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LATCH:%.*]] ]
; CHECK-NEXT:    [[EARLYCND:%.*]] = icmp ult i32 [[IV]], [[N:%.*]]
; CHECK-NEXT:    br i1 [[EARLYCND]], label [[CONTINUE:%.*]], label [[EXIT:%.*]]
; CHECK:       continue:
; CHECK-NEXT:    store volatile i32 [[IV]], i32* @A
; CHECK-NEXT:    [[EARLYCND2:%.*]] = icmp ult i32 [[IV]], [[M:%.*]]
; CHECK-NEXT:    br i1 [[EARLYCND2]], label [[LATCH]], label [[EXIT]]
; CHECK:       latch:
; CHECK-NEXT:    store volatile i32 [[IV]], i32* @A
; CHECK-NEXT:    [[IV_NEXT]] = add i32 [[IV]], 1
; CHECK-NEXT:    br label [[LOOP]]
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry], [ %iv.next, %latch]
  %earlycnd = icmp ult i32 %iv, %n
  br i1 %earlycnd, label %continue, label %exit

continue:
  store volatile i32 %iv, i32* @A
  %earlycnd2 = icmp ult i32 %iv, %m
  br i1 %earlycnd2, label %latch, label %exit

latch:
  store volatile i32 %iv, i32* @A
  %iv.next = add i32 %iv, 1
  br label %loop

exit:
  ret void
}
