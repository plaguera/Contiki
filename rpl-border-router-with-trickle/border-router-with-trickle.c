#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"
#include "dev/serial-line.h"
#include "dev/slip.h"
#include "lib/random.h"
#include "lib/trickle-timer.h"
#include "net/ip/uip.h"
#include "net/ip/uip-debug.h"
#include "net/ipv6/uip-ds6.h"
#include "net/netstack.h"
#include "net/rpl/rpl.h"
#include "net/rpl/rpl-private.h"
#include "simple-udp.h"
#include "servreg-hack.h"
#include "sys/ctimer.h"
#include "sys/etimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if RPL_WITH_NON_STORING
#include "net/rpl/rpl-ns.h"
#endif /* RPL_WITH_NON_STORING */

#if CONTIKI_TARGET_Z1
#include "dev/uart0.h"
#else
#include "dev/uart1.h"
#endif

#define UDP_PORT 1234
#define SERVICE_ID 190
#define IMIN 16 /* ticks */
#define IMAX 10 /* doublings */
#define REDUNDANCY_CONST 2
#define NSAMPLES 3
#define TRICKLE_PROTO_PORT 30001

struct sample
{
  int value;
  int index;
  int interval;
};

struct trickle_packet
{
  uint8_t token;
  int node;
  int interval;
};

static struct simple_udp_connection unicast_connection;
static struct uip_udp_conn *server_conn;
static struct uip_udp_conn *trickle_conn;

static struct etimer et;
static struct trickle_timer tt;
static int node = 0, interval = 0;
static uip_ipaddr_t prefix, ipaddr;
static uint8_t prefix_set;
static struct trickle_packet packet;

/*---------------------------------------------------------------------------*/
PROCESS(unicast_receiver_process, "Unicast Receiver Process");
PROCESS(border_router_process, "Border Router Process");
PROCESS(webserver_nogui_process, "Web server");
/*---------------------------------------------------------------------------*/
static void tcpip_handler(void) {
  if (uip_newdata()) {
    struct trickle_packet data = ((struct trickle_packet *)uip_appdata)[0];
    PRINTF("At %lu (I=%lu, c=%u): ", (unsigned long)clock_time(), (unsigned long)tt.i_cur, tt.c);
    PRINTF("Our token=0x%02x, theirs=0x%02x\n", packet.token, data.token);

    if (packet.token == data.token) {
      PRINTF("Consistent RX\n");
      trickle_timer_consistency(&tt);
    }
    else {
      if ((signed char)(packet.token - data.token) < 0)
      {
        PRINTF("Theirs is newer. Update\n");
        packet.token = data.token;
      }
      else PRINTF("They are behind\n");
      trickle_timer_inconsistency(&tt);

      PRINTF("At %lu: Trickle inconsistency. Scheduled TX for %lu\n", (unsigned long)clock_time(), (unsigned long)(tt.ct.etimer.timer.start + tt.ct.etimer.timer.interval));
    }
  }
  return;
}
/*---------------------------------------------------------------------------*/
static void trickle_tx(void *ptr, uint8_t suppress) {

  struct trickle_timer *loc_tt = (struct trickle_timer *)ptr;
  if (suppress == TRICKLE_TIMER_TX_SUPPRESS) return;

  PRINTF("At %lu (I=%lu, c=%u): ", (unsigned long)clock_time(), (unsigned long)loc_tt->i_cur, loc_tt->c);
  PRINTF("Trickle TX token 0x%02x\n", packet.token);

  /* Destination IP: link-local all-nodes multicast */
  uip_ipaddr_copy(&trickle_conn->ripaddr, &ipaddr);
  uip_udp_packet_send(trickle_conn, &packet, sizeof(packet));

  /* Restore to 'accept incoming from any IP' */
  uip_create_unspecified(&trickle_conn->ripaddr);
}
/*---------------------------------------------------------------------------*/

#if WEBSERVER == 0
/* No webserver */
AUTOSTART_PROCESSES(&border_router_process, &unicast_receiver_process);
#elif WEBSERVER > 1
/* Use an external webserver application */
#include "webserver-nogui.h"
AUTOSTART_PROCESSES(&border_router_process, &webserver_nogui_process, &unicast_receiver_process);
#else
/* Use simple webserver with only one page for minimum footprint.
 * Multiple connections can result in interleaved tcp segments since
 * a single static buffer is used for all segments.
 */
#include "httpd-simple.h"
/* The internal webserver can provide additional information if
 * enough program flash is available.
 */
#define WEBSERVER_CONF_LOADTIME 0
#define WEBSERVER_CONF_FILESTATS 0
#define WEBSERVER_CONF_NEIGHBOR_STATUS 0
/* Adding links requires a larger RAM buffer. To avoid static allocation
 * the stack can be used for formatting; however tcp retransmissions
 * and multiple connections can result in garbled segments.
 * TODO:use PSOCk_GENERATOR_SEND and tcp state storage to fix this.
 */
#define WEBSERVER_CONF_ROUTE_LINKS 0
#if WEBSERVER_CONF_ROUTE_LINKS
#define BUF_USES_STACK 1
#endif


PROCESS_THREAD(webserver_nogui_process, ev, data)
{
  PROCESS_BEGIN();

  httpd_init();

  while (1)
  {
    PROCESS_WAIT_EVENT_UNTIL(ev == tcpip_event);
    httpd_appcall(data);
  }

  PROCESS_END();
}
AUTOSTART_PROCESSES(&border_router_process, &webserver_nogui_process, &unicast_receiver_process);

static const char *TOP = "<html><head><title>ContikiRPL</title></head><body>\n";
static const char *BOTTOM = "</body></html>\n";
#if BUF_USES_STACK
static char *bufptr, *bufend;
#define ADD(...)                                              \
  do                                                          \
  {                                                           \
    bufptr += snprintf(bufptr, bufend - bufptr, __VA_ARGS__); \
  } while (0)
#else
static char buf[256];
static int blen;
#define ADD(...)                                                   \
  do                                                               \
  {                                                                \
    blen += snprintf(&buf[blen], sizeof(buf) - blen, __VA_ARGS__); \
  } while (0)
#endif
/*---------------------------------------------------------------------------*/
static void
ipaddr_add(const uip_ipaddr_t *addr)
{
  uint16_t a;
  int i, f;
  for (i = 0, f = 0; i < sizeof(uip_ipaddr_t); i += 2)
  {
    a = (addr->u8[i] << 8) + addr->u8[i + 1];
    if (a == 0 && f >= 0)
    {
      if (f++ == 0)
        ADD("::");
    }
    else
    {
      if (f > 0)
      {
        f = -1;
      }
      else if (i > 0)
      {
        ADD(":");
      }
      ADD("%x", a);
    }
  }
}
/*---------------------------------------------------------------------------*/
static PT_THREAD(generate_routes(struct httpd_state *s))
{
  static uip_ds6_route_t *r;
#if RPL_WITH_NON_STORING
  static rpl_ns_node_t *link;
#endif /* RPL_WITH_NON_STORING */
  static uip_ds6_nbr_t *nbr;
#if BUF_USES_STACK
  char buf[256];
#endif
#if WEBSERVER_CONF_LOADTIME
  static clock_time_t numticks;
  numticks = clock_time();
#endif
  PSOCK_BEGIN(&s->sout);

  if (strcmp(s->filename[0], '/') == 0 && strcmp(s->filename[1], 's') == 0 && isdigit(s->filename[2]) && strcmp(s->filename[3], 'n') == 0 && isdigit(s->filename[4]))
  {
    interval = s->filename[2] - '0';
    node = s->filename[4] - '0';
    printf("Interval = '%d' - Node = '%d'\n", interval, node);
  }

  SEND_STRING(&s->sout, TOP);
#if BUF_USES_STACK
  bufptr = buf;
  bufend = bufptr + sizeof(buf);
#else
  blen = 0;
#endif
  char buffer[100];
  snprintf(buffer, 100, "<h5>Change Node [%d] to Interval => NSAMPLEPERIOD%d</h5>", node, interval);
  if (node >= 0 && (interval == 1 || interval == 2))
    ADD(buffer);
  ADD("Neighbors<pre>");

  for (nbr = nbr_table_head(ds6_neighbors);
       nbr != NULL;
       nbr = nbr_table_next(ds6_neighbors, nbr))
  {

#if WEBSERVER_CONF_NEIGHBOR_STATUS
#if BUF_USES_STACK
    {
      char *j = bufptr + 25;
      ipaddr_add(&nbr->ipaddr);
      while (bufptr < j)
        ADD(" ");
      switch (nbr->state)
      {
      case NBR_INCOMPLETE:
        ADD(" INCOMPLETE");
        break;
      case NBR_REACHABLE:
        ADD(" REACHABLE");
        break;
      case NBR_STALE:
        ADD(" STALE");
        break;
      case NBR_DELAY:
        ADD(" DELAY");
        break;
      case NBR_PROBE:
        ADD(" NBR_PROBE");
        break;
      }
    }
#else
    {
      uint8_t j = blen + 25;
      ipaddr_add(&nbr->ipaddr);
      while (blen < j)
        ADD(" ");
      switch (nbr->state)
      {
      case NBR_INCOMPLETE:
        ADD(" INCOMPLETE");
        break;
      case NBR_REACHABLE:
        ADD(" REACHABLE");
        break;
      case NBR_STALE:
        ADD(" STALE");
        break;
      case NBR_DELAY:
        ADD(" DELAY");
        break;
      case NBR_PROBE:
        ADD(" NBR_PROBE");
        break;
      }
    }
#endif
#else
    ipaddr_add(&nbr->ipaddr);
#endif

    ADD("\n");
#if BUF_USES_STACK
    if (bufptr > bufend - 45)
    {
      SEND_STRING(&s->sout, buf);
      bufptr = buf;
      bufend = bufptr + sizeof(buf);
    }
#else
    if (blen > sizeof(buf) - 45)
    {
      SEND_STRING(&s->sout, buf);
      blen = 0;
    }
#endif
  }
  ADD("</pre>Routes<pre>\n");
  SEND_STRING(&s->sout, buf);
#if BUF_USES_STACK
  bufptr = buf;
  bufend = bufptr + sizeof(buf);
#else
  blen = 0;
#endif

  for (r = uip_ds6_route_head(); r != NULL; r = uip_ds6_route_next(r))
  {

#if BUF_USES_STACK
#if WEBSERVER_CONF_ROUTE_LINKS
    ADD("<a href=http://[");
    ipaddr_add(&r->ipaddr);
    ADD("]/status.shtml>");
    ipaddr_add(&r->ipaddr);
    ADD("</a>");
#else
    ipaddr_add(&r->ipaddr);
#endif
#else
#if WEBSERVER_CONF_ROUTE_LINKS
    ADD("<a href=http://[");
    ipaddr_add(&r->ipaddr);
    ADD("]/status.shtml>");
    SEND_STRING(&s->sout, buf); //TODO: why tunslip6 needs an output here, wpcapslip does not
    blen = 0;
    ipaddr_add(&r->ipaddr);
    ADD("</a>");
#else
    ipaddr_add(&r->ipaddr);
#endif
#endif
    ADD("/%u (via ", r->length);
    ipaddr_add(uip_ds6_route_nexthop(r));
    if (1 || (r->state.lifetime < 600))
    {
      ADD(") %lus\n", (unsigned long)r->state.lifetime);
    }
    else
    {
      ADD(")\n");
    }
    SEND_STRING(&s->sout, buf);
#if BUF_USES_STACK
    bufptr = buf;
    bufend = bufptr + sizeof(buf);
#else
    blen = 0;
#endif
  }
  ADD("</pre>");

#if RPL_WITH_NON_STORING
  ADD("Links<pre>\n");
  SEND_STRING(&s->sout, buf);
#if BUF_USES_STACK
  bufptr = buf;
  bufend = bufptr + sizeof(buf);
#else
  blen = 0;
#endif
  for (link = rpl_ns_node_head(); link != NULL; link = rpl_ns_node_next(link))
  {
    if (link->parent != NULL)
    {
      uip_ipaddr_t child_ipaddr;
      uip_ipaddr_t parent_ipaddr;

      rpl_ns_get_node_global_addr(&child_ipaddr, link);
      rpl_ns_get_node_global_addr(&parent_ipaddr, link->parent);

#if BUF_USES_STACK
#if WEBSERVER_CONF_ROUTE_LINKS
      ADD("<a href=http://[");
      ipaddr_add(&child_ipaddr);
      ADD("]/status.shtml>");
      ipaddr_add(&child_ipaddr);
      ADD("</a>");
#else
      ipaddr_add(&child_ipaddr);
#endif
#else
#if WEBSERVER_CONF_ROUTE_LINKS
      ADD("<a href=http://[");
      ipaddr_add(&child_ipaddr);
      ADD("]/status.shtml>");
      SEND_STRING(&s->sout, buf); //TODO: why tunslip6 needs an output here, wpcapslip does not
      blen = 0;
      ipaddr_add(&child_ipaddr);
      ADD("</a>");
#else
      ipaddr_add(&child_ipaddr);
#endif
#endif

      ADD(" (parent: ");
      ipaddr_add(&parent_ipaddr);
      if (1 || (link->lifetime < 600))
      {
        ADD(") %us\n", (unsigned int)link->lifetime); // iotlab printf does not have %lu
        //ADD(") %lus\n", (unsigned long)r->state.lifetime);
      }
      else
      {
        ADD(")\n");
      }
      SEND_STRING(&s->sout, buf);
#if BUF_USES_STACK
      bufptr = buf;
      bufend = bufptr + sizeof(buf);
#else
      blen = 0;
#endif
    }
  }
  ADD("</pre>");
#endif /* RPL_WITH_NON_STORING */

#if WEBSERVER_CONF_FILESTATS
  static uint16_t numtimes;
  ADD("<br><i>This page sent %u times</i>", ++numtimes);
#endif

#if WEBSERVER_CONF_LOADTIME
  numticks = clock_time() - numticks + 1;
  ADD(" <i>(%u.%02u sec)</i>",numticks/CLOCK_SECOND,(100*(numticks%CLOCK_SECOND))/CLOCK_SECOND));
#endif

  SEND_STRING(&s->sout, buf);
  SEND_STRING(&s->sout, BOTTOM);

  //=============================================
  // Aquí el Border Route decide la actualización del token
  packet.node = node;
  packet.interval = interval;
  packet.token++;
  printf("At %lu: Generating a new token 0x%02x\n", (unsigned long)clock_time(), packet.token);
  trickle_timer_reset_event(&tt);

  //=============================================

  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
httpd_simple_script_t
httpd_simple_get_script(const char *name)
{
  return generate_routes;
}

#endif /* WEBSERVER */

/*---------------------------------------------------------------------------*/
static void print_local_addresses(void)
{
  int i;
  uint8_t state;

  PRINTA("Server IPv6 addresses:\n");
  for (i = 0; i < UIP_DS6_ADDR_NB; i++)
  {
    state = uip_ds6_if.addr_list[i].state;
    if (uip_ds6_if.addr_list[i].isused &&
        (state == ADDR_TENTATIVE || state == ADDR_PREFERRED))
    {
      PRINTA(" ");
      uip_debug_ipaddr_print(&uip_ds6_if.addr_list[i].ipaddr);
      PRINTA("\n");
    }
  }
}
/*---------------------------------------------------------------------------*/
void request_prefix(void)
{
  uip_buf[0] = '?';
  uip_buf[1] = 'P';
  uip_len = 2;
  slip_send();
  uip_len = 0;
}
/*---------------------------------------------------------------------------*/
static void create_rpl_dag(uip_ipaddr_t *ipaddr);
void set_prefix_64(uip_ipaddr_t *prefix_64)
{
  rpl_dag_t *dag;
  uip_ipaddr_t ipaddr;
  memcpy(&prefix, prefix_64, 16);
  memcpy(&ipaddr, prefix_64, 16);
  prefix_set = 1;
  uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);
  
  dag = rpl_set_root(RPL_DEFAULT_INSTANCE, &ipaddr);
  if (dag != NULL)
  {
    rpl_set_prefix(dag, &prefix, 64);
    PRINTF("created a new RPL dag\n");
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(border_router_process, ev, data)
{
  static struct etimer et;
  PROCESS_BEGIN();

  printf("Trickle protocol started\n");
  uip_create_linklocal_allnodes_mcast(&ipaddr);
  
  trickle_conn = udp_new(NULL, UIP_HTONS(TRICKLE_PROTO_PORT), NULL);
  udp_bind(trickle_conn, UIP_HTONS(TRICKLE_PROTO_PORT));

  PRINTF("Connection: local/remote port %u/%u\n", UIP_HTONS(trickle_conn->lport), UIP_HTONS(trickle_conn->rport));

  packet.token = 0;
  trickle_timer_config(&tt, IMIN, IMAX, REDUNDANCY_CONST);
  trickle_timer_set(&tt, trickle_tx, &tt);
  prefix_set = 0;
  NETSTACK_MAC.off(0);
  PROCESS_PAUSE();
  NETSTACK_MAC.off(1);

#if DEBUG || 1
  print_local_addresses();
#endif

  while (1) {
    PROCESS_YIELD();
    if (ev == tcpip_event) tcpip_handler();
  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
static void receiver(struct simple_udp_connection *c, const uip_ipaddr_t *sender_addr, uint16_t sender_port, const uip_ipaddr_t *receiver_addr, uint16_t receiver_port, const struct sample *data, uint16_t datalen)
{
  int i;
  printf("Data received from ");
  uip_debug_ipaddr_print(sender_addr);
  printf(" on port %d from port %d with length %d:\n", receiver_port, sender_port, datalen);

  for (i = 0; i < NSAMPLES; i++)
    printf("\t[Sample %d]: Value = %d | Index = %d | Interval Used = %d\n", i + 1, data[i].value, data[i].index, data[i].interval);
}
/*---------------------------------------------------------------------------*/
static uip_ipaddr_t *set_global_address(void)
{
  static uip_ipaddr_t l_ipaddr;
  int i;
  uint8_t state;

  uip_ip6addr(&l_ipaddr, 0xaaaa, 0, 0, 0, 0, 0, 0, 0);
  uip_ds6_set_addr_iid(&l_ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&l_ipaddr, 0, ADDR_AUTOCONF);

  printf("IPv6 addresses: ");
  for (i = 0; i < UIP_DS6_ADDR_NB; i++)
  {
    state = uip_ds6_if.addr_list[i].state;
    if (uip_ds6_if.addr_list[i].isused && (state == ADDR_TENTATIVE || state == ADDR_PREFERRED))
    {
      printf("\n");
      uip_debug_ipaddr_print(&uip_ds6_if.addr_list[i].ipaddr);
    }
  }
  printf("\n");
  return &l_ipaddr;
}
/*---------------------------------------------------------------------------*/
static void create_rpl_dag(uip_ipaddr_t *l_ipaddr)
{
  struct uip_ds6_addr *root_if;
  root_if = uip_ds6_addr_lookup(l_ipaddr);

  if (root_if != NULL)
  {
    rpl_dag_t *dag;
    uip_ipaddr_t prefix;

    rpl_set_root(RPL_DEFAULT_INSTANCE, l_ipaddr);
    dag = rpl_get_any_dag();
    uip_ip6addr(&prefix, 0xaaaa, 0, 0, 0, 0, 0, 0, 0);
    rpl_set_prefix(dag, &prefix, 64);
    printf("created a new RPL dag\n");
  }
  else printf("failed to create a new RPL DAG\n");
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(unicast_receiver_process, ev, data)
{
  uip_ipaddr_t *l_ipaddr;

  PROCESS_BEGIN();

  servreg_hack_init();

  l_ipaddr = set_global_address();

  create_rpl_dag(l_ipaddr);

  servreg_hack_register(SERVICE_ID, l_ipaddr);

  simple_udp_register(&unicast_connection, UDP_PORT, NULL, UDP_PORT, receiver);

  while (1)
  {
    PROCESS_WAIT_EVENT();
  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
