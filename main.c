#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <unistd.h>

#define BUFFER_SIZE 120
#define PATH_SIZE 24

#define DEFAULT_NAMEDURLS_SIZE 128

// REDIRECT_RESPONSE_SIZE specifies the size for a redirect
// HTTP response. Since the max size for a long URL is 128,
// the REDIRECT_RESPONSE_SIZE is 232 bytes at the maximum.
#define REDIRECT_RESPONSE_SIZE 256

// LIST_RESPONSE_SIZE specifies the size for a response to
// /list. Rough calculations suggest the size specified.
#define LIST_RESPONSE_SIZE (namedURLS_size * 256) + 512

struct NamedURL {
    char* name;
    char* url;
};

const char* NOT_FOUND =  "HTTP/1.1 404 Not Found \r\n"
                    "Content-Type: text/html\r\n"
                    "Connection: close\r\n"
                    "Content-Length: 79\r\n"
                    "\r\n"
                    "<h1> Not Found </h1>"
                    "<pre>There is no registry for the name your provided.</pre>";

char* REDIRECT_RESPONSE(char * to) {
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
    char* path = malloc(PATH_SIZE);
    if (!buffer || !path) {
        perror("Failed to allocate memory for getPath");
        exit(EXIT_FAILURE);
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


char* getLongURL(void *namedURLS, int namedURLS_size, char* path) {
    struct NamedURL namedURL;
    int i = 0;
    char *name = path;
    while (i < namedURLS_size) {
        int start = i * sizeof(struct NamedURL);
        memcpy((void *)(&namedURL), &namedURLS[start], sizeof(struct NamedURL));
        if (strcmp(namedURL.name, path) == 0) {
            // add logic for adding the extra parts here
            return namedURL.url;
        }
        i += 1;
    }
    return NULL;
}

char* LIST_RESPONSE(void *namedURLS, int namedURLS_size){
    char* response = malloc(LIST_RESPONSE_SIZE);
    char* body = malloc(namedURLS_size * 256 + 256);

    struct NamedURL namedURL;
    int i = 0;
    while (i < namedURLS_size) {
        int start = i * sizeof(struct NamedURL);
        memcpy((void *)&namedURL, &namedURLS[start], sizeof(struct NamedURL));
        sprintf(&body[i * 256], "<li><a href='%s'>%s</a></li>", namedURL.url, namedURL.name); // max size 177 bytes
        i += 1;
    }

    sprintf(response,
        "HTTP/1.1 200 OK \r\n"
        "Cache-Control: no-store\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %lu\r\n"
        "\r\n"
        "%s",
        strlen(body), body);

    free(body);
    return response;
}

int main() {
    void *namedURLS = malloc(sizeof(struct NamedURL) * DEFAULT_NAMEDURLS_SIZE);
    int namedURLS_size = 0;

    // example for testing:
    struct NamedURL helloWorld;
    helloWorld.name = "/yt";
    helloWorld.url = "https://youtube.com";
    memcpy(namedURLS, &helloWorld, sizeof(struct NamedURL));
    namedURLS_size += 1;


    // local address (returned after a bind)
    struct sockaddr_in my_addr;
    // peer address (returned after an accept)
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_size = sizeof(peer_addr);

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
            char *response = LIST_RESPONSE(namedURLS, namedURLS_size);
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

        char* longurl = getLongURL(namedURLS, namedURLS_size, path);

        if (longurl == NULL) {
            if (write(cfd, NOT_FOUND, strlen(NOT_FOUND)) == -1) {
                perror("write to a peer connection for not found failed");
            }
            free(path);
            shutdown(cfd, SHUT_WR);
            close(cfd);
            continue;
        }


        char *response = REDIRECT_RESPONSE(longurl);
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
}
