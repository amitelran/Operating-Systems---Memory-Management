// Segments in proc->gdt.
#define NSEGS     7
#define MAX_PSYC_PAGES 15  //maximum number of process's pages in the physical memory
#define MAX_TOTAL_PAGES 30 // maximum number of pages for process.
#define LIFO 1
#define SCFIFO 2
#define LAP 3
#define NONE 4

// TASK 3: VERBOSE_PRINT macro values
#define TRUE 1
#define FALSE 0


/*****************************  TASK 1-2: Page struct *****************************/

enum pagestate { NOTUSED, PHYSMEMORY, SWAPFILE };

struct page {
  uint virtualAddr;           // Page's virtual address
  uint num_times_accessed;    // Counter number of times page has been accessed
  int offset_in_file;         // If page is in file, this field stores its offset inside the swap file
  uint pages_array_index;     // The page's index in the pages array
  enum pagestate state;       // The page's state
};


// Linked list for pages current in the physical memory
struct page_in_mem 
{
    struct page pg;              // Link in the list (every link is a page)
    struct page_in_mem *next;     // Next link in list
    struct page_in_mem *prev;     // Previous link in list
};


/**********************************************************************************/

// Per-CPU state
struct cpu {
  uchar id;                    // Local APIC ID; index into cpus[] below
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  
  // Cpu-local storage variables; see below
  struct cpu *cpu;
  struct proc *proc;           // The currently-running process.
};

extern struct cpu cpus[NCPU];
extern int ncpu;

// Per-CPU variables, holding pointers to the
// current cpu and to the current process.
// The asm suffix tells gcc to use "%gs:0" to refer to cpu
// and "%gs:4" to refer to proc.  seginit sets up the
// %gs segment register so that %gs refers to the memory
// holding those two variables in the local cpu's struct cpu.
// This is similar to how thread-local variables are implemented
// in thread libraries such as Linux pthreads.
extern struct cpu *cpu asm("%gs:0");       // &cpus[cpunum()]
extern struct proc *proc asm("%gs:4");     // cpus[cpunum()].proc

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  //Swap file. must initiate with create swap file
  struct file *swapFile;			//page file


  struct page_in_mem paging_meta_data[MAX_TOTAL_PAGES];   // TASK 1.1: Array of paging meta_data
  struct page_in_mem *mem_pages_head;                     // TASK 1.1: Pointer to Linked List head of pages currently in memory
  uint num_pages_in_file;                                 // Number of pages stored in file (up to 15)
  uint num_pages_in_phys_mem;                             // Number of pages stored in physical memory (up to 15)
  uint number_of_pgFLTS;                                  // TASK 3: number of page faults
  uint number_of_paging_out_times;                        // TASK 3: number of paged out

};


void update_num_times_accessed(void); 
void printPageInfo(struct page_in_mem * page);
void printPageFromFile(struct page_in_mem * page);
void printPageArray(void);
void printList(void);
void resetProcessPagesData(struct proc * p);

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
