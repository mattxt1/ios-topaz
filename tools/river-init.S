/*
 * river eMMC test init (PID 1), derived from sweet-init.S: after the usual
 * greeting it reads the first 512 bytes of boot_b (GPT entry 42) through
 * the read-only character and block devices of bsd/dev/river_emmc.c, prints
 * their first 8 bytes ("ANDROID!" expected), checks that opening the disk
 * for writing is refused, then keeps the 5 s heartbeat.
 *
 * Same rules as sweet-init.S: position independent, no relocations, no
 * writable data (tools/mkinit.py). Raw XNU syscalls: number in x16,
 * svc #0x80, carry set on error.
 */
#define SYS_read	3
#define SYS_write	4
#define SYS_open	5
#define SYS_close	6
#define SYS_getpid	20
#define SYS_select	93
#define O_RDONLY	0
#define O_RDWR		2

	.text
	.globl	_start
	.p2align 2
_start:
	mov	x9, sp
	and	x9, x9, #~15
	mov	sp, x9

	adr	x0, console
	mov	x1, #O_RDWR
	mov	x2, #0
	mov	x16, #SYS_open
	svc	#0x80
	b.cs	hang
	mov	x19, x0

	adr	x1, hello
	adr	x2, hello_tail
	sub	x2, x2, x1
	bl	write_console
	mov	x16, #SYS_getpid
	svc	#0x80
	add	w0, w0, #'0'
	sub	sp, sp, #16
	strb	w0, [sp]
	mov	x1, sp
	mov	x2, #1
	bl	write_console
	add	sp, sp, #16
	adr	x1, hello_tail
	adr	x2, raw_path
	sub	x2, x2, x1
	bl	write_console

	sub	sp, sp, #528		/* 512-byte buffer, 16-aligned */
	adr	x22, raw_path
	bl	read_dev
	adr	x22, blk_path
	bl	read_dev

	/* Opening for writing must fail: the driver returns EROFS. */
	adr	x0, raw_path
	mov	x1, #O_RDWR
	mov	x2, #0
	mov	x16, #SYS_open
	svc	#0x80
	b.cs	1f
	mov	x16, #SYS_close
	svc	#0x80
	adr	x1, wr_bad
	adr	x2, wr_bad_end
	b	2f
1:	adr	x1, wr_ok
	adr	x2, wr_bad
2:	sub	x2, x2, x1
	bl	write_console

tick:
	mov	x0, #0
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	adr	x4, five_sec
	mov	x16, #SYS_select
	svc	#0x80
	adr	x1, alive
	adr	x2, alive_end
	sub	x2, x2, x1
	bl	write_console
	b	tick

/* Print "<path>: <first 8 bytes>\n" for the device at x22; buffer at sp. */
read_dev:
	mov	x23, x30
	mov	x0, x22
	bl	strlen_x0
	mov	x2, x0
	mov	x1, x22
	bl	write_console
	mov	x0, x22
	mov	x1, #O_RDONLY
	mov	x2, #0
	mov	x16, #SYS_open
	svc	#0x80
	b.cs	3f
	mov	x20, x0
	mov	x1, sp
	mov	x2, #512
	mov	x16, #SYS_read
	svc	#0x80
	b.cs	4f
	cmp	x0, #512
	b.ne	4f
	adr	x1, got
	adr	x2, got_end
	sub	x2, x2, x1
	bl	write_console
	mov	x1, sp
	mov	x2, #8
	bl	write_console
	adr	x1, newline
	mov	x2, #1
	bl	write_console
	mov	x0, x20
	mov	x16, #SYS_close
	svc	#0x80
	mov	x30, x23
	ret
3:	adr	x1, open_fail
	adr	x2, read_fail
	b	5f
4:	mov	x0, x20
	mov	x16, #SYS_close
	svc	#0x80
	adr	x1, read_fail
	adr	x2, read_fail_end
5:	sub	x2, x2, x1
	bl	write_console
	mov	x30, x23
	ret

/* x0 = strlen(x0) */
strlen_x0:
	mov	x1, x0
6:	ldrb	w2, [x1], #1
	cbnz	w2, 6b
	sub	x0, x1, x0
	sub	x0, x0, #1
	ret

/* write(x19, x1, x2) */
write_console:
	mov	x0, x19
	mov	x16, #SYS_write
	svc	#0x80
	ret

hang:
	b	hang

	.p2align 3
five_sec:
	.quad	5
	.quad	0
console:
	.asciz	"/dev/console"
hello:
	.ascii	"\n*** RIVER userspace: eMMC test init running as PID "
hello_tail:
	.ascii	" ***\n"
raw_path:
	.asciz	"/dev/remmc0s42"
blk_path:
	.asciz	"/dev/emmc0s42"
got:
	.ascii	": first bytes "
got_end:
newline:
	.ascii	"\n"
open_fail:
	.ascii	": open failed\n"
read_fail:
	.ascii	": read failed\n"
read_fail_end:
wr_ok:
	.ascii	"RIVER init: open for writing refused (read-only) OK\n"
wr_bad:
	.ascii	"RIVER init: OPEN FOR WRITING WAS ALLOWED\n"
wr_bad_end:
alive:
	.ascii	"RIVER init: still alive\n"
alive_end:
