#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define HASHTABLE_OK   0
#define HASHTABLE_ERR  1
#define HASHTABLE_OOM  2
#define HASHTABLE_FULL 3

struct hashtable_entry
{
  const char *key;
  void *val;
  bool taken;
};


typedef struct hashtable_entry HashTable;

HashTable *hahstable_create(int (*destructor)(struct hashtable_entry*));

void hashtable_release(HashTable *);

size_t hashtable_size(const HashTable *);

void *hashtable_get(HashTable *, const char *);

int hashtable_del(HashTable *, const char *);

int  hashtable_map(HashTable *, int (*func)(struct hashtable_entry *));

int hashtable_map2(HashTable *, 
		   int (*func)(struct hashtable_entry *, void *), void *);
#endif


