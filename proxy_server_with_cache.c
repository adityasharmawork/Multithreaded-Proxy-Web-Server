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
#define MAX_BYTES 4096

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

void * thread_fn(void * socketNew) { // void 8 means anything data type can be passed, it is like any keyword in Java
    sem_wait(&semaphore); // sem_wait reduces the value of semaphore by 1 and checks if there is space to create new threads for any new client request
    int p;
    sem_getvalue(&semaphore, p);
    printf("Semaphore value is : %d\n", p);
    int *t = (int*) socketNew;
    int socket = *t; // Copying the value of socketNew to socket
    
    int bytes_send_client, len;

    char* buffer = (char*)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);

    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0);

    while(bytes_send_client > 0) {
        len = strlen(buffer);
        if(substr(buffer, "\r\n\r\n") == NULL)  { // Every HTTP request ends with "\r\n\r\n", so until we get this, keep on receiving bytes data from request
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
        } else {
            break;
        }
    }

    // As soon as we get any request, we need to search it in our cache, for this we are making a copy of this request 
    // It is not necessary, but we do it for a good practice

    char *tempReq = (char *)malloc(strlen(buffer) * sizeof(char)+1);
    for(int i = 0; i < strlen(buffer); i++) {
        tempReq[i] = buffer[i];
    }

    struct cache_element* temp = find(tempReq);

    if(temp != NULL) { // If it exists in cache, we can use it
        int size =  temp->len/sizeof(char);
        int pos = 0;
        char response[MAX_BYTES];
        while(pos < size) {
            bzero(response, MAX_BYTES);
            for(int i = 0; i < MAX_BYTES; i++) {
                response[i] = temp->data[i];
                pos++;
            }
            send(socket, response, MAX_BYTES, 0);
        }
        printf("Data retrieved from the cache : \n");
        printf("%s\n\n", response);
    } else if(bytes_send_client > 0) { // If it is not there is cache, we need to use the ParsedRequest library
        len = strlen(buffer);
        ParsedRequest* request = ParsedRequest_create();
        
        if(ParsedRequest_parse(request, buffer, len) < 0) {
            printf("Parsing failed\n");
        } else {
            bzero(buffer, MAX_BYTES);
            if(!strcmp(request->method, "GET")) { // strcmp returns 0 if both the request->method and "GET" are equal, i.e. it tells if the request is calling a GET method
                if(request->host && request->path && checkHTTPversion(request->version) == 1) { // The libary checkHTTPversion accepts only HTTP version 1.0
                    bytes_send_client = handle_request(socket, request, tempReq);
                    if(bytes_send_client == -1) { // Actual Main Server having website data sends nothing in return
                        sendErrorMessage(socket, 500); // This is how dfferent HTTP Status codes are sent back
                    }
                } else { // The request couldn't be parsed by our server
                    sendErrorMessage(socket, 500);
                }
            } else {
                printf("Only GET method supported till now\n");
            }
        }
        ParsedRequest_destroy(request);
    } else if(bytes_send_client == 0) {
        printf("Client is disconnected\n");
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, p);

    printf("Semaphore post value is : %d\n", p);
    
    free(tempReq);

    return NULL;
}

int main(int argc, char* argv[]) {
    int client_socketId, client_len;
    struct sockaddr_in server_addr, client_addr;
    sem_init(&semaphore, 0, MAX_CLIENTS); // 0 is the minimum value of sempahore
    pthread_mutex_init(&lock, NULL); // We need to define it NULL, as in C Language evevrything is garbage value by default

    if(argv == 2) { // ./proxy 9090 (It means user has also provided a port number, so we should run it in that port instead of hard coded 8080)
        port_number = atoi(argv[1]);
    } else {
        printf("Too few arguments\n");
        exit(1);
    }

    printf("Starting Proxy Server at Port : %d\n", port_number);
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0); // AF_INET is for using IPV4, and SOCK_STREAM directs to use TCP over UDP for a better and secure connection with three way handshake

    if(proxy_socketId < 0) {
        perror("Failed to create a socket\n");
        exit(1);
    }

    int reuse = 1; // If the socket is made, we need to reuse that socket

    if(setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) {
        perror("setSockOpt failed\n");
    }

    bzero((char*)&server_addr, sizeof(server_addr)); // As C language by default, sets all defualt values to garbage values, we need to clean them
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Port is not available\n");
        exit(1);
    }

    printf("Binding on port : %d\n", port_number);

    int listen_status = listen(proxy_socketId, MAX_CLIENTS);
    if(listen_status < 0) {
        perror("Error in listening\n");
        exit(1);
    }

    int i = 0;
    int Connected_socketId[MAX_CLIENTS];

    while(1) {
        bzero((char *)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr);
        client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr, (socklen_t*)&client_len);
        if(client_socketId < 0) {
            printf("Not able to connect\n");
            exit(1);
        } else {
            Connected_socketId[i] = client_socketId;
        }
        struct sockaddr_in * client_pt = (struct sockaddr_in *)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN);
        printf("Client is connected with port numebr : %d and ip address : %s\n", ntohs(client_addr.sin_port), str);
    
        pthread_create(&tid[i], NULL, thread_fn, (void *) &Connected_socketId[i]); // To make new thread, and thread_fn defines the function that should run when creating a new thread for a new client
        i++;
    }
    close(proxy_socketId);
    return 0;

}
