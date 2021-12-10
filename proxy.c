#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define MAXEVENTS 100
#define MAXLINE 25
#define LISTENQ 50
#define HTTP_REQUEST_MAX_SIZE 4096

#define HOSTNAME_MAX_SIZE 512
#define PORT_MAX_SIZE 6
#define URI_MAX_SIZE 4096
#define METHOD_SIZE 32
#define BUF_SIZE 500

#define MAX_OBJECT_SIZE 102400

#define READ_REQUEST 1
#define SEND_REQUEST 2
#define READ_RESPONSE 3
#define SEND_RESPONSE 4


/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
int g_edge_triggered = 1;
int killed_SIGINT = 0;


/*
the socket corresponding to the requesting client
the socket corresponding to the connection to the Web server
the current state of the request (see Client Request States)
the buffer to read into and write from
the total number of bytes read from the client
the total number of bytes to write to the server
the total number of bytes written to the server
the total number of bytes read from the server
the total number of bytes written to the client
*/

typedef struct client_request {
    int clientfd;
    int serverfd;
    int request_state;
    char read_buff[MAX_OBJECT_SIZE];
    int bytes_read_client;
    int bytes_to_write_server;
    int bytes_written_server;
    int bytes_read_server;
    int bytes_written_client;
} client_request;

void sig_int_handler(int signum);
int open_listen_fd(char *port);
int is_complete_request(const char *request);
void get_remaining_headers(char *headers, const char *request);
int parse_request(const char *request, char *method,
                  char *hostname, char *port, char *uri, char *headers);
void handle_new_connection(int epollfd, struct epoll_event *ev);
void handle_client_request(int epollfd, struct epoll_event *ev);
void read_from_client(int epollfd, struct epoll_event *ev, struct client_request *request);
void write_to_server(int epollfd, struct epoll_event *ev, struct client_request *request);
void read_from_server(int epollfd, struct epoll_event *ev, struct client_request *request);
void write_to_client(int epollfd, struct epoll_event *ev, struct client_request *request);
void handle_complete_request(int epollfd, struct client_request *request);
void modsocket(int epollfd, int cfd, client_request *ri);

// Imagine network events as a binary signal pulse.
// Edge triggered epoll only returns when an edge occurs, ie. transitioning from 0 to 1 or 1 to 0.
// Regardless for how long the state stays on 0 or 1.
//
// Level triggered on the other hand will keep on triggering while the state stays the same.
// Thus you will read some of the data and then the event will trigger again to get more data
// You end up being triggered more to handle the same data
//
// Use level trigger mode when you can't consume all the data in the socket/file descriptor
// and want epoll to keep triggering while data is available.
//
//Typically one wants to use edge trigger mode and make sure all data available is read and buffered.



int main(int argc, char **argv)
{
    int listenfd;

    /* Error handling */
    struct sigaction sigact;
    sigact.sa_flags = 0;

    sigact.sa_handler = sig_int_handler;
    sigaction(SIGINT, &sigact, NULL);

    struct epoll_event event, events[100000];

    int epollfd = epoll_create(LISTENQ);

    if (epollfd < 0) {
        printf("Unable to create epoll fd\n");
        exit(1);
    }

    if (argc < 2) {
        printf("Usage: %s portnumber\n", argv[0]);
        exit(1);
    }

    listenfd = open_listen_fd(argv[1]);

    event.data.fd = listenfd;
    event.events = EPOLLIN | EPOLLET;

    int s = epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &event);
    if (s == -1) {
        perror("epoll_ctl");
        abort();
    }

    while (1) {
        int rc = epoll_wait(epollfd, events, MAXEVENTS, 1000);

        if (rc == 0) {
            printf("Returned 0, checking stuff\n");
            if (killed_SIGINT == 1) {
                int i = 0;
                printf("Freeing unused pointer memory\n");
                while (events[i].data.ptr != NULL) {
                    free(events[i].data.ptr);
                    i++;
                }
                break;
            }
            else {
                continue;
            }
        }

        for (int i = 0; i < rc; i++) {
            if (((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) || (!(events[i].events & EPOLLIN))) {
                fprintf(stderr, "something happened!\n");
                fprintf(stderr, "epoll error\n");
                close(events[i].data.fd);
                free(events[i].data.ptr);
                continue;
            }
            else if (events[i].data.fd == listenfd) {
                handle_new_connection(epollfd, &events[i]);
            }
            else if (events[i].events & EPOLLIN) {
                handle_client_request(epollfd, &events[i]);
            }
            else {
                printf("Got some other event\n");
            }
        }
    }

    return 0;
}



int open_listen_fd(char *port) {
    int listenfd;
    struct sockaddr_in ip4addr;

    ip4addr.sin_family = AF_INET;
    ip4addr.sin_port = htons(atoi(port));
    ip4addr.sin_addr.s_addr = INADDR_ANY;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket error");
        exit(EXIT_FAILURE);
    }


    if (bind(listenfd, (struct sockaddr*)&ip4addr, sizeof(struct sockaddr_in)) < 0) {
        close(listenfd);
        perror("bind error");
        exit(EXIT_FAILURE);
    }

    if (listen(listenfd, 100) < 0) {
        close(listenfd);
        perror("Listen error");
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(listenfd, F_GETFL, 0);
    fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

    return listenfd;
}


void get_remaining_headers(char *headers, const char *request) {

    char *token;
    char requestcopy[MAX_OBJECT_SIZE];
    memset(requestcopy, 0, MAX_OBJECT_SIZE);
    strcpy(requestcopy, request);

    /* get the first token */
    token = strtok(requestcopy, "\r\n");

    token = strtok(NULL, "\r\n");

    int headerslen = 0;

    /* walk through other tokens */
    while(token != NULL) {
        char headername[100];
        char *headernameend = strchr(token, ':');
        char currheader[100];

        strcpy(currheader, token);
        *headernameend = '\0';
        strcpy(headername, token);
        if (strcmp(headername, "Host") == 0 || strcmp(headername, "User-Agent") == 0 ||
            strcmp(headername, "Proxy-Connection") == 0 || strcmp(headername, "Connection") == 0) {
        }
        else {
            strcpy(headers+headerslen, currheader);
            headerslen += strlen(currheader);
            strcpy(headers+headerslen, "\r\n");
            headerslen += 2;
        }
        token = strtok(NULL, "\r\n");
    }

    strcpy(headers+headerslen, "\r\n");
}

int parse_request(const char *request, char *method,
                  char *hostname, char *port, char *uri, char *headers) {

    char *hostport = (char*)calloc(HTTP_REQUEST_MAX_SIZE, sizeof(char));
    char *version = (char*)calloc(HTTP_REQUEST_MAX_SIZE, sizeof(char));
    sscanf(request, "%s %s %s\r\n", method, hostport, version);

    char *begin = strstr(hostport, "//") + 2;
    char *end = strchr(begin, '/');

    *end = '\0';

    strcpy(hostname, begin);
    strcpy(uri, end+1);

    char *portstart = strchr(begin, ':');
    if (portstart == NULL) {
        strcpy(port, "80");
    }
    else {
        strcpy(port, portstart+1);
        char *hostend = strstr(hostname, port) - 1;
        *hostend = '\0';

    }

    get_remaining_headers(headers, request);

    free(hostport);
    free(version);

    return 1;
}

void handle_new_connection(int epollfd, struct epoll_event *ev) {
    struct sockaddr_in in_addr;
    unsigned int addr_size = sizeof(in_addr);
    char hbuf[MAXLINE], sbuf[MAXLINE];

    while (1) {

        int cfd = accept(ev->data.fd, (struct sockaddr *) &in_addr, &addr_size);

        if (cfd < 0) {
            break;
        }

        int s = getnameinfo((struct sockaddr *) &in_addr,
                            addr_size,
                            hbuf, sizeof(hbuf),
                            sbuf, sizeof(sbuf),
                            NI_NUMERICHOST | NI_NUMERICSERV);

        if (s == 0) {
            printf("Accepted connection on descriptor %d (host=%s, port=%s)\n", cfd, hbuf, sbuf);
        }

        int flags = fcntl(cfd, F_GETFL, 0);
        fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

        client_request *ri = (client_request *) calloc(1, sizeof(client_request));

        ri->clientfd = cfd;
        ri->bytes_read_client = 0;
        ri->request_state = READ_REQUEST;

        modsocket(epollfd, cfd, ri);
    }
}


void modsocket(int epollfd, int cfd, client_request* ri) {
    int s;

    struct epoll_event event;
    event.data.fd = cfd;

    if (ri->request_state == READ_REQUEST || ri->request_state == READ_RESPONSE) {
        event.events = EPOLLIN | EPOLLET;
    }
    else {
        event.events = EPOLLOUT | EPOLLET;
    }

    event.data.ptr = ri;

    if (ri->request_state == READ_REQUEST || ri->request_state == SEND_REQUEST) {
        s = epoll_ctl(epollfd, EPOLL_CTL_ADD, cfd, &event);
    }
    else {
        s = epoll_ctl(epollfd, EPOLL_CTL_MOD, cfd, &event);
    }

    if (s == -1) {
        perror("modsocket");
        abort();
    }
}

void handle_client_request(int epollfd, struct epoll_event *ev) {

    client_request *request = (client_request *)ev->data.ptr;

    if (request->request_state == READ_REQUEST) {
        read_from_client(epollfd, ev, request);
    }

    if (request->request_state == SEND_REQUEST) {
        write_to_server(epollfd, ev, request);
    }


    if (request->request_state == READ_RESPONSE) {
        read_from_server(epollfd, ev, request);
    }

    if (request->request_state == SEND_RESPONSE) {
        write_to_client(epollfd, ev, request);
    }

    if (request->request_state == 5) {
        free(ev->data.ptr);
    }
}


void read_from_client(int epollfd, struct epoll_event *ev, struct client_request *request) {

    int n;

    int done = 0;

    for (int i = 0; !done; i++) {
        n = read(request->clientfd, request->read_buff+request->bytes_read_client, MAXLINE);

        if (n != -1) {
            request->bytes_read_client += n;
        }

        if (!g_edge_triggered) {
            printf("Done & not edge triggered\n");
            done = 1;
        }

        if (n == -1) {
            done = 1;
        }

        if (n == 0) {
            done = 1;
            if (i == 0) {
                close(request->clientfd);
            }
        }
        else {

            if (is_complete_request(request->read_buff)) {
                done = 1;

                request->request_state = SEND_REQUEST;

                handle_complete_request(epollfd, request);

                modsocket(epollfd, request->serverfd, request);
            }
        }
    }


}

void handle_complete_request(int epollfd, struct client_request *request) {
    char *method;
    char *hostname;
    char *port;
    char *uri;
    char *headers;

    char buf[MAX_OBJECT_SIZE];

    /* We have data on the fd waiting to be read. Read and
    display it. If edge triggered, we must read whatever data is available
    completely, as we are running in edge-triggered mode
    and won't get a notification again for the same data.
    */

    method = (char *) calloc(METHOD_SIZE + 1, sizeof(char));
    hostname = (char *) calloc(HOSTNAME_MAX_SIZE + 1, sizeof(char));
    port = (char *) calloc(PORT_MAX_SIZE + 1, sizeof(char));
    uri = (char *) calloc(URI_MAX_SIZE + 1, sizeof(char));
    headers = (char *) calloc(BUF_SIZE + 1, sizeof(char));


    parse_request(request->read_buff, method, hostname, port, uri, headers);

    int bufferlocation = 0;

    ////HEADERS
    bufferlocation = sprintf(buf, "%s /%s HTTP/1.0\r\n", method, uri);
    bufferlocation += sprintf(buf + bufferlocation, "Host: %s\r\n", hostname);
    bufferlocation += sprintf(buf + bufferlocation, "User-Agent: %s", user_agent_hdr);
    bufferlocation += sprintf(buf + bufferlocation, "Connection: close\r\n");
    bufferlocation += sprintf(buf + bufferlocation, "Proxy-Connection: close\r\n");
    sprintf(buf + bufferlocation, "%s", headers);

    request->bytes_to_write_server = strlen(buf);


    ////RESET
    memset(request->read_buff, 0, MAX_OBJECT_SIZE);
    strcpy(request->read_buff, buf);


    ////NEW SOCKET
    struct sockaddr_in webserver;
    int webserverfd = socket(AF_INET, SOCK_STREAM, 0);

    if (webserverfd == -1) {
        printf("could not create socket\n");
    }

    webserver.sin_family = AF_INET;
    webserver.sin_port = htons(atoi(port));
    webserver.sin_addr.s_addr = INADDR_ANY;


    if (connect(webserverfd, (struct sockaddr *) &webserver, sizeof(webserver)) < 0) {
        printf("Connection failed\n");
    } else {
        printf("Opened connection to webserver on port [%s] and saved to request\n", port);
        request->serverfd = webserverfd;
    }

    int flags = fcntl(webserverfd, F_GETFL, 0);
    fcntl(webserverfd, F_SETFL, flags | O_NONBLOCK);

    free(method);
    free(port);
    free(hostname);
    free(uri);
    free(headers);

}

void write_to_server(int epollfd, struct epoll_event *ev, struct client_request *request) {

    int n = 0;

    while (request->bytes_written_server < request->bytes_to_write_server) {
        n = send(request->serverfd, request->read_buff+request->bytes_written_server, strlen(request->read_buff), 0);

        if (n != -1){
            request->bytes_written_server += n;
        }
        else {
            break;
        }

        if (!g_edge_triggered) {
            printf("Done & not edge triggered\n");
            break;
        }
        if (n == 0) {
            break;
        }
    }

    if (request->bytes_written_server == request->bytes_to_write_server) {
        request->request_state = READ_RESPONSE;
        memset(request->read_buff, 0, MAX_OBJECT_SIZE);
        modsocket(epollfd, request->serverfd, request);
    }

}


void read_from_server(int epollfd, struct epoll_event *ev, struct client_request *request) {
    int n;
    int done = 0;

    while(!done) {
        n = read(request->serverfd, request->read_buff+request->bytes_read_server, MAXLINE);

        if (n > 0) {
            request->bytes_read_server += n;
        }

        if (!g_edge_triggered) {
            printf("Done & not edge triggered\n");
            done = 1;
        }
        if (n == 0) {
            done = 1;
            request->request_state = SEND_RESPONSE;
            modsocket(epollfd, request->clientfd, request);
            close(request->serverfd);
        }

        if (n == -1) {
            done = 1;
        }
    }
}

void write_to_client(int epollfd, struct epoll_event *ev, struct client_request *request) {

    int n = 0;

    while (request->bytes_written_client < request->bytes_read_server) {
        n = send(request->clientfd, request->read_buff+request->bytes_written_client, request->bytes_read_server, 0);

        if (n != -1) {
            request->bytes_written_client += n;
        }
        else {
            break;
        }

        if (!g_edge_triggered) {
            printf("Done & not edge triggered\n");
            break;
        }
        if (n == 0) {
            break;
        }

    }

    if (request->bytes_written_client == request->bytes_read_server) {
        request->request_state = 5;
        close(request->clientfd);
    }
}


void sig_int_handler(int signum) {
    killed_SIGINT = 1;
}

int is_complete_request(const char *request) {

    //printf("buff[%d]=[%c] bytesread = %d\n", n-1, buff[n-1], bytesread);
    //Client is sending hello\n\0 so need to back off 2
    const char *ret = strstr(request, "\r\n\r\n");
    if (ret != NULL) {
        return 1;
    }
    return 0;
}

