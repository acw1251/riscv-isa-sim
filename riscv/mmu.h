// See LICENSE for license details.

#ifndef _RISCV_MMU_H
#define _RISCV_MMU_H

#include "decode.h"
#include "trap.h"
#include "common.h"
#include "config.h"
#include "sim.h"
#include "processor.h"
#include "memtracer.h"
#include <stdlib.h>
#include <vector>

// virtual memory configuration
#define PGSHIFT 12
const reg_t PGSIZE = 1 << PGSHIFT;
const reg_t PGMASK = ~(PGSIZE-1);
const reg_t PGOFFSETMASK = PGSIZE-1;

// The processor may use a cache to improve simulation performance, so the
// processor needs to know if it can cache an instruction fetch result.
struct insn_fetch_t
{
  bool cacheable;
  insn_t insn;
};

class trigger_matched_t
{
  public:
    trigger_matched_t(int index,
        trigger_operation_t operation, reg_t address, reg_t data) :
      index(index), operation(operation), address(address), data(data) {}

    int index;
    trigger_operation_t operation;
    reg_t address;
    reg_t data;
};

// this class implements a processor's port into the virtual memory system.
// an MMU and instruction cache are maintained for simulator performance.
class mmu_t
{
public:
  mmu_t(sim_t* sim, processor_t* proc);
  ~mmu_t();

  // template for functions that load an aligned value from memory
  #define load_func(type) \
    inline type##_t load_##type(reg_t addr) { \
      if (addr & (sizeof(type##_t)-1)) \
        throw trap_load_address_misaligned(addr); \
      reg_t vpn = addr >> PGSHIFT; \
      reg_t offset = addr & PGOFFSETMASK; \
      if (likely(tlb_load_tag[vpn % TLB_ENTRIES] == vpn)) { \
        type##_t data = *(type##_t*)(tlb_data[vpn % TLB_ENTRIES] + offset); \
        if (unlikely(check_triggers_load)) { \
          if (likely(!matched_trigger)) { \
            matched_trigger = trigger_exception(OPERATION_LOAD, addr, data); \
            if (matched_trigger) \
              throw *matched_trigger; \
          } \
        } \
        return data; \
      } \
      type##_t res; \
      load_slow_path(addr, sizeof(type##_t), (uint8_t*)&res); \
      return res; \
    }

  // load value from memory at aligned address; zero extend to register width
  load_func(uint8)
  load_func(uint16)
  load_func(uint32)
  load_func(uint64)

  // load value from memory at aligned address; sign extend to register width
  load_func(int8)
  load_func(int16)
  load_func(int32)
  load_func(int64)

  // template for functions that store an aligned value to memory
  #define store_func(type) \
    void store_##type(reg_t addr, type##_t val) { \
      if (addr & (sizeof(type##_t)-1)) \
        throw trap_store_address_misaligned(addr); \
      reg_t vpn = addr >> PGSHIFT; \
      reg_t offset = addr & PGOFFSETMASK; \
      if (likely(tlb_store_tag[vpn % TLB_ENTRIES] == vpn)) { \
        if (unlikely(check_triggers_store)) { \
          if (likely(!matched_trigger)) { \
            matched_trigger = trigger_exception(OPERATION_STORE, addr, val); \
            if (matched_trigger) \
              throw *matched_trigger; \
          } \
        } \
        *(type##_t*)(tlb_data[vpn % TLB_ENTRIES] + offset) = val; \
      } \
      else \
        store_slow_path(addr, sizeof(type##_t), (const uint8_t*)&val); \
    }

  // store value to memory at aligned address
  store_func(uint8)
  store_func(uint16)
  store_func(uint32)
  store_func(uint64)

  // this is necessary for gen_icache
  static const reg_t ICACHE_ENTRIES = 1024;

  inline insn_fetch_t load_insn(reg_t addr)
  {
    const uint16_t* iaddr = translate_insn_addr(addr);
    insn_bits_t insn = *iaddr;
    int length = insn_length(insn);

    if (likely(length == 4)) {
      insn |= (insn_bits_t)*(const int16_t*)translate_insn_addr(addr + 2) << 16;
    } else if (length == 2) {
      insn = (int16_t)insn;
    } else if (length == 6) {
      insn |= (insn_bits_t)*(const int16_t*)translate_insn_addr(addr + 4) << 32;
      insn |= (insn_bits_t)*(const uint16_t*)translate_insn_addr(addr + 2) << 16;
    } else {
      static_assert(sizeof(insn_bits_t) == 8, "insn_bits_t must be uint64_t");
      insn |= (insn_bits_t)*(const int16_t*)translate_insn_addr(addr + 6) << 48;
      insn |= (insn_bits_t)*(const uint16_t*)translate_insn_addr(addr + 4) << 32;
      insn |= (insn_bits_t)*(const uint16_t*)translate_insn_addr(addr + 2) << 16;
    }

    insn_fetch_t fetch = {true, insn};
    reg_t paddr = sim->mem_to_addr((char*)iaddr);
    if (tracer.interested_in_range(paddr, paddr + 1, FETCH)) {
      // if this instruction is being traced by a tracer, mark it as not
      // cacheable so this function gets called every time to trace it
      fetch.cacheable = false;
      tracer.trace(paddr, length, FETCH);
    } else if (check_triggers_fetch) {
      fetch.cacheable = false;
    }
    return fetch;
  }

  void flush_tlb();
  void flush_icache();

  void set_check_triggers_fetch(bool x)
  {
    check_triggers_fetch = x;
  }
  void set_check_triggers_load(bool x)
  {
    check_triggers_load = x;
  }
  void set_check_triggers_store(bool x)
  {
    check_triggers_store = x;
  }

  void register_memtracer(memtracer_t*);

private:
  sim_t* sim;
  processor_t* proc;
  memtracer_list_t tracer;
  uint16_t fetch_temp;

  // implement a TLB for simulator performance
  static const reg_t TLB_ENTRIES = 256;
  // If a TLB tag has TLB_CHECK_TRIGGERS set, then the MMU must check for a
  // trigger match before completing an access.
  static const reg_t TLB_CHECK_TRIGGERS = 1L<<63;
  char* tlb_data[TLB_ENTRIES];
  reg_t tlb_insn_tag[TLB_ENTRIES];
  reg_t tlb_load_tag[TLB_ENTRIES];
  reg_t tlb_store_tag[TLB_ENTRIES];

  // finish translation on a TLB miss and update the TLB
  void refill_tlb(reg_t vaddr, reg_t paddr, access_type type);
  const char* fill_from_mmio(reg_t vaddr, reg_t paddr);

  // perform a page table walk for a given VA; set referenced/dirty bits
  reg_t walk(reg_t addr, access_type type, reg_t prv);

  // handle uncommon cases: TLB misses, page faults, MMIO
  const uint16_t* fetch_slow_path(reg_t addr);
  void load_slow_path(reg_t addr, reg_t len, uint8_t* bytes);
  void store_slow_path(reg_t addr, reg_t len, const uint8_t* bytes);
  reg_t translate(reg_t addr, access_type type);

  // ITLB lookup
  inline const uint16_t* translate_insn_addr(reg_t addr) {
    reg_t vpn = addr >> PGSHIFT;
    reg_t offset = addr & PGOFFSETMASK;
    if (likely(tlb_insn_tag[vpn % TLB_ENTRIES] == vpn)) {
      uint16_t* ptr = (uint16_t*)(tlb_data[vpn % TLB_ENTRIES] + offset);
      if (unlikely(check_triggers_fetch)) {
        int match = proc->trigger_match(OPERATION_EXECUTE, addr, *ptr);
        if (match >= 0)
          throw trigger_matched_t(match, OPERATION_EXECUTE, addr, *ptr);
        // XXX: This is what the load and store macros do
        // if (likely(!matched_trigger)) {
        //   matched_trigger = trigger_exception(OPERATION_EXECUTE, addr, *ptr);
        //   if (matched_trigger)
        //     throw *matched_trigger;
        // }
      }
      return ptr;
    }
    return fetch_slow_path(addr);
  }

  inline trigger_matched_t *trigger_exception(trigger_operation_t operation,
      reg_t address, reg_t data)
  {
    if (!proc) {
      return NULL;
    }
    int match = proc->trigger_match(operation, address, data);
    if (match == -1)
      return NULL;
    if (proc->state.mcontrol[match].timing == 0) {
      throw trigger_matched_t(match, operation, address, data);
    }
    return new trigger_matched_t(match, operation, address, data);
  }

  bool check_triggers_fetch;
  bool check_triggers_load;
  bool check_triggers_store;
  // The exception describing a matched trigger, or NULL.
  trigger_matched_t *matched_trigger;

  friend class processor_t;
};

#endif
