#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

void
copyContentsOfProcStructs(struct proc * child);

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  release(&ptable.lock);

  
  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }

  sp = p->kstack + KSTACKSIZE;
  
  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  // TASK 1.1: init paging_meta_data array and structs
  if (SELECTION != NONE)
  {
    resetProcessPagesData(p);
  }
  p->number_of_pgFLTS = 0;
  p->number_of_paging_out_times = 0;
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;
  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }

  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Copy contents of swapFile\list\array from parent to child
  if (proc && (SELECTION != NONE) && (proc->pid > 2))
  {
    copyContentsOfProcStructs(np);
  }

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));
 
  pid = np->pid;
  // lock to force the compiler to emit the np->state write last.
  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;


  if (/*proc && */(proc->swapFile != 0) && (SELECTION != NONE))
  {
      removeSwapFile(proc);
      phys_mem_pages_stats.number_of_swap_files--;
      proc->swapFile = 0;
  }

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;

  // TASK 3: Print process information upon user process termination (only if VERBOSE_PRINT is set)
  if ((VERBOSE_PRINT == 1) && ((proc->tf->cs&3) == DPL_USER))
  {
    static char *states[] = {
    [UNUSED]    "unused",
    [EMBRYO]    "embryo",
    [SLEEPING]  "sleep ",
    [RUNNABLE]  "runble",
    [RUNNING]   "run",
    [ZOMBIE]    "zombie"
    };

    cprintf("\n------------------------------------------------------------\n");
    cprintf("\nPrinting verbose print upon user process termination:\n");
    // TASK 3: <field 1> <field 2> <allocated memory pages> <paged out> <page faults> <total number of paged out> <field set 3>
    cprintf("<pid: %d> <state: %s> <allocated memory pages: %d> <paged out: %d> <page faults: %d> <total paged out: %d> <name: %s>\n",
            proc->pid, states[proc->state], proc->num_pages_in_phys_mem, proc->num_pages_in_file, proc->number_of_pgFLTS, proc->number_of_paging_out_times, proc->name);

    // ratio to be printed: free pages in the system (should be printed: <current free pages> / <total free pages>)
    cprintf("\nFree pages in the system: %d / %d\n", phys_mem_pages_stats.current_number_of_free_pages, phys_mem_pages_stats.initial_pages_number);
    cprintf("Number of swap files in system: %d\n",  phys_mem_pages_stats.number_of_swap_files);
    cprintf("\n------------------------------------------------------------\n");
    cprintf("\n");
  }
           
  cprintf("----------------- EXIT: process %d exiting -----------------\n\n", proc->pid);
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, proc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot 
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    //cprintf("%d %s %s", p->pid, state, p->name);        // -----> This the original cprintf ctrl+P

    // TASK 3: new ctrl+P (^P) printing: <field 1> <field 2> <allocated memory pages> <paged out> <page faults> <total number of paged out> <field set 3>
    cprintf("<pid: %d> <state: %s> <allocated memory pages: %d> <paged out: %d> <page faults: %d> <total paged out: %d> <name: %s>\n",
            p->pid, state, p->num_pages_in_phys_mem, p->num_pages_in_file, p->number_of_pgFLTS, p->number_of_paging_out_times, p->name);

    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }

    // ratio to be printed: free pages in the system (should be printed: <current free pages> / <total free pages>)
    cprintf("\nFree pages in the system: %d / %d\n", phys_mem_pages_stats.current_number_of_free_pages, phys_mem_pages_stats.initial_pages_number);
    cprintf("Number of swap files in system: %d\n",  phys_mem_pages_stats.number_of_swap_files);
    cprintf("\n");
  }
}



/*****************************  Reset process's page structs (in case oldsz in allocuvm is 0) *****************************/


void
resetProcessPagesData(struct proc * p)
{
  for(int i = 0; i < MAX_TOTAL_PAGES; i++)
  {
    p->swapFile = 0;
    p->paging_meta_data[i].pg.virtualAddr = 0xFFFFFFFF;         // Initialize fake virtual address
    p->paging_meta_data[i].pg.num_times_accessed = 0;           // Initialize number of time page has been accessed
    p->paging_meta_data[i].pg.state = NOTUSED;                  // Page state
    p->paging_meta_data[i].pg.offset_in_file = -1;              // If page is in file, this field stores its offset inside the swap file
    p->paging_meta_data[i].pg.pages_array_index = i;            // The page's index in the pages array
    p->paging_meta_data[i].next = 0;                            // Initialize page's pointer to next page in physical memory list
    p->paging_meta_data[i].prev = 0;                            // Initialize page's pointer to previous page in physical memory list
  }
  p->num_pages_in_file = 0;                               // Initialize number of pages stored in file counter
  p->num_pages_in_phys_mem = 0;                           // Initialize number of pages stored in physical memory
  p->mem_pages_head = 0;
}



/*****************************  TASK 2: in LAP policy: Update ALL pages lap counter (PTE_A bit is set) for ALL process every tick (called from trap)  *****************************/


void
update_num_times_accessed(void)
{ 
  pte_t * pte = 0;
  struct page_in_mem * pageToUpdate = 0;
  struct proc * p;

  // Go over all processes
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p && (p->pid > 2) && (p->state != UNUSED)  && (p->state != EMBRYO) && (p->state != ZOMBIE))
    {
      // Iterate through all process's pages
     for (int i = 0; i < MAX_TOTAL_PAGES; i++)
     {  
        pageToUpdate = &p->paging_meta_data[i];
        if (pageToUpdate->pg.state == PHYSMEMORY)
        { 
          // pageToUpdate is a page in the phyiscal memory. now we want to get the page's entries.
          pte = walkpgdirForProc(p->pgdir, (void *) pageToUpdate->pg.virtualAddr, 0);
          if (*pte & PTE_A)
          {
            //cprintf("update num_times_accessed in Proc num : %d , page with address : %p \n" , p->pid, pageToUpdate->pg.virtualAddr);
            pageToUpdate->pg.num_times_accessed++;

            // Clear the page's PTE_A flag (page has been accessed --> incremented counter, thus, clearing the flag for the next access)
            clear_PTE_A(p->pgdir,(char*) pageToUpdate->pg.virtualAddr);
          }
        }
      }
    }
  }
  release(&ptable.lock);
}



/*****************************  TASK 1: Copy swap file, linked list and array from parent to child process *****************************/


void
copyContentsOfProcStructs(struct proc * child)
{
  int fileCheck;
  uint pgIndex;
  int placeOnFile = 0;
  struct page_in_mem * tmp;

  // Read contents of parent's swap file and write into child process swap file
  if (proc->pid > 2)
  { 
    if (proc->swapFile != 0)
    {
      char buffer[PGSIZE];
     
      cprintf("\nCopying contents of father process's %d swap file into child's process %d swap file...\n\n", proc->pid, child->pid);
      if (child->swapFile == 0)
      {
        createSwapFile(child);
        phys_mem_pages_stats.number_of_swap_files++;
      }
       
      memset(buffer, 0, PGSIZE);
      while((fileCheck = readFromSwapFile(proc, buffer, placeOnFile * PGSIZE, PGSIZE)) != -1)
      {
        if ((fileCheck = writeToSwapFile(child, buffer, placeOnFile * PGSIZE, PGSIZE)) == -1)
        {
          cprintf("ERROR in writing at copyContentsOfProcStructs\n");
        }
        placeOnFile++;
        memset(buffer, 0, PGSIZE);
      }
    }

  }
   
  // Update next and prev of child physical memory list
  for(int i = 0; i < MAX_TOTAL_PAGES; i++)
  {
    child->paging_meta_data[i].pg = proc->paging_meta_data[i].pg;
    if (proc->paging_meta_data[i].prev != 0)
    {
      tmp = proc->paging_meta_data[i].prev;
      pgIndex = tmp->pg.pages_array_index;
      child->paging_meta_data[i].prev = &child->paging_meta_data[pgIndex];
    }
    if (proc->paging_meta_data[i].next != 0)
    {
      tmp = proc->paging_meta_data[i].next;
      pgIndex = tmp->pg.pages_array_index;
      child->paging_meta_data[i].next = &child->paging_meta_data[pgIndex];
    }
  }

  // Set pointer to header of physical memory list in child process
  if (proc->mem_pages_head != 0)
  {
    tmp = proc->mem_pages_head;
    pgIndex = tmp->pg.pages_array_index;
    child->mem_pages_head = &child->paging_meta_data[pgIndex];
  }

  // Set number of pages in file and physical memory
  child->num_pages_in_file = proc->num_pages_in_file;
  child->num_pages_in_phys_mem = proc->num_pages_in_phys_mem;
}


/*****************************  Print page's info *****************************/


void 
printPageInfo(struct page_in_mem * page)
{
  struct page_in_mem * tmp;
  pte_t *pte;
  cprintf("\nPage Info:\n");
  cprintf("page number %d virtual address: 0x%x\n", page->pg.pages_array_index, page->pg.virtualAddr);
  cprintf("page number %d number of times accessed: %d\n", page->pg.pages_array_index, page->pg.num_times_accessed);
  cprintf("page number %d offset in file: %d\n", page->pg.pages_array_index, page->pg.offset_in_file);
  cprintf("page number %d state: %d\n", page->pg.pages_array_index, page->pg.state);
  pte = walkpgdirForProc(proc->pgdir, (void *) page->pg.virtualAddr, 0);
  cprintf("page number %d *pte = %x, PTE_PG = %x, PTE_P = %x\n", page->pg.pages_array_index, *pte, (*pte & PTE_PG), (*pte & PTE_P));
  if (page->next != 0){
    tmp = page->next;
    cprintf("page number %d next's index: %d\n", page->pg.pages_array_index, tmp->pg.pages_array_index);
  }
  if (page->prev != 0){
    tmp = page->prev;
    cprintf("page number %d prev's index: %d\n", page->pg.pages_array_index, tmp->pg.pages_array_index);
  }
  cprintf("\n");
}



/*****************************  Print page's info *****************************/

void 
printPageFromFile(struct page_in_mem * page)
{
  char buffer[PGSIZE];
  memset(buffer, 0, PGSIZE);
  readFromSwapFile(proc, buffer, page->pg.offset_in_file, PGSIZE);
  cprintf("** Page from file contents: **\n");
  for (int i = 0; i < PGSIZE; i++){
    cprintf("%d", buffer[i]);
  }
  cprintf("\n\n");
  writeToSwapFile(proc, buffer, page->pg.offset_in_file, PGSIZE);
}


/*****************************  Print page's info for all of the meta data array *****************************/


void 
printPageArray(void)
{
  struct page_in_mem * tmp;
  cprintf("\n***** Printing all pages in array data for process %d: *****\n", proc->pid);
  for (int i = 0; i < MAX_TOTAL_PAGES; i++)
  {
    tmp = &proc->paging_meta_data[i];
    if (tmp->pg.state != NOTUSED){
      printPageInfo(tmp);
    }
  }
  cprintf("***** Finished printing all pages in array data for process %d *****\n\n", proc->pid);
}


/*****************************  Print list of pages in physical memory *****************************/

void 
printList(void)
{
  if ((SELECTION == NONE) || (proc->pid <= 2))
  {
    return;
  }
  struct page_in_mem * tmp = proc->mem_pages_head;
  pte_t *pte = 0;
  cprintf("\n***** Printing List for process %d *****\n\n", proc->pid);
  while(tmp != 0)
  {
    if (tmp->next != 0) {

      if (SELECTION == LAP){
        cprintf("%d (accss: %d)---> ", tmp->pg.pages_array_index, tmp->pg.num_times_accessed); 
      }

      else if (SELECTION == SCFIFO) {  
        pte = walkpgdirForProc(proc->pgdir, (void *) tmp->pg.virtualAddr, 0);

        if ((*pte & PTE_A) > 0){
          cprintf("%d (on) ---> ", tmp->pg.pages_array_index); 
        }
        else {
          cprintf("%d (off) ---> ", tmp->pg.pages_array_index); 
        } 
      }

      else {
        cprintf("%d ---> ", tmp->pg.pages_array_index); 
      }
    }

    else {
      if (SELECTION == LAP) {
        cprintf("%d (accss: %d)\n", tmp->pg.pages_array_index, tmp->pg.num_times_accessed); 
      }

      else if (SELECTION == SCFIFO) {  
        pte = walkpgdirForProc(proc->pgdir, (void *) tmp->pg.virtualAddr, 0);

        if ((*pte & PTE_A) > 0){
          cprintf("%d (on)\n", tmp->pg.pages_array_index); 
        }
        else {
          cprintf("%d (off)\n", tmp->pg.pages_array_index); 
        }
      }

      else {
        cprintf("%d\n", tmp->pg.pages_array_index); 
      }
    }
    tmp = tmp->next;
  }
  cprintf("\n***** Finished Printing List for process %d *****\n\n", proc->pid);
}