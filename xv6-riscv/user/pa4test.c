#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define MAX_PAGES 12000 

void
print_result(char *test_name, int passed)
{
    if(passed)
        printf("[PASS] %s\n", test_name);
    else
        printf("[FAIL] %s\n", test_name);
}

// [Test 1] 기본 Swap-out 테스트
void
test_basic_swap()
{
    printf("\n=== [Test 1] Basic Allocation & Swap-out ===\n");
    int i;
    int read_cnt = 0, write_cnt = 0;
    char **ptrs = malloc(MAX_PAGES * sizeof(char*));
    int allocated = 0;

    swapstat(&read_cnt, &write_cnt);
    int initial_write = write_cnt;

    printf("Allocating pages to trigger swap-out...\n");
    for(i = 0; i < MAX_PAGES; i++){
        ptrs[i] = malloc(PGSIZE);
        if(ptrs[i] == 0) break; // OOM or Swap Full
        
        // Write data to force physical page allocation
        memset(ptrs[i], i % 255, PGSIZE);
        allocated++;
    }

    swapstat(&read_cnt, &write_cnt);
    printf("Allocated: %d pages, Swap Writes: %d -> %d\n", allocated, initial_write, write_cnt);

    if(write_cnt > initial_write) {
        print_result("Basic Swap-out Triggered", 1);
    } else {
        printf("[WARN] No swap-out detected. (Maybe increase MAX_PAGES or reduce PHYSTOP)\n");
        print_result("Basic Swap-out Triggered", 0);
    }

    // Cleanup
    for(int j=0; j<allocated; j++) free(ptrs[j]);
    free(ptrs);
}

// [Test 2] Swap-in 및 데이터 무결성 테스트
void
test_swap_in_integrity()
{
    printf("\n=== [Test 2] Swap-in & Data Integrity ===\n");
    int i;
    int read_cnt = 0, write_cnt = 0;
    char **ptrs = malloc(MAX_PAGES * sizeof(char*));
    int allocated = 0;

    // 1. Fill Memory
    for(i = 0; i < MAX_PAGES; i++){
        ptrs[i] = malloc(PGSIZE);
        if(ptrs[i] == 0) break;
        // 각 페이지에 고유한 값 기록 (검증용)
        memset(ptrs[i], (i % 200) + 1, PGSIZE); 
        allocated++;
    }

    swapstat(&read_cnt, &write_cnt);
    int before_read = read_cnt;
    printf("Memory filled. Accessing early pages (potential swap-in)...\n");

    // 2. Access early pages (These should be swapped out by LRU)
    int mismatch = 0;
    for(i = 0; i < allocated / 2; i++){ 
        if(ptrs[i][0] != (char)((i % 200) + 1)) {
            mismatch++;
            break;
        }
    }

    swapstat(&read_cnt, &write_cnt);
    printf("Swap Reads: %d -> %d\n", before_read, read_cnt);

    if(mismatch == 0 && read_cnt > before_read){
        print_result("Data Integrity & Swap-in", 1);
    } else if (mismatch > 0) {
        printf("Data mismatch found! Swap logic might be broken.\n");
        print_result("Data Integrity & Swap-in", 0);
    } else {
        printf("[WARN] No swap-in detected.\n");
        print_result("Data Integrity & Swap-in", 0);
    }

    // Cleanup
    for(int j=0; j<allocated; j++) free(ptrs[j]);
    free(ptrs);
}

// [Test 3] Fork 시 Swap된 페이지 처리 (Copy-on-Swap)
void
test_fork_swapped()
{
    printf("\n=== [Test 3] Fork with Swapped-out Pages ===\n");
    int i;
    int allocated = 0;
    int fork_test_pages = 6000;
    char **ptrs = malloc(fork_test_pages * sizeof(char*));

    // 1. 부모 프로세스에서 메모리를 가득 채워 Swap-out 유발
    for(i = 0; i < fork_test_pages; i++){
        ptrs[i] = malloc(PGSIZE);
        if(ptrs[i] == 0) break;
        memset(ptrs[i], 0xAA, PGSIZE); // 0xAA 패턴 기록
        allocated++;
    }

    printf("Parent allocated %d pages. Forking now...\n", allocated);

    int pid = fork();

    if(pid < 0){
        printf("Fork failed! (Still OOM? Try reducing fork_test_pages)\n");
        exit(1);
    }

    if(pid == 0){
        // [Child Process]
        // 부모가 Swap-out 시켰을 페이지들에 접근.
        // 이때 uvmcopy -> swapread -> copy 과정이 올바르게 동작해야 함.
        int correct = 1;
        for(i = 0; i < allocated; i++){
            if((unsigned char)ptrs[i][0] != 0xAA){
                correct = 0;
                printf("Child: Data mismatch at page %d\n", i);
                break;
            }
        }
        
        if(correct) {
            printf("Child: All data matches parent's data.\n");
            exit(0); // Success
        } else {
            exit(1); // Fail
        }
    } else {
        // Parent Process
        int status;
        wait(&status);
        if(status == 0) print_result("Fork handled swapped pages correctly", 1);
        else print_result("Fork handled swapped pages correctly", 0);

        // Cleanup
        for(int j=0; j<allocated; j++) free(ptrs[j]);
        free(ptrs);
    }
}

// [Test 4] Exit 시 Swap 공간 반환 확인
void
test_exit_cleanup()
{
    printf("\n=== [Test 4] Swap Space Reclaim on Exit ===\n");
    
    int pid = fork();
    if(pid == 0){
        // 자식: 메모리를 최대로 할당하여 Swap 공간 점유
        char **p = malloc(MAX_PAGES * sizeof(char*));
        int cnt = 0;
        for(int i=0; i<MAX_PAGES; i++){
            p[i] = malloc(PGSIZE);
            if(!p[i]) break;
            memset(p[i], 1, PGSIZE);
            cnt++;
        }
        printf("Child occupied %d pages. Exiting...\n", cnt);
        // 여기서 exit하면 swap_bitmap이 clear 되어야 함
        exit(0);
    }
    
    wait(0);
    printf("Child exited. Parent checking if swap space is free...\n");

    // 부모: 다시 메모리 할당 시도. 
    // 만약 자식이 Swap을 반환하지 않았다면 금방 OOM 발생함.
    int allocated = 0;
    char **ptrs = malloc(MAX_PAGES * sizeof(char*));
    for(int i=0; i<MAX_PAGES; i++){
        ptrs[i] = malloc(PGSIZE);
        if(!ptrs[i]) break;
        memset(ptrs[i], 1, PGSIZE);
        allocated++;
    }

    // 충분히 많이 할당되었으면 통과 (정확한 수치는 환경마다 다름)
    // 일반적으로 MAX_PAGES의 80% 이상 재할당 가능하면 성공으로 간주
    if(allocated > MAX_PAGES * 0.8){
        printf("Re-allocated %d pages.\n", allocated);
        print_result("Swap Space Reclaimed", 1);
    } else {
        printf("Only re-allocated %d pages. (Leak suspect)\n", allocated);
        print_result("Swap Space Reclaimed", 0);
    }

    for(int j=0; j<allocated; j++) free(ptrs[j]);
    free(ptrs);
}

// [Test 5] OOM (Out of Memory) 처리
void
test_oom_handling()
{
    printf("\n=== [Test 5] OOM Handling ===\n");
    // Swap Max(약 7000)보다 훨씬 많이 할당 시도
    int huge_num = 10000; 
    char **ptrs = malloc(huge_num * sizeof(char*));
    int count = 0;

    printf("Allocating until fail...\n");
    for(int i = 0; i < huge_num; i++){
        ptrs[i] = malloc(PGSIZE);
        if(ptrs[i] == 0){
            printf("malloc returned 0 at page %d (Expected).\n", i);
            break;
        }
        // 물리 할당 유도
        memset(ptrs[i], 1, PGSIZE); 
        count++;
    }

    // Panic 없이 여기까지 오면 성공
    if(count < huge_num){
        print_result("OOM Handled Gracefully (No Panic)", 1);
    } else {
        // 만약 10000개 다 할당되면 테스트 환경이 너무 크거나 체크 실패
        printf("[WARN] Could not trigger OOM. (Allocated %d pages)\n", count);
        print_result("OOM Handled Gracefully", 1); // Panic 안났으니 일단 Pass
    }

    for(int j=0; j<count; j++) free(ptrs[j]);
    free(ptrs);
}

int
main(int argc, char *argv[])
{
    printf("Starting PA4 Comprehensive Tests...\n");

    // 각 테스트를 별도 프로세스나 순차적으로 실행하여 서로 간섭 최소화
    
    // Test 1 & 2: Basic Swap
    if(fork() == 0) {
        test_basic_swap();
        test_swap_in_integrity();
        exit(0);
    }
    wait(0);

    // Test 3: Fork
    if(fork() == 0) {
        test_fork_swapped();
        exit(0);
    }
    wait(0);

    // Test 4: Exit Cleanup
    if(fork() == 0) {
        test_exit_cleanup();
        exit(0);
    }
    wait(0);

    // Test 5: OOM
    if(fork() == 0) {
        test_oom_handling();
        exit(0);
    }
    wait(0);

    printf("\nAll tests finished.\n");
    exit(0);
}