/****************************************************************************
 * net/icmpv6/icmpv6_input.c
 * Handling incoming ICMPv6 input
 *
 *   Copyright (C) 2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Adapted for NuttX from logic in uIP which also has a BSD-like license:
 *
 *   Original author Adam Dunkels <adam@dunkels.com>
 *   Copyright () 2001-2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#ifdef CONFIG_NET

#include <stdint.h>
#include <string.h>
#include <debug.h>

#include <net/if.h>
#include <arpa/inet.h>

#include <nuttx/net/netconfig.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netstats.h>
#include <nuttx/net/ip.h>

#include "devif/devif.h"
#include "utils/utils.h"
#include "neighbor/neighbor.h"
#include "icmpv6/icmpv6.h"

#ifdef CONFIG_NET_ICMPv6

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ETHBUF    ((struct eth_hdr_s *)&dev->d_buf[0])
#define ICMPv6BUF ((struct icmpv6_iphdr_s *)&dev->d_buf[NET_LL_HDRLEN(dev)])

/****************************************************************************
 * Public Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

#ifdef CONFIG_NET_ICMPv6v6_PING
FAR struct devif_callback_s *g_echocallback = NULL;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: icmpv6_input
 *
 * Description:
 *   Handle incoming ICMPv6 input
 *
 * Parameters:
 *   dev - The device driver structure containing the received ICMPv6
 *         packet
 *
 * Return:
 *   None
 *
 * Assumptions:
 *   Called from the interrupt level or with interrupts disabled.
 *
 ****************************************************************************/

void icmpv6_input(FAR struct net_driver_s *dev)
{
  FAR struct icmpv6_iphdr_s *icmp = ICMPv6BUF;

#ifdef CONFIG_NET_STATISTICS
  g_netstats.icmpv6.recv++;
#endif

  /* Set a bit in the d_flags to distinguish this from an IPv6 packet */

  IFF_SET_IPv6(dev->d_flags);

  /* If we get a neighbor solicitation for our address we should send
   * a neighbor advertisement message back.
   */

  if (icmp->type == ICMPv6_NEIGHBOR_SOLICIT)
    {
      /* If the data matches our address, add the neighbor to the list
       * of neighbors.
       */

      if (net_ipv6addr_cmp(icmp->icmpv6data, dev->d_ipv6addr))
        {
          if (icmp->options[0] == ICMPv6_OPTION_SOURCE_LINK_ADDRESS)
            {
              /* Save the sender's address in our neighbor list. */

              net_neighbor_add(icmp->srcipaddr,
                               (FAR struct net_neighbor_addr_s *)&(icmp->options[2]));
            }

          /* We should now send a neighbor advertisement back to where the
           * neighbor solicitation came from.
           */

          icmp->type = ICMPv6_NEIGHBOR_ADVERTISE;
          icmp->flags = ICMPv6_FLAG_S; /* Solicited flag. */

          icmp->reserved1 = 0;
          icmp->reserved2 = 0;
          icmp->reserved3 = 0;

          net_ipv6addr_copy(icmp->destipaddr, icmp->srcipaddr);
          net_ipv6addr_copy(icmp->srcipaddr, dev->d_ipv6addr);

          icmp->options[0]   = ICMPv6_OPTION_TARGET_LINK_ADDRESS;
          icmp->options[1]   = 1;  /* Options length, 1 = 8 bytes. */
          memcpy(&(icmp->options[2]), &dev->d_mac, IFHWADDRLEN);

          icmp->icmpv6chksum = 0;
          icmp->icmpv6chksum = ~icmpv6_chksum(dev);

#ifdef CONFIG_NET_ETHERNET
          /* Move the source and to the destination addresses in the
           * Ethernet header and use our MAC as the new source address.
           */

#ifdef CONFIG_NET_MULTILINK
          if (dev->d_lltype == NET_LL_ETHERNET)
#endif
            {
              FAR struct eth_hdr_s *eth = ETHBUF;

              memcpy(eth->dest, eth->src, ETHER_ADDR_LEN);
              memcpy(eth->src, dev->d_mac.ether_addr_octet, ETHER_ADDR_LEN);
            }
#endif
        }
      else
        {
          goto drop;
        }
    }
  else if (icmp->type == ICMPv6_ECHO_REQUEST)
    {
      /* ICMPv6 echo (i.e., ping) processing. This is simple, we only
       * change the ICMPv6 type from ECHO to ECHO_REPLY and update the
       * ICMPv6 checksum before we return the packet.
       */

      icmp->type = ICMPv6_ECHO_REPLY;

      net_ipv6addr_copy(icmp->destipaddr, icmp->srcipaddr);
      net_ipv6addr_copy(icmp->srcipaddr, dev->d_ipv6addr);

      icmp->icmpv6chksum = 0;
      icmp->icmpv6chksum = ~icmpv6_chksum(dev);
    }

  /* If an ICMPv6 echo reply is received then there should also be
   * a thread waiting to received the echo response.
   */

#ifdef CONFIG_NET_ICMPv6v6_PING
  else if (icmp->type == ICMPv6_ECHO_REPLY && g_echocallback)
    {
      uint16_t flags = ICMPv6_ECHOREPLY;

      if (g_echocallback)
        {
          /* Dispatch the ECHO reply to the waiting thread */

          flags = devif_callback_execute(dev, icmp, flags, g_echocallback);
        }

      /* If the ECHO reply was not handled, then drop the packet */

      if (flags == ICMPv6_ECHOREPLY)
        {
          /* The ECHO reply was not handled */

          goto drop;
        }
    }
#endif

  else
    {
      nlldbg("Unknown ICMPv6 cmd: %d\n", icmp->type);
      goto typeerr;
    }

  nllvdbg("Outgoing ICMPv6 packet length: %d (%d)\n",
          dev->d_len, (icmp->len[0] << 8) | icmp->len[1]);

#ifdef CONFIG_NET_STATISTICS
  g_netstats.icmpv6.sent++;
  g_netstats.ip.sent++;
#endif
  return;

typeerr:
#ifdef CONFIG_NET_STATISTICS
  g_netstats.icmpv6.typeerr++;
#endif

drop:
#ifdef CONFIG_NET_STATISTICS
  g_netstats.icmpv6.drop++;
#endif
  dev->d_len = 0;
}

#endif /* CONFIG_NET_ICMPv6 */
#endif /* CONFIG_NET */