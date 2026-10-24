/* SPDX-License-Identifier: MIT
 * Transceiver 1.3, module-bound research player. Preserves original SDR prefix.
 * S=80f400. Two 4096-byte buffers at80c000/80d000, signed16 offsets.
 * Mode2=timer-only, mode3=native bounded TX + Timer0 frequency playback.
 * S+64=preloaded cyclic flag; +68=reserved (formerly log pointer).
 * S+72/76=phases at maximum consistent foreground observation;
 * +84=max complete foreground interruption span, +88=preceding IRQ sequence;
 * +120=multi-IRQ observations, +124=min no-IRQ observation span;
 * +128/132=ready block sequence+1, +136=checksum, +140/144=RTC start/last;
 * +108=center frequency word, +160=offset shift 1..15; fixed per session;
 * +148=total ring records, +152=native return, +156=IRQ count at maximum observation span.
 * Ring80e000:128x32 bytes {index,entry,exit,RTC,observation_phase,word,readback,CTRL0}.
 * Bounded <=3200000 updates, native timeout <=60s. Independent Pico alarm.
 */
.include "stream.s"
.org 0x800
.global paired_start
paired_start:
 st.aw blink,[sp,-4]
 mov r9,0x80f400
 ld r0,[r9]
 brhi r0,3,p_start_return
 brlo r0,2,p_start_return
 ld r1,[r9,4]
 breq r1,0,p_start_return
 cmp r1,1966080
 bhi p_start_return
 ld r1,[r9,8]
 breq r1,0,p_start_return
 cmp r1,3200000
 bhi p_start_return
 ld r1,[r9,12]
 cmp r1,80
 blo p_start_return
 ld r0,[r9]
 breq r0,2,p_started
 mov r0,2
 ld r1,[r9,4]
 mov r2,0x904e8
 jl [r2]
 mov r9,0x80f400
 st r0,[r9,152]
p_started:
 mov r9,0x80f400
 ld r0,[gp,0x14]
 cmp r0,paired_start
 bne p_save_cb
 ld r0,[r9,16]
p_save_cb:
 st r0,[r9,16]
 mov r0,0x46535435
 st r0,[r9,92]
 b p_init
p_start_return:
 ld.ab blink,[sp,4]
 mov r0,0
 j [blink]
.org 0x8b0
/* Two consistent (IRQ epoch, Timer0 phase) snapshots. The eight NOPs
 * give a finite observation window; its no-interrupt cost is calibrated.
 * Reject snapshots straddling an IRQ or too close to an unserviced wrap.
 */
p_observe:
 ld r2,[r9,100]
 lr r0,[0x21]
 ld r4,[r9,100]
 brne r2,r4,p_observe_reject
 nop_s
 nop_s
 nop_s
 nop_s
 nop_s
 nop_s
 nop_s
 nop_s
 ld r4,[r9,100]
 lr r1,[0x21]
 ld r3,[r9,100]
 brne r4,r3,p_observe_reject
 brlo r0,16,p_observe_reject
 brlo r1,16,p_observe_reject
 b p_observe_result
p_observe_reject:
 j [blink]
.org 0x900
p_init:
 lr r0,[0x22]
 st r0,[r9,32]
 lr r0,[0x23]
 st r0,[r9,36]
 lr r0,[0x21]
 st r0,[r9,40]
 lr r0,[0x25]
 st r0,[r9,44]
 lr r0,[0xe]
 st r0,[r9,96]
 mov r0,p_service
 st r0,[gp,0x14]
 st 1,[r9,80]
 b p_irq_setup
p_kick:
 clri r0
 ld r1,[gp,0x24]
 bset r1,r1,1
 st r1,[gp,0x24]
 seti r0
 j [blink]
/* Sampled ring records: index, entry phase, exit phase, RTC,
 * post-write observation phase, frequency, readback, end CONTROL0.
 * Registers r0-r3/r9 only; blink and r4-r7 are untouched by the ISR.
 * Trace exit phase is captured before restoring registers.
 * Timer0 is the only implemented ARC timer (TIMER_BUILD=10104).
 * Entry/exit modulo timestamps plus IP flag identify crossing a period.
 */
.org 0x980
p_log:
 ld r2,[r9,148]
 and r3,r2,127
 asl r3,r3,5
 add r3,r3,0x80e000
 st r0,[r3]
 add r2,r2,1
 st r2,[r9,148]
 ld r2,[sp,44]
 st r2,[r3,4]
 lr r2,[0x21]
 st r2,[r3,16]
 st r1,[r3,20]
 ld r2,[0xf30058]
 st r2,[r3,24]
 ld r2,[0xf04014]
 st r2,[r3,12]
 b p_irq_logged
.org 0xa00
p_irq_setup:
 clri r8
 lr r0,[0x40b]
 st r0,[r9,60]
 mov r0,16
 sr r0,[0x40b]
 lr r0,[0x40c]
 st r0,[r9,48]
 lr r0,[0x206]
 st r0,[r9,52]
 lr r0,[0x40d]
 st r0,[r9,56]
 mov r0,0
 sr r0,[0x22]
 sr r0,[0x40d]
 mov r0,0
 sr r0,[0x206]
 lr r0,[0x206]
 st r0,[r9,116]
 mov r0,1
 sr r0,[0x40c]
 mov r0,0x96000
 sr r0,[0x25]
 lr r0,[0x25]
 st r0,[r9,112]
 ld r0,[r9,12]
 sub r0,r0,1
 sr r0,[0x23]
 mov r0,0
 sr r0,[0x21]
 ld r0,[0xf04014]
 st r0,[r9,140]
 mov r0,1
 sr r0,[0x22]
 ld r0,[r9,60]
 sr r0,[0x40b]
 seti r8
 bl p_kick
 b p_start_return

/* Full IVT is a separately written fragment at 96000. */
.org 0xb00
.global diagnostic
diagnostic:
 ld r0,[0x80f500]
 breq r0,1,diag_fault
 mov r0,0x96000
 mov r1,0x80d000
 mov r2,60
copy_vectors:
 ld.ab r3,[r0,4]
 st.ab r3,[r1,4]
 sub.f r2,r2,1
 bne copy_vectors
 lr r0,[0x208]
 st r0,[0x80f504]
 lr r0,[0x18]
 st r0,[0x80f508]
 st 0,[gp,0x14]
 mov r0,0x46535435
 st r0,[0x80f500]
 mov r0,0
 j [blink]
diag_fault:
 mov r0,0x96000
 sr r0,[0x25]
 unimp_s
 j [blink]
.org 0xb80
.global fault_handler
fault_handler:
 clri r0
 lr r0,[0x403]
 st r0,[0x80f510]
 lr r0,[0x400]
 st r0,[0x80f514]
 lr r0,[0x25]
 st r0,[0x80f518]
 mov r0,0x45584350
 st r0,[0x80f51c]
 mov r0,0
 sr r0,[0x22]
 mov r0,0x84000
 sr r0,[0x25]
 mov r0,0x928f8
 j [r0]
p_underrun:
 st 4,[r9,28]
 b p_irq_stop
.org 0xc00
p_pair:
 ld r0,[r9,20]
 ld r1,[r9,8]
 cmp r0,r1
 bhs p_pair_done
 bmsk r2,r0,10
 brne r2,0,p_buffer_ready
 ld r2,[r9,64]
 brne r2,0,p_buffer_ready
 lsr r2,r0,11
 and r3,r2,1
 asl r3,r3,2
 add r3,r3,128
 ld r1,[r9,r3]
 add r2,r2,1
 brne r2,r1,p_underrun
p_buffer_ready:
 and r3,r0,4095
 asl r3,r3,1
 add r3,r3,0x80c000
 ldh.x r1,[r3]
 ld r2,[r9,160]
 asl r1,r1,r2
 ld r2,[r9,108]
 add r1,r1,r2
 st r1,[0xf30058]
 sync
 st r1,[r9,104]
 ld r2,[r9,136]
 add r2,r2,r1
 st r2,[r9,136]
 add r0,r0,1
 st r0,[r9,20]
 bmsk r2,r0,10
 cmp r2,0
 beq p_log
 b p_irq_return
.org 0xc98
p_pair_done:
 ld r0,[0xf04014]
 st r0,[r9,144]
 st 2,[r9,28]
 b p_irq_stop
.org 0xd00
p_irq:
 st.aw r0,[sp,-48]
 lr r0,[0x21]
 st r0,[sp,44]
 st r1,[sp,4]
 st r2,[sp,8]
 st r3,[sp,12]
 st r9,[sp,36]
 mov r9,0x80f400
 mov r0,1
 sr r0,[0x22]
 ld r0,[r9,100]
 add r0,r0,1
 st r0,[r9,100]
 b p_pair
p_irq_logged:
 lr r2,[0x22]
 st r2,[r3,28]
 lr r2,[0x21]
 st r2,[r3,8]
 b p_irq_return
p_irq_stop:
 mov r0,0
 sr r0,[0x22]
p_irq_return:
 ld r9,[sp,36]
 ld r3,[sp,12]
 ld r2,[sp,8]
 ld r1,[sp,4]
 ld.ab r0,[sp,48]
 rtie
.org 0xe00
p_cleanup:
 ld r0,[r9,80]
 breq r0,0,p_cleanup_return
 clri r8
 mov r0,0
 sr r0,[0x22]
 ld r0,[r9]
 cmp r0,2
 blo p_restore_timer
 lr r0,[0x40b]
 mov r1,16
 sr r1,[0x40b]
 ld r1,[r9,48]
 sr r1,[0x40c]
 ld r1,[r9,52]
 sr r1,[0x206]
 ld r1,[r9,56]
 sr r1,[0x40d]
 sr r0,[0x40b]
 ld r0,[r9,44]
 sr r0,[0x25]
p_restore_timer:
 ld r0,[r9,36]
 sr r0,[0x23]
 ld r0,[r9,40]
 sr r0,[0x21]
 ld r0,[r9,32]
 sr r0,[0x22]
 st 0,[r9,80]
 ld r0,[gp,0x14]
 cmp r0,p_service
 bne p_restored
 ld r0,[r9,16]
 st r0,[gp,0x14]
p_restored:
 seti r8
p_cleanup_return:
 j [blink]
p_service_return:
 mov r0,r13
 ld blink,[sp,4]
 ld.ab r13,[sp,8]
 j [blink]
.org 0xf00
p_service:
 st.aw r13,[sp,-8]
 st blink,[sp,4]
 mov r13,r0
 mov r9,0x80f400
 ld r0,[r9,28]
 brne r0,0,p_cleanup_first
 and r0,r13,0x404
 breq r0,0,p_forward
 st 1,[r9,28]
p_cleanup_first:
 bl p_cleanup
p_forward:
 mov r9,0x80f400
 ld r1,[r9,16]
 breq r1,0,p_after_stock
 mov r0,r13
 jl [r1]
 mov r13,r0
p_after_stock:
 mov r9,0x80f400
 bl p_observe
 ld r0,[r9,24]
 add r0,r0,1
 st r0,[r9,24]
 ld r0,[r9,80]
 cmp r0,0
 beq p_service_return
p_service_kick:
 bl p_kick
 b p_service_return


.org 0xf80
p_observe_result:
 sub r3,r3,r2
 brne r3,0,p_observe_event
 sub r3,r1,r0
 ld r4,[r9,124]
 breq r4,0,p_observe_baseline
 cmp r3,r4
 bhs p_observe_return
p_observe_baseline:
 st r3,[r9,124]
p_observe_return:
 j [blink]
p_observe_event:
 mov r5,r3
 breq r3,1,p_observe_duration
 ld r4,[r9,120]
 add r4,r4,1
 st r4,[r9,120]
p_observe_duration:
 ld r4,[r9,12]
 mpy r3,r3,r4
 sub r4,r1,r0
 add r3,r3,r4
 ld r4,[r9,84]
 cmp r3,r4
 bls p_observe_return
 st r3,[r9,84]
 st r2,[r9,88]
 st r0,[r9,72]
 st r1,[r9,76]
 st r5,[r9,156]
 j [blink]
