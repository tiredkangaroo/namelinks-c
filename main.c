#include "hiredis/read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <unistd.h>
#include <hiredis/hiredis.h>

// BUFFER_SIZE specifies the size of the HTTP buffer to
// intake in order to get the path from the request.
#define BUFFER_SIZE 160

// NAME_SIZE specifies the size of a name for the named
// URL.
#define NAME_SIZE 24

#define LONGURL_SIZE = 128

// DEFAULT_NAMEDURLS_SIZE specifies the number of named
// URLs to allocate memory for by default.
#define DEFAULT_NAMEDURLS_SIZE 16

// REDIRECT_RESPONSE_SIZE specifies the size for a redirect
// HTTP response. Since the max size for a long URL is 128,
// the REDIRECT_RESPONSE_SIZE is 232 bytes at the maximum.
#define REDIRECT_RESPONSE_SIZE 256

// LIST_RESPONSE_SIZE specifies the size for a response to
// /list. Rough calculations suggest the size specified.
#define LIST_RESPONSE_SIZE (reply -> elements * 256) + 512

// NamedURL represents a named URL, which is used to redirect
// a request.
struct NamedURL {
    char* name;
    char* url;
};

// NOT_FOUND is an HTTP response that represents a response
// returned when a name doesn't exist.
const char* NOT_FOUND =  "HTTP/1.1 404 Not Found \r\n"
                    "Content-Type: text/html\r\n"
                    "Connection: close\r\n"
                    "Content-Length: 79\r\n"
                    "\r\n"
                    "<h1> Not Found </h1>"
                    "<pre>There is no registry for the name your provided.</pre>";

// createRedirectResponse returns a redirect response that
// redirects the browser to the link specified in the
// parameter.
char* createRedirectResponse(char * to) {
    char* location = malloc(REDIRECT_RESPONSE_SIZE);
    if (location == NULL) {
        return NULL;
    }
    sprintf(location,
        "HTTP/1.1 308 Permanent Redirect \r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: 0\r\n"
        "Location: %s\r\n"
        "\r\n",
    to);
    return location;
}

// getPath extracts the path of an HTTP request from
// the connection file descriptor.
char* getPath(int cfd) {
    char* buffer = malloc(BUFFER_SIZE);
    char* path = malloc(NAME_SIZE);

    if (buffer == NULL || path == NULL) {
        perror("memory for getPath was not able to be allocated");
        free(buffer);
        free(path);
        return NULL;
    }

    ssize_t bytes_read = read(cfd, buffer, BUFFER_SIZE);
    if (bytes_read <= 0) {
        free(buffer);
        free(path);
        return NULL;
    }

    if (sscanf(buffer, "GET %s HTTP/", path) == 1) {
        free(buffer);
        return path;
    }

    free(buffer);
    free(path);
    return NULL;
}

// split writes multiple strings to buf from s seperated
// by the delimiter.
int split(char* buf, char* s, char delim) {
    int s_len = strlen(s);
    int s_pos = 0;

    int element_start = 0;
    int buf_pos = 0;

    int num_elements = 0;

    while (s_pos <= s_len) {
        if (s[s_pos] == delim || s[s_pos] == '\0') {
            int element_len = s_pos - element_start;
            if (element_len > 0) {
                memcpy(&buf[buf_pos], &s[element_start], element_len);
                buf[buf_pos + element_len] = '\0';

                element_start = s_pos + 1;
                buf_pos += element_len + 1;
                num_elements++;
            }
        }
        s_pos++;
    }

    return num_elements;
}

// getLongURL gets the long url from a named url from path.
char* getLongURL(redisContext* redisctx, char* path) {
    char cmd[strlen(path) + 24];
    snprintf(cmd, strlen(path) + 24, "HGET namelinks %s", &path[1]); // no need for snprintf here

    redisReply *reply = redisCommand(redisctx, cmd);
    if (reply -> type != REDIS_REPLY_STRING) {
        if (reply -> type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "redis getLongURL error: %s", reply -> str);
        }
        freeReplyObject(reply);
        return NULL;
    }

    char *longURL = malloc(reply -> len);
    strncpy(longURL, reply -> str, reply -> len);
    freeReplyObject(reply);
    return longURL;
}

// createListResponse generates a response of named URLs.
char* createListResponse(redisContext *redisctx) {
    redisReply *reply = redisCommand(redisctx, "HGETALL namelinks");
    if (reply -> type != REDIS_REPLY_ARRAY) {
        if (reply -> type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "redis createListResponse error: %s", reply->str);
        }
        freeReplyObject(reply);
        return NULL;
    }
    char body[reply -> elements * 256];
    char* response = malloc(reply -> elements * 256 + 256);
    if (response == NULL) {
        fprintf(stderr, "allocations for createListResponse failed\n");
        free(response);
        return NULL;
    }
    int body_pos = 0;

    size_t i = 0;
    while (i < reply -> elements) {
        char* key = reply -> element[i] -> str;
        char* value = reply -> element[i + 1] -> str;
        body_pos += snprintf(&body[body_pos], 256, "<li><a href='%s'>%s</a></li>", value, key);
        i += 2;
    }

    snprintf(response, reply -> elements * 256 + 256,
                           "HTTP/1.1 200 OK\r\n"
                           "Cache-Control: no-store\r\n"
                           "Content-Type: text/html\r\n"
                           "Content-Length: %lu\r\n"
                           "\r\n"
                           "%s",
                           strlen(body), body);

    return response;
}


int main() {
    // local address (returned after a bind)
    struct sockaddr_in my_addr;
    // peer address (returned after an accept)
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_size = sizeof(peer_addr);

    redisContext *redisctx = redisConnect("127.0.0.1", 6379);
    if (redisctx -> err) {
        fprintf(stderr, "connecting to redis resulted in an error: %s", redisctx->errstr);
        exit(EXIT_FAILURE);
    }

    // sfd represents a file descriptor in which will be used
    // for bind and accept operations
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        perror("making a file descriptor for socket returned -1");
        exit(EXIT_FAILURE);
    }

    // add some info
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(8000);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);


    // bind
    if (bind(sfd, (struct sockaddr *)&my_addr, sizeof(my_addr)) == -1) {
        perror("binding failed");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    if (listen(sfd, 32) == -1) {
        perror("listening failed");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        // cfd represents the file descriptor for the connection.
        int cfd = accept(sfd, (struct sockaddr *)&peer_addr, &peer_addr_size);
        if (cfd == -1) {
            perror("connection with a peer failed");
        }

        char* path = getPath(cfd);
        if (path == NULL) { // invalid path
            close(cfd);
            continue;
        }

        if (strcmp(path, "/list") == 0) {
            char *response = createListResponse(redisctx);
            if (response != NULL) {
                if (write(cfd, response, strlen(response)) == -1) {
                    perror("write to a peer connection for list response failed");
                }
                free(response);
            }
            free(path);
            shutdown(cfd, SHUT_WR);
            close(cfd);
            continue;
        }

        char* longurl = getLongURL(redisctx, path);
        if (longurl == NULL) {
            if (write(cfd, NOT_FOUND, strlen(NOT_FOUND)) == -1) {
                perror("write to a peer connection for not found failed");
            }
            free(path);
            shutdown(cfd, SHUT_WR);
            close(cfd);
            continue;
        }


        char *response = createRedirectResponse(longurl);
        free(longurl);
        if (response != NULL) {
            if (write(cfd, response, strlen(response)) == -1) {
                perror("write to a peer connection for redirect response failed");
            }
            free(response);
            free(path);
            shutdown(cfd, SHUT_WR);
            close(cfd);
            continue;
        }

        // clean up after connection
        free(path);
        shutdown(cfd, SHUT_WR);
        close(cfd);
    }

    // unreachable code
    close(sfd);
}
