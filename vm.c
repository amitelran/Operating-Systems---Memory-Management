#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"


void set_present_clear_pagedout(pde_t *pgdir, char *virtAddr);
void clear_present_set_pagedout(pde_t *pgdir, char *virtAddr);
void addNewPageToPhysMem(char * mem);
int mem2File(struct page_in_mem * pageToSwapFromMem);
struct page_in_mem * choosePageFromPhysMem(void);
int writePageToFile(struct page_in_mem * pageToSwapToFile);
int file2mem(struct page_in_mem * pageToSwapFromFile);
int swapPages(uint virtAddr);
void updateListPointers(struct page_in_mem * pageInList);
int deletePage(struct page_in_mem * pageToDelete);
int check_PTE_A(pde_t *pgdir, char *virtAddr);
void clear_PTE_A(pde_t *pgdir, char *virtAddr);
void movePageToListEndSCFIFO(struct page_in_mem * pageToMoveToTail);
void set_PTE_A(pde_t *pgdir, char *virtAddr);

// TASK 1-2: In case of freevm --> should use the global system page directory
uint use_system_pgdir = 0;
pde_t *system_pgdir;



extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
struct segdesc gdt[NSEGS];

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpunum()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);

  // Map cpu, and curproc
  c->gdt[SEG_KCPU] = SEG(STA_W, &c->cpu, 8, 0);

  lgdt(c->gdt, sizeof(c->gdt));
  loadgs(SEG_KCPU << 3);
  
  // Initialize cpu-local storage.
  cpu = c;
  proc = 0;
}



// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)p2v(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table 
    // entries, if necessary.
    *pde = v2p(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}


pte_t *
walkpgdirForProc(pde_t *pgdir, const void *virtualAddres, int alloc)
{
  return walkpgdir(pgdir, virtualAddres, alloc);
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;
  
  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
// 
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP, 
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;
  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;

  memset(pgdir, 0, PGSIZE);

  if (p2v(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");

  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start, 
                (uint)k->phys_start, k->perm) < 0)
      return 0;

  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(v2p(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  pushcli();
  cpu->gdt[SEG_TSS] = SEG16(STS_T32A, &cpu->ts, sizeof(cpu->ts)-1, 0);
  cpu->gdt[SEG_TSS].s = 0;
  cpu->ts.ss0 = SEG_KDATA << 3;
  cpu->ts.esp0 = (uint)proc->kstack + KSTACKSIZE;
  ltr(SEG_TSS << 3);
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");
  lcr3(v2p(p->pgdir));  // switch to new address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;
  
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, v2p(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;
  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, p2v(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;
  int check;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);

  for(; a < newsz; a += PGSIZE)
  {
      if ((SELECTION != NONE) && (proc->pid > 2))
      {
        // Check if exceeded number of pages in physical memory, if so, write pages to file
        if (proc->num_pages_in_phys_mem == MAX_PSYC_PAGES)
        {
          if (proc->num_pages_in_file == (MAX_TOTAL_PAGES - MAX_PSYC_PAGES)){     // Check if reached maximum number of pages
              cprintf("allocuvm: Reached limit of pages per process, cannot allocate pages for process\n");
              return 0;
          }

          // If no swap file exists for process --> create a swap file
          else if (proc->swapFile == 0)
          {
            createSwapFile(proc);
            phys_mem_pages_stats.number_of_swap_files++;
          }
          if ((check = mem2File(choosePageFromPhysMem())) == -1)
          {
            return 0;
          }
        }
      }

      mem = kalloc();
      // If not able to allocate physical memory to page
      if (mem == 0){
          cprintf("allocuvm out of memory\n");
          deallocuvm(pgdir, newsz, oldsz);
          return 0;
      }
      memset(mem, 0, PGSIZE);
      mappages(pgdir, (char*)a, PGSIZE, v2p(mem), PTE_W|PTE_U);
      if ((SELECTION != NONE))
      {
        addNewPageToPhysMem((char *)a);                // Add the new allocated page to physical memory
      }
  }
  return newsz;
}


// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{ 
  struct page_in_mem * pageInArray;
  pte_t *pte;
  uint a, pa;
  int found;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE) 
  {
    found = 0;
    pte = walkpgdir(pgdir, (char*)a, 0);
    if (!pte)
    {
      a += (NPTENTRIES - 1) * PGSIZE;
    }

    // Entry in page table is present
    else if ((*pte & PTE_P) != 0)
    {
      pa = PTE_ADDR(*pte);
      if (pa == 0)
      {
        cprintf("deallocuvm: process %d is about to panic, pages in memory: %d, pages in file: %d\n", proc->pid, proc->num_pages_in_phys_mem, proc->num_pages_in_file);
        printPageArray();
        panic("kfree in deallocuvm");
      }
      char *v = p2v(pa);
      kfree(v);
      *pte = 0;

      // TASK 2: check that the page directory is our process's page directory (and not the father's in fork)
      if ((SELECTION != NONE) && (proc->pgdir == pgdir))
      {
        for (pageInArray = &proc->paging_meta_data[0]; pageInArray < &proc->paging_meta_data[MAX_TOTAL_PAGES]; pageInArray++)
        {
          if (pageInArray->pg.virtualAddr == a)     // If found (according to address), break
          {
            found = 1;
            break;
          }   
        }
        if (found)
        {
          deletePage(pageInArray);  
        }
      }
    }

    // Entry is not present --> checking if in swap file
    else if ((SELECTION != NONE) && (*pte & PTE_PG) && (pgdir == proc->pgdir)) 
    {
      uint vAddr = PTE_ADDR((uint) a);
      for (pageInArray = &proc->paging_meta_data[0]; pageInArray < &proc->paging_meta_data[MAX_TOTAL_PAGES]; pageInArray++)
      {
        if (pageInArray->pg.virtualAddr == vAddr)     // If found (according to address), break
        {
          found = 1;
          break;
        }   
      }

      // Found --> initialize page's field in array
      if (found)
      {
        proc->num_pages_in_file--;
        pageInArray->pg.virtualAddr = 0xFFFFFFFF;           
        pageInArray->pg.num_times_accessed = 0;            
        pageInArray->pg.offset_in_file = -1;                
        pageInArray->pg.state = NOTUSED;      
      }
      *pte = 0;
    }
  }
  return newsz;
}


/**********************************************************************************************************************************/
/*****************************  TASK 1: Add a NEW page to the physical memory\Linked List\Pages array *****************************/


void
addNewPageToPhysMem(char * addr)
{
  struct page_in_mem * tailPage = proc->mem_pages_head;
  struct page_in_mem * pageInArray = 0;
  int page_array_index = 0;
  int found = 0;

  // If head of physical memory pages is null --> set it's virtual address 
  if (!tailPage)
  {
      pageInArray = &proc->paging_meta_data[0];
      proc->mem_pages_head = pageInArray;
  }

  // Search for last link in physical memory pages list --> set the next link virtual address
  else
  {
    while (tailPage->next != 0)
    {
        tailPage = tailPage->next;
    }

    // Search if already exists entry in page directory 
    for(pageInArray = &proc->paging_meta_data[0]; pageInArray < &proc->paging_meta_data[MAX_TOTAL_PAGES]; pageInArray++)
    {
      if (pageInArray->pg.virtualAddr == PTE_ADDR((uint) addr))
      {
        found = 1;
        break;
      }   
      page_array_index++;
    } 

    // If entry doesn't exist, add a new page
    if (!found)
    {
      page_array_index = 0;
      for(pageInArray = &proc->paging_meta_data[0]; pageInArray < &proc->paging_meta_data[MAX_TOTAL_PAGES]; pageInArray++)
      {
        if(pageInArray->pg.state == NOTUSED)
        {
          break;
        }   
        page_array_index++;
      } 
    }
   
    tailPage->next = pageInArray;
    pageInArray->prev = tailPage; 
    proc->paging_meta_data[tailPage->pg.pages_array_index] = *tailPage;
  }

  // Initialize page struct fields
  pageInArray->pg.pages_array_index = page_array_index;
  pageInArray->next = 0;
  pageInArray->pg.virtualAddr = (uint) addr;
  pageInArray->pg.state = PHYSMEMORY;
  pageInArray->pg.num_times_accessed = 0;
  pageInArray->pg.offset_in_file = -1;
  
  // Update array of pages
  proc->paging_meta_data[page_array_index] = *pageInArray;

  proc->num_pages_in_phys_mem++;   
}



/**********************************************************************************************************************************/
/*****************************  TASK 1: Remove page from the physical memory\Linked List and write to swap file  *****************************************/



int
mem2File(struct page_in_mem * pageToSwapFromMem)
{
  int check;
  pte_t *pte;

  // Check if should use the system global pgdir
  pte_t * pgdir = proc->pgdir;
  if (use_system_pgdir == 1)
  {
    pgdir = system_pgdir;
  }

  pte = walkpgdir(pgdir, (char*)pageToSwapFromMem->pg.virtualAddr, 0);

  // Write page to swap file from page's virtual address
  if ((check = writePageToFile(pageToSwapFromMem)) == -1)
  {
    return -1;
  }

  // Update list pointers
  updateListPointers(pageToSwapFromMem);

  proc->paging_meta_data[pageToSwapFromMem->pg.pages_array_index] = *pageToSwapFromMem;

   // Update relevant bits
  clear_present_set_pagedout(pgdir, (char*) pageToSwapFromMem->pg.virtualAddr);      // Clear PTE_P and set PTE_PG bits
  clear_PTE_A(pgdir, (char *)pageToSwapFromMem->pg.virtualAddr);                     // Clear PTE_A (accessed flag) bit

  uint pAddr = PTE_ADDR(*pte);
  char * vAddr = p2v(pAddr);
  kfree(vAddr);
  proc->num_pages_in_phys_mem--;
  proc->number_of_paging_out_times++;
  lcr3(v2p (pgdir));
  return 0;
}



/**********************************************************************************************************************************/
/*****************************  TASK 1: Write page to file*****************************/


int
writePageToFile(struct page_in_mem * pageToSwapToFile)
{
  int check;
  int offsetExists = 0;

  // Check if should use the system global pgdir
  pte_t * pgdir = proc->pgdir;
  if (use_system_pgdir == 1)
  {
    pgdir = system_pgdir;
  }
  pte_t *pte;
  pte = walkpgdir(pgdir, (char*)pageToSwapToFile->pg.virtualAddr, 0);
  if (pte == 0)
  {
    panic ("writePageToFile: page should exist\n");   
  }
  if (!(*pte & PTE_P))
  {
    panic ("writePageToFile: page should be present\n"); 
  }

  // Write page to swap file (if offset is not 0, swap into the page's offset, else swap into the end of file)
  if (pageToSwapToFile->pg.offset_in_file > -1)
  {
    offsetExists = 1;
    if ((check = writeToSwapFile(proc, (char*) pageToSwapToFile->pg.virtualAddr, pageToSwapToFile->pg.offset_in_file, PGSIZE)) == -1)
    {
      cprintf("ERROR in writePageToFile first if\n");
      return -1;
    }
  }
  else if ((check = writeToSwapFile(proc, (char*) pageToSwapToFile->pg.virtualAddr, PGSIZE * (proc->num_pages_in_file), PGSIZE)) == -1)
  {
    cprintf("ERROR in writePageToFile second if\n");
    return -1;
  }

  // If new page into file --> set offset in file
  if (!offsetExists){
    pageToSwapToFile->pg.offset_in_file = PGSIZE * (proc->num_pages_in_file);
  }

  // Set state and flags
  pageToSwapToFile->pg.state = SWAPFILE;
  pageToSwapToFile->pg.num_times_accessed = 0;
  proc->num_pages_in_file++;
  cprintf("writePage: process %d, wrote page number %d to File. Number of pages in file: %d\n", proc->pid, pageToSwapToFile->pg.pages_array_index, proc->num_pages_in_file);
  return 0;
}


/**********************************************************************************************************************************/
/*****************************  TASK 1: Read page from file *****************************/


int
file2mem(struct page_in_mem * pageToSwapFromFile)
{
  int check;
  int offset = pageToSwapFromFile->pg.offset_in_file;

  // Read page buffer from file to buffer 
  if (offset < 0)
  {
    cprintf("file2mem  : page not in file\n");
    return -1;
  }

  // Check if should use the system global pgdir
  pte_t * pgdir = proc->pgdir;
  if (use_system_pgdir == 1)
  {
    pgdir = system_pgdir;
  }

  pte_t * pte;
  pte = walkpgdir(pgdir, (void *)(PTE_ADDR(pageToSwapFromFile->pg.virtualAddr)), 0);
  char * memInPgdir;
  if((memInPgdir = kalloc()) == 0)
  {
    panic("file2mem: out of memory\n");
  }

  mappages(pgdir, (char *) pageToSwapFromFile->pg.virtualAddr, PGSIZE, v2p(memInPgdir), PTE_W | PTE_U);
  if ((check = readFromSwapFile(proc, memInPgdir, offset, PGSIZE)) == -1)
  {
    cprintf("file2mem: READ FAIL\n");
    return -1;
  }

  if ((pte = walkpgdir(pgdir, (void*)pageToSwapFromFile->pg.virtualAddr, 0)) == 0) {
    panic("file2mem: pte should exist");
  }
  
  set_present_clear_pagedout(pgdir, (char *) pageToSwapFromFile->pg.virtualAddr);

  proc->num_pages_in_phys_mem++; 

  // Update page's fields
  pageToSwapFromFile->pg.num_times_accessed = 0;
  pageToSwapFromFile->pg.offset_in_file = -1;
  pageToSwapFromFile->pg.state = PHYSMEMORY;

  proc->num_pages_in_file--;

  // Insert page to physical memory pages list
  struct page_in_mem * tailPage = proc->mem_pages_head;
  while (tailPage->next != 0)
  {
      tailPage = tailPage->next;
  }
  tailPage->next = pageToSwapFromFile;
  pageToSwapFromFile->prev = tailPage;
  pageToSwapFromFile->next = 0;

  
  // Update array of pages
  proc->paging_meta_data[tailPage->pg.pages_array_index] = *tailPage;
  proc->paging_meta_data[pageToSwapFromFile->pg.pages_array_index] = *pageToSwapFromFile;
  return 0;
}



/**********************************************************************************************************************************/
/*****************************  TASK 1: Swap between page in physical memory and desired page from File  *****************************************/


int
swapPages(uint virtAddr)
{
  cprintf("swapPages starts\n");
  struct page_in_mem * pageToSwapToFile = choosePageFromPhysMem();          // Get the chosen page by policy to remove from physical memory to the file
  struct page_in_mem * pageToSwapToMem;
  int found = 0;

  // Look for required page entry in paging meta data array
  for (pageToSwapToMem = &proc->paging_meta_data[0]; pageToSwapToMem < &proc->paging_meta_data[MAX_TOTAL_PAGES]; pageToSwapToMem++)
  {
    if (pageToSwapToMem->pg.virtualAddr == virtAddr)
    {
      found = 1;
      break;
    }   
  } 

  // If didn't find the required page, return -1 error
  if (!found)
  {
    return -1;
  }

  // Update the offset where to insert to file the page to swap out
  pageToSwapToFile->pg.offset_in_file = pageToSwapToMem->pg.offset_in_file;  

  // Read from swap file and write page to swap file
  file2mem(pageToSwapToMem);
  mem2File(pageToSwapToFile);

  // Check if should use the system global pgdir
  pte_t * pgdir = proc->pgdir;
  if (use_system_pgdir == 1)
  {
    pgdir = system_pgdir;
  }
  set_PTE_A(pgdir,(char *) pageToSwapToMem->pg.virtualAddr);

  cprintf("swapPages ends.\npage number %d --> file.\npage number %d with virtual address 0x%x--> physical memory.\n",pageToSwapToFile->pg.pages_array_index, pageToSwapToMem->pg.pages_array_index, pageToSwapToMem->pg.virtualAddr);
  cprintf("Swap Pages: List after swaping pages : \n");
  printList();
  return 0;
}



/**********************************************************************************************************************************/
/*****************************  TASK 1: Delete page from memory *****************************/


int
deletePage(struct page_in_mem * pageToDelete)
{
  if (proc->num_pages_in_phys_mem > 0)
  {
    proc->num_pages_in_phys_mem--;
  }
  int found_PageFromFile = 0;

  // Initialize all page's entry in array fields
  pageToDelete->pg.virtualAddr = 0xFFFFFFFF;
  pageToDelete->pg.num_times_accessed = 0;           // Initialize number of time page has been accessed
  pageToDelete->pg.state = NOTUSED;                  // Page state
  pageToDelete->pg.offset_in_file = -1;              // If page is in file, this field stores its offset inside the swap file

  // If any pages on file --> bring a page from file into physical memory
  if (proc->num_pages_in_file > 0)
  {
    struct page_in_mem * bringPageFromFile;
    for (bringPageFromFile = &proc->paging_meta_data[0]; bringPageFromFile < &proc->paging_meta_data[MAX_TOTAL_PAGES]; bringPageFromFile++)
    {
      if (bringPageFromFile->pg.state == SWAPFILE)
      {
        found_PageFromFile = 1;
        break;
      }   
    } 
    if(found_PageFromFile)
    {
      file2mem(bringPageFromFile);
    } 
  }

  updateListPointers(pageToDelete);
  proc->paging_meta_data[pageToDelete->pg.pages_array_index] = *pageToDelete;
  cprintf("deletePage process %d number of pages in mem = %d\n", proc->pid, proc->num_pages_in_phys_mem);
  return 0;
}



/**********************************************************************************************************************************/
/*****************************  TASK 1: Choose Page to remove from physical memory pages list  *****************************************/


struct page_in_mem *
choosePageFromPhysMem(void)
{
  struct page_in_mem *pageToRemove = proc->mem_pages_head;
  struct page_in_mem *tmp;
  int foundPage = 0;

  // Check if should use the system global pgdir
  pte_t * pgdir = proc->pgdir;
  if (use_system_pgdir == 1)
  {
    pgdir = system_pgdir;
  }

   // If list is null (no page to swap out) --> return to calling function allocuvm
  if (!pageToRemove)
  {
    return 0;
  }

  cprintf("choosePageFromPhysMem: BEFORE choosing. printList:\n");
  printList();
  switch(SELECTION)
  {
    case(LIFO):

          // Find last page link in list to swap out
          while (pageToRemove->next != 0)
          {
            pageToRemove = pageToRemove->next;
          }
          break;

    // According to the order in which the pages were created and the status of the PTE_A (accessed/reference bit) flag of the page table entry 
    // Inspect pages from oldest to newest:
    // If page's referenced bit is on, give it a second chance: Clear bit & Move to end of list
    case(SCFIFO):

          while (!foundPage)
          {
            // If PTE_A is set (been accessed) --> move to the end of list
            if (check_PTE_A(pgdir, (char *)pageToRemove->pg.virtualAddr) > 0)
            { 
              
              movePageToListEndSCFIFO(pageToRemove);
              pageToRemove = proc->mem_pages_head;
            }
            // PTE_A is not set --> choose that page
            else
            {
              foundPage = 1;
            }
          }
          break;


    case(LAP):

          // Find least accessed page to swap out
          tmp = proc->mem_pages_head;
          int minimalAccessed = tmp->pg.num_times_accessed;
          while (tmp->next != 0)
          {
            tmp = tmp->next;
            if (tmp->pg.num_times_accessed < minimalAccessed)
            {
              pageToRemove = tmp;
              minimalAccessed = tmp->pg.num_times_accessed;
            }
          }
          break;
  }
  return pageToRemove;
}



/**********************************************************************************************************************************/
/*****************************  TASK 1: Update list of pages in physical memory pointer when moving page into swap file *****************************************/


void
updateListPointers(struct page_in_mem * pageInList)
{
  struct page_in_mem *previousPage;
  struct page_in_mem *nextPage;

  // Check if not head of list
  if (pageInList->prev != 0)
  {
    previousPage = pageInList->prev;
    previousPage->next = pageInList->next;

    // Not head and not tail (In the middle of the list)
    if (pageInList->next != 0)
    {
      nextPage = pageInList->next;
      nextPage->prev = previousPage;
      proc->paging_meta_data[nextPage->pg.pages_array_index] = *nextPage;
    }

    // Tail of list (and not head)
    proc->paging_meta_data[previousPage->pg.pages_array_index] = *previousPage;
  }

  // Head of list
  else
  {
    // Set process pointer to head of list
    proc->mem_pages_head = pageInList->next;

    // More pages in list
    if (pageInList->next != 0)
    {
      nextPage = pageInList->next;
      nextPage->prev = 0;
      proc->paging_meta_data[nextPage->pg.pages_array_index] = *nextPage;
    }
  }

  // Set pointer of current page
  pageInList->prev = 0;
  pageInList->next = 0;
  proc->paging_meta_data[pageInList->pg.pages_array_index] = *pageInList;
}


/****************************************************************************************************/
/****************************************************************************************************/
/*******************************************************************************************************************/



// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;
  if(pgdir == 0)
    panic("freevm: no pgdir");

  // freevm: should use global pgdir
  use_system_pgdir = 1;
  system_pgdir = pgdir;
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = p2v(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  use_system_pgdir = 0;     // Turn off usage of global system pgdir when finishing in freevm
  kfree((char*)pgdir);
}




// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}


/*****************************  TASK 2: SCFIFO - move page to the end of list and clear PTE_A *****************************/


void
movePageToListEndSCFIFO(struct page_in_mem * pageToMoveToTail)
{
  // Check if should use the system global pgdir
  pte_t * pgdir = proc->pgdir;
  if (use_system_pgdir == 1)
  {
    pgdir = system_pgdir;
  }

  // Clear PTE_A flag
  clear_PTE_A(pgdir, (char *)pageToMoveToTail->pg.virtualAddr);

  struct page_in_mem * previousPage = pageToMoveToTail->prev;
  struct page_in_mem * nextPage = pageToMoveToTail->next;

  // Find last page in list
  struct page_in_mem * listTailPage = proc->mem_pages_head;
  while (listTailPage->next != 0)
  {
    listTailPage = listTailPage->next;
  }

  // If last page in list is the same page as received as input, return (do nothing other than clearing its' flag)
  if (listTailPage->pg.pages_array_index == pageToMoveToTail->pg.pages_array_index)
  {
    return;
  }

  // Previous page in list is not null
  if (previousPage != 0)
  {
    previousPage->next = nextPage;
    // Set next page's in list prev pointer to the previous page in list
    if (nextPage != 0)
    {
      nextPage->prev = previousPage;
      proc->paging_meta_data[nextPage->pg.pages_array_index] = *nextPage;
    }
    proc->paging_meta_data[previousPage->pg.pages_array_index] = *previousPage;
  }

  // Page to move to tail is list's head (we already know our page is not the last in list)
  else
  {
    nextPage->prev = 0;
    proc->paging_meta_data[nextPage->pg.pages_array_index] = *nextPage;
    proc->mem_pages_head = nextPage;
  }

  listTailPage->next = pageToMoveToTail;
  pageToMoveToTail->prev = listTailPage;
  pageToMoveToTail->next = 0;

  // Update pages array
  proc->paging_meta_data[listTailPage->pg.pages_array_index] = *listTailPage;
  proc->paging_meta_data[pageToMoveToTail->pg.pages_array_index] = *pageToMoveToTail;
  
}


/*****************************  TASK 2: Clear PTE_A bit *****************************/

void
clear_PTE_A(pde_t *pgdir, char *virtAddr)
{
  pte_t *pte;
  pte = walkpgdir(pgdir, virtAddr, 0);
  if(pte == 0)
    panic("clear_PTE_A");
  *pte &= ~PTE_A;
}


/*****************************  TASK 2: Set PTE_A bit *****************************/

void
set_PTE_A(pde_t *pgdir, char *virtAddr)
{
  pte_t *pte;
  pte = walkpgdir(pgdir, virtAddr, 0);
  if(pte == 0)
    panic("clear_PTE_A");
  *pte |= PTE_A;
}

/*****************************  TASK 2: Check PTE_A bit *****************************/

int
check_PTE_A(pde_t *pgdir, char *virtAddr)
{
  pte_t *pte;
  pte = walkpgdir(pgdir, virtAddr, 0);
  if(pte == 0)
    panic("check_PTE_A");
  int checkPTE_A = 0;
  checkPTE_A = *pte;
  checkPTE_A &= PTE_A;   
  if (checkPTE_A != 0)
  {
    return 1;
  }          
  else
  {
    return 0;
  }               
}


/*****************************  TASK 1.1: Set PTE_P and clear PTE_PG on a page  *****************************/


void
set_present_clear_pagedout(pde_t *pgdir, char *virtAddr)
{
  pte_t *pte;
  pte = walkpgdir(pgdir, virtAddr, 0);
  if(pte == 0)
  {
    panic("set_present_clear_pagedout\n");
  }
  *pte &= ~PTE_PG;                                // Clear PTE_PG bit
  *pte = *pte | PTE_P;                            // Set PTE_P bit (indicating page is Paged Out to swap file)
}



/*****************************  TASK 1.1: Clear PTE_P and set PTE_PG on a page *****************************/


void
clear_present_set_pagedout(pde_t *pgdir, char *virtAddr)
{
  pte_t *pte;
  pte = walkpgdir(pgdir, virtAddr, 0);
  if(pte == 0)
  {
    panic("clear_present_set_pagedout\n");
  }
  *pte &= ~PTE_P;                                // Clear PTE_P bit
  *pte = *pte | PTE_PG;                          // Set PTE_PG bit (indicating page is Paged Out to swap file)
}


/************************************************************************************************************/


// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
  {
    return 0;
  }

  // For each page in process's page directory
  for(i = 0; i < sz; i += PGSIZE)
  {
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
    {
      panic("copyuvm: pte should exist");
    }


    // Case that page is not in memory, and not in swap file, should panic
    if (!((*pte & PTE_P) || (*pte & PTE_PG)))
    {
      panic("copyuvm: page not present and not in swap file");
    }

    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);

    // Case the page is in not in file --> page is in memory, allocate regularly
    if ((*pte & PTE_PG) == 0)
    { 
      if((mem = kalloc()) == 0)
      {
        goto bad;
      }
      memmove(mem, (char*)p2v(pa), PGSIZE);
      if(mappages(d, (void*)i, PGSIZE, v2p(mem), flags) < 0)
        goto bad;
    }

    // (*pte & PTE_P) is not 0 --> page is in the file --> Allocate page table entry without inserting memory and mapping
    else 
    {
      pte_t *filed_page_pte = walkpgdir(d, (void*) i, 1);
      *filed_page_pte = (*pte & 0xfff);
    }
  }

  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)p2v(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

