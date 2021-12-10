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


#define READ_REQUEST 1
#define SEND_REQUEST 2
#define READ_RESPONSE 3
#define SEND_RESPONSE 4
#define MAXEVENTS 200
#define MAXLINE 25
#define LISTENQ 50
#define HTTP_REQUEST_MAX_SIZE 4096
#define MAX_OBJECT_SIZE 102400
#define HOSTNAME_MAX_SIZE 512
#define PORT_MAX_SIZE 6
#define URI_MAX_SIZE 4096
#define METHOD_SIZE 32
#define BUF_SIZE 1024
#define HEADER_SIZE 100

#define END_MESSAGE "\r\n\r\n"




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

typedef struct request_info{
    int clientfd;
    int sfd;
    int req_state;
    char buff[MAX_OBJECT_SIZE];
    int bytes_read_client;
    int bytes_to_write_server;
    int bytes_written_server;
    int bytes_read_server;
    int bytes_written_client;
} request_info;

void sig_int_handler(int signum);
int open_listen_fd(char *port);
int is_complete_request(const char *request);
void get_remaining_headers(char *headers, const char *request);
int parse_request(const char *request, char *method,
                  char *hostname, char *port, char *uri, char *headers);
void modsocket(int epollfd, int cfd, request_info *ri);
void handle_new_connection(int epollfd, struct epoll_event *ev);
void handle_client_request(int epollfd, struct epoll_event *ev);
void handle_complete_request(int epollfd, struct request_info*request);
void read_from_client(int epollfd, struct epoll_event *ev, struct request_info *request);
void read_from_server(int epollfd, struct epoll_event *ev, struct request_info *request);
void write_to_server(int epollfd, struct epoll_event *ev, struct request_info *request);
void write_to_client(int epollfd, struct epoll_event *ev, struct request_info *request);


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


//void echo_data(int fd, char *buff, int size)
//{
//    printf("Sending: %s\n", buff);
//    write(fd, buff, size+1);
//}

int main(int argc, char **argv)
{
    int listenfd;

    /* Error handling */
    struct sigaction sigact;
    sigact.sa_flags = 0;

    sigact.sa_handler = sig_int_handler;
    sigaction(SIGINT, &sigact, NULL);

    struct epoll_event event, events[MAX_OBJECT_SIZE];

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
    int header_size= 0;
    memset(requestcopy, 0, MAX_OBJECT_SIZE);
    strcpy(requestcopy, request);

    /* get the first token */
    token = strtok(requestcopy, "\r\n");

    token = strtok(NULL, "\r\n");


    /* walk through other tokens */
    while(token != NULL) {
        char headername[HEADER_SIZE];
        char *header_end = strchr(token, ':');
        char my_header[HEADER_SIZE];

        strcpy(my_header, token);
        *header_end = '\0';
        strcpy(headername, token);
        if (!(strcmp(headername, "Host") == 0 || strcmp(headername, "User-Agent") == 0 ||
            strcmp(headername, "Proxy-Connection") == 0 || strcmp(headername, "Connection") == 0)){
            strcpy(headers+header_size, my_header);
            header_size += strlen(my_header);
            strcpy(headers+header_size, "\r\n");
            header_size += 2;
        }
        token = strtok(NULL, "\r\n");
    }

    strcpy(headers+header_size, "\r\n");
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

        request_info *ri = (request_info *) calloc(1, sizeof(request_info));

        ri->clientfd = cfd;
        ri->bytes_read_client = 0;
        ri->req_state = READ_REQUEST;

        modsocket(epollfd, cfd, ri);
    }
}


void modsocket(int epollfd, int cfd, request_info* ri) {
    int s;

    struct epoll_event event;
    event.data.fd = cfd;

    if (ri->req_state == READ_REQUEST || ri->req_state == READ_RESPONSE) {
        event.events = EPOLLIN | EPOLLET;
    }
    else {
        event.events = EPOLLOUT | EPOLLET;
    }

    event.data.ptr = ri;

    if (ri->req_state == READ_REQUEST || ri->req_state == SEND_REQUEST) {
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


//TODO:CLEAN SPACE FROM MAP??
void handle_client_request(int epollfd, struct epoll_event *ev) {

    request_info *request = (request_info *)ev->data.ptr;

    if (request->req_state == READ_REQUEST) {
        read_from_client(epollfd, ev, request);
    }

    if (request->req_state == SEND_REQUEST) {
        write_to_server(epollfd, ev, request);
    }


    if (request->req_state == READ_RESPONSE) {
        read_from_server(epollfd, ev, request);
    }

    if (request->req_state == SEND_RESPONSE) {
        write_to_client(epollfd, ev, request);
    }

    if (request->req_state == 5) {
        free(ev->data.ptr);
    }
}


void read_from_client(int epollfd, struct epoll_event *ev, struct request_info *request) {

    int n;

    int done = 0;

    for (int i = 0; !done; i++) {
        n = read(request->clientfd, request->buff+request->bytes_read_client, MAXLINE);

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

            if (is_complete_request(request->buff)) {
                done = 1;

                request->req_state = SEND_REQUEST;

                handle_complete_request(epollfd, request);

                modsocket(epollfd, request->sfd, request);
            }
        }
    }


}

void handle_complete_request(int epollfd, struct request_info *request) {
    char *method;
    char *hostname;
    char *port;
    char *uri;
    char *headers;

    char buf[MAX_OBJECT_SIZE];

    method = (char *) calloc(METHOD_SIZE + 1, sizeof(char));
    hostname = (char *) calloc(HOSTNAME_MAX_SIZE + 1, sizeof(char));
    port = (char *) calloc(PORT_MAX_SIZE + 1, sizeof(char));
    uri = (char *) calloc(URI_MAX_SIZE + 1, sizeof(char));
    headers = (char *) calloc(BUF_SIZE + 1, sizeof(char));


    parse_request(request->buff, method, hostname, port, uri, headers);

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
    memset(request->buff, 0, MAX_OBJECT_SIZE);
    strcpy(request->buff, buf);


    ////NEW SOCKET
    struct sockaddr_in webserver;
    int websfd = socket(AF_INET, SOCK_STREAM, 0);

    if (websfd == -1) {
        printf("could not create socket\n");
    }

    webserver.sin_family = AF_INET;
    webserver.sin_port = htons(atoi(port));
    webserver.sin_addr.s_addr = INADDR_ANY;


    if (connect(websfd, (struct sockaddr *) &webserver, sizeof(webserver)) < 0) {
        printf("Connection failed\n");
    } else {
        printf("Opened connection to webserver on port [%s] and saved to request\n", port);
        request->sfd = websfd;
    }

    int flags = fcntl(websfd, F_GETFL, 0);
    fcntl(websfd, F_SETFL, flags | O_NONBLOCK);

    free(method);
    free(port);
    free(hostname);
    free(uri);
    free(headers);

}

void write_to_server(int epollfd, struct epoll_event *ev, struct request_info *request) {

    int n = 0;

    while (request->bytes_written_server < request->bytes_to_write_server) {
        n = send(request->sfd, request->buff+request->bytes_written_server, strlen(request->buff), 0);

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
        request->req_state = READ_RESPONSE;
        memset(request->buff, 0, MAX_OBJECT_SIZE);
        modsocket(epollfd, request->sfd, request);
    }

}


void read_from_server(int epollfd, struct epoll_event *ev, struct request_info *request) {
    int n;
    int done = 0;

    while(!done) {
        n = read(request->sfd, request->buff+request->bytes_read_server, MAXLINE);

        if (n > 0) {
            request->bytes_read_server += n;
        }

        if (!g_edge_triggered) {
            printf("Done & not edge triggered\n");
            done = 1;
        }
        if (n == 0) {
            done = 1;
            request->req_state = SEND_RESPONSE;
            modsocket(epollfd, request->clientfd, request);
            close(request->sfd);
        }

        if (n == -1) {
            done = 1;
        }
    }
}

void write_to_client(int epollfd, struct epoll_event *ev, struct request_info *request) {

    int n = 0;

    while (request->bytes_written_client < request->bytes_read_server) {
        n = send(request->clientfd, request->buff+request->bytes_written_client, request->bytes_read_server, 0);

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
        request->req_state = 5;
        close(request->clientfd);
    }
}


void sig_int_handler(int signum) {
    killed_SIGINT = 1;
}

int is_complete_request(const char *request) {

    //printf("buff[%d]=[%c] bytesread = %d\n", n-1, buff[n-1], bytesread);
    //Client is sending hello\n\0 so need to back off 2
    const char *ret = strstr(request, END_MESSAGE);
    if (ret != NULL) {
        return 1;
    }
    return 0;
}

