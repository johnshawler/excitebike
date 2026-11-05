| boot.s - Zoo Keeper Amiga 1200 bootblock (KS 3.x, 68020+).  Must stay <= 1024 bytes.
|
| Entered by the OS strap with A6 = ExecBase, A1 = trackdisk IOStdReq (open, DF0:).
| 0. CPU check: 68040/68060 are refused (cyan screen) - the cache handling below is the 68020/68030 one.
| 1. AllocMem(len, MEMF_CHIP) a staging buffer (wherever the OS has room).
| 2. trackdisk CMD_READ: disk byte offset 1024 .. 1024+len  -> staging buffer.
| 3. TD_MOTOR off.
| 4. Supervisor(): mask all interrupts, finish the Blitter, switch off all DMA, clear + disable the caches, copy the staging buffer to
|    PAYLOAD_BASE (0x800) with a relocator that runs from 0x200 (memmove: ascending if dst < src, else descending), jump to 0x800.
|    The OS is never used again.
| Failure (screen turns a solid colour, machine hangs):
|    magenta = AllocMem failed (payload too big for free chip RAM), red = disk read error, cyan = 68040/68060.
|
| `payload_len` (offset 20) is patched by tools/mkdisk.py (payload size rounded up to a multiple of 512).
    
    .text
    .long   0x444F5300          | 'DOS\0'
    .long   0                   | checksum (filled in by mkadf.py)
    .long   880                 | root block
entry:                          | offset 12
    bra.s   start
    .word   0
    .long   0x5A4B4C44          | offset 16: marker 'ZKLD' (checked by mkdisk.py)
payload_len:
    .long   0                   | offset 20: patched

start:
    move.l  %a1,%a4             | a4 = IO request
    move.w  296(%a6),%d0        | ExecBase->AttnFlags: bit 3 = 68040, bit 7 = 68060
    move.w  #0x00FF,%d1         | cyan
    btst    #3,%d0
    bne     fail
    btst    #7,%d0
    bne     fail
    move.l  payload_len(%pc),%d7
    move.l  %d7,%d0
    moveq   #2,%d1              | MEMF_CHIP
    jsr     -198(%a6)           | AllocMem
    move.l  %d0,%d6             | d6 = staging buffer
    beq.s   fail_mem
    move.w  #2,28(%a4)          | io_Command = CMD_READ
    move.l  %d7,36(%a4)         | io_Length
    move.l  %d6,40(%a4)         | io_Data
    move.l  #1024,44(%a4)       | io_Offset (payload starts at ADF byte 1024)
    move.l  %a4,%a1
    jsr     -456(%a6)           | DoIO
    tst.b   31(%a4)             | io_Error
    bne.s   fail_read
    move.w  #9,28(%a4)          | TD_MOTOR
    clr.l   36(%a4)             | io_Length = 0 -> motor off
    move.l  %a4,%a1
    jsr     -456(%a6)
    lea     super(%pc),%a5
    jsr     -30(%a6)            | Supervisor(a5)  - never returns

fail_mem:
    move.w  #0x0F0F,%d1
    bra.s   fail
fail_read:
    move.w  #0x0F00,%d1
fail:
    move.w  #0x7FFF,0xDFF096    | DMA off -> screen = COLOR00
    move.w  #0x7FFF,0xDFF09A
1:  move.w  %d1,0xDFF180
    bra.s   1b

super:                          | supervisor mode, d6 = source, d7 = length
    move.w  #0x2700,%sr
    lea     0x400.w,%sp
    move.w  #0x7FFF,0xDFF09A    | INTENA off
    move.w  #0x7FFF,0xDFF09C    | INTREQ clear
    move.b  0xDFF002,%d0        | DMACONR: dummy read (BBUSY quirk)
2:  btst    #6,0xDFF002         | wait for Blitter idle (BBUSY, bit 14)
    bne.s   2b
    move.w  #0x7FFF,0xDFF096    | all DMA off
    move.w  #0x7FFF,0xDFF09C
    move.l  #0x0808,%d0         | CACR: clear I-cache (CI) and D-cache (CD, 68030), both disabled
    movec   %d0,%cacr
    move.l  %d6,%a0             | the package (stage 2 loader first) is in the staging buffer: stage2.s finds fast RAM and relocates the game there
    jmp     (%a0)

