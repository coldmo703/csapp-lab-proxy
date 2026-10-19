#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define SBUFSIZE 32
#define NTHREADS 16

typedef struct {
    int *buf;
    int n;
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} sbuf_t;

typedef struct block {
    char *object;
    char *tag;
    size_t size;
    unsigned long timestamp; /* 마지막 접근 시점 (LRU 추적용) */
    struct block *next;
} block;

typedef struct {
    block *start;
    block *top;
    size_t total_size;
    int readcnt;
    sem_t mutex;
    sem_t w;
    unsigned long lru_counter; /* 접근 시마다 1씩 증가하는 전역 타임스탬프 */
} cache;

void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);

void cache_init(cache *c);
void cache_deinit(cache *c);
int cache_search(cache *c, char *url, char *dest, size_t *size);
void cache_modify(cache *c, char *data, size_t size, char *url);
block *create_block(char *data, size_t size, char *url, unsigned long timestamp);

void doit(int client_fd);
int read_requestline(rio_t *rp, int fd, char *hostname, char *port, char *path, char *url);
void build_requesthdrs(rio_t *client_rp, char *server_http_header, char *hostname, char *port, char *path);
void *thread(void *vargp);

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
sbuf_t sbuf;
cache c;

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);

    sbuf_init(&sbuf, SBUFSIZE);
    for (int i = 0; i < NTHREADS; i++) {
        Pthread_create(&tid, NULL, thread, NULL);
    }

    cache_init(&c);

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        sbuf_insert(&sbuf, connfd);
    }
}

void doit(int client_fd) {
    int server_fd;
    rio_t client_rio, server_rio;
    char hostname[MAXLINE], port[MAXLINE], path[MAXLINE], url[MAXLINE];
    char server_http_header[MAXLINE];
    char server_response[MAXLINE];
    
    /* 단일 객체 최대 크기를 담을 수 있는 누적 버퍼 */
    char cache_buf[MAX_OBJECT_SIZE];
    size_t total_object_size = 0;
    size_t cached_size = 0;
    ssize_t n;

    Rio_readinitb(&client_rio, client_fd);

    if (!read_requestline(&client_rio, client_fd, hostname, port, path, url)) {
        return;
    }
    build_requesthdrs(&client_rio, server_http_header, hostname, port, path);

    /* 1. 캐시 검색 (Key는 URI) */
    if (cache_search(&c, url, cache_buf, &cached_size)) {
        Rio_writen(client_fd, cache_buf, cached_size);
        return;
    }

    /* 2. 캐시 미스 시 엔드 서버 통신 */
    server_fd = open_clientfd(hostname, port);
    if (server_fd < 0) {
        printf("Connection to end server failed.\n");
        return;
    }
    Rio_writen(server_fd, server_http_header, strlen(server_http_header));

    Rio_readinitb(&server_rio, server_fd);
    while ((n = Rio_readnb(&server_rio, server_response, MAXLINE)) > 0) {
        Rio_writen(client_fd, server_response, n);

        /* 단일 객체 제한을 넘지 않는 선에서 누적 버퍼에 저장 */
        if (total_object_size + n <= MAX_OBJECT_SIZE) {
            memcpy(cache_buf + total_object_size, server_response, n);
        }
        total_object_size += n;
    }
    Close(server_fd);

    /* 3. 단일 객체 최대 크기 이내인 경우에만 캐시에 등록 */
    if (total_object_size <= MAX_OBJECT_SIZE) {
        cache_modify(&c, cache_buf, total_object_size, url);
    }
}

int read_requestline(rio_t *rp, int fd, char *hostname, char *port, char *path, char *url) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];

    if (!Rio_readlineb(rp, buf, MAXLINE)) {
        return 0;
    }
    printf("Request headers:\n%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET") != 0) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Proxy does not implement this method");
        return 0;
    }

    /* 캐시 태그로 사용할 원본 URI 복사 */
    strcpy(url, uri);
    strcpy(port, "80");

    char *host_ptr;
    if ((host_ptr = strstr(uri, "//")) != NULL) {
        host_ptr += 2;
    } else {
        host_ptr = uri;
    }

    char *path_ptr = strchr(host_ptr, '/');
    if (path_ptr != NULL) {
        strcpy(path, path_ptr);
        *path_ptr = '\0';
    } else {
        strcpy(path, "/");
    }

    char *port_ptr = strchr(host_ptr, ':');
    if (port_ptr != NULL) {
        *port_ptr = '\0';
        strcpy(port, port_ptr + 1);
    }
    strcpy(hostname, host_ptr);

    return 1;
}

void build_requesthdrs(rio_t *client_rp, char *server_http_header, char *hostname, char *port, char *path) {
    char buf[MAXLINE];
    char other_hdrs[MAXLINE] = "";
    char host_hdr[MAXLINE] = "";

    while (Rio_readlineb(client_rp, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) {
            break;
        }

        if (strncasecmp(buf, "Host:", 5) == 0) {
            strcpy(host_hdr, buf);
            continue;
        }
        if (strncasecmp(buf, "User-Agent:", 11) == 0 ||
            strncasecmp(buf, "Connection:", 11) == 0 ||
            strncasecmp(buf, "Proxy-Connection:", 17) == 0) {
            continue;
        }
        strcat(other_hdrs, buf);
    }
    if (strlen(host_hdr) == 0) {
        sprintf(host_hdr, "Host: %s\r\n", hostname);
    }
    sprintf(server_http_header, "GET %s HTTP/1.0\r\n", path);
    strcat(server_http_header, host_hdr);
    strcat(server_http_header, user_agent_hdr);
    strcat(server_http_header, "Connection: close\r\n");
    strcat(server_http_header, "Proxy-Connection: close\r\n");
    strcat(server_http_header, other_hdrs);
    strcat(server_http_header, "\r\n");
}

void *thread(void *vargp) {
    Pthread_detach(pthread_self());
    while (1) {
        int clientfd = sbuf_remove(&sbuf);
        doit(clientfd);
        Close(clientfd);
    }
}

/* --- Cache Functions --- */

void cache_init(cache *c) {
    c->total_size = 0;
    c->readcnt = 0;
    c->start = Calloc(1, sizeof(block)); /* 더미 헤드 노드 */
    c->start->next = NULL;
    c->top = c->start;
    Sem_init(&c->mutex, 0, 1);
    Sem_init(&c->w, 0, 1);
}

void cache_deinit(cache *c) {
    block *curr = c->start;
    while (curr != NULL) {
        block *next = curr->next;
        if (curr->object) Free(curr->object);
        if (curr->tag) Free(curr->tag);
        Free(curr);
        curr = next;
    }
}

int cache_search(cache *c, char *url, char *dest, size_t *size) {
    block *b;
    int is_found = 0;

    /* 1. Reader 진입 */
    P(&c->mutex);
    c->readcnt++;
    if (c->readcnt == 1) {
        P(&c->w);
    }
    V(&c->mutex);

    /* 2. 동시 읽기 및 적중 시 타임스탬프 갱신 */
    for (b = c->start->next; b != NULL; b = b->next) {
        if (strcmp(b->tag, url) == 0) {
            memcpy(dest, b->object, b->size);
            *size = b->size;
            is_found = 1;

            /* Hit된 객체의 접근 시간 갱신 */
            P(&c->mutex);
            b->timestamp = ++(c->lru_counter);
            V(&c->mutex);
            break;
        }
    }

    /* 3. Reader 퇴장 */
    P(&c->mutex);
    c->readcnt--;
    if (c->readcnt == 0) {
        V(&c->w);
    }
    V(&c->mutex);

    return is_found;
}

block *create_block(char *data, size_t size, char *url, unsigned long timestamp) {
    block *b = (block *)Malloc(sizeof(block));
    
    b->object = (char *)Malloc(size);
    memcpy(b->object, data, size);
    
    b->tag = (char *)Malloc(strlen(url) + 1);
    strcpy(b->tag, url);
    
    b->size = size;
    b->timestamp = timestamp;
    b->next = NULL;
    return b;
}

void cache_modify(cache *c, char *data, size_t size, char *url) {
    if (size > MAX_OBJECT_SIZE) {
        return;
    }

    P(&c->w);

    /* 용량 초과 시 LRU 노드를 찾아 축출 */
    while (c->total_size + size > MAX_CACHE_SIZE && c->start->next != NULL) {
        block *prev = c->start;
        block *curr = c->start->next;
        
        block *victim_prev = prev;
        block *victim = curr;
        unsigned long min_timestamp = curr->timestamp;

        /* 가장 작은 timestamp(가장 오래 전에 접근된 블록) 탐색 */
        while (curr != NULL) {
            if (curr->timestamp < min_timestamp) {
                min_timestamp = curr->timestamp;
                victim = curr;
                victim_prev = prev;
            }
            prev = curr;
            curr = curr->next;
        }

        /* LRU victim 노드 연결 해제 및 메모리 반환 */
        victim_prev->next = victim->next;
        if (c->top == victim) {
            c->top = victim_prev;
        }
        c->total_size -= victim->size;

        Free(victim->object);
        Free(victim->tag);
        Free(victim);
    }

    /* 새 블록 생성 및 추가 */
    block *b = create_block(data, size, url, ++(c->lru_counter));
    c->top->next = b;
    c->top = b;
    c->total_size += size;

    V(&c->w);
}

/* --- sbuf Functions --- */

void sbuf_init(sbuf_t *sp, int n) {
    sp->buf = Calloc(n, sizeof(int));
    sp->n = n;
    sp->front = sp->rear = 0;
    Sem_init(&sp->mutex, 0, 1);
    Sem_init(&sp->slots, 0, n);
    Sem_init(&sp->items, 0, 0);
}

void sbuf_deinit(sbuf_t *sp) {
    Free(sp->buf);
}

void sbuf_insert(sbuf_t *sp, int item) {
    P(&sp->slots);
    P(&sp->mutex);
    sp->buf[(++sp->rear) % (sp->n)] = item;
    V(&sp->mutex);
    V(&sp->items);
}

int sbuf_remove(sbuf_t *sp) {
    int item;
    P(&sp->items);
    P(&sp->mutex);
    item = sp->buf[(++sp->front) % (sp->n)];
    V(&sp->mutex);
    V(&sp->slots);
    return item;
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    /* HTTP response body 생성 */
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body, "%s<body bgcolor=\"ffffff\">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Proxy Server</em>\r\n", body);

    /* HTTP response 헤더 및 바디 전송 */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}