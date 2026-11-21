/* so_util.c -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * HL2: loads the 26 Source engine modules side by side, an arbitrary number
 * of PT_LOAD segments, RELR/ANDROID_RELR packed relocations and cross-module
 * symbol resolution (CreateInterface plumbing relies on it).
 */

#include <switch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <elf.h>
#include <sys/stat.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"

// not in devkitA64's elf.h
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

/* Android's APS2 packed relocations. Not the same thing as DT_RELR: the table
 * is a SLEB128 stream, and the corresponding section carries
 * SHT_ANDROID_RELA (0x60000002) with sh_entsize == 1. */
#ifndef DT_ANDROID_RELA
#define DT_ANDROID_REL     0x6000000f
#define DT_ANDROID_RELSZ   0x60000010
#define DT_ANDROID_RELA    0x60000011
#define DT_ANDROID_RELASZ  0x60000012
#endif
#ifndef SHT_ANDROID_RELA
#define SHT_ANDROID_RELA   0x60000002
#endif

static so_module *so_list = NULL;

so_module *so_first(void) {
  return so_list;
}

so_module *so_find_module(const char *basename) {
  for (so_module *m = so_list; m; m = m->next) {
    if (strcmp(m->name, basename) == 0)
      return m;
  }
  return NULL;
}

int so_is_module(const void *handle) {
  for (so_module *m = so_list; m; m = m->next)
    if ((const void *)m == handle)
      return 1;
  return 0;
}

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

void so_flush_caches(so_module *mod) {
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  armICacheInvalidate(mod->load_virtbase, mod->load_size);
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
}

void so_finalize(so_module *mod) {
  Result rc = 0;

  // map the entire thing as code memory
  rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);
  if (R_FAILED(rc)) fatal_error("Error: svcMapProcessCodeMemory failed:\n%08x", rc);

  // the RW mirror at load_base is gone now; rebase the symbol table pointers
  // to the live mapping so runtime dlsym() keeps working
  mod->syms = (Elf64_Sym *)((uintptr_t)mod->syms - (uintptr_t)mod->load_base + (uintptr_t)mod->load_virtbase);
  mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab - (uintptr_t)mod->load_base + (uintptr_t)mod->load_virtbase);

  // the kernel never allows W -> X transitions on code memory, so the RX
  // segments must be set first, straight from the freshly-mapped state;
  // everything else (data segments AND the alignment gaps lld leaves
  // between segments) becomes RW afterwards. build a page map to emit
  // each permission exactly once per page.
  const size_t num_pages = mod->load_size / 0x1000;
  uint8_t *is_x_page = calloc(num_pages, 1);
  if (!is_x_page) fatal_error("Error: out of memory in so_finalize");

  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    const size_t first = p->p_vaddr / 0x1000;
    const size_t last = (ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < num_pages; pg++)
      is_x_page[pg] = 1;
  }

  // pass 0 sets the executable runs to RX, pass 1 sets the rest to RW
  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < num_pages) {
      if (is_x_page[pg] != want_x) {
        pg++;
        continue;
      }
      size_t run_end = pg;
      while (run_end < num_pages && is_x_page[run_end] == want_x)
        run_end++;
      const u64 addr = (u64)mod->load_virtbase + pg * 0x1000;
      const u64 size = (run_end - pg) * 0x1000;
      rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), addr, size, want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc)) fatal_error("Error: could not map %u bytes of %s memory at %p:\n%08x", (u32)size, want_x ? "RX" : "RW", (void *)addr, rc);
      pg = run_end;
    }
  }

  free(is_x_page);
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;

  memset(mod, 0, sizeof(*mod));
  // store the basename so dlopen("/abs/path/libengine.so") can match
  const char *bn = strrchr(filename, '/');
  strncpy(mod->name, bn ? bn + 1 : filename, sizeof(mod->name) - 1);

  FILE *fd = fopen(filename, "rb");
  if (fd == NULL)
    return -1;

  /* Prefer stat(): ftell() after fseek(SEEK_END) returns -1 for some files on
   * the libnx FAT driver, and -1 widened to size_t is 0xFFFFFFFFFFFFFFFF,
   * which turns into a guaranteed malloc failure. */
  {
    struct stat st;
    if (stat(filename, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
      mod->so_size = (size_t)st.st_size;
    } else {
      fseek(fd, 0, SEEK_END);
      const long sz = ftell(fd);
      if (sz <= 0) {
        debugPrintf("so_load: %s size lookup failed (ftell=%ld)\n", filename, sz);
        fclose(fd);
        return -1;
      }
      mod->so_size = (size_t)sz;
      fseek(fd, 0, SEEK_SET);
    }
    if (mod->so_size < 4096 || mod->so_size > (size_t)512 * 1024 * 1024) {
      debugPrintf("so_load: %s implausible size %zu\n", filename, mod->so_size);
      fclose(fd);
      return -1;
    }
  }

  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    debugPrintf("so_load: %s malloc(%zu) failed -- temporary image\n", filename, mod->so_size);
    fclose(fd);
    return -2;
  }

  fread(mod->so_base, mod->so_size, 1, fd);
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("so_load: %s has too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4;
    goto err_free_so;
  }

  // save a pristine copy of the phdrs for dl_iterate_phdr
  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  // total size of the LOAD zone = highest vaddr+memsz of all LOAD segments
  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  // align total size to page size
  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  mod->load_base = base;
  if (!mod->load_base) goto err_free_so;
  memset(mod->load_base, 0, mod->load_size);

  // reserve virtual memory space for the entire LOAD zone
  virtmemLock();
  mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->load_memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
  virtmemUnlock();

  debugPrintf("%s: load base = %p, size = %u KB\n", mod->name, mod->load_virtbase, (u32)(mod->load_size / 1024));

  // copy segments to where they belong, then fix up the runtime phdrs to
  // point into the virtual address space
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    } else if (strcmp(sh_name, ".init_array") == 0) {
      mod->init_array = (void (**)(void))((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      mod->num_init = mod->sec_hdr[i].sh_size / 8;
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    res = -2;
    goto err_free_load;
  }

  // register in the global module list (append, load order matters for resolution)
  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }

  return 0;

err_free_load:
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;

  return res;
}

// find a dynamic tag in the file image; returns 0 if missing
static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
      for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag)
          return dyn->d_un.d_val;
    }
  }
  return 0;
}

// lowest PT_LOAD vaddr; mod->phdr is the pristine (pre-rebase) copy
static Elf64_Addr so_min_load_vaddr(so_module *mod) {
  Elf64_Addr min = (Elf64_Addr)~0ULL;
  for (int i = 0; i < mod->phnum; i++)
    if (mod->phdr[i].p_type == PT_LOAD && mod->phdr[i].p_vaddr < min)
      min = mod->phdr[i].p_vaddr;
  return (min == (Elf64_Addr)~0ULL) ? 0 : min;
}

// SLEB128 reader for Android's packed relocation stream.
static size_t so_sleb128(const uint8_t **pp, const uint8_t *end) {
  const uint8_t *p = *pp;
  size_t result = 0;
  int shift = 0;
  uint8_t byte = 0;
  while (p < end) {
    byte = *p++;
    result |= (size_t)(byte & 0x7F) << shift;
    shift += 7;
    if (!(byte & 0x80)) break;
  }
  if (shift < 64 && (byte & 0x40))
    result |= ~(size_t)0 << shift;
  *pp = p;
  return result;
}
// APS2 (DT_ANDROID_RELA / SHT_ANDROID_RELA) decoder.
//
// Layout (per the Android linker and lld):
//   'APS2'
//   sleb128 total_relocation_count
//   sleb128 initial r_offset
//   repeated groups until the count is exhausted:
//     sleb128 group_size
//     sleb128 group_flags
//     if (flags & 2) sleb128 shared r_offset delta
//     if (flags & 1) sleb128 shared r_info
//     if ((flags & 8) && (flags & 4)) sleb128 shared r_addend delta
//     per relocation in the group:
//       if !(flags & 2) sleb128 r_offset delta      (running)
//       if !(flags & 1) sleb128 r_info delta        (running)
//       if ((flags & 8) && !(flags & 4)) sleb128 r_addend delta (running)
//
// Flags: 1 GROUPED_BY_INFO, 2 GROUPED_BY_OFFSET_DELTA,
//        4 GROUPED_BY_ADDEND, 8 GROUP_HAS_ADDEND.
static void so_apply_android_rela(so_module *mod, Elf64_Addr vaddr, Elf64_Xword size,
                                  Elf64_Addr min_vaddr) {
  if (!vaddr || size < 16) return;
  if (vaddr < min_vaddr) return;
  const size_t off = (size_t)(vaddr - min_vaddr);
  if (off >= mod->load_size || size > mod->load_size - off) {
    debugPrintf("%s: APS2 block outside image\n", mod->name);
    return;
  }
  const uint8_t *base = (const uint8_t *)((uintptr_t)mod->load_base + off);
  const uint8_t *end  = base + size;
  if (memcmp(base, "APS2", 4) != 0) {
    debugPrintf("%s: APS2 magic missing (%02x %02x %02x %02x)\n", mod->name,
                base[0], base[1], base[2], base[3]);
    return;
  }
  const uint8_t *p = base + 4;

  const size_t total = so_sleb128(&p, end);
  intptr_t cur_off = (intptr_t)so_sleb128(&p, end);   // initial r_offset
  uint64_t running_info = 0;
  intptr_t running_add = 0;

  long applied = 0, skipped = 0, groups = 0;
  size_t done = 0;

  while (done < total && p < end) {
    const size_t group_size = so_sleb128(&p, end);
    if (group_size == 0) break;
    const size_t flags = so_sleb128(&p, end);
    groups++;
    if (groups > 100000) break;

    intptr_t shared_off = 0;
    uint64_t shared_info = 0;
    intptr_t shared_add = 0;
    if (flags & 2) shared_off  = (intptr_t)so_sleb128(&p, end);
    if (flags & 1) shared_info = (uint64_t)so_sleb128(&p, end);
    if ((flags & 8) && (flags & 4)) shared_add = (intptr_t)so_sleb128(&p, end);

    for (size_t i = 0; i < group_size && done < total; i++, done++) {
      if (flags & 2) {
        cur_off += shared_off;
      } else {
        cur_off += (intptr_t)so_sleb128(&p, end);
      }

      uint64_t info;
      if (flags & 1) {
        info = shared_info;
      } else {
        /* Measured on this module set: the info field is an absolute value, not a
         * delta. Accumulating turned R_AARCH64_ABS64 (257) into 514/771/1028... */
        info = (uint64_t)so_sleb128(&p, end);
      }

      intptr_t add = 0;
      if (flags & 8) {
        if (flags & 4) running_add += shared_add;
        else           running_add += (intptr_t)so_sleb128(&p, end);
        add = running_add;
      }

      const unsigned sym  = (unsigned)ELF64_R_SYM(info);
      const unsigned type = (unsigned)ELF64_R_TYPE(info);

      if (cur_off < (intptr_t)min_vaddr) { skipped++; continue; }
      const size_t slot_off = (size_t)(cur_off - (intptr_t)min_vaddr);
      if (slot_off + 8 > mod->load_size) { skipped++; continue; }
      uintptr_t *slot = (uintptr_t *)((uintptr_t)mod->load_base + slot_off);

      switch (type) {
        case R_AARCH64_RELATIVE:
          *slot = (uintptr_t)mod->load_virtbase + add;
          applied++;
          break;
        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
          if (sym < (size_t)mod->num_syms) {
            Elf64_Sym *s = &mod->syms[sym];
            if (s->st_shndx != SHN_UNDEF)
              *slot = (uintptr_t)mod->load_virtbase + s->st_value + add;
            applied++;
          } else skipped++;
          break;
#ifndef R_AARCH64_TLS_DTPMOD64
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLS_TPREL64  1030
#define R_AARCH64_TLSDESC      1031
#endif
        case R_AARCH64_TLS_DTPMOD64:
        case R_AARCH64_TLS_DTPREL64:
        case R_AARCH64_TLS_TPREL64:
        case R_AARCH64_TLSDESC:
          /* Static TLS in a .so: nothing meaningful to relocate here, and the
           * engine does not use __thread across module boundaries. Count them
           * separately so they stop inflating the unknown-type tally. */
          applied++;
          break;
        case R_AARCH64_NONE:
          applied++;
          break;
        default:
          if (skipped < 4)
            debugPrintf("%s: APS2 unknown type %u at %llx\n",
                        mod->name, type, (unsigned long long)cur_off);
          skipped++;
          break;
      }
    }
  }
  debugPrintf("%s: APS2 total=%zu %ld groups, %ld applied, %ld skipped\n",
              mod->name, total, groups, applied, skipped);
}

// Walk one RELA table located at a link-time vaddr. Diagnostics instead of
// dying: one malformed entry must not cost the whole module.
static void so_apply_rela(so_module *mod, Elf64_Addr vaddr, Elf64_Xword size,
                          Elf64_Xword ent, Elf64_Addr min_vaddr) {
  if (!vaddr || !size) return;
  if (ent < sizeof(Elf64_Rela)) ent = sizeof(Elf64_Rela);
  if (vaddr < min_vaddr) {
    debugPrintf("%s: reloc vaddr %llx below min %llx\n",
                mod->name, (unsigned long long)vaddr, (unsigned long long)min_vaddr);
    return;
  }
  const size_t off = (size_t)(vaddr - min_vaddr);
  if (off >= mod->load_size || size > mod->load_size - off) {
    debugPrintf("%s: reloc table out of range off=%zu size=%llu load=%zu\n",
                mod->name, off, (unsigned long long)size, mod->load_size);
    return;
  }
  Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + off);
  const int nrel = (int)(size / ent);
  int bad_off = 0, bad_sym = 0, bad_type = 0;
  for (int j = 0; j < nrel; j++) {
    if (rels[j].r_offset >= mod->load_size) { bad_off++; continue; }
    const unsigned symidx = (unsigned)ELF64_R_SYM(rels[j].r_info);
    if (symidx >= (unsigned)mod->num_syms) { bad_sym++; continue; }
    uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
    Elf64_Sym *sym = &mod->syms[symidx];
    const int type = ELF64_R_TYPE(rels[j].r_info);
    switch (type) {
      case R_AARCH64_ABS64:
        if (sym->st_shndx == SHN_UNDEF) *ptr = rels[j].r_addend;
        else *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
        break;
      case R_AARCH64_RELATIVE:
        *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
        break;
      case R_AARCH64_GLOB_DAT:
      case R_AARCH64_JUMP_SLOT:
        if (sym->st_shndx != SHN_UNDEF)
          *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
        break;
#ifndef R_AARCH64_TLS_DTPMOD64
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLS_TPREL64  1030
#define R_AARCH64_TLSDESC      1031
#endif
      case R_AARCH64_TLS_DTPMOD64:
        case R_AARCH64_TLS_DTPREL64:
        case R_AARCH64_TLS_TPREL64:
        case R_AARCH64_TLSDESC:
          /* Static TLS in a .so: nothing meaningful to relocate here, and the
           * engine does not use __thread across module boundaries. Count them
           * separately so they stop inflating the unknown-type tally. */
          applied++;
          break;
        case R_AARCH64_NONE:
        break;
      default:
        bad_type++;
        if (bad_type <= 4)
          debugPrintf("%s: reloc %d/%d unknown type %x (offset %llx)\n",
                      mod->name, j, nrel, type, (unsigned long long)rels[j].r_offset);
        break;
    }
  }
  if (bad_off || bad_sym || bad_type)
    debugPrintf("%s: reloc %d entries, skipped %d off / %d sym / %d type\n",
                mod->name, nrel, bad_off, bad_sym, bad_type);
}

// RELR/ANDROID_RELR packed relative relocations;
// each location gets the load base added to the value stored in the file
static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz,
                            Elf64_Addr min_vaddr) {
  uintptr_t where = 0;
  int applied = 0, skipped = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      // even entry: address of the next relocation
      where = (uintptr_t)entry;
      if (where >= min_vaddr && (size_t)(where - min_vaddr) + 8 <= mod->load_size) {
        *(uint64_t *)((uintptr_t)mod->load_base + (where - min_vaddr)) += (uint64_t)mod->load_virtbase;
        applied++;
      } else skipped++;
      where += 8;
    } else {
      // odd entry: bitmap for the next 63 words
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit)) {
          const uintptr_t off = where + (uintptr_t)(bit - 1) * 8;
          if (off >= min_vaddr && (size_t)(off - min_vaddr) + 8 <= mod->load_size) {
            *(uint64_t *)((uintptr_t)mod->load_base + (off - min_vaddr)) += (uint64_t)mod->load_virtbase;
            applied++;
          } else skipped++;
        }
      }
      where += 63 * 8;
    }
  }
  debugPrintf("%s: RELR %zu entries -> %d applied, %d skipped\n",
              mod->name, count, applied, skipped);
}

int so_relocate(so_module *mod) {
  const Elf64_Addr min_vaddr = so_min_load_vaddr(mod);

  const Elf64_Xword rela     = so_dynamic_tag(mod, DT_RELA);
  const Elf64_Xword relasz   = so_dynamic_tag(mod, DT_RELASZ);
  const Elf64_Xword relaent  = so_dynamic_tag(mod, DT_RELAENT);
  const Elf64_Xword jmprel   = so_dynamic_tag(mod, DT_JMPREL);
  const Elf64_Xword pltrelsz = so_dynamic_tag(mod, DT_PLTRELSZ);

  Elf64_Xword relr_off  = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off  = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }

  const Elf64_Xword arela   = so_dynamic_tag(mod, DT_ANDROID_RELA);
  const Elf64_Xword arelasz = so_dynamic_tag(mod, DT_ANDROID_RELASZ);

  debugPrintf("%s: tags RELA=%llx/%llu APS2=%llx/%llu RELR=%llx/%llu JMPREL=%llx/%llu\n",
              mod->name,
              (unsigned long long)rela, (unsigned long long)relasz,
              (unsigned long long)arela, (unsigned long long)arelasz,
              (unsigned long long)relr_off, (unsigned long long)relr_size,
              (unsigned long long)jmprel, (unsigned long long)pltrelsz);

  /* Section headers in this module set are unreliable: .rela.dyn is
   * SHT_ANDROID_RELA with sh_entsize==1, and the payload is an APS2 stream,
   * not an array of Elf64_Rela. Nothing here consults them any more. */
  if (rela && relasz)
    so_apply_rela(mod, rela, relasz, relaent, min_vaddr);

  if (arela && arelasz)
    so_apply_android_rela(mod, arela, arelasz, min_vaddr);

  /* RELATIVE relocations first, then symbol slots land on top of them. */
  if (relr_off && relr_size) {
    if (relr_off >= min_vaddr && (size_t)(relr_off - min_vaddr) < mod->load_size)
      so_process_relr(mod,
                      (const Elf64_Xword *)((uintptr_t)mod->load_base + (relr_off - min_vaddr)),
                      (size_t)relr_size, min_vaddr);
    else
      debugPrintf("%s: RELR offset %llx outside image\n",
                  mod->name, (unsigned long long)relr_off);
  }

  if (jmprel && pltrelsz)
    so_apply_rela(mod, jmprel, pltrelsz, relaent, min_vaddr);

  return 0;
}

// look up an exported (defined) symbol in a single module;
// returns its virtual (runtime) address or 0
uintptr_t so_lookup_export(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

uintptr_t so_lookup_export_all(const char *name) {
  for (so_module *m = so_list; m; m = m->next) {
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }
  return 0;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  // The import table takes priority over module exports so the host can
  // override anything (e.g. SDL_*, libc); everything else falls through to
  // the other Source modules' exports (CreateInterface, tier0 API, ...).
  for (int k = 0; k < num_funcs; k++) {
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;
  }

  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }

  return 0;
}

// Resolve UNDEF imports referenced by one RELA table at a link-time vaddr.
static void so_resolve_table(so_module *mod, Elf64_Addr vaddr, Elf64_Xword size,
                             Elf64_Xword ent, Elf64_Addr min_vaddr,
                             DynLibFunction *funcs, int num_funcs,
                             int taint_missing_imports, int *missing) {
  if (!vaddr || !size) return;
  if (ent < sizeof(Elf64_Rela)) ent = sizeof(Elf64_Rela);
  if (vaddr < min_vaddr) return;
  const size_t off = (size_t)(vaddr - min_vaddr);
  if (off >= mod->load_size || size > mod->load_size - off) {
    debugPrintf("%s: resolve table out of range off=%zu size=%llu\n",
                mod->name, off, (unsigned long long)size);
    return;
  }
  Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + off);
  const int nrel = (int)(size / ent);
  for (int j = 0; j < nrel; j++) {
    if (rels[j].r_offset >= mod->load_size) continue;
    const unsigned symidx = (unsigned)ELF64_R_SYM(rels[j].r_info);
    if (symidx >= (unsigned)mod->num_syms) continue;
    const int type = ELF64_R_TYPE(rels[j].r_info);
    if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
        type != R_AARCH64_JUMP_SLOT)
      continue;
    Elf64_Sym *sym = &mod->syms[symidx];
    if (sym->st_shndx != SHN_UNDEF) continue;
    uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
    char *name = mod->dynstrtab + sym->st_name;
    uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
    if (addr) {
      *ptr = addr + rels[j].r_addend;
    } else {
      (*missing)++;
      debugPrintf("%s: unresolved import: %s\n", mod->name, name);
      // taint so an unresolved call crashes at an obvious address
      if (taint_missing_imports)
        *ptr = rels[j].r_offset;
    }
  }
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int missing = 0;
  const Elf64_Addr min_vaddr = so_min_load_vaddr(mod);

  const Elf64_Xword rela     = so_dynamic_tag(mod, DT_RELA);
  const Elf64_Xword relasz   = so_dynamic_tag(mod, DT_RELASZ);
  const Elf64_Xword relaent  = so_dynamic_tag(mod, DT_RELAENT);
  const Elf64_Xword jmprel   = so_dynamic_tag(mod, DT_JMPREL);
  const Elf64_Xword pltrelsz = so_dynamic_tag(mod, DT_PLTRELSZ);

  /* Same rule as so_relocate: the section headers are not trustworthy, so the
   * section-header fallback is gone. Symbol slots live in DT_RELA and
   * DT_JMPREL; RELATIVE entries carry no symbols and need no resolving. */
  if (rela && relasz)
    so_resolve_table(mod, rela, relasz, relaent, min_vaddr,
                     funcs, num_funcs, taint_missing_imports, &missing);

  if (jmprel && pltrelsz)
    so_resolve_table(mod, jmprel, pltrelsz, relaent, min_vaddr,
                     funcs, num_funcs, taint_missing_imports, &missing);

  if (missing)
    debugPrintf("%s: %d unresolved imports\n", mod->name, missing);

  return 0;
}

void so_execute_init_array(so_module *mod) {
  // uses the location captured at load time: safe to run after so_free_temp
  for (int i = 0; i < mod->num_init; i++) {
    if (mod->init_array[i])
      mod->init_array[i]();
  }
}

// pre-finalize only: address inside the RW mirror, for hooking/patching
uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
  }

  fatal_error("Error: could not find symbol:\n%s", symbol);
  return 0;
}

uintptr_t so_find_addr_with_size(so_module *mod, const char *symbol, size_t *symbol_size) {
  if (symbol_size)
    *symbol_size = 0;

  for (int i = 0; i < mod->num_syms; i++) {
    const char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0) {
      if (symbol_size)
        *symbol_size = mod->syms[i].st_size;
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
    }
  }

  // Android's GameUI keeps useful local function names in .symtab, but they
  // are not part of .dynsym and therefore are not present in the load image.
  // Consult the temporary ELF file image while it is still available.
  if (!mod->so_base)
    return 0;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const Elf64_Shdr *symtab = &mod->sec_hdr[i];
    if (symtab->sh_type != SHT_SYMTAB ||
        symtab->sh_link >= mod->elf_hdr->e_shnum ||
        symtab->sh_entsize != sizeof(Elf64_Sym) ||
        symtab->sh_offset + symtab->sh_size > mod->so_size)
      continue;

    const Elf64_Shdr *strtab = &mod->sec_hdr[symtab->sh_link];
    if (strtab->sh_type != SHT_STRTAB ||
        strtab->sh_offset + strtab->sh_size > mod->so_size)
      continue;

    const Elf64_Sym *symbols =
        (const Elf64_Sym *)((uintptr_t)mod->so_base + symtab->sh_offset);
    const char *strings = (const char *)mod->so_base + strtab->sh_offset;
    const size_t count = symtab->sh_size / sizeof(*symbols);

    for (size_t j = 0; j < count; j++) {
      if (symbols[j].st_shndx == SHN_UNDEF ||
          symbols[j].st_name >= strtab->sh_size ||
          symbols[j].st_value >= mod->load_size ||
          symbols[j].st_size > mod->load_size - symbols[j].st_value)
        continue;
      if (strcmp(strings + symbols[j].st_name, symbol) == 0) {
        if (symbol_size)
          *symbol_size = symbols[j].st_size;
        return (uintptr_t)mod->load_base + symbols[j].st_value;
      }
    }
  }

  return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s", symbol);
  return addr;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL)
    return -1;

  if (mod->so_base) {
    // someone forgot to free the temp data
    so_free_temp(mod);
  }

  // remap everything as RW
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    const u64 seg_start = ((u64)mod->load_virtbase + p->p_vaddr) & ~0xFFFull;
    const u64 seg_end = ALIGN_MEM((u64)mod->load_virtbase + p->p_vaddr + p->p_memsz, 0x1000);
    svcSetProcessMemoryPermission(envGetOwnProcessHandle(), seg_start, seg_end - seg_start, Perm_Rw);
  }
  // unmap everything
  svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)mod->load_virtbase, (u64)mod->load_base, mod->load_size);

  // release virtual address range
  virtmemLock();
  virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();

  // remove from list
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next) {
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
    }
  }

  return 0;
}

// matches the layout bionic/libunwind expect
struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr; // link-time vaddrs + dlpi_addr = runtime
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}
