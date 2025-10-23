#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

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
int add_cache_element(char* data, int size, char* url);
void remove_cache_element();

int port_number = 8080;
int proxy_socketId;

// We will create new separate threads for each new client request, to make our application multithreaded (Handles multiple concurrent requests)
// As soon as any client sends a request to our proxy server, we will create a new thread for that and open a new socket for handling those client requests

pthread_t tid[MAX_CLIENTS];

sem_t semaphore;
pthread_mutex_t lock;

cache_element* head;
int cache_size;

int main(int argc, char* argv[]) {
    int client_socketId, client_len;
    struct sockaddr server_addr, client_addr;
    sem_init(&semaphore, 0, MAX_CLIENTS); // 0 is the minimum value of sempahore
    pthread_mutex_init(&lock, NULL); // We need to define it NULL, as in C Language evevrything is garbage value by default

}