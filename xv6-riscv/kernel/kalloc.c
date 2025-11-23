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
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

struct spinlock lru_lock;
struct page pages[PHYSTOP/PGSIZE]; // Physical Page Metadata Arrangement
struct page *lru_head = 0;         // LRU List Head

// Bitmap for Swap Space management (simple array implementation)
// 8 blocks per page (4096 / 512 = 8)
int swap_bitmap [SWAPMAX / 8]; // 1 is in use; 0 is empty
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
  struct page *p = &pages[pa / PGSIZE];
  acquire(&lru_lock);
  if(lru_head == 0) {
    lru_head = p;
    p->next = p;
    p->prev = p;
  } else {
    p->next = lru_head;
    p->prev = lru_head->prev;
    lru_head->prev->next = p;
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
  if(p->next == p) { // When there is only one in the list
      lru_head = 0;
  } else {
      p->prev->next = p->next;
      p->next->prev = p->prev;
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

  // 1. Select a victim page using Clock Algorithm
  // Iterate through the circular linked list starting from lru_head
  p = lru_head;
  while(1){
    // Get the PTE corresponding to this page
    // We use the 'pagetable' and 'vaddr' stored in struct page
    pte = walk(p->pagetable, (uint64)p->vaddr, 0);

    // If the page has been accessed (PTE_A is set)
    if((*pte) & PTE_A){
      *pte &= ~PTE_A;     // Clear Access bit (Give a second chance)
      p = p->next;        // Move to the next page
    } else {
      // Victim found (PTE_A is 0)
      break;
    }
  }

  // 2. Allocate swap space (Bitmap search)
  acquire(&swap_lock);
  for(i = 0; i < (SWAPMAX / 8); i++){
    if(swap_bitmap[i] == 0){
      swap_bitmap[i] = 1; // Mark as used
      swap_idx = i;
      break;
    }
  }
  release(&swap_lock);

  if(swap_idx == -1)
    panic("swap_out: Out of swap space");

  // 3. Write page to disk (Swap space)
  // Calculate physical address from the page structure index
  pa = (p - pages) * PGSIZE;

  // swapwrite takes physical address and block number.
  // 1 page = 4096 bytes, 1 block = 512 bytes -> 8 blocks per page
  swapwrite(pa, swap_idx * 8);

  // 4. Update PTE
  *pte &= ~PTE_V;       // Clear Valid bit (Page is no longer in memory)
  *pte |= PTE_S;        // Set Swapped bit (Custom flag to indicate swap)

  // Store the swap index in the PPN field of the PTE
  // We clear the old PPN (top 54 bits) and set the new swap index
  // 0x3FF is the mask for flags (10 bits)
  *pte = ((*pte) & 0x3FF) | ((uint64)swap_idx << 10);

  // 5. Flush TLB to ensure CPU doesn't use stale mapping
  sfence_vma();

  // 6. Remove from LRU list and return physical page
  // Since we hold lru_lock, we manipulate pointers directly

  if(p->next == p){
    // List has only one element
    lru_head = 0;
  } else {
    // Remove p from the list
    p->prev->next = p->next;
    p->next->prev = p->prev;

    // If we are removing the head, advance the head
    if(lru_head == p)
      lru_head = p->next;
  }

  // Clear pointers for safety
  p->next = 0;
  p->prev = 0;

  // Release the lock before returning
  release(&lru_lock);

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
        if(!r) return 0; // Really OOM
  }

  memset((char*)r, 5, PGSIZE);
  return (void*)r;
}
