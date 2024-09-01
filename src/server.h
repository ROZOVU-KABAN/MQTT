#ifndef SERVER_H
#define SERVER_H

#define EPOLL_MAX_EVENTS 256
#define EPOLL_TIMEOUT -1

#define ERRCLIENTDC 1
#define ERRPACKETERR 2
#define ERRMAXREQSIZE 3


#define REARM_R 0
#define REARM_W 1

int start_server(const char *, const char *);

struct sol_info 
{
  int nclients;
  int nconnections;
  long long start_time;
  long long bytes_recv;
  long long bytes_send;
  long long messages_sent;
  long long messages_recv;
};

#endif
