; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=thumbv8.1m.main-arm-none-eabi -mattr=+mve %s -o - | FileCheck %s

define arm_aapcs_vfpcc <4 x i32> @load_4xi32_a4(<4 x i32>* %vp) {
; CHECK-LABEL: load_4xi32_a4:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    vldrw.u32 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  %0 = load <4 x i32>, <4 x i32>* %vp, align 4
  ret <4 x i32> %0
}

define arm_aapcs_vfpcc <4 x i32> @load_4xi32_a2(<4 x i32>* %vp) {
; CHECK-LABEL: load_4xi32_a2:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    vldrh.u16 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  %0 = load <4 x i32>, <4 x i32>* %vp, align 2
  ret <4 x i32> %0
}

define arm_aapcs_vfpcc <4 x i32> @load_4xi32_a1(<4 x i32>* %vp) {
; CHECK-LABEL: load_4xi32_a1:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    vldrb.u8 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  %0 = load <4 x i32>, <4 x i32>* %vp, align 1
  ret <4 x i32> %0
}

define arm_aapcs_vfpcc void @store_4xi32_a4(<4 x i32>* %vp, <4 x i32> %val) {
; CHECK-LABEL: store_4xi32_a4:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    vstrw.32 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  store <4 x i32> %val, <4 x i32>* %vp, align 4
  ret void
}

define arm_aapcs_vfpcc void @store_4xi32_a2(<4 x i32>* %vp, <4 x i32> %val) {
; CHECK-LABEL: store_4xi32_a2:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    vstrh.16 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  store <4 x i32> %val, <4 x i32>* %vp, align 2
  ret void
}

define arm_aapcs_vfpcc void @store_4xi32_a1(<4 x i32>* %vp, <4 x i32> %val) {
; CHECK-LABEL: store_4xi32_a1:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    vstrb.8 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  store <4 x i32> %val, <4 x i32>* %vp, align 1
  ret void
}

define arm_aapcs_vfpcc <4 x i32> @load_4xi32_a4_offset_pos(i32* %ip) {
; CHECK-LABEL: load_4xi32_a4_offset_pos:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    add.w r0, r0, #508
; CHECK-NEXT:    vldrw.u32 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  %ipoffset = getelementptr inbounds i32, i32* %ip, i32 127
  %vp = bitcast i32* %ipoffset to <4 x i32>*
  %0 = load <4 x i32>, <4 x i32>* %vp, align 4
  ret <4 x i32> %0
}

define arm_aapcs_vfpcc <4 x i32> @load_4xi32_a4_offset_neg(i32* %ip) {
; CHECK-LABEL: load_4xi32_a4_offset_neg:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    sub.w r0, r0, #508
; CHECK-NEXT:    vldrw.u32 q0, [r0]
; CHECK-NEXT:    bx lr
entry:
  %ipoffset = getelementptr inbounds i32, i32* %ip, i32 -127
  %vp = bitcast i32* %ipoffset to <4 x i32>*
  %0 = load <4 x i32>, <4 x i32>* %vp, align 4
  ret <4 x i32> %0
}

define arm_aapcs_vfpcc <4 x i32> @loadstore_4xi32_stack_off16() {
; CHECK-LABEL: loadstore_4xi32_stack_off16:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    .pad #40
; CHECK-NEXT:    sub sp, #40
; CHECK-NEXT:    movs r0, #1
; CHECK-NEXT:    vdup.32 q0, r0
; CHECK-NEXT:    mov r0, sp
; CHECK-NEXT:    vstrw.32 q0, [r0]
; CHECK-NEXT:    movs r0, #3
; CHECK-NEXT:    vstrw.32 q0, [sp, #16]
; CHECK-NEXT:    str r0, [sp, #16]
; CHECK-NEXT:    vldrw.u32 q0, [sp, #16]
; CHECK-NEXT:    add sp, #40
; CHECK-NEXT:    bx lr
entry:
  %c = alloca [1 x [5 x [2 x i32]]], align 4
  %0 = bitcast [1 x [5 x [2 x i32]]]* %c to i8*
  %arrayidx5 = getelementptr inbounds [1 x [5 x [2 x i32]]], [1 x [5 x [2 x i32]]]* %c, i32 0, i32 0, i32 0, i32 0
  %1 = bitcast [1 x [5 x [2 x i32]]]* %c to <4 x i32>*
  store <4 x i32> <i32 1, i32 1, i32 1, i32 1>, <4 x i32>* %1, align 4
  %arrayidx5.2 = getelementptr inbounds [1 x [5 x [2 x i32]]], [1 x [5 x [2 x i32]]]* %c, i32 0, i32 0, i32 2, i32 0
  %2 = bitcast i32* %arrayidx5.2 to <4 x i32>*
  store <4 x i32> <i32 1, i32 1, i32 1, i32 1>, <4 x i32>* %2, align 4
  store i32 3, i32* %arrayidx5.2, align 4
  %3 = load <4 x i32>, <4 x i32>* %2, align 4
  ret <4 x i32> %3
}

define arm_aapcs_vfpcc <8 x i16> @loadstore_8xi16_stack_off16() {
; CHECK-LABEL: loadstore_8xi16_stack_off16:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    .pad #40
; CHECK-NEXT:    sub sp, #40
; CHECK-NEXT:    movs r0, #1
; CHECK-NEXT:    vdup.16 q0, r0
; CHECK-NEXT:    mov r0, sp
; CHECK-NEXT:    vstrh.16 q0, [r0]
; CHECK-NEXT:    movs r0, #3
; CHECK-NEXT:    vstrh.16 q0, [sp, #16]
; CHECK-NEXT:    strh.w r0, [sp, #16]
; CHECK-NEXT:    vldrh.u16 q0, [sp, #16]
; CHECK-NEXT:    add sp, #40
; CHECK-NEXT:    bx lr
entry:
  %c = alloca [1 x [10 x [2 x i16]]], align 2
  %0 = bitcast [1 x [10 x [2 x i16]]]* %c to i8*
  %arrayidx5 = getelementptr inbounds [1 x [10 x [2 x i16]]], [1 x [10 x [2 x i16]]]* %c, i32 0, i32 0, i32 0, i32 0
  %1 = bitcast [1 x [10 x [2 x i16]]]* %c to <8 x i16>*
  store <8 x i16> <i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1>, <8 x i16>* %1, align 2
  %arrayidx5.2 = getelementptr inbounds [1 x [10 x [2 x i16]]], [1 x [10 x [2 x i16]]]* %c, i32 0, i32 0, i32 4, i32 0
  %2 = bitcast i16* %arrayidx5.2 to <8 x i16>*
  store <8 x i16> <i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1>, <8 x i16>* %2, align 2
  store i16 3, i16* %arrayidx5.2, align 2
  %3 = load <8 x i16>, <8 x i16>* %2, align 2
  ret <8 x i16> %3
}

define arm_aapcs_vfpcc <16 x i8> @loadstore_16xi8_stack_off16() {
; CHECK-LABEL: loadstore_16xi8_stack_off16:
; CHECK:       @ %bb.0: @ %entry
; CHECK-NEXT:    .pad #40
; CHECK-NEXT:    sub sp, #40
; CHECK-NEXT:    movs r0, #1
; CHECK-NEXT:    vdup.8 q0, r0
; CHECK-NEXT:    mov r0, sp
; CHECK-NEXT:    vstrb.8 q0, [r0]
; CHECK-NEXT:    movs r0, #3
; CHECK-NEXT:    vstrb.8 q0, [sp, #16]
; CHECK-NEXT:    strb.w r0, [sp, #16]
; CHECK-NEXT:    vldrb.u8 q0, [sp, #16]
; CHECK-NEXT:    add sp, #40
; CHECK-NEXT:    bx lr
entry:
  %c = alloca [1 x [20 x [2 x i8]]], align 1
  %0 = bitcast [1 x [20 x [2 x i8]]]* %c to i8*
  %arrayidx5 = getelementptr inbounds [1 x [20 x [2 x i8]]], [1 x [20 x [2 x i8]]]* %c, i32 0, i32 0, i32 0, i32 0
  %1 = bitcast [1 x [20 x [2 x i8]]]* %c to <16 x i8>*
  store <16 x i8> <i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1>, <16 x i8>* %1, align 1
  %arrayidx5.2 = getelementptr inbounds [1 x [20 x [2 x i8]]], [1 x [20 x [2 x i8]]]* %c, i32 0, i32 0, i32 8, i32 0
  %2 = bitcast i8* %arrayidx5.2 to <16 x i8>*
  store <16 x i8> <i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1, i8 1>, <16 x i8>* %2, align 1
  store i8 3, i8* %arrayidx5.2, align 1
  %3 = load <16 x i8>, <16 x i8>* %2, align 1
  ret <16 x i8> %3
}
