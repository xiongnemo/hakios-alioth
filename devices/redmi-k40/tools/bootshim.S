/* AArch64 Android Image shim: relocate UEFI and its XNUP payload.
 * Keep FD_BASE/FD_SIZE/PAYLOAD_BASE identical to the UEFI memory map.
 */
.text
.global _start
_start:
  adr x4, firmware
  b copy_firmware
  .quad FD_BASE
  .quad FD_SIZE
  .quad 0
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"
  .long 0
copy_firmware:
  ldr x5, =FD_BASE
  ldr x6, =FD_SIZE
  cmp x4, x5
  b.eq firmware_in_place
1:
  ldp x2, x3, [x4], #16
  stp x2, x3, [x5], #16
  subs x6, x6, #16
  b.ne 1b
  b copy_payload
firmware_in_place:
  add x4, x4, x6
copy_payload:
  ldr w2, [x4]
  ldr w3, =0x50554e58
  cmp w2, w3
  b.ne enter_firmware
  ldr w6, [x4, #4]
  /* Bound the copy to the payload reservation shared with the loader. */
  ldr w3, =0x03fff000
  cmp w6, w3
  b.hi enter_firmware
  add x6, x6, #23
  and x6, x6, #~15
  ldr x5, =PAYLOAD_BASE
2:
  ldp x2, x3, [x4], #16
  stp x2, x3, [x5], #16
  subs x6, x6, #16
  b.ne 2b
enter_firmware:
  dsb sy
  ic iallu
  dsb sy
  isb
  ldr x5, =FD_BASE
  br x5
.ltorg
.balign 16
firmware:
