/*
* Proxy Web server - PA3
* Assumptions: Link prefetch implemented only for href for now, blocklist for only
* a list of website which is in this code itself not taking from outside file
*
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <regex.h>
#include <signal.h>

#define MAX_REQUESTS 100
#define BUFFER_SIZE 32654
#define MAX_URL_LENGTH 1024
#define CACHE_DIR "./cache"
#define MAX_CACHE_ENTRIES 1000
#define MAX_LINKS 100
#define MAX_BLOCKLIST_SIZE 50
#define MAX_PATTERN_LENGTH 256
#define MAX_FILENAME_LENGTH 256

typedef struct {
    char *url;
    time_t created_at;
    char *file_path;
} CacheObject;

typedef struct {
    char url[MAX_URL_LENGTH];
} Link;

typedef struct {
    char pattern[MAX_PATTERN_LENGTH];
    regex_t compiled;
} BlockPattern;

CacheObject *cache_table[MAX_CACHE_ENTRIES] = {NULL};
int cache_entry_count = 0;
BlockPattern blocklist[MAX_BLOCKLIST_SIZE];
int blocklist_count = 0;
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
int timeout = 60;
int end_flag = 0;

void *handle_client(void *arg);
int parse_request(int client_sock, char *request, char *host, int *port, char *path, char *headers);
void handle_http_errors(int client_sock, const char *http_v, const char *code);
int check_cache(char *url, time_t current_time, char **data, size_t *data_size);
int connect_to_server(const char *host, int port);
void add_to_cache(char* url, const char* host, const char* path, const char* data, size_t data_size, int skip_headers);
void *prefetch_links(void *arg);
void extract_links(const char *html, size_t size, const char *base_url, Link *links, int *num_links);
int check_blocklist(const char *host);
void init_blocklist();
char *generate_cache_filename(const char *host, const char *path);
char *find_response_body(const char *response, size_t response_size, size_t *body_size);
void signal_handler(int sig);
int remove_all_files(const char *dir_path);

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <timeout>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    timeout = atoi(argv[2]);
    
    if (port <= 0 || timeout <= 0) {
        fprintf(stderr, "Invalid port or timeout value\n");
        return 1;
    }
    
    init_blocklist();
    
    int proxy_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_sock < 0) {
        perror("Socket creation failed");
        return 1;
    }
    
    int opt = 1;
    setsockopt(proxy_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in proxy_addr = {0};
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(port);
    
    if (bind(proxy_sock, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) < 0) {
        perror("Bind failed");
        close(proxy_sock);
        return 1;
    }
    
    if (listen(proxy_sock, 10) < 0) {
        perror("Listen failed");
        close(proxy_sock);
        return 1;
    }
    
    printf("Proxy server started on port %d with timeout %d seconds\n", port, timeout);
    printf("Cache directory at: %s\n", CACHE_DIR);
    
    signal(SIGINT, signal_handler);
    mkdir(CACHE_DIR, 0700);
    
    while (!end_flag) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(proxy_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            perror("Accept failed");
            continue;
        }
        
        int *client_ptr = malloc(sizeof(int));
        if (!client_ptr) {
            close(client_sock);
            continue;
        }
        *client_ptr = client_sock;
        
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_ptr) != 0) {
            close(client_sock);
            free(client_ptr);
            continue;
        }
        pthread_detach(thread_id);
    }
    
    printf("Shutting down proxy server...\n");
    close(proxy_sock);
    pthread_mutex_destroy(&cache_mutex);
    pthread_mutex_destroy(&log_mutex);
    printf("Proxy server shutdown complete\n");
    
    return 0;
}

void *handle_client(void *arg) {
    int client_sock = *((int *)arg);
    free(arg);
    
    int keep_alive = 0;
    struct timeval tv = {10, 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    for (int i = 0; i < MAX_REQUESTS; i++) {
        char buffer[BUFFER_SIZE] = {0};
        char host[MAX_URL_LENGTH] = {0};
        char path[MAX_URL_LENGTH] = {0};
        char headers[BUFFER_SIZE] = {0};
        int port = 80;
        
        ssize_t bytes_read = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) break;
        
        buffer[bytes_read] = '\0';
        keep_alive = (strstr(buffer, "Connection: keep-alive") != NULL);
        
        if (parse_request(client_sock, buffer, host, &port, path, headers) <= 0) {
            if (!keep_alive) break;
            continue;
        }
        
        if (check_blocklist(host)) {
            printf("Blocked access to: %s\n", host);
            handle_http_errors(client_sock, "1.1", "403");
            if (!keep_alive) break;
            continue;
        }
        
        char full_url[MAX_URL_LENGTH * 2];
        snprintf(full_url, sizeof(full_url), "%s%s", host, path);
        
        char *cached_data = NULL;
        size_t cached_size = 0;
        time_t current_time = time(NULL);
        
        if (check_cache(full_url, current_time, &cached_data, &cached_size)) {
            printf("Cache Found for: %s\n", full_url);
            send(client_sock, cached_data, cached_size, 0);
            free(cached_data);
            if (!keep_alive) break;
            continue;
        }
        
        printf("Cache Not Found for: %s\n", full_url);
        
        int server_fd = connect_to_server(host, port);
        if (server_fd < 0) {
            printf("Failed to connect to: %s:%d\n", host, port);
            handle_http_errors(client_sock, "1.1", "404");
            if (!keep_alive) break;
            continue;
        }
        
        char server_request[BUFFER_SIZE];
        snprintf(server_request, BUFFER_SIZE,
                "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                path, host);
        
        if (send(server_fd, server_request, strlen(server_request), 0) < 0) {
            handle_http_errors(client_sock, "1.1", "500");
            close(server_fd);
            if (!keep_alive) break;
            continue;
        }
        
        char *response = NULL;
        size_t response_size = 0;
        char recv_buffer[BUFFER_SIZE];
        
        while ((bytes_read = recv(server_fd, recv_buffer, BUFFER_SIZE - 1, 0)) > 0) {
            send(client_sock, recv_buffer, bytes_read, 0);
            
            // saving response for caching
            char *new_response = realloc(response, response_size + bytes_read);
            if (new_response) {
                response = new_response;
                memcpy(response + response_size, recv_buffer, bytes_read);
                response_size += bytes_read;
            }
        }
        
        close(server_fd);
        
        if (response && response_size > 0) {
            add_to_cache(full_url, host, path, response, response_size, 1);
            
            // Prefetch
            if (strstr(full_url, ".html") || strstr(full_url, ".htm") || strstr(path, "/") == path) {
                char *response_copy = malloc(response_size);
                char *url_copy = strdup(full_url);
                
                if (response_copy && url_copy) {
                    memcpy(response_copy, response, response_size);
                    
                    struct {
                        char *response;
                        size_t response_size;
                        char *url;
                    } *prefetch_data = malloc(sizeof(*prefetch_data));
                    
                    if (prefetch_data) {
                        prefetch_data->response = response_copy;
                        prefetch_data->response_size = response_size;
                        prefetch_data->url = url_copy;
                        
                        pthread_t prefetch_thread;
                        if (pthread_create(&prefetch_thread, NULL, prefetch_links, prefetch_data) == 0) {
                            pthread_detach(prefetch_thread);
                            // printf("Started prefetching for: %s\n", url_copy);
                        } else {
                            free(response_copy);
                            free(url_copy);
                            free(prefetch_data);
                        }
                    } else {
                        free(response_copy);
                        free(url_copy);
                    }
                }
            }
            
            free(response);
        }
        
        if (!keep_alive) break;
    }
    
    close(client_sock);
    return NULL;
}

void init_blocklist() {
    const char *patterns[] = {
        ".*\\.google\\.com", "mail\\.yahoo\\.com", "157\\.240\\.28\\.[0-2][0-9][0-9]",
        "facebook\\.com", "tiktok\\.com"
    };
    
    blocklist_count = sizeof(patterns) / sizeof(patterns[0]);
    
    for (int i = 0; i < blocklist_count; i++) {
        strncpy(blocklist[i].pattern, patterns[i], MAX_PATTERN_LENGTH - 1);
        // used the regex.h library - i'm not sure if that is accepted
        regcomp(&blocklist[i].compiled, blocklist[i].pattern, REG_EXTENDED | REG_NOSUB);
    }
}

int check_blocklist(const char *host) {
    for (int i = 0; i < blocklist_count; i++)
        if (regexec(&blocklist[i].compiled, host, 0, NULL, 0) == 0)
            return 1;
    return 0;
}

void extract_links(const char *html, size_t size, const char *base_url, Link *links, int *num_links) {
    *num_links = 0;
    
    char base_host[MAX_URL_LENGTH] = {0};
    char *protocol_end = strstr(base_url, "://");
    if (protocol_end) {
        char *path_start = strchr(protocol_end + 3, '/');
        if (path_start)
            strncpy(base_host, base_url, path_start - base_url);
        else
            strcpy(base_host, base_url);
    }
    
    // Find href links (TODO: prefetch more tags like src)
    const char *href_pattern = "href=\"";
    const char *pos = html;
    
    while (*num_links < MAX_LINKS && (pos = strstr(pos, href_pattern)) != NULL) {
        pos += 6;
        const char *end = strchr(pos, '"');
        
        if (end && end - pos < MAX_URL_LENGTH - 1) {
            char link[MAX_URL_LENGTH] = {0};
            strncpy(link, pos, end - pos);
            
            if (strncmp(link, "http", 4) == 0) {
                strncpy(links[*num_links].url, link, MAX_URL_LENGTH - 1);
                (*num_links)++;
            } else if (link[0] == '/') {
                size_t base_len = strlen(base_host);
                size_t link_len = strlen(link);
                if (base_len + link_len < MAX_URL_LENGTH) {
                    strcpy(links[*num_links].url, base_host);
                    strncat(links[*num_links].url, link, MAX_URL_LENGTH - base_len - 1);
                    (*num_links)++;
                }
            }
        }
        pos = end ? end + 1 : html + size;
    }
}

void *prefetch_links(void *arg) {
    struct {
        char *response;
        size_t response_size;
        char *url;
    } *data = arg;
    
    const char *body = strstr(data->response, "\r\n\r\n");
    if (body) {
        body += 4;
        Link links[MAX_LINKS];
        int num_links = 0;
        
        extract_links(body, data->response_size - (body - data->response), data->url, links, &num_links);
        // printf("Found %d links to prefetch from %s\n", num_links, data->url);
        
        for (int i = 0; i < num_links; i++) {
            char host[MAX_URL_LENGTH] = {0};
            char path[MAX_URL_LENGTH] = {0};
            
            char *protocol_end = strstr(links[i].url, "://");
            if (protocol_end) {
                char *host_start = protocol_end + 3;
                char *path_start = strchr(host_start, '/');
                
                if (path_start) {
                    strncpy(host, host_start, path_start - host_start);
                    strcpy(path, path_start);
                } else {
                    strcpy(host, host_start);
                    strcpy(path, "/");
                }
                
                if (check_blocklist(host))
                    continue;
                
                char *cached_data = NULL;
                size_t cached_size = 0;
                
                if (check_cache(links[i].url, time(NULL), &cached_data, &cached_size)) {
                    printf("Already cached. %s\n", links[i].url);
                    free(cached_data);
                    continue;
                }
                
                int server_fd = connect_to_server(host, 80);
                if (server_fd >= 0) {
                    char request[BUFFER_SIZE];
                    snprintf(request, BUFFER_SIZE,
                            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                            path, host);
                    
                    if (send(server_fd, request, strlen(request), 0) >= 0) {
                        char *resp = NULL;
                        size_t resp_size = 0;
                        char buffer[BUFFER_SIZE];
                        ssize_t bytes_read;
                        
                        while ((bytes_read = recv(server_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
                            char *new_resp = realloc(resp, resp_size + bytes_read);
                            if (new_resp) {
                                resp = new_resp;
                                memcpy(resp + resp_size, buffer, bytes_read);
                                resp_size += bytes_read;
                            }
                        }
                        
                        if (resp && resp_size > 0) {
                            add_to_cache(links[i].url, host, path, resp, resp_size, 1);
                            printf("Prefetched: %s\n", links[i].url);
                            free(resp);
                        }
                    }
                    close(server_fd);
                }
            }
        }
    }
    
    free(data->response);
    free(data->url);
    free(data);
    return NULL;
}

int parse_request(int client_sock, char *request, char *host, int *port, char *path, char *headers) {
    char *first_line = strtok(request, "\r\n");
    if (!first_line)
        return 0;
    
    if (strncmp(first_line, "GET ", 4) != 0) {
        handle_http_errors(client_sock, "1.1", "405");
        return -1;
    }
    
    char *url_start = first_line + 4;
    char *url_end = strchr(url_start, ' ');
    if (!url_end)
        return 0;
    
    *url_end = '\0';
    
    char *protocol_end = strstr(url_start, "://");
    if (!protocol_end) {
        *url_end = ' ';
        return 0;
    }
    
    char *host_start = protocol_end + 3;
    char *path_start = strchr(host_start, '/');
    
    if (!path_start) {
        strcpy(host, host_start);
        strcpy(path, "/");
    } else {
        *path_start = '\0';
        strcpy(host, host_start);
        *path_start = '/';
        strcpy(path, path_start);
    }
    
    *url_end = ' ';
    
    char *port_start = strchr(host, ':');
    if (port_start) {
        *port_start = '\0';
        *port = atoi(port_start + 1);
    } else {
        *port = 80;
    }
    
    char *headers_start = strchr(request, '\n');
    if (headers_start)
        strcpy(headers, headers_start + 1);
    else
        headers[0] = '\0';
    
    return 1;
}

void handle_http_errors(int client_sock, const char *http_v, const char *code) {
    static const char *messages[] = {
        "400", "Bad Request", "Request could not be parsed or is malformed",
        "403", "Forbidden", "Access to this website is blocked by proxy policy",
        "404", "Not Found", "The requested resource was not found",
        "405", "Method Not Allowed", "Only GET method is supported",
        "500", "Internal Server Error", "An internal server error occurred",
        "505", "HTTP Version Not Supported", "HTTP version not supported"
    };
    
    for (int i = 0; i < 18; i += 3) {
        if (strcmp(code, messages[i]) == 0) {
            char response[512];
            snprintf(response, sizeof(response),
                    "HTTP/%s %s %s\r\nContent-Type: text/plain\r\nContent-Length: %ld\r\n\r\n%s",
                    http_v, messages[i], messages[i+1], strlen(messages[i+2]), messages[i+2]);
            send(client_sock, response, strlen(response), 0);
            return;
        }
    }
}

char *generate_cache_filename(const char *host, const char *path) {
    char *filename = (char *)malloc(MAX_FILENAME_LENGTH);
    if (!filename) return NULL;
    
    strcpy(filename, host);
    
    // Replace problematic characters in path with underscores
    char path_copy[MAX_URL_LENGTH];
    strncpy(path_copy, path, MAX_URL_LENGTH - 1);
    path_copy[MAX_URL_LENGTH - 1] = '\0';
    
    char *p = path_copy;
    while (*p) {
        if (*p == '/' || *p == '?' || *p == '&' || *p == '=' || *p == ':')
            *p = '_';
        p++;
    }
    
    strcat(filename, path_copy);
    
    if (strlen(filename) > MAX_FILENAME_LENGTH - 20) {
        filename[MAX_FILENAME_LENGTH - 20] = '\0';
    }
    
    return filename;
}

char *find_response_body(const char *response, size_t response_size, size_t *body_size) {
    const char *body = strstr(response, "\r\n\r\n");
    if (body) {
        body += 4;
        *body_size = response_size - (body - response);
        return (char *)body;
    }
    *body_size = 0;
    return NULL;
}

// Connect to remote server
int connect_to_server(const char *host, int port) {
    struct hostent *he = gethostbyname(host);
    if (!he)
        return -1;
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        return -1;
    
    struct timeval tv = {5, 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(server_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        return -1;
    }
    
    return server_fd;
}

int check_cache(char *url, time_t current_time, char **data, size_t *data_size) {
    // Skip dynamic content
    if (strchr(url, '?'))
        return 0;
    
    pthread_mutex_lock(&cache_mutex);
    
    for (int i = 0; i < cache_entry_count; i++) {
        // printf("Cache Entries: %s, %s, %ld\n", cache_table[i]->url, cache_table[i]->file_path, current_time - cache_table[i]->created_at);
        if (cache_table[i] && strcmp(cache_table[i]->url, url) == 0) {
            if (current_time - cache_table[i]->created_at > timeout) {
                pthread_mutex_unlock(&cache_mutex);
                return 0;
            }
            
            FILE *file = fopen(cache_table[i]->file_path, "rb");
            if (!file) {
                pthread_mutex_unlock(&cache_mutex);
                return 0;
            }
            
            fseek(file, 0, SEEK_END);
            size_t file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            
            char *file_data = malloc(file_size);
            if (!file_data || fread(file_data, 1, file_size, file) != file_size) {
                free(file_data);
                fclose(file);
                pthread_mutex_unlock(&cache_mutex);
                return 0;
            }
            fclose(file);
            
            char headers[512];
            snprintf(headers, sizeof(headers),
                    "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nX-Cache: HIT\r\n\r\n",
                    file_size);
            
            *data_size = strlen(headers) + file_size;
            *data = malloc(*data_size);
            if (*data) {
                memcpy(*data, headers, strlen(headers));
                memcpy(*data + strlen(headers), file_data, file_size);
            }
            
            free(file_data);
            pthread_mutex_unlock(&cache_mutex);
            return *data != NULL;
        }
    }
    
    pthread_mutex_unlock(&cache_mutex);
    return 0;
}

void add_to_cache(char* url, const char* host, const char* path, const char* data, size_t data_size, int skip_headers) {
    if (!data || data_size == 0 || strchr(url, '?'))
        return;
    
    pthread_mutex_lock(&cache_mutex);
    mkdir(CACHE_DIR, 0700);
    
    const char *content_data = data;
    size_t content_size = data_size;
    
    if (skip_headers) {
        size_t body_size = 0;
        char *body = find_response_body(data, data_size, &body_size);
        if (body) {
            content_data = body;
            content_size = body_size;
        }
    }
    
    char *filename = generate_cache_filename(host, path);
    if (!filename) {
        pthread_mutex_unlock(&cache_mutex);
        return;
    }
    
    char file_path[MAX_FILENAME_LENGTH + sizeof(CACHE_DIR) + 2];
    snprintf(file_path, sizeof(file_path), "%s/%s", CACHE_DIR, filename);
    free(filename);
    
    // Check if URL exists in cache
    if(cache_entry_count == 0) {
        cache_entry_count++;
    }
    
    for (int i = 0; i < cache_entry_count; i++) {
        if (cache_table[i] && strcmp(cache_table[i]->url, url) == 0) {
            // Update existing entry if present
            FILE *file = fopen(cache_table[i]->file_path, "wb");
            if (file) {
                fwrite(content_data, 1, content_size, file);
                fclose(file);
                cache_table[i]->created_at = time(NULL);
                // printf("Updated cache: %s\n", cache_table[i]->file_path);
            }
            pthread_mutex_unlock(&cache_mutex);
            return;
        }
    }
    
    FILE *file = fopen(file_path, "wb");
    if (!file) {
        pthread_mutex_unlock(&cache_mutex);
        return;
    }
    
    fwrite(content_data, 1, content_size, file);
    fclose(file);
    // printf("Added to cache: %s\n", file_path);
    
    CacheObject *new_entry = malloc(sizeof(CacheObject));
    if (!new_entry) {
        remove(file_path);
        pthread_mutex_unlock(&cache_mutex);
        return;
    }
    
    new_entry->url = strdup(url);
    new_entry->file_path = strdup(file_path);
    new_entry->created_at = time(NULL);
    
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!cache_table[i]) {
            cache_table[i] = new_entry;
            break;
        }
    }
    
    pthread_mutex_unlock(&cache_mutex);
}

void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\nReceived Ctrl+C. Shutting down gracefully...\n");
        remove_all_files(CACHE_DIR);
        end_flag = 1;
        exit(0);
    }
}

int remove_all_files(const char *dir_path) {
    char command[512];
    snprintf(command, sizeof(command), "rm -f %s/*", dir_path);
    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        perror("popen failed");
        return -1;
    }
    pclose(fp);
    return 0;
}
