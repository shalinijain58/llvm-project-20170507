# NOTE: Assertions have been autogenerated by utils/update_mca_test_checks.py
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=haswell -instruction-tables < %s | FileCheck %s

cmpxchg8b  (%rax)
cmpxchg16b (%rax)
lock cmpxchg8b  (%rax)
lock cmpxchg16b (%rax)

# CHECK:      Instruction Info:
# CHECK-NEXT: [1]: #uOps
# CHECK-NEXT: [2]: Latency
# CHECK-NEXT: [3]: RThroughput
# CHECK-NEXT: [4]: MayLoad
# CHECK-NEXT: [5]: MayStore
# CHECK-NEXT: [6]: HasSideEffects (U)

# CHECK:      [1]    [2]    [3]    [4]    [5]    [6]    Instructions:
# CHECK-NEXT:  14     17    2.75    *      *            cmpxchg8b	(%rax)
# CHECK-NEXT:  19     22    4.00    *      *            cmpxchg16b	(%rax)
# CHECK-NEXT:  14     17    2.75    *      *            lock		cmpxchg8b	(%rax)
# CHECK-NEXT:  19     22    4.00    *      *            lock		cmpxchg16b	(%rax)

# CHECK:      Resources:
# CHECK-NEXT: [0]   - HWDivider
# CHECK-NEXT: [1]   - HWFPDivider
# CHECK-NEXT: [2]   - HWPort0
# CHECK-NEXT: [3]   - HWPort1
# CHECK-NEXT: [4]   - HWPort2
# CHECK-NEXT: [5]   - HWPort3
# CHECK-NEXT: [6]   - HWPort4
# CHECK-NEXT: [7]   - HWPort5
# CHECK-NEXT: [8]   - HWPort6
# CHECK-NEXT: [9]   - HWPort7

# CHECK:      Resource pressure per iteration:
# CHECK-NEXT: [0]    [1]    [2]    [3]    [4]    [5]    [6]    [7]    [8]    [9]
# CHECK-NEXT:  -      -     17.50  7.50   3.33   3.33   4.00   15.50  13.50  1.33

# CHECK:      Resource pressure by instruction:
# CHECK-NEXT: [0]    [1]    [2]    [3]    [4]    [5]    [6]    [7]    [8]    [9]    Instructions:
# CHECK-NEXT:  -      -     3.25   2.25   0.83   0.83   1.00   2.25   3.25   0.33   cmpxchg8b	(%rax)
# CHECK-NEXT:  -      -     5.50   1.50   0.83   0.83   1.00   5.50   3.50   0.33   cmpxchg16b	(%rax)
# CHECK-NEXT:  -      -     3.25   2.25   0.83   0.83   1.00   2.25   3.25   0.33   lock		cmpxchg8b	(%rax)
# CHECK-NEXT:  -      -     5.50   1.50   0.83   0.83   1.00   5.50   3.50   0.33   lock		cmpxchg16b	(%rax)
