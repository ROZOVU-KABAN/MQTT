#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include "mqtt.h"
#include "network.h"
#include "pack.c"
#include "util.h"
#include "server.h"
#include "core.h"
#include "config.h"
#include "hashtable.h"


static const double SOL_SECONDS = 88775.24;

static struct sol_info info;
static struct sol sol;

typedef int handler(struct closure *, union mqtt_packet *);

static int connect_handler(struct closure *, union mqtt_packet *);
static int disconnect_handler(struct closure *, union mqtt_packet *);
static int subscribe_handler(struct closure *, union mqtt_packet *);
static int unsubscribe_handler(struct closure *, union mqtt_packet *);
static int publish_handler(struct closure *, union mqtt_packet *);
static int puback_handler(struct closure *, union mqtt_packet *);
static int pubrec_handler(struct closure *, union mqtt_packet *);
static int pubrel_handler(struct closure *, union mqtt_packet *);
static int pubcomp_handler(struct closure *, union mqtt_packet *);
static int pingreq_handler(struct closure *, union mqtt_packet *);


static handler *handler[15] =
{
  NULL,
  connect_handler,
  NULL,
  publish_handler,
  puback_handler,
  pubrec_handler,
  pubrel_handler,
  pubcomp_handler,
  subscribe_handler,
  NULL,
  unsubscribe_handler,
  NULL,
  pingreq_handler,
  NULL,
  disconnect_handler
};

struct connection 
{
   char ip[INET_ADDRSTRLEN + 1];
   int fd;
};

static void on_read(struct evloop *, void *);
static void on_write(struct evloop *, void *);
static void on_accep(struct evloop *, void *);


static void publish_stats(struct evloop *, void *);

static int accept_new_client(int fd, struct connection *conn)
{
   if(!conn)
	   return -1;

   int clientsock = accept_connection(fd);
   if(clientsock == -1)
	   return -1;

   struct sockaddr_in addr;
   socklen_t addrlen = sizeof(addr);

   if(getpeername(clientsock, (struct sockaddr *) &addr, &addrlen) < 0)
	   return -1;

   char ip_buff[INET_ADDRSTRLEN + 1];
   if(inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf)) == NULL)
	   return -1;

   struct sockaddr_in sin;
   socket_len sinlen = sizeof(sin);
   if(getsockname(fd, (struct sockaddr*)&sin, &sinlen) < 0)
	   return -1;

   conn->fd = clientsock;
   strcpy(conn->ip, ip_buf);
   return 0;
}

static void on_accept(struct evloop *loop, void *arg)
{
   struct closure *server = arg;
   struct connection conn;
   accept_new_client(server->fd, &conn);

   struct closure *client_closure = malloc(sizeof(*client_closure));
   if(!client_closure)
	   return;

   client_closure->fd = conn.fd;
   client_closure->obj = NULL;
   client_closure->payload = NULL;
   client_closure->args = client_closure;
   client_closure->call = on_read;
   generate_uuid(client_closure->closure_id);
   hashtable_put(sol.closures, client_closure->closure_id, client_closure);
   
   evloop_add_callback(loop, client_closure);
   evloop_rearm_callback_read(loop, server);

   info.nclients++;
   info.nconnections++;
   sol_info("New connection from %s on port %s", conn.ip, conf->port);
}

