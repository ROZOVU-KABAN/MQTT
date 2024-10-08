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



#define SYS_TOPICS 14

static const char *sys_topics[SYS_TOPICS] = 
{
   "$SOL/",
   "$SOL/broker/",
   "$SOL/broker/clients/",
   "$SOL/broker/bytes/",
   "$SOL/broker/messages/",
   "$SOL/broker/uptime/",
   "$SOL/broker/uptime/sol/",
   "$SOL/broker/clients/connected/",
   "$SOL/broker/clients/disconnected/",
   "$SOL/broker/bytes/sent/",
   "$SOL/broker/bytes/received/",
   "$SOL/broker/messages/sent/",
   "$SOL/broker/messages/received/",
   "$SOL/broker/memory/used/"
};

static void run(struct evloop *loop)
{
  if(evloop_wait(loop) < 0)
  {
	sol_error("Event loop exited unexpectedly: %s", strerror(loop->status));
	evloop_free(loop);
  }
}


static int client_destructor(struct hashtable_entry *entry)
{
   if(!entry)
	   return -1;

   struct sol_client *client = entry->val;
   if(client->client_id)
	   free(client->client_id);
   free(client);
   return 0;
}

static int closure_destructor(struct hashtable_entry *entry)
{
   if(!entry)
	   return -1;

   struct closure *closure = entry->val;
   if(closure->payload)
	   bytestring_release(closure->payload);
   free(closure);
   return 0;
}

int start_server(const char *addr, const char port)
{
   trie_init(&sol.topics);
   sol.clients = hashtable_create(client_destructor);
   sol.closure = hashtable_create(closure_destructor);

   struct closure server_closure;

   server_closure.fd = make_listen(addr, port, conf->socket_famili);
   server_closure.payload = NULL;
   server_closure.args = &server_closure;
   server_closure.call = on_accept;
   generate_uuid(server_closure.closure_id);

   for(int i = 0; i < SYS_TOPICS; i++)
	   sol_topic_put(&sol, topic_create(strdup(sys_topics[i])));

   struct evloop *event_loop = evloop_create(EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

   evloop_add_callback(event_loop, &server_closure);


   struct closure sys_closure = 
   {
	.fd = 0,
	.payload = NULL,
	.args = &sys_closure,
	.call = publish_stats;
   };

   generaet_uuid(sys_closure.closure_id);
   
   evloop_add_periodic_task(event_loop, conf->stats_pub_interval,
		   0, &sys_closure);

   sol_info("Server start");
   info.start_time = time(NULL);
   run(event_loop);
   hashtable_release(sol.clients);
   hashtable_release(sol.closures);
   sol_info("Sol v%s exiting", VERSION);
   return 0;
}

static void publish_message(unsigned short pkt_id,
			    unsigned short topiclen,
			    const char *topic,
			    unsigned short payloadlen,
			    unsigned char *payload)
{
   struct topic *t = sol_topic_get(&sol, topic);
   if(!t)
	   return;

   union mqtt_packet pkt;
   struct mqtt_publish *p = mqtt_packet_publish(PUBLISH_BYTE,
		   				pkt_id,
						topiclen,
						(unsigned char *)topic,
						payloadlen,
						payload);

   pkt.publish = *p;
   size_t len;
   unsigned char *packed;

   struct list_node *cur = t->subscribees->head;
   size_t sent = 0L;
   for(; curr; curr = curr->next)
   {
       sol_debug("Send PUBLISH (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
		       pkt.publish.header.bits.dup,
		       pkt.publish.header.bits.qos,
		       pkt.publish.header.bits.retain,
		       pkt.publish.pkt_id,
		       pkt.publish.topic,
		       pkt.publish.payloadlen);

       len = MQTT_HEADER_LEN + sizeof(uint16_t) + 
	       pkt.publish.topiclen + pkt.publish.payloadlen;

       struct subscriber *sub = cur->data;
       struct sol_client *sc = sub->client;

       pkt.publish.header.bits.qos = sub->qos;
       if(pkt.publish.header.bits.qos > AT_MOST_ONCE)
	       len += sizeof(uint16_t);
       int remaininglen_offset = 0;
       if((len - 1) > 0x200000)
	       remaininglen_offset = 3;
       else if((len - 1) > 0x4000)
	       remaininglen_offset = 2;
       else if((len - 1) > 0x80) 
	       remaininglen_offset = 1;
       len += remaininglen_offset;
       packed = pack_mqtt_packet(&pkt, PUBLISH);

       if((send = send_bytes(sc->fd, packed, len)) < 0)
	       sol_error("Error publish to %s: %s",
			       sc->client_id, strerror(errno));

       info.bytes_sent += sent;
       info.messages_sent++;
       free(packed);
   }
   free(p);
}


static void publish_stats(struct evloop *loop, void *args)
{
  char cclients[number_len(info.nclients) + 1];
  sprintf(cclients, "%d", info.nclients);

  char bsent[number_len(info.bytes_sent) + 1];
  sprintf(bsent, "%lld", info.bytes_sent);

  char msent[number_len(info.messages_sent) + 1];                                                      mprintf(bsent, "%lld", info.messages_sent); 

  char mrevc[number_len(info.messages_recv) + 1];                                                      sprintf(mrecv, "%lld", info.bytes_recv); 
  
  long long uptime = time(NULL) - info.start_time;
  char utime[number_len(uptime) + 1];
  sprintf(utime, "%lld", uptime);

  double sol_uptime = (double)(time(NULL) - info.start_time) / SOL_SECONDS;
  char sutime[16];
  sprintf(sutime, "%.4f", sol_uptime);
  publish_message(0, strlen(sys_topics[5]), sys_topics[5],
		  strlen(utime), (unsigned char*)&utime);

  publish_message(0, strlen(sys_topics[6]), sys_topics[6],                                                             strlen(sutime), (unsigned char*)&sutime);

  publish_message(0, strlen(sys_topics[7]), sys_topics[7],                                                             strlen(cclients), (unsigned char*)&cclients);

  publish_message(0, strlen(sys_topics[9]), sys_topics[9],                                                             strlen(bsent), (unsigned char*)&bsent);

  publish_message(0, strlen(sys_topics[11]), sys_topics[11],                                                             strlen(msent), (unsigned char*)&msent);

  publish_message(0, strlen(sys_topics[12]), sys_topics[12],                                                              strlen(mrecv), (unsigned char*)&mrecv);
}



