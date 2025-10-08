#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct cache_element cache_element;

struct cache_element { // This is basically a list of all cache elements (cache_element_list), a linked list
    char* data; // The data we received as response when we fetched the server for a specific request
    int len; // The length of the data (number of bytes)
    char* url; // The URL for which this cache is stored
    time_t lru_time_track; // To implement LRU cache
    cache_element* next;
};

