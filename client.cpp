#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string>

#define MAXDATASIZE 4096

// Get a pointer to the IP address in the sockaddr structure
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
    int sockfd, numbytes;
    char buf[MAXDATASIZE]; // Buffer for receiving data
    struct addrinfo hints, *servinfo, *p;
    int rv;
    char s[INET6_ADDRSTRLEN];

    if (argc != 4)
    {
        fprintf(stderr, "usage: client <proxy address> <proxy port> <URL to retrieve>\n");
        exit(1);
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(argv[1], argv[2], &hints, &servinfo)) != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // Loop through all results and connect to the first one that works
    for (p = servinfo; p != NULL; p = p->ai_next)
    {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                             p->ai_protocol)) == -1)
        {
            perror("client: socket");
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sockfd);
            perror("client: connect");
            continue;
        }

        break;
    }

    if (p == NULL)
    {
        fprintf(stderr, "client: failed to connect\n");
        freeaddrinfo(servinfo); // Free the address info structure before exiting
        return 2;
    }

    // Get the IP address of the proxy we connected to
    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
    printf("client: connecting to %s\n", s);
    freeaddrinfo(servinfo); // All done with this structure

    char request[MAXDATASIZE];
    std::string url = argv[3];

    if (url.compare(0, 7, "http://") != 0)
    {
        fprintf(stderr, "Error: Only http:// URLs are supported\n");
        return 1;
    }
    size_t host_start = 7;
    size_t path_start = url.find_first_of("/?#", host_start);
    std::string host;

    if (path_start != std::string::npos)
    {
        host = url.substr(host_start, path_start - host_start);
    }
    else
    {
        host = url.substr(host_start);
    }
    if (host.empty())
    {
        fprintf(stderr, "Error: Invalid URL\n");
        return 1;
    }
    std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", url.c_str(), host.c_str());

    // Send the request to the proxy
    size_t request_len = strlen(request);
    size_t total_sent = 0;

    while (total_sent < (size_t)request_len)
    {
        ssize_t bytes_sent = send(sockfd, request + total_sent, request_len - total_sent, 0);

        if (bytes_sent < 0)
        {
            if (errno == EINTR)
                continue;

            perror("send");
            close(sockfd);
            return 1;
        }

        if (bytes_sent == 0)
        {
            fprintf(stderr, "send: connection closed\n");
            close(sockfd);
            return 1;
        }
        total_sent += bytes_sent;
    }
    // Open a file to save the downloaded webpage
    FILE *fp = fopen("downloaded.html", "wb");
    if (fp == NULL)
    {
        perror("fopen");
        close(sockfd);
        return 1;
    }

    // Buffer for storing HTTP response headers
    char response_header[8192];
    size_t header_len = 0;
    int header_done = 0;

    while ((numbytes = recv(sockfd, buf, sizeof(buf), 0)) > 0)
    {
        size_t i = 0;
        // Read HTTP headers first
        if (!header_done)
        {
            for (i = 0; i < (size_t)numbytes; i++)
            {
                if (header_len >= sizeof(response_header) - 1)
                {
                    fprintf(stderr, "Response header too large\n");
                    fclose(fp);
                    close(sockfd);
                    return 1;
                }

                response_header[header_len++] = buf[i]; // Add the current byte to the response header buffer
                response_header[header_len] = '\0'; // Null-terminate the response header buffer

                // Check if the HTTP headers have ended
                if (strstr(response_header, "\r\n\r\n") != NULL)
                {
                    header_done = 1;
                    i++;
                    break;
                }
            }
        }

        // Save the HTTP response body
        if (header_done && i < (size_t)numbytes)
        {
            size_t remaining = (size_t)numbytes - i;
            size_t written = fwrite(buf + i, 1, remaining, fp);
            if (written != remaining || ferror(fp))
            {
                fprintf(stderr, "Error: Failed to write file\n");
                fclose(fp);
                close(sockfd);
                return 1;
            }
        }
    }

    // Check for receiving errors
    if (numbytes < 0)
    {
        perror("recv");
        fclose(fp);
        close(sockfd);
        return 1;
    }

    if (!header_done)
    {
        fprintf(stderr, "Incomplete HTTP response header\n");
        fclose(fp);
        close(sockfd);
        return 1;
    }

    printf("Download completed: downloaded.html\n");

    fclose(fp);
    close(sockfd);

    return 0;
}