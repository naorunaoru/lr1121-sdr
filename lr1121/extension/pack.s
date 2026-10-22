/* SPDX-License-Identifier: MIT */
.section .text
.global _start
/* 16 banks, 125 complex pairs each at 2048-byte strides; raw signed-23.
   Pack in place only after DMA invalidates a completed descriptor.
   Host rearms descriptor THEN clears ready. Never overwrite unread data.
   All changed integer registers are caller-saved; no stack or flash writes. */
_start:
    mov r9,0x80f000
    ld r0,[r9,8]
    brne r0,0,active
    b idle_marker
active:
    ld r0,[r9,36]
    cmp r0,0
    bne service
    ld r2,[r9,12]
    asl r11,r2,2
    add r11,r11,0x80f100
    asl r10,r2,4
    add r10,r10,0x80e000
    brhs r2,2,desc_ok
    asl r10,r2,4
    add r10,r10,0x800a00
desc_ok:
    ld r0,[r10]
    and r0,r0,3
    brne r0,0,kick
    asl r3,r2,11
    add r3,r3,0x806000
    ld r0,[r3]
    bbit1 r0,31,kick
    ld r8,[r9,28]
    bbit0 r8,8,peak_init
    and r8,r8,31
    ld r0,[r9,20]
    b pack_page
peak_init:
    mov r4,r3
    mov r5,0
    mov r6,250
peak_loop:
    ld.ab r7,[r4,4]
    asl r7,r7,9
    asr r7,r7,9
    abs r7,r7
    or r5,r5,r7
    sub.f r6,r6,1
    bne peak_loop
    ld r0,[r9,20]
    mov r12,2047
    brne r0,8,scale_init
    mov r12,127
scale_init:
    mov r8,0
scale_loop:
    brls r5,r12,scale_done
    lsr r5,r5,1
    add r8,r8,1
    b scale_loop
scale_done:
    b pack_page
kick:
    b service
/* Each encrypted 256-byte block has an uncontrolled first 16 bytes.
   Link at 0x94510; offsets 0xf0..0xff below must never execute. */
.org 0x100
pack_page:
    mov r10,r3
    add r4,r3,4
    mov r5,125
    add r12,r8,9
pair_loop:
    ld.ab r6,[r3,4]
    ld.ab r7,[r3,4]
    asl r6,r6,9
    asl r7,r7,9
    asr r6,r6,r12
    asr r7,r7,r12
    cmp r0,8
    beq pack8
pack12:
    stb.ab r6,[r4,1]
    lsr r6,r6,8
    and r6,r6,15
    asl r1,r7,4
    or r6,r6,r1
    stb.ab r6,[r4,1]
    asr r7,r7,4
    stb.ab r7,[r4,1]
pair_done:
    sub.f r5,r5,1
    bne pair_loop
    ld r1,[r9,16]
    add r1,r1,1
    st r1,[r9,16]
    bmsk r1,r1,23
    asl r8,r8,24
    or r8,r8,r1
    bset r8,r8,31
    st r8,[r10]
    st r8,[r11]
    add r2,r2,1
    and r2,r2,15
    st r2,[r9,12]
    b kick
