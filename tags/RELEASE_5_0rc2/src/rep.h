
/*
 * rep.h
 *
 * Written by Alexander Motin <mav@FreeBSD.org>
 */

#ifndef _REP_H_
#define _REP_H_

#include "defs.h"
#include "msg.h"
#include "command.h"
#include <netgraph/ng_message.h>

/*
 * DEFINITIONS
 */

  /* Total state of a repeater */
  struct rep {
    char		name[LINK_MAX_NAME];	/* Name of this repeater */
    int			id;			/* Index of this link in gReps */
    Link		links[2];		/* Links used by repeater */
    int			csock;			/* Socket node control socket */
    int			p_open;			/* Opened phys */
    int			p_up;			/* Up phys */
    ng_ID_t		node_id;		/* ng_tee node ID */
    int			refs;			/* Number of references */
    int			dead;			/* Dead flag */
  };
  
/*
 * VARIABLES
 */

  extern const struct cmdtab	RepSetCmds[];

/*
 * FUNCTIONS
 */

  extern int	RepStat(Context ctx, int ac, char *av[], void *arg);
  extern int	RepCommand(Context ctx, int ac, char *av[], void *arg);
  extern int	RepCreate(Link in, char *out);
  extern void	RepShutdown(Rep r);

  extern void	RepIncoming(Link l);
  extern int	RepIsSync(Link l); /* Is pair link is synchronous */
  extern void	RepSetAccm(Link l, u_int32_t xmit, u_int32_t recv); /* Set async accm */
  extern void	RepUp(Link l);
  extern void	RepDown(Link l);
  extern int	RepGetHook(Link l, char *path, char *hook);
  extern Rep	RepFind(char *name);

#endif

