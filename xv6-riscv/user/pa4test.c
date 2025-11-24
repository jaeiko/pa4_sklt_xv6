// user/pa4test.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
test_swap_basic(void)
{
  int i;
  int PGSIZE = 4096;
  int num_pages = 12000; // 충분히 큰 수로 설정 (메모리 크기에 따라 조절)
  char **ptrs; // 포인터 배열
  int read_count = 0, write_count = 0;

  printf("\n[Test 1] Basic Allocation & Swap-out\n");

  // 포인터 배열 할당
  ptrs = (char**)malloc(num_pages * sizeof(char*));

  printf("Allocating %d pages...\n", num_pages);
  for(i = 0; i < num_pages; i++){
    ptrs[i] = malloc(PGSIZE); 
    if(ptrs[i] == 0){
      printf("malloc failed at iteration %d (Expected behavior for OOM)\n", i);
      break;
    }
    // 페이지에 값 쓰기 (물리 메모리 할당 유도)
    memset(ptrs[i], i % 255, PGSIZE); 
  }

  swapstat(&read_count, &write_count);
  printf("Swap Stat -> Read: %d, Write: %d\n", read_count, write_count);

  if(write_count > 0){
    printf("[PASS] Swap-out occurred!\n");
  } else {
    printf("[FAIL] No Swap-out. Need to allocate more memory or reduce PHYSTOP.\n");
  }

  // 할당된 메모리 해제
  for(int j = 0; j < i; j++) {
      if(ptrs[j]) free(ptrs[j]);
  }
  free(ptrs);
}

void
test_swap_in_access(void)
{
    int i;
    int PGSIZE = 4096;
    int num_pages = 12000; // 스왑이 일어날 만큼 할당
    char **ptrs;
    int read_count_before = 0, write_count_before = 0;
    int read_count_after = 0, write_count_after = 0;

    printf("\n[Test 2] Swap-in Verification\n");

    ptrs = (char**)malloc(num_pages * sizeof(char*));

    // 1. 메모리 가득 채우기 (Swap-out 유도)
    for(i = 0; i < num_pages; i++){
        ptrs[i] = malloc(PGSIZE);
        if(ptrs[i]) memset(ptrs[i], 1, PGSIZE); 
    }

    swapstat(&read_count_before, &write_count_before);
    printf("Before Access -> Read: %d, Write: %d\n", read_count_before, write_count_before);

    // 2. 앞쪽 페이지 접근 (LRU에 의해 스왑 아웃되었을 가능성 높음)
    printf("Accessing early pages...\n");
    for(i = 0; i < 100; i++){
        if(ptrs[i]){
            // 읽기 시도 -> 스왑 아웃 되었다면 Swap-in 발생
            if(ptrs[i][0] != 1) {
                printf("[FAIL] Content mismatch at page %d\n", i);
            }
        }
    }

    swapstat(&read_count_after, &write_count_after);
    printf("After Access -> Read: %d, Write: %d\n", read_count_after, write_count_after);

    if(read_count_after > read_count_before){
        printf("[PASS] Swap-in occurred!\n");
    } else {
        printf("[WARN] No Swap-in occurred (Maybe pages were not swapped out?)\n");
    }

    // 정리
     for(int j = 0; j < num_pages; j++) {
        if(ptrs[j]) free(ptrs[j]);
    }
    free(ptrs);
}

int
main(int argc, char *argv[])
{
  int pid;

  // === Test 1 실행 (자식 프로세스) ===
  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }
  
  if (pid == 0) {
    // 자식 프로세스에서 Test 1 실행
    test_swap_basic();
    exit(0); // 테스트가 끝나면 메모리 반환하며 종료
  }
  
  // 부모는 자식이 끝나길 기다림 (메모리가 정리될 때까지 대기)
  wait(0); 

  // === Test 2 실행 (자식 프로세스) ===
  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    // 깨끗한 상태에서 Test 2 실행
    test_swap_in_access();
    exit(0);
  }

  wait(0);
  
  printf("\nAll tests completed.\n");
  exit(0);
}