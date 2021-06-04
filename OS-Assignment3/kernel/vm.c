#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

void in(int page){
  struct proc * p = myproc();
  if(p->tail == MAX_TOTAL_PAGES - 1)
    p->tail = -1;
  p->tail += 1;
  p->pages[p->tail] = page;
  p->numOfPages += 1;
}

void out(){
  struct proc* p = myproc();
  if(p->head == MAX_TOTAL_PAGES - 1)
    p->head = 0;
  else
    p->head += 1;  
  p->numOfPages -= 1;
}

// removing all pages and then inserting them again but without the specified page
void
removePage(int pageNumber){
  struct proc * p = myproc();
  for(int i = 0; i < p->numOfPages; i++){
    int page = p->pages[p->head];
    out();
    if(page != pageNumber)
      in(page);
  }
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){    
    if((pte = walk(pagetable, a, 0)) != 0){
      if((*pte & PTE_V) != 0){
        if(PTE_FLAGS(*pte) == PTE_V)
          panic("uvmunmap: not a leaf");
        if(do_free){
          uint64 pa = PTE2PA(*pte);
          kfree((void*)pa);
          #if SELECTION != NONE
            if(a/PGSIZE < 32){
              // no longer will be in memory
              myproc()->data[a/PGSIZE].inUse = 0;
              myproc()->pagesInMemory = (myproc()->pagesInMemory == 0) ? 0 : myproc()->pagesInMemory - 1;
              myproc()->data[a/PGSIZE].offset = -1;
              // removing the specified page (not in memory) from the queue
              removePage(a/PGSIZE);
            }
          #endif
        }
      }
      else{
        #if SELECTION != NONE
          if(a/PGSIZE < 32)
            myproc()->data[a/PGSIZE].offset = -1;
        #endif
      }
      // remove it to avoid page out
      *pte = 0; 
    }
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
NONE_uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// returning the first empty offset for a page that is not in use
uint
getOffset()
{
  struct proc* p = myproc();
  uint found = 0;
  for(int i = 0; i < p->sz && !found; i += PGSIZE){
    int inUse = 0;
    for(int j = 0; j < MAX_TOTAL_PAGES && !inUse; j++)
      if(p->data[j].offset == i)
        inUse = 1; 
    if(inUse == 0)
      return i;
  }
  return found;
}

uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  #if SELECTION == NONE
    return NONE_uvmalloc(pagetable, oldsz, newsz);
  #endif
  char *mem;
  uint64 a;
  struct proc* p = myproc();
  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    if(p->pid > 1){
      uint64 numOfPages = a/PGSIZE;
      if(numOfPages > MAX_TOTAL_PAGES)
        return -1;
      if(p->pagesInMemory >= MAX_PSYC_PAGES){
        page_to_file(p, getOffset());
        mem = kalloc();
        if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U) < 0) {
          uvmdealloc(pagetable, newsz, oldsz);
          return 0;
        }
        p->data[numOfPages].inUse = 1;
        p->pagesInMemory += 1;
        p->data[numOfPages].offset = -1;
        p->data[numOfPages].agingCounter = initAging(numOfPages);
      }
      else {
        mem = kalloc();
        if(mem == 0){
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        memset(mem, 0, PGSIZE);
        if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) < 0){
          kfree(mem);
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        p->data[numOfPages].inUse = 1;
        p->pagesInMemory += 1;
        p->data[numOfPages].agingCounter = initAging(numOfPages);
      }
    }
    else {
      mem = kalloc();
        if(mem == 0){
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        memset(mem, 0, PGSIZE);
        if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
          kfree(mem);
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){    
    if((pte = walk(old, i, 0)) !=0 && (*pte & PTE_V) != 0){
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      if((mem = kalloc()) == 0)
        goto err;
      memmove(mem, (char*)pa, PGSIZE);
      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto err;
      }
    }
    
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// Returns the min aging counter pageIndex which it's in use
int
nfua(struct proc* p)
{
  uint min = (1L << 31);
  int index = -1;
  for(int i = 3; i < MAX_TOTAL_PAGES; i++){ 
    if(p->data[i].inUse && p->data[i].agingCounter < min){
      min = p->data[i].agingCounter;
      index = i;
    }
  }
  return index;
}

// counts the number of 1's in age (GEEKS FOR GEEKS)
int 
countOnes(uint age){
  unsigned int count = 0;
  while(age){
    count += age & 1;
    age >>= 1;
  }
  return count;
}

int 
lapa(struct proc* p){
  int minOnes = -1;
  int minAge = -1;
  int pageIndex = -1;
  for(int i = 3; i < MAX_TOTAL_PAGES; i++){
    if(p->data[i].inUse ){
      uint age = p->data[i].agingCounter;
      int numOfOnes = countOnes(age);
      // if number of 1's is too small * or * it equals and then we take min age
      if(minOnes == -1 || numOfOnes < minOnes || (numOfOnes == minOnes && age < minAge)){
        minOnes = numOfOnes;
        minAge = age;
        pageIndex = i;
      }
    }
  }
  return pageIndex;
}

int
scfifo(struct proc* p)
{
  int page;
  for(int i = 0; i < p->numOfPages; i++){
    page = p->pages[p->head];
    pte_t * pte = walk(p->pagetable, page*PGSIZE, 0);
    uint pte_flags = PTE_FLAGS(*pte);
    if((pte_flags & PTE_A)){
      *pte = *pte & (~PTE_A);
      out();
      in(page);
    }
    else{ 
      out();
      return page;
    }
  }
  page = p->pages[p->head];
  out();
  return page;
}

// By algorithm, returns which page index should we swap to file
int
getIndexToRemove(void)
{
  #if SELECTION == NFUA
    struct proc* p = myproc();
    return nfua(p);
  #endif
  #if SELECTION == LAPA
    struct proc* p = myproc();
    return lapa(p);
  #endif
  #if SELECTION == SCFIFO
    struct proc* p = myproc();
    return scfifo(p);
  #endif
  return 0;
}

// swaps a page in ram to the file
void
page_to_file(struct proc* p, uint pageOffset)
{
  int index = getIndexToRemove();
  pte_t *pte =  walk(p->pagetable, index*PGSIZE, 0); 
  uint64 pha = PTE2PA(*pte);
  if(writeToSwapFile(p, (char*)pha, pageOffset, PGSIZE) < 0)
    panic("write to file failed");
  kfree((void*)pha);
  *pte = (*pte & (~PTE_V)) | PTE_PG;
  p->data[index].offset = pageOffset;
  p->data[index].inUse = 0;
  p->pagesInMemory -= 1;
}

void
swap_in(struct proc * p, uint64 va, pte_t* pte)
{
  int missingPageIndex = PGROUNDDOWN(va) / PGSIZE;
  if(p->data[missingPageIndex].offset == -1)
    panic("Fail in handling page fault");
  uint pageOffset = p->data[missingPageIndex].offset;
  char* buff;
  if((buff = kalloc()) == 0)
    panic("Fail in kalloc while handling page fault");
  readFromSwapFile(p, buff, pageOffset, PGSIZE);
  if(p->pagesInMemory < MAX_PSYC_PAGES) {
    *pte = PA2PTE((uint64)buff) | PTE_V;
  }
  else {
    page_to_file(p, pageOffset);
    *pte = PA2PTE((uint64)buff) | ((PTE_FLAGS(*pte) & ~PTE_PG) | PTE_V);
  }
  p->data[missingPageIndex].agingCounter = initAging(missingPageIndex);
  p->data[missingPageIndex].offset = -1;
  p->data[missingPageIndex].inUse = 1;
  p->pagesInMemory += 1;
  sfence_vma();
}

// initiats aging foreach page inserted in memory
uint
initAging(int page)
{
  #if SELECTION == NFUA
    return 0;
  #endif
  #if SELECTION == LAPA
    return 0xFFFFFFFF;
  #endif
  #if SELECTION == SCFIFO
    in(page);
    return 0;
  #endif
  return 0;
}

// updates the aging counter foreach page when returning to the scheduler
void
updateAging(void)
{
  #if SELECTION == NFUA || SELECTION == LAPA
    struct proc* p = myproc();
    for(int i = 0; i < MAX_TOTAL_PAGES; i++){
      pte_t* pte = walk(p->pagetable, i*PGSIZE, 0);
      if(*pte & PTE_V){
        p->data[i].agingCounter = p->data[i].agingCounter >> 1;
        // if the page accessed, then it will get high aging counter
        if(*pte & PTE_A){
          p->data[i].agingCounter |= (1L << 31);
          *pte = *pte & (~PTE_A);
        }
      }
    }
  #endif
}