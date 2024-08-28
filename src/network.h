#ifndef NETWORK_H
#define NETWORK_H

#include <stdio.h>
#include <stdint.h>
#include <sys\types.h>
#include "util.h"


#define UNIX 0
#define INET 1

int set_nonblocking(int);

int set_tcp_nodelay(int);

int create_and_bind(const char *, const char *, int);

int make_listen(const char *, const char *, int);

int accept_connection(int);

ssize_t send_bytes(int, const unsigned char *, size_t);

ssize_t recv_bytes(int, unsigned char *, size_t);

#endif
