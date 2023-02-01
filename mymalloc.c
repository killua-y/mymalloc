#define _DEFAULT_SOURCE
#define _BSD_SOURCE 
#include <unistd.h>
#include <malloc.h> 
#include <stdio.h> 
#include <string.h>
#include <assert.h>
// for sysconf
#include <unistd.h>
// for mmap
#include <sys/mman.h>
#include "debug.h" // definition of debug_printf
#include "malloc.h"
// for lock
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <sched.h>
#include <linux/mman.h>

// the block structor definition
typedef struct block {
  size_t size;        // How many bytes beyond this block have been allocated in the heap
  struct block *prev; // represent the prev block in the linked list.
  struct block *next; // represent the next block in the linked list.
  int free;           // represent is this memory free. (0 for not free, 1 for free)
} block_t;

#define BLOCK_SIZE sizeof(block_t)         // represent the block size
#define PAGE_SIZE sysconf(_SC_PAGE_SIZE)   // represent the size of 

block_t * head = NULL;

// locking
volatile bool locked = 0;

// thread-safe method
static inline void safelock() {
  if (!__sync_bool_compare_and_swap(&locked, 0, 1)) {
    int i = 0;
    do {
      if (__sync_bool_compare_and_swap(&locked, 0, 1))
      break;
      else {
        if(i == 10) {
          i = 0;
          sched_yield();
          } else
          ++i;
          }
          } while (1);
        }
}

#define lock safelock();

#define unlock \
    __asm__ __volatile__ ("" ::: "memory"); \
    locked = 0;


// a help method to create the block inside the block list and return the pointer to that block
void * createblock(size_t s, block_t ** block, block_t ** prevblock) {
  if (*block == NULL) {
    *block = mmap(NULL, BLOCK_SIZE + s, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    //*block = sbrk(BLOCK_SIZE + s);
    (*block)->size = s;
    (*block)->free = 0;
    (*block)->prev = *prevblock;
    (*block)->next = NULL;
    return *block;

  }  else if (((*block)->free == 1) && ((*block)->size >= s + BLOCK_SIZE + sizeof(char))) {
    block_t * result = *block;
    size_t currentSize = (*block)->size;
    block_t * currentNext = (*block)->next;
    block_t * leftover = (block_t *)((char * )*block + BLOCK_SIZE + s);

    result->free = 0;
    result->size = s;
    result->prev = *prevblock;
    result->next = leftover;

    leftover->free = 1;
    leftover->size = currentSize - BLOCK_SIZE - s;
    leftover->prev = result;
    leftover->next = currentNext;

    return result;
  } else if (((*block)->free == 1) && ((*block)->size >= s)) {
    (*block)->free = 0;
    return *block;
  } else {
    return createblock(s, &((*block)->next), &(*block));
  }
}

// find the end block (not NULL)
void * findlast(block_t ** block) {
  if ((*block)->next == NULL) {
    return *block;
  } else {
    findlast(&((*block)->next));
  }
}

// get the n of PAGESIZE we need to allocate
int getsize(size_t s) {
    int result = 1;
    while (PAGE_SIZE * result < s) {
        result += 1;
    }
    return result;
}

// malloc
void *mymalloc(size_t s) {
  if (head == NULL) {
    head = mmap( NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    //head = (block_t *)sbrk(BLOCK_SIZE);
    head->size = 0;
    head->free = 0;
    head->prev = NULL;
    head->next = NULL;
  }
  
  lock

  if (s >= PAGE_SIZE - BLOCK_SIZE) {
    block_t * endblock = findlast(&head);
    int ns = getsize(s + BLOCK_SIZE);
    block_t * newblock = mmap(NULL, ns * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    endblock->next = newblock;

    newblock->size = ns;
    newblock->free = 0;
    newblock->prev = endblock;
    newblock->next = NULL;

    // return the memory part of the block
    void * p = (void *)((char * )newblock + BLOCK_SIZE);
    p = (void *)((char * )p + BLOCK_SIZE);
    debug_printf("malloc %zu bytes\n", s);
    unlock
    return p;
  } else {
    // get the block that contain assigned memory part
    void * p =(void *) createblock(s, &head->next, &head);
    p = (void *)((char * )p + BLOCK_SIZE);
    debug_printf("malloc %zu bytes\n", s);
    unlock
    return p;
  }
}

// calloc
void *mycalloc(size_t nmemb, size_t s) {

  size_t callocsize = nmemb * s;
  
  void * p =(void *) mymalloc(callocsize);

  if (p != NULL) {
    memset(p, '\0', callocsize);
  }
  
  debug_printf("calloc %zu bytes\n", s);

  return p;
}

// coalescing helper, 
// it will go through the block list to the end to merge the two connected free block
void coalescing(block_t ** block) {
  if (*block == NULL) {
    return;
  }

  block_t * nextblock = (*block)->next;
  if (nextblock == NULL) {
    return;
  }
  block_t * nextnextblock = nextblock->next;
  if (((*block)->free == 1) && (nextblock->free == 1) && (nextblock == (*block + (*block)->size + BLOCK_SIZE))) {
    size_t newsize = (*block)->size + nextblock->size + BLOCK_SIZE;

    if (newsize >= PAGE_SIZE - BLOCK_SIZE) {
      coalescing(&nextblock);
      return;
    }

    // change the size and next
    (*block)->size = newsize;
    (*block)->next = nextnextblock;
    // change the prev of the nextnextblock to skip the nextblock
    // if it is NULL, do nothing
    if (nextnextblock != NULL) {
      nextnextblock->prev = *block;
    }
    coalescing(&(*block));
    return;
  }
  coalescing(&nextblock);
  return;
}

// free
void myfree(void *ptr) {
  lock
  block_t * p = (block_t *)ptr - 1;

  // if a block of size is large or equal to PAGE_SIZE
  if (sizeof(p) >= PAGE_SIZE) {
    block_t * prevblock = p->prev;
    block_t * nextblock = p->next;
    prevblock->next = nextblock;
    nextblock->prev = prevblock;
    debug_printf("Freed %zu bytes\n", p->size * PAGE_SIZE);
    unlock;
    munmap(p, p->size * PAGE_SIZE);
    return;
  }

  // if the block size is less than PAGE_SIZE
  p->free = 1;
  debug_printf("Freed %zu bytes\n", p->size);
  // coalscing the connected free blocks
  coalescing(&head);
  unlock
}