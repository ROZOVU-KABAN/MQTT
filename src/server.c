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


static ssize_t recv_packet(int clientfd, unsigned char *buf, char *command)
{
   ssize_t nbytes = 0;
   if((nbytes = recv_bytes(clientfd, buf, 1)) <= 0)
	   return -ERRCLIENTDC;
   unsigned char byte = *buf;
   buf++;
   if(DISCONNECT < byte || CONNECT > byte)
   	return -ERRPACKETERR;
   
   unsigned char buff[4];
   int count = 0;
   int n = 0;
   do
   {
 	if((n = recv_bytes(clientfd, buf+count, 1)) <= 0)
		return -ERRCLIENTDC;
	buff[count] = buf[count];
	nbytes += n;
   }while(buff[count++] & (1 << 7));


   const unsigned char *pbuf = &buff[0];
   unsigned long long tlen = mqtt_decode_length(&pbuf);

   if(tlen > conf->max_request_size)
   {
	   nbytes = -ERRMAXREQSIZE;
	   goto exit;
   }

   if((n = recv_bytes(clientfd, buf + 1, tlen)) < 0)
	   goto err;
   nbytes += n;
   *command = byte;
exit:
   return nbytes;
err:
   shutdown(clienfd, 0);
   close(clientfd);
   return nbytes;
}


static void on_read(struct evloop *loop, void *arg)
{
  struct closure *cb = arg;

  unsigned char *buffer = malloc(conf->max_request_size);
  ssize_t bytes = 0;
  char command = 0;

  bytes = recv_packet(cb->fd, buffer, &command);
  if(bytes == -ERRCLIENTDC || bytes == -ERRMAXREQSIZE)
	  goto exit;

  if(bytes == -ERRPACKETERR)
	  goto errdc;
  info.bytes_recv++;

  union mqtt_packet packet;
  unpack_mqtt_packet(buffer, &packet);
  union mqtt_header header hdr = {.byte = command};
  int rc = handlers[hdr.bits.type](cb, &packet);
  if(rc == REARM_W)
  {
     cb->call = on_write;
     evloop_rearm_callbazck_write(loop, cb);
  } else if(rc == REARM_R)
  {
     cb->call = on_read;
     evloop_rearm_callback_read(loop, cb);
  }

exit:
  free(buffer);
  return;
errdc:
  free(buffer);
  sol_error("Dropping client");
  shutdown(cb->fd, 0);
  close(cb->fd);

  hashtable_del(sol.clients, ((struct sol_client*) cb->obj)->client_id);
  hashtable_del(sol.closures, cb->closure_id);
  info.nclients--;
  info.nconnections--;
  return;
}

static void on_write(struct evloop *loop, void *arg)
{
  struct closure *cb = arg;
  ssize_t sent;
  if((sent = send_bytes(cb->fd, cb->payload->data, cb->payload->size)) < 0)
	  sol_error("Error writing on socket to client %s: %s",
			  ((struct sol_client*)cb->obj)->client_id, strerror(errno));

  info.bytes_sent += sent;
  bytestring_release(cb->payload);
  cb->payload = NULL;

  cb->call = on_read;
  evloop_rearm_callback_read(loop, cb);
}


