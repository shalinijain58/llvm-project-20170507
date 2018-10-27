# NOTE: Assertions have been autogenerated by utils/update_mca_test_checks.py
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=x86-64 -timeline -timeline-max-iterations=3 < %s | FileCheck %s

add %eax, %ecx
add %eax, %edx
add %eax, %ebx
add %edx, %esi
add %ebx, %eax
add %edx, %esi
add %ebx, %eax
add %ebx, %eax

# CHECK:      Iterations:        100
# CHECK-NEXT: Instructions:      800
# CHECK-NEXT: Total Cycles:      403
# CHECK-NEXT: Total uOps:        800

# CHECK:      Dispatch Width:    4
# CHECK-NEXT: uOps Per Cycle:    1.99
# CHECK-NEXT: IPC:               1.99
# CHECK-NEXT: Block RThroughput: 2.7

# CHECK:      Instruction Info:
# CHECK-NEXT: [1]: #uOps
# CHECK-NEXT: [2]: Latency
# CHECK-NEXT: [3]: RThroughput
# CHECK-NEXT: [4]: MayLoad
# CHECK-NEXT: [5]: MayStore
# CHECK-NEXT: [6]: HasSideEffects (U)

# CHECK:      [1]    [2]    [3]    [4]    [5]    [6]    Instructions:
# CHECK-NEXT:  1      1     0.33                        addl	%eax, %ecx
# CHECK-NEXT:  1      1     0.33                        addl	%eax, %edx
# CHECK-NEXT:  1      1     0.33                        addl	%eax, %ebx
# CHECK-NEXT:  1      1     0.33                        addl	%edx, %esi
# CHECK-NEXT:  1      1     0.33                        addl	%ebx, %eax
# CHECK-NEXT:  1      1     0.33                        addl	%edx, %esi
# CHECK-NEXT:  1      1     0.33                        addl	%ebx, %eax
# CHECK-NEXT:  1      1     0.33                        addl	%ebx, %eax

# CHECK:      Resources:
# CHECK-NEXT: [0]   - SBDivider
# CHECK-NEXT: [1]   - SBFPDivider
# CHECK-NEXT: [2]   - SBPort0
# CHECK-NEXT: [3]   - SBPort1
# CHECK-NEXT: [4]   - SBPort4
# CHECK-NEXT: [5]   - SBPort5
# CHECK-NEXT: [6.0] - SBPort23
# CHECK-NEXT: [6.1] - SBPort23

# CHECK:      Resource pressure per iteration:
# CHECK-NEXT: [0]    [1]    [2]    [3]    [4]    [5]    [6.0]  [6.1]
# CHECK-NEXT:  -      -     2.66   2.67    -     2.67    -      -

# CHECK:      Resource pressure by instruction:
# CHECK-NEXT: [0]    [1]    [2]    [3]    [4]    [5]    [6.0]  [6.1]  Instructions:
# CHECK-NEXT:  -      -     0.33   0.33    -     0.34    -      -     addl	%eax, %ecx
# CHECK-NEXT:  -      -     0.33   0.34    -     0.33    -      -     addl	%eax, %edx
# CHECK-NEXT:  -      -     0.34   0.33    -     0.33    -      -     addl	%eax, %ebx
# CHECK-NEXT:  -      -     0.33   0.33    -     0.34    -      -     addl	%edx, %esi
# CHECK-NEXT:  -      -     0.33   0.34    -     0.33    -      -     addl	%ebx, %eax
# CHECK-NEXT:  -      -     0.34   0.33    -     0.33    -      -     addl	%edx, %esi
# CHECK-NEXT:  -      -     0.33   0.33    -     0.34    -      -     addl	%ebx, %eax
# CHECK-NEXT:  -      -     0.33   0.34    -     0.33    -      -     addl	%ebx, %eax

# CHECK:      Timeline view:
# CHECK-NEXT:                     01234
# CHECK-NEXT: Index     0123456789

# CHECK:      [0,0]     DeER .    .   .   addl	%eax, %ecx
# CHECK-NEXT: [0,1]     DeER .    .   .   addl	%eax, %edx
# CHECK-NEXT: [0,2]     DeER .    .   .   addl	%eax, %ebx
# CHECK-NEXT: [0,3]     D=eER.    .   .   addl	%edx, %esi
# CHECK-NEXT: [0,4]     .DeER.    .   .   addl	%ebx, %eax
# CHECK-NEXT: [0,5]     .D=eER    .   .   addl	%edx, %esi
# CHECK-NEXT: [0,6]     .D=eER    .   .   addl	%ebx, %eax
# CHECK-NEXT: [0,7]     .D==eER   .   .   addl	%ebx, %eax
# CHECK-NEXT: [1,0]     . D==eER  .   .   addl	%eax, %ecx
# CHECK-NEXT: [1,1]     . D==eER  .   .   addl	%eax, %edx
# CHECK-NEXT: [1,2]     . D==eER  .   .   addl	%eax, %ebx
# CHECK-NEXT: [1,3]     . D===eER .   .   addl	%edx, %esi
# CHECK-NEXT: [1,4]     .  D==eER .   .   addl	%ebx, %eax
# CHECK-NEXT: [1,5]     .  D===eER.   .   addl	%edx, %esi
# CHECK-NEXT: [1,6]     .  D===eER.   .   addl	%ebx, %eax
# CHECK-NEXT: [1,7]     .  D====eER   .   addl	%ebx, %eax
# CHECK-NEXT: [2,0]     .   D====eER  .   addl	%eax, %ecx
# CHECK-NEXT: [2,1]     .   D====eER  .   addl	%eax, %edx
# CHECK-NEXT: [2,2]     .   D====eER  .   addl	%eax, %ebx
# CHECK-NEXT: [2,3]     .   D=====eER .   addl	%edx, %esi
# CHECK-NEXT: [2,4]     .    D====eER .   addl	%ebx, %eax
# CHECK-NEXT: [2,5]     .    D=====eER.   addl	%edx, %esi
# CHECK-NEXT: [2,6]     .    D=====eER.   addl	%ebx, %eax
# CHECK-NEXT: [2,7]     .    D======eER   addl	%ebx, %eax

# CHECK:      Average Wait times (based on the timeline view):
# CHECK-NEXT: [0]: Executions
# CHECK-NEXT: [1]: Average time spent waiting in a scheduler's queue
# CHECK-NEXT: [2]: Average time spent waiting in a scheduler's queue while ready
# CHECK-NEXT: [3]: Average time elapsed from WB until retire stage

# CHECK:            [0]    [1]    [2]    [3]
# CHECK-NEXT: 0.     3     3.0    0.3    0.0       addl	%eax, %ecx
# CHECK-NEXT: 1.     3     3.0    0.3    0.0       addl	%eax, %edx
# CHECK-NEXT: 2.     3     3.0    0.3    0.0       addl	%eax, %ebx
# CHECK-NEXT: 3.     3     4.0    0.0    0.0       addl	%edx, %esi
# CHECK-NEXT: 4.     3     3.0    0.0    0.0       addl	%ebx, %eax
# CHECK-NEXT: 5.     3     4.0    0.0    0.0       addl	%edx, %esi
# CHECK-NEXT: 6.     3     4.0    0.0    0.0       addl	%ebx, %eax
# CHECK-NEXT: 7.     3     5.0    0.0    0.0       addl	%ebx, %eax
