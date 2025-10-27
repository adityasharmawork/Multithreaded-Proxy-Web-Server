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
#define MAX_ELEMENT_SIZE 10*(1<<10) // 10 * 2^10
#define MAX_SIZE 200*(1<<20) // 200 * 2^20

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

int connectRemoteServer(char* host_addr, int port_num) { // Function connecting our proxy server to final main server
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0); // Socket on the Final main Remote Server which our proxy server connects to
    if(remoteSocket < 0) {
        printf("Error in creating your socket to Remote Server\n");
        return -1;
    }

    struct hostent* host = gethostbyname(host_addr);
    if(host == NULL) {
        fprintf(stderr, "No such host exists\n");
        return -1;
    }

    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);

    // Actual - 
    // bcopy((char *)&host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);
    // Fixed - 
    bcopy((char *)host->h_addr_list[0], (char *)&server_addr.sin_addr.s_addr, host->h_length);

    if(connect(remoteSocket, (struct sockaddr *)&server_addr, (size_t)sizeof(server_addr)) < 0) {
        fprintf(stderr, "Error in connecting\n");
        return -1;
    }

    return remoteSocket;
}

int handle_request(int clientSocketId, ParsedRequest* request, char* tempReq) {
    char* buf = (char*)malloc((sizeof(char) * MAX_BYTES));
    strcpy(buf, "GET");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf);

    if(ParsedHeader_set(request, "Connection", "close") < 0) {
        printf("Set header key is not working\n");
    }

    if(ParsedHeader_get(request, "Host") == NULL) {
        if(ParsedHeader_set(request, "Host", request->host) < 0) {
            printf("Set host header key is not working\n");
        }
    }

    if(ParsedRequest_unparse_headers(request, buf+len, (size_t)MAX_BYTES-len) < 0) {
        printf("Unparse failed\n");
    }

    int server_port = 80; // This is the port of the actual main server, not the proxy server (In most of the cases, the servers handling HTTP requests end with 80)
    if(request->port != NULL) {
        server_port = atoi(request->port);
    }

    int remoteSocketId = connectRemoteServer(request->host, server_port);
    if(remoteSocketId < 0) {
        fprintf(stderr, "Error in generating a Remote Socket Id\n");
        return -1;
    }

    // The below is the implementation of sending bytes from our proxy server to the actual main remote server
    int bytes_send = send(remoteSocketId, buf, strlen(buf), 0);
    bzero(buf, MAX_BYTES);

    // The below is the implementation of receiving bytes from the actual main remote server to our proxy server
    bytes_send = recv(remoteSocketId, buf, MAX_BYTES-1, 0); // Assuming we will keep on receiving bytes upto MAX_BYTES and as they appear one by one as "a b c ... \0" (The last character is \0, this is why we are using MAX_BYTES-1)
    char * temp_buffer = (char*)malloc(sizeof(char)*MAX_BYTES);
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    // The below shows that we need to receive the bytes from the actual main remote server to our proxy server till bytes_send (Which actually refers to bytes received in this case) > 0
    while(bytes_send > 0) { // The bytes_send actually refers to bytes we receive here, and we will keep on receiving these bytes on socket till we keep getting it
        bytes_send = send(clientSocketId, buf, bytes_send, 0); // We are sending all bytes our proxy server is receiving from the actual main remote server to the client socket
        
        for(int i = 0; i < bytes_send / sizeof(char); i++) { // Ultimately at last we need to store this bytes data to our cache as well, so we need to store it to buffer as well
            temp_buffer[temp_buffer_index] = buf[i];
            temp_buffer_index++;
        }

        temp_buffer_size += MAX_BYTES;
        temp_buffer = (char*)realloc(temp_buffer, temp_buffer_size);

        if(bytes_send < 0) {
            perror("Error in sending data to the client\n");
            break;
        }
        bzero(buf, MAX_BYTES);

        // As soon as we receive the bytes data from the actual main remote server, we are continuously sending it to client, so we are first sending it to the client socket,
        // Then, storing it in the buffer, so that later it can be stored in the cache,
        // Then, again receiving next set of bytes from the actual main remote server
        bytes_send = recv(remoteSocketId, buf, MAX_BYTES-1, 0);
    }

    temp_buffer[temp_buffer_index] = '\0'; // This ensures for example, even if the temp_buffer had the capacity of holding 1000 characters but if it has only 5 characters, it will add a '\0' at 6th position, making it converting to string very possible
    free(buf); // Freeing all dynamically created values
    add_cache_element(temp_buffer, strlen(temp_buffer), tempReq); // Adding temp_bufer to cache
    free(temp_buffer); // Freeing all dynamically created values
    close(remoteSocketId); // Closing the Socket connected to the Remote Server
    
    return 0; // Everything worked well, with no errors

}

int checkHTTPversion(char * msg) {
    
    int version = -1;
    
    if(strncmp(msg, "HTTP/1.1", 8) == 0) {
        version = 1;
    } else if(strncmp(msg, "HTTP/1.0", 8) == 0) {
        version = 1;
    } else {
        version = -1;
    }

    return version;
}

int sendErrorMessage(int socket, int status_code) {
    char str[1024];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    switch(status_code)
	{
		case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
				  printf("400 Bad Request\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
				  printf("403 Forbidden\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
				  printf("404 Not Found\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
				  //printf("500 Internal Server Error\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
				  printf("501 Not Implemented\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
				  printf("505 HTTP Version Not Supported\n");
				  send(socket, str, strlen(str), 0);
				  break;

		default:  return -1;

	}

    return 1;
}

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

cache_element* find(char* url) {
    cache_element* site = NULL;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Remove cache Lock acquired : %d\n", temp_lock_val);
    if(head != NULL) {
        site = head;
        while(site != NULL) {
            if(!strcmp(site->url, url)) {
                printf("LRU time track before : %ld", site->lru_time_track);
                printf("\n URL Found\n");
                // As we are using an LRU Cache and as this URL is just now used, we need to set it's time to now, i.e., time(NULL);
                site->lru_time_track = time(NULL);
                printf("LRU time track after : %ld\n", site->lru_time_track);
                break;
            }
            site = site->next;
        }
    } else {
        printf("URL not found\n");
    }

    temp_lock_val = pthread_mutex_unlock(&lock); // Releasing the Lock
    printf("Lock is unlocked\n");

    return site;

}

int add_cache_element(char* data, int size, char* url) { // Adding elements to cache
    int temp_lock_val = pthread_mutex_lock(&lock); // Acquiring the Lock
    printf("Add Cache Lock Acquired : %d\n", temp_lock_val);
    
    int element_size = size+1+strlen(url)+sizeof(cache_element); // size -> Input element buffer size, strlen(url) -> size of url, sizeof(cache_element) -> size of struct
    
    if(element_size > MAX_ELEMENT_SIZE) { // Cannot add to cache
        temp_lock_val = pthread_mutex_unlock(&lock); // Release all locks and return 0 showing this cannot be added
        printf("Add cache lock is unlocked\n");
        return 0;
    } else { // Can be added
        while(cache_size + element_size > MAX_SIZE) { // cache_size -> size that existing elements in cache have already taken up, element_size -> size of the new element to be added to cache
            remove_cache_element(); // Keep on removing cache elements until we get a free space to fit the new element to cache
        }

        // After that, now we can add the new element to the cache
        // Now, let us create the cache_element to be added to cache
        cache_element* element = (cache_element*)malloc(sizeof(cache_element)); // Dynamically creating space for the new element to add to cache
        element->data = (char*)malloc(sizeof(size+1)); // Size of the new input buffer element to be added to cache, +1 as because at last we are reqired to store an extra data
        strcpy(element->data, data);
        element->url = (char*)malloc(1 + (strlen(url) * sizeof(char)));
        strcpy(element->url, url);
        element->lru_time_track = time(NULL); // We are adding the element now, that means it is being now, so lru_time_track for this cache element will be set to time(NULL)
        element->next = head; // The next of this cache element will be set to head, i.e., if it is the first element of the cache_element linked list, the next of this first element will be set to NULL
        
    }
}