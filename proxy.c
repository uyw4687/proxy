#include <stdio.h>
#include "csapp.h"

#include <signal.h>
#include <stdlib.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* My max response size */
#define MAX_REQUEST_SIZE 100000
#define MAX_RESPONSE_SIZE 1049000

#define MAX_HEADER_SIZE 100000

/* You won't lose style points for including this long line in your code */
static const char user_agent_hdr[] = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char connection_hdr[] = "Connection: close\r\n";
static const char proxy_connection_hdr[] = "Proxy-Connection: close\r\n";

struct cacheinfo{
    char *hostname;
    char *port;
    char *filename;
    char *content;
    int size;
    int time;
};

int numcache = 0;
struct cacheinfo *cache[1000];
int allocated[1000];
int sizeleft = MAX_CACHE_SIZE;
int cachetime = 0;

int ifhosthdr = 0;
pthread_rwlock_t lock;

void doit(int fd);
int read_requesthdrs(rio_t *rp, char *request_headers);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
int parse_uri(char *uri, char *hostname, char *port, char *filename);
void serve(int fd, char *hostname, char *port, char *filename, char *request_headers);

int checkcache(char *hostname, char *port, char *filename)
{
    int i;

    pthread_rwlock_rdlock(&lock);
    for(i=0;i<numcache;i++)
    {
        if(!strncmp(cache[i]->hostname, hostname, strlen(hostname)+1))
            if(!strncmp(cache[i]->port, port, strlen(port)+1))
                if(!strncmp(cache[i]->filename, filename, strlen(filename)+1))
                {
                    pthread_rwlock_unlock(&lock);
                    return i;
                }
    }
    pthread_rwlock_unlock(&lock);
    return -1;
}

void sendcache(int fd, int i)
{
    pthread_rwlock_wrlock(&lock);
    cache[i]->time = cachetime++;
    Rio_writen(fd, cache[i]->content, cache[i]->size);
    pthread_rwlock_unlock(&lock);
}

void makespace(int size)
{
    int i;
    int index;
    int min = 2147483642;

    while(sizeleft < size)
    {
        for(i=0;i<numcache;i++)
        {
            if(cache[i]->time < min)
            {
                min = cache[i]->time;
                index = i;
            }
        }
        free(cache[index]->hostname);
        free(cache[index]->port);
        free(cache[index]->filename);
        free(cache[index]->content);
        free(cache[index]);

        cache[index] = cache[--numcache];

        sizeleft += cache[i]->size;
    }
}

void putcache(char *response, int size, char *hostname, char *port, char *filename)
{
    int index = numcache++;

    pthread_rwlock_wrlock(&lock);

    if(sizeleft < size)
        makespace(size);

    cache[index] = Malloc(sizeof(struct cacheinfo));

    cache[index]->hostname = Malloc(strlen(hostname)+1);
    strncpy(cache[index]->hostname, hostname, strlen(hostname)+1);
    cache[index]->port = Malloc(strlen(port)+1);
    strncpy(cache[index]->port, port, strlen(port)+1);
    cache[index]->filename = Malloc(strlen(filename)+1);
    strncpy(cache[index]->filename, filename, strlen(filename)+1);

    cache[index]->content = Malloc(size);
    memcpy(cache[index]->content, response, size);

    cache[index]->size = size;

    cache[index]->time = cachetime++;
    sizeleft -= size;

    pthread_rwlock_unlock(&lock);
}

void *thread(void *vargp)
{
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;  
}

int main(int argc, char **argv)
{
    int listenfd, *connfdp;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    int min;
    int index;
    int i, j;

    if(argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);
    pthread_rwlock_init(&lock, NULL);

    listenfd = Open_listenfd(argv[1]);
    while(1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen); //line:netp:proxy:accept
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        Pthread_create(&tid, NULL, thread, connfdp);
        
        //time reset
        if(cachetime > 2147483600)
        {
            pthread_rwlock_wrlock(&lock);

            for(i=0;i<numcache;i++)
            {
                min = 2147483640;

                for(j=0;j<numcache;j++)
                {
                    if(cache[j]->time < min && cache[j]->time >= i)
                    {
                        min = cache[j]->time;
                        index = j;
                    }
                }
                cache[index]->time = i;
            }
            cachetime = numcache;

            pthread_rwlock_unlock(&lock);
        }
    }

    return 0;
}

void doit(int fd){
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], port[MAXLINE], filename[MAXLINE];
    char request_headers[MAX_HEADER_SIZE];
    rio_t rio;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if(Rio_readlineb(&rio, buf, MAXLINE) <= 0) //line:netp:doit:readrequest
        return;
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version); //line:netp:doit:parserequest

    if(strcasecmp(method, "GET")) { 
        clienterror(fd, method, "501", "Not Implemented",
                "Proxy does not implement this method");
        return;
    }

    if(read_requesthdrs(&rio, request_headers) < 0)
        return;

    /* Parse URI from GET request */
    if(!parse_uri(uri, hostname, port, filename))
        serve(fd, hostname, port, filename, request_headers);
}

/*
 * read_requesthdrs - read HTTP request headers
 */
/* $begin read_requesthdrs */
int read_requesthdrs(rio_t *rp, char *request_headers) 
{
    char buf[MAXLINE];

    if(Rio_readlineb(rp, buf, MAXLINE) < 0)
        return -1;
    printf("%s", buf);

    strncpy(request_headers, buf, strlen(buf)+1);

    while(strncmp(buf, "\r\n", strlen("\r\n")+1))
    {
        if(strstr(buf, "Host:"))
            ifhosthdr = 1;
        if(Rio_readlineb(rp, buf, MAXLINE) < 0)
            return -1;
        printf("%s", buf);
        strcat(request_headers, buf);
    }
    return 0;
}
/* $end read_requesthdrs */

/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Proxy Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    if(Rio_writen(fd, buf, strlen(buf)) < 0)
        return;
    sprintf(buf, "Content-type: text/html\r\n");
    if(Rio_writen(fd, buf, strlen(buf)) < 0)
        return;
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    if(Rio_writen(fd, buf, strlen(buf)) < 0)
        return;
    if(Rio_writen(fd, body, strlen(body)) < 0)
        return;
}
/* $end clienterror */

/*
 * parse_uri - parse URI into filename and CGI args
 *             return 0 if dynamic content, 1 if static
 */
/* $begin parse_uri */
int parse_uri(char *uri, char *hostname, char *port, char *filename) 
{
    char *ptr1, *ptr2;
    char *hoststart;

    if(!strncmp(uri, "http://localhost", strlen("http://localhost")))
    {
        ptr1 = strchr(uri+strlen("http://"), ':');

        //if port specified
        if(ptr1)
        {
            if(ptr1[1] == '\0')
                return -1;

            ptr2 = strchr(ptr1, '/');

            //filepath exists
            if(ptr2)
            {
                strncpy(filename, ptr2, strlen(ptr2)+1);
                ptr2[0] = '\0';
                strncpy(port, ptr1+1, strlen(ptr1+1)+1);
            }
            else
            {
                strncpy(port, ptr1+1, strlen(ptr1+1)+1);
                strncpy(filename, "/",strlen("/")+1);
            }
        }
        else
        {
            strncpy(port, "80", strlen("80")+1);
            ptr2 = strchr(uri+strlen("http://"), '/');

            //filepath exists
            if(ptr2)
            {
                strncpy(filename, ptr2, strlen(ptr2)+1);
                ptr2[0] = '\0';
            }
            else
                strncpy(filename, "/", strlen("/")+1);
        }
        strncpy(hostname, "localhost", strlen("localhost")+1);
        return 0;
    }
    if(!strncmp(uri, "http://", strlen("http://")))
    {
        hoststart = strstr(uri, "http://") + strlen("http://");
        ptr1 = strchr(uri+strlen("http://"), ':');

        if(ptr1)
        {
            if(ptr1[1] == '\0')
                return -1;

            ptr2 = strchr(ptr1, '/');

            if(ptr2)
            {
                strncpy(filename, ptr2, strlen(ptr2)+1);
                ptr2[0] = '\0';
                strncpy(port, ptr1+1, strlen(ptr1+1)+1);
            }
            else
            {
                strncpy(port, ptr1+1, strlen(ptr1+1)+1);
                strncpy(filename, "/",strlen("/")+1);
            }

            ptr1[0] = '\0';
            strncpy(hostname, hoststart, strlen(hoststart)+1);
        }
        else
        {
            strncpy(port, "80", strlen("80")+1);
            ptr2 = strchr(hoststart, '/');

            if(ptr2)
            {
                strncpy(filename, ptr2, strlen(ptr2)+1);
                ptr2[0] = '\0';
                strncpy(hostname, hoststart, strlen(hoststart)+1); 
            }
            else
            {
                strncpy(hostname, hoststart, strlen(hoststart)+1);
                strncpy(filename, "/", strlen("/")+1);
            }
        }
        printf("hostname : %s, port : %s, filename : %s\n", hostname, port, filename);
        return 0;
    }
    else 
        return -1;
}
/* $end parse_uri */

/*
 * serve - get a response and copy it back to the client 
 */
/* $begin serve */
void serve(int fd, char *hostname, char *port, char *filename, char *request_headers) 
{
    int clientfd;
    char buf[MAXBUF];
    char request[MAX_REQUEST_SIZE];
    char response[MAX_RESPONSE_SIZE];
    rio_t rio;
    int content_length=-1;
    int headersize;
    int index;
    int checktype = 0;
    int n = 1;

    if( (index = checkcache(hostname, port, filename)) != -1)
    {
        sendcache(fd, index);
        return;
    }
    
    response[0] = '\0';

    sprintf(request, "GET %s HTTP/1.0\r\n", filename);
    if(!ifhosthdr)
        sprintf(request, "%sHost: %s\r\n", request, hostname);
    sprintf(request, "%s%s", request, user_agent_hdr);
    sprintf(request, "%s%s", request, connection_hdr);
    sprintf(request, "%s%s", request, proxy_connection_hdr);
    sprintf(request, "%s%s\r\n", request, request_headers);

    clientfd = Open_clientfd(hostname, port);

    if(clientfd < 0)
        return;

    Rio_readinitb(&rio, clientfd);
    
    if(Rio_writen(clientfd, request, strlen(request)) < 0)
        return;
    
    if(Rio_readlineb(&rio, buf, MAXLINE) < 0)
        return;
    strcat(response, buf);

    while(strncmp(buf, "\r\n", strlen("\r\n")+1))
    {
        if(strstr(buf, "Content-Type:") || strstr(buf, "Content-type:"))
            checktype = 1;
        if(strstr(buf, "Content-Length:"))
            content_length = atoi(strstr(buf, "Content-Length:") + strlen("Content-Length:"));
        else if(strstr(buf, "Content-length:"))
            content_length = atoi(strstr(buf, "Content-length:") + strlen("Content-length:"));
        if(Rio_readlineb(&rio, buf, MAXLINE) < 0)
            return;
        strcat(response, buf);
    }

    if(content_length > MAXBUF)
    {
        clienterror(fd, buf, "413", "Payload Too Large",
            "Binary file too big");
        return;
    }

    if(!checktype && content_length == -1)
        content_length = 0;
    
    //a possible binary file
    if (content_length != -1)
    {
        if(Rio_readnb(&rio, buf, content_length+2) < 0)
            return;

        headersize = strlen(response);

        memcpy(response+headersize, buf, content_length+2);

        if(Rio_writen(fd, response, headersize+content_length+2) < 0)
            return;
        
        if(headersize+content_length+2 <= MAX_OBJECT_SIZE)
            putcache(response, headersize+content_length+2, hostname, port, filename);
        return;
    }

    while(n > 0)
    {
        if((n = Rio_readlineb(&rio, buf, MAXLINE)) < 0)
            return;
        if(n == 0)
            break;
        strcat(response, buf);
    }

    if(Rio_writen(fd, response, strlen(response)) < 0)
        return;
    if(strlen(response) <= MAX_OBJECT_SIZE)
        putcache(response, strlen(response), hostname, port, filename);
}
/* $end serve */
