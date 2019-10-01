# NOTE: Assertions have been autogenerated by utils/update_mca_test_checks.py
# RUN: llvm-mca -mtriple=arm-none-none-eabi -mcpu=cortex-m4 -mattr=+fp64 -instruction-tables < %s | FileCheck %s

vadd.f32 s0, s2, s2
vadd.f64 d0, d2, d2

# CHECK:      Instruction Info:
# CHECK-NEXT: [1]: #uOps
# CHECK-NEXT: [2]: Latency
# CHECK-NEXT: [3]: RThroughput
# CHECK-NEXT: [4]: MayLoad
# CHECK-NEXT: [5]: MayStore
# CHECK-NEXT: [6]: HasSideEffects (U)

# CHECK:      [1]    [2]    [3]    [4]    [5]    [6]    Instructions:
# CHECK-NEXT:  1      1     1.00                        vadd.f32	s0, s2, s2
# CHECK-NEXT:  1      1     1.00                        vadd.f64	d0, d2, d2

# CHECK:      Resources:
# CHECK-NEXT: [0]   - M4Unit

# CHECK:      Resource pressure per iteration:
# CHECK-NEXT: [0]
# CHECK-NEXT: 2.00

# CHECK:      Resource pressure by instruction:
# CHECK-NEXT: [0]    Instructions:
# CHECK-NEXT: 1.00   vadd.f32	s0, s2, s2
# CHECK-NEXT: 1.00   vadd.f64	d0, d2, d2
