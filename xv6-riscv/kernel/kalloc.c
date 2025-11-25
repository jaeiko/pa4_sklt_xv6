// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

void lru_init(void);
void lru_add(uint64);
void lru_remove(uint64);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: struct for page control
struct spinlock lru_lock;
struct page pages[PHYSTOP/PGSIZE]; // Physical Page Metadata Arrangement
struct page *lru_head = 0;         // LRU List Head

// Bitmap for Swap Space management (simple array implementation)
// 4 blocks per page
int swap_bitmap [SWAPMAX / 4]; // 1 is in use; 0 is empty
struct spinlock swap_lock;


void
kinit()
{
  initlock(&kmem.lock, "kmem");
  // PA4: Initialize LRU list and Swap lock
  lru_init();
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// LRU and Swap Initialization Functions
void
lru_init()
{
  initlock(&lru_lock, "lru");
  initlock(&swap_lock, "swap");
  lru_head = 0;
  // initializing swap_bitmap
  memset(swap_bitmap, 0, sizeof(swap_bitmap));
}

// Add a page to the LRU list (call in kalloc or maps)
void
lru_add(uint64 pa)
{
  struct page *p = &pages[pa / PGSIZE]; // Import page structures for that physical address
  acquire(&lru_lock);
  if(lru_head == 0) {
    // If the list is empty, point to myself to create a circular list
    lru_head = p;
    p->next = p;
    p->prev = p;
  } else {
    // Insert in front of the head of the list (considered the most recent used)
    struct page *tail = lru_head->prev;

    p->next = lru_head;
    p->prev = tail;
    
    tail->next = p;
    lru_head->prev = p;
  }
  release(&lru_lock);
}

// Remove pages from the LRU list (call when kfree or uvmunmap)
void
lru_remove(uint64 pa) 
{
  struct page *p = &pages[pa / PGSIZE];
  acquire(&lru_lock);
  
  // Ignore pages that are not in the LRU list (already removed or never added)
  if(p->next == 0){
    release(&lru_lock);
    return;
  }

  if(p->next == p) { // When there is only one in the list
      lru_head = 0;
  } else {
      // Disconnect link
      p->prev->next = p->next;
      p->next->prev = p->prev;
      // If the node being removed was head, move the head to the next node
      if(lru_head == p) lru_head = p->next;
  }
  // Hang up the link for safety
  p->next = 0;
  p->prev = 0;
  release(&lru_lock);
}

// Swap out a victim page to disk and return its physical address
void*
swap_out(void)
{
  struct page *p;
  pte_t *pte;
  uint64 pa;
  int swap_idx = -1;
  int i;

  acquire(&lru_lock);

  // Check if LRU list is empty
  if(lru_head == 0) {
    release(&lru_lock);
    return 0; 
  }

  // 1. Select a victim page using Clock Algorithm
  p = lru_head;
  while(1){
    pte = walk(p->pagetable, (uint64)p->vaddr, 0);

    // Defense code: PTE is not valid (just in case)
    if(pte == 0 || (*pte & PTE_V) == 0) {
      p = p->next;
      continue;
    }
    
    if((*pte) & PTE_A){
      *pte &= ~PTE_A;     // Give second chance
      p = p->next;
    } else {
      break; // Victim found
    }
  }

  // 2. Allocate swap space
  acquire(&swap_lock);
  for(i = 0; i < (SWAPMAX / 4); i++){
    if(swap_bitmap[i] == 0){
      swap_bitmap[i] = 1; // // Mark as in use
      swap_idx = i;
      break;
    }
  }
  release(&swap_lock);

  // Swap space is full
  if(swap_idx == -1) {
    release(&lru_lock);
    return 0;
  }

  // 3. Remove from LRU list BEFORE releasing lock
  // This prevents other CPUs from picking the same victim
  if(p->next == p){
    lru_head = 0;
  } else {
    p->prev->next = p->next;
    p->next->prev = p->prev;
    
    lru_head = p->next; // Move the head next
  }
  p->next = 0;
  p->prev = 0;

  pa = (p - pages) * PGSIZE;  // Calculate physical addresses with page structure indexes

  // Release lock to allow I/O sleep
  release(&lru_lock);
  
  // 4. Write page to disk (Safe now, this page is private)
  swapwrite(pa, swap_idx, 0); 
  
  // 5. Update PTE
  // // Turn off PTE_V (Valid) and turn on PTE_S (Swapped)

  // Save Swap Index to PTE upper bits   
  *pte = ((*pte) & 0x3FF) | ((uint64)swap_idx << 10);
  *pte &= ~PTE_V;
  *pte |= PTE_S;

  // 6. Flush TLB
  sfence_vma();

  // Return the physical address to be reused by kalloc
  return (void*)pa;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(!r) {
    // If there is no memory, try Swap out
    r = swap_out();
    if(!r) { 
      printf("kalloc: out of memory\n");
      return 0; // Really OOM
    }
  }

  memset((char*)r, 5, PGSIZE);
  return (void*)r;
}
