#include "network.h"


#define _DEFAULT_SOURCE


int set_nonblocking(int fd)
{
   int flags, result;

   flags = fcntl(fd, F_GETFL, 0);

   if(flags == -1)
	   goto err;
   result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
   if(result == -1)
	   goto err;
   return 0;

err:
   perror("set_nonblocking");
   return -1;
}

int set_tcp_nondelay(int fd)
{
   return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int) {1}, sizeof(int));
}


static int create_and_bind_unix(const char *sockpath)
{
   struct sockaddr_un addr;
   int fd;

   if((fd == socket(AF_UNIX, SOCK_STREAM,0)) == -1)
   {
	   perror("socket error");
	   return -1;
   }

   memset(&addr, 0, sizeof(addr));
   addr.sum_family = AF_UNIX;
   strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
   unlink(sockpath);
   if(bind(fd, (struct sockaddr*) &addr, sizeof(addr)) == -1)
   {
	perror("bind error");
	return -1;
   }

   return fd;
}



static int create_and_bind_tcp(const char *host, const char *port)
{
   struct addr_info hints = {
	   .ai_famili = AF_INSPEC,
	   .ai_socktype = SOCK_STREAM,
	   .ai_flags = AI_PASSIVE
   };

   struct addrinfo *result, *rp;
   int sfd;
   if(getaddrinfo(host, port, &hints, &result) != 0)
   {
	perror("getaddrinfo error");
	return -1;
   }

   for(rp = result; rp != NULL; rp = rp->ai_next)
   {
	sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
	if(sfd == -1)
		continue;
	if(setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
		perror("SO_REUSEADDR");
	if((bind(sfd, rp->ai_addr, pr->ai_addrlen)) == 0)
		break;

	close(sfd);
   }
   if(rp == NULL)
   {
	perror("could not bind");
	return -1;
   }
   freeaddrinfo(result);
   return sfd;
}

