#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"
#include "lib/random.h"
#include "lib/trickle-timer.h"
#include "net/ip/uip.h"
#include "net/ip/uip-debug.h"
#include "net/ipv6/uip-ds6.h"
#include "sys/ctimer.h"
#include "sys/etimer.h"
#include "sys/node-id.h"

#include "node-id.h"
#include "servreg-hack.h"
#include "simple-udp.h"

#include <stdio.h>
#include <string.h>

#define UDP_PORT 1234
#define SERVICE_ID 190
#define IMIN 16 /* ticks */
#define IMAX 10 /* doublings */
#define REDUNDANCY_CONST 2
#define NSAMPLES 3
#define NSAMPLEPERIOD1 300
#define NSAMPLEPERIOD2 600
#define TRICKLE_PROTO_PORT 30001

static struct collect_conn tc;
static struct simple_udp_connection unicast_connection;
static struct uip_udp_conn *trickle_conn;

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

static struct etimer et;
static struct trickle_timer tt;
static struct trickle_packet packet;
static int sample_interval = NSAMPLEPERIOD1;
static uip_ipaddr_t ipaddr;
static int interval_changed = 0;

/*---------------------------------------------------------------------------*/
PROCESS(unicast_sender_process, "Unicast Sender Process");
PROCESS(trickle_protocol_process, "Trickle Protocol Process");
AUTOSTART_PROCESSES(&trickle_protocol_process, &unicast_sender_process);
/*---------------------------------------------------------------------------*/
static void
tcpip_handler(void)
{
  if (uip_newdata())
  {
    struct trickle_packet data = ((struct trickle_packet *)uip_appdata)[0];
    PRINTF("At %lu (I=%lu, c=%u): ", (unsigned long)clock_time(), (unsigned long)tt.i_cur, tt.c);
    PRINTF("Our token=0x%02x, theirs=0x%02x\n", packet.token, data.token);
    if (packet.token == data.token)
    {
      PRINTF("Consistent RX\n");
      trickle_timer_consistency(&tt);
    }
    else
    {
      if ((signed char)(packet.token - data.token) < 0)
      {
        PRINTF("Theirs is newer. Update\n");
        packet.token = data.token;

        if (data.node == node_id)
        {
          if (sample_interval == NSAMPLEPERIOD1)
            interval_changed = 1;
          else if (sample_interval == 2)
            interval_changed = 2;
          if (data.interval == NSAMPLEPERIOD2)
            sample_interval = NSAMPLEPERIOD1;
          else if (data.interval == 2)
            sample_interval = NSAMPLEPERIOD2;
          PRINTF("Change Node [%d]'s Interval => %d\n", node_id, sample_interval);
        }
      }
      else  PRINTF("They are behind\n");

      trickle_timer_inconsistency(&tt);
      PRINTF("At %lu: Trickle inconsistency. Scheduled TX for %lu\n", (unsigned long)clock_time(), (unsigned long)(tt.ct.etimer.timer.start + tt.ct.etimer.timer.interval));
    }
  }
  return;
}
/*---------------------------------------------------------------------------*/
static void trickle_tx(void *ptr, uint8_t suppress)
{
  /* *ptr is a pointer to the trickle_timer that triggered this callback. In
   * his example we know that ptr points to tt. However, we pretend that we did
   * not know (which would be the case if we e.g. had multiple trickle timers)
   * and cast it to a local struct trickle_timer* */
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
PROCESS_THREAD(trickle_protocol_process, ev, data)
{
  PROCESS_BEGIN();

  PRINTF("Trickle protocol started\n");

  uip_create_linklocal_allnodes_mcast(&ipaddr);

  trickle_conn = udp_new(NULL, UIP_HTONS(TRICKLE_PROTO_PORT), NULL);
  udp_bind(trickle_conn, UIP_HTONS(TRICKLE_PROTO_PORT));

  PRINTF("Connection: local/remote port %u/%u\n", UIP_HTONS(trickle_conn->lport), UIP_HTONS(trickle_conn->rport));

  packet.token = 0;

  trickle_timer_config(&tt, IMIN, IMAX, REDUNDANCY_CONST);
  trickle_timer_set(&tt, trickle_tx, &tt);
  while (1)
  {
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
  printf("Data received on port %d from port %d with length %d\n", receiver_port, sender_port, datalen);
}
/*---------------------------------------------------------------------------*/
static void set_global_address(void)
{
  uip_ipaddr_t l_ipaddr;
  int i;
  uint8_t state;

  uip_ip6addr(&l_ipaddr, 0xaaaa, 0, 0, 0, 0, 0, 0, 0);
  uip_ds6_set_addr_iid(&l_ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&l_ipaddr, 0, ADDR_AUTOCONF);

  printf("IPv6 addresses: ");
  for (i = 0; i < UIP_DS6_ADDR_NB; i++)
  {
    state = uip_ds6_if.addr_list[i].state;
    if (uip_ds6_if.addr_list[i].isused &&
        (state == ADDR_TENTATIVE || state == ADDR_PREFERRED))
    {
      uip_debug_ipaddr_print(&uip_ds6_if.addr_list[i].ipaddr);
      printf("\n");
    }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(unicast_sender_process, ev, data)
{
  static struct etimer periodic;
  static int index_samples = 0;
  static struct sample samples[NSAMPLES];
  uip_ipaddr_t *addr;

  PROCESS_BEGIN();

  servreg_hack_init();

  set_global_address();

  simple_udp_register(&unicast_connection, UDP_PORT, NULL, UDP_PORT, receiver);

  while (1)
  {

    etimer_set(&periodic, CLOCK_SECOND * sample_interval / 2);

    PROCESS_WAIT_UNTIL(etimer_expired(&periodic));

    {
      samples[index_samples % NSAMPLES].value = random_rand() % 50;
      samples[index_samples % NSAMPLES].index = index_samples + 1;
      if (interval_changed != 0)
      {
        samples[index_samples % NSAMPLES].interval = interval_changed;
        interval_changed = 0;
      }
      else if (sample_interval == NSAMPLEPERIOD1)
        samples[index_samples % NSAMPLES].interval = 1;
      else if (sample_interval == NSAMPLEPERIOD2)
        samples[index_samples % NSAMPLES].interval = 2;
      printf("[New Sample]: Value = %d | Index = %d | Interval Used = %d\n", samples[index_samples % NSAMPLES].value, samples[index_samples % NSAMPLES].index, samples[index_samples % NSAMPLES].interval);

      index_samples++;

      if (index_samples % NSAMPLES != 0) continue;

      addr = servreg_hack_lookup(SERVICE_ID);
      if (addr != NULL)
      {
        printf("Sending unicast to ");
        uip_debug_ipaddr_print(addr);
        printf("\n");
        simple_udp_sendto(&unicast_connection, samples, sizeof(samples), addr);
      }
      else printf("Service %d not found\n", SERVICE_ID);
    }
  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
