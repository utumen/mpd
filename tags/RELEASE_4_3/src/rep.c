
/*
 * rep.c
 *
 * Written by Alexander Motin <mav@FreeBSD.org>
 */

#include "ppp.h"
#include "rep.h"
#include "msg.h"
#include "ngfunc.h"
#include "log.h"
#include "util.h"

#include <netgraph/ng_message.h>
#ifdef __DragonFly__
#include <netgraph/socket/ng_socket.h>
#include <netgraph/tee/ng_tee.h>
#else
#include <netgraph/ng_socket.h>
#include <netgraph/ng_tee.h>
#endif
#include <netgraph.h>

/*
 * DEFINITIONS
 */

  /* Set menu options */
  enum {
    SET_ACCEPT,
    SET_DENY,
    SET_ENABLE,
    SET_DISABLE,
    SET_YES,
    SET_NO,
  };

/*
 * INTERNAL FUNCTIONS
 */

  static Rep	RepFind(char *name);
  static int	RepSetCommand(Context ctx, int ac, char *av[], void *arg);
  static void	RepShowLinks(Context ctx, Rep r);

/*
 * GLOBAL VARIABLES
 */

  struct discrim	self_discrim;

  const struct cmdtab RepSetCmds[] = {
    { "accept [opt ...]",		"Accept option",
	RepSetCommand, NULL, (void *) SET_ACCEPT },
    { "deny [opt ...]",			"Deny option",
	RepSetCommand, NULL, (void *) SET_DENY },
    { "enable [opt ...]",		"Enable option",
	RepSetCommand, NULL, (void *) SET_ENABLE },
    { "disable [opt ...]",		"Disable option",
	RepSetCommand, NULL, (void *) SET_DISABLE },
    { "yes [opt ...]",			"Enable and accept option",
	RepSetCommand, NULL, (void *) SET_YES },
    { "no [opt ...]",			"Disable and deny option",
	RepSetCommand, NULL, (void *) SET_NO },
    { NULL },
  };

/*
 * INTERNAL VARIABLES
 */

  static const struct confinfo	gConfList[] = {
    { 0,	0,			NULL		},
  };

/*
 * RepOpen()
 */

void
RepOpen(void)
{
//  MsgSend(rep->msgs, MSG_OPEN, NULL);
}

/*
 * RepClose()
 */

void
RepClose(void)
{
//  MsgSend(rep->msgs, MSG_CLOSE, NULL);
}

/*
 * RepIncoming()
 */

void
RepIncoming(PhysInfo p)
{
    Rep		r = p->rep;
    int		n = (r->physes[0] == p)?0:1;
    struct ngm_mkpeer       mkp;
    union {
        u_char buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
        struct ng_mesg reply;
    } repbuf;
    struct ng_mesg *const reply = &repbuf.reply;
    struct nodeinfo *ninfo = (struct nodeinfo *)&reply->data;
    char	buf[64];
    
    r->initiator = n;

    Log(LG_REP, ("[%s] REP: INCOMING event from %s (%d)",
	r->name, p->name, n));

    if (r->csock <= 0) {
	/* Create a new netgraph node to control TCP ksocket node. */
	if (NgMkSockNode(NULL, &r->csock, NULL) < 0) {
    	    Log(LG_ERR, ("[%s] REP: can't create control socket: %s",
    		r->name, strerror(errno)));
    	    PhysClose(p);
	    return;
	}
	(void)fcntl(r->csock, F_SETFD, 1);
    }

    snprintf(mkp.type, sizeof(mkp.type), "%s", NG_TEE_NODE_TYPE);
    snprintf(mkp.ourhook, sizeof(mkp.ourhook), "tee");
    snprintf(mkp.peerhook, sizeof(mkp.peerhook), NG_TEE_HOOK_LEFT2RIGHT);
    if (NgSendMsg(r->csock, ".:", NGM_GENERIC_COOKIE,
        NGM_MKPEER, &mkp, sizeof(mkp)) < 0) {
    	Log(LG_ERR, ("[%s] REP: can't attach %s %s node: %s",
    	    p->name, NG_TEE_NODE_TYPE, mkp.ourhook, strerror(errno)));
	close(r->csock);
    	PhysClose(p);
	return;
    }

    /* Get tee node ID */
    if (NgSendMsg(r->csock, ".:tee",
	NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) != -1) {
	    if (NgRecvMsg(r->csock, reply, sizeof(repbuf), NULL) != -1) {
	        r->node_id = ninfo->id;
	    }
    }
    
    PhysGetCallingNum(r->physes[n], buf, sizeof(buf));
    PhysSetCallingNum(r->physes[1-n], buf);

    PhysGetCalledNum(r->physes[n], buf, sizeof(buf));
    PhysSetCalledNum(r->physes[1-n], buf);

    PhysOpen(r->physes[1-n]);
}

/*
 * RepUp()
 */

void
RepUp(PhysInfo p)
{
    Rep r = p->rep;
    int n = (r->physes[0] == p)?0:1;
    
    Log(LG_REP, ("[%s] REP: UP event from %s (%d)",
	r->name, p->name, n));

    r->p_up |= (1 << n);
    
    if (n != r->initiator) {
	PhysOpen(r->physes[1-n]);
    }

    if (r->p_up == 3 && r->csock > 0 && r->node_id) {
	char path[NG_PATHLEN + 1];
	
	snprintf(path, sizeof(path), "[%x]:", r->node_id);
	NgFuncShutdownNode(r->csock, r->name, path);
	r->node_id = 0;
	close(r->csock);
	r->csock = -1;
    }
}

/*
 * RepDown()
 */

void
RepDown(PhysInfo p)
{
    Rep r = p->rep;
    int n = (r->physes[0] == p)?0:1;

    Log(LG_REP, ("[%s] REP: DOWN event from %s (%d)",
	r->name, p->name, n));

    r->p_up &= ~(1 << n);
    
    PhysClose(r->physes[0]);
    PhysClose(r->physes[1]);

    if (r->csock > 0 && r->node_id) {
	char path[NG_PATHLEN + 1];
	
	snprintf(path, sizeof(path), "[%x]:", r->node_id);
	NgFuncShutdownNode(r->csock, r->name, path);
	r->node_id = 0;
	close(r->csock);
	r->csock = -1;
    }
}

/*
 * RepIsSync()
 */

int
RepIsSync(PhysInfo p) {
    Rep r = p->rep;
    int n = (r->physes[0] == p)?0:1;
    
    return (PhysIsSync(r->physes[1-n]));
}

/*
 * RepSetAccm()
 */

void
RepSetAccm(PhysInfo p, u_int32_t xmit, u_int32_t recv) {
    Rep r = p->rep;
    int n = (r->physes[0] == p)?0:1;
    
    Log(LG_REP, ("[%s] REP: SetAccm(0x%08x, 0x%08x) from %s (%d)",
	r->name, xmit, recv, p->name, n));

    PhysSetAccm(r->physes[1-n], xmit, recv);
}

/*
 * RepGetHook()
 */

int
RepGetHook(PhysInfo p, char *path, char *hook)
{
    Rep r = p->rep;
    int n = (r->physes[0] == p)?0:1;

    if (r->node_id == 0)
	return (0);

    snprintf(path, NG_PATHLEN, "[%lx]:", (u_long)r->node_id);
    if (n == 0)
	snprintf(hook, NG_HOOKLEN, NG_TEE_HOOK_LEFT);
    else
	snprintf(hook, NG_HOOKLEN, NG_TEE_HOOK_RIGHT);
    return (1);
}

/*
 * RepCommand()
 *
 * Show list of all bundles or set bundle
 */

int
RepCommand(Context ctx, int ac, char *av[], void *arg)
{
  Rep	r;
  int	k;

  switch (ac) {
    case 0:

      Printf("Defined repeaters:\r\n");

      for (k = 0; k < gNumReps; k++)
	if ((r = gReps[k]) != NULL) {
	  Printf("\t%-15s", r->name);
	  RepShowLinks(ctx, r);
	}
      break;

    case 1:

      /* Change bundle, and link also if needed */
      if ((r = RepFind(av[0])) != NULL) {
	ctx->rep = r;
	ctx->phys = r->physes[0];
	ctx->bund = NULL;
	ctx->lnk = NULL;
      } else
	Printf("Repeater \"%s\" not defined.\r\n", av[0]);
      break;

    default:
      return(-1);
  }
  return(0);
}

/*
 * RepCreateCmd()
 *
 * Create a new repeater.
 */

int
RepCreateCmd(Context ctx, int ac, char *av[], void *arg)
{
    Rep		r;
    PhysInfo	new_link;
    int		k;

  /* Args */
  if (ac != 3)
    return(-1);

#if NG_NODESIZ>=32
  if (strlen(av[0])>16) {
#else
  if (strlen(av[0])>6) {
#endif
    Log(LG_ERR, ("repeater name \"%s\" is too long", av[0]));
    return (-1);
  }

  /* See if repeater name already taken */
  if ((r = RepFind(av[0])) != NULL) {
    Log(LG_ERR, ("repeater \"%s\" already exists", av[0]));
    return (-1);
  }

  /* Create a new repeater structure */
  r = Malloc(MB_REP, sizeof(*r));
  snprintf(r->name, sizeof(r->name), "%s", av[0]);
  r->csock = -1;

  /* Create each link and add it to the repeater */
  for (k = 1; k < ac; k++) {
#if NG_NODESIZ>=32
    if (strlen(av[k])>16) {
#else
    if (strlen(av[k])>6) {
#endif
	Log(LG_ERR, ("phys name \"%s\" is too long", av[k]));
        RepShutdown(r);
	return (-1);
    }
    if ((new_link = PhysInit(av[k], NULL, r)) == NULL) {
      Log(LG_ERR, ("[%s] creation of phys \"%s\" failed", av[0], av[k]));
      RepShutdown(r);
      return (-1);
    } else {
      r->physes[k - 1] = new_link;
    }
  }

  /* Add repeater to the list of repeaters and make it the current active repeater */
  for (k = 0; k < gNumReps && gReps[k] != NULL; k++);
  if (k == gNumReps)			/* add a new repeater pointer */
    LengthenArray(&gReps, sizeof(*gReps), &gNumReps, MB_REP);
  gReps[k] = r;
  
  ctx->rep = r;
  ctx->phys = r->physes[0];
  ctx->lnk = NULL;
  ctx->bund = NULL;

  /* Done */
  return(0);
}

/*
 * RepShutdown()
 *
 */
 
void
RepShutdown(Rep r)
{
    int		k;
    
    for(k = 0; k < 2; k++) {
	if (r->physes[k])
	    PhysShutdown(r->physes[k]);
    }

    for (k = 0; 
	k < gNumReps && gReps[k] != r;
	k++);
    if (k < gNumReps)
	gReps[k] = NULL;

    if (r->csock > 0 && r->node_id) {
	char path[NG_PATHLEN + 1];
	
	snprintf(path, sizeof(path), "[%x]:", r->node_id);
	NgFuncShutdownNode(r->csock, r->name, path);
	r->node_id = 0;
	close(r->csock);
	r->csock = -1;
    }
    
    Freee(MB_REP, r);
}

/*
 * RepStat()
 *
 * Show state of a repeater
 */

int
RepStat(Context ctx, int ac, char *av[], void *arg)
{
  Rep	r;

  /* Find repeater they're talking about */
  switch (ac) {
    case 0:
      r = ctx->rep;
      break;
    case 1:
      if ((r = RepFind(av[0])) == NULL) {
	Printf("Repeater \"%s\" not defined.\r\n", av[0]);
	return(0);
      }
      break;
    default:
      return(-1);
  }

  /* Show stuff about the repeater */
  Printf("Repeater %s:\r\n", r->name);
  Printf("\tPhyses          : ");
  RepShowLinks(ctx, r);

  return(0);
}

/*
 * RepShowLinks()
 */

static void
RepShowLinks(Context ctx, Rep r)
{
    int		j;

  for (j = 0; j < 2; j++) {
    Printf("%s", r->physes[j]->name);
    if (!r->physes[j]->type)
      Printf("[no type/%s] ",
      	gPhysStateNames[r->physes[j]->state]);
    else
      Printf("[%s/%s] ", r->physes[j]->type->name,
      	gPhysStateNames[r->physes[j]->state]);
  }
  Printf("\r\n");
}

/*
 * RepFind()
 *
 * Find a repeater structure
 */

static Rep
RepFind(char *name)
{
  int	k;

  for (k = 0;
    k < gNumReps && (strcmp(gReps[k]->name, name));
    k++);
  return((k < gNumReps) ? gReps[k] : NULL);
}

/*
 * RepSetCommand()
 */

static int
RepSetCommand(Context ctx, int ac, char *av[], void *arg)
{
    Rep		r = ctx->rep;

  if (ac == 0)
    return(-1);
  switch ((intptr_t)arg) {
    case SET_ACCEPT:
      AcceptCommand(ac, av, &r->options, gConfList);
      break;

    case SET_DENY:
      DenyCommand(ac, av, &r->options, gConfList);
      break;

    case SET_ENABLE:
      EnableCommand(ac, av, &r->options, gConfList);
      break;

    case SET_DISABLE:
      DisableCommand(ac, av, &r->options, gConfList);
      break;

    case SET_YES:
      YesCommand(ac, av, &r->options, gConfList);
      break;

    case SET_NO:
      NoCommand(ac, av, &r->options, gConfList);
      break;

    default:
      assert(0);
  }
  return(0);
}

