// user/pa4test.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
test_swap(void)
{
  int i;
  int PGSIZE = 4096;
  int num_pages = 2000; // Assuming PHYSTOP is reduced, this should trigger swap
  char *ptr;
  int pid;

  printf("Starting PA4 Swap Test...\n");
  
  // Prepare variables for swapstat
  int read_count = 0, write_count = 0;

  // 1. Massive memory allocation (uses malloc, which calls sbrk)
  printf("Allocating pages...\n");
  for(i = 0; i < num_pages; i++){
    ptr = malloc(PGSIZE); // Allocate 4KB
    if(ptr == 0){
      printf("malloc failed at iteration %d\n", i);
      break;
    }
    // Write to the page to ensure physical memory is actually allocated
    // (Essential if lazy allocation is implemented, or to consume kalloc)
    memset(ptr, 1, PGSIZE); 
  }

  // 2. Check Swap Statistics
  swapstat(&read_count, &write_count);
  printf("Swap Stat -> Read: %d, Write: %d\n", read_count, write_count);

  if(write_count > 0){
    printf("[PASS] Swap-out occurred!\n");
  } else {
    printf("[FAIL] No Swap-out. Try reducing PHYSTOP further.\n");
  }

  // 3. Create child process to test Copy-on-Write and Swap-in logic
  pid = fork();
  if(pid == 0){
      // Child process
      printf("Child accessing memory...\n");
      // Accessing memory allocated by parent might trigger Swap-in
      exit(0);
  } else {
      wait(0);
      swapstat(&read_count, &write_count);
      printf("After Child -> Read: %d, Write: %d\n", read_count, write_count);
  }

  exit(0);
}

int
main(int argc, char *argv[])
{
  test_swap();
  exit(0);
}
