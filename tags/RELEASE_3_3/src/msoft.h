
/*
 * msoft.h
 *
 * Rewritten by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1998-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#ifndef _MSOFT_H_
#define _MSOFT_H_

#include <sys/types.h>

/*
 * FUNCTIONS
 */

  extern void	NTChallengeResponse(const u_char *chal,
		  const char *password, u_char *hash);

  extern void	NTPasswordHash(const char *password, u_char *hash);
  extern void	LMPasswordHash(const char *password, u_char *hash);

  extern void	MsoftGetKey(const u_char *h, u_char *h2, int len);
  extern void	MsoftGetStartKey(u_char *chal, u_char *h);

  extern void	GenerateNTResponse(const u_char *authchal,
		  const u_char *peerchal, const char *username,
		  const char *password, u_char *hash);
  extern void	GenerateAuthenticatorResponse(const char *password,
		  const u_char *nthash, const u_char *peerchal,
		  const u_char *authchal, const char *username,
		  char *hash);
  extern int	CheckAuthenticatorResponse(const char *password,
		  const u_char *nthash, const u_char *peerchal,
		  const u_char *authchal, const char *username,
		  const char *rechash);

  extern void	MsoftGetMasterKey(u_char *resp, u_char *h);
  extern void	MsoftGetAsymetricStartKey(u_char *h, int xmit);

#endif
