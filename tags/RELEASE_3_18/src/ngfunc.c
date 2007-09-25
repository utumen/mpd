
/*
 * ngfunc.c
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 *
 * TCP MSSFIX contributed by Sergey Korolew <dsATbittu.org.ru>
 *
 * Routines for doing netgraph stuff
 *
 */

#include "ppp.h"
#include "bund.h"
#include "ngfunc.h"
#include "input.h"
#include "ccp.h"
#include "netgraph.h"

#include <net/bpf.h>

#include <netgraph/ng_message.h>
#include <netgraph/ng_socket.h>
#include <netgraph/ng_ksocket.h>
#include <netgraph/ng_iface.h>
#include <netgraph/ng_ppp.h>
#include <netgraph/ng_vjc.h>
#include <netgraph/ng_bpf.h>

/*
 * DEFINITIONS
 */

  #define TEMPHOOK		"temphook"
  #define MAX_IFACE_CREATE	128

/*
 * INTERNAL FUNCTIONS
 */

  static void	NgFuncDataEvent(int type, void *cookie);
  static void	NgFuncCtrlEvent(int type, void *cookie);
  static int	NgFuncCreateIface(Bund b,
			const char *ifname, char *buf, int max);
  static int	NgFuncIfaceExists(Bund b,
			const char *ifname, char *buf, int max);
  static void	NgFuncShutdownInternal(Bund b, int iface, int ppp);

  static void	NgFuncErrx(const char *fmt, ...);
  static void	NgFuncErr(const char *fmt, ...);

/*
 * INTERNAL VARIABLES
 */

  /* A BPF filter for matching an IP packet if it constitutes 'demand' */
  static const struct bpf_insn gDemandProg[] = {

	/* Load IP protocol number and IP header length */
/*00*/	BPF_STMT(BPF_LD+BPF_B+BPF_ABS, 9),		/* A <- IP protocol */
/*01*/	BPF_STMT(BPF_LDX+BPF_B+BPF_MSH, 0),		/* X <- header len */

	/* Compare to interesting possibilities */
/*02*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, IPPROTO_IGMP, 4, 0),	/* -> 07 */
/*03*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, IPPROTO_ICMP, 4, 0),	/* -> 08 */
/*04*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, IPPROTO_UDP, 11, 0),	/* -> 16 */
/*05*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, IPPROTO_TCP, 16, 0),	/* -> 22 */

	/* Some other protocol -> accept */
/*06*/	BPF_STMT(BPF_RET+BPF_K, (u_int)-1),

	/* Protocol is IGMP -> reject (no multicast stuff) */
/*07*/	BPF_STMT(BPF_RET+BPF_K, 0),

	/* Protocol is ICMP -> reject ICMP replies */
/*08*/	BPF_STMT(BPF_LD+BPF_B+BPF_IND, 0),		/* A <- ICMP type */
/*09*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, ICMP_ECHOREPLY, 0, 1),
/*10*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject ECHOREPLY */
/*11*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, ICMP_UNREACH, 0, 1),
/*12*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject UNREACH */
/*13*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, ICMP_REDIRECT, 0, 1),
/*14*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject REDIRECT */
/*15*/	BPF_STMT(BPF_RET+BPF_K, (u_int)-1),		/* OK, accept */

	/* Protocol is UDP -> reject NTP and port 24 traffic */
#define NTP_PORT	123
#define U24_PORT	24			/* XXX InterJet-specific hack */
/*16*/	BPF_STMT(BPF_LD+BPF_H+BPF_IND, 2),		/* A <- UDP dest port */
/*17*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, NTP_PORT, 0, 1),/* compare NTP_PORT */
/*18*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject NTP */
/*19*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, U24_PORT, 0, 1),/* compare port 24 */
/*20*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject port 24 */
/*21*/	BPF_STMT(BPF_RET+BPF_K, (u_int)-1),		/* OK, accept */

	/* Protocol is TCP -> reject if TH_RST bit set */
/*22*/	BPF_STMT(BPF_LD+BPF_B+BPF_IND, 13),		/* A <- TCP flags */
/*23*/	BPF_STMT(BPF_ALU+BPF_AND+BPF_K, TH_RST),	/* A <- A & TH_RST */
/*24*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0, 0, 1),	/* compare to zero */
/*25*/	BPF_STMT(BPF_RET+BPF_K, (u_int)-1),		/* accept packet */
/*26*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject packet */

  };

  #define DEMAND_PROG_LEN	(sizeof(gDemandProg) / sizeof(*gDemandProg))

  /* A BPF filter that matches nothing */
  static const struct bpf_insn gNoMatchProg[] = {
	BPF_STMT(BPF_RET+BPF_K, 0)
  };

  #define NOMATCH_PROG_LEN	(sizeof(gNoMatchProg) / sizeof(*gNoMatchProg))

  /* A BPF filter that matches TCP SYN packets */
  static const struct bpf_insn gTCPSYNProg[] = {

	/* Load IP protocol number and IP header length */
/*00*/	BPF_STMT(BPF_LD+BPF_B+BPF_ABS, 9),		/* A <- IP protocol */
/*01*/	BPF_STMT(BPF_LDX+BPF_B+BPF_MSH, 0),		/* X <- header len */

/*02*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, IPPROTO_TCP, 1, 0),	/* -> 04 */
/*03*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject packet */

	/* Protocol is TCP -> accept if TH_SYN bit set */
/*04*/	BPF_STMT(BPF_LD+BPF_B+BPF_IND, 13),		/* A <- TCP flags */
/*05*/	BPF_STMT(BPF_ALU+BPF_AND+BPF_K, TH_SYN),	/* A <- A & TH_SYN */
/*06*/	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, 0, 1, 0),	/* compare to zero */
/*07*/	BPF_STMT(BPF_RET+BPF_K, (u_int)-1),		/* accept packet */
/*08*/	BPF_STMT(BPF_RET+BPF_K, 0),			/* reject packet */
  };

  #define TCPSYN_PROG_LEN	(sizeof(gTCPSYNProg) / sizeof(*gTCPSYNProg))

/*
 * NgFuncInit()
 *
 * Setup the initial PPP netgraph framework. Initializes these fields
 * in the supplied bundle structure:
 *
 *	iface.ifname	- Interface name
 *	csock		- Control socket for socket netgraph node
 *	dsock		- Data socket for socket netgraph node
 *
 * Returns -1 if error.
 */

int
NgFuncInit(Bund b, const char *reqIface)
{
  union {
      u_char		buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
      struct ng_mesg	reply;
  }			u;
  struct nodeinfo	*const ni = (struct nodeinfo *)(void *)u.reply.data;
  struct ngm_mkpeer	mp;
  struct ngm_connect	cn;
  struct ngm_name	nm;
  char			path[NG_PATHLEN + 1];
  int			newIface = 0;
  int			newPpp = 0;

  /* Set up libnetgraph logging */
  NgSetErrLog(NgFuncErr, NgFuncErrx);

  /* Create a netgraph socket node */
  if (NgMkSockNode(NULL, &b->csock, &b->dsock) < 0) {
    Log(LG_ERR, ("[%s] can't create %s node: %s",
      b->name, NG_SOCKET_NODE_TYPE, strerror(errno)));
    return(NULL);
  }
  (void) fcntl(b->csock, F_SETFD, 1);
  (void) fcntl(b->dsock, F_SETFD, 1);

  /* Create new iface node if necessary, else find the one specified */
  if (reqIface != NULL) {
    switch (NgFuncIfaceExists(b,
	reqIface, b->iface.ifname, sizeof(b->iface.ifname))) {
    case -1:			/* not a netgraph interface */
      Log(LG_ERR, ("[%s] interface \"%s\" is not a netgraph interface",
	b->name, reqIface));
      goto fail;
      break;
    case 0:			/* interface does not exist */
      if (NgFuncCreateIface(b,
	  reqIface, b->iface.ifname, sizeof(b->iface.ifname)) < 0) {
	Log(LG_ERR, ("[%s] can't create interface \"%s\"", b->name, reqIface));
	goto fail;
      }
      break;
    case 1:			/* interface exists */
      break;
    default:
      assert(0);
    }
  } else {
    if (NgFuncCreateIface(b,
	NULL, b->iface.ifname, sizeof(b->iface.ifname)) < 0) {
      Log(LG_ERR, ("[%s] can't create netgraph interface", b->name));
      goto fail;
    }
    newIface = 1;
  }
 
  /* Create new PPP node */
  snprintf(mp.type, sizeof(mp.type), "%s", NG_PPP_NODE_TYPE);
  snprintf(mp.ourhook, sizeof(mp.ourhook), "%s", MPD_HOOK_PPP);
  snprintf(mp.peerhook, sizeof(mp.peerhook), "%s", NG_PPP_HOOK_BYPASS);
  if (NgSendMsg(b->csock, ".",
      NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0) {
    Log(LG_ERR, ("[%s] can't create %s node: %s",
      b->name, mp.type, strerror(errno)));
    goto fail;
  }
  newPpp = 1;

  /* Give it a name */
  snprintf(nm.name, sizeof(nm.name), "mpd%d-%s", getpid(), b->name);
  if (NgSendMsg(b->csock, MPD_HOOK_PPP,
      NGM_GENERIC_COOKIE, NGM_NAME, &nm, sizeof(nm)) < 0) {
    Log(LG_ERR, ("[%s] can't name %s node: %s",
      b->name, NG_PPP_NODE_TYPE, strerror(errno)));
    goto fail;
  }
  Log(LG_ALWAYS, ("[%s] %s node is \"%s\"",
    b->name, NG_PPP_NODE_TYPE, nm.name));

  /* Get PPP node ID */
  if (NgSendMsg(b->csock, MPD_HOOK_PPP,
      NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) < 0) {
    Log(LG_ERR, ("[%s] ppp nodeinfo: %s", b->name, strerror(errno)));
    goto fail;
  }
  if (NgRecvMsg(b->csock, &u.reply, sizeof(u), NULL) < 0) {
    Log(LG_ERR, ("[%s] node \"%s\" reply: %s",
      b->name, MPD_HOOK_PPP, strerror(errno)));
    goto fail;
  }
  b->nodeID = ni->id;

  /* Add a bpf node to the PPP node on the "inet" hook */
  snprintf(mp.type, sizeof(mp.type), "%s", NG_BPF_NODE_TYPE);
  snprintf(mp.ourhook, sizeof(mp.ourhook), "%s", NG_PPP_HOOK_INET);
  snprintf(mp.peerhook, sizeof(mp.peerhook), "%s", BPF_HOOK_PPP);
  if (NgSendMsg(b->csock, MPD_HOOK_PPP,
      NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0) {
    Log(LG_ERR, ("[%s] can't create %s node: %s",
      b->name, NG_BPF_NODE_TYPE, strerror(errno)));
    goto fail;
  }

  /* Connect the other side of the bpf node to the iface node */
  snprintf(path, sizeof(path), "%s.%s", MPD_HOOK_PPP, NG_PPP_HOOK_INET);
  snprintf(cn.path, sizeof(cn.path), "%s:", b->iface.ifname);
  snprintf(cn.ourhook, sizeof(cn.ourhook), "%s", BPF_HOOK_IFACE);
  snprintf(cn.peerhook, sizeof(cn.peerhook), "%s", NG_IFACE_HOOK_INET);
  if (NgSendMsg(b->csock, path,
      NGM_GENERIC_COOKIE, NGM_CONNECT, &cn, sizeof(cn)) < 0) {
    Log(LG_ERR, ("[%s] can't connect %s and %s: %s",
      b->name, BPF_HOOK_IFACE, NG_IFACE_HOOK_INET, strerror(errno)));
    goto fail;
  }

  /* Connect a hook from the bpf node to our socket node */
  snprintf(cn.path, sizeof(cn.path), "%s.%s", MPD_HOOK_PPP, NG_PPP_HOOK_INET);
  snprintf(cn.ourhook, sizeof(cn.ourhook), "%s", MPD_HOOK_DEMAND_TAP);
  snprintf(cn.peerhook, sizeof(cn.peerhook), "%s", BPF_HOOK_MPD);
  if (NgSendMsg(b->csock, ".",
      NGM_GENERIC_COOKIE, NGM_CONNECT, &cn, sizeof(cn)) < 0) {
    Log(LG_ERR, ("[%s] can't connect %s and %s: %s",
      b->name, BPF_HOOK_MPD, MPD_HOOK_DEMAND_TAP, strerror(errno)));
    goto fail;
  }

  /* Configure bpf(8) node */
  NgFuncConfigBPF(b, BPF_MODE_OFF);

  /* Add a VJ compression node */
  snprintf(mp.type, sizeof(mp.type), "%s", NG_VJC_NODE_TYPE);
  snprintf(mp.ourhook, sizeof(mp.ourhook), "%s", NG_PPP_HOOK_VJC_IP);
  snprintf(mp.peerhook, sizeof(mp.peerhook), "%s", NG_VJC_HOOK_IP);
  if (NgSendMsg(b->csock, MPD_HOOK_PPP,
      NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0) {
    Log(LG_ERR, ("[%s] can't create %s node: %s",
      b->name, NG_VJC_NODE_TYPE, strerror(errno)));
    goto fail;
  }

  /* Connect the other three hooks between the ppp and vjc nodes */
  snprintf(cn.path, sizeof(cn.path), "%s", NG_PPP_HOOK_VJC_IP);
  snprintf(cn.ourhook, sizeof(cn.ourhook), "%s", NG_PPP_HOOK_VJC_COMP);
  snprintf(cn.peerhook, sizeof(cn.peerhook), "%s", NG_VJC_HOOK_VJCOMP);
  if (NgSendMsg(b->csock, MPD_HOOK_PPP,
      NGM_GENERIC_COOKIE, NGM_CONNECT, &cn, sizeof(cn)) < 0) {
    Log(LG_ERR, ("[%s] can't connect %s and %s: %s",
      b->name, NG_PPP_HOOK_VJC_COMP, NG_VJC_HOOK_VJCOMP, strerror(errno)));
    goto fail;
  }
  snprintf(cn.ourhook, sizeof(cn.ourhook), "%s", NG_PPP_HOOK_VJC_UNCOMP);
  snprintf(cn.peerhook, sizeof(cn.peerhook), "%s", NG_VJC_HOOK_VJUNCOMP);
  if (NgSendMsg(b->csock, MPD_HOOK_PPP,
      NGM_GENERIC_COOKIE, NGM_CONNECT, &cn, sizeof(cn)) < 0) {
    Log(LG_ERR, ("[%s] can't connect %s and %s: %s", b->name,
      NG_PPP_HOOK_VJC_UNCOMP, NG_VJC_HOOK_VJUNCOMP, strerror(errno)));
    goto fail;
  }
  snprintf(cn.ourhook, sizeof(cn.ourhook), "%s", NG_PPP_HOOK_VJC_VJIP);
  snprintf(cn.peerhook, sizeof(cn.peerhook), "%s", NG_VJC_HOOK_VJIP);
  if (NgSendMsg(b->csock, MPD_HOOK_PPP,
      NGM_GENERIC_COOKIE, NGM_CONNECT, &cn, sizeof(cn)) < 0) {
    Log(LG_ERR, ("[%s] can't connect %s and %s: %s",
      b->name, NG_PPP_HOOK_VJC_VJIP, NG_VJC_HOOK_VJIP, strerror(errno)));
    goto fail;
  }

  /* Listen for happenings on our node */
  EventRegister(&b->dataEvent, EVENT_READ,
    b->dsock, DEV_PRIO, NgFuncDataEvent, b);
  EventRegister(&b->ctrlEvent, EVENT_READ,
    b->csock, DEV_PRIO, NgFuncCtrlEvent, b);

  /* OK */
  return(0);

fail:
  NgFuncShutdownInternal(b, newIface, newPpp);
  return(-1);
}

/*
 * NgFuncIfaceExists()
 *
 * Test if a netgraph interface exists. Returns:
 *
 *	0	Netgraph interface does not exist
 *	1	Netgraph interface exists
 *     -1	Interface is not a netgraph interface
 */

static int
NgFuncIfaceExists(Bund b, const char *ifname, char *buf, int max)
{
  union {
      u_char		buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
      struct ng_mesg	reply;
  }			u;
  char		path[NG_PATHLEN + 1];
  char		*eptr;
  int		ifnum;

  /* Check interface name */
  if (strncmp(ifname, NG_IFACE_IFACE_NAME, strlen(NG_IFACE_IFACE_NAME)) != 0)
    return(-1);
  ifnum = (int)strtoul(ifname + strlen(NG_IFACE_IFACE_NAME), &eptr, 10);
  if (ifnum < 0 || *eptr != '\0')
    return(-1);

  /* See if interface exists */
  snprintf(path, sizeof(path), "%s%d:", NG_IFACE_IFACE_NAME, ifnum);
  if (NgSendMsg(b->csock, path, NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) < 0)
    return(0);
  if (NgRecvMsg(b->csock, &u.reply, sizeof(u), NULL) < 0) {
    Log(LG_ERR, ("[%s] node \"%s\" reply: %s", b->name, path, strerror(errno)));
    return(-1);
  }

  /* It exists */
  if (buf != NULL)
    snprintf(buf, max, "%s%d", NG_IFACE_IFACE_NAME, ifnum);
  return(1);
}

/*
 * NgFuncCreateIface()
 *
 * Create a new netgraph interface, optionally with a specific name.
 * If "ifname" is not NULL, then create interfaces until "ifname" is
 * created.  Interfaces are consecutively numbered when created, so
 * we have no other choice but to create all lower numbered interfaces
 * in order to create one with a given index.
 */

static int
NgFuncCreateIface(Bund b, const char *ifname, char *buf, int max)
{
  union {
      u_char		buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
      struct ng_mesg	reply;
  }			u;
  struct nodeinfo	*const ni = (struct nodeinfo *)(void *)u.reply.data;
  struct ngm_rmhook	rm;
  struct ngm_mkpeer	mp;
  int			rtn = 0;

  /* If ifname is not null, create interfaces until it gets created */
  if (ifname != NULL) {
    int count;

    for (count = 0; count < MAX_IFACE_CREATE; count++) {
      switch (NgFuncIfaceExists(b, ifname, buf, max)) {
      case 1:				/* ok now it exists */
	return(0);
      case 0:				/* nope, create another one */
	NgFuncCreateIface(b, NULL, NULL, 0);
	break;
      case -1:				/* something weird happened */
	return(-1);
      default:
	assert(0);
      }
    }
    Log(LG_ERR, ("[%s] created %d interfaces, that's too many!",
      b->name, count));
    return(-1);
  }

  /* Create iface node (as a temporary peer of the socket node) */
  snprintf(mp.type, sizeof(mp.type), "%s", NG_IFACE_NODE_TYPE);
  snprintf(mp.ourhook, sizeof(mp.ourhook), "%s", TEMPHOOK);
  snprintf(mp.peerhook, sizeof(mp.peerhook), "%s", NG_IFACE_HOOK_INET);
  if (NgSendMsg(b->csock, ".",
      NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0) {
    Log(LG_ERR, ("[%s] can't create %s node: %s",
      b->name, NG_IFACE_NODE_TYPE, strerror(errno)));
    return(-1);
  }

  /* Get the new node's name */
  if (NgSendMsg(b->csock, TEMPHOOK,
      NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) < 0) {
    Log(LG_ERR, ("[%s] %s: %s", b->name, "NGM_NODEINFO", strerror(errno)));
    rtn = -1;
    goto done;
  }
  if (NgRecvMsg(b->csock, &u.reply, sizeof(u), NULL) < 0) {
    Log(LG_ERR, ("[%s] reply from %s: %s",
      b->name, NG_IFACE_NODE_TYPE, strerror(errno)));
    rtn = -1;
    goto done;
  }
  snprintf(buf, max, "%s", ni->name);

done:
  /* Disconnect temporary hook */
  snprintf(rm.ourhook, sizeof(rm.ourhook), "%s", TEMPHOOK);
  if (NgSendMsg(b->csock, ".",
      NGM_GENERIC_COOKIE, NGM_RMHOOK, &rm, sizeof(rm)) < 0) {
    Log(LG_ERR, ("[%s] can't remove hook %s: %s",
      b->name, TEMPHOOK, strerror(errno)));
    rtn = -1;
  }

  /* Done */
  return(rtn);
}

/*
 * NgFuncConfigBPF()
 *
 * Configure the BPF node for one of three modes: either total pass through,
 * total blockage, or else block all traffic and redirect outgoing demand
 * to mpd's socket node.
 */

void
NgFuncConfigBPF(Bund b, int mode)
{
  union {
      u_char			buf[NG_BPF_HOOKPROG_SIZE(DEMAND_PROG_LEN)];
      struct ng_bpf_hookprog	hprog;
  }				u;
  struct ng_bpf_hookprog	*const hp = &u.hprog;
  char				path[NG_PATHLEN + 1];

  /* Get absolute path to bpf node */
  snprintf(path, sizeof(path), "%s:%s", b->iface.ifname, NG_IFACE_HOOK_INET);

  /* First, configure the hook on the interface node side of the BPF node */
  memset(&u, 0, sizeof(u));
  snprintf(hp->thisHook, sizeof(hp->thisHook), "%s", BPF_HOOK_IFACE);
  hp->bpf_prog_len = DEMAND_PROG_LEN;
  memcpy(&hp->bpf_prog, &gDemandProg, DEMAND_PROG_LEN * sizeof(*gDemandProg));
  switch (mode) {
    case BPF_MODE_OFF:
      memset(&hp->ifMatch, 0, sizeof(hp->ifMatch));
      memset(&hp->ifNotMatch, 0, sizeof(hp->ifNotMatch));
      break;
    case BPF_MODE_ON:
    case BPF_MODE_MSSFIX:
      snprintf(hp->ifMatch, sizeof(hp->ifMatch), "%s", BPF_HOOK_PPP);
      snprintf(hp->ifNotMatch, sizeof(hp->ifNotMatch), "%s", BPF_HOOK_PPP);
      break;
    case BPF_MODE_DEMAND:
      snprintf(hp->ifMatch, sizeof(hp->ifMatch), "%s", BPF_HOOK_MPD);
      memset(&hp->ifNotMatch, 0, sizeof(hp->ifNotMatch));
      break;
    default:
      assert(0);
  }

  /* Set new program on the BPF_HOOK_IFACE hook */
  if (NgSendMsg(b->csock, path, NGM_BPF_COOKIE,
      NGM_BPF_SET_PROGRAM, hp, NG_BPF_HOOKPROG_SIZE(hp->bpf_prog_len)) < 0) {
    Log(LG_ERR, ("[%s] can't set %s node program: %s",
      b->name, NG_BPF_NODE_TYPE, strerror(errno)));
    DoExit(EX_ERRDEAD);
  }

  /* Now, configure the hook on the PPP node side of the BPF node */
  memset(&u, 0, sizeof(u));
  snprintf(hp->thisHook, sizeof(hp->thisHook), "%s", BPF_HOOK_PPP);
  hp->bpf_prog_len = TCPSYN_PROG_LEN;
  memcpy(&hp->bpf_prog,
    &gTCPSYNProg, TCPSYN_PROG_LEN * sizeof(*gTCPSYNProg));
  switch (mode) {
    case BPF_MODE_OFF:
    case BPF_MODE_DEMAND:
      memset(&hp->ifMatch, 0, sizeof(hp->ifMatch));
      memset(&hp->ifNotMatch, 0, sizeof(hp->ifNotMatch));
      break;
    case BPF_MODE_ON:
      snprintf(hp->ifMatch, sizeof(hp->ifMatch), "%s", BPF_HOOK_IFACE);
      snprintf(hp->ifNotMatch, sizeof(hp->ifNotMatch), "%s", BPF_HOOK_IFACE);
      break;
    case BPF_MODE_MSSFIX:
      snprintf(hp->ifMatch, sizeof(hp->ifMatch), "%s", BPF_HOOK_MPD);
      snprintf(hp->ifNotMatch, sizeof(hp->ifNotMatch), "%s", BPF_HOOK_IFACE);
      break;
    default:
      assert(0);
  }

  /* Set new program on the BPF_HOOK_IFACE hook */
  if (NgSendMsg(b->csock, path, NGM_BPF_COOKIE,
      NGM_BPF_SET_PROGRAM, hp, NG_BPF_HOOKPROG_SIZE(hp->bpf_prog_len)) < 0) {
    Log(LG_ERR, ("[%s] can't set %s node program: %s",
      b->name, NG_BPF_NODE_TYPE, strerror(errno)));
    DoExit(EX_ERRDEAD);
  }
  /* Configure the hook on the MPD node side of the BPF node */
  memset(&u, 0, sizeof(u));
  snprintf(hp->thisHook, sizeof(hp->thisHook), "%s", BPF_HOOK_MPD);
  hp->bpf_prog_len = NOMATCH_PROG_LEN;
  memcpy(&hp->bpf_prog,
    &gNoMatchProg, NOMATCH_PROG_LEN * sizeof(*gNoMatchProg));
  switch (mode) {
    case BPF_MODE_OFF:
    case BPF_MODE_DEMAND:
      memset(&hp->ifMatch, 0, sizeof(hp->ifMatch));
      memset(&hp->ifNotMatch, 0, sizeof(hp->ifNotMatch));
      break;
    case BPF_MODE_ON:
    case BPF_MODE_MSSFIX:
      snprintf(hp->ifMatch, sizeof(hp->ifMatch), "%s", BPF_HOOK_IFACE);
      snprintf(hp->ifNotMatch, sizeof(hp->ifNotMatch), "%s", BPF_HOOK_IFACE);
      break;
    default:
      assert(0);
  }

  /* Set new program on the BPF_HOOK_IFACE hook */
  if (NgSendMsg(b->csock, path, NGM_BPF_COOKIE,
      NGM_BPF_SET_PROGRAM, hp, NG_BPF_HOOKPROG_SIZE(hp->bpf_prog_len)) < 0) {
    Log(LG_ERR, ("[%s] can't set %s node program: %s",
      b->name, NG_BPF_NODE_TYPE, strerror(errno)));
    DoExit(EX_ERRDEAD);
  }
}

/*
 * NgFuncShutdown()
 *
 * Shutdown the netgraph stuff associated with the current bundle
 */

void
NgFuncShutdown(Bund b)
{
  NgFuncShutdownInternal(b, 1, 1);
}

/*
 * NgFuncShutdownInternal()
 */

static void
NgFuncShutdownInternal(Bund b, int iface, int ppp)
{
  char	path[NG_PATHLEN + 1];
  Bund	bund_save;
  Link	lnk_save;
  int	k;

  if (iface) {
    snprintf(path, sizeof(path), "%s:", b->iface.ifname);
    NgFuncShutdownNode(b, b->name, path);
  }
  lnk_save = lnk;
  bund_save = bund;
  for (k = 0; k < b->n_links; k++) {
    lnk = b->links[k];
    bund = lnk->bund;
    if (lnk && lnk->phys && lnk->phys->type && lnk->phys->type->shutdown)
      (*lnk->phys->type->shutdown)(lnk->phys);
  }
  bund = bund_save;
  lnk = lnk_save;
  if (ppp) {
    snprintf(path, sizeof(path), "%s.%s", MPD_HOOK_PPP, NG_PPP_HOOK_INET);
    NgFuncShutdownNode(b, b->name, path);
    NgFuncShutdownNode(b, b->name, MPD_HOOK_PPP);
  }
  close(b->csock);
  b->csock = -1;
  EventUnRegister(&b->ctrlEvent);
  close(b->dsock);
  b->dsock = -1;
  EventUnRegister(&b->dataEvent);
}

/*
 * NgFuncShutdownNode()
 */

int
NgFuncShutdownNode(Bund b, const char *label, const char *path)
{
  int rtn;

  if ((rtn = NgSendMsg(b->csock, path,
      NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0)) < 0) {
    if (errno != ENOENT) {
      Log(LG_ERR, ("[%s] can't shutdown \"%s\": %s",
	label, path, strerror(errno)));
    }
  }
  return(rtn);
}

/*
 * NgFuncSetConfig()
 */

void
NgFuncSetConfig(void)
{
  if (NgSendMsg(bund->csock, MPD_HOOK_PPP, NGM_PPP_COOKIE,
      NGM_PPP_SET_CONFIG, &bund->pppConfig, sizeof(bund->pppConfig)) < 0) {
    Log(LG_ERR, ("[%s] can't config %s: %s",
      bund->name, MPD_HOOK_PPP, strerror(errno)));
    DoExit(EX_ERRDEAD);
  }
}

/*
 * NgFuncDataEvent()
 */

static void
NgFuncDataEvent(int type, void *cookie)
{
  u_char		buf[8192];
  struct sockaddr_ng	naddr;
  int			nread, nsize = sizeof(naddr);

  /* Set bundle */
  bund = (Bund) cookie;
  lnk = bund->links[0];

  /* Re-register event */
  EventRegister(&bund->dataEvent, EVENT_READ,
    bund->dsock, DEV_PRIO, NgFuncDataEvent, bund);

  /* Read data */
  if ((nread = recvfrom(bund->dsock, buf, sizeof(buf),
      0, (struct sockaddr *)&naddr, &nsize)) < 0) {
    if (errno == EAGAIN)
      return;
    Log(LG_BUND, ("[%s] socket read: %s", bund->name, strerror(errno)));
    DoExit(EX_ERRDEAD);
  }

  /* A PPP frame from the bypass hook? */
  if (strcmp(naddr.sg_data, MPD_HOOK_PPP) == 0) {
    u_int16_t	linkNum, proto;

    /* Extract link number and protocol */
    memcpy(&linkNum, buf, 2);
    linkNum = ntohs(linkNum);
    memcpy(&proto, buf + 2, 2);
    proto = ntohs(proto);

    /* Debugging */
    LogDumpBuf(LG_FRAME, buf, nread,
      "[%s] rec'd bypass frame link=%d proto=0x%04x",
      bund->name, (int16_t)linkNum, proto);

    /* Set link */
    assert(linkNum == NG_PPP_BUNDLE_LINKNUM || linkNum < bund->n_links);
    lnk = (linkNum < bund->n_links) ? bund->links[linkNum] : NULL;

    /* Input frame */
    InputFrame(linkNum, proto,
      mbwrite(mballoc(MB_FRAME_IN, nread - 4), buf + 4, nread - 4));
    return;
  }

  /* A snooped, outgoing IP frame? */
  if (strcmp(naddr.sg_data, MPD_HOOK_DEMAND_TAP) == 0) {

    /* Debugging */
    LogDumpBuf(LG_FRAME, buf, nread,
      "[%s] rec'd outgoing IP frame", bund->name);
    IfaceListenInput(PROTO_IP,
      mbwrite(mballoc(MB_FRAME_IN, nread), buf, nread));
    return;
  }

  /* Unknown hook! */
  LogDumpBuf(LG_FRAME, buf, nread,
    "[%s] rec'd data on unknown hook \"%s\"", bund->name, naddr.sg_data);
  DoExit(EX_ERRDEAD);
}

/*
 * NgFuncCtrlEvent()
 */

static void
NgFuncCtrlEvent(int type, void *cookie)
{
  union {
      u_char		buf[8192];
      struct ng_mesg	msg;
  }			u;
  char			raddr[NG_PATHLEN + 1];
  int			len;

  /* Set bundle */
  bund = (Bund) cookie;
  lnk = bund->links[0];

  /* Re-register */
  EventRegister(&bund->ctrlEvent, EVENT_READ,
    bund->csock, DEV_PRIO, NgFuncCtrlEvent, bund);

  /* Read message */
  if ((len = NgRecvMsg(bund->csock, &u.msg, sizeof(u), raddr)) < 0) {
    Log(LG_ERR, ("[%s] can't read unexpected message: %s",
      bund->name, strerror(errno)));
    return;
  }

  /* Examine message */
  switch (u.msg.header.typecookie) {
#ifdef COMPRESSION_MPPC
    case NGM_MPPC_COOKIE:
      CcpRecvMsg(&u.msg, len);
      return;
#endif
    case NGM_KSOCKET_COOKIE:		/* XXX ignore NGM_KSOCKET_CONNECT */
      if (u.msg.header.cmd == NGM_KSOCKET_CONNECT)
	return;
      break;
    default:
      break;
  }

  /* Unknown message */
  Log(LG_ERR, ("[%s] rec'd unknown ctrl message, cookie=%d cmd=%d",
    bund->name, u.msg.header.typecookie, u.msg.header.cmd));
}

/*
 * NgFuncConnect()
 */

int
NgFuncConnect(const char *path, const char *hook,
	const char *path2, const char *hook2)
{
  struct ngm_connect	cn;

  snprintf(cn.path, sizeof(cn.path), "%s", path2);
  snprintf(cn.ourhook, sizeof(cn.ourhook), "%s", hook);
  snprintf(cn.peerhook, sizeof(cn.peerhook), "%s", hook2);
  if (NgSendMsg(bund->csock, path,
      NGM_GENERIC_COOKIE, NGM_CONNECT, &cn, sizeof(cn)) < 0) {
    Log(LG_ERR, ("[%s] can't connect %s,%s and %s,%s: %s",
      bund->name, path, hook, path2, hook2, strerror(errno)));
    return(-1);
  }
  return(0);
}

/*
 * NgFuncDisconnect()
 */

int
NgFuncDisconnect(const char *path, const char *hook)
{
  struct ngm_rmhook	rm;

  /* Disconnect hook */
  snprintf(rm.ourhook, sizeof(rm.ourhook), "%s", hook);
  if (NgSendMsg(bund->csock, path,
      NGM_GENERIC_COOKIE, NGM_RMHOOK, &rm, sizeof(rm)) < 0) {
    Log(LG_ERR, ("[%s] can't remove hook %s from node \"%s\": %s",
      bund->name, hook, path, strerror(errno)));
    return(-1);
  }
  return(0);
}

/*
 * NgFuncWritePppFrame()
 *
 * Consumes the mbuf.
 */

int
NgFuncWritePppFrame(int linkNum, int proto, Mbuf bp)
{
  Mbuf		hdr;
  u_int16_t	temp;

  /* Prepend ppp node bypass header */
  hdr = mballoc(bp->type, 4);
  temp = htons(linkNum);
  memcpy(MBDATA(hdr), &temp, 2);
  temp = htons(proto);
  memcpy(MBDATA(hdr) + 2, &temp, 2);
  hdr->next = bp;
  bp = hdr;

  /* Debugging */
  LogDumpBp(LG_FRAME, bp,
    "[%s] xmit bypass frame link=%d proto=0x%04x",
    bund->name, (int16_t)linkNum, proto);

  /* Write frame */
  return NgFuncWriteFrame(
    linkNum == NG_PPP_BUNDLE_LINKNUM ? bund->name : bund->links[linkNum]->name,
    MPD_HOOK_PPP, bp);
}

/*
 * NgFuncWriteFrame()
 *
 * Consumes the mbuf.
 */

int
NgFuncWriteFrame(const char *label, const char *hookname, Mbuf bp)
{
  u_char		buf[sizeof(struct sockaddr_ng) + NG_HOOKLEN];
  struct sockaddr_ng	*ng = (struct sockaddr_ng *)buf;
  int			rtn;

  /* Set dest address */
  memset(&buf, 0, sizeof(buf));
  snprintf(ng->sg_data, NG_HOOKLEN + 1, "%s", hookname);
  ng->sg_family = AF_NETGRAPH;
  ng->sg_len = 3 + strlen(ng->sg_data);

  /* Write frame */
  bp = mbunify(bp);
  rtn = sendto(bund->dsock, MBDATA(bp), MBLEN(bp),
    0, (struct sockaddr *)ng, ng->sg_len);

  /* ENOBUFS can be expected on some links, e.g., ng_pptpgre(4) */
  if (rtn < 0 && errno != ENOBUFS) {
    Log(LG_ERR, ("[%s] error writing len %d frame to %s: %s",
      label, MBLEN(bp), hookname, strerror(errno)));
  }
  PFREE(bp);
  return rtn;
}

/*
 * NgFuncGetStats()
 *
 * Get (and optionally clear) link or whole bundle statistics
 */

int
NgFuncGetStats(u_int16_t linkNum, int clear, struct ng_ppp_link_stat *statp)
{
  union {
      u_char			buf[sizeof(struct ng_mesg)
				  + sizeof(struct ng_ppp_link_stat)];
      struct ng_mesg		reply;
  }				u;
  int				cmd;

  /* Get stats */
  cmd = clear ? NGM_PPP_GETCLR_LINK_STATS : NGM_PPP_GET_LINK_STATS;
  if (NgSendMsg(bund->csock, MPD_HOOK_PPP,
      NGM_PPP_COOKIE, cmd, &linkNum, sizeof(linkNum)) < 0) {
    Log(LG_ERR, ("[%s] can't get stats, link=%d: %s",
      bund->name, linkNum, strerror(errno)));
    return(-1);
  }
  if (NgRecvMsg(bund->csock, &u.reply, sizeof(u), NULL) < 0) {
    Log(LG_ERR, ("[%s] node \"%s\" reply: %s",
      bund->name, MPD_HOOK_PPP, strerror(errno)));
    return(-1);
  }
  if (statp != NULL)
    memcpy(statp, u.reply.data, sizeof(*statp));
  return(0);
}

/*
 * NgFuncErrx()
 */

static void
NgFuncErrx(const char *fmt, ...)
{
  char		buf[1024];
  va_list	args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Log(LG_ERR, ("[%s] netgraph: %s", bund ? bund->name : "", buf));
}

/*
 * NgFuncErr()
 */

static void
NgFuncErr(const char *fmt, ...)
{
  char		buf[100];
  va_list	args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Log(LG_ERR, ("[%s] netgraph: %s: %s", bund ? bund->name : "",
    buf, strerror(errno)));
}


