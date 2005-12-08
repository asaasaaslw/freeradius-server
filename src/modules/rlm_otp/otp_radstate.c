/*
 * otp_radstate.c
 * $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2001,2002  Google, Inc.
 * Copyright 2005 TRI-D Systems, Inc.
 */

#ifdef FREERADIUS
#define _LRAD_MD4_H
#define _LRAD_SHA1_H
#endif
#include "otp.h"

#include <string.h>
#include <openssl/des.h> /* des_cblock */
#include <openssl/md5.h>
#include <openssl/hmac.h>


static const char rcsid[] = "$Id$";


/*
 * Generate the State attribute, suitable for passing to pairmake().
 * challenge must be a null terminated string, and be sized at least
 * as large as indicated in the function definition.
 *
 * Returns 0 on success, non-zero otherwise.  For successful returns,
 * ascii_state (suitable for passing to pairmake()) and raw_state, if
 * non-NULL, will be pointing to allocated storage.  The caller is
 * responsible for freeing the storage.  raw_state will not be
 * null-terminated, the caller should know the expected size (any
 * variance in size is solely due to the length of the challenge arg).
 *
 * In the simplest implementation, we would just use the challenge as state.
 * Unfortunately, the RADIUS secret protects only the User-Password
 * attribute; an attacker that can remove packets from the wire and insert
 * new ones can simply insert a replayed state without having to know
 * the secret.  If not for an attacker that can remove packets from the
 * network, I believe trivial state to be secure.
 *
 * So, we have to make up for that deficiency by signing our state with
 * data unique to this specific request.  A NAS would use the Request
 * Authenticator, but we don't know what that will be when the State is
 * returned to us, so we'll use the time.  So our replay prevention
 * is limited to a time interval (inst->chal_delay).  We could keep
 * track of all challenges issued over that time interval for
 * better protection.
 *
 * Our state, then, is
 *   (challenge + flags + time + hmac(challenge + resync + time, key)),
 * where '+' denotes concatentation, 'challenge' is ...
 * the challenge, 'flags' is a 32-bit value that can be used to record
 * additional info, 'time' is the 32-bit time (LSB if time_t is 64 bits)
 * in network byte order, and 'key' is a random key, generated in
 * otp_instantiate().  This means that only the server which generates a
 * challenge can verify it; this should be OK if your NAS's load balance
 * across RADIUS servers using a "first available" algorithm.  If your
 * NAS's round-robin (ugh), you could use the RADIUS secret instead, but
 * read RFC 2104 first, and make very sure you really want to do this.
 *
 * Note that putting the time in network byte order is pointless, since
 * only "this" server will be able to verify the hmac, due to the unique
 * key.  But I've left it in there for future consideration of sync'd
 * keys across servers (eg, using the RADIUS secret, which is probably
 * not a good idea; or reading from a file, which might be OK.)
 */
int
otp_gen_state(char **ascii_state, unsigned char **raw_state,
              const unsigned char challenge[OTP_MAX_CHALLENGE_LEN],
              size_t clen,
              int32_t flags, int32_t when, const unsigned char key[16])
{
  HMAC_CTX hmac_ctx;
  unsigned char hmac[MD5_DIGEST_LENGTH];
  char *p;

  /*
   * Generate the hmac.  We already have a dependency on openssl for
   * DES, so we'll use it's hmac functionality also -- saves us from
   * having to collect the data to be signed into one contiguous piece.
   */
  HMAC_Init(&hmac_ctx, key, sizeof(key), EVP_md5());
  HMAC_Update(&hmac_ctx, challenge, clen);
  HMAC_Update(&hmac_ctx, (unsigned char *) &flags, 4);
  HMAC_Update(&hmac_ctx, (unsigned char *) &when, 4);
  HMAC_Final(&hmac_ctx, hmac, NULL);
  HMAC_cleanup(&hmac_ctx);

  /* Fill in raw_state if requested. */
  if (raw_state) {
    *raw_state = rad_malloc(clen + 8 + sizeof(hmac));
    p = *raw_state;
    (void) memcpy(p, challenge, clen);
    p += clen;
    (void) memcpy(p, &flags, 4);
    p += 4;
    (void) memcpy(p, &when, 4);
    p += 4;
    (void) memcpy(p, hmac, sizeof(hmac));
  }

  /*
   * Fill in ascii_state if requested.  (pairmake() forces us to to this.)
   * "0x" is required for pairmake().  Note that each octet expands into
   * 2 hex digits in ASCII (0xAA -> 0x4141).
   */
  if (ascii_state) {
    *ascii_state = rad_malloc(2 +			/* "0x"      */
                              clen * 2 +		/* challenge */
                              8 +			/* flags     */
                              8 +			/* time      */
                              sizeof(hmac) * 2 +	/* hmac      */
                              1);			/* '\0'      */
    (void) sprintf(*ascii_state, "0x");
    p = *ascii_state + 2;

    /* Add the challenge. */
    (void) otp_keyblock2keystring(p, challenge, clen, otp_hex_conversion);
    p += clen * 2;

    /* Add the flags and time. */
    (void) otp_keyblock2keystring(p, (unsigned char *) &flags, 4,
                                  otp_hex_conversion);
    p += 8;
    (void) otp_keyblock2keystring(p, (unsigned char *) &when, 4,
                                  otp_hex_conversion);
    p += 8;

    /* Add the hmac. */
    (void) otp_keyblock2keystring(p, hmac, 16, otp_hex_conversion);
  }

  return 0;
}
