
/*
 * radius.h
 *
 * Written by Michael Bretterklieber <mbretter@inode.at>
 * Written by Brendan Bank <brendan@gnarst.net>
 */

#include "ppp.h"
#include "auth.h"
#include "ccp_mppc.h"
#include <radlib.h>

#ifndef _RADIUS_H_
#define _RADIUS_H_

#define RADIUS_CHAP		1
#define RADIUS_PAP		2
#define RADIUS_MAX_SERVERS	10

#define RAD_NACK		0
#define RAD_ACK			1

/* for mppe-keys */
#define AUTH_LEN		16
#define SALT_LEN		2

#define MPPE_POLICY_NONE	0
#define MPPE_POLICY_ALLOWED	1
#define MPPE_POLICY_REQUIRED	2

#define MPPE_TYPE_0BIT		0	/* No encryption required */
#define MPPE_TYPE_40BIT		2
#define MPPE_TYPE_128BIT	4
#define MPPE_TYPE_56BIT		8

/*
 * FUNCTIONS
 */

extern int	RadiusPAPAuthenticate(const char *name, const char *password);
extern int	RadiusCHAPAuthenticate(const char *name, const char *password,
			int passlen, const char *challenge, int challenge_size,
			u_char chapid, int chap_type);
extern int	RadiusMSCHAPChangePassword(const char *mschapvalue, int mschapvaluelen, const char *challenge, 
			int challenge_size, u_char chapid, int chap_type);
extern int	RadiusStart(short request_type);
extern int	RadiusPutAuth(const char *name, const char *password,
			int passlen, const char *challenge, int challenge_size,
			u_char chapid, int auth_type);
extern int	RadiusPutChangePassword(const char *mschapvalue, int mschapvaluelen, u_char chapid, int chap_type); 
extern int	RadiusSendRequest(void);
extern int	RadiusGetParams(void);
extern int	RadiusAccount(short acct_type);
extern void	RadiusSetAuth(AuthData auth);
extern int	RadStat(int ac, char *av[], void *arg);
extern void	RadiusDestroy(void);
extern void	RadiusDown(void);

extern const	struct cmdtab RadiusSetCmds[];

  struct radiusserver_conf {
    char	*hostname;
    char	*sharedsecret;
    int		auth_port;
    int		acct_port;
    struct	radiusserver_conf *next;
  };
  typedef struct radiusserver_conf *RadServe_Conf;

  /* Configuration for a radius server */
  struct radiusconf {
    int		radius_timeout;
    int		radius_retries;
    char	file[PATH_MAX];
    struct radiusserver_conf *server;
  };
  typedef struct radiusconf *RadConf;

  struct radius {
    struct rad_handle	*radh;		/* RadLib Handle */
    short		valid;		/* Auth was successful */
    char		*reply_message;	/* Text wich may displayed to the user */
    char		authname[AUTH_MAX_AUTHNAME];
    char		session_id[256];	/* Session-Id needed for accounting */
    char		multi_session_id[256];	/* Multi-Session-Id needed for accounting */
    time_t		acct_start;	/* Accounting start-time */
    unsigned		vj:1;		/* FRAMED Compression */
    struct in_addr	ip;		/* FRAMED IP */
    struct in_addr	mask;	/* FRAMED Netmask */
    unsigned long	class;		/* Class */
    unsigned long	mtu;		/* FRAMED MTU */
    unsigned long	session_timeout;/* Session-Timeout */
    unsigned long	idle_timeout;	/* Idle-Timeout */
    unsigned long	protocol;	/* FRAMED Protocol */
    unsigned long	service_type;	/* Service Type */
    char		*filterid;	/* FRAMED Filter Id */
    char		*msdomain;	/* Microsoft domain */
    char		*mschap_error;	/* MSCHAP Error Message */    
    char		*mschapv2resp;	/* Response String for MSCHAPv2 */
    struct {
      int	policy;			/* MPPE_POLICY_* */
      int	types;			/* MPPE_TYPE_*BIT bitmask */
      u_char	recvkey[MPPE_KEY_LEN];	/* MS-CHAP v2 Keys */
      size_t	recvkeylen;
      u_char	sendkey[MPPE_KEY_LEN];
      size_t	sendkeylen;
      u_char	lm_key[8];		/* MS-CHAP v1 Keys 40 or 56 Bit */
      u_char	nt_hash[MPPE_KEY_LEN];	/* MS-CHAP v1 calculating 128 Bit Key */
      u_char	padding[8];		/* Padding to fit in 16 byte boundary */
    }			mppe;
    struct radiusconf	conf;
  };

  struct rad_chapvalue {
    u_char	ident;
    u_char	response[CHAP_MAX_VAL];
  };

  struct rad_mschapvalue {
    u_char	ident;
    u_char	flags;
    u_char	lm_response[24];
    u_char	nt_response[24];
  };

  struct rad_mschapv2value {
    u_char	ident;
    u_char	flags;
    u_char	pchallenge[16];
    u_char	reserved[8];
    u_char	response[24];
  };
  
  struct rad_mschapv2value_cpw {
    u_char	code;
    u_char	ident;
    u_char	encryptedHash[16];
    u_char	pchallenge[16];
    u_char	reserved[8];    
    u_char	nt_response[24];
    u_char	flags[2]; 
  };
  
  struct rad_mschap_new_nt_pw {
    u_char	ident;
    short	chunk;
    u_char	data[129];
  };
  
#endif