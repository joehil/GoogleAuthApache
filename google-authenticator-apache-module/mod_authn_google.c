/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Copyright 2011 Bradley K. Goodman
 * brad at bradgoodman dot com
 *
 * Portions Copyright 2010 Google Inc.
 * By Markus Gutschke
 *
 * This source code has been fixed for compiling error and other bugs by
 * Nicola Asuni - Fubra.com - 2011-12-07
 * This source code has been modified to be able to use an additional static password
 * joehil                   - 2017-06-30
 */

#include "apr_strings.h"
#include "apr_md5.h"            /* for apr_password_validate */

#include "ap_config.h"
#include "ap_provider.h"
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "apr_sha1.h"

#include "mod_auth.h"
#include "base32.h"
#include "hmac.h"
#include "sha1.h"

#include "apu.h"
#include "apr_general.h"
#include "apr_base64.h"

//#define DEBUG

ap_regex_t *cookie_regexp;

typedef struct {
	char *pwfile;
	int cookieLife;
	int entryWindow;
} authn_google_config_rec;


static unsigned int get_timestamp() {
	apr_time_t apr_time = apr_time_now();
	apr_time /= 1000000;
	apr_time /= 30;
//	int unix_time =  time(0L)/30;
//	printf ("APR time is %lu unix time is %d\n",apr_time,unix_time);
	return (apr_time);
}

static uint8_t *get_shared_secret( request_rec *r, const char *buf, int *secretLen) {
  // Decode secret key
  int base32Len = strlen(buf);
  *secretLen = (base32Len*5 + 7)/8;
  unsigned char *secret = apr_palloc(r->pool,base32Len + 1);
  memcpy(secret, buf, base32Len);
  secret[base32Len] = '\000';
  if ((*secretLen = base32_decode(secret, secret, base32Len)) < 1) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                "Could not find a valid BASE32 encoded secret");
    memset(secret, 0, base32Len);
    return NULL;
  }
  memset(secret + *secretLen, 0, base32Len + 1 - *secretLen);
  return secret;
}

static void *create_authn_google_dir_config(apr_pool_t *p, char *d) {
    authn_google_config_rec *conf = apr_palloc(p, sizeof(*conf));
    conf->pwfile = NULL;     /* just to illustrate the default really */
	conf->cookieLife=0;
	conf->entryWindow=0;
    return conf;
}

static const char *set_authn_google_slot(cmd_parms *cmd, void *offset, const char *f, const char *t) {
    if (t && strcmp(t, "standard")) {
        return apr_pstrcat(cmd->pool, "Invalid auth file type: ", t, NULL);
    }
    return ap_set_file_slot(cmd, offset, f);
}

static const char *set_authn_set_int(cmd_parms *cmd, void *offset, const char *f ) {
    return ap_set_int_slot(cmd, offset, f);
}

static const command_rec authn_google_cmds[] = {
    AP_INIT_TAKE12("GoogleAuthUserPath", set_authn_google_slot,
                   (void *)APR_OFFSETOF(authn_google_config_rec, pwfile),
                   OR_AUTHCFG, "Directory containing Google Authenticator credential files"),
    AP_INIT_TAKE1("GoogleAuthCookieLife", set_authn_set_int,
                   (void *)APR_OFFSETOF(authn_google_config_rec, cookieLife),
                   OR_AUTHCFG, "Enable authentication cookies with lifespan given in seconds"),
    AP_INIT_TAKE1("GoogleAuthEntryWindow", set_authn_set_int,
                   (void *)APR_OFFSETOF(authn_google_config_rec, entryWindow),
                   OR_AUTHCFG, "Difference in seconds for timing syncronization"),
    {NULL}
};

module AP_MODULE_DECLARE_DATA authn_google_module;

static char * hash_cookie(apr_pool_t *p, uint8_t *secret,int secretLen,unsigned long expires) {
	unsigned char hash[SHA1_DIGEST_LENGTH];
	hmac_sha1(secret, secretLen, (uint8_t *) &expires, sizeof(unsigned int), hash, SHA1_DIGEST_LENGTH);
	int len;
	char *encoded = apr_palloc(p,(SHA1_DIGEST_LENGTH*2)+3);
	len = apr_base64_encode_binary(encoded,hash,SHA1_DIGEST_LENGTH);
	return encoded;
}

static char *getSharedKey(request_rec *r,char *filename) {
    char l[MAX_STRING_LEN];
	char *sharedKey = 0L;
	apr_status_t status;
    ap_configfile_t *f;

    status = ap_pcfg_openfile(&f, r->pool, filename);

    if (status != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r,
                      "check_password: Could not open password file: %s", filename);
        return 0L;
    }

    while (!(ap_cfg_getline(l, MAX_STRING_LEN, f))) {
        /* Skip comment or blank lines. */
        if ((l[0] == '"') || (!l[0])) {
            continue;
        }
		if (!sharedKey) {
			sharedKey = apr_pstrdup(r->pool,l);
		}
		/* Scratch codes to follow */
    }
    ap_cfg_closefile(f);
	return sharedKey;
}

static char *getStaticPW(request_rec *r,char *filename) {
    char l[MAX_STRING_LEN];
	char *sharedKey = 0L;
	apr_status_t status;
    ap_configfile_t *f;

    status = ap_pcfg_openfile(&f, r->pool, filename);

    if (status != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r,
                      "check_password: Could not open password file: %s", filename);
        return 0L;
    }

    while (!(ap_cfg_getline(l, MAX_STRING_LEN, f))) {
        if ((l[0] == '"') && (l[1]==' ') && (l[2]=='P') && (l[3]=='A') && (l[4]=='S') && (l[5]=='S')
	&& (l[6]=='W') && (l[7]=='O') && (l[8]=='R') && (l[9]=='D') && (l[10]=='=')) {
            sharedKey = apr_pstrdup(r->pool,&l[11]);
        }
    }
    ap_cfg_closefile(f);
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_ERR, status, r,
                      "StaticPW: %s", sharedKey);
#endif
	return sharedKey;
}

/**
  * \brief getUserSecret Based on the given username, get the users secret key 
  * \param r Request
  * \param username Username
  * \param secret Secret key returned here. Must be allocated by caller
  * \return Pointer to secret key data on success, NULL on error
 **/
static uint8_t *getUserSecret(request_rec *r, const char *username, int *secretLen) {
	authn_google_config_rec *conf = ap_get_module_config(r->per_dir_config, &authn_google_module);
	char *ga_filename = apr_psprintf(r->pool,"%s/%s",conf->pwfile,username);
	char *sharedKey;
	sharedKey = getSharedKey(r,ga_filename);
	if (!sharedKey) {
		return 0L;
	}
	uint8_t *secret = get_shared_secret(r,sharedKey,secretLen);
	return secret;
}

static uint8_t *getUserPW(request_rec *r, const char *username) {
	authn_google_config_rec *conf = ap_get_module_config(r->per_dir_config, &authn_google_module);
	char *ga_filename = apr_psprintf(r->pool,"%s/%s",conf->pwfile,username);
	char *sharedKey;
	sharedKey = getStaticPW(r,ga_filename);
	if (!sharedKey) {
		return 0L;
	}
	return sharedKey;
}


/**
  * \brief find_cookie
  * \param r
  * \param user If not null, will return username here
  * \param secret Secret key to hash
  * \param secretLen
  * \return  Zero if not found or invalid, 1 if found valid cookie
	* If secret key is NULL, function will extract username, and look
	* up sercret key based on this username.
 **/

static int find_cookie(request_rec *r, const char **user, uint8_t *secret, int secretLen) {
	char *cookie=0L;
	char *cookie_expire=0L;
	char *cookie_valid=0L;
	ap_regmatch_t regmatch[AP_MAX_REG_MATCH];
	//authn_google_config_rec *conf = ap_get_module_config(r->per_dir_config, &authn_google_module);
	cookie = (char *) apr_table_get(r->headers_in, "Cookie");
	if (cookie) {
		if (!ap_regexec(cookie_regexp, cookie, AP_MAX_REG_MATCH, regmatch, 0)) {
			if (user) *user  = ap_pregsub(r->pool, "$1", cookie,AP_MAX_REG_MATCH,regmatch);
			cookie_expire = ap_pregsub(r->pool, "$2", cookie,AP_MAX_REG_MATCH,regmatch);
			cookie_valid = ap_pregsub(r->pool, "$3", cookie,AP_MAX_REG_MATCH,regmatch);
			if ((!cookie_valid) || (!cookie_expire)) {
				if (user) *user  = ap_pregsub(r->pool, "$4", cookie,AP_MAX_REG_MATCH,regmatch);
				cookie_expire = ap_pregsub(r->pool, "$5", cookie,AP_MAX_REG_MATCH,regmatch);
				cookie_valid = ap_pregsub(r->pool, "$6", cookie,AP_MAX_REG_MATCH,regmatch);
			}
				
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Found cookie Expires \"%s\" Valid \"%s\"",cookie_expire,cookie_valid);
#endif
				if (cookie_expire && cookie_valid && *user) {
					long unsigned int exp = apr_atoi64(cookie_expire);
					long unsigned int now = apr_time_now()/1000000;
					if (exp < now) {
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Expired. Now=%lu Expire=%lu\n",now,exp);
#endif
						return 0;	/* Expired */
					}
					if (!secret) {
						secret = getUserSecret(r,*user,&secretLen);
					}
					char *h = hash_cookie(r->pool,secret,secretLen,exp);
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "Match cookie \"%s\" vs  \"%s\"",h,cookie_valid);
#endif
					if (apr_strnatcmp(h,cookie_valid)==0)
						return 1; /* Valid Cookie */
					else {
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "MISMATCHED  cookie \"%s\" vs  \"%s\"",h,cookie_valid);
#endif
						return 0; /* Mismatched */
					}
				}
		}
	}
	return 0;	/* Not found */
}

static unsigned int computeTimeCode(unsigned int tm, unsigned char *secret, int secretLen) {
	unsigned char hash[SHA1_DIGEST_LENGTH];
	unsigned long chlg = tm ;
	unsigned char challenge[8];
	unsigned int truncatedHash = 0;
	int j;
	for (j = 8; j--; chlg >>= 8) {
		challenge[j] = chlg;
	}
	hmac_sha1(secret, secretLen, challenge, 8, hash, SHA1_DIGEST_LENGTH);
	int offset = hash[SHA1_DIGEST_LENGTH - 1] & 0xF;
	for (j = 0; j < 4; ++j) {
		truncatedHash <<= 8;
		truncatedHash  |= hash[offset + j];
	}
	memset(hash, 0, sizeof(hash));
	truncatedHash &= 0x7FFFFFFF;
	truncatedHash %= 1000000;
	return truncatedHash;
}


/* Create and add an authentication cookie to the request,
	 if we have been configured to do so. This will allow 
	 subsequent requests to work without having to re-authenticate */

static void addCookie(request_rec *r, uint8_t *secret, int secretLen) {
    authn_google_config_rec *conf = ap_get_module_config(r->per_dir_config, &authn_google_module);
	if (conf->cookieLife) {
		unsigned long exp = (apr_time_now() / (1000000) ) + conf->cookieLife;
		char *h = hash_cookie(r->pool,secret,secretLen,exp);
		char * cookie = apr_psprintf(r->pool,"google_authn=%s:%lu:%s",r->user,exp,h);
#ifdef DEBUG
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Created cookie expires %lu (time = %d) hash is %s Cookie: %s", exp, conf->cookieLife, h, cookie);
#endif
		apr_table_addn(r->headers_out, "Set-Cookie", cookie);
	}
}

/* Mark a file with the last used time  - do disallow reuse */
//static void markLastUsed(request_rec *r,char *user) {}

static authn_status ga_check_password(request_rec *r, const char *user, const char *password) {
    authn_google_config_rec *conf = ap_get_module_config(r->per_dir_config, &authn_google_module);
    //apr_status_t status;
	//char *ga_filename;
	char *sharedKey=0L;
	char *userPW=0L;
	int tm;
	int pwLen;
	int i;//,j;
	unsigned int truncatedHash = 0;
	
#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "**** PW AUTH at  T=%lu  user  \"%s\"",apr_time_now()/1000000,user);
#endif

	int secretLen;
	userPW = getUserPW(r,user);
	if (userPW)
		pwLen = strlen(userPW);
	else
		pwLen=0L;
#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "userPW length: %d",pwLen);
#endif
	sharedKey = getUserSecret(r,user,&secretLen);
	uint8_t *secret = sharedKey;
	if (!secret) {
		return AUTH_DENIED;
	}
	if (strncmp(userPW, password, pwLen)!=0) {
		return AUTH_DENIED;
	}
	else {
		password+=pwLen;
	}


	/***
	 *** Perform Google Authentication
	 ***/
	tm  = get_timestamp();

#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Secret Key is \"%s\" @ T=%d",sharedKey,tm);
#endif

	int code = (int) apr_atoi64(password);
	for (i = -(conf->entryWindow); i <= (conf->entryWindow); ++i) {
	truncatedHash = computeTimeCode(tm+i,secret,secretLen);
	
#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Checking codes  @ T=%d \"%d\" vs.  \"%d\"",tm,truncatedHash,code);
#endif

			if (truncatedHash == (unsigned int)code) {
				/**\todo  - check to see if time-based code has been invalidated */
				addCookie(r,secret,secretLen);
				return AUTH_GRANTED;
			}
		}

#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Validating for  \"%s\" Shared Key  \"%s\"",password,sharedKey);
#endif

    return AUTH_DENIED;
}

static char *hex_encode(apr_pool_t *p, uint8_t *data,int len) {
	const char *hex = "0123456789abcdef";
	char *result = apr_palloc(p,(APR_MD5_DIGESTSIZE*2)+1);
	int idx;
	char *h = result;
	for (idx=0; idx<APR_MD5_DIGESTSIZE; idx++) {
		*h++ = hex[data[idx] >> 4];
		*h++ = hex[data[idx] & 0xF];
	}
	*h=(char) 0;
	return result;
	}


/* This handles Digest Authentication. Returns a has of the 
   User, Realm and (Required) Password. Caller (Digest module)
	 determines if the entered password was actually valid
*/
static authn_status ga_get_realm_hash(request_rec *r, const char *user, const char *realm, char **rethash) {
    authn_google_config_rec *conf = ap_get_module_config(r->per_dir_config, &authn_google_module);
    //ap_configfile_t *f;
    //char l[MAX_STRING_LEN];
    //apr_status_t status;
    //char *file_hash = NULL;
	char *sharedKey;
	char *ga_filename;

#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "**** DIGEST AUTH at  T=%lu  user  \"%s\"",apr_time_now()/1000000,user);
#endif

	unsigned char *hash = apr_palloc(r->pool,APR_MD5_DIGESTSIZE);

	ga_filename = apr_psprintf(r->pool,"%s/%s",conf->pwfile,user);

	sharedKey = getSharedKey(r,ga_filename);

	if (!sharedKey)
    return AUTH_USER_NOT_FOUND;

	int secretLen;
	uint8_t *secret = get_shared_secret(r,sharedKey,&secretLen);

	unsigned int truncatedHash = computeTimeCode(get_timestamp(),secret,secretLen);
	char *pwstr = apr_psprintf(r->pool,"%6.6u",truncatedHash);
	char *hashstr = apr_psprintf(r->pool,"%s:%s:%s",user,realm,pwstr);
	
#ifdef DEBUG
ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Password \"%s\" at modulus %lu",pwstr,(apr_time_now() / 1000000) % 30);
#endif

	apr_md5(hash ,hashstr,strlen(hashstr));
	*rethash = hex_encode(r->pool,hash,APR_MD5_DIGESTSIZE);
	addCookie(r,secret,secretLen);
    return AUTH_USER_FOUND;
}


	/***
	 *** Check for Valid
	 *** Authentication Cookie
	 ***/

static int do_cookie_auth(request_rec *r) {

#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "**** COOKIE AUTH at  T=%lu",apr_time_now()/1000000);
#endif

	//unsigned int cookie_expires;
	//char *cookie_valid;
	const char *user;

	authn_google_config_rec *conf = ap_get_module_config(r->per_dir_config, &authn_google_module);

	if (conf->cookieLife && find_cookie(r, &user, 0L, 0L)) {
#ifdef DEBUG
    	ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "User %s auth granted from cookie",user);
#endif
		r->user = (char *) user;
		r->ap_auth_type = "Cookie";
		return OK;
	}
	return DECLINED;	/* Let someone else deal with it */
}

static void ga_child_init (apr_pool_t *p, server_rec *s) {
	cookie_regexp = ap_pregcomp(p, "^google_authn=([^;,]+):([^;,]+):([^;,]+)|[;,][ \t]*google_authn=([^;,]+):([^;,]+):([^;,]+)", AP_REG_EXTENDED);
}

static const authn_provider authn_google_provider = {&ga_check_password, &ga_get_realm_hash};

static void register_hooks(apr_pool_t *p) {
	static const char * const parsePre[]={ "mod_auth_digest.c", NULL };
	ap_hook_child_init(ga_child_init, 0L, 0L, APR_HOOK_FIRST);
	ap_hook_check_user_id(do_cookie_auth, 0L, parsePre, APR_HOOK_FIRST);
	ap_register_provider(p, AUTHN_PROVIDER_GROUP, "google_authenticator", "0", &authn_google_provider);
}

module AP_MODULE_DECLARE_DATA authn_google_module = {
    STANDARD20_MODULE_STUFF,
    create_authn_google_dir_config,	/* dir config creater */
    NULL,							/* dir merger --- default is to override */
    NULL,							/* server config */
    NULL,							/* merge server config */
    authn_google_cmds,				/* command apr_table_t */
    register_hooks					/* register hooks */
};
