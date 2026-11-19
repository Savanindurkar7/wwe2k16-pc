/**
 * @file        tests/unit/codegen/jump_table_bias_test.cpp
 * @brief       Regression test for the bctr jump-table index-bias / union-window fix
 *
 * Root cause: the compiler bounds the raw index
 * with `clrlwi r7,r9,30` and then adds a constant bias with
 * `addi r7,r7,7` before scaling and indexing the table:
 *
 *     clrlwi  r7, r9, 30
 *     addi    r7, r7, 7
 *     rlwinm  r0, r7, 2, 0, 29
 *     lis     r12, 0x8201
 *     addi    r12, r12, 0x20
 *     lwzx    r0, r12, r0
 *     mtctr   r0
 *     bctr
 *
 * Pre-fix, `detectJumpTable` treated the `addi r7,r7,7` as an index overwrite
 * and read the table from its un-biased base (entries 0..3); the guest index
 * is 7..10, so the emitted `switch` fell through to `default: __builtin_trap()`.
 * The cycle-8 fix captured the bias and emitted `case 7..10`, but the bias is
 * CONDITIONAL in the real binary: a sibling path reaches the same shared bctr
 * with the un-biased index (path A: `clrlwi rX,rS,29; cmplwi cr6,rX,7; bne
 * <setup>` -> 0..6; path B falls through `clrlwi rX,rS,30; addi rX,rX,7` ->
 * 7..10), so the shared 11-entry table covers the UNION [0, 11).
 *
 * Cycle-9 union mode: entries are read from the UN-BIASED base over
 * [0, unionCount) and `indexBias` is 0, so `build_bctr` emits plain
 * `case 0..unionCount-1` labels. For a self-addi bias unionCount is
 * `indexBias + maxEntries`; for a cross-register bias committed by a mask
 * proof (cycle 7) the path-B window is only `2^(32-MB)` entries wide, so
 * unionCount = max(path-A bounds count, indexBias + 2^(32-MB)) -- the real
 * sites read 11 entries (cases 0..10), not the 15 the old sum over-read.
 * Cycle-9b (review F2) tightens the gate: union mode is
 * only entered when the DUAL-WINDOW pattern is actually detected -- a
 * `cmplwi/cmpwi cr?,rX,K` bounds check on the same index register with
 * K == indexBias whose immediately following conditional branch skips the
 * re-mask+bias block (branch target beyond the bias instruction, still inside
 * the table setup). Otherwise the cycle-8 biased-window behavior is preserved
 * (`indexBias` kept, entries read from the biased base). The union entry count
 * is computed in uint64 and must not exceed `max_jump_table_entries`; on
 * overflow/cap-exceed union mode is rejected and the biased window is used
 * (never an unbounded read). A partial union switch is never emitted: any
 * mid-window read/validation failure returns nullopt and the emission falls
 * back to the guest-exact `PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32)`.
 *
 * The test drives the production detector directly on a synthetic in-memory
 * PPC binary (assembled instruction words; the exact sequences are the ones
 * below). Coverage boundary: this asserts detection + bias arithmetic; the
 * generated `case 0..10` C++ is verified against a real game binary in
 * the cycle-9 generated-code check.
 *
 * Instruction words were assembled with
 *   powerpc-none-elf-as -a32 -be -mregnames -mpower7
 * and disassembled back with powerpc-none-elf-objdump.
 * NOTE (cycle 9): the dual-window words in the task brief for `clrlwi r7,r9,29`
 * and `bne cr6,+8` were re-derived (no assembler is installed here) — the
 * brief's 0x5527078E decodes to MB=30/ME=7 (not a clrlwi 29) and 0x40860008
 * decodes to BI=6 (cr1, not cr6). The corrected words are 0x5527077E and
 * 0x409A000C; see the constants below.
 * NOTE (cycle 9b, review F1): the first corrected branch word 0x409A0008 has
 * BD=+8, so it branched from 0x08 to 0x10 (the path-B `addi r7,r7,7`); both
 * paths then applied the bias and the section did not exercise the dual-window
 * split. The word is now 0x409A000C (BD=+0x0C), which branches from 0x08 to
 * 0x14 and skips BOTH the re-mask at 0x0C and the bias at 0x10; the section
 * asserts that displacement explicitly.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/code_region.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>

#include "../../../src/codegen/decoded_binary.h"

namespace {

using rex::codegen::BinaryView;
using rex::codegen::CodeRegion;
using rex::codegen::DecodedBinary;
using rex::codegen::TestModule;

constexpr uint32_t kBase = 0x82010000;

// Instruction words (big-endian).
constexpr uint32_t kClrlwi_r7_r9_30 = 0x552707BE;
constexpr uint32_t kClrlwi_r7_r9_29 = 0x5527077E;  // corrected (brief said 0x5527078E)
constexpr uint32_t kAddi_r7_r7_7 = 0x38E70007;
constexpr uint32_t kOri_r7_r7_7 = 0x60E70007;
constexpr uint32_t kRlwinm_r0_r7_2 = 0x54E0103A;
constexpr uint32_t kRlwinm_r5_r7_2 = 0x54E5103A;  // rlwinm r5,r7,2,0,29
constexpr uint32_t kAddi_r5_r5_7 = 0x38A50007;    // addi r5,r5,7 (RA!=0 -> stays addi)
constexpr uint32_t kLis_r12_8201 = 0x3D808201;
constexpr uint32_t kAddi_r12_r12_20 = 0x398C0020;
constexpr uint32_t kAddi_r12_r12_24 = 0x398C0024;
constexpr uint32_t kAddi_r12_r12_2C = 0x398C002C;
constexpr uint32_t kLwzx_r0_r12_r0 = 0x7C0C002E;
constexpr uint32_t kLwzx_r0_r12_r5 = 0x7C0C282E;  // lwzx r0,r12,r5
constexpr uint32_t kMtctr_r0 = 0x7C0903A6;
constexpr uint32_t kBctr = 0x4E800420;
constexpr uint32_t kCmplwi_cr6_r7_3 = 0x2B070003;
constexpr uint32_t kCmplwi_cr6_r7_7 = 0x2B070007;
constexpr uint32_t kBgt_cr6_to_0x24 = 0x4199001C;
constexpr uint32_t kBne_cr6_to_0x14 = 0x409A000C;  // F1: BD=+0x0C -> 0x14 (skips re-mask+bias)
constexpr uint32_t kNop = 0x60000000;
// Oversized-window constants (F2b cap tests; see the sections below).
constexpr uint32_t kCmplwi_cr6_r7_520 = 0x2B070208;  // cmplwi cr6,r7,520
constexpr uint32_t kCmplwi_cr6_r7_600 = 0x2B070258;  // cmplwi cr6,r7,600
constexpr uint32_t kAddi_r7_r7_520 = 0x38E70208;     // addi r7,r7,520

// Cross-register dual-window constants (cycle 6; real-world shape at
// 0x83362BE4): the bias addi writes a DIFFERENT register than it reads
// (`addi r19,r11,7`, RT=19 != RA=11), so the index-bias capture must trace
// through the source register r11 and commit on its clrlwi mask.
// Derivation (rlwinm = 21<<26 | RS<<21 | RA<<16 | SH<<11 | MB<<6 | ME<<1;
// cmpli = 10<<26 | BF<<23 | L<<21 | RA<<16 | UIMM; addi = 14<<26 | RT<<21 |
// RA<<16 | SIMM):
//   clrlwi r19,r10,29    = rlwinm r19,r10,0,29,31 -> 0x54000000 | 10<<21 |
//                          19<<16 | 29<<6 | 31<<1 = 0x5553077E
//   clrlwi r11,r11,30    = rlwinm r11,r11,0,30,31 -> 0x54000000 | 11<<21 |
//                          11<<16 | 30<<6 | 31<<1 = 0x556B07BE
//   cmplwi cr6,r19,7     = 0x28000000 | 6<<23 | 19<<16 | 7 = 0x2B130007
//   addi r19,r11,7       = 0x38000000 | 19<<21 | 11<<16 | 7 = 0x3A6B0007
//   rlwinm r0,r19,2,0,29 = 0x54000000 | 19<<21 | 2<<11 | 29<<1 = 0x5660103A
constexpr uint32_t kClrlwi_r19_r10_29 = 0x5553077E;
constexpr uint32_t kClrlwi_r11_r11_30 = 0x556B07BE;
constexpr uint32_t kCmplwi_cr6_r19_7 = 0x2B130007;
constexpr uint32_t kAddi_r19_r11_7 = 0x3A6B0007;
constexpr uint32_t kRlwinm_r0_r19_2 = 0x5660103A;

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
  bytes.push_back(static_cast<uint8_t>(word >> 24));
  bytes.push_back(static_cast<uint8_t>(word >> 16));
  bytes.push_back(static_cast<uint8_t>(word >> 8));
  bytes.push_back(static_cast<uint8_t>(word));
}

uint32_t case_addr(uint32_t case_base, uint32_t index) { return case_base + 8 * index; }

// 11 case bodies: `li r3, 0x100+i; blr` each (also the jump-table targets).
void append_case_bodies(std::vector<uint8_t>& bytes, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    append_word(bytes, 0x38600000u | (0x100u + i));  // li r3, 0x100+i
    append_word(bytes, 0x4E800020u);                 // blr
  }
}

// 11-entry absolute table; entry i points at case body i.
void append_table(std::vector<uint8_t>& bytes, uint32_t case_base) {
  for (uint32_t i = 0; i < 11; i++) {
    append_word(bytes, case_addr(case_base, i));
  }
}

// N-entry absolute table; entry i points at case body i.
void append_table_n(std::vector<uint8_t>& bytes, uint32_t count, uint32_t case_base) {
  for (uint32_t i = 0; i < count; i++) {
    append_word(bytes, case_addr(case_base, i));
  }
}

// Single-path biased shape: clrlwi bounds -> addi bias -> scaled lwzx ->
// mtctr/bctr. There is NO `cmplwi r7,7` dual-window discriminator, so (cycle
// 9b F2a) union mode must NOT be entered and the cycle-8 biased window is
// used. bctr at 0x8201001C, table at 0x82010020, case bodies at 0x8201004C.
std::vector<uint8_t> biased_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r7_r9_30);  // 0x00: bounds 0..3
  append_word(bytes, kAddi_r7_r7_7);     // 0x04: bias +7
  append_word(bytes, kRlwinm_r0_r7_2);   // 0x08
  append_word(bytes, kLis_r12_8201);     // 0x0C
  append_word(bytes, kAddi_r12_r12_20);  // 0x10: table = 0x82010020
  append_word(bytes, kLwzx_r0_r12_r0);   // 0x14
  append_word(bytes, kMtctr_r0);         // 0x18
  append_word(bytes, kBctr);             // 0x1C
  append_table(bytes, kBase + 0x4C);     // 0x20..0x4B
  append_case_bodies(bytes, 11);         // 0x4C..0xA3
  return bytes;
}

// Same sequence without the bias addi; bctr at 0x82010018.
std::vector<uint8_t> unbiased_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r7_r9_30);  // 0x00
  append_word(bytes, kRlwinm_r0_r7_2);   // 0x04
  append_word(bytes, kLis_r12_8201);     // 0x08
  append_word(bytes, kAddi_r12_r12_20);  // 0x0C: table = 0x82010020
  append_word(bytes, kLwzx_r0_r12_r0);   // 0x10
  append_word(bytes, kMtctr_r0);         // 0x14
  append_word(bytes, kBctr);             // 0x18
  append_word(bytes, kNop);              // 0x1C
  append_table(bytes, kBase + 0x4C);     // 0x20..0x4B
  append_case_bodies(bytes, 11);         // 0x4C..0xA3
  return bytes;
}

// Bias BEFORE a register-writing bounds check cannot be captured (tracing stops
// at the clrlwi), so the un-biased window is used. bctr at 0x8201001C,
// table at 0x82010020, case bodies at 0x8201004C.
std::vector<uint8_t> bias_before_clrlwi_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kAddi_r7_r7_7);     // 0x00: bias first
  append_word(bytes, kClrlwi_r7_r9_30);  // 0x04: bounds 0..3 (writes r7)
  append_word(bytes, kRlwinm_r0_r7_2);   // 0x08
  append_word(bytes, kLis_r12_8201);     // 0x0C
  append_word(bytes, kAddi_r12_r12_20);  // 0x10: table = 0x82010020
  append_word(bytes, kLwzx_r0_r12_r0);   // 0x14
  append_word(bytes, kMtctr_r0);         // 0x18
  append_word(bytes, kBctr);             // 0x1C
  append_table(bytes, kBase + 0x4C);     // 0x20..0x4B
  append_case_bodies(bytes, 11);         // 0x4C..0xA3
  return bytes;
}

// Bias captured but positioned BEFORE a non-writing bounds check (cmplwi):
// exercises the positional discriminator (biasAddr > bounds.checkAddr).
// bctr at 0x82010020, table at 0x82010024, case bodies at 0x82010050.
std::vector<uint8_t> bias_before_cmpli_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kAddi_r7_r7_7);     // 0x00: bias
  append_word(bytes, kCmplwi_cr6_r7_3);  // 0x04: bounds 0..3 (no register write)
  append_word(bytes, kBgt_cr6_to_0x24);  // 0x08: conditional branch to 0x24
  append_word(bytes, kRlwinm_r0_r7_2);   // 0x0C
  append_word(bytes, kLis_r12_8201);     // 0x10
  append_word(bytes, kAddi_r12_r12_24);  // 0x14: table = 0x82010024
  append_word(bytes, kLwzx_r0_r12_r0);   // 0x18
  append_word(bytes, kMtctr_r0);         // 0x1C
  append_word(bytes, kBctr);             // 0x20
  append_table(bytes, kBase + 0x50);     // 0x24..0x4F
  append_case_bodies(bytes, 11);         // 0x50..0xA7
  return bytes;
}

// Dual-window shared bctr (the real-world shape): path A bounds-checks the
// raw index with clrlwi 29 / cmplwi 7 / bne (un-biased 0..6); the fall-through
// path B re-masks with clrlwi 30 and adds the +7 bias (7..10). The path-A
// branch at 0x08 (0x409A000C, BD=+0x0C) targets 0x14, so it skips BOTH the
// re-mask at 0x0C (`clrlwi r7,r9,30`) and the bias at 0x10 (`addi r7,r7,7`)
// and lands on the shared setup (`rlwinm`). The shared 11-entry table covers
// the union 0..10. bctr at 0x82010028, table at 0x8201002C (lis 0x8201 + addi
// 0x2C; the brief's +0x20 would point at the lwzx/mtctr/bctr words and cannot
// hold the 11-entry table), case bodies at 0x82010058.
std::vector<uint8_t> dual_window_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r7_r9_29);   // 0x00: path A bounds 0..7
  append_word(bytes, kCmplwi_cr6_r7_7);   // 0x04: compare with 7 (K == bias)
  append_word(bytes, kBne_cr6_to_0x14);   // 0x08: path A -> 0x14 (skips 0x0C/0x10)
  append_word(bytes, kClrlwi_r7_r9_30);   // 0x0C: path B re-mask 0..3
  append_word(bytes, kAddi_r7_r7_7);      // 0x10: path B bias +7 -> 7..10
  append_word(bytes, kRlwinm_r0_r7_2);    // 0x14: shared setup
  append_word(bytes, kLis_r12_8201);      // 0x18
  append_word(bytes, kAddi_r12_r12_2C);   // 0x1C: table = 0x8201002C
  append_word(bytes, kLwzx_r0_r12_r0);    // 0x20
  append_word(bytes, kMtctr_r0);          // 0x24
  append_word(bytes, kBctr);              // 0x28
  append_table(bytes, kBase + 0x58);      // 0x2C..0x57 (11 entries)
  append_case_bodies(bytes, 11);          // 0x58..0xAF
  return bytes;
}

// Cross-register dual-window (cycle 6, real-world shape;
// union count corrected in cycle 7): path A masks the raw index to 29 bits
// (`clrlwi r19,r10,29`), bounds-checks it against 7 and branches to the shared
// setup (un-biased 0..7); path B falls through, re-masks r11 to 30 bits and
// adds the +7 bias into a DIFFERENT register (`addi r19,r11,7`, RT=19 !=
// RA=11) giving 7..10. The traced index register is r19 (the addi
// destination), so the bias addi is cross-register; the clrlwi on r11 is the
// mask proof that commits the pending candidate, and its MB=30 fixes the
// path-B window at 2^(32-30) = 4 entries. The union count is
// max(path-A bounds 8, bias 7 + 4) = 11 (NOT the old bias+bounds sum 15, which
// over-read four data words and rejected the table), so the shared 11-entry
// table covers 0..10. bctr at 0x82010028, table at 0x8201002C, case bodies at
// 0x82010058.
std::vector<uint8_t> cross_register_dual_window_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r19_r10_29);  // 0x00: path A bounds 0..7
  append_word(bytes, kCmplwi_cr6_r19_7);   // 0x04: compare with 7 (K == bias)
  append_word(bytes, kBne_cr6_to_0x14);    // 0x08: path A -> 0x14 (skips 0x0C/0x10)
  append_word(bytes, kClrlwi_r11_r11_30);  // 0x0C: path B re-mask 0..3
  append_word(bytes, kAddi_r19_r11_7);     // 0x10: path B bias +7 (cross-register)
  append_word(bytes, kRlwinm_r0_r19_2);    // 0x14: shared setup
  append_word(bytes, kLis_r12_8201);       // 0x18
  append_word(bytes, kAddi_r12_r12_2C);    // 0x1C: table = 0x8201002C
  append_word(bytes, kLwzx_r0_r12_r0);     // 0x20
  append_word(bytes, kMtctr_r0);           // 0x24
  append_word(bytes, kBctr);               // 0x28
  append_table_n(bytes, 11, kBase + 0x58); // 0x2C..0x57 (11 entries)
  append_case_bodies(bytes, 11);           // 0x58..0xAF
  return bytes;
}

// Negative cross-register fixture: the same 11-entry table, but ENTRY 10 (the
// final required entry, at 0x54) holds a NOP word (0x60000000) instead of a
// code pointer. Union mode reads exactly 11 entries (cycle-7 count); entry 10's
// value is not a target inside the region -> union mode rejects and
// detectJumpTable returns nullopt, so build_bctr falls back to
// PPC_CALL_INDIRECT_FUNC.
std::vector<uint8_t> cross_register_short_table_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r19_r10_29);  // 0x00: path A bounds 0..7
  append_word(bytes, kCmplwi_cr6_r19_7);   // 0x04: compare with 7 (K == bias)
  append_word(bytes, kBne_cr6_to_0x14);    // 0x08: path A -> 0x14 (skips 0x0C/0x10)
  append_word(bytes, kClrlwi_r11_r11_30);  // 0x0C: path B re-mask 0..3
  append_word(bytes, kAddi_r19_r11_7);     // 0x10: path B bias +7 (cross-register)
  append_word(bytes, kRlwinm_r0_r19_2);    // 0x14: shared setup
  append_word(bytes, kLis_r12_8201);       // 0x18
  append_word(bytes, kAddi_r12_r12_2C);    // 0x1C: table = 0x8201002C
  append_word(bytes, kLwzx_r0_r12_r0);     // 0x20
  append_word(bytes, kMtctr_r0);           // 0x24
  append_word(bytes, kBctr);               // 0x28
  append_table_n(bytes, 10, kBase + 0x58); // 0x2C..0x53 (entries 0..9)
  append_word(bytes, kNop);                // 0x54: entry 10 -> 0x60000000 (not a target)
  append_case_bodies(bytes, 11);           // 0x58..0xAF
  return bytes;
}

// Negative (P1 ori guard): a self-`ori r7,r7,7` must NOT be captured as an
// index bias (OR can collapse indices: 0..3 | 7 == 7), so the un-biased window
// is read. bctr at 0x8201001C, table at 0x82010020, case bodies at 0x8201004C.
std::vector<uint8_t> ori_bias_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r7_r9_30);  // 0x00: bounds 0..3
  append_word(bytes, kOri_r7_r7_7);      // 0x04: ori bias (must be ignored)
  append_word(bytes, kRlwinm_r0_r7_2);   // 0x08
  append_word(bytes, kLis_r12_8201);     // 0x0C
  append_word(bytes, kAddi_r12_r12_20);  // 0x10: table = 0x82010020
  append_word(bytes, kLwzx_r0_r12_r0);   // 0x14
  append_word(bytes, kMtctr_r0);         // 0x18
  append_word(bytes, kBctr);             // 0x1C
  append_table(bytes, kBase + 0x4C);     // 0x20..0x4B
  append_case_bodies(bytes, 11);         // 0x4C..0xA3
  return bytes;
}

// Negative (P1 post-scale guard): a self-addi on the SCALED register
// (`rlwinm r5,r7,2; addi r5,r5,7`) is a byte-offset adjustment, not an element
// bias, so it must NOT be captured (indexBias == 0, un-biased window read).
// The scaled register is r5 rather than r0 because `addi r0,r0,N` decodes as
// `li` (RA=0), which would never reach the addi bias-capture path and so would
// not exercise the guard. bctr at 0x8201001C, table at 0x82010020, case bodies
// at 0x8201004C.
std::vector<uint8_t> scaled_bias_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r7_r9_30);  // 0x00: bounds 0..3
  append_word(bytes, kRlwinm_r5_r7_2);   // 0x04: scale r7 -> r5
  append_word(bytes, kAddi_r5_r5_7);     // 0x08: post-scale addi (must be ignored)
  append_word(bytes, kLis_r12_8201);     // 0x0C
  append_word(bytes, kAddi_r12_r12_20);  // 0x10: table = 0x82010020
  append_word(bytes, kLwzx_r0_r12_r5);   // 0x14
  append_word(bytes, kMtctr_r0);         // 0x18
  append_word(bytes, kBctr);             // 0x1C
  append_table(bytes, kBase + 0x4C);     // 0x20..0x4B
  append_case_bodies(bytes, 11);         // 0x4C..0xA3
  return bytes;
}

// Oversized-bias dual-window (F2b): the union window [0, bias+count) would be
// 520 + 4 = 524 entries > max_jump_table_entries (512), so union mode must be
// rejected and the cycle-8 biased window used. The biased entries live at
// tableBase = 0x8201002C + 520*4; the gap before them is NOP-filled, so a
// union read would read 0x60000000 as entry 0, which is not a code target and
// is rejected -- this test fails if the cap check is missing. (The gap cannot
// be zero-filled: a >=2-word null run splits the decoded code region, which
// would also put the biased entries outside `containingRegion`.) bctr at
// 0x82010028.
std::vector<uint8_t> oversized_bias_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r7_r9_29);    // 0x00: path A bounds 0..7
  append_word(bytes, kCmplwi_cr6_r7_520);  // 0x04: K == bias (520)
  append_word(bytes, kBne_cr6_to_0x14);    // 0x08: path A -> 0x14 (skips 0x0C/0x10)
  append_word(bytes, kClrlwi_r7_r9_30);    // 0x0C: path B re-mask 0..3
  append_word(bytes, kAddi_r7_r7_520);     // 0x10: path B bias +520
  append_word(bytes, kRlwinm_r0_r7_2);     // 0x14: shared setup
  append_word(bytes, kLis_r12_8201);       // 0x18
  append_word(bytes, kAddi_r12_r12_2C);    // 0x1C: table = 0x8201002C
  append_word(bytes, kLwzx_r0_r12_r0);     // 0x20
  append_word(bytes, kMtctr_r0);           // 0x24
  append_word(bytes, kBctr);               // 0x28
  // NOP gap up to the biased window at 0x2C + 520*4 (see the comment above:
  // nulls would split the code region).
  while (bytes.size() < 0x2C + 520 * 4) append_word(bytes, kNop);
  const uint32_t case_base = kBase + 0x2C + 520 * 4 + 4 * 4;  // after 4 entries
  for (uint32_t i = 0; i < 4; i++) append_word(bytes, case_addr(case_base, i));
  append_case_bodies(bytes, 4);
  return bytes;
}

// Oversized-bounds dual-window (F2b): the bounds check carries a 600 immediate
// (601 entries) and the +7 bias makes the union 7 + 601 = 608 entries >
// max_jump_table_entries (512), so union mode must be rejected. The fallback is
// the cycle-8 biased-window read (601 entries from tableBase = 0x8201002C +
// 7*4), i.e. bounded by the bounds immediate, not by the union cap.
// bctr at 0x82010028.
std::vector<uint8_t> oversized_bounds_binary() {
  std::vector<uint8_t> bytes;
  append_word(bytes, kClrlwi_r7_r9_29);    // 0x00: path A bounds 0..7
  append_word(bytes, kCmplwi_cr6_r7_7);    // 0x04: K == bias (dual-window check)
  append_word(bytes, kBne_cr6_to_0x14);    // 0x08: path A -> 0x14 (skips 0x0C/0x10)
  append_word(bytes, kCmplwi_cr6_r7_600);  // 0x0C: oversized bounds (601 entries)
  append_word(bytes, kAddi_r7_r7_7);       // 0x10: path B bias +7
  append_word(bytes, kRlwinm_r0_r7_2);     // 0x14: shared setup
  append_word(bytes, kLis_r12_8201);       // 0x18
  append_word(bytes, kAddi_r12_r12_2C);    // 0x1C: table = 0x8201002C
  append_word(bytes, kLwzx_r0_r12_r0);     // 0x20
  append_word(bytes, kMtctr_r0);           // 0x24
  append_word(bytes, kBctr);               // 0x28
  // 608 entries: the union window [0, 608) would be read if union mode were
  // (wrongly) entered; the biased window reads entries 7..607.
  const uint32_t case_base = kBase + 0x2C + 608 * 4;
  for (uint32_t i = 0; i < 608; i++) append_word(bytes, case_addr(case_base, i));
  append_case_bodies(bytes, 608);
  return bytes;
}

struct Detection {
  bool found = false;
  int32_t indexBias = 0;
  uint32_t tableAddress = 0;
  uint8_t indexRegister = 0xFF;
  std::vector<uint32_t> targets;
};

Detection detect(const std::vector<uint8_t>& bytes, uint32_t bctrAddr) {
  TestModule module;
  module.Load(kBase, bytes.data(), bytes.size());

  BinaryView view = BinaryView::fromModule(module);
  DecodedBinary decoded(view);
  decoded.decode();

  Detection result;
  const CodeRegion* region = decoded.regionContaining(bctrAddr);
  REQUIRE(region != nullptr);

  auto jt = rex::codegen::detectJumpTable(decoded, bctrAddr, *region, kBase, region->end);
  if (jt) {
    result.found = true;
    result.indexBias = jt->indexBias;
    result.tableAddress = jt->tableAddress;
    result.indexRegister = jt->indexRegister;
    result.targets = jt->targets;
  }
  return result;
}

}  // namespace

TEST_CASE("jump table index bias", "[jump_table]") {
  SECTION("single-path biased table (no cmplwi-split) keeps the biased window") {
    auto bytes = biased_binary();
    Detection d = detect(bytes, kBase + 0x1C);

    REQUIRE(d.found);
    // F2a: there is no `cmplwi r7,7` dual-window discriminator, so there is no
    // proof of an un-biased sibling path; the cycle-8 biased window is kept
    // (indexBias == 7, entries read from the biased base).
    CHECK(d.indexBias == 7);
    CHECK(d.indexRegister == 7);
    CHECK(d.tableAddress == kBase + 0x20);
    CHECK(d.targets.size() == 4);

    // Entries 7..10 of the 11-entry table (case bodies 7..10).
    const std::vector<uint32_t> biased_window = {kBase + 0x84, kBase + 0x8C, kBase + 0x94,
                                                 kBase + 0x9C};
    CHECK(d.targets == biased_window);

    // The pre-fix (un-biased 0..3) read would have returned entries 0..3.
    const std::vector<uint32_t> unbiased_window = {kBase + 0x4C, kBase + 0x54, kBase + 0x5C,
                                                   kBase + 0x64};
    CHECK(d.targets != unbiased_window);

    // Emission contract: build_bctr prints `case i + indexBias` -> cases 7..10.
    for (size_t i = 0; i < d.targets.size(); i++) {
      CHECK(static_cast<int32_t>(i) + d.indexBias == static_cast<int32_t>(i) + 7);
    }
    CHECK(static_cast<int32_t>(d.targets.size() - 1) + d.indexBias == 10);
  }

  SECTION("no bias addi reads the un-biased table window") {
    auto bytes = unbiased_binary();
    Detection d = detect(bytes, kBase + 0x18);

    REQUIRE(d.found);
    CHECK(d.indexBias == 0);
    CHECK(d.tableAddress == kBase + 0x20);
    const std::vector<uint32_t> unbiased_window = {kBase + 0x4C, kBase + 0x54, kBase + 0x5C,
                                                   kBase + 0x64};
    CHECK(d.targets == unbiased_window);
  }

  SECTION("bias before a register-writing bounds check is not a table offset") {
    auto bytes = bias_before_clrlwi_binary();
    Detection d = detect(bytes, kBase + 0x1C);

    REQUIRE(d.found);
    CHECK(d.indexBias == 0);
    const std::vector<uint32_t> unbiased_window = {kBase + 0x4C, kBase + 0x54, kBase + 0x5C,
                                                   kBase + 0x64};
    CHECK(d.targets == unbiased_window);
  }

  SECTION("bias before a non-writing bounds check is rejected positionally") {
    auto bytes = bias_before_cmpli_binary();
    Detection d = detect(bytes, kBase + 0x20);

    REQUIRE(d.found);
    CHECK(d.indexBias == 0);
    CHECK(d.tableAddress == kBase + 0x24);
    const std::vector<uint32_t> unbiased_window = {kBase + 0x50, kBase + 0x58, kBase + 0x60,
                                                   kBase + 0x68};
    CHECK(d.targets == unbiased_window);
  }

  SECTION("dual-window shared bctr reads the 11-entry union window (cases 0..10)") {
    auto bytes = dual_window_binary();

    // F1: the path-A branch word itself must skip BOTH path-B instructions.
    // From its address (0x08) the +0x0C displacement lands on the shared setup
    // at 0x14, i.e. past the re-mask at 0x0C (`clrlwi r7,r9,30`) and past the
    // bias at 0x10 (`addi r7,r7,7`).
    const int32_t branch_disp = static_cast<int16_t>(kBne_cr6_to_0x14 & 0x0000FFFCu);
    CHECK((kBase + 0x08) + static_cast<uint32_t>(branch_disp) == kBase + 0x14);
    CHECK((kBase + 0x08) + static_cast<uint32_t>(branch_disp) > kBase + 0x10);

    Detection d = detect(bytes, kBase + 0x28);

    REQUIRE(d.found);
    // Union mode: the +7 bias is conditional (path A reaches the table
    // un-biased), so the union [0, 7+4) is read from the un-biased base and
    // the emission is plain cases 0..10.
    CHECK(d.indexBias == 0);
    CHECK(d.indexRegister == 7);
    CHECK(d.tableAddress == kBase + 0x2C);
    CHECK(d.targets.size() == 11);

    std::vector<uint32_t> all_cases;
    for (uint32_t i = 0; i < 11; i++) all_cases.push_back(kBase + 0x58 + 8 * i);
    CHECK(d.targets == all_cases);

    // Emission contract: cases 0..10 (no partial union switch).
    for (size_t i = 0; i < d.targets.size(); i++) {
      CHECK(static_cast<int32_t>(i) + d.indexBias == static_cast<int32_t>(i));
    }
    CHECK(static_cast<int32_t>(d.targets.size() - 1) + d.indexBias == 10);
  }

  SECTION("oversized bias rejects union mode and keeps the biased window (F2b)") {
    auto bytes = oversized_bias_binary();
    Detection d = detect(bytes, kBase + 0x28);

    REQUIRE(d.found);
    // Union count 520 + 4 = 524 > max_jump_table_entries (512): union mode is
    // rejected, the biased window [520, 524) is read from the biased base.
    CHECK(d.indexBias == 520);
    CHECK(d.tableAddress == kBase + 0x2C);
    CHECK(d.targets.size() == 4);

    const uint32_t case_base = kBase + 0x2C + 520 * 4 + 4 * 4;
    std::vector<uint32_t> biased_cases;
    for (uint32_t i = 0; i < 4; i++) biased_cases.push_back(case_addr(case_base, i));
    CHECK(d.targets == biased_cases);
  }

  SECTION("oversized bounds rejects union mode and keeps the biased window (F2b)") {
    auto bytes = oversized_bounds_binary();
    Detection d = detect(bytes, kBase + 0x28);

    REQUIRE(d.found);
    // Union count 7 + 601 = 608 > max_jump_table_entries (512): union mode is
    // rejected; the cycle-8 biased window reads the 601 entries bounded by the
    // bounds immediate, starting at table entry 7.
    CHECK(d.indexBias == 7);
    CHECK(d.tableAddress == kBase + 0x2C);
    CHECK(d.targets.size() == 601);

    const uint32_t case_base = kBase + 0x2C + 608 * 4;
    CHECK(d.targets.front() == case_addr(case_base, 7));
    CHECK(d.targets.back() == case_addr(case_base, 607));
  }

  SECTION("self-ori bias is not captured (P1 ori guard)") {
    auto bytes = ori_bias_binary();
    Detection d = detect(bytes, kBase + 0x1C);

    REQUIRE(d.found);
    CHECK(d.indexBias == 0);
    CHECK(d.tableAddress == kBase + 0x20);
    const std::vector<uint32_t> unbiased_window = {kBase + 0x4C, kBase + 0x54, kBase + 0x5C,
                                                   kBase + 0x64};
    CHECK(d.targets == unbiased_window);
  }

  SECTION("post-scale self-addi is not captured (P1 scaled-register guard)") {
    auto bytes = scaled_bias_binary();
    Detection d = detect(bytes, kBase + 0x1C);

    REQUIRE(d.found);
    CHECK(d.indexBias == 0);
    CHECK(d.tableAddress == kBase + 0x20);
    const std::vector<uint32_t> unbiased_window = {kBase + 0x4C, kBase + 0x54, kBase + 0x5C,
                                                   kBase + 0x64};
    CHECK(d.targets == unbiased_window);
  }

  SECTION("cross-register bias dual-window reads the 11-entry union window (cases 0..10)") {
    auto bytes = cross_register_dual_window_binary();
    Detection d = detect(bytes, kBase + 0x28);

    REQUIRE(d.found);
    // The pending candidate is committed by the clrlwi mask on the SOURCE
    // register (r11) with MB=30, so path B is 2^(32-30) = 4 entries wide; the
    // union count is max(path-A bounds 8, bias 7 + 4) = 11. The traced index
    // register stays r19 (the addi destination) and union mode emits plain
    // cases 0..10.
    CHECK(d.indexBias == 0);
    CHECK(d.indexRegister == 19);
    CHECK(d.tableAddress == kBase + 0x2C);
    REQUIRE(d.targets.size() == 11);

    for (uint32_t i = 0; i < 11; i++) {
      CHECK(d.targets[i] == kBase + 0x58 + 8 * i);
    }
    CHECK(static_cast<int32_t>(d.targets.size() - 1) + d.indexBias == 10);
  }

  SECTION("cross-register bias with invalid union tail rejects the table") {
    auto bytes = cross_register_short_table_binary();
    Detection d = detect(bytes, kBase + 0x28);

    // Union entry 10 reads the NOP word at 0x54 as the address 0x60000000,
    // which is not a code target inside the region: union mode must reject the
    // table entirely (nullopt) rather than emit a partial switch with a trap
    // default.
    CHECK_FALSE(d.found);
  }
}
