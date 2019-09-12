/*
 * Copyright (c) 2007, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \file
 *         Example of how the collect primitive works.
 * \author
 *         Adam Dunkels <adam@sics.se>
 */

#include "contiki.h"
#include "lib/random.h"
#include "net/rime/rime.h"
#include "net/rime/collect.h"
#include "dev/leds.h"
#include "dev/button-sensor.h"
#include "lib/random.h"

#include "net/netstack.h"

#include <stdio.h>

#define NSAMPLES 3
#define NSAMPLEPERIOD1 300
#define NSAMPLEPERIOD2 600

static struct collect_conn tc;

struct sample {
  int value;
  int index;
  int interval;
};

/*---------------------------------------------------------------------------*/
PROCESS(example_collect_process, "Test collect process");
AUTOSTART_PROCESSES(&example_collect_process);
/*---------------------------------------------------------------------------*/
static void
recv(const linkaddr_t *originator, uint8_t seqno, uint8_t hops)
{
  int i;
  struct sample* data = (struct sample*) packetbuf_dataptr();

  printf("Sink got message from %d.%d, seqno %d, hops %d: len %d ' ", originator->u8[0], originator->u8[1], seqno, hops, packetbuf_datalen());
   for (i = 0; i < NSAMPLES; i++)
    printf("[Sample %d]: Value = %d | Index = %d | Interval Used = %d ", i, data[i].value, data[i].index, data[i].interval);
  printf("'\n");
}
/*---------------------------------------------------------------------------*/
static const struct collect_callbacks callbacks = { recv };
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(example_collect_process, ev, data)
{
  static struct etimer periodic;
  static struct etimer et;
    
  static int sample_interval = NSAMPLEPERIOD1;
  static int index_samples = 0;
  static struct sample samples[NSAMPLES];

  PROCESS_BEGIN();

  collect_open(&tc, 130, COLLECT_ROUTER, &callbacks);

  if(linkaddr_node_addr.u8[0] == 1 && linkaddr_node_addr.u8[1] == 0) {
	  printf("I am sink\n");
	  collect_set_sink(&tc, 1);
  }

  /* Allow some time for the network to settle. */
  etimer_set(&et, 120 * CLOCK_SECOND);
  PROCESS_WAIT_UNTIL(etimer_expired(&et));

  while (1) {

    etimer_set(&periodic, CLOCK_SECOND * sample_interval);
    etimer_set(&et, CLOCK_SECOND * sample_interval);

    PROCESS_WAIT_UNTIL(etimer_expired(&et));

    {
      samples[index_samples % NSAMPLES].value = random_rand() % 50;
      samples[index_samples % NSAMPLES].index = index_samples + 1;
      if (sample_interval == NSAMPLEPERIOD1)
        samples[index_samples % NSAMPLES].interval = 1;
      else if (sample_interval == NSAMPLEPERIOD2)
        samples[index_samples % NSAMPLES].interval = 2;
      printf("[New Sample]: Value = %d | Index = %d | Interval Used = %d\n", samples[index_samples % NSAMPLES].value, samples[index_samples % NSAMPLES].index, samples[index_samples % NSAMPLES].interval);

      index_samples++;

      if (index_samples % NSAMPLES != 0) {
        PROCESS_WAIT_UNTIL(etimer_expired(&periodic));
        continue;
      }

      printf("Sending\n");
      packetbuf_clear();
      packetbuf_copyfrom(samples, sizeof(samples));
      collect_send(&tc, 15);

      static linkaddr_t oldparent;
      const linkaddr_t *parent;

      parent = collect_parent(&tc);
      if(!linkaddr_cmp(parent, &oldparent)) {
        if(!linkaddr_cmp(&oldparent, &linkaddr_null)) {
          printf("#L %d 0\n", oldparent.u8[0]);
        }
        if(!linkaddr_cmp(parent, &linkaddr_null)) {
          printf("#L %d 1\n", parent->u8[0]);
        }
        linkaddr_copy(&oldparent, parent);
      }
    }
    PROCESS_WAIT_UNTIL(etimer_expired(&periodic));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
