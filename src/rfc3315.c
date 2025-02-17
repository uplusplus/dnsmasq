/* dnsmasq is Copyright (c) 2000-2013 Simon Kelley

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 dated June, 1991, or
   (at your option) version 3 dated 29 June, 2007.
 
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
     
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "dnsmasq.h"

#ifdef HAVE_DHCP6

struct state {
  unsigned char *clid;
  int clid_len, iaid, ia_type, interface, hostname_auth;
  char *client_hostname, *hostname, *domain, *send_domain;
  struct dhcp_context *context;
  struct in6_addr *link_address;
  unsigned int xid, fqdn_flags;
  char *iface_name;
  void *packet_options, *end;
  struct dhcp_netid *tags, *context_tags;
#ifdef OPTION6_PREFIX_CLASS
  struct prefix_class *send_prefix_class;
#endif
};

static int dhcp6_maybe_relay(struct in6_addr *link_address, struct dhcp_netid **relay_tagsp, struct dhcp_context *context, 
			     int interface, char *iface_name, struct in6_addr *fallback, void *inbuff, size_t sz, int is_unicast, time_t now);
static int dhcp6_no_relay(int msg_type,  struct in6_addr *link_address, struct dhcp_netid *tags, struct dhcp_context *context, 
			  int interface, char *iface_name, struct in6_addr *fallback, void *inbuff, size_t sz, int is_unicast, time_t now);
static void log6_opts(int nest, unsigned int xid, void *start_opts, void *end_opts);
static void log6_packet(struct state *state, char *type, struct in6_addr *addr, char *string);

static void *opt6_find (void *opts, void *end, unsigned int search, unsigned int minsize);
static void *opt6_next(void *opts, void *end);
static unsigned int opt6_uint(unsigned char *opt, int offset, int size);
static void get_context_tag(struct state *state, struct dhcp_context *context);
static int check_ia(struct state *state, void *opt, void **endp, void **ia_option);
static int build_ia(struct state *state, int *t1cntr);
static void end_ia(int t1cntr, unsigned int min_time, int do_fuzz);
#ifdef OPTION6_PREFIX_CLASS
static struct prefix_class *prefix_class_from_context(struct dhcp_context *context);
#endif
static void mark_context_used(struct state *state, struct dhcp_context *context, struct in6_addr *addr);
static void mark_config_used(struct dhcp_context *context, struct in6_addr *addr);
static int check_address(struct state *state, struct in6_addr *addr);
static void add_address(struct state *state, struct dhcp_context *context, unsigned int lease_time, unsigned int requested_time, 
			unsigned int *min_time, struct in6_addr *addr, int update_lease, time_t now);
static void update_leases(struct state *state, struct dhcp_context *context, struct in6_addr *addr, unsigned int lease_time, time_t now);
static int add_local_addrs(struct dhcp_context *context);
struct dhcp_netid *add_options(struct state *state, struct in6_addr *fallback, struct dhcp_context *context);
static void calculate_times(struct dhcp_context *context, unsigned int *min_time, unsigned int *valid_timep, 
			    unsigned int *preferred_timep, unsigned int lease_time, unsigned int requested_time);

#define opt6_len(opt) ((int)(opt6_uint(opt, -2, 2)))
#define opt6_type(opt) (opt6_uint(opt, -4, 2))
#define opt6_ptr(opt, i) ((void *)&(((unsigned char *)(opt))[4+(i)]))


unsigned short dhcp6_reply(struct dhcp_context *context, int interface, char *iface_name,
			   struct in6_addr *fallback, size_t sz, int is_unicast, time_t now)
{
  struct dhcp_netid *relay_tags = NULL;
  struct dhcp_vendor *vendor;
  int msg_type;
  
  if (sz <= 4)
    return 0;
  
  msg_type = *((unsigned char *)daemon->dhcp_packet.iov_base);
  
  /* Mark these so we only match each at most once, to avoid tangled linked lists */
  for (vendor = daemon->dhcp_vendors; vendor; vendor = vendor->next)
    vendor->netid.next = &vendor->netid;
  
  save_counter(0);
  
  if (dhcp6_maybe_relay(NULL, &relay_tags, context, interface, iface_name, fallback, daemon->dhcp_packet.iov_base, sz, is_unicast, now))
    return msg_type == DHCP6RELAYFORW ? DHCPV6_SERVER_PORT : DHCPV6_CLIENT_PORT;

  return 0;
}

/* This cost me blood to write, it will probably cost you blood to understand - srk. */
static int dhcp6_maybe_relay(struct in6_addr *link_address, struct dhcp_netid **relay_tagsp, struct dhcp_context *context,
			     int interface, char *iface_name, struct in6_addr *fallback, void *inbuff, size_t sz, int is_unicast, time_t now)
{
  void *end = inbuff + sz;
  void *opts = inbuff + 34;
  int msg_type = *((unsigned char *)inbuff);
  unsigned char *outmsgtypep;
  void *opt;
  struct dhcp_vendor *vendor;

  /* if not an encaplsulated relayed message, just do the stuff */
  if (msg_type != DHCP6RELAYFORW)
    {
      /* if link_address != NULL if points to the link address field of the 
	 innermost nested RELAYFORW message, which is where we find the
	 address of the network on which we can allocate an address.
	 Recalculate the available contexts using that information. */
      
      if (link_address)
	{
	  struct dhcp_context *c;
	  context = NULL;
	   
	  if (!IN6_IS_ADDR_LOOPBACK(link_address) &&
	      !IN6_IS_ADDR_LINKLOCAL(link_address) &&
	      !IN6_IS_ADDR_MULTICAST(link_address))
	    for (c = daemon->dhcp6; c; c = c->next)
	      if ((c->flags & CONTEXT_DHCP) &&
		  !(c->flags & CONTEXT_TEMPLATE) &&
		  is_same_net6(link_address, &c->start6, c->prefix) &&
		  is_same_net6(link_address, &c->end6, c->prefix))
		{
		  c->current = context;
		  context = c;
		}
	  
	  if (!context)
	    {
	      inet_ntop(AF_INET6, link_address, daemon->addrbuff, ADDRSTRLEN); 
	      my_syslog(MS_DHCP | LOG_WARNING, 
			_("no address range available for DHCPv6 request from relay at %s"),
			daemon->addrbuff);
	      return 0;
	    }
	}

      if (!context)
	{
	  my_syslog(MS_DHCP | LOG_WARNING, 
		    _("no address range available for DHCPv6 request via %s"), iface_name);
	  return 0;
	}

      return dhcp6_no_relay(msg_type, link_address, *relay_tagsp, context, interface, iface_name, fallback, inbuff, sz, is_unicast, now);
    }

  /* must have at least msg_type+hopcount+link_address+peer_address+minimal size option
     which is               1   +    1   +    16      +     16     + 2 + 2 = 38 */
  if (sz < 38)
    return 0;
  
  /* copy header stuff into reply message and set type to reply */
  outmsgtypep = put_opt6(inbuff, 34);
  *outmsgtypep = DHCP6RELAYREPL;

  /* look for relay options and set tags if found. */
  for (vendor = daemon->dhcp_vendors; vendor; vendor = vendor->next)
    {
      int mopt;
      
      if (vendor->match_type == MATCH_SUBSCRIBER)
	mopt = OPTION6_SUBSCRIBER_ID;
      else if (vendor->match_type == MATCH_REMOTE)
	mopt = OPTION6_REMOTE_ID; 
      else
	continue;

      if ((opt = opt6_find(opts, end, mopt, 1)) &&
	  vendor->len == opt6_len(opt) &&
	  memcmp(vendor->data, opt6_ptr(opt, 0), vendor->len) == 0 &&
	  vendor->netid.next != &vendor->netid)
	{
	  vendor->netid.next = *relay_tagsp;
	  *relay_tagsp = &vendor->netid;
	  break;
	}
    }
  
  for (opt = opts; opt; opt = opt6_next(opt, end))
    {
      int o = new_opt6(opt6_type(opt));
      if (opt6_type(opt) == OPTION6_RELAY_MSG)
	{
	  struct in6_addr link_address;
	  /* the packet data is unaligned, copy to aligned storage */
	  memcpy(&link_address, inbuff + 2, IN6ADDRSZ); 
	  /* Not, zero is_unicast since that is now known to refer to the 
	     relayed packet, not the original sent by the client */
	  if (!dhcp6_maybe_relay(&link_address, relay_tagsp, context, interface, iface_name, fallback, opt6_ptr(opt, 0), opt6_len(opt), 0, now))
	    return 0;
	}
      else
	put_opt6(opt6_ptr(opt, 0), opt6_len(opt));
      end_opt6(o);	    
    }
  
  return 1;
}

static int dhcp6_no_relay(int msg_type, struct in6_addr *link_address, struct dhcp_netid *tags, struct dhcp_context *context, 
			  int interface, char *iface_name, struct in6_addr *fallback, void *inbuff, size_t sz, int is_unicast, time_t now)
{
  void *opt;
  int i, o, o1, start_opts;
  struct dhcp_opt *opt_cfg;
  struct dhcp_netid *tagif;
  struct dhcp_config *config = NULL;
  struct dhcp_netid known_id, iface_id, v6_id;
  unsigned char *outmsgtypep;
  struct dhcp_vendor *vendor;
  struct dhcp_context *context_tmp;
  unsigned int ignore = 0;
  struct state state;
#ifdef OPTION6_PREFIX_CLASS
  struct prefix_class *p;
  int dump_all_prefix_classes = 0;
#endif

  state.packet_options = inbuff + 4;
  state.end = inbuff + sz;
  state.clid = NULL;
  state.clid_len = 0;
  state.context_tags = NULL;
  state.tags = tags;
  state.link_address = link_address;
  state.interface = interface;
  state.domain = NULL;
  state.send_domain = NULL;
  state.context = context;
  state.hostname_auth = 0;
  state.hostname = NULL;
  state.client_hostname = NULL;
  state.iface_name = iface_name;
  state.fqdn_flags = 0x01; /* default to send if we recieve no FQDN option */
#ifdef OPTION6_PREFIX_CLASS
  state.send_prefix_class = NULL;
#endif

  /* set tag with name == interface */
  iface_id.net = iface_name;
  iface_id.next = state.tags;
  state.tags = &iface_id; 

  /* set tag "dhcpv6" */
  v6_id.net = "dhcpv6";
  v6_id.next = state.tags;
  state.tags = &v6_id;

  /* copy over transaction-id, and save pointer to message type */
  outmsgtypep = put_opt6(inbuff, 4);
  start_opts = save_counter(-1);
  state.xid = outmsgtypep[3] | outmsgtypep[2] << 8 | outmsgtypep[1] << 16;
   
  /* We're going to be linking tags from all context we use. 
     mark them as unused so we don't link one twice and break the list */
  for (context_tmp = context; context_tmp; context_tmp = context_tmp->current)
    {
      context->netid.next = &context->netid;

      if (option_bool(OPT_LOG_OPTS))
	{
	   inet_ntop(AF_INET6, &context_tmp->start6, daemon->dhcp_buff, ADDRSTRLEN); 
	   inet_ntop(AF_INET6, &context_tmp->end6, daemon->dhcp_buff2, ADDRSTRLEN); 
	   if (context_tmp->flags & (CONTEXT_STATIC))
	     my_syslog(MS_DHCP | LOG_INFO, _("%u available DHCPv6 subnet: %s/%d"),
		       state.xid, daemon->dhcp_buff, context_tmp->prefix);
	   else
	     my_syslog(MS_DHCP | LOG_INFO, _("%u available DHCP range: %s -- %s"), 
		       state.xid, daemon->dhcp_buff, daemon->dhcp_buff2);
	}
    }

  if ((opt = opt6_find(state.packet_options, state.end, OPTION6_CLIENT_ID, 1)))
    {
      state.clid = opt6_ptr(opt, 0);
      state.clid_len = opt6_len(opt);
      o = new_opt6(OPTION6_CLIENT_ID);
      put_opt6(state.clid, state.clid_len);
      end_opt6(o);
    }
  else if (msg_type != DHCP6IREQ)
    return 0;

  /* server-id must match except for SOLICIT and CONFIRM messages */
  if (msg_type != DHCP6SOLICIT && msg_type != DHCP6CONFIRM && msg_type != DHCP6IREQ &&
      (!(opt = opt6_find(state.packet_options, state.end, OPTION6_SERVER_ID, 1)) ||
       opt6_len(opt) != daemon->duid_len ||
       memcmp(opt6_ptr(opt, 0), daemon->duid, daemon->duid_len) != 0))
    return 0;
  
  o = new_opt6(OPTION6_SERVER_ID);
  put_opt6(daemon->duid, daemon->duid_len);
  end_opt6(o);

  if (is_unicast &&
      (msg_type == DHCP6REQUEST || msg_type == DHCP6RENEW || msg_type == DHCP6RELEASE || msg_type == DHCP6DECLINE))
    
    {  
      o1 = new_opt6(OPTION6_STATUS_CODE);
      put_opt6_short(DHCP6USEMULTI);
      put_opt6_string("Use multicast");
      end_opt6(o1);
      return 1;
    }

  /* match vendor and user class options */
  for (vendor = daemon->dhcp_vendors; vendor; vendor = vendor->next)
    {
      int mopt;
      
      if (vendor->match_type == MATCH_VENDOR)
	mopt = OPTION6_VENDOR_CLASS;
      else if (vendor->match_type == MATCH_USER)
	mopt = OPTION6_USER_CLASS; 
      else
	continue;

      if ((opt = opt6_find(state.packet_options, state.end, mopt, 2)))
	{
	  void *enc_opt, *enc_end = opt6_ptr(opt, opt6_len(opt));
	  int offset = 0;
	  
	  if (mopt == OPTION6_VENDOR_CLASS)
	    {
	      if (opt6_len(opt) < 4)
		continue;
	      
	      if (vendor->enterprise != opt6_uint(opt, 0, 4))
		continue;
	    
	      offset = 4;
	    }
 
	  for (enc_opt = opt6_ptr(opt, offset); enc_opt; enc_opt = opt6_next(enc_opt, enc_end))
	    for (i = 0; i <= (opt6_len(enc_opt) - vendor->len); i++)
	      if (memcmp(vendor->data, opt6_ptr(enc_opt, i), vendor->len) == 0)
		{
		  vendor->netid.next = state.tags;
		  state.tags = &vendor->netid;
		  break;
		}
	}
    }

  if (option_bool(OPT_LOG_OPTS) && (opt = opt6_find(state.packet_options, state.end, OPTION6_VENDOR_CLASS, 4)))
    my_syslog(MS_DHCP | LOG_INFO, _("%u vendor class: %u"), state.xid, opt6_uint(opt, 0, 4));
  
  /* dhcp-match. If we have hex-and-wildcards, look for a left-anchored match.
     Otherwise assume the option is an array, and look for a matching element. 
     If no data given, existance of the option is enough. This code handles 
     V-I opts too. */
  for (opt_cfg = daemon->dhcp_match6; opt_cfg; opt_cfg = opt_cfg->next)
    {
      int match = 0;
      
      if (opt_cfg->flags & DHOPT_RFC3925)
	{
	  for (opt = opt6_find(state.packet_options, state.end, OPTION6_VENDOR_OPTS, 4);
	       opt;
	       opt = opt6_find(opt6_next(opt, state.end), state.end, OPTION6_VENDOR_OPTS, 4))
	    {
	      void *vopt;
	      void *vend = opt6_ptr(opt, opt6_len(opt));
	      
	      for (vopt = opt6_find(opt6_ptr(opt, 4), vend, opt_cfg->opt, 0);
		   vopt;
		   vopt = opt6_find(opt6_next(vopt, vend), vend, opt_cfg->opt, 0))
		if ((match = match_bytes(opt_cfg, opt6_ptr(vopt, 0), opt6_len(vopt))))
		  break;
	    }
	  if (match)
	    break;
	}
      else
	{
	  if (!(opt = opt6_find(state.packet_options, state.end, opt_cfg->opt, 1)))
	    continue;
	  
	  match = match_bytes(opt_cfg, opt6_ptr(opt, 0), opt6_len(opt));
	} 
  
      if (match)
	{
	  opt_cfg->netid->next = state.tags;
	  state.tags = opt_cfg->netid;
	}
    }
  
  if ((opt = opt6_find(state.packet_options, state.end, OPTION6_FQDN, 1)))
    {
      /* RFC4704 refers */
       int len = opt6_len(opt) - 1;
       
       state.fqdn_flags = opt6_uint(opt, 0, 1);
       
       /* Always force update, since the client has no way to do it itself. */
       if (!option_bool(OPT_FQDN_UPDATE) && !(state.fqdn_flags & 0x01))
	 state.fqdn_flags |= 0x03;
 
       state.fqdn_flags &= ~0x04;

       if (len != 0 && len < 255)
	 {
	   unsigned char *pp, *op = opt6_ptr(opt, 1);
	   char *pq = daemon->dhcp_buff;
	   
	   pp = op;
	   while (*op != 0 && ((op + (*op)) - pp) < len)
	     {
	       memcpy(pq, op+1, *op);
	       pq += *op;
	       op += (*op)+1;
	       *(pq++) = '.';
	     }
	   
	   if (pq != daemon->dhcp_buff)
	     pq--;
	   *pq = 0;
	   
	   if (legal_hostname(daemon->dhcp_buff))
	     {
	       state.client_hostname = daemon->dhcp_buff;
	       if (option_bool(OPT_LOG_OPTS))
		 my_syslog(MS_DHCP | LOG_INFO, _("%u client provides name: %s"), state.xid, state.client_hostname); 
	     }
	 }
    }	 
  
  if (state.clid)
    {
      config = find_config6(daemon->dhcp_conf, context, state.clid, state.clid_len, NULL);
      
      if (have_config(config, CONFIG_NAME))
	{
	  state.hostname = config->hostname;
	  state.domain = config->domain;
	  state.hostname_auth = 1;
	}
      else if (state.client_hostname)
	{
	  state.domain = strip_hostname(state.client_hostname);
	  
	  if (strlen(state.client_hostname) != 0)
	    {
	      state.hostname = state.client_hostname;
	      if (!config)
		{
		  /* Search again now we have a hostname. 
		     Only accept configs without CLID here, (it won't match)
		     to avoid impersonation by name. */
		  struct dhcp_config *new = find_config6(daemon->dhcp_conf, context, NULL, 0, state.hostname);
		  if (new && !have_config(new, CONFIG_CLID) && !new->hwaddr)
		    config = new;
		}
	    }
	}
    }

  if (config)
    {
      struct dhcp_netid_list *list;
      
      for (list = config->netid; list; list = list->next)
        {
          list->list->next = state.tags;
          state.tags = list->list;
        }

      /* set "known" tag for known hosts */
      known_id.net = "known";
      known_id.next = state.tags;
      state.tags = &known_id;

      if (have_config(config, CONFIG_DISABLE))
	ignore = 1;
    }

#ifdef OPTION6_PREFIX_CLASS
  /* OPTION_PREFIX_CLASS in ORO, send addresses in all prefix classes */
  if (daemon->prefix_classes && (msg_type == DHCP6SOLICIT || msg_type == DHCP6REQUEST))
    {
      void *oro;
      
      if ((oro = opt6_find(state.packet_options, state.end, OPTION6_ORO, 0)))
	for (i = 0; i <  opt6_len(oro) - 1; i += 2)
	  if (opt6_uint(oro, i, 2) == OPTION6_PREFIX_CLASS)
	    {
	      dump_all_prefix_classes = 1;
	      break;
	    }
      
      if (msg_type != DHCP6SOLICIT || dump_all_prefix_classes)
	/* Add the tags associated with prefix classes so we can use the DHCP ranges.
	   Not done for SOLICIT as we add them  one-at-time. */
	for (p = daemon->prefix_classes; p ; p = p->next)
	  {
	    p->tag.next = state.tags;
	    state.tags = &p->tag;
	  }
    }    
#endif

  tagif = run_tag_if(state.tags);
  
  /* if all the netids in the ignore list are present, ignore this client */
  if (daemon->dhcp_ignore)
    {
      struct dhcp_netid_list *id_list;
     
      for (id_list = daemon->dhcp_ignore; id_list; id_list = id_list->next)
	if (match_netid(id_list->list, tagif, 0))
	  ignore = 1;
    }
  
  /* if all the netids in the ignore_name list are present, ignore client-supplied name */
  if (!state.hostname_auth)
    {
       struct dhcp_netid_list *id_list;
       
       for (id_list = daemon->dhcp_ignore_names; id_list; id_list = id_list->next)
	 if ((!id_list->list) || match_netid(id_list->list, tagif, 0))
	   break;
       if (id_list)
	 state.hostname = NULL;
    }
  

  switch (msg_type)
    {
    default:
      return 0;


    case DHCP6SOLICIT:
      {
	void *rapid_commit = opt6_find(state.packet_options, state.end, OPTION6_RAPID_COMMIT, 0);
      	int address_assigned = 0;
	/* tags without all prefix-class tags */
	struct dhcp_netid *solicit_tags = tagif;
	struct dhcp_context *c;

	if (rapid_commit)
	  {
	    o = new_opt6(OPTION6_RAPID_COMMIT);
	    end_opt6(o);
	  }

	/* set reply message type */
	*outmsgtypep = rapid_commit ? DHCP6REPLY : DHCP6ADVERTISE;

	log6_packet(&state, "DHCP6SOLICIT", NULL, ignore ? _("ignored") : NULL);
	
	if (ignore)
	  return 0;
	
	/* reset USED bits in leases */
	lease6_reset();

	/* Can use configured address max once per prefix */
	for (c = context; c; c = c->current)
	  c->flags &= ~CONTEXT_CONF_USED;

	for (opt = state.packet_options; opt; opt = opt6_next(opt, state.end))
	  {   
	    void *ia_option, *ia_end;
	    unsigned int min_time = 0xffffffff;
	    int t1cntr;
	    int ia_counter;
	    /* set unless we're sending a particular prefix-class, when we
	       want only dhcp-ranges with the correct tags set and not those without any tags. */
	    int plain_range = 1;
	    u32 lease_time, requested_time;
	    struct dhcp_lease *ltmp;
	    struct in6_addr *req_addr;
	    struct in6_addr addr;

	    if (!check_ia(&state, opt, &ia_end, &ia_option))
	      continue;
	    
	    /* reset USED bits in contexts - one address per prefix per IAID */
	    for (c = context; c; c = c->current)
	      c->flags &= ~CONTEXT_USED;

#ifdef OPTION6_PREFIX_CLASS
	    if (daemon->prefix_classes && state.ia_type == OPTION6_IA_NA)
	      {
		void *prefix_opt;
		int prefix_class;
		
		if (dump_all_prefix_classes)
		  /* OPTION_PREFIX_CLASS in ORO, send addresses in all prefix classes */
		  plain_range = 0;
		else 
		  { 
		    if ((prefix_opt = opt6_find(opt6_ptr(opt, 12), ia_end, OPTION6_PREFIX_CLASS, 2)))
		      {
			
			prefix_class = opt6_uint(prefix_opt, 0, 2);
			
			for (p = daemon->prefix_classes; p ; p = p->next)
			  if (p->class == prefix_class)
			    break;
			
			if (!p)
			  my_syslog(MS_DHCP | LOG_WARNING, _("unknown prefix-class %d"), prefix_class);
			else
			  {
			    /* add tag to list, and exclude undecorated dhcp-ranges */
			    p->tag.next = state.tags;
			    solicit_tags = run_tag_if(&p->tag);
			    plain_range = 0;
			    state.send_prefix_class = p;
			  }
		      }
		    else
		      {
			/* client didn't ask for a prefix class, lets see if we can find one. */
			for (p = daemon->prefix_classes; p ; p = p->next)
			  {
			    p->tag.next = NULL;
			    if (match_netid(&p->tag, solicit_tags, 1))
			      break;
			  }
			
			if (p)
			  {
			    plain_range = 0;
			    state.send_prefix_class = p;
			  }
		      }

		    if (p && option_bool(OPT_LOG_OPTS))
		      my_syslog(MS_DHCP | LOG_INFO, "%u prefix class %d tag:%s", state.xid, p->class, p->tag.net); 
		  }
	      }
#endif

	    o = build_ia(&state, &t1cntr);
#ifndef RANDOM_IP_ADDRESS
	    for (ia_counter = 0; ia_option; ia_counter++, ia_option = opt6_find(opt6_next(ia_option, ia_end), ia_end, OPTION6_IAADDR, 24))
	      {
		req_addr = opt6_ptr(ia_option, 0);
		requested_time = opt6_uint(ia_option, 16, 4);
		
		if ((c = address6_valid(context, req_addr, solicit_tags, plain_range)))
		  {
		    lease_time = c->lease_time;
		    /* If the client asks for an address on the same network as a configured address, 
		       offer the configured address instead, to make moving to newly-configured
		       addresses automatic. */
		    if (!(c->flags & CONTEXT_CONF_USED) && config_valid(config, c, &addr) && check_address(&state, &addr))
		      {
			req_addr = &addr;
			mark_config_used(c, &addr);
			if (have_config(config, CONFIG_TIME))
			  lease_time = config->lease_time;
		      }
		    else if (!(c = address6_available(context, req_addr, solicit_tags, plain_range)))
		      continue; /* not an address we're allowed */
		    else if (!check_address(&state, req_addr))
		      continue; /* address leased elsewhere */
		    
		    /* add address to output packet */
#ifdef OPTION6_PREFIX_CLASS
		    if (dump_all_prefix_classes && state.ia_type == OPTION6_IA_NA)
		      state.send_prefix_class = prefix_class_from_context(c);
#endif		    
		    add_address(&state, c, lease_time, requested_time, &min_time, req_addr, rapid_commit != NULL, now);
		    mark_context_used(&state, context, req_addr);
		    get_context_tag(&state, c);
		    address_assigned = 1;
		  }
	      }
	    
	    /* Suggest configured address(es) */
	    for (c = context; c; c = c->current) 
	      if (!(c->flags & CONTEXT_CONF_USED) &&
		  match_netid(c->filter, solicit_tags, plain_range) &&
		  config_valid(config, c, &addr) && 
		  check_address(&state, &addr))
		{
		  mark_config_used(context, &addr);
		  if (have_config(config, CONFIG_TIME))
		    lease_time = config->lease_time;
		  else
		    lease_time = c->lease_time;
		  /* add address to output packet */
#ifdef OPTION6_PREFIX_CLASS
		  if (dump_all_prefix_classes && state.ia_type == OPTION6_IA_NA)
		    state.send_prefix_class = prefix_class_from_context(c);
#endif
		  add_address(&state, c, lease_time, requested_time, &min_time, &addr, rapid_commit != NULL, now);
		  mark_context_used(&state, context, &addr);
		  get_context_tag(&state, c);
		  address_assigned = 1;
		}
	    
	    /* return addresses for existing leases */
	    ltmp = NULL;
	    while ((ltmp = lease6_find_by_client(ltmp, state.ia_type == OPTION6_IA_NA ? LEASE_NA : LEASE_TA, state.clid, state.clid_len, state.iaid)))
	      {
		req_addr = (struct in6_addr *)ltmp->hwaddr;
		if ((c = address6_available(context, req_addr, solicit_tags, plain_range)))
		  {
#ifdef OPTION6_PREFIX_CLASS
		    if (dump_all_prefix_classes && state.ia_type == OPTION6_IA_NA)
		      state.send_prefix_class = prefix_class_from_context(c);
#endif
		    add_address(&state, c, c->lease_time, c->lease_time, &min_time, req_addr, rapid_commit != NULL, now);
		    mark_context_used(&state, context, req_addr);
		    get_context_tag(&state, c);
		    address_assigned = 1;
		  }
	      }
#endif 	//not define RANDOM_IP_ADDRESS   
	    /* Return addresses for all valid contexts which don't yet have one */
	    while ((c = address6_allocate(context, state.clid, state.clid_len, state.iaid, ia_counter, solicit_tags, plain_range, &addr)))
	      {
#ifdef OPTION6_PREFIX_CLASS
		if (dump_all_prefix_classes && state.ia_type == OPTION6_IA_NA)
		  state.send_prefix_class = prefix_class_from_context(c);
#endif
		add_address(&state, c, c->lease_time, c->lease_time, &min_time, &addr, rapid_commit != NULL, now);
		mark_context_used(&state, context, &addr);
		get_context_tag(&state, c);
		address_assigned = 1;
	      }
	    
	    end_ia(t1cntr, min_time, 0);
	    end_opt6(o);	
	  }

	if (address_assigned) 
	  {
	    o1 = new_opt6(OPTION6_STATUS_CODE);
	    put_opt6_short(DHCP6SUCCESS);
	    put_opt6_string(_("success"));
	    end_opt6(o1);
	    
	    /* If --dhcp-authoritative is set, we can tell client not to wait for
	       other possible servers */
	    o = new_opt6(OPTION6_PREFERENCE);
	    put_opt6_char(option_bool(OPT_AUTHORITATIVE) ? 255 : 0);
	    end_opt6(o);
	    tagif = add_options(&state, fallback, context);
	  }
	else
	  { 
	    /* no address, return error */
	    o1 = new_opt6(OPTION6_STATUS_CODE);
	    put_opt6_short(DHCP6NOADDRS);
	    put_opt6_string(_("no addresses available"));
	    end_opt6(o1);
	    log6_packet(&state, "DHCPADVERTISE", NULL, _("no addresses available"));
	  }

	break;
      }
      
    case DHCP6REQUEST:
      {
	int address_assigned = 0;
	
	/* set reply message type */
	*outmsgtypep = DHCP6REPLY;

	log6_packet(&state, "DHCPREQUEST", NULL, ignore ? _("ignored") : NULL);
	
	if (ignore)
	  return 0;
	
	for (opt = state.packet_options; opt; opt = opt6_next(opt, state.end))
	  {   
	    void *ia_option, *ia_end;
	    unsigned int min_time = 0xffffffff;
	    int t1cntr;
	    
	     if (!check_ia(&state, opt, &ia_end, &ia_option))
	       continue;
	    
	    o = build_ia(&state, &t1cntr);
	      
	    for (; ia_option; ia_option = opt6_find(opt6_next(ia_option, ia_end), ia_end, OPTION6_IAADDR, 24))
	      {
		struct in6_addr *req_addr = opt6_ptr(ia_option, 0);
		u32 requested_time = opt6_uint(ia_option, 16, 4);
		struct dhcp_context *dynamic, *c;
		unsigned int lease_time;
		struct in6_addr addr;
		int config_ok = 0;
		
		if ((c = address6_valid(context, req_addr, tagif, 1)))
		  config_ok = config_valid(config, c, &addr) && IN6_ARE_ADDR_EQUAL(&addr, req_addr);
		
		if ((dynamic = address6_available(context, req_addr, tagif, 1)) || c)
		  {
		    if (!dynamic && !config_ok)
		      {
			/* Static range, not configured. */
			o1 = new_opt6(OPTION6_STATUS_CODE);
			put_opt6_short(DHCP6UNSPEC);
			put_opt6_string(_("address unavailable"));
			end_opt6(o1);
		      }
		    else if (!check_address(&state, req_addr))
		      {
			/* Address leased to another DUID/IAID */
			o1 = new_opt6(OPTION6_STATUS_CODE);
			put_opt6_short(DHCP6UNSPEC);
			put_opt6_string(_("address in use"));
			end_opt6(o1);
		      }
#ifdef RANDOM_IP_ADDRESS
		    else if(!lease_is_alive(lease6_find(state.clid, state.clid_len,
					  state.ia_type == OPTION6_IA_NA ? LEASE_NA : LEASE_TA, 
					  state.iaid, req_addr), dnsmasq_time())){
				/* dead. */
				o1 = new_opt6(OPTION6_STATUS_CODE);
				put_opt6_short(DHCP6UNSPEC);
				put_opt6_string(_("address unavailable"));
				end_opt6(o1);
			}
#endif
		    else 
		      {
			if (!dynamic)
			  dynamic = c;

			lease_time = dynamic->lease_time;
			
			if (config_ok && have_config(config, CONFIG_TIME))
			  lease_time = config->lease_time;

#ifdef OPTION6_PREFIX_CLASS
			if (dump_all_prefix_classes && state.ia_type == OPTION6_IA_NA)
			  state.send_prefix_class = prefix_class_from_context(c);
#endif
			add_address(&state, dynamic, lease_time, requested_time, &min_time, req_addr, 1, now);
			get_context_tag(&state, dynamic);
			address_assigned = 1;
		      }
		  }
		else 
		  {
		    /* requested address not on the correct link */
		    o1 = new_opt6(OPTION6_STATUS_CODE);
		    put_opt6_short(DHCP6NOTONLINK);
		    put_opt6_string(_("not on link"));
		    end_opt6(o1);
		  }
	      }
	 
	    end_ia(t1cntr, min_time, 0);
	    end_opt6(o);	
	  }

	if (address_assigned) 
	  {
	    o1 = new_opt6(OPTION6_STATUS_CODE);
	    put_opt6_short(DHCP6SUCCESS);
	    put_opt6_string(_("success"));
	    end_opt6(o1);
	  }
	else
	  { 
	    /* no address, return error */
	    o1 = new_opt6(OPTION6_STATUS_CODE);
	    put_opt6_short(DHCP6NOADDRS);
	    put_opt6_string(_("no addresses available"));
	    end_opt6(o1);
	    log6_packet(&state, "DHCPREPLY", NULL, _("no addresses available"));
	  }

	tagif = add_options(&state, fallback, context);
	break;
      }
      
  
    case DHCP6RENEW:
      {
	/* set reply message type */
	*outmsgtypep = DHCP6REPLY;
	
	log6_packet(&state, "DHCPRENEW", NULL, NULL);

	for (opt = state.packet_options; opt; opt = opt6_next(opt, state.end))
	  {
	    void *ia_option, *ia_end;
	    unsigned int min_time = 0xffffffff;
	    int t1cntr, iacntr;
	    
	    if (!check_ia(&state, opt, &ia_end, &ia_option))
	      continue;
	    
	    o = build_ia(&state, &t1cntr);
	    iacntr = save_counter(-1); 
	    
	    for (; ia_option; ia_option = opt6_find(opt6_next(ia_option, ia_end), ia_end, OPTION6_IAADDR, 24))
	      {
		struct dhcp_lease *lease = NULL;
		struct in6_addr *req_addr = opt6_ptr(ia_option, 0);
		u32 requested_time = opt6_uint(ia_option, 16, 4);
		unsigned int preferred_time = 0; /* in case renewal inappropriate */
		unsigned int valid_time = 0;
		char *message = NULL;
		struct dhcp_context *this_context;
		
		if (!(lease = lease6_find(state.clid, state.clid_len,
					  state.ia_type == OPTION6_IA_NA ? LEASE_NA : LEASE_TA, 
					  state.iaid, req_addr)))
		  {
		    /* If the server cannot find a client entry for the IA the server
		       returns the IA containing no addresses with a Status Code option set
		       to NoBinding in the Reply message. */
		    save_counter(iacntr);
		    t1cntr = 0;
		    
		    log6_packet(&state, "DHCPREPLY", req_addr, _("lease not found"));
		    
		    o1 = new_opt6(OPTION6_STATUS_CODE);
		    put_opt6_short(DHCP6NOBINDING);
		    put_opt6_string(_("no binding found"));
		    end_opt6(o1);
		    break;
		  }
		
		
		if ((this_context = address6_available(context, req_addr, tagif, 1)) ||
		    (this_context = address6_valid(context, req_addr, tagif, 1)))
		  {
		    struct in6_addr addr;
		    unsigned int lease_time;

		    get_context_tag(&state, this_context);
		    
		    if (config_valid(config, this_context, &addr) && IN6_ARE_ADDR_EQUAL(&addr, req_addr) && have_config(config, CONFIG_TIME))
		      lease_time = config->lease_time;
		    else 
		      lease_time = this_context->lease_time;
		    
		    calculate_times(this_context, &min_time, &valid_time, &preferred_time, lease_time, requested_time); 
		    
		    lease_set_expires(lease, valid_time, now);
		    if (state.ia_type == OPTION6_IA_NA && state.hostname)
		      {
			char *addr_domain = get_domain6(req_addr);
			if (!state.send_domain)
			  state.send_domain = addr_domain;
			lease_set_hostname(lease, state.hostname, state.hostname_auth, addr_domain, state.domain); 
			message = state.hostname;
		      }
		    
		    
		    if (preferred_time == 0)
		      message = _("deprecated");
		  }
		else
		  message = _("address invalid");
		
		log6_packet(&state, "DHCPREPLY", req_addr, message);	
		
		o1 =  new_opt6(OPTION6_IAADDR);
		put_opt6(req_addr, sizeof(*req_addr));
		put_opt6_long(preferred_time);
		put_opt6_long(valid_time);
		end_opt6(o1);
	      }
	    
	    end_ia(t1cntr, min_time, 1);
	    end_opt6(o);
	  }
	
	tagif = add_options(&state, fallback, context);
	break;
	
      }
      
    case DHCP6CONFIRM:
      {
	/* set reply message type */
	*outmsgtypep = DHCP6REPLY;
	
	log6_packet(&state, "DHCPCONFIRM", NULL, NULL);
	
	for (opt = state.packet_options; opt; opt = opt6_next(opt, state.end))
	  {
	    void *ia_option, *ia_end;
	    
	    for (check_ia(&state, opt, &ia_end, &ia_option);
		 ia_option;
		 ia_option = opt6_find(opt6_next(ia_option, ia_end), ia_end, OPTION6_IAADDR, 24))
	      {
		struct in6_addr *req_addr = opt6_ptr(ia_option, 0);
		
		if (!address6_available(context, req_addr, tagif, 1))
		  {
		    o1 = new_opt6(OPTION6_STATUS_CODE);
		    put_opt6_short(DHCP6NOTONLINK);
		    put_opt6_string(_("confirm failed"));
		    end_opt6(o1);
		    return 1;
		  }

		log6_packet(&state, "DHCPREPLY", req_addr, state.hostname);
	      }
	  }	 

	o1 = new_opt6(OPTION6_STATUS_CODE);
	put_opt6_short(DHCP6SUCCESS );
	put_opt6_string(_("all addresses still on link"));
	end_opt6(o1);
	break;
    }
      
    case DHCP6IREQ:
      {
	/* We can't discriminate contexts based on address, as we don't know it.
	   If there is only one possible context, we can use its tags */
	if (context && context->netid.net && !context->current)
	  {
	    context->netid.next = NULL;
	    state.context_tags =  &context->netid;
	  }
	log6_packet(&state, "DHCPINFORMATION-REQUEST", NULL, ignore ? _("ignored") : state.hostname);
	if (ignore)
	  return 0;
	*outmsgtypep = DHCP6REPLY;
	tagif = add_options(&state, fallback, context);
	break;
      }
      
      
    case DHCP6RELEASE:
      {
	/* set reply message type */
	*outmsgtypep = DHCP6REPLY;

	log6_packet(&state, "DHCPRELEASE", NULL, NULL);

	for (opt = state.packet_options; opt; opt = opt6_next(opt, state.end))
	  {
	    void *ia_option, *ia_end;
	    int made_ia = 0;
	    	    
	    for (check_ia(&state, opt, &ia_end, &ia_option);
		 ia_option;
		 ia_option = opt6_find(opt6_next(ia_option, ia_end), ia_end, OPTION6_IAADDR, 24)) 
	      {
		struct dhcp_lease *lease;
		
		if ((lease = lease6_find(state.clid, state.clid_len, state.ia_type == OPTION6_IA_NA ? LEASE_NA : LEASE_TA,
					 state.iaid, opt6_ptr(ia_option, 0))))
		  lease_prune(lease, now);
		else
		  {
		    if (!made_ia)
		      {
			o = new_opt6(state.ia_type);
			put_opt6_long(state.iaid);
			if (state.ia_type == OPTION6_IA_NA)
			  {
			    put_opt6_long(0);
			    put_opt6_long(0); 
			  }
			made_ia = 1;
		      }
		    
		    o1 = new_opt6(OPTION6_IAADDR);
		    put_opt6(opt6_ptr(ia_option, 0), IN6ADDRSZ);
		    put_opt6_long(0);
		    put_opt6_long(0);
		    end_opt6(o1);
		  }
	      }
	    
	    if (made_ia)
	      {
		o1 = new_opt6(OPTION6_STATUS_CODE);
		put_opt6_short(DHCP6NOBINDING);
		put_opt6_string(_("no binding found"));
		end_opt6(o1);
		
		end_opt6(o);
	      }
	  }
	
	o1 = new_opt6(OPTION6_STATUS_CODE);
	put_opt6_short(DHCP6SUCCESS);
	put_opt6_string(_("release received"));
	end_opt6(o1);
	
	break;
      }

    case DHCP6DECLINE:
      {
	/* set reply message type */
	*outmsgtypep = DHCP6REPLY;
	
	log6_packet(&state, "DHCPDECLINE", NULL, NULL);

	for (opt = state.packet_options; opt; opt = opt6_next(opt, state.end))
	  {
	    void *ia_option, *ia_end;
	    int made_ia = 0;
	    	    
	    for (check_ia(&state, opt, &ia_end, &ia_option);
		 ia_option;
		 ia_option = opt6_find(opt6_next(ia_option, ia_end), ia_end, OPTION6_IAADDR, 24)) 
	      {
		struct dhcp_lease *lease;
		struct in6_addr *addrp = opt6_ptr(ia_option, 0);

		if (have_config(config, CONFIG_ADDR6) && IN6_ARE_ADDR_EQUAL(&config->addr6, addrp))
		  {
		    prettyprint_time(daemon->dhcp_buff3, DECLINE_BACKOFF);
		    inet_ntop(AF_INET6, addrp, daemon->addrbuff, ADDRSTRLEN);
		    my_syslog(MS_DHCP | LOG_WARNING, _("disabling DHCP static address %s for %s"), 
			      daemon->addrbuff, daemon->dhcp_buff3);
		    config->flags |= CONFIG_DECLINED;
		    config->decline_time = now;
		  }
		else
		  /* make sure this host gets a different address next time. */
		  for (; context; context = context->current)
		    context->addr_epoch++;
		
		if ((lease = lease6_find(state.clid, state.clid_len, state.ia_type == OPTION6_IA_NA ? LEASE_NA : LEASE_TA,
					 state.iaid, opt6_ptr(ia_option, 0))))
		  lease_prune(lease, now);
		else
		  {
		    if (!made_ia)
		      {
			o = new_opt6(state.ia_type);
			put_opt6_long(state.iaid);
			if (state.ia_type == OPTION6_IA_NA)
			  {
			    put_opt6_long(0);
			    put_opt6_long(0); 
			  }
			made_ia = 1;
		      }
		    
		    o1 = new_opt6(OPTION6_IAADDR);
		    put_opt6(opt6_ptr(ia_option, 0), IN6ADDRSZ);
		    put_opt6_long(0);
		    put_opt6_long(0);
		    end_opt6(o1);
		  }
	      }
	    
	    if (made_ia)
	      {
		o1 = new_opt6(OPTION6_STATUS_CODE);
		put_opt6_short(DHCP6NOBINDING);
		put_opt6_string(_("no binding found"));
		end_opt6(o1);
		
		end_opt6(o);
	      }
	    
	  }
	break;
      }

    }
  
  log_tags(tagif, state.xid);
  
  if (option_bool(OPT_LOG_OPTS))
    log6_opts(0, state.xid, daemon->outpacket.iov_base + start_opts, daemon->outpacket.iov_base + save_counter(-1));
  
  return 1;

}

struct dhcp_netid *add_options(struct state *state, struct in6_addr *fallback, struct dhcp_context *context)  
{
  void *oro;
  /* filter options based on tags, those we want get DHOPT_TAGOK bit set */
  struct dhcp_netid *tagif = option_filter(state->tags, state->context_tags, daemon->dhcp_opts6);
  struct dhcp_opt *opt_cfg;
  int done_dns = 0, do_encap = 0;
  int i, o, o1;

  oro = opt6_find(state->packet_options, state->end, OPTION6_ORO, 0);
  
  for (opt_cfg = daemon->dhcp_opts6; opt_cfg; opt_cfg = opt_cfg->next)
    {
      /* netids match and not encapsulated? */
      if (!(opt_cfg->flags & DHOPT_TAGOK))
	continue;
      
      if (!(opt_cfg->flags & DHOPT_FORCE) && oro)
	{
	  for (i = 0; i <  opt6_len(oro) - 1; i += 2)
	    if (opt6_uint(oro, i, 2) == (unsigned)opt_cfg->opt)
	      break;
	  
	  /* option not requested */
	  if (i >=  opt6_len(oro) - 1)
	    continue;
	}
      
      if (opt_cfg->opt == OPTION6_DNS_SERVER)
	{
	  done_dns = 1;
	  if (opt_cfg->len == 0)
	    continue;
	}
      
      o = new_opt6(opt_cfg->opt);
      if (opt_cfg->flags & DHOPT_ADDR6)
	{
	  int j;
	  struct in6_addr *a = (struct in6_addr *)opt_cfg->val;
          for (j = 0; j < opt_cfg->len; j+=IN6ADDRSZ, a++)
            {
              /* zero means "self" (but not in vendorclass options.) */
              if (IN6_IS_ADDR_UNSPECIFIED(a))
                {
		  if (!add_local_addrs(context))
		    put_opt6(fallback, IN6ADDRSZ);
		}
              else
                put_opt6(a, IN6ADDRSZ);
            }
	}
      else if (opt_cfg->val)
	put_opt6(opt_cfg->val, opt_cfg->len);
      end_opt6(o);
    }
  
  if (daemon->port == NAMESERVER_PORT && !done_dns && 
      (!IN6_IS_ADDR_UNSPECIFIED(&context->local6) ||
       !IN6_IS_ADDR_UNSPECIFIED(fallback)))
    {
      o = new_opt6(OPTION6_DNS_SERVER);
      if (!add_local_addrs(context))
	put_opt6(fallback, IN6ADDRSZ);
      end_opt6(o); 
    }
   
    /* handle vendor-identifying vendor-encapsulated options,
       dhcp-option = vi-encap:13,17,....... */
  for (opt_cfg = daemon->dhcp_opts6; opt_cfg; opt_cfg = opt_cfg->next)
    opt_cfg->flags &= ~DHOPT_ENCAP_DONE;
    
  if (oro)
    for (i = 0; i <  opt6_len(oro) - 1; i += 2)
      if (opt6_uint(oro, i, 2) == OPTION6_VENDOR_OPTS)
	do_encap = 1;
  
  for (opt_cfg = daemon->dhcp_opts6; opt_cfg; opt_cfg = opt_cfg->next)
    { 
      if (opt_cfg->flags & DHOPT_RFC3925)
	{
	  int found = 0;
	  struct dhcp_opt *oc;
	  
	  if (opt_cfg->flags & DHOPT_ENCAP_DONE)
	    continue;
	  
	  for (oc = daemon->dhcp_opts6; oc; oc = oc->next)
	    {
	      oc->flags &= ~DHOPT_ENCAP_MATCH;
	      
	      if (!(oc->flags & DHOPT_RFC3925) || opt_cfg->u.encap != oc->u.encap)
		continue;
	      
	      oc->flags |= DHOPT_ENCAP_DONE;
	      if (match_netid(oc->netid, tagif, 1))
		{
		  /* option requested/forced? */
		  if (!oro || do_encap || (oc->flags & DHOPT_FORCE))
		    {
		      oc->flags |= DHOPT_ENCAP_MATCH;
		      found = 1;
		    }
		} 
	    }
	  
	  if (found)
	    { 
	      o = new_opt6(OPTION6_VENDOR_OPTS);	      
	      put_opt6_long(opt_cfg->u.encap);	
	     
	      for (oc = daemon->dhcp_opts6; oc; oc = oc->next)
		if (oc->flags & DHOPT_ENCAP_MATCH)
		  {
		    o1 = new_opt6(oc->opt);
		    put_opt6(oc->val, oc->len);
		    end_opt6(o1);
		  }
	      end_opt6(o);
	    }
	}
    }      


  if (state->hostname)
    {
      unsigned char *p;
      size_t len = strlen(state->hostname);
      
      if (state->send_domain)
	len += strlen(state->send_domain) + 1;

      o = new_opt6(OPTION6_FQDN);
      if ((p = expand(len + 3)))
	{
	  *(p++) = state->fqdn_flags;
	  p = do_rfc1035_name(p, state->hostname);
	  if (state->send_domain)
	    p = do_rfc1035_name(p, state->send_domain);
	  *p = 0;
	}
      end_opt6(o);
    }


  /* logging */
  if (option_bool(OPT_LOG_OPTS) && oro)
    {
      char *q = daemon->namebuff;
      for (i = 0; i <  opt6_len(oro) - 1; i += 2)
	{
	  char *s = option_string(AF_INET6, opt6_uint(oro, i, 2), NULL, 0, NULL, 0);
	  q += snprintf(q, MAXDNAME - (q - daemon->namebuff),
			"%d%s%s%s", 
			opt6_uint(oro, i, 2),
			strlen(s) != 0 ? ":" : "",
			s, 
			(i > opt6_len(oro) - 3) ? "" : ", ");
	  if ( i >  opt6_len(oro) - 3 || (q - daemon->namebuff) > 40)
	    {
	      q = daemon->namebuff;
	      my_syslog(MS_DHCP | LOG_INFO, _("%u requested options: %s"), state->xid, daemon->namebuff);
	    }
	}
    } 

  return tagif;
}
 
static int add_local_addrs(struct dhcp_context *context)
{
  int done = 0;
  
  for (; context; context = context->current)
    if ((context->flags & CONTEXT_USED) && !IN6_IS_ADDR_UNSPECIFIED(&context->local6))
      {
	/* squash duplicates */
	struct dhcp_context *c;
	for (c = context->current; c; c = c->current)
	  if ((c->flags & CONTEXT_USED) &&
	      IN6_ARE_ADDR_EQUAL(&context->local6, &c->local6))
	    break;
	
	if (!c)
	  { 
	    done = 1;
	    put_opt6(&context->local6, IN6ADDRSZ);
	  }
      }

  return done;
}


static void get_context_tag(struct state *state, struct dhcp_context *context)
{
  /* get tags from context if we've not used it before */
  if (context->netid.next == &context->netid && context->netid.net)
    {
      context->netid.next = state->context_tags;
      state->context_tags = &context->netid;
      if (!state->hostname_auth)
	{
	  struct dhcp_netid_list *id_list;
	  
	  for (id_list = daemon->dhcp_ignore_names; id_list; id_list = id_list->next)
	    if ((!id_list->list) || match_netid(id_list->list, &context->netid, 0))
	      break;
	  if (id_list)
	    state->hostname = NULL;
	}
    }
} 

#ifdef OPTION6_PREFIX_CLASS
static struct prefix_class *prefix_class_from_context(struct dhcp_context *context)
{
  struct prefix_class *p;
  struct dhcp_netid *t;
  
  for (p = daemon->prefix_classes; p ; p = p->next)
    for (t = context->filter; t; t = t->next)
      if (strcmp(p->tag.net, t->net) == 0)
	return p;
  
 return NULL;
}
#endif

static int check_ia(struct state *state, void *opt, void **endp, void **ia_option)
{
  state->ia_type = opt6_type(opt);
  *ia_option = NULL;

  if (state->ia_type != OPTION6_IA_NA && state->ia_type != OPTION6_IA_TA)
    return 0;
  
  if (state->ia_type == OPTION6_IA_NA && opt6_len(opt) < 12)
    return 0;
	    
  if (state->ia_type == OPTION6_IA_TA && opt6_len(opt) < 4)
    return 0;
  
  *endp = opt6_ptr(opt, opt6_len(opt));
  state->iaid = opt6_uint(opt, 0, 4);
  *ia_option = opt6_find(opt6_ptr(opt, state->ia_type == OPTION6_IA_NA ? 12 : 4), *endp, OPTION6_IAADDR, 24);

  return 1;
}


static int build_ia(struct state *state, int *t1cntr)
{
  int  o = new_opt6(state->ia_type);
 
  put_opt6_long(state->iaid);
  *t1cntr = 0;
	    
  if (state->ia_type == OPTION6_IA_NA)
    {
      /* save pointer */
      *t1cntr = save_counter(-1);
      /* so we can fill these in later */
      put_opt6_long(0);
      put_opt6_long(0); 
    }

  return o;
}

static void end_ia(int t1cntr, unsigned int min_time, int do_fuzz)
{
  if (t1cntr != 0)
    {
      /* go back an fill in fields in IA_NA option */
      int sav = save_counter(t1cntr);
      unsigned int t1, t2, fuzz = 0;

      if (do_fuzz)
	{
	  fuzz = rand16();
      
	  while (fuzz > (min_time/16))
	    fuzz = fuzz/2;
	}
      
      t1 = (min_time == 0xffffffff) ? 0xffffffff : min_time/2 - fuzz;
      t2 = (min_time == 0xffffffff) ? 0xffffffff : ((min_time/8)*7) - fuzz;
      put_opt6_long(t1);
      put_opt6_long(t2);
      save_counter(sav);
    }	
}

static void add_address(struct state *state, struct dhcp_context *context, unsigned int lease_time, unsigned int requested_time, 
			unsigned int *min_time, struct in6_addr *addr, int do_update, time_t now)
{
  unsigned int valid_time, preferred_time;
  int o = new_opt6(OPTION6_IAADDR);
  struct dhcp_lease *lease;

  calculate_times(context, min_time, &valid_time, &preferred_time, lease_time, requested_time); 
  
  put_opt6(addr, sizeof(*addr));
  put_opt6_long(preferred_time);
  put_opt6_long(valid_time); 		    
  
#ifdef OPTION6_PREFIX_CLASS
  if (state->send_prefix_class)
    {
      int o1 = new_opt6(OPTION6_PREFIX_CLASS);
      put_opt6_short(state->send_prefix_class->class);
      end_opt6(o1);
    }
#endif

  end_opt6(o);
  
  if (do_update)
    update_leases(state, context, addr, valid_time, now);

  if ((lease = lease6_find_by_addr(addr, 128, 0)))
    lease->flags |= LEASE_USED;

  /* get tags from context if we've not used it before */
  if (context->netid.next == &context->netid && context->netid.net)
    {
      context->netid.next = state->context_tags;
      state->context_tags = &context->netid;
      
      if (!state->hostname_auth)
	{
	  struct dhcp_netid_list *id_list;
	  
	  for (id_list = daemon->dhcp_ignore_names; id_list; id_list = id_list->next)
	    if ((!id_list->list) || match_netid(id_list->list, &context->netid, 0))
	      break;
	  if (id_list)
	    state->hostname = NULL;
	}
    }

  log6_packet(state, do_update ? "DHCPREPLY" : "DHCPADVERTISE", addr, state->hostname);

}

static void mark_context_used(struct state *state, struct dhcp_context *context, struct in6_addr *addr)
{
  /* Mark that we have an address for this prefix. */
#ifdef OPTION6_PREFIX_CLASS
  for (; context; context = context->current)
    if (is_same_net6(addr, &context->start6, context->prefix) &&
	(!state->send_prefix_class || state->send_prefix_class == prefix_class_from_context(context)))
      context->flags |= CONTEXT_USED;
#else
  (void)state; /* warning */
  for (; context; context = context->current)
    if (is_same_net6(addr, &context->start6, context->prefix))
      context->flags |= CONTEXT_USED;
#endif
}

static void mark_config_used(struct dhcp_context *context, struct in6_addr *addr)
{
  for (; context; context = context->current)
    if (is_same_net6(addr, &context->start6, context->prefix))
      context->flags |= CONTEXT_CONF_USED;
}

/* make sure address not leased to another CLID/IAID */
static int check_address(struct state *state, struct in6_addr *addr)
{ 
  struct dhcp_lease *lease;

  if (!(lease = lease6_find_by_addr(addr, 128, 0)))
    return 1;

  if (lease->clid_len != state->clid_len || 
      memcmp(lease->clid, state->clid, state->clid_len) != 0 ||
      lease->hwaddr_type != state->iaid)
    return 0;

  return 1;
}

static void calculate_times(struct dhcp_context *context, unsigned int *min_time, unsigned int *valid_timep, 
			    unsigned int *preferred_timep, unsigned int lease_time, unsigned int requested_time)
{
  unsigned int preferred_time, valid_time;

  if (requested_time < 120u )
    requested_time = 120u; /* sanity */
  if (lease_time == 0xffffffff || (requested_time != 0xffffffff && requested_time < lease_time))
    lease_time = requested_time;
		    
  valid_time = (context->valid < lease_time) ? context->valid : lease_time;
  preferred_time = (context->preferred < lease_time) ? context->preferred : lease_time;

  if (context->flags & CONTEXT_DEPRECATE)
    preferred_time = 0;

  if (preferred_time != 0 && preferred_time < *min_time)
    *min_time = preferred_time;
  
  if (valid_time != 0 && valid_time < *min_time)
    *min_time = valid_time;
  
  *valid_timep = valid_time;
  *preferred_timep = preferred_time;
}

static void update_leases(struct state *state, struct dhcp_context *context, struct in6_addr *addr, unsigned int lease_time, time_t now)
{
  struct dhcp_lease *lease = lease6_find_by_addr(addr, 128, 0);
  struct dhcp_netid *tagif = run_tag_if(state->tags);

  if (!lease)
    lease = lease6_allocate(addr, state->ia_type == OPTION6_IA_NA ? LEASE_NA : LEASE_TA);
  
  if (lease)
    {
      lease_set_expires(lease, lease_time, now);
      lease_set_hwaddr(lease, NULL, state->clid, 0, state->iaid, state->clid_len, now, 0);
      lease_set_interface(lease, state->interface, now);
      if (state->hostname && state->ia_type == OPTION6_IA_NA)
	{
	  char *addr_domain = get_domain6(addr);
	  if (!state->send_domain)
	    state->send_domain = addr_domain;
	  lease_set_hostname(lease, state->hostname, state->hostname_auth, addr_domain, state->domain);
	}
      
#ifdef HAVE_SCRIPT
      if (daemon->lease_change_command)
	{
	  void *class_opt;
	  lease->flags |= LEASE_CHANGED;
	  free(lease->extradata);
	  lease->extradata = NULL;
	  lease->extradata_size = lease->extradata_len = 0;
	  lease->hwaddr_len = 0; /* surrogate for no of vendor classes */
	  
	  if ((class_opt = opt6_find(state->packet_options, state->end, OPTION6_VENDOR_CLASS, 4)))
	    {
	      void *enc_opt, *enc_end = opt6_ptr(class_opt, opt6_len(class_opt));
	      lease->hwaddr_len++;
	      /* send enterprise number first  */
	      sprintf(daemon->dhcp_buff2, "%u", opt6_uint(class_opt, 0, 4));
	      lease_add_extradata(lease, (unsigned char *)daemon->dhcp_buff2, strlen(daemon->dhcp_buff2), 0);
	      
	      if (opt6_len(class_opt) >= 6) 
		for (enc_opt = opt6_ptr(class_opt, 4); enc_opt; enc_opt = opt6_next(enc_opt, enc_end))
		  {
		    lease->hwaddr_len++;
		    lease_add_extradata(lease, opt6_ptr(enc_opt, 0), opt6_len(enc_opt), 0);
		  }
	    }
	  
	  lease_add_extradata(lease, (unsigned char *)state->client_hostname, 
			      state->client_hostname ? strlen(state->client_hostname) : 0, 0);				
	  
	  /* space-concat tag set */
	  if (!tagif && !context->netid.net)
	    lease_add_extradata(lease, NULL, 0, 0);
	  else
	    {
	      if (context->netid.net)
		lease_add_extradata(lease, (unsigned char *)context->netid.net, strlen(context->netid.net), tagif ? ' ' : 0);
	      
	      if (tagif)
		{
		  struct dhcp_netid *n;
		  for (n = tagif; n; n = n->next)
		    {
		      struct dhcp_netid *n1;
		      /* kill dupes */
		      for (n1 = n->next; n1; n1 = n1->next)
			if (strcmp(n->net, n1->net) == 0)
			  break;
		      if (!n1)
			lease_add_extradata(lease, (unsigned char *)n->net, strlen(n->net), n->next ? ' ' : 0); 
		    }
		}
	    }
	  
	  if (state->link_address)
	    inet_ntop(AF_INET6, state->link_address, daemon->addrbuff, ADDRSTRLEN);
	  
	  lease_add_extradata(lease, (unsigned char *)daemon->addrbuff, state->link_address ? strlen(daemon->addrbuff) : 0, 0);
	  
	  if ((class_opt = opt6_find(state->packet_options, state->end, OPTION6_USER_CLASS, 2)))
	    {
	      void *enc_opt, *enc_end = opt6_ptr(class_opt, opt6_len(class_opt));
	      for (enc_opt = opt6_ptr(class_opt, 0); enc_opt; enc_opt = opt6_next(enc_opt, enc_end))
		lease_add_extradata(lease, opt6_ptr(enc_opt, 0), opt6_len(enc_opt), 0);
	    }
	}
#endif	
      
    }
}
			  
			
	
static void log6_opts(int nest, unsigned int xid, void *start_opts, void *end_opts)
{
  void *opt;
  char *desc = nest ? "nest" : "sent";
  
  if (start_opts == end_opts)
    return;
  
  for (opt = start_opts; opt; opt = opt6_next(opt, end_opts))
    {
      int type = opt6_type(opt);
      void *ia_options = NULL;
      char *optname;
      
      if (type == OPTION6_IA_NA)
	{
	  sprintf(daemon->namebuff, "IAID=%u T1=%u T2=%u",
		  opt6_uint(opt, 0, 4), opt6_uint(opt, 4, 4), opt6_uint(opt, 8, 4));
	  optname = "ia-na";
	  ia_options = opt6_ptr(opt, 12);
	}
      else if (type == OPTION6_IA_TA)
	{
	  sprintf(daemon->namebuff, "IAID=%u", opt6_uint(opt, 0, 4));
	  optname = "ia-ta";
	  ia_options = opt6_ptr(opt, 4);
	}
      else if (type == OPTION6_IAADDR)
	{
	  inet_ntop(AF_INET6, opt6_ptr(opt, 0), daemon->addrbuff, ADDRSTRLEN);
	  sprintf(daemon->namebuff, "%s PL=%u VL=%u", 
		  daemon->addrbuff, opt6_uint(opt, 16, 4), opt6_uint(opt, 20, 4));
	  optname = "iaaddr";
	  ia_options = opt6_ptr(opt, 24);
	}
#ifdef OPTION6_PREFIX_CLASS
      else if (type == OPTION6_PREFIX_CLASS)
	{
	  optname = "prefix-class";
	  sprintf(daemon->namebuff, "class=%u", opt6_uint(opt, 0, 2));
	}
#endif
      else if (type == OPTION6_STATUS_CODE)
	{
	  int len = sprintf(daemon->namebuff, "%u ", opt6_uint(opt, 0, 2));
	  memcpy(daemon->namebuff + len, opt6_ptr(opt, 2), opt6_len(opt)-2);
	  daemon->namebuff[len + opt6_len(opt) - 2] = 0;
	  optname = "status";
	}
      else
	{
	  /* account for flag byte on FQDN */
	  int offset = type == OPTION6_FQDN ? 1 : 0;
	  optname = option_string(AF_INET6, type, opt6_ptr(opt, offset), opt6_len(opt) - offset, daemon->namebuff, MAXDNAME);
	}
      
      my_syslog(MS_DHCP | LOG_INFO, "%u %s size:%3d option:%3d %s  %s", 
		xid, desc, opt6_len(opt), type, optname, daemon->namebuff);
      
      if (ia_options)
	log6_opts(1, xid, ia_options, opt6_ptr(opt, opt6_len(opt)));
    }
}		 
 
static void log6_packet(struct state *state, char *type, struct in6_addr *addr, char *string)
{
  int clid_len = state->clid_len;

  /* avoid buffer overflow */
  if (clid_len > 100)
    clid_len = 100;
  
  print_mac(daemon->namebuff, state->clid, clid_len);

  if (addr)
    {
      inet_ntop(AF_INET6, addr, daemon->dhcp_buff2, 255);
      strcat(daemon->dhcp_buff2, " ");
    }
  else
    daemon->dhcp_buff2[0] = 0;

  if(option_bool(OPT_LOG_OPTS))
    my_syslog(MS_DHCP | LOG_INFO, "%u %s(%s) %s%s %s",
	      state->xid, 
	      type,
	      state->iface_name, 
	      daemon->dhcp_buff2,
	      daemon->namebuff,
	      string ? string : "");
  else
    my_syslog(MS_DHCP | LOG_INFO, "%s(%s) %s%s %s",
	      type,
	      state->iface_name, 
	      daemon->dhcp_buff2,
	      daemon->namebuff,
	      string ? string : "");
}

static void *opt6_find (void *opts, void *end, unsigned int search, unsigned int minsize)
{
  u16 opt, opt_len;
  void *start;
  
  if (!opts)
    return NULL;
    
  while (1)
    {
      if (end - opts < 4) 
	return NULL;
      
      start = opts;
      GETSHORT(opt, opts);
      GETSHORT(opt_len, opts);
      
      if (opt_len > (end - opts))
	return NULL;
      
      if (opt == search && (opt_len >= minsize))
	return start;
      
      opts += opt_len;
    }
}

static void *opt6_next(void *opts, void *end)
{
  u16 opt_len;
  
  if (end - opts < 4) 
    return NULL;
  
  opts += 2;
  GETSHORT(opt_len, opts);
  
  if (opt_len >= (end - opts))
    return NULL;
  
  return opts + opt_len;
}

static unsigned int opt6_uint(unsigned char *opt, int offset, int size)
{
  /* this worries about unaligned data and byte order */
  unsigned int ret = 0;
  int i;
  unsigned char *p = opt6_ptr(opt, offset);
  
  for (i = 0; i < size; i++)
    ret = (ret << 8) | *p++;
  
  return ret;
} 

#endif
