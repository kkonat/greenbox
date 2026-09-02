/*
 * guest.ld - link script for a greenbox guest program.
 *
 * Two regions at two very different origins, and the distance between them is
 * the point. The loader places .text in executable IRAM and everything else in
 * DRAM, at two unrelated runtime addresses, so mkguest.py has to be able to
 * look at any word a relocation points at and say which half it belongs to.
 * Text at 0 and data at 0x10000000 makes that a range check.
 *
 * Nothing here is a real address. Every absolute reference in the output is an
 * offset that the loader fixes up.
 */

ENTRY(gb_main)

MEMORY
{
  gtext (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
  gdata (rw)  : ORIGIN = 0x10000000, LENGTH = 256K
}

SECTIONS
{
  /* Code and literal pools together: an l32r reaches its literal PC-relative,
   * so the pool has to travel with the code that loads from it. */
  .text : ALIGN(4)
  {
    *(.literal.gb_main .text.gb_main)      /* entry near the front, for eyeballing */
    *(.literal .literal.* .text .text.*)
    *(.iram1 .iram1.*)
    . = ALIGN(4);
  } > gtext

  /* .rodata lives here, NOT with the code. IRAM only tolerates aligned 32-bit
   * access and string tables get read a byte at a time. */
  .data : ALIGN(4)
  {
    *(.rodata .rodata.*)
    *(.data .data.* .sdata .sdata.*)
    . = ALIGN(4);
  } > gdata

  .bss (NOLOAD) : ALIGN(4)
  {
    *(.bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(4);
  } > gdata

  /DISCARD/ :
  {
    *(.eh_frame) *(.eh_frame_hdr)
    *(.comment) *(.note .note.*)
    *(.xt.prop .xt.prop.*) *(.xt.lit .xt.lit.*)
    *(.debug_*)
  }
}
