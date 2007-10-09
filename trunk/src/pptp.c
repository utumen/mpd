
/*
 * pptp.c
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1998-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#include "ppp.h"
#include "phys.h"
#include "mbuf.h"
#include "ngfunc.h"
#include "pptp.h"
#include "pptp_ctrl.h"
#include "log.h"

#include <netgraph/ng_message.h>
#ifdef __DragonFly__
#include <netgraph/socket/ng_socket.h>
#include <netgraph/ksocket/ng_ksocket.h>
#include <netgraph/pptpgre/ng_pptpgre.h>
#else
#include <netgraph/ng_socket.h>
#include <netgraph/ng_ksocket.h>
#include <netgraph/ng_pptpgre.h>
#endif
#include <netgraph.h>

/*
 * DEFINITIONS
 */

  #define PPTP_MRU		PPTP_MTU

  #define PPTP_MAX_ERRORS	10
  #define PPTP_REOPEN_PAUSE	5

  #define MAX_IOVEC		32

  #define PPTP_CALL_MIN_BPS	56000
  #define PPTP_CALL_MAX_BPS	64000

  struct pptpinfo {
    struct {
	struct u_addr	self_addr;	/* self IP address */
	struct u_range	peer_addr;	/* Peer IP addresses allowed */
	in_port_t	self_port;	/* self port */
	in_port_t	peer_port;	/* Peer port required (or zero) */
	struct optinfo	options;
	char		callingnum[64];	/* PPTP phone number to use */
	char		callednum[64];	/* PPTP phone number to use */
    } conf;
    void		*listener;	/* Listener pointer */
    struct u_addr	self_addr;	/* Current self IP address */
    struct u_addr	peer_addr;	/* Current peer IP address */
    in_port_t		peer_port;	/* Current peer port */
    u_char		originate;	/* Call originated locally */
    u_char		outcall;	/* Call is outgoing vs. incoming */
    u_char		sync;		/* Call is sync vs. async */
    struct pptpctrlinfo	cinfo;
    ng_ID_t		node_id;
    char		callingnum[64];	/* PPTP phone number to use */
    char		callednum[64];	/* PPTP phone number to use */
  };
  typedef struct pptpinfo	*PptpInfo;

  /* Set menu options */
  enum {
    SET_SELFADDR,
    SET_PEERADDR,
    SET_CALLINGNUM,
    SET_CALLEDNUM,
    SET_ENABLE,
    SET_DISABLE,
  };

  /* Binary options */
  enum {
    PPTP_CONF_OUTCALL,		/* when originating, calls are "outgoing" */
    PPTP_CONF_DELAYED_ACK,	/* enable delayed receive ack algorithm */
    PPTP_CONF_ALWAYS_ACK,	/* include ack with all outgoing data packets */
#if NGM_PPTPGRE_COOKIE >= 1082548365
    PPTP_CONF_WINDOWING,	/* control (stupid) windowing algorithm */
#endif
  };

/*
 * INTERNAL FUNCTIONS
 */

  static int	PptpInit(Link l);
  static int	PptpInst(Link l, Link lt);
  static void	PptpOpen(Link l);
  static void	PptpClose(Link l);
  static void	PptpShutdown(Link l);
  static void	PptpStat(Context ctx);
  static int	PptpOriginated(Link l);
  static int	PptpIsSync(Link l);
  static int	PptpSetAccm(Link l, u_int32_t xmit, u_int32_t recv);
  static int	PptpSetCallingNum(Link l, void *buf);
  static int	PptpSetCalledNum(Link l, void *buf);
  static int	PptpPeerAddr(Link l, void *buf, int buf_len);
  static int	PptpPeerPort(Link l, void *buf, int buf_len);
  static int	PptpCallingNum(Link l, void *buf, int buf_len);
  static int	PptpCalledNum(Link l, void *buf, int buf_len);

  static void	PptpInitCtrl(void);
  static int	PptpOriginate(Link l);
  static void	PptpDoClose(Link l);
  static void	PptpKillNode(Link l);
  static void	PptpResult(void *cookie, const char *errmsg, int frameType);
  static void	PptpSetLinkInfo(void *cookie, u_int32_t sa, u_int32_t ra);
  static void	PptpCancel(void *cookie);
  static int	PptpHookUp(Link l);
  static void	PptpListenUpdate(Link l);

  static struct pptplinkinfo	PptpIncoming(struct pptpctrlinfo *cinfo,
				  struct u_addr *self, struct u_addr *peer, in_port_t port, int bearType,
				  const char *callingNum,
				  const char *calledNum,
				  const char *subAddress);

  static struct pptplinkinfo	PptpOutgoing(struct pptpctrlinfo *cinfo,
				  struct u_addr *self, struct u_addr *peer, in_port_t port, int bearType,
				  int frameType, int minBps, int maxBps,
				  const char *calledNum,
				  const char *subAddress);

  static struct pptplinkinfo	PptpPeerCall(struct pptpctrlinfo *cinfo,
				  struct u_addr *self, struct u_addr *peer, in_port_t port, int incoming,
				  const char *callingNum,
				  const char *calledNum,
				  const char *subAddress);

  static int	PptpSetCommand(Context ctx, int ac, char *av[], void *arg);

/*
 * GLOBAL VARIABLES
 */

  const struct phystype	gPptpPhysType = {
    .name		= "pptp",
    .descr		= "Point-to-Point Tunneling Protocol",
    .minReopenDelay	= PPTP_REOPEN_PAUSE,
    .mtu		= PPTP_MTU,
    .mru		= PPTP_MRU,
    .tmpl		= 1,
    .init		= PptpInit,
    .inst		= PptpInst,
    .open		= PptpOpen,
    .close		= PptpClose,
    .update		= PptpListenUpdate,
    .shutdown		= PptpShutdown,
    .showstat		= PptpStat,
    .originate		= PptpOriginated,
    .issync		= PptpIsSync,
    .setaccm            = PptpSetAccm,
    .setcallingnum	= PptpSetCallingNum,
    .setcallednum	= PptpSetCalledNum,
    .peeraddr		= PptpPeerAddr,
    .peerport		= PptpPeerPort,
    .callingnum		= PptpCallingNum,
    .callednum		= PptpCalledNum,
  };

  const struct cmdtab	PptpSetCmds[] = {
    { "self ip [port]",			"Set local IP address",
	PptpSetCommand, NULL, (void *) SET_SELFADDR },
    { "peer ip [port]",			"Set remote IP address",
	PptpSetCommand, NULL, (void *) SET_PEERADDR },
    { "callingnum number",		"Set calling PPTP telephone number",
	PptpSetCommand, NULL, (void *) SET_CALLINGNUM },
    { "callednum number",		"Set called PPTP telephone number",
	PptpSetCommand, NULL, (void *) SET_CALLEDNUM },
    { "enable [opt ...]",		"Enable option",
	PptpSetCommand, NULL, (void *) SET_ENABLE },
    { "disable [opt ...]",		"Disable option",
	PptpSetCommand, NULL, (void *) SET_DISABLE },
    { NULL },
  };

/*
 * INTERNAL VARIABLES
 */

  static u_char			gInitialized;
  static struct confinfo	gConfList[] = {
    { 0,	PPTP_CONF_OUTCALL,	"outcall"	},
    { 0,	PPTP_CONF_DELAYED_ACK,	"delayed-ack"	},
    { 0,	PPTP_CONF_ALWAYS_ACK,	"always-ack"	},
#if NGM_PPTPGRE_COOKIE >= 1082548365
    { 0,	PPTP_CONF_WINDOWING,	"windowing"	},
#endif
    { 0,	0,			NULL		},
  };

/*
 * PptpInit()
 */

static int
PptpInit(Link l)
{
  PptpInfo	pptp;

  /* Initialize this link */
  pptp = (PptpInfo) (l->info = Malloc(MB_PHYS, sizeof(*pptp)));

  pptp->conf.self_addr.family = AF_INET;
  Enable(&pptp->conf.options, PPTP_CONF_OUTCALL);
  Enable(&pptp->conf.options, PPTP_CONF_DELAYED_ACK);
#if NGM_PPTPGRE_COOKIE >= 1082548365
  Enable(&pptp->conf.options, PPTP_CONF_WINDOWING);
#endif

  /* Initialize first time */
  if (!gInitialized) {
    PptpInitCtrl();
  }

  return(0);
}

/*
 * PptpInst()
 */

static int
PptpInst(Link l, Link lt)
{
    PptpInfo	pptp;

    /* Initialize this link */
    pptp = (PptpInfo) (l->info = Mdup(MB_PHYS, lt->info, sizeof(*pptp)));
    pptp->listener = NULL;

    return(0);
}

/*
 * PptpOpen()
 */

static void
PptpOpen(Link l)
{
  PptpInfo		const pptp = (PptpInfo) l->info;

  /* Check state */
  switch (l->state) {
    case PHYS_STATE_DOWN:
      if (PptpOriginate(l) < 0) {
	Log(LG_PHYS, ("[%s] PPTP call failed", l->name));
	PhysDown(l, STR_ERROR, NULL);
	return;
      }
      l->state = PHYS_STATE_CONNECTING;
      break;

    case PHYS_STATE_CONNECTING:
      if (pptp->originate)	/* our call to peer is already in progress */
	break;
      if (pptp->outcall) {

	/* Hook up nodes */
	Log(LG_PHYS, ("[%s] PPTP: attaching to peer's outgoing call", l->name));
	if (PptpHookUp(l) < 0) {
	  PptpDoClose(l);	/* We should not set state=DOWN as PptpResult() will be called once more */
	  break;
	}

	l->state = PHYS_STATE_UP;
	PhysUp(l);

	(*pptp->cinfo.answer)(pptp->cinfo.cookie,
	  PPTP_OCR_RESL_OK, 0, 0, 64000 /*XXX*/ );
	return;
      }
      return; 	/* wait for peer's incoming pptp call to complete */

    case PHYS_STATE_UP:
      PhysUp(l);
      return;

    default:
      assert(0);
  }
}

/*
 * PptpOriginate()
 *
 * Initiate an "incoming" or an "outgoing" call to the remote site
 */

static int
PptpOriginate(Link l)
{
  PptpInfo		const pptp = (PptpInfo) l->info;
  struct pptpctrlinfo	cinfo;
  struct pptplinkinfo	linfo;
  const u_short		port = pptp->conf.peer_port ?
			  pptp->conf.peer_port : PPTP_PORT;

  pptp->originate = TRUE;
  pptp->outcall = Enabled(&pptp->conf.options, PPTP_CONF_OUTCALL);
  memset(&linfo, 0, sizeof(linfo));
  linfo.cookie = l;
  linfo.result = PptpResult;
  linfo.setLinkInfo = PptpSetLinkInfo;
  linfo.cancel = PptpCancel;
  strlcpy(pptp->callingnum, pptp->conf.callingnum, sizeof(pptp->callingnum));
  strlcpy(pptp->callednum, pptp->conf.callednum, sizeof(pptp->callednum));
  if (!pptp->outcall) {
    int frameType = PPTP_FRAMECAP_SYNC;
    if (l->rep && !RepIsSync(l))
	    frameType = PPTP_FRAMECAP_ASYNC;
    cinfo = PptpCtrlInCall(linfo, 
      &pptp->conf.self_addr, &pptp->conf.peer_addr.addr, port,
      PPTP_BEARCAP_ANY, frameType,
      PPTP_CALL_MIN_BPS, PPTP_CALL_MAX_BPS, 
      pptp->callingnum, pptp->callednum, "");
  } else {
    cinfo = PptpCtrlOutCall(linfo, 
      &pptp->conf.self_addr, &pptp->conf.peer_addr.addr, port,
      PPTP_BEARCAP_ANY, PPTP_FRAMECAP_ANY,
      PPTP_CALL_MIN_BPS, PPTP_CALL_MAX_BPS,
      pptp->callednum, "");
  }
  if (cinfo.cookie == NULL)
    return(-1);
  pptp->self_addr = pptp->conf.self_addr;
  pptp->peer_addr = pptp->conf.peer_addr.addr;
  pptp->peer_port = port;
  pptp->cinfo = cinfo;
  return(0);
}

/*
 * PptpClose()
 */

static void
PptpClose(Link l)
{
  PptpDoClose(l);
}

/*
 * PptpShutdown()
 */

static void
PptpShutdown(Link l)
{
    PptpInfo      const pptp = (PptpInfo) l->info;

    if (pptp->listener) {
	PptpCtrlUnListen(pptp->listener);
	pptp->listener = NULL;
    }
    PptpKillNode(l);
    Freee(MB_PHYS, l->info);
}

/*
 * PptpDoClose()
 */

static void
PptpDoClose(Link l)
{
  PptpInfo      const pptp = (PptpInfo) l->info;

  if (l->state != PHYS_STATE_DOWN) {		/* avoid double close */
    (*pptp->cinfo.close)(pptp->cinfo.cookie, PPTP_CDN_RESL_ADMIN, 0, 0);
    PptpKillNode(l);
  }
}

/*
 * PptpKillNode()
 */

static void
PptpKillNode(Link l)
{
	PptpInfo const	pptp = (PptpInfo) l->info;
	char		path[NG_PATHSIZ];
	int		csock = -1;

	if (pptp->node_id == 0)
		return;

	/* Get a temporary netgraph socket node */
	if (NgMkSockNode(NULL, &csock, NULL) == -1) {
		Log(LG_ERR, ("PPTP: NgMkSockNode: %s", strerror(errno)));
		return;
	}
	
	/* Disconnect session hook. */
	snprintf(path, sizeof(path), "[%lx]:", (u_long)pptp->node_id);
	NgFuncShutdownNode(csock, l->name, path);
	
	close(csock);
	
	pptp->node_id = 0;
}

/*
 * PptpOriginated()
 */

static int
PptpOriginated(Link l)
{
  PptpInfo	const pptp = (PptpInfo) l->info;

  return(pptp->originate ? LINK_ORIGINATE_LOCAL : LINK_ORIGINATE_REMOTE);
}

/*
 * PptpIsSync()
 */

static int
PptpIsSync(Link l)
{
    PptpInfo	const pptp = (PptpInfo) l->info;

    return (pptp->sync);
}

static int
PptpSetAccm(Link l, u_int32_t xmit, u_int32_t recv)
{
    PptpInfo	const pptp = (PptpInfo) l->info;
    
    if (!pptp->cinfo.close || !pptp->cinfo.cookie)
	    return (-1);

    (*pptp->cinfo.setLinkInfo)(pptp->cinfo.cookie, xmit, recv);
    return (0);
}

static int
PptpSetCallingNum(Link l, void *buf)
{
    PptpInfo	const pptp = (PptpInfo) l->info;

    strlcpy(pptp->conf.callingnum, buf, sizeof(pptp->conf.callingnum));
    return(0);
}

static int
PptpSetCalledNum(Link l, void *buf)
{
    PptpInfo	const pptp = (PptpInfo) l->info;

    strlcpy(pptp->conf.callednum, buf, sizeof(pptp->conf.callednum));
    return(0);
}

static int
PptpPeerAddr(Link l, void *buf, int buf_len)
{
  PptpInfo	const pptp = (PptpInfo) l->info;

  if (u_addrtoa(&pptp->peer_addr, buf, buf_len))
    return(0);
  else
    return(-1);
}

static int
PptpPeerPort(Link l, void *buf, int buf_len)
{
  PptpInfo	const pptp = (PptpInfo) l->info;

  if (snprintf(buf, buf_len, "%d", pptp->peer_port))
    return(0);
  else
    return(-1);
}

static int
PptpCallingNum(Link l, void *buf, int buf_len)
{
    PptpInfo	const pptp = (PptpInfo) l->info;

    strlcpy((char*)buf, pptp->callingnum, buf_len);
    return(0);
}

static int
PptpCalledNum(Link l, void *buf, int buf_len)
{
    PptpInfo	const pptp = (PptpInfo) l->info;

    strlcpy((char*)buf, pptp->callednum, buf_len);
    return(0);
}

/*
 * PptpStat()
 */

void
PptpStat(Context ctx)
{
  PptpInfo	const pptp = (PptpInfo) ctx->lnk->info;
  char		buf[32];

  Printf("PPTP configuration:\r\n");
  Printf("\tSelf addr    : %s",
    u_addrtoa(&pptp->conf.self_addr, buf, sizeof(buf)));
  if (pptp->conf.self_port)
    Printf(", port %u", pptp->conf.self_port);
  Printf("\r\n");
  Printf("\tPeer range   : %s",
    u_rangetoa(&pptp->conf.peer_addr, buf, sizeof(buf)));
  if (pptp->conf.peer_port)
    Printf(", port %u", pptp->conf.peer_port);
  Printf("\r\n");
  Printf("\tCalling number: %s\r\n", pptp->conf.callingnum);
  Printf("\tCalled number: %s\r\n", pptp->conf.callednum);
  Printf("PPTP options:\r\n");
  OptStat(ctx, &pptp->conf.options, gConfList);
  Printf("PPTP status:\r\n");
  Printf("\tState        : %s\r\n", gPhysStateNames[ctx->lnk->state]);
  if (ctx->lnk->state != PHYS_STATE_DOWN) {
    Printf("\tIncoming     : %s\r\n", (pptp->originate?"NO":"YES"));
    Printf("\tCurrent self : %s\r\n",
	u_addrtoa(&pptp->self_addr, buf, sizeof(buf)));
    Printf("\tCurrent peer : %s, port %u\r\n",
	u_addrtoa(&pptp->peer_addr, buf, sizeof(buf)), pptp->peer_port);
    Printf("\tFraming      : %s\r\n", (pptp->sync?"Sync":"Async"));
    Printf("\tCalling number: %s\r\n", pptp->callingnum);
    Printf("\tCalled number: %s\r\n", pptp->callednum);
  }
}

/*
 * PptpInitCtrl()
 */

static void
PptpInitCtrl(void)
{
  if (PptpCtrlInit(PptpIncoming, PptpOutgoing) < 0) {
    Log(LG_ERR, ("PPTP ctrl init failed"));
    return;
  }
  gInitialized = TRUE;
}

/*
 * PptpResult()
 *
 * The control code calls this function to report a PPTP link
 * being connected, disconnected, or failing to connect.
 */

static void
PptpResult(void *cookie, const char *errmsg, int frameType)
{
  PptpInfo	pptp;
  Link 	l;

  /* It this fake call? */
  if (!cookie)
    return;

  l = (Link)cookie;
  pptp = (PptpInfo) l->info;

  switch (l->state) {
    case PHYS_STATE_CONNECTING:
      if (!errmsg) {

	/* Hook up nodes */
	Log(LG_PHYS, ("[%s] PPTP call successful", l->name));
	if (PptpHookUp(l) < 0) {
	  PptpDoClose(l); /* We should not set state=DOWN as PptpResult() will be called once more */
	  break;
	}

	if (pptp->originate && !pptp->outcall)
		(*pptp->cinfo.connected)(pptp->cinfo.cookie, 64000 /*XXX*/ );

	/* OK */
	l->state = PHYS_STATE_UP;
	pptp->sync = (frameType&PPTP_FRAMECAP_ASYNC)?0:1;
	PhysUp(l);
      } else {
	Log(LG_PHYS, ("[%s] PPTP call failed", l->name));
	l->state = PHYS_STATE_DOWN;
	u_addrclear(&pptp->self_addr);
	u_addrclear(&pptp->peer_addr);
	pptp->peer_port = 0;
        pptp->callingnum[0]=0;
        pptp->callednum[0]=0;
	PhysDown(l, STR_CON_FAILED, "%s", errmsg);
      }
      break;
    case PHYS_STATE_UP:
      assert(errmsg);
      Log(LG_PHYS, ("[%s] PPTP call terminated", l->name));
      PptpDoClose(l);
      l->state = PHYS_STATE_DOWN;
      u_addrclear(&pptp->self_addr);
      u_addrclear(&pptp->peer_addr);
      pptp->peer_port = 0;
      pptp->callingnum[0]=0;
      pptp->callednum[0]=0;
      PhysDown(l, STR_DROPPED, NULL);
      break;
    case PHYS_STATE_DOWN:
      return;
    default:
      assert(0);
  }
}

/*
 * PptpSetLinkInfo()
 *
 * Received LinkInfo from peer;
 */

void
PptpSetLinkInfo(void *cookie, u_int32_t sa, u_int32_t ra)
{
    Link 	l;

    /* It this fake call? */
    if (!cookie)
	    return;

    l = (Link)cookie;

    if (l->rep != NULL) {
	    RepSetAccm(l, sa, ra);
    }
};

/*
 * PptpHookUp()
 *
 * Connect the PPTP/GRE node to the PPP node
 */

static int
PptpHookUp(Link l)
{
  const PptpInfo		pi = (PptpInfo)l->info;
  char	        		ksockpath[NG_PATHSIZ];
  char	        		pptppath[NG_PATHSIZ];
  struct ngm_mkpeer		mkp;
  struct ng_pptpgre_conf	gc;
  struct sockaddr_storage	self_addr, peer_addr;
  struct u_addr			u_self_addr, u_peer_addr;
  union {
	u_char buf[sizeof(struct ng_ksocket_sockopt) + sizeof(int)];
	struct ng_ksocket_sockopt ksso;
  } u;
  struct ng_ksocket_sockopt *const ksso = &u.ksso;
  int		csock = -1;
  char        	path[NG_PATHSIZ];
  char		hook[NG_HOOKSIZ];
    union {
        u_char buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
        struct ng_mesg reply;
    } repbuf;
    struct ng_mesg *const reply = &repbuf.reply;
    struct nodeinfo *ninfo = (struct nodeinfo *)&reply->data;

  /* Get session info */
  memset(&gc, 0, sizeof(gc));
  PptpCtrlGetSessionInfo(&pi->cinfo, &u_self_addr,
    &u_peer_addr, &gc.cid, &gc.peerCid, &gc.recvWin, &gc.peerPpd);
    
  u_addrtosockaddr(&u_self_addr, 0, &self_addr);
  u_addrtosockaddr(&u_peer_addr, 0, &peer_addr);

    if (!PhysGetUpperHook(l, path, hook)) {
        Log(LG_PHYS, ("[%s] PPTP: can't get upper hook", l->name));
        return(-1);
    }
    
    /* Get a temporary netgraph socket node */
    if (NgMkSockNode(NULL, &csock, NULL) == -1) {
	Log(LG_ERR, ("PPTP: NgMkSockNode: %s", strerror(errno)));
	return(-1);
    }

  /* Attach PPTP/GRE node to PPP node */
  snprintf(mkp.type, sizeof(mkp.type), "%s", NG_PPTPGRE_NODE_TYPE);
  snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s", hook);
  snprintf(mkp.peerhook, sizeof(mkp.peerhook),
    "%s", NG_PPTPGRE_HOOK_UPPER);
  if (NgSendMsg(csock, path, NGM_GENERIC_COOKIE,
      NGM_MKPEER, &mkp, sizeof(mkp)) < 0) {
    Log(LG_ERR, ("[%s] PPTP: can't attach %s node: %s",
      l->name, NG_PPTPGRE_NODE_TYPE, strerror(errno)));
    close(csock);
    return(-1);
  }
  snprintf(pptppath, sizeof(pptppath), "%s.%s", path, hook);

    /* Get pptpgre node ID */
    if (NgSendMsg(csock, pptppath,
        NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) != -1) {
	    if (NgRecvMsg(csock, reply, sizeof(repbuf), NULL) != -1) {
	        pi->node_id = ninfo->id;
	    }
    }

  /* Attach ksocket node to PPTP/GRE node */
  snprintf(mkp.type, sizeof(mkp.type), "%s", NG_KSOCKET_NODE_TYPE);
  snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s", NG_PPTPGRE_HOOK_LOWER);
  if (u_self_addr.family==AF_INET6) {
    //ng_ksocket doesn't support inet6 name
    snprintf(mkp.peerhook, sizeof(mkp.peerhook), "%d/%d/%d", PF_INET6, SOCK_RAW, IPPROTO_GRE); 
  } else {
    snprintf(mkp.peerhook, sizeof(mkp.peerhook), "inet/raw/gre");
  }
  if (NgSendMsg(csock, pptppath, NGM_GENERIC_COOKIE,
      NGM_MKPEER, &mkp, sizeof(mkp)) < 0) {
    Log(LG_ERR, ("[%s] PPTP: can't attach %s node: %s",
      l->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
    close(csock);
    return(-1);
  }
  snprintf(ksockpath, sizeof(ksockpath),
    "%s.%s", pptppath, NG_PPTPGRE_HOOK_LOWER);

    /* increase recvspace to avoid packet loss due to very small GRE recv buffer. */
    ksso->level=SOL_SOCKET;
    ksso->name=SO_RCVBUF;
    ((int *)(ksso->value))[0]=48*1024;
    if (NgSendMsg(csock, ksockpath, NGM_KSOCKET_COOKIE,
	NGM_KSOCKET_SETOPT, &u, sizeof(u)) < 0) {
	    Log(LG_ERR, ("[%s] PPTP: can't setsockopt %s node: %s",
		l->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
    }

  /* Bind ksocket socket to local IP address */
  if (NgSendMsg(csock, ksockpath, NGM_KSOCKET_COOKIE,
      NGM_KSOCKET_BIND, &self_addr, self_addr.ss_len) < 0) {
    Log(LG_ERR, ("[%s] PPTP: can't bind() %s node: %s",
      l->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
    close(csock);
    return(-1);
  }

  /* Connect ksocket socket to remote IP address */
  if (NgSendMsg(csock, ksockpath, NGM_KSOCKET_COOKIE,
      NGM_KSOCKET_CONNECT, &peer_addr, peer_addr.ss_len) < 0
      && errno != EINPROGRESS) {	/* happens in -current (weird) */
    Log(LG_ERR, ("[%s] PPTP: can't connect() %s node: %s",
      l->name, NG_KSOCKET_NODE_TYPE, strerror(errno)));
    close(csock);
    return(-1);
  }

  /* Configure PPTP/GRE node */
  gc.enabled = 1;
  gc.enableDelayedAck = Enabled(&pi->conf.options, PPTP_CONF_DELAYED_ACK);
  gc.enableAlwaysAck = Enabled(&pi->conf.options, PPTP_CONF_ALWAYS_ACK);
#if NGM_PPTPGRE_COOKIE >= 1082548365
  gc.enableWindowing = Enabled(&pi->conf.options, PPTP_CONF_WINDOWING);
#endif

  if (NgSendMsg(csock, pptppath, NGM_PPTPGRE_COOKIE,
      NGM_PPTPGRE_SET_CONFIG, &gc, sizeof(gc)) < 0) {
    Log(LG_ERR, ("[%s] PPTP: can't config %s node: %s",
      l->name, NG_PPTPGRE_NODE_TYPE, strerror(errno)));
    close(csock);
    return(-1);
  }
  
  close(csock);

  /* Done */
  return(0);
}

/*
 * PptpIncoming()
 *
 * The control code calls this function to report that some
 * remote PPTP client has asked us if we will accept an incoming
 * call relayed over PPTP.
 */

static struct pptplinkinfo
PptpIncoming(struct pptpctrlinfo *cinfo,
	struct u_addr *self, struct u_addr *peer, in_port_t port, int bearType,
	const char *callingNum,
	const char *calledNum,
	const char *subAddress)
{
  return(PptpPeerCall(cinfo, self, peer, port, TRUE, callingNum, calledNum, subAddress));
}

/*
 * PptpOutgoing()
 *
 * The control code calls this function to report that some
 * remote PPTP client has asked us if we will dial out to some
 * phone number. We don't actually do this, but some clients
 * initiate their connections as outgoing calls for some reason.
 */

static struct pptplinkinfo
PptpOutgoing(struct pptpctrlinfo *cinfo,
	struct u_addr *self, struct u_addr *peer, in_port_t port, int bearType,
	int frameType, int minBps, int maxBps,
	const char *calledNum, const char *subAddress)
{
  return(PptpPeerCall(cinfo, self, peer, port, FALSE, "", calledNum, subAddress));
}

/*
 * PptpPeerCall()
 *
 * Peer has initiated a call (either incoming or outgoing; either
 * way it's the same to us). If we have an available link that may
 * accept calls from the peer's IP addresss and port, then say yes.
 */

static struct pptplinkinfo
PptpPeerCall(struct pptpctrlinfo *cinfo,
	struct u_addr *self, struct u_addr *peer, in_port_t port, int incoming,
	const char *callingNum,
	const char *calledNum,
	const char *subAddress)
{
  struct pptplinkinfo	linfo;
  Link			l = NULL;
  PptpInfo		pi = NULL;
  int			k;

  memset(&linfo, 0, sizeof(linfo));

  linfo.cookie = NULL;
  linfo.result = PptpResult;
  linfo.setLinkInfo = PptpSetLinkInfo;
  linfo.cancel = PptpCancel;

  if (gShutdownInProgress) {
    Log(LG_PHYS, ("Shutdown sequence in progress, ignoring request."));
    return(linfo);
  }

  if (OVERLOAD()) {
    Log(LG_PHYS, ("Daemon overloaded, ignoring request."));
    return(linfo);
  }

    /* Find a suitable link; prefer the link best matching peer's IP address */
    for (k = 0; k < gNumLinks; k++) {
	Link l2;
	PptpInfo pi2;

	if (!gLinks[k] || gLinks[k]->type != &gPptpPhysType)
		continue;

	l2 = gLinks[k];
	pi2 = (PptpInfo)l2->info;

	/* See if link is feasible */
	if ((!PhysIsBusy(l2)) &&
	    Enabled(&l2->conf.options, LINK_CONF_INCOMING) &&
	    (u_addrempty(&pi2->conf.self_addr) || (u_addrcompare(&pi2->conf.self_addr, self) == 0)) &&
	    IpAddrInRange(&pi2->conf.peer_addr, peer) &&
	    (!pi2->conf.peer_port || pi2->conf.peer_port == port)) {

    		/* Link is feasible; now see if it's preferable */
    		if (!pi || pi2->conf.peer_addr.width > pi->conf.peer_addr.width) {
			l = l2;
			pi = pi2;
			if (u_rangehost(&pi->conf.peer_addr)) {
				break;	/* Nothing could be better */
			}
    		}
	}
    }

    if (l != NULL && l->tmpl) {
        l = LinkInst(l, NULL, 0, 0);
        pi = (PptpInfo)l->info;
    }

    /* If no link is suitable, can't take the call */
    if (l == NULL) {
	Log(LG_PHYS, ("No free PPTP link with requested parameters "
	    "was found"));
	return(linfo);
    }

  Log(LG_PHYS, ("[%s] Accepting PPTP connection", l->name));
  PhysIncoming(l);

  /* Got one */
  linfo.cookie = l;
  l->state = PHYS_STATE_CONNECTING;
  pi->cinfo = *cinfo;
  pi->originate = FALSE;
  pi->outcall = !incoming;
  pi->sync = 1;
  pi->self_addr = *self;
  pi->peer_addr = *peer;
  pi->peer_port = port;
  strlcpy(pi->callingnum, callingNum, sizeof(pi->callingnum));
  strlcpy(pi->callednum, calledNum, sizeof(pi->callednum));
  return(linfo);
}

/*
 * PptpCancel()
 *
 * The control code calls this function to cancel a
 * local outgoing call in progress.
 */

static void
PptpCancel(void *cookie)
{
  PptpInfo	pi;
  Link 		l;

  /* It this fake call? */
  if (!cookie)
    return;

  l = (Link)cookie;
  pi = (PptpInfo) l->info;

  Log(LG_PHYS, ("[%s] PPTP call cancelled in state %s",
    l->name, gPhysStateNames[l->state]));
  if (l->state == PHYS_STATE_DOWN)
    return;
  l->state = PHYS_STATE_DOWN;
  u_addrclear(&pi->peer_addr);
  pi->peer_port = 0;
  pi->callingnum[0]=0;
  pi->callednum[0]=0;
  PhysDown(l, STR_CON_FAILED0, NULL);
}

/*
 * PptpListenUpdate()
 */

static void
PptpListenUpdate(Link l)
{
    PptpInfo	pi = (PptpInfo) l->info;

    if (pi->listener == NULL) {
	if (Enabled(&l->conf.options, LINK_CONF_INCOMING)) {
	    char	buf[64];

	    /* Set up listening for incoming connections */
	    if ((pi->listener = 
		PptpCtrlListen(&pi->conf.self_addr, pi->conf.self_port))
		    != NULL) {
		Log(LG_PHYS, ("PPTP: waiting for connection on %s",
		    u_addrtoa(&pi->conf.self_addr, buf, sizeof(buf))));
	    } else {
		Log(LG_ERR, ("PPTP: Error, can't listen for connection!"));
	    }
	}
    } else {
	if (!Enabled(&l->conf.options, LINK_CONF_INCOMING)) {
	    PptpCtrlUnListen(pi->listener);
	    pi->listener = NULL;
	}
    }
}

/*
 * PptpSetCommand()
 */

static int
PptpSetCommand(Context ctx, int ac, char *av[], void *arg)
{
	PptpInfo	const pi = (PptpInfo) ctx->lnk->info;
	struct u_range	rng;
	int		port;

  switch ((intptr_t)arg) {
    case SET_SELFADDR:
    case SET_PEERADDR:
      if (ac < 1 || ac > 2 || !ParseRange(av[0], &rng, ALLOW_IPV4|ALLOW_IPV6))
	return(-1);
      if (ac > 1) {
	if ((port = atoi(av[1])) < 0 || port > 0xffff)
	  return(-1);
      } else {
	port = 0;
      }
      if ((intptr_t)arg == SET_SELFADDR) {
	pi->conf.self_addr = rng.addr;
	pi->conf.self_port = port;
      } else {
	pi->conf.peer_addr = rng;
	pi->conf.peer_port = port;
      }
      break;
    case SET_CALLINGNUM:
      if (ac != 1)
	return(-1);
      snprintf(pi->conf.callingnum, sizeof(pi->conf.callingnum), "%s", av[0]);
      break;
    case SET_CALLEDNUM:
      if (ac != 1)
	return(-1);
      snprintf(pi->conf.callednum, sizeof(pi->conf.callednum), "%s", av[0]);
      break;
    case SET_ENABLE:
      EnableCommand(ac, av, &pi->conf.options, gConfList);
      PptpListenUpdate(ctx->lnk);
      break;
    case SET_DISABLE:
      DisableCommand(ac, av, &pi->conf.options, gConfList);
      PptpListenUpdate(ctx->lnk);
      break;
    default:
      assert(0);
  }
  return(0);
}
