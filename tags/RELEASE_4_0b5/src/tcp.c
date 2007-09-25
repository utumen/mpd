
/*
 * tcp.c
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#include "ppp.h"
#include "phys.h"
#include "mbuf.h"
#include "ngfunc.h"
#include "tcp.h"

#ifdef __DragonFly__
#include <netgraph/socket/ng_socket.h>
#include <netgraph/ng_message.h>
#include <netgraph/async/ng_async.h>
#include <netgraph/ksocket/ng_ksocket.h>
#else
#include <netgraph/ng_socket.h>
#include <netgraph/ng_message.h>
#include <netgraph/ng_async.h>
#include <netgraph/ng_ksocket.h>
#endif
#include <netgraph.h>

/*
 * DEFINITIONS
 */

#define TCP_MTU		2048
#define TCP_MRU		2048
#define TCP_REOPEN_PAUSE	10
#define LISTENHOOK		"listen"

struct tcpinfo {
	/* Configuration */
	struct u_addr	peer_addr;
	struct u_addr	self_addr;
	in_port_t		peer_port;
	in_port_t		self_port;

	/* State */
	int			csock;
	int			dsock;
	uint32_t		flags;
#define	TCPINFO_ASYNC		0x00000001
#define	TCPINFO_LISTEN		0x00000002
	struct sockaddr_storage	sin_peer;
	struct ng_async_cfg	acfg;
	EventRef		ev_connect;
	EventRef		ev_accept;
	int			origination;
};

typedef struct tcpinfo	*TcpInfo;

/* Set menu options */
enum {
	SET_PEERADDR,
	SET_SELFADDR,
	SET_ORIGINATION,
};

/*
 * INTERNAL FUNCTIONS
 */

static int	TcpInit(PhysInfo p);
static void	TcpOpen(PhysInfo p);
static void	TcpClose(PhysInfo p);
static void	TcpShutdown(PhysInfo p);
static void	TcpStat(PhysInfo p);
static int	TcpOriginate(PhysInfo p);
static int	TcpPeerAddr(PhysInfo p, void *buf, int buf_len);

static int	TcpListen(TcpInfo tcp);
static void	TcpDoClose(TcpInfo tcp);
static int	TcpAsyncConfig(TcpInfo tcp);
static void	TcpAcceptEvent(int type, void *cookie);
static void	TcpConnectEvent(int type, void *cookie);

static int	TcpSetCommand(int ac, char *av[], void *arg);

/*
 * GLOBAL VARIABLES
 */

const struct phystype gTcpPhysType = {
	.name		= "tcp",
	.synchronous	= FALSE,
	.minReopenDelay	= TCP_REOPEN_PAUSE,
	.mtu		= TCP_MTU,
	.mru		= TCP_MRU,
	.init		= TcpInit,
	.open		= TcpOpen,
	.close		= TcpClose,
	.shutdown	= TcpShutdown,
	.showstat	= TcpStat,
	.originate	= TcpOriginate,
	.peeraddr	= TcpPeerAddr,
};

const struct cmdtab TcpSetCmds[] = {
    { "self ip [port]",			"Set local IP address",
	TcpSetCommand, NULL, (void *) SET_SELFADDR },
    { "peer ip [port]",			"Set remote IP address",
	TcpSetCommand, NULL, (void *) SET_PEERADDR },
    { "origination < local | remote >",	"Set link origination",
	TcpSetCommand, NULL, (void *) SET_ORIGINATION },
    { NULL },
};


/*
 * TcpInit()
 */

static int
TcpInit(PhysInfo p)
{
	TcpInfo tcp;

	tcp = (TcpInfo) (p->info = Malloc(MB_PHYS, sizeof(*tcp)));
	tcp->origination = LINK_ORIGINATE_UNKNOWN;
	tcp->csock = -1;
	tcp->flags = 0;

	return (0);
}

static int
TcpAsyncConfig(TcpInfo tcp)
{
	struct ngm_mkpeer mkp;
	char path[NG_PATHLEN + 1];

	snprintf(mkp.type, sizeof(mkp.type), "%s", NG_ASYNC_NODE_TYPE);
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s%d",
	    NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex);
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), NG_ASYNC_HOOK_SYNC);
	if (NgSendMsg(bund->csock, MPD_HOOK_PPP, NGM_GENERIC_COOKIE,
	    NGM_MKPEER, &mkp, sizeof(mkp)) < 0) {
		Log(LG_ERR, ("[%s] can't attach %s node: %s",
		    lnk->name, NG_ASYNC_NODE_TYPE, strerror(errno)));
		return (errno);
	}
	
	/* Configure the async converter node. */
	memset(&tcp->acfg, 0, sizeof(tcp->acfg));
	tcp->acfg.enabled = TRUE;
	tcp->acfg.accm = ~0;
	tcp->acfg.amru = TCP_MRU;
	tcp->acfg.smru = TCP_MTU;
	snprintf(path, sizeof(path), ".:%s.%s%d", MPD_HOOK_PPP,
	    NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex);
	if (NgSendMsg(bund->csock, path, NGM_ASYNC_COOKIE,
	    NGM_ASYNC_CMD_SET_CONFIG, &tcp->acfg, sizeof(tcp->acfg)) < 0) {
		Log(LG_ERR, ("[%s] can't config %s", lnk->name, path));
		return (errno);
	}

	tcp->flags |= TCPINFO_ASYNC;

	return (0);
}

/*
 * TcpOpen()
 */

static void
TcpOpen(PhysInfo p)
{
	TcpInfo	const tcp = (TcpInfo) lnk->phys->info;
	struct ngm_mkpeer mkp;
	char path[NG_PATHLEN + 1];
	struct sockaddr_storage addr;
	int rval, error;
	char buf[64];

	if (tcp->origination == LINK_ORIGINATE_REMOTE) {
		Log(LG_PHYS2, ("[%s] %s() on incoming call", lnk->name,
		    __func__));
		PhysUp();
		return;
	}

	assert (tcp->origination == LINK_ORIGINATE_LOCAL);

	/* Attach async node to PPP node. */
	if ((tcp->flags & TCPINFO_ASYNC) == 0 &&
	    (error = TcpAsyncConfig(tcp)) > 0)
		goto fail;

	/* Create a new netgraph node to control TCP ksocket node. */
	if (tcp->csock == -1 &&
	    NgMkSockNode(NULL, &tcp->csock, &tcp->dsock) < 0) {
		Log(LG_ERR, ("[%s] TCP can't create control socket: %s",
		    lnk->name, strerror(errno)));
		goto fail;
	}

	/*
	 * Attach fresh ksocket node next to async node.
	 * Remove any stale node.
	 */
	snprintf(mkp.type, sizeof(mkp.type), "%s", NG_KSOCKET_NODE_TYPE);
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), NG_ASYNC_HOOK_ASYNC);
	if ((tcp->self_addr.family==AF_INET6) || 
	    (tcp->self_addr.family==AF_UNSPEC && tcp->peer_addr.family==AF_INET6)) {
	    snprintf(mkp.peerhook, sizeof(mkp.peerhook), "%d/%d/%d", PF_INET6, SOCK_STREAM, IPPROTO_TCP);
	} else {
	    snprintf(mkp.peerhook, sizeof(mkp.peerhook), "inet/stream/tcp");
	}
	snprintf(path, sizeof(path), "[%x]:%s%d", bund->nodeID,
	    NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex);
	NgFuncDisconnect(path, NG_ASYNC_HOOK_ASYNC);
	if (NgSendMsg(tcp->csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mkp,
	    sizeof(mkp)) < 0) {
		Log(LG_ERR, ("[%s] can't attach %s node: %s", lnk->name,
		    NG_KSOCKET_NODE_TYPE, strerror(errno)));
		goto fail;
	}

	/* Start connecting to peer. */
	u_addrtosockaddr(&tcp->peer_addr, tcp->peer_port, &addr);
	snprintf(path, sizeof(path), "[%x]:%s%d.%s", bund->nodeID,
	    NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex, NG_ASYNC_HOOK_ASYNC);
	rval = NgSendMsg(tcp->csock, path, NGM_KSOCKET_COOKIE,
	    NGM_KSOCKET_CONNECT, &addr, addr.ss_len);
	if (rval < 0 && errno != EINPROGRESS) {
		Log(LG_ERR, ("[%s] can't connect %s node: %s", lnk->name,
		    NG_KSOCKET_NODE_TYPE, strerror(errno))); 
		goto fail;
	}
	if (rval == 0)	/* Can happen when peer is local. */
		TcpConnectEvent(EVENT_READ, lnk);
	else {
		assert(errno == EINPROGRESS);
		EventRegister(&tcp->ev_connect, EVENT_READ, tcp->csock,
		    0, TcpConnectEvent, lnk);
		Log(LG_PHYS, ("[%s] connecting to %s %u", lnk->name,
		    u_addrtoa(&tcp->peer_addr, buf, sizeof(buf)), tcp->peer_port));
	}

	return;
fail:
	TcpDoClose(tcp);
	PhysDown(STR_ERROR, NULL);
}

static int
TcpListen(TcpInfo tcp)
{
	struct ngm_mkpeer mkp;
	struct sockaddr_storage addr;
	int32_t backlog = 1;
	int error;
	char buf[64];

	assert(tcp->origination == LINK_ORIGINATE_REMOTE);
	assert((tcp->flags & TCPINFO_LISTEN) == 0);

	/* Attach async node to PPP node. */
	if ((tcp->flags & TCPINFO_ASYNC) == 0 &&
	    (error = TcpAsyncConfig(tcp)) > 0)
		goto fail;

	/* Create a new netgraph node to control TCP ksocket node. */
	if (tcp->csock == -1 &&
	    NgMkSockNode(NULL, &tcp->csock, &tcp->dsock) < 0) {
		Log(LG_ERR, ("[%s] TCP can't create control socket: %s",
		    lnk->name, strerror(errno)));
		error = errno;
		goto fail;
	}

	/* Make listening TCP ksocket node. */
	snprintf(mkp.type, sizeof(mkp.type), "%s",
	    NG_KSOCKET_NODE_TYPE);
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), LISTENHOOK);
	if ((tcp->self_addr.family==AF_INET6) || 
	    (tcp->self_addr.family==AF_UNSPEC && tcp->peer_addr.family==AF_INET6)) {
	    snprintf(mkp.peerhook, sizeof(mkp.peerhook), "%d/%d/%d", PF_INET6, SOCK_STREAM, IPPROTO_TCP);
	} else {
	    snprintf(mkp.peerhook, sizeof(mkp.peerhook), "inet/stream/tcp");
	}
	if (NgSendMsg(tcp->csock, ".", NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mkp, sizeof(mkp)) < 0) {
		Log(LG_ERR, ("[%s] can't attach %s node: %s",
		    lnk->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
		error = errno;
		goto fail2;
	}

	/* Bind socket. */
	u_addrtosockaddr(&tcp->self_addr, tcp->self_port, &addr);
	if (NgSendMsg(tcp->csock, LISTENHOOK, NGM_KSOCKET_COOKIE,
	    NGM_KSOCKET_BIND, &addr, addr.ss_len) < 0) {
		Log(LG_ERR, ("[%s] can't bind %s node: %s",
		    lnk->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
		error = errno;
		goto fail2;
	}

	/* Listen. */
	if (NgSendMsg(tcp->csock, LISTENHOOK, NGM_KSOCKET_COOKIE,
	    NGM_KSOCKET_LISTEN, &backlog, sizeof(backlog)) < 0) {
		Log(LG_ERR, ("[%s] can't listen on %s node: %s",
		    lnk->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
		error = errno;
		goto fail2;
	}

	/* Tell that we are willing to receive accept message. */
	if (NgSendMsg(tcp->csock, LISTENHOOK, NGM_KSOCKET_COOKIE,
	    NGM_KSOCKET_ACCEPT, NULL, 0) < 0) {
		Log(LG_ERR, ("[%s] can't accept on %s node: %s",
		    lnk->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
		error = errno;
		goto fail2;
	}

	Log(LG_PHYS, ("[%s] waiting for connection on %s %u",
	    lnk->name, u_addrtoa(&tcp->self_addr, buf, sizeof(buf)), tcp->self_port));
	EventRegister(&tcp->ev_accept, EVENT_READ, tcp->csock,
	    0, TcpAcceptEvent, lnk);

	tcp->flags |= TCPINFO_LISTEN;

	return (0);

fail2:
	NgSendMsg(tcp->csock, LISTENHOOK, NGM_GENERIC_COOKIE, NGM_SHUTDOWN,
	    NULL, 0);
fail:
	TcpDoClose(tcp);
	PhysDown(STR_ERROR, NULL);
	return (error);
}

/*
 * TcpConnectEvent() triggers when outgoing connection succeeds.
 */
static void
TcpConnectEvent(int type, void *cookie)
{
	struct {
		struct ng_mesg	resp;
		int32_t		rval;
	} cn;
	TcpInfo	tcp;
	char path[NG_PATHLEN + 1];

	/* Restore context. */
	lnk = (Link) cookie;
	bund = lnk->bund;
	tcp = (TcpInfo) lnk->phys->info;

	assert(type == EVENT_READ);
	assert(tcp->origination == LINK_ORIGINATE_LOCAL);

	/* Check whether the connection was successful or not. */
	if (NgRecvMsg(tcp->csock, &cn.resp, sizeof(cn), path) < 0) {
		Log(LG_ERR, ("[%s] error reading message from \"%s\": %s",
		    lnk->name, path, strerror(errno)));
		goto failed;
	}

	assert(cn.resp.header.typecookie == NGM_KSOCKET_COOKIE);
	assert(cn.resp.header.cmd == NGM_KSOCKET_CONNECT);

	if (cn.rval != 0) {
		Log(LG_PHYS, ("[%s] failed to connect: %s", lnk->name,
		    strerror(cn.rval)));
		goto failed;
	}

	/* Report connected. */
	Log(LG_PHYS, ("[%s] connection established", lnk->name));
	PhysUp();

	return;
failed:
	TcpDoClose(tcp);
	PhysDown(STR_ERROR, NULL);
}

/*
 * TcpAcceptEvent() triggers when we accept incoming connection.
 */
static void
TcpAcceptEvent(int type, void *cookie)
{
	struct {
		struct ng_mesg	resp;
		uint32_t	id;
		struct sockaddr_storage sin;
	} ac;
	TcpInfo	tcp;
	struct ngm_connect cn;
	char path[NG_PATHLEN + 1];
	struct u_addr	addr;
	in_port_t	port;
	char		buf[64];

	/* Restore context. */
	lnk = (Link) cookie;
	bund = lnk->bund;
	tcp = (TcpInfo) lnk->phys->info;

	assert(type == EVENT_READ);
	assert(tcp->origination == LINK_ORIGINATE_REMOTE);

	/* Accept cloned ng_ksocket(4). */
	if (NgRecvMsg(tcp->csock, &ac.resp, sizeof(ac), NULL) < 0) {
		Log(LG_ERR, ("[%s] error reading message from \"%s\": %s",
		    lnk->name, path, strerror(errno)));
		goto failed;
	}
	sockaddrtou_addr(&ac.sin, &addr, &port);

	Log(LG_PHYS, ("[%s] incoming connection from %s %u", lnk->name,
	    u_addrtoa(&addr, buf, sizeof(buf)), port));

	if (gShutdownInProgress) {
		Log(LG_PHYS, ("Shutdown sequence in progress, ignoring"));
		return;
	}

	/*
	 * If passive, and peer address specified,
	 * only accept from that address. Same check with port.
	 */
	if ((!u_addrempty(&tcp->peer_addr)) &&
	    u_addrcompare(&tcp->peer_addr, &addr)) {
		Log(LG_PHYS, ("[%s] rejected: wrong IP address", lnk->name));
		goto failed;
	}
	if (tcp->peer_port != 0 &&
	    tcp->peer_port != port) {
		Log(LG_PHYS, ("[%s] rejected: wrong port", lnk->name));
		goto failed;
	}
	memcpy(&tcp->sin_peer, &ac.sin, sizeof(tcp->sin_peer));

	/* Connect new born ksocket to our link. */
	snprintf(cn.path, sizeof(cn.path), "[%x]:", ac.id);
	snprintf(cn.ourhook, sizeof(cn.ourhook), NG_ASYNC_HOOK_ASYNC);
	snprintf(cn.peerhook, sizeof(cn.peerhook), "data");
	snprintf(path, sizeof(path), "[%x]:%s%d", bund->nodeID,
	    NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex);
	if (NgSendMsg(bund->csock, path, NGM_GENERIC_COOKIE, NGM_CONNECT,
	    &cn, sizeof(cn)) < 0) {
		Log(LG_ERR, ("[%s] can't connect new born ksocket: %s",
		    lnk->name, strerror(errno)));
		goto failed;
  	}

	/* Report connected. */
	Log(LG_PHYS, ("[%s] connected with %s %u", lnk->name,
	    u_addrtoa(&addr, buf, sizeof(buf)), port));
	RecordLinkUpDownReason(NULL, 1, STR_INCOMING_CALL, "", NULL);
	BundOpenLink(lnk);

	return;

failed:
	TcpDoClose(tcp);
	PhysDown(STR_ERROR, NULL);
}

/*
 * TcpClose()
 */

static void
TcpClose(PhysInfo p)
{
	TcpDoClose((TcpInfo) p->info);
	PhysDown(0, NULL);
}

/*
 * TcpShutdown()
 */

static void
TcpShutdown(PhysInfo p)
{
	TcpInfo const tcp = (TcpInfo) p->info;
	char path[NG_PATHLEN + 1];

	TcpDoClose(tcp);

	snprintf(path, sizeof(path), "[%x]:%s%d", bund->nodeID,
	    NG_PPP_HOOK_LINK_PREFIX, lnk->bundleIndex);
	NgFuncShutdownNode(bund, bund->name, path);
	close(tcp->csock);
	close(tcp->dsock);
	tcp->csock = -1;
}

/*
 * TcpDoClose()
 */

static void
TcpDoClose(TcpInfo tcp)
{
	EventUnRegister(&tcp->ev_connect);
}

/*
 * TcpOriginate()
 */

static int
TcpOriginate(PhysInfo p)
{
	TcpInfo const tcp = (TcpInfo) lnk->phys->info;

	return (tcp->origination);
}

static int
TcpPeerAddr(PhysInfo p, void *buf, int buf_len)
{
	TcpInfo const tcp = (TcpInfo) p;

	if (u_addrtoa(&tcp->peer_addr, buf, buf_len))
		return (0);
  	else
		return (-1);
}

/*
 * TcpStat()
 */

void
TcpStat(PhysInfo p)
{
	TcpInfo const tcp = (TcpInfo) lnk->phys->info;
	char	buf[64];

	Printf("TCP configuration:\r\n");
	Printf("\tSelf address : %s, port %u\r\n",
	    u_addrtoa(&tcp->self_addr, buf, sizeof(buf)), tcp->self_port);
	Printf("\tPeer address : %s, port %u\r\n",
	    u_addrtoa(&tcp->peer_addr, buf, sizeof(buf)), tcp->peer_port);
	Printf("\tConnect mode : %s\r\n",
	    tcp->origination == LINK_ORIGINATE_LOCAL ?
	    "local" : "remote");
}

/*
 * TcpSetCommand()
 */

static int
TcpSetCommand(int ac, char *av[], void *arg)
{
	TcpInfo	const tcp = (TcpInfo) lnk->phys->info;
	struct sockaddr_storage *sin;   
  
	switch ((intptr_t)arg) {
	case SET_PEERADDR:
		if ((sin = ParseAddrPort(ac, av, ALLOW_IPV4|ALLOW_IPV6)) == NULL)
			return (-1);
		sockaddrtou_addr(sin, &tcp->peer_addr, &tcp->peer_port);
		break;
	case SET_SELFADDR:
		if ((sin = ParseAddrPort(ac, av, ALLOW_IPV4|ALLOW_IPV6)) == NULL)
			return (-1);
		sockaddrtou_addr(sin, &tcp->self_addr, &tcp->self_port);
		break;
	case SET_ORIGINATION:
		if (ac != 1)
			return (-1);
		if (strcasecmp(av[0], "local") == 0) {
			tcp->origination = LINK_ORIGINATE_LOCAL;
			break;
		}
		if (strcasecmp(av[0], "remote") == 0) {
			tcp->origination = LINK_ORIGINATE_REMOTE;
			return (TcpListen(tcp));
			break;
      		}
		Log(LG_ERR, ("Invalid link origination \"%s\"", av[0]));
		return (-1);

	default:
		assert(0);
	}

	return (0);
}