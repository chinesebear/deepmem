
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "deep_mem.h"
#include "deep_log.h"
#define WASM_FILE_SIZE 1024
#define DEEPVM_MEMPOOL_SIZE 30*1024
uint8_t deepvm_mempool[DEEPVM_MEMPOOL_SIZE]= {0};
uint8_t example[100]= {"This is a example for logsys."};
int main(void) {
    // deep_info("This a log for information");
    // deep_debug("This a log for debuging");
    // deep_warn("This a log for warning");
    // deep_error("This a log for error");
    // deep_dump("example", example, 100);
    deep_mem_init(deepvm_mempool, DEEPVM_MEMPOOL_SIZE);
    for(int i = 0; i < 1000; i++) {
        deep_info("malloc %d times", i);
        uint8_t *p = deep_malloc(100);
        if (p == NULL) {
            deep_error("malloc fail");
            break;
        }
        deep_free(p);
    }
    return 0;
}
