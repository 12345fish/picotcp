/*********************************************************************
   PicoTCP. Copyright (c) 2012-2015 Altran Intelligent Systems. Some rights reserved.
   See LICENSE and COPYING for usage.

   .

   Authors: Daniele Lacamera
 *********************************************************************/

#include "pico_config.h"
#include "pico_tree.h"
#include "pico_icmp6.h"
#include "pico_ipv6.h"
#include "pico_stack.h"
#include "pico_ipv6_nd.h"
#include "pico_ethernet.h"
#include "pico_6lowpan.h"
#include "pico_6lowpan_ll.h"

#ifdef PICO_SUPPORT_IPV6
#define MAX_INITIAL_RTR_ADVERTISEMENTS (3)
#define DEFAULT_METRIC                 (10)

#ifdef DEBUG_IPV6_ND
#define nd_dbg dbg
#else
#define nd_dbg(...) do {} while(0)
#endif

extern struct pico_tree IPV6Links;

#define ONE_MINUTE                          ((pico_time)(1000 * 60))

#ifdef PICO_SUPPORT_6LOWPAN
    #define MAX_RTR_SOLICITATIONS           (3)
    #define RTR_SOLICITATION_INTERVAL       (10000)
    #define MAX_RTR_SOLICITATION_INTERVAL   (60000)
#endif

enum pico_ipv6_neighbor_state {
    PICO_ND_STATE_INCOMPLETE = 0,
    PICO_ND_STATE_REACHABLE,
    PICO_ND_STATE_STALE,
    PICO_ND_STATE_DELAY,
    PICO_ND_STATE_PROBE
};

/**
 * Caches prototypes
 */
struct pico_ipv6_neighbor {
    enum pico_ipv6_neighbor_state state;
    struct pico_ip6 address;
    union pico_hw_addr hwaddr;
    struct pico_device *dev;
    uint16_t is_router;
    uint16_t failure_multi_count;
    uint16_t failure_uni_count;
    uint16_t frames_queued;
    pico_time expire;
};

struct pico_ipv6_router {
    struct pico_ipv6_neighbor *router;
    struct pico_ipv6_link *link;
    int is_default;
    pico_time invalidation;
};

/******************************************************************************
 *  Function prototypes
 ******************************************************************************/

#ifdef PICO_SUPPORT_6LOWPAN
static void pico_6lp_nd_deregister(struct pico_ipv6_link *);
static void pico_6lp_nd_unreachable_gateway(struct pico_ip6 *a);
static int pico_6lp_nd_neigh_adv_process(struct pico_frame *f);
static int neigh_sol_detect_dad_6lp(struct pico_frame *f);
#endif

#ifdef DEBUG_IPV6_ND
static void print_nd_state(struct pico_ipv6_neighbor *n)
{
    if (!n)
    {
        nd_dbg("CAN'T PRINT STATE, NULL NEIGHBOR\n");
        return;
    }

    switch (n->state)
    {
    case PICO_ND_STATE_INCOMPLETE:
        nd_dbg("NB STATE : INCOMPLETE\n");
        break;
    case PICO_ND_STATE_REACHABLE:
        nd_dbg("NB STATE : REACHABLE\n");
        break;
    case PICO_ND_STATE_STALE:
        nd_dbg("NB STATE : STALE\n");
        break;
    case PICO_ND_STATE_DELAY:
        nd_dbg("NB STATE : DELAY\n");
        break;
    case PICO_ND_STATE_PROBE:
        nd_dbg("NB STATE : PROBE\n");
        break;
    default:
        nd_dbg("NB STATE : NOT DEFINED??\n");
        break;
    };

}
#else
#define print_nd_state(n) \
    do{} while (0)
#endif

static int pico_ipv6_neighbor_compare(void *ka, void *kb)
{
    struct pico_ipv6_neighbor *a = ka, *b = kb;
    return pico_ipv6_compare(&a->address, &b->address);
}

static int pico_ipv6_router_compare(void *ka, void *kb)
{
    struct pico_ipv6_router *a = ka, *b = kb;
    return pico_ipv6_neighbor_compare(a->router, b->router);
}

static int pico_ipv6_nd_qcompare(void *ka, void *kb){
    struct pico_frame *a = ka, *b = kb;
    struct pico_ipv6_hdr *a_hdr = NULL, *b_hdr = NULL;
    struct pico_ip6 *a_dest_addr, *b_dest_addr;
    int ret;

    a_hdr = (struct pico_ipv6_hdr *)a->net_hdr;
    b_hdr = (struct pico_ipv6_hdr *)b->net_hdr;

    a_dest_addr = &a_hdr->dst;
    b_dest_addr = &b_hdr->dst;

    ret = pico_ipv6_compare(a_dest_addr, b_dest_addr);

    if(ret){
        return ret;
    }

    if(a->timestamp < b->timestamp){
        return -1;
    }
    if(a->timestamp > b->timestamp){
        return 1;
    }
    return 0;
}

static PICO_TREE_DECLARE(IPV6NQueue, pico_ipv6_nd_qcompare);
static PICO_TREE_DECLARE(NCache, pico_ipv6_neighbor_compare);
static PICO_TREE_DECLARE(RCache, pico_ipv6_router_compare);

static struct pico_ipv6_neighbor *pico_get_neighbor_from_ncache(struct pico_ip6 *dst)
{
    struct pico_ipv6_neighbor test = {
        0
    };

    test.address = *dst;
    return pico_tree_findKey(&NCache, &test);
}

static struct pico_ipv6_router *pico_get_router_from_rcache(struct pico_ip6 *dst)
{
    struct pico_ipv6_router *router = NULL;
    struct pico_ipv6_router test = {
        0
    };

    test.router = PICO_ZALLOC(sizeof(struct pico_ipv6_neighbor));

    if (!test.router)
    {
        dbg("Could not allocate neighbor to get router from rcache\n");
        return NULL;
    }

    test.router->address = *dst;
    router = pico_tree_findKey(&RCache, &test);

    PICO_FREE(test.router);
    return router;
}

static struct pico_ipv6_router *pico_nd_get_default_router(void)
{
  struct pico_tree_node *index, *_tmp;
  struct pico_ipv6_router *tmp = NULL, *ret = NULL;

  pico_tree_foreach_safe(index, &RCache, _tmp)
  {
      tmp = index->keyValue;
      if(tmp->is_default)
      {
          ret = tmp;
          break;
      }
  }

  return ret;
}

static struct pico_ip6 *pico_nd_get_default_router_addr(void)
{
  struct pico_ipv6_router *default_router = NULL;

  default_router = pico_nd_get_default_router();
  if (default_router)
      return &(default_router->router->address);
  else
      return NULL;
}

static void pico_ipv6_assign_default_router(int is_default)
{
    struct pico_tree_node *index, *_tmp;
    struct pico_ipv6_router *r;
    int assigned = 0;
    struct pico_ip6 zero = {
        .addr = {0}
    };
    if(is_default)
    {
      pico_tree_foreach_safe(index, &RCache, _tmp)
      {
          r = index->keyValue;
          if (r->is_default)
          {
            r->is_default = 0;
            if(pico_ipv6_route_del(zero, zero, r->router->address, DEFAULT_METRIC, r->link) != 0) {
                nd_dbg("assign def router: Route could not be deleted\n");
            }
          }
          else if(!assigned)
          {
            if (pico_ipv6_route_add(zero, zero, r->router->address, DEFAULT_METRIC, r->link) != 0) {
                nd_dbg("Route could not be added when assigning new default router\n");
            } else {
                nd_dbg("Route added when assigning new default router\n");
                r->is_default = 1;
                assigned = 1;
            }
          }
      }
    }
}

static void pico_ipv6_router_add_link(struct pico_ip6 *addr, struct pico_ipv6_link *link)
{
    struct pico_tree_node *index, *_tmp;
    struct pico_ipv6_router *r;
    pico_tree_foreach_safe(index, &RCache, _tmp)
    {
        r = index->keyValue;
        if( pico_ipv6_compare(&r->router->address, addr) == 0)
        {
          r->link = link;
          break;
        }
    }
}

static void pico_nd_clear_pending_packets(struct pico_ip6 *dst)
{
    struct pico_tree_node *index = NULL;
    struct pico_frame *frame = NULL;
    struct pico_ipv6_hdr *frame_hdr = NULL;

    pico_tree_foreach(index,&IPV6NQueue) {
        frame = index->keyValue;
        frame_hdr = (struct pico_ipv6_hdr *)frame->net_hdr;
        if (!pico_ipv6_compare(dst, &frame_hdr->dst)) {
            pico_tree_delete(&IPV6NQueue,frame);
            pico_frame_discard(frame);
        }
    }
}

static void pico_ipv6_nd_queued_trigger(struct pico_ip6 *dst)
{
    struct pico_tree_node *index = NULL;
    struct pico_frame *frame = NULL;
    struct pico_ipv6_hdr *frame_hdr = NULL;
    struct pico_ipv6_neighbor *n = NULL;
    struct pico_ip6 frame_dst;

    n = pico_get_neighbor_from_ncache(dst);

    pico_tree_foreach(index,&IPV6NQueue){
        frame = index->keyValue;
        frame_hdr = (struct pico_ipv6_hdr *)frame->net_hdr;
        frame_dst = frame_hdr->dst;

        if(!pico_ipv6_is_linklocal(frame_dst.addr)) {
          frame_dst = pico_ipv6_route_get_gateway(&frame_dst);
        }

        if(pico_ipv6_is_unspecified(frame_dst.addr)){
          frame_dst = frame_hdr->dst;
        }
        if(pico_ipv6_compare(dst, &frame_dst) == 0){
          if(n) {
            n->frames_queued--;
          }

          if (pico_datalink_send(frame) <= 0) {
              pico_frame_discard(frame);
          }

          pico_tree_delete(&IPV6NQueue,frame);
        }
    }
}

static void ipv6_duplicate_detected(struct pico_ipv6_link *l)
{
    struct pico_device *dev;
    int is_ll = pico_ipv6_is_linklocal(l->address.addr);
    dev = l->dev;
    nd_dbg("IPV6: Duplicate address detected. Removing link.\n");
    pico_ipv6_link_del(l->dev, l->address);
#ifdef PICO_SUPPORT_6LOWPAN
    if (PICO_DEV_IS_6LOWPAN(l->dev)) {
        pico_6lp_nd_deregister(l);
    }
#endif
    if (is_ll)
        pico_device_ipv6_random_ll(dev);
}

static struct pico_ipv6_neighbor *pico_nd_add(struct pico_ip6 *addr, struct pico_device *dev)
{
    struct pico_ipv6_neighbor *n;
    char address[120];
    /* Create a new NCE */
    n = PICO_ZALLOC(sizeof(struct pico_ipv6_neighbor));
    if (!n)
        return NULL;
    pico_ipv6_to_string(address, addr->addr);
    memcpy(&n->address, addr, sizeof(struct pico_ip6));
    n->dev = dev;
    n->frames_queued = 0;
    n->state = PICO_ND_STATE_INCOMPLETE;
    n->expire = PICO_TIME_MS() + ONE_MINUTE;

    if (pico_tree_insert(&NCache, n)) {
        nd_dbg("IPv6 ND: Failed to insert neigbor in tree\n");
        PICO_FREE(n);
        return NULL;
    }
    return n;
}

static void pico_ipv6_nd_unreachable(struct pico_ip6 *a)
{
    struct pico_frame *f;
    struct pico_ipv6_hdr *hdr;
    struct pico_tree_node *index = NULL;
    struct pico_ip6 dst;

#ifdef PICO_SUPPORT_6LOWPAN
    /* 6LP: Find any 6LoWPAN-hosts for which this address might have been a default gateway.
     * If such a host found, send a router solicitation again */
    pico_6lp_nd_unreachable_gateway(a);
#endif /* PICO_SUPPORT_6LOWPAN */

    pico_tree_foreach(index,&IPV6NQueue){
      f = index->keyValue;
      hdr = (struct pico_ipv6_hdr *) f->net_hdr;
      dst = pico_ipv6_route_get_gateway(&hdr->dst);
      if(pico_ipv6_is_unspecified(dst.addr)){
          dst = hdr->dst;
      }

      if (memcmp(dst.addr,a->addr,PICO_SIZE_IP6) == 0){
          if(!pico_source_is_local(f)){
              pico_notify_dest_unreachable(f);
          }
          pico_tree_delete(&IPV6NQueue,f);
          pico_frame_discard(f);
      }
    }
}

static void pico_nd_delete_entry(struct pico_ipv6_neighbor *n)
{
    struct pico_ipv6_router *r = NULL;

    pico_nd_clear_pending_packets(&n->address);

    /* If it is a router, it should be in the RCache */
    r = pico_get_router_from_rcache(&n->address);

    pico_ipv6_nd_unreachable(&n->address);

    if(r) {
        pico_ipv6_assign_default_router(r->is_default);
        pico_ipv6_router_down(&n->address);
        pico_tree_delete(&RCache, r);

        /* TODO: search for new route */
        /* TODO: new link lifetime? */

        PICO_FREE(r);
    }

    pico_tree_delete(&NCache, n);
    PICO_FREE(n);
}

static void pico_nd_new_expire_time(struct pico_ipv6_neighbor *n)
{
    if (n->state == PICO_ND_STATE_REACHABLE)
        n->expire = PICO_TIME_MS() + PICO_ND_REACHABLE_TIME;
    else if ((n->state == PICO_ND_STATE_DELAY) || (n->state == PICO_ND_STATE_STALE)){
        n->expire = PICO_TIME_MS() + PICO_ND_DELAY_FIRST_PROBE_TIME;
    }
    else {
        n->expire = PICO_TIME_MS() + n->dev->hostvars.retranstime;
    }
}

static void pico_nd_discover(struct pico_ipv6_neighbor *n)
{
    struct pico_ipv6_route *gw = NULL;

#ifdef DEBUG_IPV6_ND
    char *ipv6_addr = PICO_ZALLOC(PICO_IPV6_STRING);
    if (!ipv6_addr) {
        dbg("Could not allocate ipv6 string for gateway debug\n");
    } else {
        pico_ipv6_to_string(ipv6_addr, n->address.addr);
        nd_dbg("Sending NS for %s\n", ipv6_addr);
        PICO_FREE(ipv6_addr);
    }
#endif

    if (!n)
        return;

    gw = pico_ipv6_gateway_by_dev(n->dev);

    if (n->state == PICO_ND_STATE_DELAY) {
        /* We wait for DELAY_FIRST_PROBE_TIME to expire
         * This will set us in state PROBE and this will call pico_nd_discover
         * in the timer_elapsed timer-callback
         */
        return;
    }

    if (n->state == PICO_ND_STATE_INCOMPLETE) {
      if (++n->failure_multi_count > PICO_ND_MAX_MULTICAST_SOLICIT){
          return;
      }

      pico_icmp6_neighbor_solicitation(n->dev, &n->address, PICO_ICMP6_ND_SOLICITED, &gw->gateway);
      nd_dbg("NS solicited for %s\n", ipv6_addr);
      print_nd_state(n);
    } else {
      if (++n->failure_uni_count > PICO_ND_MAX_UNICAST_SOLICIT){
          return;
      }
      pico_icmp6_neighbor_solicitation(n->dev, &n->address, PICO_ICMP6_ND_UNICAST, &gw->gateway);
      nd_dbg("NS unicast for %s\n", ipv6_addr);
      print_nd_state(n);
    }

    pico_nd_new_expire_time(n);
}

static struct pico_eth *pico_nd_get_neighbor(struct pico_ip6 *addr, struct pico_ipv6_neighbor *n, struct pico_device *dev)
{
    nd_dbg("Finding neighbor %02x:...:%02x\n", addr->addr[0], addr->addr[15]);
    print_nd_state(n);

    if (!n) {
        n = pico_nd_add(addr, dev);
        pico_nd_discover(n);
        return NULL;
    }

    if (n->state == PICO_ND_STATE_INCOMPLETE) {
        /* Make timer callback handle pico_nd_discover */
        pico_nd_new_expire_time(n);
        return NULL;
    }

    if (n->state == PICO_ND_STATE_STALE) {
        n->state = PICO_ND_STATE_DELAY;
        pico_nd_new_expire_time(n);
    }

    if (n->state != PICO_ND_STATE_REACHABLE) {
        pico_nd_discover(n);
    }

    return &n->hwaddr.mac;
}

static struct pico_eth *pico_nd_get(struct pico_ip6 *address, struct pico_device *dev)
{
    struct pico_ip6 gateway = {{0}}, addr = {{0}};

    /* should we use gateway, or is dst local (gateway == 0)? */
    gateway = pico_ipv6_route_get_gateway(address);
    if (memcmp(gateway.addr, PICO_IP6_ANY, PICO_SIZE_IP6) == 0)
        addr = *address;
    else
        addr = gateway;

    return pico_nd_get_neighbor(&addr, pico_get_neighbor_from_ncache(&addr), dev);
}

static int pico_nd_get_length_of_options(struct pico_frame *f, uint8_t **first_option)
{
    int optlen = 0;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;

    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    switch (icmp6_hdr->type) {
    case PICO_ICMP6_ROUTER_SOL:
        optlen = f->transport_len - PICO_ICMP6HDR_ROUTER_SOL_SIZE;
        if (optlen && first_option)
            *first_option = ((uint8_t *)&icmp6_hdr->msg.info.router_sol) + sizeof(struct router_sol_s);
        break;
    case PICO_ICMP6_ROUTER_ADV:
        optlen = f->transport_len - PICO_ICMP6HDR_ROUTER_ADV_SIZE;
        if (optlen && first_option)
            *first_option = ((uint8_t *)&icmp6_hdr->msg.info.router_adv) + sizeof(struct router_adv_s);
        break;
    case PICO_ICMP6_NEIGH_SOL:
        optlen = f->transport_len - PICO_ICMP6HDR_NEIGH_SOL_SIZE;
        if (optlen && first_option)
            *first_option = ((uint8_t *)&icmp6_hdr->msg.info.neigh_sol) + sizeof(struct neigh_sol_s);
        break;
    case PICO_ICMP6_NEIGH_ADV:
        optlen = f->transport_len - PICO_ICMP6HDR_NEIGH_ADV_SIZE;
        if (optlen && first_option)
            *first_option = ((uint8_t *)&icmp6_hdr->msg.info.neigh_adv) + sizeof(struct neigh_adv_s);
        break;
    case PICO_ICMP6_REDIRECT:
        optlen = f->transport_len - PICO_ICMP6HDR_REDIRECT_SIZE;
        if (optlen && first_option)
            *first_option = ((uint8_t *)&icmp6_hdr->msg.info.redirect) + sizeof(struct redirect_s);
        break;
    default:
        optlen = 0;
        nd_dbg("No valid option received for options processing\n");
    }

    if (!optlen && first_option)
        *first_option = NULL;

    return optlen;
}

static int neigh_options(struct pico_frame *f, void *opt, uint8_t expected_opt)
{
    /* RFC 4861 $7.1.2 + $7.2.5.
     *  * The contents of any defined options that are not specified to be used
     *  * with Neighbor Advertisement messages MUST be ignored and the packet
     *  * processed as normal. The only defined option that may appear is the
     *  * Target Link-Layer Address option.
     *  */
    int optlen = 0;
    uint8_t *option = NULL;
    int len = 0;
    uint8_t type = 0;
    int found = 0;

    optlen = pico_nd_get_length_of_options(f, &option);

    while (optlen > 0) {
        type = ((struct pico_icmp6_opt_na *)option)->type;
        len = ((struct pico_icmp6_opt_na *)option)->len;
        optlen -= len << 3; /* len in units of 8 octets */
        if (len <= 0)
            return -1; /* malformed option. */

        if (type == expected_opt) {
            if (found > 0)
                return -1; /* malformed option: option is there twice. */

            if (expected_opt == PICO_ND_OPT_REDIRECT) {
                memcpy(opt, option, sizeof(struct pico_icmp6_opt_redirect));
            } else {
                memcpy(opt, option, (size_t)(len << 3));
            }
            found++;
        }

        if (optlen > 0) {
            option += len << 3;
        } else { /* parsing options: terminated. */
            return found;
        }
    }

    return found;
}

static size_t pico_hw_addr_len(struct pico_device *dev, struct pico_icmp6_opt_lladdr *opt)
{
    size_t len = PICO_SIZE_ETH;
#ifndef PICO_SUPPORT_6LOWPAN
    IGNORE_PARAMETER(dev);
    IGNORE_PARAMETER(opt);
#else
    if (PICO_DEV_IS_6LOWPAN(dev)) {
        if (1 == opt->len) {
            len = (size_t)SIZE_6LOWPAN_SHORT;
        } else {
            len = (size_t)SIZE_6LOWPAN_EXT;
        }
    }
#endif
    return len;
}

static void pico_ipv6_neighbor_update(struct pico_ipv6_neighbor *n, struct pico_icmp6_opt_lladdr *opt, struct pico_device *dev)
{
    memcpy(n->hwaddr.data, opt->addr.data, pico_hw_addr_len(dev, opt));
}

static int pico_ipv6_neighbor_compare_stored(struct pico_ipv6_neighbor *n, struct pico_icmp6_opt_lladdr *opt, struct pico_device *dev)
{
    return memcmp(n->hwaddr.data, opt->addr.data, pico_hw_addr_len(dev, opt));
}

static void neigh_adv_reconfirm_router_option(struct pico_ipv6_neighbor *n, unsigned int isRouter)
{
    struct pico_tree_node *index = NULL, *_tmp = NULL;
    struct pico_ipv6_router *r;
    if (!isRouter && n->is_router) {
        pico_ipv6_router_down(&n->address);
        pico_tree_foreach_safe(index, &RCache, _tmp)
        {
          r = index->keyValue;
          if(r->router == n)
          {
            pico_ipv6_assign_default_router(r->is_default);
            pico_tree_delete(&RCache, r);
            break;
          }
        }
    }

    if (isRouter)
        n->is_router = 1;
    else
        n->is_router = 0;
}


static int neigh_adv_reconfirm_no_tlla(struct pico_ipv6_neighbor *n, struct pico_icmp6_hdr *hdr)
{
    if (IS_SOLICITED(hdr)) {
        n->state = PICO_ND_STATE_REACHABLE;
        n->failure_uni_count = 0;
        n->failure_multi_count = 0;
        pico_ipv6_nd_queued_trigger(&n->address);
        pico_nd_new_expire_time(n);
        return 0;
    }
    return -1;
}


static int neigh_adv_reconfirm(struct pico_ipv6_neighbor *n, struct pico_icmp6_opt_lladdr *opt, struct pico_icmp6_hdr *hdr, struct pico_device *dev)
{

    if (IS_SOLICITED(hdr) && !IS_OVERRIDE(hdr) && (pico_ipv6_neighbor_compare_stored(n, opt, dev) == 0)) {
        n->state = PICO_ND_STATE_REACHABLE;
        n->failure_uni_count = 0;
        n->failure_multi_count = 0;
        pico_ipv6_nd_queued_trigger(&n->address);
        pico_nd_new_expire_time(n);
        return 0;
    }

    if ((n->state == PICO_ND_STATE_REACHABLE) && IS_SOLICITED(hdr) && !IS_OVERRIDE(hdr)) {
        n->state = PICO_ND_STATE_STALE;
        return 0;
    }

    if (IS_SOLICITED(hdr) && IS_OVERRIDE(hdr)) {
        pico_ipv6_neighbor_update(n, opt, dev);
        n->state = PICO_ND_STATE_REACHABLE;
        n->failure_uni_count = 0;
        n->failure_multi_count = 0;
        pico_ipv6_nd_queued_trigger(&n->address);
        pico_nd_new_expire_time(n);
        return 0;
    }

    if (!IS_SOLICITED(hdr) && IS_OVERRIDE(hdr) && (pico_ipv6_neighbor_compare_stored(n, opt, dev) != 0)) {
        pico_ipv6_neighbor_update(n, opt, dev);
        n->state = PICO_ND_STATE_STALE;
        pico_ipv6_nd_queued_trigger(&n->address);
        pico_nd_new_expire_time(n);
        return 0;
    }

    if ((n->state == PICO_ND_STATE_REACHABLE) && (!IS_SOLICITED(hdr)) && (!IS_OVERRIDE(hdr)) &&
        (pico_ipv6_neighbor_compare_stored(n, opt, dev) != 0)) {

        /* I.  If the Override flag is clear and the supplied link-layer address
         *     differs from that in the cache, then one of two actions takes
         *     place:
         *     a. If the state of the entry is REACHABLE, set it to STALE, but
         *        do not update the entry in any other way.
         *     b. Otherwise, the received advertisement should be ignored and
         *        MUST NOT update the cache.
         */
        n->state = PICO_ND_STATE_STALE;
        pico_nd_new_expire_time(n);
        return 0;
    }

    return -1;
}

static void neigh_adv_process_incomplete(struct pico_ipv6_neighbor *n, struct pico_frame *f, struct pico_icmp6_opt_lladdr *opt)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    if (!n || !f)
        return;

    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    if (!icmp6_hdr)
        return;

    if (IS_SOLICITED(icmp6_hdr)) {
        n->state = PICO_ND_STATE_REACHABLE;
        n->failure_multi_count = 0;
        n->failure_uni_count = 0;
        pico_nd_new_expire_time(n);
    } else {
        n->state = PICO_ND_STATE_STALE;
    }

    if (opt)
        pico_ipv6_neighbor_update(n, opt, n->dev);

    pico_ipv6_nd_queued_trigger(&n->address);
}


static int neigh_adv_process(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_neighbor *n = NULL;
    struct pico_icmp6_opt_lladdr opt = {
        0
    };
    int optres = neigh_options(f, &opt, PICO_ND_OPT_LLADDR_TGT);
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    if (optres < 0) { /* Malformed packet: option field cannot be processed. */
        return -1;
    }

#ifdef PICO_SUPPORT_6LOWPAN
    if (PICO_DEV_IS_6LOWPAN(f->dev)) {
        /* 6LoWPAN: parse Address Registration Comfirmation(nothing on success, remove link on failure) */
        pico_6lp_nd_neigh_adv_process(f);
    }
#endif

    /* Check if there's a NCE in the cache */
    n = pico_get_neighbor_from_ncache(&icmp6_hdr->msg.info.neigh_adv.target);

    if (!n) {
        return 0;
    }

    if ((optres == 0) || IS_OVERRIDE(icmp6_hdr) || (pico_ipv6_neighbor_compare_stored(n, &opt, f->dev) == 0)) {
        neigh_adv_reconfirm_router_option(n, IS_ROUTER(icmp6_hdr));
    }

    if ((optres > 0) && (n->state == PICO_ND_STATE_INCOMPLETE)) {
        neigh_adv_process_incomplete(n, f, &opt);
        return 0;
    }

    if (optres > 0)
        return neigh_adv_reconfirm(n, &opt, icmp6_hdr, f->dev);
    else
        return neigh_adv_reconfirm_no_tlla(n, icmp6_hdr);

}

static struct pico_ipv6_neighbor *pico_ipv6_neighbor_from_sol_new(struct pico_ip6 *ip, struct pico_icmp6_opt_lladdr *opt, struct pico_device *dev)
{
    size_t len = pico_hw_addr_len(dev, opt);
    struct pico_ipv6_neighbor *n = NULL;
    n = pico_nd_add(ip, dev);
    if (!n)
        return NULL;

    memcpy(n->hwaddr.data, opt->addr.data, len);
    memset(n->hwaddr.data + len, 0, sizeof(union pico_hw_addr) - len);
    n->state = PICO_ND_STATE_STALE;
    pico_nd_new_expire_time(n);
    pico_ipv6_nd_queued_trigger(ip);
    return n;
}

static void pico_ipv6_neighbor_from_unsolicited(struct pico_frame *f)
{
    struct pico_ipv6_neighbor *n = NULL;
    struct pico_icmp6_opt_lladdr opt = {
        0
    };
    struct pico_ipv6_hdr *ip = (struct pico_ipv6_hdr *)f->net_hdr;
    int valid_lladdr = neigh_options(f, &opt, PICO_ND_OPT_LLADDR_SRC);

    if (!pico_ipv6_is_unspecified(ip->src.addr)) {
        n = pico_get_neighbor_from_ncache(&ip->src);
        if (!n) {
            /* NO NCE */
            if (valid_lladdr > 0) {
                nd_dbg("NB FROM UNSOLICITED\n");
                n = pico_ipv6_neighbor_from_sol_new(&ip->src, &opt, f->dev);
            } else {
                n = pico_nd_add(&ip->src, f->dev);

                if (n) {
                    nd_dbg("NB FROM UNSOLICITED: added\n");
                    n->state = PICO_ND_STATE_INCOMPLETE;
                } else {
                    nd_dbg("NB FROM UNSOLICITED: could not add neighbor, aborting route update\n");
                    return;
                }
            }
        } else if (pico_ipv6_neighbor_compare_stored(n, &opt, f->dev) != 0) {
            /* NCE exists but different LL addr */
            nd_dbg("NB FROM UNSOLICITED: diff ll addr\n");
            pico_ipv6_neighbor_update(n, &opt, n->dev);
            n->state = PICO_ND_STATE_STALE;
            pico_ipv6_nd_queued_trigger(&n->address);
            pico_nd_new_expire_time(n);
        } else {
            /* NCE exists with same LL addr */
            nd_dbg("NB FROM UNSOLICITED: SAME LL ADDR\n");
        }

        if (!n)
            return;

        n->failure_uni_count = 0;
        n->failure_multi_count = 0;
    }
}

static void pico_ipv6_router_from_unsolicited(struct pico_frame *f)
{
    struct pico_ipv6_neighbor *n = NULL;
    struct pico_ipv6_router *r = NULL;
    struct router_adv_s *r_adv_hdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    struct pico_ipv6_hdr *ip = (struct pico_ipv6_hdr *)f->net_hdr;

    if (!pico_ipv6_is_unspecified(ip->src.addr)) {
        r_adv_hdr = &icmp6_hdr->msg.info.router_adv;
        n = pico_get_neighbor_from_ncache(&ip->src);

        if (!n) {
            /* TODO:  */
        }

        /* Indicate he is a router, TODO: don't set it always! */
        n->is_router = 1;

        /* TODO: set correct expire time? */

        r = pico_get_router_from_rcache(&ip->src);

        if (!r) {
            r = PICO_ZALLOC(sizeof(struct pico_ipv6_router));
            r->router = n;
            if (pico_tree_insert(&RCache, r)) {
                nd_dbg("Could not insert router in rcache\n");
                PICO_FREE(r);
                return;
            }
        }

        if(r_adv_hdr->life_time != 0) {
            r->invalidation = PICO_TIME_MS() + (pico_time)(r_adv_hdr->life_time * 1000);
        } else {
            /* TODO: WHAT IF 0? */
            pico_ipv6_assign_default_router(r->is_default);
            pico_tree_delete(&RCache, r);
        }
    }
}
static int neigh_sol_detect_dad(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_ipv6_link *link = NULL;
    ipv6_hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    if (!f->dev->mode) {
        link = pico_ipv6_link_istentative(&icmp6_hdr->msg.info.neigh_adv.target);
        if (link) {
            if (pico_ipv6_is_unicast(&ipv6_hdr->src))
            {
                /* RFC4862 5.4.3 : sender is performing address resolution,
                 * our address is not yet valid, discard silently.
                 */
                dbg("DAD:Sender performing AR\n");
            }

            else if (pico_ipv6_is_unspecified(ipv6_hdr->src.addr) &&
                     !pico_ipv6_is_allhosts_multicast(ipv6_hdr->dst.addr))
            {
                /* RFC4862 5.4.3 : sender is performing DaD */
                dbg("DAD:Sender performing DaD\n");
                ipv6_duplicate_detected(link);
            }

            return 0;
        }
    }

    return -1; /* Current link is not tentative */
}

static int neigh_sol_process(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_link *link = NULL;
    int valid_lladdr;
    struct pico_icmp6_opt_lladdr opt = {
        0
    };
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    valid_lladdr = neigh_options(f, &opt, PICO_ND_OPT_LLADDR_SRC);

    if (valid_lladdr < 0)
        return -1; /* Malformed packet. */

    pico_ipv6_neighbor_from_unsolicited(f);

    if (f->dev->mode != LL_MODE_ETHERNET && !valid_lladdr && (0 == neigh_sol_detect_dad(f)))
        return 0;
#ifdef PICO_SUPPORT_6LOWPAN
    else if (PICO_DEV_IS_6LOWPAN(f->dev)) {
        nd_dbg("[6LP-ND] Received Address Registration Option\n");
        neigh_sol_detect_dad_6lp(f);
    }
#endif

    link = pico_ipv6_link_get(&icmp6_hdr->msg.info.neigh_adv.target);
    if (!link) { /* Not for us. */
        return -1;
    }

    pico_icmp6_neighbor_advertisement(f,  &icmp6_hdr->msg.info.neigh_adv.target);

    return 0;
}

static int icmp6_initial_checks(struct pico_frame *f)
{
    /* Common "step 0" validation */
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;

    ipv6_hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    /* RFC4861 - 7.1.2 :
     *       - The IP Hop Limit field has a value of 255, i.e., the packet
     *               could not possibly have been forwarded by a router.
     *       - ICMP Checksum is valid.
     *       - ICMP Code is 0.
     */
    if (ipv6_hdr->hop != 255 || pico_icmp6_checksum(f) != 0 || icmp6_hdr->code != 0)
        return -1;

    return 0;
}

static int neigh_adv_option_len_validity_check(struct pico_frame *f)
{
    /* Step 4 validation */
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    uint8_t *opt;
    int optlen = f->transport_len - PICO_ICMP6HDR_NEIGH_ADV_SIZE;
    /* RFC4861 - 7.1.2 :
     *       - All included options have a length that is greater than zero.
     */
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    opt = ((uint8_t *)&icmp6_hdr->msg.info.neigh_adv) + sizeof(struct neigh_adv_s);

    while(optlen > 0) {
        int opt_size = (opt[1] << 3);
        if (opt_size == 0)
            return -1;

        opt = opt + opt_size;
        optlen -= opt_size;
    }
    return 0;
}

static int neigh_adv_mcast_validity_check(struct pico_frame *f)
{
    /* Step 3 validation */
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    /* RFC4861 - 7.1.2 :
     *       - If the IP Destination Address is a multicast address the
     *         Solicited flag is zero.
     */
    ipv6_hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    if (pico_ipv6_is_multicast(ipv6_hdr->dst.addr) && IS_SOLICITED(icmp6_hdr))
        return -1;

    return neigh_adv_option_len_validity_check(f);
}

static int neigh_adv_validity_checks(struct pico_frame *f)
{
    /* Step 2 validation */
    /* RFC4861 - 7.1.2:
     * - ICMP length (derived from the IP length) is 24 or more octets.
     */
    if (f->transport_len < PICO_ICMP6HDR_NEIGH_ADV_SIZE)
        return -1;

    return neigh_adv_mcast_validity_check(f);
}


static int neigh_sol_mcast_validity_check(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    if (pico_ipv6_is_solnode_multicast(icmp6_hdr->msg.info.neigh_sol.target.addr, f->dev) == 0)
        return -1;

    return 0;
}

static int neigh_sol_unicast_validity_check(struct pico_frame *f)
{
    struct pico_ipv6_link *link;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;

#ifdef PICO_SUPPORT_6LOWPAN
    /* Don't validate target address, the sol is always targeted at 6LBR so
     * no possible interface on the 6LBR can have the same address as specified in
     * the target */
    if (PICO_DEV_IS_6LOWPAN(f->dev))
        return 0;
#endif

    link = pico_ipv6_link_by_dev(f->dev);
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    while(link) {
        /* RFC4861, 7.2.3:
         *
         *  - The Target Address is a "valid" unicast or anycast address
         *    assigned to the receiving interface [ADDRCONF],
         *  - The Target Address is a unicast or anycast address for which the
         *    node is offering proxy service, or
         *  - The Target Address is a "tentative" address on which Duplicate
         *    Address Detection is being performed
         */
        if (pico_ipv6_compare(&link->address, &icmp6_hdr->msg.info.neigh_sol.target) == 0)
            return 0;

        link = pico_ipv6_link_by_dev_next(f->dev, link);
    }
    return -1;

}

static int neigh_sol_validate_unspec(struct pico_frame *f)
{
    /* RFC4861, 7.1.1:
     *
     * - If the IP source address is the unspecified address, the IP
     *   destination address is a solicited-node multicast address.
     *
     * - If the IP source address is the unspecified address, there is no
     *   source link-layer address option in the message.
     *
     */

    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)(f->net_hdr);
    struct pico_icmp6_opt_lladdr opt = {
        0
    };
    int valid_lladdr = neigh_options(f, &opt, PICO_ND_OPT_LLADDR_SRC);
    if (!f->dev->mode && pico_ipv6_is_solnode_multicast(hdr->dst.addr, f->dev) == 0) {
        return -1;
    }

    if (valid_lladdr) {
        return -1;
    }

    return 0;
}

static int neigh_sol_validity_checks(struct pico_frame *f)
{
    /* Step 2 validation */
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)(f->net_hdr);
    if (f->transport_len < PICO_ICMP6HDR_NEIGH_ADV_SIZE)
    {
        nd_dbg("Neigh sol validity fail: transport len fail. %d\n", f->transport_len);
        return -1;
    }

    if ((pico_ipv6_is_unspecified(hdr->src.addr)) && (neigh_sol_validate_unspec(f) < 0))
    {
        nd_dbg("Neigh sol validity fail: unspecified %d && validate unspec %d\n", pico_ipv6_is_unspecified(hdr->src.addr), neigh_sol_validate_unspec(f));
        return -1;
    }

    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    if (pico_ipv6_is_multicast(icmp6_hdr->msg.info.neigh_adv.target.addr)) {
        return neigh_sol_mcast_validity_check(f);
    }

    return neigh_sol_unicast_validity_check(f);
}

static int router_adv_validity_checks(struct pico_frame *f)
{
    /* Step 2 validation */
    if (f->transport_len < PICO_ICMP6HDR_ROUTER_ADV_SIZE)
        return -1;

    return 0;
}

static int neigh_adv_checks(struct pico_frame *f)
{
    /* Step 1 validation */
    if (icmp6_initial_checks(f) < 0)
        return -1;

    return neigh_adv_validity_checks(f);
}

static int redirect_process(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_ipv6_link *link = NULL;
    struct redirect_s *redirect_hdr = NULL;
    struct pico_ip6 gateway = {{0}};
    struct pico_ip6 zero = {{0}};
    struct pico_icmp6_opt_lladdr opt_ll = {0};
    struct pico_icmp6_opt_redirect opt_redirect = {0};
    int optres_lladdr = 0, optres_redirect = 0;

    ipv6_hdr  = (struct pico_ipv6_hdr *)f->net_hdr;
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    redirect_hdr = &(icmp6_hdr->msg.info.redirect);

    /* Check the options */
    optres_lladdr   = neigh_options(f, &opt_ll, PICO_ND_OPT_LLADDR_TGT);
    optres_redirect = neigh_options(f, &opt_redirect, PICO_ND_OPT_REDIRECT);

    if (optres_lladdr < 0 || optres_redirect < 0) {
        /* Malformed packet */
        return -1;
    }

    /* If NBMA link, this has to be included but currently NBMA is (will?) not be supported by picoTCP */
    if (optres_lladdr) {
        /* RFC 4861 $8.3
         *  * If a neighbor cache entry is created for the target,
         *  * its reachability state MUST be set to STALE
         *  * If a cache entry already existed and it is updated with a different ll addr,
         *  * its reachability state MUST also be set to STALE.
         *  * If the ll addr is the same as that already in cache,
         *  * the cache entry's state remains unchanged
         *  */
        struct pico_ipv6_neighbor *target_neighbor = NULL;

        target_neighbor = pico_get_neighbor_from_ncache(&redirect_hdr->target);
        if (target_neighbor) {
            /* Neighbor is already known */
            if (pico_ipv6_neighbor_compare_stored(target_neighbor, &opt_ll, f->dev) != 0) {
                /* ll addr is NOT same as that already in cache */
                pico_ipv6_neighbor_update(target_neighbor, &opt_ll, target_neighbor->dev);
                target_neighbor->state = PICO_ND_STATE_STALE;
                pico_ipv6_nd_queued_trigger(&target_neighbor->address);
            } else {
                /* ll addr is the same as that already in cache */
                /* DO NOTHING */
            }
        } else {
            /* Neighbor is NOT known */
            target_neighbor = pico_nd_add(&redirect_hdr->target, f->dev);

            if (target_neighbor) {
                pico_ipv6_neighbor_update(target_neighbor, &opt_ll, target_neighbor->dev);
                target_neighbor->state = PICO_ND_STATE_STALE;
            } else {
                nd_dbg("Redirect: could not add neighbor, aborting route update\n");
                return -1;
            }
        }
    } else {
        /* No ll addr option, we have to find out ourselves */
    }

    /* TODO process opt_redirect
     * Currently no use was seen in processing opt_redirect
     */

    /* Get our link using our ipv6 addr from the ipv6 hdr */
    link = pico_ipv6_link_get(&ipv6_hdr->dst);

    /* Get our current gateway to the dst addr */
    gateway = pico_ipv6_route_get_gateway(&redirect_hdr->dest);

#ifdef DEBUG_IPV6_ND
    /* Is the gateway zero-address? */
    if (memcmp(&gateway, &zero, PICO_SIZE_IP6) == 0) {
        char *ipv6_addr = PICO_ZALLOC(PICO_IPV6_STRING);
        if (!ipv6_addr) {
            dbg("Could not allocate ipv6 string for gateway debug\n");
        } else {
            pico_ipv6_to_string(ipv6_addr, ipv6_hdr->src.addr);
            dbg("Zero gateway from route with ip addr: %s", ipv6_addr);
            PICO_FREE(ipv6_addr);
        }
    }
#endif

    /* remove old route to dest */
    if (pico_ipv6_route_del(redirect_hdr->dest, zero, gateway, DEFAULT_METRIC, link) != 0) {
#ifdef DEBUG_IPV6_ND
        char *ipv6_addr = PICO_ZALLOC(PICO_IPV6_STRING);
        if (!ipv6_addr) {
            dbg("Could not allocate ipv6 string for route del debug\n");
        } else {
            pico_ipv6_to_string(ipv6_addr, redirect_hdr->dest.addr);
            dbg("Old route to %s was not found.\n", ipv6_addr);
            PICO_FREE(ipv6_addr);
        }
#endif
    }

    /* add new route recv from redirect to known routes so in future we will use this one */
    if (pico_ipv6_route_add(redirect_hdr->dest, zero, redirect_hdr->target, DEFAULT_METRIC, link) != 0) {
#ifdef DEBUG_IPV6_ND
        char *ipv6_addr = PICO_ZALLOC(PICO_IPV6_STRING);
        if (!ipv6_addr) {
            dbg("Could not allocate ipv6 string for route add debug\n");
        } else {
            pico_ipv6_to_string(ipv6_addr, redirect_hdr->dest.addr);
            dbg("New route could not be added for destination %s.\n", ipv6_addr);
            PICO_FREE(ipv6_addr);
        }
#endif
    }

    return 0;
}

static int pico_nd_prefix_option_valid(struct pico_device *dev, struct pico_icmp6_opt_prefix *prefix)
{
    pico_time now = PICO_TIME_MS();
    struct pico_ipv6_link *link;

#ifndef PICO_SUPPORT_6LOWPAN
    IGNORE_PARAMETER(dev);
#endif

    /* RFC4862 5.5.3 */
    /* a) If the Autonomous flag is not set, silently ignore the Prefix
     *       Information option.
     */
    if (prefix->aac == 0)
        return -1;

    /* b) If the prefix is the link-local prefix, silently ignore the
     *       Prefix Information option
     */
    if (pico_ipv6_is_linklocal(prefix->prefix.addr))
        return -1;

    /* c) If the preferred lifetime is greater than the valid lifetime,
     *       silently ignore the Prefix Information option
     */
    if (long_be(prefix->pref_lifetime) > long_be(prefix->val_lifetime))
        return -1;

#ifdef PICO_SUPPORT_6LOWPAN
    /* RFC6775 (6LoWPAN): Should the host erroneously receive a PIO with the L (on-link)
     *      flag set, then that PIO MUST be ignored.
     */
    if (PICO_DEV_IS_6LOWPAN(dev) && prefix->onlink)
        return -1;
#endif

    if (prefix->val_lifetime == 0)
        return -1;


    if (prefix->prefix_len != 64) {
        return -1;
    }

    link = pico_ipv6_prefix_configured(&prefix->prefix);
    if (link) {
        pico_ipv6_lifetime_set(link, now + (1000 * (pico_time)(long_be(prefix->val_lifetime))));
        /* if other router supplies route to same prefix,
         * link variable is not NULL but that doesn't mean R ADV is not valid
         */
        /* return -1; */
    }

    return 0;
}

/*MARK*/
#ifdef PICO_SUPPORT_6LOWPAN
static void pico_6lp_nd_unreachable_gateway(struct pico_ip6 *a)
{
    struct pico_ipv6_route *route = NULL;
    struct pico_ipv6_link *local = NULL;
    struct pico_tree_node *node = NULL;
    struct pico_device *dev = NULL;

    /* RFC6775, 5.3:
     *  ... HOSTS need to intelligently retransmit RSs when one of its
     *  default routers becomes unreachable ...
     */
    pico_tree_foreach(node, &Device_tree) {
        if (PICO_DEV_IS_6LOWPAN(dev) && (!dev->hostvars.routing)) {
            /* Check if there's a gateway configured */
            route = pico_ipv6_gateway_by_dev(dev);
            while (route) {
                if (0 == pico_ipv6_compare(&route->gateway, a)) {
                    local = pico_ipv6_linklocal_get(dev);
                    pico_6lp_nd_start_soliciting(local, route);
                    break;
                }
                route = pico_ipv6_gateway_by_dev_next(dev, route);
            }
        }
    }
}

static int pico_6lp_nd_validate_sol_aro(struct pico_icmp6_opt_aro *aro)
{
    if (aro->len != 2 || aro->status != 0)
        return -1;
    return 0;
}

static int pico_6lp_nd_validate_adv_aro(struct pico_device *dev, struct pico_icmp6_opt_aro *aro, uint8_t *status)
{
    union pico_ll_addr addr, eui;

    /* RFC6775 - 5.5.2 :
     *      - If the length field is not two, the option is silently ignored.
     *      - If the EUI-64 field does not match the EUI-64 of the interface,
     *        the option is silently ignored.
     */
    if (aro->len != 2)
        return -1;

    /* TODO: Update to abstract address, e.g. remove dependency of '.pan' */
    eui.pan.addr._ext = aro->eui64;
    eui.pan.mode = AM_6LOWPAN_EXT;
    addr.pan.addr._ext = ((struct pico_6lowpan_info *)dev->eth)->addr_ext;
    addr.pan.mode = AM_6LOWPAN_EXT;

    if (dev && pico_6lowpan_lls[dev->mode].addr_cmp) {
        if (pico_6lowpan_lls[dev->mode].addr_cmp(&addr, &eui))
            return -1;
    } else {
        return -1;
    }

    *status = aro->status;
    return 0;
}

/* Deregisters a link from all default gateways */
static void pico_6lp_nd_deregister(struct pico_ipv6_link *l)
{
    struct pico_ipv6_route *gw = pico_ipv6_gateway_by_dev(l->dev);
    while (gw) {
        pico_icmp6_neighbor_solicitation(l->dev, &l->address, PICO_ICMP6_ND_DEREGISTER, &gw->gateway);
        gw = pico_ipv6_gateway_by_dev_next(l->dev, gw);
    }
}

/* Retransmits neighbors solicitations with address registration if ARO is not acknowledged */
static void pico_6lp_nd_register_try(pico_time now, void *arg)
{
    struct pico_ipv6_link *l = arg;
    struct pico_ipv6_route *gw = pico_ipv6_gateway_by_dev(l->dev);
    IGNORE_PARAMETER(now);
    while (gw) {
        l->istentative = 1;
        pico_icmp6_neighbor_solicitation(l->dev, &l->address, PICO_ICMP6_ND_DAD, &gw->gateway);
        gw = pico_ipv6_gateway_by_dev_next(l->dev, gw);
    }
    pico_timer_add(l->dev->hostvars.retranstime, pico_6lp_nd_register_try, l);
}

/* Tries to register a link with one or more of its default routers */
void pico_6lp_nd_register(struct pico_ipv6_link *link)
{
    /* RFC6775: When a host has configured a non-link-local IPv6 address, it registers that
     *      address with one or more of its default routers using the Address Registration
     *      Option (ARO) in an NS message. */
    pico_6lp_nd_register_try(PICO_TIME_MS(), link);
}

/* Check if there are default routers configured. If not, sent a router solicitation */
static void pico_6lp_nd_do_solicit(pico_time now, void *arg)
{
    struct pico_ipv6_route *gw = arg;
    struct pico_ip6 *dst = NULL;
    IGNORE_PARAMETER(now);

    if (!pico_ipv6_gateway_by_dev(gw->link->dev) && !gw->link->dev->hostvars.routing) {
        /* If the solicitation is to be sent unicast */
        if (!pico_ipv6_is_unspecified(gw->gateway.addr) && gw->retrans < MAX_RTR_SOLICITATIONS)
            dst = &gw->gateway;

        /* Exponential backoff */
        if (++gw->retrans == MAX_RTR_SOLICITATIONS) {
            gw->backoff <<= 1;
            if (gw->backoff >= MAX_RTR_SOLICITATION_INTERVAL)
                gw->backoff = (pico_time)MAX_RTR_SOLICITATION_INTERVAL;
        }

        /* If router list is empty, send router solicitation */
        pico_icmp6_router_solicitation(gw->link->dev, &gw->link->address, dst);

        /* Apply exponential retransmission timer, see RFC6775 5.3 */
        pico_timer_add(gw->backoff, pico_6lp_nd_do_solicit, gw);
        nd_dbg("[6LP-ND]$ No default routers configured, soliciting\n");
    } else {
        PICO_FREE(gw);
    }
}

/* Start transmitting repetitive router solicitations */
int pico_6lp_nd_start_soliciting(struct pico_ipv6_link *l, struct pico_ipv6_route *gw)
{
    struct pico_ipv6_route *dummy = PICO_ZALLOC(sizeof(struct pico_ipv6_route));
    struct pico_ip6 *dst = NULL;

    if (dummy) {
        if (gw) { // If the router solicitation has to be sent unicast ...
            dst = &gw->gateway; // ... the gateway is the destination
            memcpy(dummy->gateway.addr, gw->gateway.addr, PICO_SIZE_IP6); // and should be retrievable in the timer event
        }
        dummy->link = l; // the link that has to be reconfirmed as well.

        /* If router list is empty, send router solicitation */
        pico_icmp6_router_solicitation(l->dev, &l->address, dst);

        if (!l->dev->hostvars.routing) {
            dummy->retrans = 0;
            dummy->backoff = RTR_SOLICITATION_INTERVAL;
            if (!pico_timer_add(dummy->backoff, pico_6lp_nd_do_solicit, dummy)) {
                PICO_FREE(dummy);
                return -1;
            }
        } else {
            PICO_FREE(dummy);
        }
        return 0;
    }
    return -1;
}

/* Validate Neighbor advertisement mesaage */
static int pico_6lp_nd_neigh_adv_validate(struct pico_frame *f, uint8_t *status)
{
    struct pico_icmp6_hdr *icmp = (struct pico_icmp6_hdr *)f->transport_hdr;
    struct pico_icmp6_opt_aro *aro = (struct pico_icmp6_opt_aro *)((uint8_t *)&icmp->msg.info.neigh_adv + sizeof(struct neigh_sol_s));
    struct pico_ipv6_hdr *ip = (struct pico_ipv6_hdr *)f->net_hdr;

    /* 6LP: Target address cannot be MCAST and the Source IP-address cannot be UNSPECIFIED or MCAST */
    if (pico_ipv6_is_multicast(icmp->msg.info.neigh_adv.target.addr) || pico_ipv6_is_unspecified(ip->src.addr) ||
        pico_ipv6_is_multicast(ip->src.addr))
        return -1;

    return pico_6lp_nd_validate_adv_aro(f->dev, aro, status);
}

/* Process neighbor advertisement */
static int pico_6lp_nd_neigh_adv_process(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp = (struct pico_icmp6_hdr *)f->transport_hdr;
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    struct pico_ipv6_link *l = NULL;
    struct pico_ip6 zero = {
        .addr = {0}
    };
    uint8_t status = 0;

    if (pico_6lp_nd_neigh_adv_validate(f, &status)) {
        return -1;
    } else {
        l = pico_ipv6_link_get(&icmp->msg.info.neigh_adv.target);
        if (l)
            l->istentative = 0;
        else
            return -1;

        /* Globally routable address has been registered @ 6LoWPAN Border Router */
        if (1 == status) { // Duplicate address detected
            nd_dbg("[6LP-ND]: Registering routable address failed, removing link...\n");
            ipv6_duplicate_detected(l);
            return -1;
        } else if (2 == status) { // Router's NCE is full, remove router from default router list
            pico_ipv6_route_del(zero, zero, hdr->src, 10, l);
            pico_6lp_nd_start_soliciting(pico_ipv6_linklocal_get(l->dev), NULL);
        } else { // Registration success
            nd_dbg("[6LP-ND]: Registering routable address succeeded!\n");
        }
    }
    return 0;
}

/* Add a new 6LoWPAN neighbor with lifetime from ARO */
static struct pico_ipv6_neighbor *pico_nd_add_6lp(struct pico_ip6 naddr, struct pico_icmp6_opt_aro *aro, struct pico_device *dev)
{
    struct pico_ipv6_neighbor *new = NULL;

    if ((new = pico_nd_add(&naddr, dev))) {
        new->expire = PICO_TIME_MS() + (pico_time)(ONE_MINUTE * aro->lifetime);
        dbg("ARO Lifetime: %d minutes\n", aro->lifetime);
    } else {
        return NULL;
    }

    return new;
}

/* RFC6775 §6.5.2.  Returning Address Registration Errors */
static int neigh_sol_dad_reply(struct pico_frame *sol, struct pico_icmp6_opt_lladdr *sllao, struct pico_icmp6_opt_aro *aro, uint8_t status)
{
    uint8_t sllao_len = (uint8_t)(sllao->len * 8);
    struct pico_icmp6_hdr *icmp = NULL;
    struct pico_frame *adv = pico_frame_copy(sol);
    struct pico_ip6 ll = {{0xfe,0x80,0,0,0,0,0,0, 0,0,0,0,0,0,0,0}};
    size_t len = pico_hw_addr_len(sol->dev, sllao);
    union pico_ll_addr lladdr;

    if (!adv) {
        return -1;
    } else {
        icmp = (struct pico_icmp6_hdr *)adv->transport_hdr;

        /* Set the status of the Address Registration */
        aro->status = status;
        if (PICO_DEV_IS_6LOWPAN(sol->dev)) {
            memcpy(lladdr.pan.addr.data, aro->eui64.addr, len);
            lladdr.pan.mode = (len == SIZE_6LOWPAN_EXT) ? AM_6LOWPAN_EXT : AM_6LOWPAN_SHORT;
            if (pico_6lowpan_lls[sol->dev->mode].addr_iid)
                pico_6lowpan_lls[sol->dev->mode].addr_iid(ll.addr + 8, &lladdr);
        }

        /* Remove the SLLAO from the frame */
        memmove(((uint8_t *)&icmp->msg.info.neigh_sol) + sizeof(struct neigh_sol_s), ((uint8_t *)&icmp->msg.info.neigh_sol) + sizeof(struct neigh_sol_s) + sllao_len, (size_t)(aro->len * 8));
        adv->transport_len = (uint16_t)(adv->transport_len - sllao_len);
        adv->len = (uint16_t)(adv->len - sllao_len);

        /* I'm a router, and it's always solicited */
        icmp->msg.info.neigh_adv.rsor = 0xE0;

        /* Set the ICMPv6 message type to Neighbor Advertisements */
        icmp->type = PICO_ICMP6_NEIGH_ADV;
        icmp->code = 0;
        icmp->crc = pico_icmp6_checksum(adv);

        pico_ipv6_frame_push(adv, NULL, &ll, PICO_PROTO_ICMP6, 0);
        return 0;
    }
}

/* RFC6775 §6.5.1.  Checking for Duplicates */
static int neigh_sol_detect_dad_6lp(struct pico_frame *f)
{
    struct pico_ipv6_neighbor *n = NULL;
    struct pico_icmp6_opt_lladdr *sllao = NULL;
    struct pico_icmp6_hdr *icmp = NULL;
    struct pico_icmp6_opt_aro *aro = NULL;
    size_t len = 0;

    icmp = (struct pico_icmp6_hdr *)f->transport_hdr;
    sllao = (struct pico_icmp6_opt_lladdr *)((uint8_t *)&icmp->msg.info.neigh_sol + sizeof(struct neigh_sol_s));
    aro = (struct pico_icmp6_opt_aro *)(((uint8_t *)&icmp->msg.info.neigh_sol) + sizeof(struct neigh_sol_s) + (sllao->len * 8));

    /* Validate Address Registration Option */
    if (pico_6lp_nd_validate_sol_aro(aro))
        return -1;

    /* See RFC6775 $6.5.1: Checking for duplicates */
    if (!(n = pico_get_neighbor_from_ncache(&icmp->msg.info.neigh_sol.target))) {
        /* No dup, add neighbor to cache */
        if (pico_nd_add_6lp(icmp->msg.info.neigh_sol.target, aro, f->dev))
            neigh_sol_dad_reply(f, sllao, aro, ICMP6_ARO_SUCCES);
        else /* No dup, but neighbor cache is full */
            neigh_sol_dad_reply(f, sllao, aro, ICMP6_ARO_FULL);
        return 0;
    } else {
        if (!aro->lifetime) {
            pico_tree_delete(&NCache, n);
            PICO_FREE(n);
            neigh_sol_dad_reply(f, sllao, aro, ICMP6_ARO_SUCCES);
            return 0;
        }
        /* Check if hwaddr differs */
        len = pico_hw_addr_len(f->dev, sllao);
        if (memcmp(sllao->addr.data, n->hwaddr.data, len) == 0) {
            n->expire = PICO_TIME_MS() + (pico_time)(ONE_MINUTE * aro->lifetime);
            neigh_sol_dad_reply(f, sllao, aro, ICMP6_ARO_DUP);
        }
        return 0;
    }
}

static int router_sol_validity_checks(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)(f->net_hdr);
    struct pico_icmp6_opt_lladdr opt = { 0 };
    int sllao_present = 0;

    /* Step 2 validation */
    if (f->transport_len < PICO_ICMP6HDR_ROUTER_SOL_SIZE_6LP)
        return -1;

    /* RFC4861, 6.1.1:
     * - If the IP source address is the unspecified address, there is no
     *   source link-layer address option in the message.
     */
    /* Check for SLLAO if the IP source address is UNSPECIFIED */
    sllao_present = neigh_options(f, &opt, PICO_ND_OPT_LLADDR_SRC);
    if (pico_ipv6_is_unspecified(hdr->src.addr)) {
        /* Frame is not valid when SLLAO is present if IP6-SRC is UNSPEC. */
        if (sllao_present) {
            return -1;
        }
    } else {
        /* Frame is not valid when no SLLAO if present if there's a IP6-SRC */
        if (sllao_present <= 0) {
            return -1;
        }
    }

    return 0;
}

static int router_sol_checks(struct pico_frame *f)
{
    /* Step 1 validation */
    if (icmp6_initial_checks(f) < 0)
        return -1;

    return router_sol_validity_checks(f);
}

static int router_sol_process(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = NULL;

    /* Determine if i'm a 6LBR, if i'm not, can't do anything with a router solicitation */
    if (!f->dev->hostvars.routing)
        return -1;

    nd_dbg("[6LBR]: Processing router solicitation...\n");

    /* Router solicitation message validation */
    if (router_sol_checks(f) < 0)
        return -1;

    /* Maybe create a tentative NCE? No, will do it later */

    /* Send a router advertisement via unicast to requesting host */
    hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    return pico_icmp6_router_advertisement(f->dev, &hdr->src);
}

#endif /* PICO_SUPPORT_6LOWPAN */

static int pico_nd_router_sol_recv(struct pico_frame *f)
{
#ifdef PICO_SUPPORT_6LOWPAN
    /* 6LoWPAN: reply on explicit router solicitations via unicast */
    if (PICO_DEV_IS_6LOWPAN(f->dev))
        return router_sol_process(f);
#endif

    /* RFC 4861 $6.2.6
     *  * A host MUST silently discard any received Router Solicitation messages.
     *  *
     *  * We are HOST-ONLY
     *  */

    IGNORE_PARAMETER(f);

    return 0;
}
static int radv_process(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_link *link;
    struct pico_ipv6_hdr *hdr;
    struct pico_ip6 zero = {
        .addr = {0}
    };

    int optres_prefix = 0;
    pico_time now = PICO_TIME_MS();
    struct pico_icmp6_opt_prefix prefix_option;
#ifdef PICO_SUPPORT_6LOWPAN
    int sllao = 0;
    int abro_valid = 0;
    int context_option_valid = 0;

    struct pico_icmp6_opt_lladdr lladdr_src;
    struct pico_icmp6_opt_abro abro;
    struct pico_icmp6_opt_6co co;
#endif

    hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    optres_prefix = neigh_options(f, &prefix_option, PICO_ND_OPT_PREFIX);

    if (optres_prefix < 0) {
        /* Malformed packet */
        return -1;
    }

#ifdef PICO_SUPPORT_6LOWPAN
    sllao = neigh_options(f, &lladdr_src, PICO_ND_OPT_LLADDR_SRC);
    if (sllao < 0) {
        /* Malformed packet */
        /* RFC6775 (6LoWPAN): An SLLAO MUST be included in the RA. */
        if (PICO_DEV_IS_6LOWPAN(f->dev)) {
            return -1;
        }
    }

#ifdef PICO_6LOWPAN_IPHC_ENABLED
    context_option_valid = neigh_options(f, &co, PICO_ND_OPT_6CO);
    if (context_option_valid < 0) {
        /* Malformed packet */
        return -1;
    }

    if (PICO_DEV_IS_6LOWPAN(f->dev)) {
        struct pico_ip6 prefix;
        memcpy(prefix.addr, (uint8_t *)&co.prefix, (size_t)(co.len - 1) << 3);
        ctx_update(prefix, co.id, co.len, co.lifetime, co.c, f->dev);
    }
#endif

    abro_valid = neigh_options(f, &abro, PICO_ND_OPT_ABRO);
    if (abro_valid < 0) {
        /* Malformed packet */
        return -1;
    }

    /* TODO: process ABRO option */
#endif


    if (pico_nd_prefix_option_valid(f->dev, &prefix_option) == 0) {
            link = pico_ipv6_link_add_local(f->dev, &prefix_option.prefix);
            if (link) {
                pico_ipv6_lifetime_set(link, now + (pico_time)(1000 * (long_be(prefix_option.val_lifetime))));
                if (pico_ipv6_route_add(zero, zero, hdr->src, 10, link) != 0) {
                        nd_dbg("Could not add default route in router adv\n");
                } else {
                    nd_dbg("added route in router adv\n");
#ifdef PICO_SUPPORT_6LOWPAN
                    if (PICO_DEV_IS_6LOWPAN(f->dev)) {
                        pico_6lp_nd_register(link);
                    }
#endif
                }
                pico_ipv6_router_add_link(&hdr->src, link);
            } else {
                nd_dbg("router adv: no link\n");
            }
    } else {
        /* prefix option is not valid, silently ignore it */
        nd_dbg("Prefix option is not valid\n");
    }

    if (icmp6_hdr->msg.info.router_adv.retrans_time != 0u) {
        f->dev->hostvars.retranstime = long_be(icmp6_hdr->msg.info.router_adv.retrans_time);
    }

    if(icmp6_hdr->msg.info.router_adv.hop) {
        f->dev->hostvars.hoplimit = icmp6_hdr->msg.info.router_adv.hop;
    }

    return 0;
}


static int pico_nd_router_adv_recv(struct pico_frame *f)
{
    struct pico_tree_node *index = NULL;
    struct pico_ipv6_link *link = NULL;
    struct pico_ipv6_hdr *hdr = NULL;
    struct pico_ipv6_route *route = NULL;

    if (icmp6_initial_checks(f) < 0)
        return -1;


    if (router_adv_validity_checks(f) < 0)
        return -1;


    hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    pico_tree_foreach(index,&IPV6Links){
      link = index->keyValue;
      if(link->rs_retries >= 1 && pico_ipv6_is_linklocal(hdr->src.addr)){
        link->rs_retries = MAX_INITIAL_RTR_ADVERTISEMENTS;
        link->rs_expire_time = 0;
      }
      else if(link->rs_retries == 0 && pico_ipv6_is_linklocal(hdr->src.addr)){
        route = pico_ipv6_gateway_by_dev(f->dev);
        pico_icmp6_router_solicitation(link->dev, &link->address, &route->gateway);
        link->rs_retries = MAX_INITIAL_RTR_ADVERTISEMENTS;
        link->rs_expire_time = 0;
      }
    }

    pico_ipv6_neighbor_from_unsolicited(f);
    pico_ipv6_router_from_unsolicited(f);

    return radv_process(f);
}

static int pico_nd_neigh_sol_recv(struct pico_frame *f)
{
    if (icmp6_initial_checks(f) < 0)
    {
        nd_dbg("ND: neigh sol initial check failed\n");
        return -1;
    }

    if (neigh_sol_validity_checks(f) < 0)
    {
        nd_dbg("ND: neigh sol validity check failed\n");
        return -1;
    }

    return neigh_sol_process(f);
}

static int pico_nd_neigh_adv_recv(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_link *link = NULL;

    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    if (neigh_adv_checks(f) < 0) {
        return -1;
    }

    /* ETH: Target address belongs to a tentative link on this device, DaD detected a dup */
    link = pico_ipv6_link_istentative(&icmp6_hdr->msg.info.neigh_adv.target);
    if (link && !link->dev->mode)
        ipv6_duplicate_detected(link);

    return neigh_adv_process(f);
}

static int pico_nd_redirect_is_valid(struct pico_frame *f)
{
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)(f->net_hdr);
    struct pico_ip6 gateway = {{0}};

    icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    gateway = pico_ipv6_route_get_gateway(&icmp6_hdr->msg.info.redirect.dest);

    if (f->transport_len < PICO_ICMP6HDR_REDIRECT_SIZE)
    {
        return -1;
    }

    if (pico_ipv6_is_linklocal(hdr->src.addr) != 1)
    {
        return -1;
    }

    if (pico_ipv6_is_multicast(icmp6_hdr->msg.info.redirect.dest.addr))
    {
        return -1;
    }

    if (! (pico_ipv6_is_linklocal(icmp6_hdr->msg.info.redirect.target.addr) == 1 || pico_ipv6_compare(&icmp6_hdr->msg.info.redirect.target, &icmp6_hdr->msg.info.redirect.dest) == 0) )
    {
        return -1;
    }

    if (memcmp(gateway.addr, hdr->src.addr, PICO_SIZE_IP6) != 0) {
        return -1;
    }

    /* ALL included options have length > 0, checked when processing redirect frame */

    return 0;
}

static int pico_nd_redirect_recv(struct pico_frame *f)
{
    if (icmp6_initial_checks(f) < 0)
    {
        nd_dbg("redirect: initial checks failed\n");
        return -1;
    }

    if (pico_nd_redirect_is_valid(f) < 0)
    {
        nd_dbg("redirect: redirect check failed\n");
        return -1;
    }

    return redirect_process(f);
}

static void pico_ipv6_nd_timer_elapsed(pico_time now, struct pico_ipv6_neighbor *n)
{
    IGNORE_PARAMETER(now);

    switch(n->state) {
    case PICO_ND_STATE_INCOMPLETE:
        /* Fallthrough */
    case PICO_ND_STATE_PROBE:
        if (n->failure_multi_count > PICO_ND_MAX_MULTICAST_SOLICIT ||
            n->failure_uni_count   > PICO_ND_MAX_UNICAST_SOLICIT) {
            nd_dbg("DELETE ENTRY\n");
            pico_nd_delete_entry(n);
            return;
        }

        pico_nd_new_expire_time(n);
        pico_nd_discover(n);
        break;

    case PICO_ND_STATE_REACHABLE:
        n->state = PICO_ND_STATE_STALE;
        /* dbg("IPv6_ND: neighbor expired!\n"); */
        return;

    case PICO_ND_STATE_STALE:
        break;

    case PICO_ND_STATE_DELAY:
        n->expire = 0ull;
        n->state = PICO_ND_STATE_PROBE;
        break;
    default:
        dbg("IPv6_ND: neighbor in wrong state!\n");
    }

    pico_nd_new_expire_time(n);
}

static void pico_ipv6_check_router_lifetime_callback(pico_time now, void *arg)
{
    struct pico_tree_node *index = NULL, *_tmp = NULL;
    struct pico_ipv6_router *r;

    IGNORE_PARAMETER(arg);

    pico_tree_foreach_safe(index, &RCache, _tmp)
    {
        r = index->keyValue;
        if (now > r->invalidation) {
            nd_dbg("ROUTER EXPIRED: %lu, %lu\n", now, r->invalidation);
            pico_ipv6_router_down(&r->router->address);
            pico_ipv6_assign_default_router(r->is_default);
            pico_tree_delete(&RCache, r);
        }
    }

    if (!pico_timer_add(200, pico_ipv6_check_router_lifetime_callback, NULL)) {
        dbg("IPV6 ND: Failed to start check router lifetime callback timer\n");
        /* TODO no idea what consequences this has */
    }
}

static void pico_ipv6_nd_timer_callback(pico_time now, void *arg)
{
    struct pico_tree_node *index = NULL, *_tmp = NULL;
    struct pico_ipv6_neighbor *n;

    IGNORE_PARAMETER(arg);

    pico_tree_foreach_safe(index, &NCache, _tmp)
    {
        n = index->keyValue;
        if (now > n->expire) {
            nd_dbg("NB EXPIRED: %lu, %lu\n", now, n->expire);
            print_nd_state(n);
            pico_ipv6_nd_timer_elapsed(now, n);
        }
    }
    if (!pico_timer_add(200, pico_ipv6_nd_timer_callback, NULL)) {
        dbg("IPV6 ND: Failed to start callback timer\n");
        /* TODO no idea what consequences this has */
    }
}

#define PICO_IPV6_ND_MIN_RADV_INTERVAL  (5000)
#define PICO_IPV6_ND_MAX_RADV_INTERVAL (15000)

static void pico_ipv6_nd_ra_timer_callback(pico_time now, void *arg)
{
    struct pico_tree_node *devindex = NULL;
    struct pico_tree_node *rindex = NULL;
    struct pico_device *dev;
    struct pico_ipv6_route *rt;
    struct pico_ip6 nm64 = { {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0 } };
    pico_time next_timer_expire = 0u;

    IGNORE_PARAMETER(arg);
    IGNORE_PARAMETER(now);
    pico_tree_foreach(rindex, &IPV6Routes)
    {
        rt = rindex->keyValue;
        if (pico_ipv6_compare(&nm64, &rt->netmask) == 0) {
            pico_tree_foreach(devindex, &Device_tree) {
                dev = devindex->keyValue;
                /* Do not send periodic router advertisements when there aren't 2 interfaces from and to the device can route */
                if ((!pico_ipv6_is_linklocal(rt->dest.addr)) && dev->hostvars.routing && (rt->link)
                    && (dev != rt->link->dev) && !PICO_DEV_IS_6LOWPAN(dev)) {
                    pico_icmp6_router_advertisement(dev, &rt->dest);
                }
            }
        }
    }

    next_timer_expire = PICO_IPV6_ND_MIN_RADV_INTERVAL + (pico_rand() % (PICO_IPV6_ND_MAX_RADV_INTERVAL - PICO_IPV6_ND_MIN_RADV_INTERVAL));
    if (!pico_timer_add(next_timer_expire, pico_ipv6_nd_ra_timer_callback, NULL)) {
        dbg("IPv6 ND: Failed to start callback timer\n");
        /* TODO no idea what consequences this has */
    }
}

static void pico_ipv6_nd_check_rs_timer_expired(pico_time now, void *arg){
    struct pico_tree_node *index = NULL;
    struct pico_ipv6_link *link = NULL;
    struct pico_ipv6_route *route = NULL;

    IGNORE_PARAMETER(arg);

    pico_tree_foreach(index,&IPV6Links){
      link = index->keyValue;
      if(pico_ipv6_is_linklocal(link->address.addr)  && link->rs_retries < MAX_INITIAL_RTR_ADVERTISEMENTS && (link->rs_expire_time < now)) {
          route = pico_ipv6_gateway_by_dev(link->dev);
          if (route) {
              link->rs_retries++;
              pico_icmp6_router_solicitation(link->dev,&link->address, &route->gateway);
          }
          link->rs_expire_time = PICO_TIME_MS() + 4000;
      }
    }

    if (!pico_timer_add(1000, pico_ipv6_nd_check_rs_timer_expired, NULL)) {
        dbg("IPV6 ND: Failed to start check rs timer\n");
        /* TODO no idea what consequences this has */
    }
}

/* Public API */

struct pico_eth *pico_ipv6_get_neighbor(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = NULL;
    struct pico_ipv6_link *l = NULL;
    if (!f)
        return NULL;

    hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    /* If we are still probing for Duplicate Address, abort now. */
    if (pico_ipv6_link_istentative(&hdr->src))
        return NULL;

    /* address belongs to ourselves? */
    l = pico_ipv6_link_get(&hdr->dst);
    if (l && !l->dev->mode)
        return &l->dev->eth->mac;
    else if (l && PICO_DEV_IS_6LOWPAN(l->dev))
        return (struct pico_eth *)l->dev->eth;

    return pico_nd_get(&hdr->dst, f->dev);
}

static struct pico_frame * pico_nd_get_oldest_frame(struct pico_frame **frames, int number_of_frames)
{
    int i = 0;
    struct pico_frame *oldest = NULL;

    if (!frames || number_of_frames <= 0)
        return NULL;

    oldest = frames[0];
    for (i = 1; i < number_of_frames; ++i) {
        if (frames[i]->timestamp < oldest->timestamp) {
            oldest = frames[i];
        }
    }

    return oldest;
}

void pico_ipv6_nd_postpone(struct pico_frame *f)
{
    struct pico_ipv6_neighbor *n = NULL;
    struct pico_ipv6_hdr *hdr = NULL;
    struct pico_ip6 *dst;
    struct pico_frame *cp = pico_frame_copy(f);

    hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    dst = &hdr->dst;

    n = pico_get_neighbor_from_ncache(dst);

    if (n) {
        if (n->frames_queued < PICO_ND_MAX_FRAMES_QUEUED) {
            if (pico_tree_insert(&IPV6NQueue, cp)) {
                nd_dbg("Could not insert frame in Queued frames tree\n");
                PICO_FREE(cp);
                return;
            } else {
                nd_dbg("PACKET INSERTED\n");
            }
            n->frames_queued++;
        } else {
            int i = 0;
            struct pico_frame *frames[PICO_ND_MAX_FRAMES_QUEUED] = {0};

            struct pico_frame *oldest = NULL;
            struct pico_tree_node *index = NULL;
            struct pico_frame *frame = NULL;
            struct pico_ipv6_hdr *frame_hdr = NULL;

            /* Get frames with dest addr == dst */
            pico_tree_foreach(index,&IPV6NQueue) {
                frame = index->keyValue;
                frame_hdr = (struct pico_ipv6_hdr *)frame->net_hdr;
                if (pico_ipv6_compare(dst, &frame_hdr->dst) == 0) {
                    frames[i++] = frame;
                }
            }

            /* Get the oldest frame*/
            oldest = pico_nd_get_oldest_frame(frames, PICO_ND_MAX_FRAMES_QUEUED);

            /* Delete oldest frame... */
            pico_tree_delete(&IPV6NQueue, oldest);
            pico_frame_discard(oldest);

            /* ...replace it with the newly recvd one */
            pico_tree_insert(&IPV6NQueue, cp);
        }
    } else {
        n = pico_nd_add(&dst, f->dev);

        if (n) {
            n->frames_queued++;

            /*
             * Insert the packet in the tree
             * Discovery should have started
             */
            pico_tree_insert(&IPV6NQueue, cp);
        } else {
            nd_dbg("Could not add NCE to postpone frame\n");
        }
    }
}

int pico_ipv6_nd_recv(struct pico_frame *f)
{
    struct pico_icmp6_hdr *hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    int ret = -1;

    switch(hdr->type) {
    case PICO_ICMP6_ROUTER_SOL:
        nd_dbg("ICMP6: received ROUTER SOL\n");
        ret = pico_nd_router_sol_recv(f);
        break;

    case PICO_ICMP6_ROUTER_ADV:
        nd_dbg("ICMP6: received ROUTER_ADV\n");
        ret = pico_nd_router_adv_recv(f);
        break;

    case PICO_ICMP6_NEIGH_SOL:
        nd_dbg("ICMP6: received NEIGH SOL\n");
        ret = pico_nd_neigh_sol_recv(f);
        break;

    case PICO_ICMP6_NEIGH_ADV:
        nd_dbg("ICMP6: received NEIGH ADV\n");
        ret = pico_nd_neigh_adv_recv(f);
        break;

    case PICO_ICMP6_REDIRECT:
        nd_dbg("ICMP6: received REDIRECT\n");
        ret = pico_nd_redirect_recv(f);
        break;
    }
    pico_frame_discard(f);
    return ret;
}

void pico_ipv6_nd_init(void)
{
    uint32_t nd_timer_id = 0, ra_timer_id = 0, router_lifetime_id = 0, check_lifetime_id = 0;

    nd_timer_id = pico_timer_add(200, pico_ipv6_nd_timer_callback, NULL);
    if (!nd_timer_id) {
        nd_dbg("IPv6 ND: Failed to start callback timer\n");
        goto fail_init;
    }

    ra_timer_id = pico_timer_add(200, pico_ipv6_nd_ra_timer_callback, NULL);
    if (!ra_timer_id) {
        nd_dbg("IPv6 ND: Failed to start RA callback timer\n");
        goto fail_ra_timer;
    }

    router_lifetime_id = pico_timer_add(200, pico_ipv6_check_router_lifetime_callback, NULL);
    if (!router_lifetime_id)
    {
        nd_dbg("IPv6 ND: Failed to start check_router_lifetime timer\n");
        goto fail_router_lifetime_timer;
    }

    check_lifetime_id = pico_timer_add(1000, pico_ipv6_check_lifetime_expired, NULL);
    if (!check_lifetime_id) {
        nd_dbg("IPv6 ND: Failed to start check_lifetime timer\n");
        goto fail_check_lifetime;
    }

    if (!pico_timer_add(1000, pico_ipv6_nd_check_rs_timer_expired, NULL)) {
        nd_dbg("IPv6 ND: Failed to start check_rs_timer_expired timer\n");
        goto fail_check_rs_timer;
    }

    return;

fail_check_rs_timer:
    pico_timer_cancel(check_lifetime_id);
fail_check_lifetime:
    pico_timer_cancel(router_lifetime_id);
fail_router_lifetime_timer:
    pico_timer_cancel(ra_timer_id);
fail_ra_timer:
    pico_timer_cancel(nd_timer_id);
fail_init:
    return;
}

#endif
