/*
** CS435 MP2: send UDP data from one node to another node
*/

#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
using namespace std;

#define MAXDATASIZE 1024 // max number of bytes we can get at once

// get sockaddr, IPv4 or IPv6:
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
    char buf[MAXDATASIZE];
    struct addrinfo hints, *servinfo, *p;
    int rv;
    char s[INET6_ADDRSTRLEN];
    char request[1500];
    char protocol_input[8], host_port[500], host[500], port[100], path[500];
    const char *protocol = "http://";
    char *filename = "output";
    FILE *fp = fopen(filename, "wb+");

    if (argc != 2)
    {
        fprintf(stderr, "usage: http_client hostname\n");
        exit(1);
    }

    /* Check if input url has the correct protocol of http. */
    strncpy(protocol_input, argv[1], 7);
    protocol_input[7] = '\0';

    if (strcmp(protocol_input, protocol) != 0)
    {
        fprintf(fp, "%s", "INVALIDPROTOCOL");
        exit(1);
    }

    /* Parse input url into host, port and path. */
    sscanf(argv[1] + 7, "%[^/]%s", host_port, path);

    int split_result = sscanf(host_port, "%[^:]:%s", host, port);

    if (split_result != 2)
    {
        strcpy(host, host_port);
        strcpy(port, "80");
    }

    /*
    printf("protocol_input : '%s'\n", protocol_input);
    printf("host_port: '%s'\n", host_port);
    printf("host : '%s'\n", host);
    printf("port : '%s'\n", port);
    printf("path : '%s'\n", path);
    */

    /* Set up protocols and address info for the to-be-connected host. */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        fprintf(fp, "%s", "NOCONNECTION");
        return 1;
    }

    /* Loop through all the results and connect to the first we can. */
    for (p = servinfo; p != NULL; p = p->ai_next)
    {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
        {
            perror("client: socket");
            continue;
        }

        // p->ai_addr has the address family, port and ip address information.
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sockfd);
            perror("client: socket connect error.");
            continue;
        }

        break;
    }

    if (p == NULL)
    {
        fprintf(stderr, "client: failed to connect\n");
        fprintf(fp, "%s", "NOCONNECTION");
        return 2;
    }

    // print server information.
    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
    printf("client: connecting to %s\n", s);

    freeaddrinfo(servinfo); // all done with this structure

    /* Send http request to the server. */
    // Fill request with host and path information
    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nAccept: */*\r\nHost: %s\r\n\r\n", path, host);
    // printf("sent request :\n'%s'\n", request);

    if ((send(sockfd, request, strlen(request), 0) == -1))
    {
        perror("error in sending http request");
        exit(5);
    }

    /* Recev once and extract header and save data into a file. */
    if ((numbytes = recv(sockfd, buf, MAXDATASIZE - 1, 0)) == -1)
    {
        perror("recv");
        exit(1);
    }
    // printf("client received first part data:\n'%s'\n", buf);

    char *data = strstr(buf, "\r\n\r\n");
    char *first_line = strtok(buf, "\n");
    // printf("first_line:\n'%s'\n", first_line);

    // if there is 404 not found response
    if (strstr(first_line, "404") != NULL)
    {
        fprintf(fp, "%s", "FILENOTFOUND");
        exit(6);
    }
    else
    {
        data += 4;
    }

    fwrite(data, 1, numbytes - (data - buf), fp);

    /* Save additional data to the file. */
    memset(buf, 0, sizeof(buf));
    while ((numbytes = recv(sockfd, buf, MAXDATASIZE - 1, 0)) > 0)
    {
        // printf("\nclient received additional data:\n\n'%s'\n", buf);
        fwrite(buf, 1, numbytes, fp);
        memset(buf, 0, sizeof(buf));
    }

    close(sockfd);

    return 0;
}