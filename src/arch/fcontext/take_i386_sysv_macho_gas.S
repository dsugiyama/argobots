/*
            Copyright Oliver Kowalke 2009.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/****************************************************************************************
 *                                                                                      *
 *  ----------------------------------------------------------------------------------  *
 *  |    0    |    1    |    2    |    3    |    4     |    5    |    6    |    7    |  *
 *  ----------------------------------------------------------------------------------  *
 *  |   0x0   |   0x4   |   0x8   |   0xc   |   0x10   |   0x14  |   0x18  |   0x1c  |  *
 *  ----------------------------------------------------------------------------------  *
 *  | fc_mxcsr|fc_x87_cw|   EDI   |   ESI   |   EBX    |   EBP   |   EIP   |   EXIT  |  *
 *  ----------------------------------------------------------------------------------  *
 *                                                                                      *
 ****************************************************************************************/

.text
.globl _take_fcontext
.align 2
_take_fcontext:
    /* fourth arg of take_fcontext() == flag indicating preserving FPU */
    movl  0x10(%esp), %ecx

    /* second arg of take_fcontext() == context jumping to */
    movl  0x20(%esp), %edx

    /* third arg of take_fcontext() == value to be returned after jump */
    movl  0x24(%esp), %eax

    /* restore ESP (pointing to context-data) from EDX */
    movl  %edx, %esp

    /* test for flag preserve_fpu */
    test  %ecx, %ecx
    je  2f

    /* restore MMX control- and status-word */
    ldmxcsr  (%esp)
    /* restore x87 control-word */
    fldcw  0x4(%esp)
2:
    /* prepare stack for FPU */
    leal  0x8(%esp), %esp

    popl  %edi  /* restore EDI */
    popl  %esi  /* restore ESI */
    popl  %ebx  /* restore EBX */
    popl  %ebp  /* restore EBP */

    /* restore return-address */
    popl  %edx

    /* use value in EAX as return-value after jump */
    /* use value in EAX as first arg in context function */
    movl  %eax, 0x4(%esp)

    /* indirect jump to context */
    jmp  *%edx
