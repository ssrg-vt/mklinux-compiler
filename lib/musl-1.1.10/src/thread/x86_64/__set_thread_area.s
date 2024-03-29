/* Copyright 2011-2012 Nicholas J. Kain, licensed under standard MIT license */
/* .text */

/* Pierre: -ffunction-section obviously does not work with as so let's mimic
 * its effect by hand */
.section .text.__set_thread_area, "ax"
.global __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	mov %rdi,%rsi           /* shift for syscall */
	movl $0x1002,%edi       /* SET_FS register */
	movl $158,%eax          /* set fs segment to */
	syscall                 /* arch_prctl(SET_FS, arg)*/
	ret
