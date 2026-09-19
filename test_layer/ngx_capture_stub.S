.text
.macro indirect name, offset
.globl \name
\name:
    .rept 20
    nop
    .endr
1:  movabs $testNgxReady, %rax
    mov (%rax), %rax
    cmp $0, %rax
    je 1b
    movabs $testNgxDispatch+\offset, %rax
    mov (%rax), %rax
    cmp $0, %rax
    jne 2f
    movabs $0xbad00002, %rax
    ret
2:  push %rax
    ret
.endm
indirect NVSDK_NGX_VULKAN_CreateFeature, 0
indirect NVSDK_NGX_VULKAN_CreateFeature1, 8
indirect NVSDK_NGX_VULKAN_EvaluateFeature, 16
indirect NVSDK_NGX_VULKAN_ReleaseFeature, 24
