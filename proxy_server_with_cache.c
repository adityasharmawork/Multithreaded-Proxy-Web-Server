#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include<pthread.h>

#define MAX_CLIENTS 10
typedef struct cache_element cache_element;


struct cache_element { // This is basically a list of all cache elements (cache_element_list), a linked list
    char* data; // The data we received as response when we fetched the server for a specific request
    int len; // The length of the data (number of bytes)
    char* url; // The URL for which this cache is stored
    time_t lru_time_track; // To implement LRU cache
    cache_element* next;
};

cache_element* find(char* url);
int add_cache_element(char* data, int size, char* url)
void remove_cache_element();

int port_number = 8080;
int proxy_socketId;

// We will create new separate threads for each new client request, to make our application multithreaded (Handles multiple concurrent requests)
// As soon as any client sends a request to our proxy server, we will create a new thread for that and open a new socket for handling those client requests

pthread_t tid[MAX_CLIENTS];