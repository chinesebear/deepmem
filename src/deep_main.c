#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "deep_mem.h"
#include "deep_log.h"
#define WASM_FILE_SIZE 1024
#define DEEPVM_MEMPOOL_SIZE 30*1024

#define ITER 1000

uint8_t deepvm_mempool[DEEPVM_MEMPOOL_SIZE]= {0};
uint8_t example[100]= {"This is a example for logsys."};

static void
log_memory (void *buff, size_t size)
{
  for (int i = 0; i < size; i++)
    {
      char string[9] = "";
      for (int j = 0; j < 4; i++, j++)
        {
          sprintf (string + 2 * j, "%02x", *(uint8_t *)(buff + i));
        }
      if (strncmp (string, "00000000", 8) != 0)
        {
          printf ("%03d: 0x%s\n", i, string);
        }
    }
}

void cycletest(uint32_t n) {
  printf("\nTEST ON MALLOCING/FREEING %u bytes: \n\n", n);
  for(int i = 1; i <= ITER; i++) {
    uint8_t *p = deep_malloc(n);
    deep_info("malloc %d times, @%p", i, p);
    if (p == NULL) {
      deep_error("malloc fail @%d", i);
      break;
    }
    *p = 0xFF;
    deep_free(p);
  }
}

int main(void) {
    // deep_info("This a log for information");
    // deep_debug("This a log for debuging");
    // deep_warn("This a log for warning");
    // deep_error("This a log for error");
    // deep_dump("example", example, 100);
    deep_mem_init(deepvm_mempool, DEEPVM_MEMPOOL_SIZE);
    cycletest(100); /* sorted */
    cycletest(60);  /* sorted on 64bit; fast on 32bit */
    cycletest(40);  /* fast */
    return 0;
}
