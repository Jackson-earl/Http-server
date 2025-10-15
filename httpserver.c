#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <regex.h>

#include "asgn2_helper_funcs.h"

#define MAX_BUFFER_SIZE 4096

typedef struct {
    char method[9];
    char uri[65];
    long content_length;
    char *body;
    char version[16];
} HttpRequest;

void send_error_response(int client_sock, int status_code, const char *status_phrase) {

    //error message body
    char body[128];
    int body_len = snprintf(body, sizeof(body), "%s\n", status_phrase);

    //HTTP error  responses
    //Header status line
    char response[256];
    int response_len;
    if (status_code == 400 || status_code == 403 || status_code == 404 || status_code == 200
        || status_code == 201 || status_code == 505 || status_code == 501) {
        response_len = snprintf(response, sizeof(response),
            "HTTP/1.1 %d %s\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            status_code, status_phrase, body_len, body);
    } else {
        response_len = snprintf(
            response, sizeof(response), "HTTP/1.1 %d %s\r\n", status_code, status_phrase);
    }

    if (write(client_sock, response, response_len) < 0) {
        printf("didn't fully write\n");
        perror("Failed to send error response");
    }
    close(client_sock);
}

void handle_get(int client_sock, const char *uri) {
    struct stat st;
    if (uri[0] != '/') {
        send_error_response(client_sock, 400, "Bad Request");
        return;
    }
    const char *file_name = uri + 1;

    //check if file exists
    if (stat(file_name, &st) != 0) {
        send_error_response(client_sock, 404, "Not Found");
        return;
    }

    //check if the file is a directory
    if (S_ISDIR(st.st_mode) || access(file_name, R_OK) != 0) {
        send_error_response(client_sock, 403, "Forbidden");
        return;
    }

    int fd = open(file_name, O_RDONLY);
    if (fd == -1) {
        send_error_response(client_sock, 500, "Internal Server Error");
        return;
    }

    //Prepare  to send HTTP headers for a 200 OK response
    char header[256];
    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %lld\r\n"
        "\r\n",
        (long long) st.st_size);

    if (write(client_sock, header, header_length) < 0) {
        perror("Failed to send HTTP header");
        close(fd);
        return;
    }

    char buffer[MAX_BUFFER_SIZE];
    ssize_t bytesRead;
    while ((bytesRead = read(fd, buffer, MAX_BUFFER_SIZE)) > 0) {
        ssize_t bytesWritten = 0;
        while (bytesWritten < bytesRead) {
            ssize_t result = write(client_sock, buffer + bytesWritten, bytesRead - bytesWritten);
            if (result == -1) {
                perror("Failed to send file content");
                close(fd);
                return;
            }
            bytesWritten += result;
        }
    }
    if (bytesRead == -1) {
        perror("Error reading file");
    }
    close(fd);
}

void handle_put(int client_sock, const char *uri, long content_length, char *body) {
    if (uri[0] != '/') {
        send_error_response(client_sock, 400, "Bad Request");
        return;
    }
    const char *file_name = uri + 1;
    int file_exists = (access(file_name, F_OK) == 0);
    int response_code = file_exists ? 200 : 201;

    int fd = open(file_name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        perror("File open error");
        send_error_response(client_sock, 403, "Forbidden");
        return;
    }

    ssize_t total_written = 0;
    while (total_written < content_length) {
        ssize_t bytes_written = write(fd, body + total_written, content_length - total_written);
        if (bytes_written == -1) {
            perror("Error writing content to file");
            close(fd);
            send_error_response(client_sock, 500, "Internal Server Error");
            return;
        }
        total_written += bytes_written;
    }
    close(fd);
    send_error_response(client_sock, response_code, response_code == 201 ? "Created" : "OK");
}

void handle_connection(int client_sock) {
    char buf[MAX_BUFFER_SIZE] = { 0 };
    ssize_t bytes_read = read_until(client_sock, buf, sizeof(buf), "\r\n\r\n");
    if (bytes_read < 0) {
        send_error_response(client_sock, 500, "Internal Server Error");
        return;
    }
    if (strstr(buf, "\r\n\r\n") == NULL) {
        send_error_response(client_sock, 400, "Bad Request");
        return;
    }
    // Initialize request struct
    HttpRequest request = { .content_length = 0, .body = NULL };

    // Compile regex for request line
    regex_t request_line_regex;
    regmatch_t matches[4] = { 0 };

    const char *pattern = "^([A-Z]{1,8}) (/[a-zA-Z0-9.-]{1,63}) (HTTP/[0-9]\\.[0-9])\r\n";
    if (regcomp(&request_line_regex, pattern, REG_EXTENDED) != 0) {
        send_error_response(client_sock, 400, "Bad Request");
        return;
    }

    // Match request line and populate request.method and request.uri
    if (regexec(&request_line_regex, buf, 4, matches, 0) != 0) {
        send_error_response(client_sock, 400, "Bad Request");
        regfree(&request_line_regex);
        return;
    }
    snprintf(request.method, sizeof(request.method), "%.*s",
        (int) (matches[1].rm_eo - matches[1].rm_so), buf + matches[1].rm_so);
    snprintf(request.uri, sizeof(request.uri), "%.*s", (int) (matches[2].rm_eo - matches[2].rm_so),
        buf + matches[2].rm_so);
    snprintf(request.version, sizeof(request.version), "%.*s",
        (int) (matches[3].rm_eo - matches[3].rm_so), buf + matches[3].rm_so);
    regfree(&request_line_regex);

    printf("version: %s\n", request.version);

    if (strcmp(request.version, "HTTP/1.1") != 0) {
        printf("this one\n");
        send_error_response(client_sock, 505, "Version Not Supported");
        return;
    }

    // Parse headers, specifically Content-Length for PUT requests
    if (strcmp(request.method, "PUT") == 0) {

        // Locate the end of headers and potential start of body data in buf
        char *body_start = strstr(buf, "\r\n\r\n") + 4;
        ssize_t header_end_offset = body_start - buf; // Position after \r\n\r\n
        ssize_t initial_body_bytes = bytes_read - header_end_offset;

        regex_t content_length_regex;
        regmatch_t length_match[2];
        if (regcomp(&content_length_regex, "Content-Length: ([0-9]+)\r\n", REG_EXTENDED) != 0) {
            send_error_response(client_sock, 500, "Internal Server Error");
            return;
        }

        if (regexec(&content_length_regex, buf, 2, length_match, 0) == 0) {
            char content_length_str[32];
            snprintf(content_length_str, sizeof(content_length_str), "%.*s",
                (int) (length_match[1].rm_eo - length_match[1].rm_so), buf + length_match[1].rm_so);
            request.content_length = strtol(content_length_str, NULL, 10);
        } else {
            send_error_response(client_sock, 400, "Bad Request");
            regfree(&content_length_regex);
            return;
        }

        regfree(&content_length_regex);

        if (request.content_length == 0) {
            send_error_response(client_sock, 400, "Bad Request");
            return;
        }
        // Read the body based on Content-Length
        if (request.content_length > 0) {
            request.body = malloc(request.content_length);
            if (request.body == NULL) {
                send_error_response(client_sock, 500, "Internal Server Error");
                return;
            }
            if (initial_body_bytes > 0) {
                memcpy(request.body, body_start, initial_body_bytes);
            }

            ssize_t remaining_bytes = request.content_length - initial_body_bytes;
            if (remaining_bytes > 0) {
                ssize_t body_read
                    = read_n_bytes(client_sock, request.body + initial_body_bytes, remaining_bytes);
                if (body_read != remaining_bytes) {
                    free(request.body);
                    send_error_response(client_sock, 400, "Bad request");
                    return;
                }
            }
        }
    }

    //Dispatch to handler based on method
    if (strcmp(request.method, "GET") == 0) {
        handle_get(client_sock, request.uri);
    } else if (strcmp(request.method, "PUT") == 0) {
        handle_put(client_sock, request.uri, request.content_length, request.body);
    } else {
        send_error_response(client_sock, 501, "Not Implemented");
    }

    free(request.body); // Free allocated memory if PUT
    close(client_sock);
}

//main
int main(int argc, char **argv) {
    if (argc != 2) {
        return EXIT_FAILURE;
    }
    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[1], &endptr, 10);

    if (port < 1 || port > 65535) {
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    while (1) {
        int client_sock = listener_accept(&sock);
        if (client_sock < 0) {
            fprintf(stderr, "Failed to accept connection\n");
            return EXIT_FAILURE;
        }
        handle_connection(client_sock);
    }
    return EXIT_SUCCESS;
}
