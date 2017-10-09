/* Pierre: -ffunction-section obviously does not work with as so let's mimic
 * its effect by hand */
.section .text.__syscall, "ax"
.global __syscall
.hidden __syscall
.type __syscall,%function
.align 4
__syscall:
	uxtw x8,w0
	mov x0,x1
	mov x1,x2
	mov x2,x3
	mov x3,x4
	mov x4,x5
	mov x5,x6
	mov x6,x7
	svc 0
	ret
