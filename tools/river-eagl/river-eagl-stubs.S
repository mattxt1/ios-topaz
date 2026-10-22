/*
 * Logging trampolines for river-eagl.c. Stub i loads i into x16 and joins
 * the common path, which saves every argument register (x0-x8, q0-q7),
 * calls river_eagl_enter(i, self, _cmd, arg2, arg3) and tail-jumps to the
 * IMP it returns with all registers and the stack as the caller left them,
 * so any method signature (stack arguments, struct returns via x8, floating
 * point) passes through unchanged.
 */
#define RIVER_EAGL_STUBS 512

	.text
	.p2align 3
	.globl _river_eagl_stubs
_river_eagl_stubs:
	.set i, 0
	.rept RIVER_EAGL_STUBS
	movz	x16, #i
	b	Lcommon
	.set i, i + 1
	.endr

	.p2align 2
Lcommon:
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	sub	sp, sp, #208
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x16, [sp, #64]
	stp	q0, q1, [sp, #80]
	stp	q2, q3, [sp, #112]
	stp	q4, q5, [sp, #144]
	stp	q6, q7, [sp, #176]
	mov	x4, x3
	mov	x3, x2
	mov	x2, x1
	mov	x1, x0
	mov	x0, x16
	bl	_river_eagl_enter
	mov	x17, x0
	ldp	q6, q7, [sp, #176]
	ldp	q4, q5, [sp, #144]
	ldp	q2, q3, [sp, #112]
	ldp	q0, q1, [sp, #80]
	ldp	x8, x16, [sp, #64]
	ldp	x6, x7, [sp, #48]
	ldp	x4, x5, [sp, #32]
	ldp	x2, x3, [sp, #16]
	ldp	x0, x1, [sp, #0]
	add	sp, sp, #208
	ldp	x29, x30, [sp], #16
	br	x17
