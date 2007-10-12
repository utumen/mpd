
/*
 * web.c
 *
 * Written by Alexander Motin <mav@FreeBSD.org>
 */

#include "ppp.h"
#include "web.h"
#include "util.h"


/*
 * DEFINITIONS
 */

  /* Set menu options */
  enum {
    SET_OPEN,
    SET_CLOSE,
    SET_USER,
    SET_SELF,
    SET_DISABLE,
    SET_ENABLE,
  };


/*
 * INTERNAL FUNCTIONS
 */

  static int	WebSetCommand(Context ctx, int ac, char *av[], void *arg);

  static int	WebServletRun(struct http_servlet *servlet,
                         struct http_request *req, struct http_response *resp);
  static void	WebServletDestroy(struct http_servlet *servlet);
  static const char*	WebAuth(void *arg,
                      struct http_request *req, const char *username,
		      const char *password);

  static int            WebUserHashEqual(struct ghash *g, const void *item1,
                              const void *item2);
  static u_int32_t      WebUserHash(struct ghash *g, const void *item);
				     
  static void	WebLogger(int sev, const char *fmt, ...);

/*
 * GLOBAL VARIABLES
 */

  const struct cmdtab WebSetCmds[] = {
    { "open",			"Open the web" ,
  	WebSetCommand, NULL, (void *) SET_OPEN },
    { "close",			"Close the web" ,
  	WebSetCommand, NULL, (void *) SET_CLOSE },
    { "user {name} {password}", "Add a web user" ,
      	WebSetCommand, NULL, (void *) SET_USER },
    { "self {ip} [{port}]",	"Set web ip and port" ,
  	WebSetCommand, NULL, (void *) SET_SELF },
    { "enable [opt ...]",	"Enable web option" ,
  	WebSetCommand, NULL, (void *) SET_ENABLE },
    { "disable [opt ...]",	"Disable web option" ,
  	WebSetCommand, NULL, (void *) SET_DISABLE },
    { NULL },
  };


/*
 * INTERNAL VARIABLES
 */

  static const struct confinfo	gConfList[] = {
    { 0,	WEB_AUTH,	"auth"	},
    { 0,	0,		NULL	},
  };

  static struct pevent_ctx *gWebCtx = NULL;
    
/*
 * WebInit()
 */

int
WebInit(Web w)
{
  /* setup web-defaults */
  memset(&gWeb, 0, sizeof(gWeb));

  Enable(&w->options, WEB_AUTH);  
  
  ParseAddr(DEFAULT_WEB_IP, &w->addr, ALLOW_IPV4|ALLOW_IPV6);
  w->port = DEFAULT_WEB_PORT;

  w->users = ghash_create(w, 0, 0, MB_WEB, WebUserHash, WebUserHashEqual, NULL, NULL);

  return 0;
}

/*
 * WebOpen()
 */

int
WebOpen(Web w)
{
  char		addrstr[INET6_ADDRSTRLEN];

  if (w->srv) {
    Log(LG_ERR, ("web: web already running"));
    return -1;
  }

  gWebCtx = pevent_ctx_create(MB_WEB, NULL);
  if (!gWebCtx) {
    Log(LG_ERR, ("%s: error pevent_ctx_create: %d", __FUNCTION__, errno));
    return(-1);
  }
  
  if (!(w->srv = http_server_start(gWebCtx, w->addr.u.ip4,
           w->port, NULL, "mpd web server", WebLogger))) {
    Log(LG_ERR, ("%s: error http_server_start: %d", __FUNCTION__, errno));
    return(-1);
  }

  if (Enabled(&w->options, WEB_AUTH)) {
    if (!(w->srvlet_auth = http_servlet_basicauth_create(WebAuth, w, NULL))) {
	Log(LG_ERR, ("%s: error http_servlet_basicauth_create: %d", __FUNCTION__, errno));
	return(-1);
    }

    if (http_server_register_servlet(w->srv, w->srvlet_auth, NULL, ".*", 5) < 0) {
	Log(LG_ERR, ("%s: error http_server_register_servlet: %d", __FUNCTION__, errno));
        return(-1);
    }
  }
  
  w->srvlet.arg=NULL;
  w->srvlet.hook=NULL;
  w->srvlet.run=WebServletRun;
  w->srvlet.destroy=WebServletDestroy;
	   
  if (http_server_register_servlet(w->srv, &w->srvlet, NULL, ".*", 10) < 0) {
    Log(LG_ERR, ("%s: error http_server_register_servlet: %d", __FUNCTION__, errno));
    return(-1);
  }
  
  Log(LG_ERR, ("web: listening on %s %d", 
	u_addrtoa(&w->addr,addrstr,sizeof(addrstr)), w->port));
  return 0;
}

/*
 * WebClose()
 */

int
WebClose(Web w)
{
  if (!w->srv) {
    Log(LG_ERR, ("web: web is not running"));
    return -1;
  }

  http_server_stop(&w->srv);
  if (gWebCtx) pevent_ctx_destroy(&gWebCtx);
  
  return 0;
}

/*
 * WebStat()
 */

int
WebStat(Context ctx, int ac, char *av[], void *arg)
{
  Web		w = &gWeb;
  WebUser	u;
  struct ghash_walk     walk;
  char		addrstr[64];

  Printf("Web configuration:\r\n");
  Printf("\tState         : %s\r\n", w->srv ? "OPENED" : "CLOSED");
  Printf("\tIP-Address    : %s\r\n", u_addrtoa(&w->addr,addrstr,sizeof(addrstr)));
  Printf("\tPort          : %d\r\n", w->port);

  Printf("Web options:\r\n");
  OptStat(ctx, &w->options, gConfList);

  Printf("Web configured users:\r\n");
  ghash_walk_init(w->users, &walk);
  while ((u = ghash_walk_next(w->users, &walk)) !=  NULL)
    Printf("\tUsername: %s\r\n", u->username);

  return 0;
}

/*
 * ConsoleSessionWriteV()
 */

static void 
WebConsoleSessionWriteV(ConsoleSession cs, const char *fmt, va_list vl)
{
  vfprintf((FILE *)(cs->cookie), fmt, vl);
}

/*
 * WebConsoleSessionWrite()
 */

static void 
WebConsoleSessionWrite(ConsoleSession cs, const char *fmt, ...)
{
  va_list vl;

  va_start(vl, fmt);
  WebConsoleSessionWriteV(cs, fmt, vl);
  va_end(vl);
}

static void
WebShowCSS(FILE *f)
{
  fprintf(f, "body {font : Arial, Helvetica, sans-serif; background-color: #EEEEEE; }\n");
  fprintf(f, "table {background-color: #FFFFFF; }\n");
  fprintf(f, "th {background-color: #00B000; }\n");
  fprintf(f, "td {background-color: #EEEEEE; }\n");
  fprintf(f, "td.r {background-color: #EECCCC; }\n");
  fprintf(f, "td.y {background-color: #EEEEBB; }\n");
  fprintf(f, "td.g {background-color: #BBEEBB; }\n");
  fprintf(f, "td.d {background-color: #CCCCCC; }\n");
  fprintf(f, "pre {background-color: #FFFFFF; }\n");
  fprintf(f, "a, a:visited, a:link { color: blue; }\n");
}

static void
WebShowSummary(FILE *f)
{
  int		b,l;
  Bund		B;
  Link  	L;
  Rep		R;
  char		buf[64],buf2[64];

  fprintf(f, "<H2>Current status summary</H2>\n");
  fprintf(f, "<table>\n");
  fprintf(f, "<TR><TH>Bund</TH><TH colspan=2>Iface</TH><TH>IPCP</TH><TH>IPV6CP</TH><TH>CCP</TH><TH>ECP</TH>"
	     "<TH>Link</TH><TH>LCP</TH><TH>User</TH><TH colspan=3>Device</TH><TH>Peer</TH><TH colspan=3></TH><TH></TH></TR>");
#define FSM_COLOR(s) (((s)==ST_OPENED)?"g":(((s)==ST_INITIAL)?"r":"y"))
#define PHYS_COLOR(s) (((s)==PHYS_STATE_UP)?"g":(((s)==PHYS_STATE_DOWN)?"r":"y"))
    for (b = 0; b<gNumLinks; b++) {
	if ((L=gLinks[b]) != NULL && L->bund == NULL && L->rep == NULL) {
	    fprintf(f, "<TR>\n");
	    fprintf(f, "<TD colspan=\"7\">&nbsp;</a></TD>\n");
	    fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;link\">%s</a></TD>\n", 
	        L->tmpl?"d":FSM_COLOR(L->lcp.fsm.state), L->name, L->name);
	    fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;lcp\">%s</a></TD>\n", 
	        L->tmpl?"d":FSM_COLOR(L->lcp.fsm.state), L->name, FsmStateName(L->lcp.fsm.state));
	    fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;auth\">%s</a></TD>\n", 
	        L->tmpl?"d":FSM_COLOR(L->lcp.fsm.state), L->name, L->lcp.auth.params.authname);
	    fprintf(f, "<TD class=\"L=%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
	        L->tmpl?"d":PHYS_COLOR(L->state), L->name, L->name);
	    fprintf(f, "<TD class=\"L=%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
	        L->tmpl?"d":PHYS_COLOR(L->state), L->name, L->type?L->type->name:"");
	    fprintf(f, "<TD class=\"L=%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
	        L->tmpl?"d":PHYS_COLOR(L->state), L->name, gPhysStateNames[L->state]);
	    if (L->state != PHYS_STATE_DOWN) {
		PhysGetPeerAddr(L, buf, sizeof(buf));
		fprintf(f, "<TD>%s</TD>\n", buf);
		PhysGetCallingNum(L, buf, sizeof(buf));
		PhysGetCalledNum(L, buf2, sizeof(buf2));
		if (PhysGetOriginate(L) == LINK_ORIGINATE_REMOTE) {
		    fprintf(f, "<TD>%s</TD><TD><=</TD><TD>%s</TD>\n", 
			buf2, buf);
		} else {
		    fprintf(f, "<TD>%s</TD><TD>=></TD><TD>%s</TD>\n", 
			buf, buf2);
		}
	    } else {
	    	fprintf(f, "<TD></TD>\n");
	    	fprintf(f, "<TD colspan=3></TD>\n");
	    }
	    fprintf(f, "<TD><A href=\"/cmd?L=%s&amp;open\">[Open]</a><A href=\"/cmd?L=%s&amp;close\">[Close]</a></TD>\n", 
	        L->name, L->name);
	    fprintf(f, "</TR>\n");
	}
    }
  for (b = 0; b<gNumBundles; b++) {
    if ((B=gBundles[b]) != NULL) {
	int rows = B->n_links?B->n_links:1;
	int first = 1;
	fprintf(f, "<TR>\n");
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?B=%s&amp;show&amp;bund\">%s</a></TD>\n", 
	    rows, B->tmpl?"d":(B->iface.up?"g":"r"), B->name, B->name);
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?B=%s&amp;show&amp;iface\">%s</a></TD>\n", 
	    rows, B->tmpl?"d":(B->iface.up?"g":"r"), B->name, B->iface.ifname);
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?B=%s&amp;show&amp;iface\">%s</a></TD>\n", 
	    rows, B->tmpl?"d":(B->iface.up?"g":"r"), B->name, (B->iface.up?"Up":"Down"));
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?B=%s&amp;show&amp;ipcp\">%s</a></TD>\n", 
	    rows, B->tmpl?"d":FSM_COLOR(B->ipcp.fsm.state), B->name,FsmStateName(B->ipcp.fsm.state));
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?B=%s&amp;show&amp;ipv6cp\">%s</a></TD>\n", 
	    rows, B->tmpl?"d":FSM_COLOR(B->ipv6cp.fsm.state), B->name,FsmStateName(B->ipv6cp.fsm.state));
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?B=%s&amp;show&amp;ccp\">%s</a></TD>\n", 
	    rows, B->tmpl?"d":FSM_COLOR(B->ccp.fsm.state), B->name,FsmStateName(B->ccp.fsm.state));
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?B=%s&amp;show&amp;ecp\">%s</a></TD>\n", 
	    rows, B->tmpl?"d":FSM_COLOR(B->ecp.fsm.state), B->name,FsmStateName(B->ecp.fsm.state));
	if (B->n_links == 0) {
	    fprintf(f, "<TD colspan=\"11\">&nbsp;</a></TD>\n</TR>\n");
	}
	for (l = 0; l < NG_PPP_MAX_LINKS; l++) {
	    if ((L=B->links[l]) != NULL) {
		if (first)
		    first = 0;
		else
		    fprintf(f, "<TR>\n");
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;link\">%s</a></TD>\n", 
		    L->tmpl?"d":FSM_COLOR(L->lcp.fsm.state), L->name, L->name);
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;lcp\">%s</a></TD>\n", 
		    L->tmpl?"d":FSM_COLOR(L->lcp.fsm.state), L->name, FsmStateName(L->lcp.fsm.state));
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;auth\">%s</a></TD>\n", 
		    L->tmpl?"d":FSM_COLOR(L->lcp.fsm.state), L->name, L->lcp.auth.params.authname);
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
		    L->tmpl?"d":PHYS_COLOR(L->state), L->name, L->name);
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
		    L->tmpl?"d":PHYS_COLOR(L->state), L->name, L->type?L->type->name:"");
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
		    L->tmpl?"d":PHYS_COLOR(L->state), L->name, gPhysStateNames[L->state]);
		if (L->state != PHYS_STATE_DOWN) {
		    PhysGetPeerAddr(L, buf, sizeof(buf));
		    fprintf(f, "<TD>%s</TD>\n", buf);
		    PhysGetCallingNum(L, buf, sizeof(buf));
		    PhysGetCalledNum(L, buf2, sizeof(buf2));
		    if (PhysGetOriginate(L) == LINK_ORIGINATE_REMOTE) {
			    fprintf(f, "<TD>%s</TD><TD><=</TD><TD>%s</TD>\n", 
				buf2, buf);
		    } else {
			    fprintf(f, "<TD>%s</TD><TD>=></TD><TD>%s</TD>\n", 
				buf, buf2);
		    }
		} else {
			fprintf(f, "<TD></TD>\n");
			fprintf(f, "<TD colspan=3></TD>\n");
		}
		fprintf(f, "<TD><A href=\"/cmd?L=%s&amp;open\">[Open]</a><A href=\"/cmd?L=%s&amp;close\">[Close]</a></TD>\n", 
		    L->name, L->name);
		fprintf(f, "</TR>\n");
	    }
	}
    }
  }
  for (b = 0; b<gNumReps; b++) {
    if ((R=gReps[b]) != NULL) {
	int shown = 0;
#define FSM_COLOR(s) (((s)==ST_OPENED)?"g":(((s)==ST_INITIAL)?"r":"y"))
#define PHYS_COLOR(s) (((s)==PHYS_STATE_UP)?"g":(((s)==PHYS_STATE_DOWN)?"r":"y"))
	int rows = (R->links[0]?1:0) + (R->links[1]?1:0);
	if (rows == 0)
	    rows = 1;
	fprintf(f, "<TR>\n");
	fprintf(f, "<TD rowspan=\"%d\" colspan=6>Repeater</TD>\n", rows);
	fprintf(f, "<TD rowspan=\"%d\" class=\"%s\"><A href=\"/cmd?R=%s&amp;show&amp;repeater\">%s</a></TD>\n", 
	     rows, R->p_up?"g":"r", R->name, R->name);
	for (l = 0; l < 2; l++) {
	    if ((L=R->links[l]) != NULL) {
		if (shown)
		    fprintf(f, "<TR>\n");
		fprintf(f, "<TD colspan=3></TD>\n");
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
		    PHYS_COLOR(L->state), L->name, L->name);
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
		    PHYS_COLOR(L->state), L->name, L->type?L->type->name:"");
		fprintf(f, "<TD class=\"%s\"><A href=\"/cmd?L=%s&amp;show&amp;device\">%s</a></TD>\n", 
		    PHYS_COLOR(L->state), L->name, gPhysStateNames[L->state]);
		if (L->state != PHYS_STATE_DOWN) {
		    PhysGetPeerAddr(L, buf, sizeof(buf));
		    fprintf(f, "<TD>%s</TD>\n", buf);
		    PhysGetCallingNum(L, buf, sizeof(buf));
		    PhysGetCalledNum(L, buf2, sizeof(buf2));
		    if (PhysGetOriginate(L) == LINK_ORIGINATE_REMOTE) {
			    fprintf(f, "<TD>%s</TD><TD><=</TD><TD>%s</TD>\n", 
				buf2, buf);
		    } else {
			    fprintf(f, "<TD>%s</TD><TD>=></TD><TD>%s</TD>\n", 
				buf, buf2);
		    }
		} else {
			fprintf(f, "<TD></TD>\n");
			fprintf(f, "<TD colspan=3></TD>\n");
		}
		fprintf(f, "<TD></TD>\n");
		fprintf(f, "</TR>\n");
		
		shown = 1;
	    }
	}
	if (!shown) {
	    fprintf(f, "<TD colspan = \"11\"></TD>\n");
	    fprintf(f, "</TR>\n");
	}
    }
  }
  fprintf(f, "</TABLE>\n");
}

static void
WebRunCmdCleanup(void *cookie) {
}

static void 
WebRunCmd(FILE *f, const char *querry)
{
  Console		c = &gConsole;
  struct console_session css;
  ConsoleSession	cs = &css;
  char			buf[1024];
  char			buf1[1024];
  char			*tmp;
  int			argc;
  char			*argv[MAX_CONSOLE_ARGS];
  char			*av[MAX_CONSOLE_ARGS];
  int			k;
  
  memset(cs, 0, sizeof(*cs));

  cs->cookie = f;
  cs->console = c;
  cs->close = NULL;
  cs->write = WebConsoleSessionWrite;
  cs->writev = WebConsoleSessionWriteV;
  cs->prompt = NULL;
  cs->context.cs = cs;

  strlcpy(buf,querry,sizeof(buf));
  tmp = buf;
  
  for (argc = 0; (argv[argc] = strsep(&tmp, "&")) != NULL;)
      if (argv[argc][0] != '\0')
         if (++argc >= MAX_CONSOLE_ARGS)
            break;

  if (argc > 0) {
    fprintf(f, "<H2>Command '");
    for (k = 1; k < argc; k++) {
	fprintf(f, "%s ",argv[k]);
    }
    if (strncmp(argv[0], "R=", 2) == 0)
        strcpy(buf1, "repeater");
    else if (strncmp(argv[0], "B=", 2) == 0)
        strcpy(buf1, "bundle");
    else
        strcpy(buf1, "link");
    fprintf(f, "' for %s '%s'</H2>\n", buf1, argv[0]+2);

    if ((!strcmp(argv[1], "show")) ||
	(!strcmp(argv[1], "open")) ||
	(!strcmp(argv[1], "close"))) {

	fprintf(f, "<P><A href=\"/\"><< Back</A></P>\n");
    
	fprintf(f, "<PRE>\n");

	pthread_cleanup_push(WebRunCmdCleanup, NULL);

        av[0] = buf1;
        av[1] = argv[0]+2;
        DoCommand(&cs->context, 2, av, NULL, 0);
  
        for (k = 1; k < argc; k++) {
    	    av[k-1] = argv[k];
        }
        DoCommand(&cs->context, argc-1, av, NULL, 0);

	pthread_cleanup_pop(0);

	fprintf(f, "</PRE>\n");
    } else {
	fprintf(f, "<P>Command denied!</P>\n");
    }
  } else {
    fprintf(f, "<P>No command cpecified!</P>\n");
  }
  fprintf(f, "<P><A href=\"/\"><< Back</A></P>\n");
}

static void
WebServletRunCleanup(void *cookie) {
    GIANT_MUTEX_UNLOCK();
}

static int	
WebServletRun(struct http_servlet *servlet,
                         struct http_request *req, struct http_response *resp)
{
    FILE *f;
    const char *path;
    const char *querry;

    if (!(f = http_response_get_output(resp, 0))) {
	return 0;
    }
    if (!(path = http_request_get_path(req)))
	return 0;
    if (!(querry = http_request_get_query_string(req)))
	return 0;

    if (!strcmp(path,"/mpd.css")) {
	http_response_set_header(resp, 0, "Content-Type", "text/css");
	WebShowCSS(f);
    } else {
	http_response_set_header(resp, 0, "Content-Type", "text/html");
	http_response_set_header(resp, 1, "Pragma", "no-cache");
	http_response_set_header(resp, 1, "Cache-Control", "no-cache, must-revalidate");
	
	pthread_cleanup_push(WebServletRunCleanup, NULL);
	GIANT_MUTEX_LOCK();
	fprintf(f, "<!DOCTYPE HTML "
	    "PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
	    "\"http://www.w3.org/TR/html4/strict.dtd\">\n");
	fprintf(f, "<HTML>\n");
	fprintf(f, "<HEAD><TITLE>Multi-link PPP Daemon for FreeBSD (mpd)</TITLE>\n");
	fprintf(f, "<LINK rel='stylesheet' href='/mpd.css' type='text/css'>\n");
	fprintf(f, "</HEAD>\n<BODY>\n");
	fprintf(f, "<H1>Multi-link PPP Daemon for FreeBSD</H1>\n");
    
	if (!strcmp(path,"/"))
	    WebShowSummary(f);
	else if (!strcmp(path,"/cmd"))
	    WebRunCmd(f, querry);
	    
	GIANT_MUTEX_UNLOCK();
	pthread_cleanup_pop(0);
	
	fprintf(f, "</BODY>\n</HTML>\n");
    }
    return 1;
};

static void	
WebServletDestroy(struct http_servlet *servlet)
{
};

static const char*	
WebAuth(void *arg, struct http_request *req, const char *username,
		      const char *password) 
{
    Web		w = (Web)arg;
    WebUser	u;
    struct web_user	iu;

    strlcpy(iu.username, username, sizeof(iu.username));
    u = ghash_get(w->users, &iu);

    if ((u == NULL) || strcmp(u->password, password)) 
      return "Access Denied";

    return NULL;    
}

/*
 * WebUserHash
 *
 * Fowler/Noll/Vo- hash
 * see http://www.isthe.com/chongo/tech/comp/fnv/index.html
 *
 * By:
 *  chongo <Landon Curt Noll> /\oo/\
 *  http://www.isthe.com/chongo/
 */

static u_int32_t
WebUserHash(struct ghash *g, const void *item)
{
  WebUser u = (WebUser) item;
  u_char *s = (u_char *) u->username;
  u_int32_t hash = 0x811c9dc5;

  while (*s) {
    hash += (hash<<1) + (hash<<4) + (hash<<7) + (hash<<8) + (hash<<24);
    /* xor the bottom with the current octet */
    hash ^= (u_int32_t)*s++;
  }

  return hash;
}

/*
 * WebUserHashEqual
 */

static int
WebUserHashEqual(struct ghash *g, const void *item1, const void *item2)
{
  WebUser u1 = (WebUser) item1;
  WebUser u2 = (WebUser) item2;

  if (u1 && u2)
    return (strcmp(u1->username, u2->username) == 0);
  else
    return 0;
}

static void
WebLogger(int sev, const char *fmt, ...)
{
  va_list       args;

  va_start(args, fmt);
  vLogPrintf(fmt, args);
  va_end(args);
}

/*
 * WebSetCommand()
 */

static int
WebSetCommand(Context ctx, int ac, char *av[], void *arg) 
{
  Web	 		w = &gWeb;
  WebUser		u;
  int			port;

  switch ((intptr_t)arg) {

    case SET_OPEN:
      WebOpen(w);
      break;

    case SET_CLOSE:
      WebClose(w);
      break;

    case SET_ENABLE:
	EnableCommand(ac, av, &w->options, gConfList);
      break;

    case SET_DISABLE:
	DisableCommand(ac, av, &w->options, gConfList);
      break;

    case SET_USER:
      if (ac != 2) 
	return(-1);

      u = Malloc(MB_WEB, sizeof(*u));
      strlcpy(u->username, av[0], sizeof(u->username));
      strlcpy(u->password, av[1], sizeof(u->password));
      ghash_put(w->users, u);
      break;

    case SET_SELF:
      if (ac < 1 || ac > 2)
	return(-1);

      if (!ParseAddr(av[0],&w->addr, ALLOW_IPV4)) 
      {
	Log(LG_ERR, ("web: Bogus IP address given %s", av[0]));
	return(-1);
      }

      if (ac == 2) {
        port =  strtol(av[1], NULL, 10);
        if (port < 1 || port > 65535) {
	    Log(LG_ERR, ("web: Bogus port given %s", av[1]));
	    return(-1);
        }
        w->port=port;
      }
      break;

    default:
      return(-1);

  }

  return 0;
}