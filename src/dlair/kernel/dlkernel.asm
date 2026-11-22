;=========================================================================
; dlkernel - the ColecoVision side of Dragon's Lair on the rev C cart.
;
; Movies' packet engine (megademo3.asm: a Graphics II picture and two PSG
; samples a packet, every packet 438 T, 614 T with the console's PSG waits)
; fed with cvframes lists: each list is one field of the arcade's laserdisc,
; 97 packets (194 samples, 11,660 Hz on the console), ending with KEYS (the
; controller, reported in the address) and END (take the next list at $C000
; and start it). A list that comes round again plays again: its writes are
; absolute, so only its sound repeats.
;
;   packet = A, opcode, ..., B, ...      A, B: 3 PSG bytes each
;     0 ADDR  A 0 lo hi|$40 d0 d1 d2 B d3 d4 d5 d6 d7    start a VRAM run
;     1 IDLE  A 1 B                                      nothing to draw
;     2 CONT  A 2 d0 d1 d2 B d3 d4 d5 d6 d7              continue the run
;     3 KEYS  A 3 B         read the controller: $FFC0+dirs, $FFE0+key,
;                           $FFD0+fires (cvframes.h)
;     4 REG   A 4 v r|$80 B VDP register r = v (an ADDR must follow before
;                           a CONT: the write moves the VDP's address)
;     5 END   A 5 B         $FFF0+seq takes the next list; back to $C000
;
;   every path: 438 T, sample A at 0, sample B at 219 (test/dlcycles.py);
;   the three writes of a sample are 18 T apart; VRAM writes >= 28 actual T
;   apart.
;
; At the start the kernel clears VRAM (patterns and colours 0, names 0..255
; three times, sprites off) and waits for a list whose first byte is a PSG
; volume byte (bit 7 set): before the cart's first list $C000 holds zeros.
;
;   sjasmplus --nologo --sym=dlkernel.sym dlkernel.asm -> dlkernel.bin (16K)
;
; With -DDIAG (the plugin's diagnostic build) the backdrop shows how far it
; got: dark blue once the registers are set (no list from the cart yet),
; magenta when the first list is taken. The lists set it after that.
;=========================================================================
                DEVICE  NOSLOT64K

PSG             EQU     0xFF
VDP_DATA        EQU     0xBE
VDP_CTRL        EQU     0xBF
KEY_MODE        EQU     0x80            ; out: the controllers read the keypad
JOY_MODE        EQU     0xC0            ; out: the controllers read the joystick
CTRL1           EQU     0xFC
STACK_TOP       EQU     0x7400
SEQ             EQU     0x7000          ; lists taken, mod 16
WINDOW          EQU     0xC000

                ORG     0x8000
                DB      0x55,0xAA               ; skip the BIOS title screen
                DW      0,0,0,0
                DW      start
                DUP     7
                ret
                nop
                nop
                EDUP
                retn
                nop
                DB      "DLAIR AV2/2026",0

start:          di
                ld      sp,STACK_TOP
                in      a,(VDP_CTRL)            ; the VDP's write latch to its first byte
                xor     a
                ld      i,a
                im      1
                ld      (SEQ),a
                call    psg_init
                call    vdp_init
                out     (JOY_MODE),a

; the first list: take until $C000 starts with a PSG byte
first:          ld      a,(SEQ)
                inc     a
                ld      (SEQ),a
                and     0x0F
                or      0xF0
                ld      l,a
                ld      h,0xFF
                ld      a,(hl)                  ; take
                ld      a,(WINDOW)
                or      a
                jp      m,restart
                ld      b,0                     ; ~1 ms, then again
1:              djnz    1B
                jr      first

restart:
                IFDEF   DIAG
                ld      a,13                    ; magenta: the first list is here
                ld      e,7
                call    vdp_reg
                ENDIF
                ld      hl,WINDOW
                ld      c,PSG                   ; every packet path leaves C=PSG

;-------------------------------------------------------------------------
period:         outi                            ; sample A: ch2
                outi                            ; ch1
                outi                            ; ch0                 ...54 T
opread:         ld      a,(hl)                  ; opcode
                inc     hl
                or      a
                jp      z,p_addr                ; op 0 arrives at 85 T
                dec     a
                jp      z,p_idle                ; op 1 at 101
                dec     a
                jp      z,p_cont                ; op 2 at 117
                dec     a
                jp      z,p_keys                ; op 3 at 133
                dec     a
                jp      z,p_reg                 ; op 4 at 149
                jp      p_end                   ; op 5 at 160

; sample B at 219 T on every path, "jp period" ending at 438

                MACRO   SAMPLE_B_AND_5          ; 219 -> 427
                outi                            ; sample B          219
                outi
                outi
                ld      c,VDP_DATA
                DUP     4
                outi                            ; d3..d6
                nop
                nop
                EDUP
                outi                            ; d7
                ld      c,PSG
                ld      a,0                     ; 8 T
                ENDM

p_addr:         ld      c,VDP_CTRL              ; 85
                outi                            ; address low
                outi                            ; address high, $40 pre-set
                ld      c,VDP_DATA              ; setting a write address is
                DUP     2                       ; not a VRAM access
                outi                            ; d0, d1
                nop
                nop
                EDUP
                outi                            ; d2
                ld      c,PSG                   ; 219
                SAMPLE_B_AND_5
                jp      period

p_cont:         ld      c,VDP_DATA              ; 117
                DUP     2
                outi                            ; d0, d1
                nop
                nop
                EDUP
                outi                            ; d2
                ld      c,PSG
                inc     ix                      ; 12 T              -> 219
                SAMPLE_B_AND_5
                jp      period

p_idle:         ld      b,7                     ; 101
1:              djnz    1B
                nop
                inc     ix                      ; -> 219
                outi                            ; sample B
                outi
                outi
                ld      b,9
1:              djnz    1B
                nop
                nop
                nop
                nop
                nop                             ; -> 427
                jp      period

; the controller: the joystick (and left fire) now, the keypad (and right
; fire) after sample B, then the three reports
p_keys:         in      a,(CTRL1)               ; 133: bits 0-3 N E S W, 6 left fire
                ld      e,a
                out     (KEY_MODE),a
                and     0x0F
                or      0xC0
                ld      d,a                     ; d: the joystick's report
                ld      a,e
                rlca
                rlca
                and     1
                ld      e,a                     ; e: left fire at bit 0
                ld      b,0                     ; -> 219
                outi                            ; sample B
                outi
                outi
                in      a,(CTRL1)               ; bits 0-3 the key, 6 right fire
                ld      b,0xFF                  ; the reports are reads of $FFxx
                ld      c,d
                ld      d,a
                ld      a,(bc)                  ; report the joystick
                ld      a,d
                and     0x0F
                or      0xE0
                ld      c,a
                ld      a,(bc)                  ; report the keypad
                ld      a,d
                rlca
                rlca
                rlca
                and     2
                or      e
                or      0xD0
                ld      c,a
                ld      a,(bc)                  ; report the fires
                out     (JOY_MODE),a
                ld      c,PSG
                ld      b,0                     ; -> 427
                jp      period

; a VDP register: two control writes
p_reg:          ld      c,VDP_CTRL              ; 149
                outi                            ; the value
                nop
                nop
                outi                            ; $80 | the register
                ld      c,PSG
                ld      b,0                     ; -> 219
                outi                            ; sample B
                outi
                outi
                ld      b,9
1:              djnz    1B
                nop
                nop
                nop
                nop
                nop                             ; -> 427
                jp      period

; the end of the list: take the next (after sample B, the last read of
; this one) and start it
p_end:          ld      a,(SEQ)                 ; 160
                inc     a
                ld      (SEQ),a
                and     0x0F
                or      0xF0
                ld      e,a
                nop                             ; -> 219
                outi                            ; sample B
                outi
                outi
                ld      d,0xFF
                ex      de,hl
                ld      a,(hl)                  ; take
                ld      hl,WINDOW
                ld      b,7
1:              djnz    1B
                ld      b,0
                ld      b,0
                nop                             ; -> 427
                jp      period

;=========================================================================
vdp_reg:        out     (VDP_CTRL),a
                ld      a,e
                or      0x80
                nop
                nop
                out     (VDP_CTRL),a
                ret

vdp_addr_write: ld      a,l
                out     (VDP_CTRL),a
                ld      a,h
                or      0x40
                nop
                out     (VDP_CTRL),a
                ret

; Graphics II as Movies: patterns $0000, colours $2000, names $1800 (0..255
; three times), sprites off; the display blanked while VRAM is cleared
vdp_init:       ld      hl,regs
                ld      e,0
1:              ld      a,(hl)
                inc     hl
                call    vdp_reg
                inc     e
                ld      a,e
                cp      8
                jr      nz,1B
                ld      hl,0x0000               ; patterns and colours (and names) to 0
                call    vdp_addr_write
                ld      bc,0x4000
2:              xor     a
                out     (VDP_DATA),a
                dec     bc
                ld      a,b
                or      c
                jr      nz,2B
                ld      hl,0x1B00
                call    vdp_addr_write
                ld      a,0xD0
                out     (VDP_DATA),a
                ld      hl,0x1800
                call    vdp_addr_write
                ld      d,3
3:              ld      e,0
4:              ld      a,e
                out     (VDP_DATA),a
                nop
                nop
                nop
                inc     e
                jr      nz,4B
                dec     d
                jr      nz,3B
                ld      a,0xC0                  ; the display on
                ld      e,1
                jp      vdp_reg

                IFDEF   DIAG
regs:           DB      0x02,0x80,0x06,0xFF,0x03,0x36,0x07,0x04     ; dark blue: the kernel runs
                ELSE
regs:           DB      0x02,0x80,0x06,0xFF,0x03,0x36,0x07,0x01     ; a black backdrop
                ENDIF

psg_init:       ld      hl,pseq
                ld      b,10
1:              push    bc
                ld      a,(hl)
                inc     hl
                out     (PSG),a
                ld      b,4
2:              djnz    2B
                pop     bc
                djnz    1B
                ret
pseq:           DB      0x9F,0xBF,0xDF,0xFF
                DB      0x81,0x00
                DB      0xA1,0x00
                DB      0xC1,0x00

codeend:
                ORG     0xBFFF
                DB      0xFF
                SAVEBIN "dlkernel.bin",0x8000,0x4000
