#include "csapp.h"
#include "sbuf.h"

#define SBUFSIZE  4
#define INIT_THREAD_N  1
#define THREAD_LIMIT 4096

static int nthreads;
static sbuf_t sbuf;

// Thread struct
typedef struct {
    pthread_t tid;
    sem_t mutex;
} ithread;
static ithread thread_list[THREAD_LIMIT];

void create_thread(int start, int end);
void *serve_thread(void *vargp);
void *adjust(void *);

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char * filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

//int main(int argc, char **argv) {
//    int listenfd, connfd;
//    char hostname[MAXLINE], port[MAXLINE];
//    socklen_t clientlen;
//    struct sockaddr_storage clientaddr;
//
//    /* Check command-line args */
//    if (argc != 2) {
//        fprintf(stderr, "usage: %s <port>\n", argv[0]);
//        exit(1);
//    }
//
//    int argv_port = atoi(argv[1]);
//    listenfd = Open_listenfd(argv_port);
//
//    while (1) {
//        clientlen = sizeof(clientaddr);
//        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
//        getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
//        printf("Accepted onnection from (%s, %s)\n", hostname, port);
//
//        doit(connfd);
//        Close(connfd);
//    }
//
//    return 0;
//}

int main(int argc, char **argv) {
    int i, listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        fprintf(stderr, "use default port 8080\n");
        listenfd = Open_listenfd(8080);
    } else {
        listenfd = Open_listenfd(atoi(argv[1]));
    }

    nthreads = INIT_THREAD_N;
    sbuf_init(&sbuf, SBUFSIZE);

    create_thread(0, nthreads);

    Pthread_create(&tid, NULL, adjust, NULL);

    while (true) {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = Accept(listenfd, (SA *) &clientaddr, &clientlen);
        sbuf_insert(&sbuf, connfd);
    }
}

void create_thread(int start, int end) {
    int i;
    for (i = start; i < end; i++) {
        Sem_init(&(thread_list[i].mutex), 0, 1);
        int *arg = (int*)Malloc(sizeof(int));
        *arg = i;
        Pthread_create(&(thread_list[i].tid), NULL, serve_thread, arg);
    }
}

void *adjust(void *vargp) {
    sbuf_t *sp = &sbuf;

    while (true) {
        if (sbuf_full(sp)) {
            if (THREAD_LIMIT == nthreads) {
                fprintf(stderr, "Too many thread!\n");
                continue;
            }

            int dn = 2 * nthreads;
            create_thread(nthreads, dn);
            nthreads = dn;
        }
        else if (sbuf_empty(sp)) {
            if (1 == nthreads)
                continue;
            int hn = nthreads / 2;
            int i;
            for (i = hn; i < nthreads; i++) {
                P(&(thread_list[i].mutex));
                Pthread_cancel(thread_list[i].tid);
                V(&(thread_list[i].mutex));
            }
            nthreads = hn;
        }
    }
}

void *serve_thread(void *vargp) {
    int idx = *(int*)vargp;
    Free(vargp);

    while (true) {
        P(&(thread_list[idx].mutex));
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);
        V(&(thread_list[idx].mutex));
    }
}

void doit(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;
    printf("Request headers:\n");
    printf("%s", buf);
    printf("%s %s %s\n", method, uri, version);
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }
    read_requesthdrs(&rio);

    /* Paese URI from GET request */
    is_static = parse_uri(uri, filename, cgiargs);
    printf("is_static: %d\n", is_static);
    printf("stat: %d\n", stat(filename, &sbuf));
    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
        return;
    }

    if (is_static) {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, sbuf.st_size);
    }
    else {
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, cgiargs);
    }
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor='ffffff'>\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

void read_requesthdrs(rio_t *rp) {
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    while(strcmp(buf, "\r\n")) {
        rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
    return;
}

int parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {
        strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        if (uri[strlen(uri)-1] == '/')
            strcat(filename, "home.html");
        return 1;
    }
    else {
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr+1);
            *ptr = '\0';
        }
        else
            strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        return 0;
    }
}

void serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response headers to client */
    get_filetype(filename, filetype);
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));
    printf("Response headers:\n");
    printf("%s", buf);

    /* Send response body to client */
    srcfd = Open(filename, O_RDONLY, 0);
    srcp = (char *)Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    Close(srcfd);
    Rio_writen(fd, srcp, strlen(srcp));
    Munmap(srcp, strlen(srcp));
}

void get_filetype(char * filename, char *filetype) {
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".mpg") || strstr(filename, ".mp4"))
        strcpy(filetype, "video/mpg");
    else
        strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs) {
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) {
        setenv("QUERY_STRING", cgiargs, 1);
        Dup2(fd, STDOUT_FILENO);
        Execve(filename, emptylist, environ);
    }
    Wait(NULL);
}
