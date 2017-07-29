#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int
exec(char *path, char **argv)
{
  // TASK 1-2: Store father process data in case of unsuccessful exec
  int num_pg_in_mem = proc->num_pages_in_phys_mem;
  int num_pg_file = proc->num_pages_in_file;
  int pg_faults = proc->number_of_pgFLTS;
  int paging_out_times = proc->number_of_paging_out_times;
  uint pgIndex;
  struct page_in_mem * tmp;
  struct page_in_mem pages_array[MAX_TOTAL_PAGES];
  struct page_in_mem * listHead;


  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;

  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) < sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;


  // TASK 1-2: case of exec, (forking from shell), we'll want to initialize pages data in a different manner:
  if (SELECTION != NONE)
  {
    // Store page, next and prev data of father process pages array
    for(int i = 0; i < MAX_TOTAL_PAGES; i++)
    {
      pages_array[i].pg = proc->paging_meta_data[i].pg;
      if (proc->paging_meta_data[i].prev != 0)
      {
        tmp = proc->paging_meta_data[i].prev;
        pgIndex = tmp->pg.pages_array_index;
        pages_array[i].prev = &pages_array[pgIndex];
      }
      if (proc->paging_meta_data[i].next != 0)
      {
        tmp = proc->paging_meta_data[i].next;
        pgIndex = tmp->pg.pages_array_index;
        pages_array[i].next = &pages_array[pgIndex];
      }
    }

    // Store pointer to header of physical memory list of father process
    if (proc->mem_pages_head != 0)
    {
      tmp = proc->mem_pages_head;
      pgIndex = tmp->pg.pages_array_index;
      listHead = &pages_array[pgIndex];
    }

    // Reset the father's pages data in case of success (will store in case of failure)
    resetProcessPagesData(proc);  
  }

  // Load program into memory.
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(proc->name, last, sizeof(proc->name));

  // Commit to the user image.
  oldpgdir = proc->pgdir;
  proc->pgdir = pgdir;
  proc->sz = sz;
  proc->tf->eip = elf.entry;  // main
  proc->tf->esp = sp;

  
  // TASK 1-2: If a swap file has been created in father process, remove it and create a new one, since the former is irrelevant
  if (SELECTION != NONE)
  {
    if (proc->swapFile != 0)
    {
      removeSwapFile(proc);
      createSwapFile(proc);
    }
  }
  
  
  switchuvm(proc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }

  // TASK 3: If unsuccesful exec --> restore old data
  if (SELECTION != NONE)
  {
    proc->num_pages_in_phys_mem = num_pg_in_mem;
    proc->num_pages_in_file = num_pg_file;
    proc->number_of_pgFLTS = pg_faults;
    proc->number_of_paging_out_times = paging_out_times;
    proc->mem_pages_head = listHead;
   
    for (i = 0; i < MAX_PSYC_PAGES; i++) {
      proc->paging_meta_data[i].pg = pages_array[i].pg;
      if (pages_array[i].prev != 0)
      {
        tmp = pages_array[i].prev;
        pgIndex = tmp->pg.pages_array_index;
        proc->paging_meta_data[i].prev = &proc->paging_meta_data[pgIndex];
      }
      if (pages_array[i].next != 0)
      {
        tmp = pages_array[i].next;
        pgIndex = tmp->pg.pages_array_index;
        proc->paging_meta_data[i].next = &proc->paging_meta_data[pgIndex];
      }
    }
     // Set pointer to header of physical memory list in child process
    if (proc->mem_pages_head != 0)
    {
      tmp = proc->mem_pages_head;
      pgIndex = tmp->pg.pages_array_index;
      proc->mem_pages_head = &proc->paging_meta_data[pgIndex];
    }
  }
  return -1;
}

