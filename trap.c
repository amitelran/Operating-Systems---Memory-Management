#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

int swapPages(uint virtAddr);

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  
  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  uint faultAddr = 0;
  pte_t * pte;

  if(tf->trapno == T_SYSCALL){
    if(proc->killed)
      exit();
    proc->tf = tf;
    syscall();
    if(proc->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpu->id == 0){
      acquire(&tickslock);

      // LAP - update all num_times_accessed of all pages
      if (SELECTION == LAP)
      {
        update_num_times_accessed();
      }
      ticks++;
      wakeup(&ticks);
      release(&tickslock);

    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpu->id, tf->cs, tf->eip);
    lapiceoi();
    break;
   


  case T_PGFLT:

    // If SELECTION is NONE, go to default case (regular xv6 page fault)
    if ((SELECTION == NONE) || (proc->pid <= 2))
    {  
      cprintf("T_PGFLT: SELECTION=NONE || init || shell\n");
      goto pf;
    }
  
    proc->number_of_pgFLTS++;                           // Increment number of page faults counter
    faultAddr = PGROUNDDOWN(rcr2());                    // Get fault address from rcr2 register
    cprintf("REACHED PGFLT! Process %d Looking for entry: 0x%x\n", proc->pid, faultAddr);
    pte = walkpgdirForProc(proc->pgdir, (char*)faultAddr, 0);
    if ((*pte & PTE_PG) == 0)
    {
      printPageArray();
      goto pf;      // This is a real Page Fault (PTE_PG is off --> page does not exist on file nor on page directory)
    }

    else
    {   
       // Swap pages - get the required page from the swap file and write a chosen page by policy from physical memory to the swap file instead
       if (swapPages(faultAddr) == -1)
       {
          // swapPages didn't find the page in the swap file --> ERROR, shouldn't reach here because we check PTE_PG bit
          cprintf("T_PGFLT: 'swap-pages' func didnt find the address: 0x%x in the swap file\n",faultAddr);
          goto pf;
       }
    }
    break;

  //PAGEBREAK: 13
  default:
    pf:
    if(proc == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpu->id, tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            proc->pid, proc->name, tf->trapno, tf->err, cpu->id, tf->eip, 
            rcr2());
    proc->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running 
  // until it gets to the regular system call return.)
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(proc && proc->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();
}
