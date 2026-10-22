.include "pack.s"
.org 0x200
service:
    ld r6,[r9,36]
    cmp r6,0
    beq service_done
    st 0,[r9,36]
    cmp r6,0x800000
    blo service_done
    cmp r6,0x80ff00
    bhi service_done
    b ack_previous
service_setup:
    clri r12
    mov r0,256
    sr r0,[0x682]
    lr r0,[0x21]
    st r0,[r9,72]
    lr r0,[0x22]
    st r0,[r9,76]
    lr r0,[0x23]
    st r0,[r9,80]
    mov r0,0xffffffff
    sr r0,[0x23]
    mov r0,0
    sr r0,[0x22]
    sr r0,[0x21]
    mov r2,0xf12034
    ld r3,[r2]
    and r3,r3,0xfffffff0
    or r3,r3,1
    st r3,[r2]
    st 0,[r2,8]
    st 6,[r2,4]
    mov r4,0
preload:
    ld.ab r1,[r6,4]
    st r1,[0xf12014]
    add r4,r4,4
    brlo r4,56,preload
    b wait_progress
.org 0x300
wait_progress:
    ld r0,[0xf12004]
    bbit1 r0,9,reply_complete
    lr r0,[0x21]
    cmp r0,64000
    bhs timed_out
    brhs r4,256,wait_progress
    lr r0,[0x6a5]
    lsr r0,r0,8
    and r0,r0,0x1ffc
    mov r1,564
    sub r5,r1,r0
refill:
    brhs r4,r5,wait_progress
    ld.ab r1,[r6,4]
    st r1,[0xf12014]
    add r4,r4,4
    brlo r4,256,refill
    b wait_progress
timed_out:
    ld r0,[r9,48]
    add r0,r0,1
    st r0,[r9,48]
reply_complete:
    st r4,[r9,92]
    ld r0,[r9,80]
    sr r0,[0x23]
    ld r0,[r9,76]
    sr r0,[0x22]
    ld r0,[r9,72]
    sr r0,[0x21]
    ld r0,[r9,44]
    add r0,r0,1
    st r0,[r9,44]
    seti r12
    b service_done
.org 0x400
service_done:
    clri r0
    ld r1,[gp,0x24]
    bset r1,r1,1
    st r1,[gp,0x24]
    seti r0
    j [blink]

.org 0x500
ack_previous:
    ld r0,[r9,100]
    breq r0,0,remember_bank
    breq r0,r6,remember_bank
    ld r1,[r9,104]
    bbit0 r1,31,remember_bank
    sub r0,r0,0x806000
    lsr r0,r0,11
    asl r1,r0,4
    add r1,r1,0x80e000
    brhs r0,2,previous_desc
    asl r1,r0,4
    add r1,r1,0x800a00
previous_desc:
    ld r2,[0x800580]
    brne r2,r1,rearm_previous
    st 1,[r9,108]
    b service_done
rearm_previous:
    mov r2,0x9fe3e7a7
    st r2,[r1]
    ld r2,[r9,112]
    add r2,r2,1
    st r2,[r9,112]
remember_bank:
    st r6,[r9,100]
    ld r0,[r6]
    st r0,[r9,104]
    b service_setup

.org 0x600
pack8:
    max r6,r6,-128
    min r6,r6,127
    max r7,r7,-128
    min r7,r7,127
    stb.ab r6,[r4,1]
    stb.ab r7,[r4,1]
    b pair_done

.org 0x700
idle_marker:
    mov r1,0x49515033
    st r1,[r9]
    mov r1,0x46494631
    st r1,[r9,120]
    ld r0,[r9,4]
    st r0,[gp,0x14]
    j [blink]
