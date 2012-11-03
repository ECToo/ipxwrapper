/* IPXWrapper - Router code
 * Copyright (C) 2011 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <windows.h>
#include <winsock2.h>

#include "router.h"
#include "common.h"
#include "ipxwrapper.h"
#include "interface.h"

static struct router_addr *router_get(struct router_vars *router, SOCKET control, SOCKET sock);
static BOOL router_handle_call(struct router_vars *router, int sock, struct router_call *call);
static void router_drop_client(struct router_vars *router, int coff);

static BOOL rclient_do(struct rclient *rclient, struct router_call *call, struct router_ret *ret);

/* Allocate router_vars structure and initialise all members
 * Returns NULL on failure
*/
struct router_vars *router_init(BOOL global) {
	struct router_vars *router = malloc(sizeof(struct router_vars));
	if(!router) {
		log_printf(LOG_ERROR, "Not enough memory to create router_vars!");
		return NULL;
	}
	
	router->running = TRUE;
	router->udp_sock = -1;
	router->listener = -1;
	router->client_count = 0;
	router->wsa_event = WSA_INVALID_EVENT;
	router->crit_sec_init = FALSE;
	router->addrs = NULL;
	router->recvbuf = NULL;
	
	if(InitializeCriticalSectionAndSpinCount(&(router->crit_sec), 0x80000000)) {
		router->crit_sec_init = TRUE;
	}else{
		log_printf(LOG_ERROR, "Error creating critical section: %s", w32_error(GetLastError()));
		
		router_destroy(router);
		return NULL;
	}
	
	if((router->wsa_event = WSACreateEvent()) == WSA_INVALID_EVENT) {
		log_printf(LOG_ERROR, "Error creating WSA event object: %s", w32_error(WSAGetLastError()));
		
		router_destroy(router);
		return NULL;
	}
	
	if((router->udp_sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		log_printf(LOG_ERROR, "Error creating UDP socket: %s", w32_error(WSAGetLastError()));
		
		router_destroy(router);
		return NULL;
	}
	
	struct sockaddr_in addr;
	
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(main_config.udp_port);
	
	if(bind(router->udp_sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		log_printf(LOG_ERROR, "Error binding UDP socket: %s", w32_error(WSAGetLastError()));
		
		router_destroy(router);
		return NULL;
	}
	
	BOOL broadcast = TRUE;
	int bufsize = 524288;	/* 512KiB */
	
	setsockopt(router->udp_sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(BOOL));
	setsockopt(router->udp_sock, SOL_SOCKET, SO_RCVBUF, (char*)&bufsize, sizeof(int));
	setsockopt(router->udp_sock, SOL_SOCKET, SO_SNDBUF, (char*)&bufsize, sizeof(int));
	
	if(WSAEventSelect(router->udp_sock, router->wsa_event, FD_READ) == -1) {
		log_printf(LOG_ERROR, "WSAEventSelect error: %s", w32_error(WSAGetLastError()));
		
		router_destroy(router);
		return NULL;
	}
	
	if(!(router->recvbuf = malloc(sizeof(struct rpacket_header) + MAX_PKT_SIZE))) {
		log_printf(LOG_ERROR, "Out of memory! Cannot allocate recv buffer");
		
		router_destroy(router);
		return NULL;
	}
	
	if(global) {
		if((router->listener = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
			log_printf(LOG_ERROR, "Failed to create TCP socket: %s", w32_error(WSAGetLastError()));
			
			router_destroy(router);
			return NULL;
		}
		
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(main_config.router_port);
		
		if(bind(router->listener, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
			log_printf(LOG_ERROR, "Failed to bind TCP socket: %s", w32_error(WSAGetLastError()));
			
			router_destroy(router);
			return NULL;
		}
		
		if(listen(router->listener, 8) == -1) {
			log_printf(LOG_ERROR, "Failed to listen for connections: %s", w32_error(WSAGetLastError()));
			
			router_destroy(router);
			return NULL;
		}
		
		if(WSAEventSelect(router->listener, router->wsa_event, FD_ACCEPT) == -1) {
			log_printf(LOG_ERROR, "WSAEventSelect error: %s", w32_error(WSAGetLastError()));
			
			router_destroy(router);
			return NULL;
		}
	}
	
	return router;
}

/* Release all resources allocated by a router and free it */
void router_destroy(struct router_vars *router) {
	struct router_addr *addr = router->addrs;
	int i;
	
	while(addr) {
		struct router_addr *del = addr;
		addr = addr->next;
		
		free(del);
	}
	
	for(i = 0; i < router->client_count; i++) {
		closesocket(router->clients[i].sock);
	}
	
	if(router->listener != -1) {
		closesocket(router->listener);
	}
	
	free(router->recvbuf);
	
	if(router->udp_sock != -1) {
		closesocket(router->udp_sock);
	}
	
	if(router->wsa_event != WSA_INVALID_EVENT) {
		WSACloseEvent(router->wsa_event);
	}
	
	if(router->crit_sec_init) {
		DeleteCriticalSection(&(router->crit_sec));
	}
	
	free(router);
}

DWORD router_main(void *arg) {
	struct router_vars *router = arg;
	
	const unsigned char f6[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
	
	while(1) {
		WaitForSingleObject(router->wsa_event, INFINITE);
		
		EnterCriticalSection(&(router->crit_sec));
		
		WSAResetEvent(router->wsa_event);
		
		if(!router->running) {
			LeaveCriticalSection(&(router->crit_sec));
			return 0;
		}
		
		LeaveCriticalSection(&(router->crit_sec));
		
		if(router->listener != -1) {
			int newfd = accept(router->listener, NULL, NULL);
			if(newfd != -1) {
				if(router->client_count == MAX_ROUTER_CLIENTS) {
					log_printf(LOG_WARNING, "Too many clients, dropping new connection!");
					goto DROP_NEWFD;
				}
				
				if(WSAEventSelect(newfd, router->wsa_event, FD_READ | FD_CLOSE) == -1) {
					log_printf(LOG_ERROR, "WSAEventSelect error: %s", w32_error(WSAGetLastError()));
					goto DROP_NEWFD;
				}
				
				router->clients[router->client_count].sock = newfd;
				router->clients[router->client_count++].recvbuf_len = 0;
				
				if(0) {
					DROP_NEWFD:
					closesocket(newfd);
				}
			}else if(WSAGetLastError() != WSAEWOULDBLOCK) {
				log_printf(LOG_ERROR, "Failed to accept client connection: %s", w32_error(WSAGetLastError()));
			}
		}
		
		int i;
		for(i = 0; i < router->client_count; i++) {
			char *bstart = ((char*)&(router->clients[i].recvbuf)) + router->clients[i].recvbuf_len;
			int len = sizeof(struct router_call) - router->clients[i].recvbuf_len;
			
			if((len = recv(router->clients[i].sock, bstart, len, 0)) == -1) {
				if(WSAGetLastError() == WSAEWOULDBLOCK) {
					continue;
				}else if(WSAGetLastError() == WSAECONNRESET) {
					/* Treat connection reset as regular close */
					len = 0;
				}else{
					log_printf(LOG_ERROR, "Error reading from client socket: %s", w32_error(WSAGetLastError()));
				}
			}
			
			if(len == -1 || len == 0) {
				router_drop_client(router, i--);
				continue;
			}
			
			if((router->clients[i].recvbuf_len += len) == sizeof(struct router_call)) {
				if(router_handle_call(router, router->clients[i].sock, &(router->clients[i].recvbuf))) {
					router->clients[i].recvbuf_len = 0;
				}else{
					router_drop_client(router, i--);
				}
			}
		}
		
		struct sockaddr_in addr;
		int addrlen = sizeof(addr);
		
		struct rpacket_header *rp_header = (struct rpacket_header*)router->recvbuf;
		ipx_packet *packet = (ipx_packet*)(router->recvbuf + sizeof(*rp_header));
		
		int len = recvfrom(router->udp_sock, (char*)packet, MAX_PKT_SIZE, 0, (struct sockaddr*)&addr, &addrlen);
		if(len == -1) {
			if(WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAECONNRESET) {
				continue;
			}
			
			log_printf(LOG_ERROR, "Error reading from UDP socket: %s", w32_error(WSAGetLastError()));
			return 1;
		}
		
		EnterCriticalSection(&(router->crit_sec));
		
		packet->size = ntohs(packet->size);
		
		if(len < sizeof(ipx_packet) - 1 || packet->size > MAX_DATA_SIZE || packet->size + sizeof(ipx_packet) - 1 != len) {
			LeaveCriticalSection(&(router->crit_sec));
			continue;
		}
		
		if(min_log_level <= LOG_DEBUG) {
			IPX_STRING_ADDR(src_addr, addr32_in(packet->src_net), addr48_in(packet->src_node), packet->src_socket);
			IPX_STRING_ADDR(dest_addr, addr32_in(packet->dest_net), addr48_in(packet->dest_node), packet->dest_socket);
			
			log_printf(LOG_DEBUG, "Recieved packet from %s (%s) for %s", src_addr, inet_ntoa(addr.sin_addr), dest_addr);
		}
		
		memset(rp_header, 0, sizeof(*rp_header));
		rp_header->src_ipaddr = addr.sin_addr.s_addr;
		
		struct router_addr *ra = router->addrs;
		
		for(; ra; ra = ra->next)
		{
			if(
				ra->local_port &&
				(ra->filter_ptype < 0 || ra->filter_ptype == packet->ptype) &&
				(memcmp(packet->dest_net, ra->addr.sa_netnum, 4) == 0 || (memcmp(packet->dest_net, f6, 4) == 0 && (ra->flags & IPX_BROADCAST || !main_config.w95_bug) && ra->flags & IPX_RECV_BCAST)) &&
				(memcmp(packet->dest_node, ra->addr.sa_nodenum, 6) == 0 || (memcmp(packet->dest_node, f6, 6) == 0 && (ra->flags & IPX_BROADCAST || !main_config.w95_bug) && ra->flags & IPX_RECV_BCAST)) &&
				packet->dest_socket == ra->addr.sa_socket &&
				
				/* Check source address matches remote_addr if set */
				(ra->remote_addr.sa_family == AF_UNSPEC || (memcmp(ra->remote_addr.sa_netnum, packet->src_net, 4) == 0 && memcmp(ra->remote_addr.sa_nodenum, packet->src_node, 6) == 0))
			) {
				addr32_t ra_net  = addr32_in(ra->addr.sa_netnum);
				addr48_t ra_node = addr48_in(ra->addr.sa_nodenum);
				
				/* Check source address */
				
				if(main_config.iface_mode != IFACE_MODE_ALL)
				{
					/* Fetch the interface this socket is bound to. */
					
					ipx_interface_t *iface = ipx_interface_by_addr(ra_net, ra_node);
					
					if(!iface)
					{
						char net_s[ADDR32_STRING_SIZE], node_s[ADDR48_STRING_SIZE];
						
						addr32_string(net_s, ra_net);
						addr48_string(node_s, ra_node);
						
						log_printf(LOG_WARNING, "No iface for %s/%s! Stale bind?", net_s, node_s);
						
						continue;
					}
					
					/* Iterate over the subnets and compare
					 * to the packet source address.
					*/
					
					ipx_interface_ip_t *ip;
					
					int source_ok = 0;
					
					DL_FOREACH(iface->ipaddr, ip)
					{
						if((ip->ipaddr & ip->netmask) == (addr.sin_addr.s_addr & ip->netmask))
						{
							source_ok = 1;
							break;
						}
					}
					
					free_ipx_interface(iface);
					
					if(!source_ok)
					{
						/* Source matching failed. */
						
						continue;
					}
				}
				
				log_printf(LOG_DEBUG, "Relaying packet to local port %hu", ntohs(ra->local_port));
				
				struct sockaddr_in send_addr;
				
				send_addr.sin_family      = AF_INET;
				send_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				send_addr.sin_port        = ra->local_port;
				
				if(sendto(router->udp_sock, (char*)rp_header, sizeof(*rp_header) + len, 0, (struct sockaddr*)&send_addr, sizeof(send_addr)) == -1)
				{
					log_printf(LOG_ERROR, "Error relaying packet: %s", w32_error(WSAGetLastError()));
				}
			}
		}
		
		LeaveCriticalSection(&(router->crit_sec));
	}
	
	return 0;
}

static int router_bind(struct router_vars *router, SOCKET control, SOCKET sock, struct sockaddr_ipx *addr, int flags) {
	/* Network number 00:00:00:00 is specified as the "current" network, this code
	 * treats it as a wildcard when used for the network OR node numbers.
	 *
	 * According to MSDN 6, IPX socket numbers are unique to systems rather than
	 * interfaces and as such, the same socket number cannot be bound to more than
	 * one interface, my code lacks any "catch all" address like INADDR_ANY as I have
	 * not found any mentions of an equivalent address for IPX. This means that a
	 * given socket number may only be used on one interface.
	 *
	 * If you know the above information about IPX socket numbers to be incorrect,
	 * PLEASE email me with corrections!
	*/
	
	struct ipx_interface *ifaces = get_ipx_interfaces(), *iface;
	
	addr32_t sa_netnum  = addr32_in(addr->sa_netnum);
	addr48_t sa_nodenum = addr48_in(addr->sa_nodenum);
	
	for(iface = ifaces; iface; iface = iface->next) {
		if(
			(sa_netnum == iface->ipx_net || sa_netnum == 0)
			&& (sa_nodenum == iface->ipx_node || sa_nodenum == 0)
			
			/* TODO: Remove this check. */
			&& iface->ipaddr
		) {
			break;
		}
	}
	
	if(!iface) {
		log_printf(LOG_ERROR, "bind failed: no such address");
		
		free_ipx_interface_list(&ifaces);
		
		WSASetLastError(WSAEADDRNOTAVAIL);
		return -1;
	}
	
	addr32_out(addr->sa_netnum, iface->ipx_net);
	addr48_out(addr->sa_nodenum, iface->ipx_node);
	
	/* TODO: Don't store the IP stuff here. */
	
	uint32_t iface_ipaddr  = iface->ipaddr->ipaddr;
	uint32_t iface_netmask = iface->ipaddr->netmask;
	
	free_ipx_interface_list(&ifaces);
	
	EnterCriticalSection(&(router->crit_sec));
	
	if(router_get(router, control, sock)) {
		log_printf(LOG_ERROR, "bind failed: socket already bound");
		
		LeaveCriticalSection(&(router->crit_sec));
		
		WSASetLastError(WSAEINVAL);
		return -1;
	}
	
	if(addr->sa_socket == 0) {
		/* Automatic socket allocations start at 1024, I have no idea if
		 * this is normal IPX behaviour, but IP does it and it doesn't seem
		 * to interfere with any IPX software I've tested.
		*/
		
		uint16_t s = 1024;
		struct router_addr *a = router->addrs;
		
		while(a) {
			if(ntohs(a->addr.sa_socket) == s) {
				if(s == 65535) {
					log_printf(LOG_ERROR, "bind failed: out of sockets?!");
					
					LeaveCriticalSection(&(router->crit_sec));
					
					WSASetLastError(WSAEADDRNOTAVAIL);
					return -1;
				}
				
				s++;
				a = router->addrs;
				
				continue;
			}
			
			a = a->next;
		}
		
		addr->sa_socket = htons(s);
	}else{
		/* Test if any bound socket is using the requested socket number. */
		
		struct router_addr *a = router->addrs;
		
		while(a) {
			if(a->addr.sa_socket == addr->sa_socket && (!(a->flags & IPX_REUSE) || !(flags & IPX_REUSE))) {
				log_printf(LOG_ERROR, "bind failed: requested socket in use");
				
				LeaveCriticalSection(&(router->crit_sec));
				
				WSASetLastError(WSAEADDRINUSE);
				return -1;
			}
			
			a = a->next;
		}
	}
	
	struct router_addr *new_addr = malloc(sizeof(struct router_addr));
	if(!new_addr) {
		LeaveCriticalSection(&(router->crit_sec));
		
		WSASetLastError(ERROR_OUTOFMEMORY);
		return -1;
	}
	
	memcpy(&(new_addr->addr), addr, sizeof(struct sockaddr_ipx));
	
	new_addr->local_port = 0;
	new_addr->ws_socket = sock;
	new_addr->control_socket = control;
	new_addr->filter_ptype = -1;
	new_addr->flags = flags;
	new_addr->ipaddr = iface_ipaddr;
	new_addr->netmask = iface_netmask;
	new_addr->remote_addr.sa_family = AF_UNSPEC;
	new_addr->next = router->addrs;
	
	router->addrs = new_addr;
	
	LeaveCriticalSection(&(router->crit_sec));
	
	return 0;
}

/* Set loopback UDP port of emulation socket in NETWORK BYTE ORDER
 * Disable recv by setting to zero
*/
static void router_set_port(struct router_vars *router, SOCKET control, SOCKET sock, uint16_t port) {
	EnterCriticalSection(&(router->crit_sec));
	
	struct router_addr *addr = router_get(router, control, sock);
	if(addr) {
		addr->local_port = port;
	}
	
	LeaveCriticalSection(&(router->crit_sec));
}

static void router_unbind(struct router_vars *router, SOCKET control, SOCKET sock) {
	EnterCriticalSection(&(router->crit_sec));
	
	struct router_addr *addr = router->addrs, *prev = NULL;
	
	while(addr) {
		if(addr->control_socket == control && addr->ws_socket == sock) {
			if(prev) {
				prev->next = addr->next;
			}else{
				router->addrs = addr->next;
			}
			
			free(addr);
			break;
		}
		
		prev = addr;
		addr = addr->next;
	}
	
	LeaveCriticalSection(&(router->crit_sec));
}

/* Return the address a given socket is bound to, NULL if unbound */
static struct router_addr *router_get(struct router_vars *router, SOCKET control, SOCKET sock) {
	EnterCriticalSection(&(router->crit_sec));
	
	struct router_addr *addr = router->addrs;
	
	while(addr && (addr->control_socket != control || addr->ws_socket != sock)) {
		addr = addr->next;
	}
	
	LeaveCriticalSection(&(router->crit_sec));
	
	return addr;
}

/* Set packet type filter for a socket
 * Disable filter by setting to negative value
*/
static void router_set_filter(struct router_vars *router, SOCKET control, SOCKET sock, int ptype) {
	EnterCriticalSection(&(router->crit_sec));
	
	struct router_addr *addr = router_get(router, control, sock);
	if(addr) {
		addr->filter_ptype = ptype;
	}
	
	LeaveCriticalSection(&(router->crit_sec));
}

static int router_set_flags(struct router_vars *router, SOCKET control, SOCKET sock, int flags) {
	EnterCriticalSection(&(router->crit_sec));
	
	struct router_addr *addr = router_get(router, control, sock);
	if(addr) {
		if(addr->flags & IPX_REUSE && !(flags & IPX_REUSE)) {
			struct router_addr *test = router->addrs;
			
			while(test) {
				if(addr != test && memcmp(&(addr->addr), &(test->addr), sizeof(struct sockaddr_ipx)) == 0) {
					/* Refuse to disable SO_REUSEADDR when another binding for the same address exists */
					LeaveCriticalSection(&(router->crit_sec));
					return WSAEINVAL;
				}
				
				test = test->next;
			}
		}
		
		addr->flags = flags;
	}
	
	LeaveCriticalSection(&(router->crit_sec));
	return ERROR_SUCCESS;
}

static BOOL router_set_remote(struct router_vars *router, SOCKET control, SOCKET sock, const struct sockaddr_ipx *addr) {
	EnterCriticalSection(&(router->crit_sec));
	
	struct router_addr *ra = router_get(router, control, sock);
	if(ra) {
		ra->remote_addr = *addr;
	}
	
	LeaveCriticalSection(&(router->crit_sec));
	return TRUE;
}

static BOOL router_handle_call(struct router_vars *router, int sock, struct router_call *call) {
	struct router_ret ret;
	
	ret.err_code = ERROR_SUCCESS;
	
	switch(call->call) {
		case rc_bind: {
			ret.ret_addr = call->arg_addr;
			
			if(router_bind(router, sock, call->sock, &(ret.ret_addr), call->arg_int) == -1) {
				ret.err_code = WSAGetLastError();
			}
			
			break;
		}
		
		case rc_unbind: {
			router_unbind(router, sock, call->sock);
			break;
		}
		
		case rc_port: {
			router_set_port(router, sock, call->sock, call->arg_int);
			break;
		}
		
		case rc_filter: {
			router_set_filter(router, sock, call->sock, call->arg_int);
			break;
		}
		
		case rc_flags: {
			ret.err_code = router_set_flags(router, sock, call->sock, call->arg_int);
			break;
		}
		
		case rc_remote: {
			router_set_remote(router, sock, call->sock, &(call->arg_addr));
			break;
		}
		
		default: {
			log_printf(LOG_ERROR, "Recieved unknown call, dropping client");
			return FALSE;
		}
	}
	
	int sent = 0, sr;
	
	while(sent < sizeof(ret)) {
		char *sbuf = ((char*)&ret) + sent;
		
		if((sr = send(sock, sbuf, sizeof(ret) - sent, 0)) == -1) {
			log_printf(LOG_ERROR, "Send error: %s, dropping client", w32_error(WSAGetLastError()));
			return FALSE;
		}
		
		sent += sr;
	}
	
	return TRUE;
}

static void router_drop_client(struct router_vars *router, int coff) {
	EnterCriticalSection(&(router->crit_sec));
	
	struct router_addr *addr = router->addrs, *dp = NULL;
	
	while(addr) {
		dp = addr->control_socket == router->clients[coff].sock ? addr : NULL;
		addr = addr->next;
		
		if(dp) {
			router_unbind(router, dp->control_socket, dp->ws_socket);
		}
	}
	
	LeaveCriticalSection(&(router->crit_sec));
	
	closesocket(router->clients[coff].sock);
	router->clients[coff] = router->clients[--router->client_count];
}

BOOL rclient_init(struct rclient *rclient) {
	rclient->cs_init = FALSE;
	rclient->sock = -1;
	rclient->router = NULL;
	rclient->thread = NULL;
	
	if(InitializeCriticalSectionAndSpinCount(&(rclient->cs), 0x80000000)) {
		rclient->cs_init = TRUE;
	}else{
		log_printf(LOG_ERROR, "Failed to initialise critical section: %s", w32_error(GetLastError()));
		return FALSE;
	}
	
	return TRUE;
}

/* Connect to the remote router process, spawns local (private) router thread if
 * it the remote one isn't running.
 *
 * Calling when a router is already running is a no-op.
*/
BOOL rclient_start(struct rclient *rclient) {
	if(rclient->sock != -1 || rclient->router) {
		return TRUE;
	}
	
	if((rclient->sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		log_printf(LOG_ERROR, "Cannot create TCP socket: %s", w32_error(WSAGetLastError()));
		return FALSE;
	}
	
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(main_config.router_port);
	
	if(connect(rclient->sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
		return TRUE;
	}
	
	log_printf(LOG_ERROR, "Cannot connect to router process: %s", w32_error(WSAGetLastError()));
	
	closesocket(rclient->sock);
	rclient->sock = -1;
	
	log_printf(LOG_INFO, "Creating private router thread...");
	
	if(!(rclient->router = router_init(FALSE))) {
		return FALSE;
	}
	
	rclient->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&router_main, rclient->router, 0, NULL);
	if(!rclient->thread) {
		log_printf(LOG_ERROR, "Failed to create router thread: %s", w32_error(GetLastError()));
		
		router_destroy(rclient->router);
		rclient->router = NULL;
		
		return FALSE;
	}
	
	return TRUE;
}

/* Disconnect from the router process or stop the private router thread.
 *
 * Do not attempt to call rclient_start() again without calling rclient_init()
 * as the critical section object is deleted.
*/
void rclient_stop(struct rclient *rclient) {
	if(rclient->sock != -1) {
		closesocket(rclient->sock);
		rclient->sock = -1;
	}
	
	if(rclient->router) {
		EnterCriticalSection(&(rclient->router->crit_sec));
		
		rclient->router->running = FALSE;
		WSASetEvent(rclient->router->wsa_event);
		
		LeaveCriticalSection(&(rclient->router->crit_sec));
		
		if(WaitForSingleObject(rclient->thread, 3000) == WAIT_TIMEOUT) {
			log_printf(LOG_WARNING, "Router thread didn't exit in 3 seconds, terminating");
			TerminateThread(rclient->thread, 0);
		}
		
		CloseHandle(rclient->thread);
		rclient->thread = NULL;
		
		router_destroy(rclient->router);
		rclient->router = NULL;
	}
	
	if(rclient->cs_init) {
		DeleteCriticalSection(&(rclient->cs));
		rclient->cs_init = FALSE;
	}
}

static BOOL rclient_do(struct rclient *rclient, struct router_call *call, struct router_ret *ret) {
	EnterCriticalSection(&(rclient->cs));
	
	int done, r;
	
	for(done = 0; done < sizeof(*call);) {
		if((r = send(rclient->sock, ((char*)call) + done, sizeof(*call) - done, 0)) == -1) {
			log_printf(LOG_ERROR, "rclient_do: send error: %s", w32_error(WSAGetLastError()));
			
			LeaveCriticalSection(&(rclient->cs));
			return FALSE;
		}
		
		done += r;
	}
	
	for(done = 0; done < sizeof(*ret);) {
		if((r = recv(rclient->sock, ((char*)ret) + done, sizeof(*ret) - done, 0)) == -1) {
			log_printf(LOG_ERROR, "rclient_do: recv error: %s", w32_error(WSAGetLastError()));
			
			LeaveCriticalSection(&(rclient->cs));
			return FALSE;
		}else if(r == 0) {
			log_printf(LOG_ERROR, "rclient_do: Lost connection");
			WSASetLastError(WSAECONNRESET);
			
			LeaveCriticalSection(&(rclient->cs));
			return FALSE;
		}
		
		done += r;
	}
		
	LeaveCriticalSection(&(rclient->cs));
	return TRUE;
}

BOOL rclient_bind(struct rclient *rclient, SOCKET sock, struct sockaddr_ipx *addr, BOOL reuse) {
	if(rclient->sock != -1) {
		struct router_call call;
		struct router_ret ret;
		
		call.call = rc_bind;
		call.sock = sock;
		call.arg_addr = *addr;
		call.arg_int = reuse;
		
		if(!rclient_do(rclient, &call, &ret)) {
			return FALSE;
		}
		
		if(ret.err_code == ERROR_SUCCESS) {
			*addr = ret.ret_addr;
			
			return TRUE;
		}else{
			WSASetLastError(ret.err_code);
			return FALSE;
		}
	}else if(rclient->router) {
		return router_bind(rclient->router, 0, sock, addr, reuse) == 0 ? TRUE: FALSE;
	}
	
	log_printf(LOG_ERROR, "rclient_bind: No router?!");
	
	WSASetLastError(WSAENETDOWN);
	return FALSE;
}

BOOL rclient_unbind(struct rclient *rclient, SOCKET sock) {
	if(rclient->sock != -1) {
		struct router_call call;
		struct router_ret ret;
		
		call.call = rc_unbind;
		call.sock = sock;
		
		if(!rclient_do(rclient, &call, &ret)) {
			return FALSE;
		}
		
		return TRUE;
	}else if(rclient->router) {
		router_unbind(rclient->router, 0, sock);
		return TRUE;
	}
	
	log_printf(LOG_ERROR, "rclient_unbind: No router?!");
	
	WSASetLastError(WSAENETDOWN);
	return FALSE;
}

BOOL rclient_set_port(struct rclient *rclient, SOCKET sock, uint16_t port) {
	if(rclient->sock != -1) {
		struct router_call call;
		struct router_ret ret;
		
		call.call = rc_port;
		call.sock = sock;
		call.arg_int = port;
		
		if(!rclient_do(rclient, &call, &ret)) {
			return FALSE;
		}
		
		return TRUE;
	}else if(rclient->router) {
		router_set_port(rclient->router, 0, sock, port);
		return TRUE;
	}
	
	log_printf(LOG_ERROR, "rclient_set_port: No router?!");
	
	WSASetLastError(WSAENETDOWN);
	return FALSE;
}

BOOL rclient_set_filter(struct rclient *rclient, SOCKET sock, int ptype) {
	if(rclient->sock != -1) {
		struct router_call call;
		struct router_ret ret;
		
		call.call = rc_filter;
		call.sock = sock;
		call.arg_int = ptype;
		
		if(!rclient_do(rclient, &call, &ret)) {
			return FALSE;
		}
		
		return TRUE;
	}else if(rclient->router) {
		router_set_filter(rclient->router, 0, sock, ptype);
		return TRUE;
	}
	
	log_printf(LOG_ERROR, "rclient_set_filter: No router?!");
	
	WSASetLastError(WSAENETDOWN);
	return FALSE;
}

BOOL rclient_set_flags(struct rclient *rclient, SOCKET sock, int flags) {
	if(rclient->sock != -1) {
		struct router_call call;
		struct router_ret ret;
		
		call.call = rc_flags;
		call.sock = sock;
		call.arg_int = flags;
		
		if(!rclient_do(rclient, &call, &ret)) {
			return FALSE;
		}
		
		if(ret.err_code == ERROR_SUCCESS) {
			return TRUE;
		}else{
			WSASetLastError(ret.err_code);
			return FALSE;
		}
	}else if(rclient->router) {
		int err = router_set_flags(rclient->router, 0, sock, flags);
		if(err == ERROR_SUCCESS) {
			return TRUE;
		}else{
			WSASetLastError(err);
			return FALSE;
		}
		
		return TRUE;
	}
	
	log_printf(LOG_ERROR, "rclient_set_reuse: No router?!");
	
	WSASetLastError(WSAENETDOWN);
	return FALSE;
}

BOOL rclient_set_remote(struct rclient *rclient, SOCKET sock, const struct sockaddr_ipx *addr) {
	if(rclient->sock != -1) {
		struct router_call call;
		struct router_ret ret;
		
		call.call = rc_remote;
		call.sock = sock;
		call.arg_addr = *addr;
		
		if(!rclient_do(rclient, &call, &ret)) {
			return FALSE;
		}
		
		return TRUE;
	}else if(rclient->router) {
		return router_set_remote(rclient->router, 0, sock, addr);
	}
	
	log_printf(LOG_ERROR, "rclient_bind: No router?!");
	
	WSASetLastError(WSAENETDOWN);
	return FALSE;
}
