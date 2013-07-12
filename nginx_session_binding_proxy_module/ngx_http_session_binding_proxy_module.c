/**
Copyright (c) <2012>, <Willem Burgers>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
------------------------------------------------------------
*/
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#define ngx_strrchr(s1, c)   strrchr((const char *) s1, (int) c)

enum {
    ngx_http_encrypted_session_key_length = 256 / 8,
    ngx_http_encrypted_session_iv_length = EVP_MAX_IV_LENGTH
};

/**
Define the location configuration for SBP
*/
typedef struct {
	ngx_flag_t					enable;
	u_char						*key;
	ngx_array_t					*variables;
} ngx_http_session_binding_proxy_loc_conf_t;

/**
Define the functions in this module
*/
static char *ngx_http_session_binding_proxy(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_session_binding_proxy_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_session_binding_proxy_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_session_binding_proxy_init(ngx_conf_t *cf);
ngx_int_t ngx_http_session_binding_proxy_3des_mac_encrypt(ngx_pool_t *pool, ngx_log_t *log,
        const u_char *iv, size_t iv_len, const u_char *key,
        size_t key_len, ngx_str_t in,
		u_char **dst, size_t *dst_len);
ngx_int_t ngx_http_session_binding_proxy_3des_mac_decrypt(ngx_pool_t *pool, ngx_log_t *log,
        const u_char *iv, size_t iv_len, const u_char *key,
        size_t key_len, ngx_str_t in, u_char **dst,
        size_t *dst_len);

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;

/**
Define the directive for SBP and which function to call when it is read in the conf
*/
static ngx_command_t ngx_http_session_binding_proxy_commands[] = {
    { ngx_string("session_binding_proxy"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_session_binding_proxy,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
 
    ngx_null_command
};

/**
Module context definition
*/
static ngx_http_module_t ngx_http_session_binding_proxy_module_ctx = {
    NULL,											/* preconfiguration */
    ngx_http_session_binding_proxy_init,			/* postconfiguration */

    NULL,											/* create main configuration */
    NULL,											/* init main configuration */

    NULL,											/* create server configuration */
    NULL,											/* merge server configuration */

	ngx_http_session_binding_proxy_create_loc_conf,	/* create location configuration */
    ngx_http_session_binding_proxy_merge_loc_conf	/* merge location configuration */
};

/**
Module definition
*/
ngx_module_t ngx_http_session_binding_proxy_module = {
    NGX_MODULE_V1,
    &ngx_http_session_binding_proxy_module_ctx,		/* module context */
    ngx_http_session_binding_proxy_commands,		/* module directives */
    NGX_HTTP_MODULE,								/* module type */
    NULL,											/* init master */
    NULL,											/* init module */
    NULL,											/* init process */
    NULL,											/* init thread */
    NULL,											/* exit thread */
    NULL,											/* exit process */
    NULL,											/* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_http_session_binding_proxy_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_session_binding_proxy_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_session_binding_proxy_loc_conf_t));

    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
	
	conf->enable = NGX_CONF_UNSET;
	conf->key = NGX_CONF_UNSET_PTR;

    return conf;
}

static char *
ngx_http_session_binding_proxy_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_session_binding_proxy_loc_conf_t *prev = parent;
    ngx_http_session_binding_proxy_loc_conf_t *conf = child;
	
	ngx_conf_merge_value(conf->enable, prev->enable, 0);
	
	ngx_conf_merge_ptr_value(conf->key, prev->key,
            NULL);

    return NGX_CONF_OK;
}

/**
Handle incoming requests. If they contain cookies that are specified in the conf, then it will be decrypted.
*/
static ngx_int_t
ngx_http_session_binding_proxy_handler(ngx_http_request_t *r)
{	
	ngx_http_session_binding_proxy_loc_conf_t	*splcf;
    splcf = ngx_http_get_module_loc_conf(r, ngx_http_session_binding_proxy_module);
	
	if (splcf->enable != 1) { //module not enabled in nginx.conf.
		return NGX_DECLINED;
	}
	
	if (splcf->key == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "session_binding_proxy: a key is required to be defined");

        return NGX_ERROR;
    }
	
	ngx_str_t							iv;
	
	//Check if we indeed have an SSL connection
	if (r->connection->ssl) {
		SSL_SESSION* ssl_session = SSL_get_session(r->connection->ssl->connection);
		if (ssl_session) {
			uint64_t* mkey = (uint64_t*)ssl_session->master_key;
			ngx_log_debug(NGX_LOG_DEBUG_HTTP,r->connection->log,0,"ssl_session_master_key: %016xL %016xL %016xL",*(mkey),*(mkey+1),*(mkey+2));
			iv.len = ssl_session->master_key_length/3;
			iv.data = ngx_pnalloc(r->pool, iv.len);
			ngx_snprintf(iv.data, iv.len, "%016xL", *mkey);
			
			if (iv.len > ngx_http_encrypted_session_iv_length) {
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
						"encrypted_session_iv: the init vector must NOT "
						"be longer than %d bytes",
						ngx_http_encrypted_session_iv_length);

				return NGX_ERROR;
			}
		}
	}
	else {
		ngx_log_error(NGX_LOG_ERR,r->connection->log,0,"cannot decrypt cookie");
		return NGX_ERROR;
	}
	
	ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"Session Binding Proxy Handler IV: %V", &iv);
	
	/**
	define most variables here now that we know the module is enabled and a proper key is set
	*/
	static ngx_str_t					verification = ngx_string("+session_binding_proxy");
	ngx_uint_t i,j;
	ngx_list_part_t						*part;
	ngx_table_elt_t						*header;
	u_char								*p, *p1, *p2, *p3, *p4, *dst;
	ngx_str_t							arg,cookie, *variable;
	size_t								len;
	ngx_int_t							rc;
	
	variable = splcf->variables->elts;
	part = &r->headers_in.headers.part;
	header = part->elts;

	for (i = 0; /* void */; i++) { //For each header

		if (i >= part->nelts) {
			if (part->next == NULL) {
				break;
			}

			part = part->next;
			header = part->elts;
			i = 0;
		}
		
		if(ngx_strncmp((&header[i])->key.data, "Cookie",6) == 0) //If the header is the cookie header
		{
			for(j = 0;j<splcf->variables->nelts;j++) { // For every specified cookie name in the conf
				ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
							"Session Binding Proxy Handler searching for: %V", &variable[j]);
				
				ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
							"Session Binding Proxy Handler in string: %V", &((&header[i])->value));
				
				if((p1 = (u_char *) ngx_strstr(((&header[i])->value).data, variable[j].data)) != NULL) //find the cookie value
				{	
					p2 = (u_char *) ngx_strchr(p1, '=');
					p3 = (u_char *) ngx_strchr(p1, ';');
					
					if (p2) {
						p2++;
						arg.len = ((&header[i])->value.data + (&header[i])->value.len) - p2;
						if(p3){
							arg.len -= ((&header[i])->value.data + (&header[i])->value.len) - p3;
						}
						arg.data = p2;
					}
					
					ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
							"Encrypted cookie value: %V", &arg);
					
					// base64 decode the cookie value
					ngx_str_t decoded;
					decoded.len = ngx_base64_decoded_length(arg.len) + 1;
					p = decoded.data = ngx_pnalloc(r->pool, decoded.len);
					if (p == NULL) {
						return NGX_ERROR;
					}
					ngx_decode_base64(&decoded, &arg);
					decoded.data[decoded.len] = '\0';
					
					// decrypt the cookie value
					rc = ngx_http_session_binding_proxy_3des_mac_decrypt(r->pool,
						r->connection->log, iv.data, iv.len,
						splcf->key, ngx_http_encrypted_session_key_length,
						decoded, &dst, &len);

					if (rc == NGX_OK) {
						ngx_str_t decrypted;
						decrypted.len = len;
						decrypted.data = dst;
						
						ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
									"Session Binding Proxy decrypted: %V",
									&decrypted);
						
						p4 = (u_char *) ngx_strrchr(decrypted.data, '+');
						
						if (p4) {// a valid cookie value contains the string "+session_binding_proxy", find it
							if (ngx_memcmp(p4,verification.data,decrypted.data + decrypted.len - p4) == 0) {
								ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
								"Valid cookie");
								
								//get the actual value of the cookie for the backend server
								cookie.len = (p1 - header[i].value.data);
								
								cookie.len += variable[j].len;
								cookie.len++;
								cookie.len += decrypted.len - verification.len;
								if(p3){
									cookie.len += (header[i].value.data + header[i].value.len) - p3;
								}
								
								p = cookie.data = ngx_palloc(r->pool, cookie.len);
								if(p == NULL) {
									return NGX_ERROR;
								}
								
								//build the new cookie header
								p = ngx_copy(p, header[i].value.data, p1 - header[i].value.data);
								p = ngx_copy(p, variable[j].data, variable[j].len);
								p = ngx_copy(p, "=", 1);
								p = ngx_copy(p, decrypted.data, decrypted.len - verification.len);
								if(p3){
									p = ngx_copy(p, p3, (header[i].value.data + header[i].value.len) - p3);
								}
								
								//replace the cookie header with this decrypted one
								header[i].value.len = cookie.len;
								header[i].value.data = cookie.data;
								
								ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
									"Session Binding Proxy cookie to backend: \"%V: %V\"",
									&header[i].key, &header[i].value);
							}
						}
					}
					else {
						ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
									"Session Binding Proxy can't decrypt cookie");
						dst = NULL;
						len = 0;
					}
				}
			}
		}
	}
	
	return NGX_DECLINED;
}

/**
This filter works on responses coming from the backend server.
Check all headers for cookies and if one of the specified cookie names in the conf is set, encrypt it.
*/
static ngx_int_t
ngx_http_session_binding_proxy_header_filter(ngx_http_request_t *r)
{
    ngx_http_session_binding_proxy_loc_conf_t  *splcf;
    splcf = ngx_http_get_module_loc_conf(r, ngx_http_session_binding_proxy_module);
	
	if (splcf->enable != 1) { //module not enabled in nginx.conf.
		return ngx_http_next_header_filter(r);
	}
	
	if (splcf->key == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "session_binding_proxy: a key is required to be defined");

        return NGX_ERROR;
    }

	ngx_str_t							iv;
	
	//check if we indeed have an SSL connection
	if (r->connection->ssl) {
		SSL_SESSION* ssl_session = SSL_get_session(r->connection->ssl->connection);
		if (ssl_session) {
			uint64_t* mkey = (uint64_t*)ssl_session->master_key;
			ngx_log_debug(NGX_LOG_DEBUG_HTTP,r->connection->log,0,"ssl_session_master_key: %016xL %016xL %016xL",*(mkey),*(mkey+1),*(mkey+2));
			iv.len = ssl_session->master_key_length/3;
			iv.data = ngx_pnalloc(r->pool, iv.len);
			ngx_snprintf(iv.data, iv.len, "%016xL", *mkey);
			
			if (iv.len > ngx_http_encrypted_session_iv_length) {
				ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
						"encrypted_session_iv: the init vector must NOT "
						"be longer than %d bytes",
						ngx_http_encrypted_session_iv_length);

				return NGX_ERROR;
			}
		}
	}
	else {
		ngx_log_error(NGX_LOG_ERR,r->connection->log,0,"cannot decrypt cookie");
		return NGX_ERROR;
	}
	
	ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"Session Binding Proxy Filter IV: %V", &iv);
	
	// Define most variables here after the checks
	static ngx_str_t					verification = ngx_string("+session_binding_proxy");
	ngx_str_t							arg, value, res, *variable;
	ngx_uint_t							i,j;
	ngx_list_part_t						*part;
	ngx_table_elt_t						*header;
	u_char								*p, *p1, *p2, *dst;
	size_t								len;
    ngx_int_t							rc;
	
	variable = splcf->variables->elts;
	part = &r->headers_out.headers.part;
	header = part->elts;

	arg.len = 0;
	
	for (i = 0; /* void */; i++) { // For each header

		if (i >= part->nelts) {
			if (part->next == NULL) {
				break;
			}

			part = part->next;
			header = part->elts;
			i = 0;
		}
		
		if(ngx_strncmp((&header[i])->key.data, "Set-Cookie",10) == 0) //If the header is a Set-Cookie header
		{
			for(j = 0;j<splcf->variables->nelts;j++) {//See if this cookie needs to be encrypted (specified in the conf)
				if(ngx_strncmp((&header[i])->value.data, variable[j].data, variable[j].len) != 0)
					continue;
				else{
					p1 = (u_char *) ngx_strchr((&header[i])->value.data, '='); // find the value of the cookie (between = and ;
					p2 = (u_char *) ngx_strchr((&header[i])->value.data, ';');
					
					//copy the cookie value to a new string and add the verification string
					arg.len = 0;
					arg.data = ngx_palloc(r->pool, arg.len);
					
					if (p1 && p2) {
						p1++;
						arg.len = (((&header[i])->value.data + (&header[i])->value.len) - p1) - (((&header[i])->value.data + (&header[i])->value.len) - p2);
						arg.data = p1;
					}
					
					value.len = arg.len + verification.len;
					p = value.data = ngx_palloc(r->pool, value.len);

					if(p == NULL) {
						return NGX_ERROR;
					}

					p = ngx_copy(p, arg.data, arg.len);
					p = ngx_copy(p, verification.data, verification.len);
					
					//encrypt the value
					rc = ngx_http_session_binding_proxy_3des_mac_encrypt(r->pool,
							r->connection->log, iv.data, iv.len,
							splcf->key, ngx_http_encrypted_session_key_length,
							value, &dst, &len);

					if (rc != NGX_OK) {
						dst = NULL;
						len = 0;

						ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
								"encrypted_session: failed to encrypt");
					}

					res.data = dst;
					res.len = len;

					//base64 encode the encrypted value, because otherwise it might not be sendable
					ngx_str_t encoded;
					len = ngx_base64_encoded_length(res.len) + 1;
					p = encoded.data = ngx_pnalloc(r->pool, len);
					if (p == NULL) {
						return NGX_ERROR;
					}
					ngx_encode_base64(&encoded, &res);
					encoded.data[encoded.len] = '\0';
					
					//build the new cookie header data
					ngx_str_t cookie_value;
					cookie_value.len = variable[j].len; // name of the cookie.
					cookie_value.len++; // =
					cookie_value.len += encoded.len; //length of the encrypted value.
					cookie_value.len += (((&header[i])->value.data + (&header[i])->value.len) - p2); //length of the rest of the cookie value
					
					p = cookie_value.data = ngx_palloc(r->pool, cookie_value.len);

					if(p == NULL) {
						return NGX_ERROR;
					}
					
					p = ngx_copy(p, variable[j].data, variable[j].len);
					p = ngx_copy(p, "=", 1);
					p = ngx_copy(p, encoded.data, encoded.len);
					p = ngx_copy(p, p2, ((&header[i])->value.data + (&header[i])->value.len) - p2);
					
					//replace the header with the encrypted one
					header[i].value.len = cookie_value.len;
					header[i].value.data = cookie_value.data;
					
					ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
						"Session Binding Proxy cookie to client: \"%V: %V\"",
						&header[i].key, &header[i].value);
					}
			}
		}
	}
	
	return ngx_http_next_header_filter(r);
}

/*
The functions below (encrypt and decrypt) are taken from agentzh's encrypted-session nginx module
Modified slightly for use in this module
see https://github.com/agentzh/encrypted-session-nginx-module for details
*/
ngx_int_t ngx_http_session_binding_proxy_3des_mac_encrypt(ngx_pool_t *pool, ngx_log_t *log,
        const u_char *iv, size_t iv_len, const u_char *key,
        size_t key_len, ngx_str_t in,
		u_char **dst, size_t *dst_len)
{
    EVP_CIPHER_CTX ctx;
    const EVP_CIPHER *cipher;
    u_char *p, *data;
    int ret;
    size_t block_size, buf_size, data_size;
    int len;

    if (key_len != ngx_http_encrypted_session_key_length)
    {
        return NGX_ERROR;
    }

    EVP_CIPHER_CTX_init(&ctx);

    cipher = EVP_aes_256_cbc();

    block_size = EVP_CIPHER_block_size(cipher);

    data_size = in.len;

    buf_size = SHA256_DIGEST_LENGTH /* for the digest */
             + (data_size + block_size - 1) /* for EVP_EncryptUpdate */
             + block_size /* for EVP_EncryptFinal */
             ;

    p = ngx_palloc(pool, buf_size + data_size);
    if (p == NULL) {
        return NGX_ERROR;
    }

    *dst = p;

    data = p + buf_size;

    ngx_memcpy(data, in.data, in.len);

    SHA256(data, data_size, p);

    p += SHA256_DIGEST_LENGTH;

    ret = EVP_EncryptInit(&ctx, cipher, key, iv);
    if (! ret) {
        goto evp_error;
    }

    /* encrypt the raw input data */

    ret = EVP_EncryptUpdate(&ctx, p, &len, data, data_size);
    if (! ret) {
        goto evp_error;
    }

    p += len;

    ret = EVP_EncryptFinal(&ctx, p, &len);
    if (! ret) {
        return NGX_ERROR;
    }

    /* XXX we should still explicitly release the ctx
* or we'll leak memory here */
    EVP_CIPHER_CTX_cleanup(&ctx);

    p += len;

    *dst_len = p - *dst;

    if (*dst_len > buf_size) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                "encrypted_session: 3des_mac_encrypt: buffer error");

        return NGX_ERROR;
    }

    return NGX_OK;

evp_error:

    EVP_CIPHER_CTX_cleanup(&ctx);

    return NGX_ERROR;
}

ngx_int_t
ngx_http_session_binding_proxy_3des_mac_decrypt(ngx_pool_t *pool, ngx_log_t *log,
        const u_char *iv, size_t iv_len, const u_char *key,
        size_t key_len, ngx_str_t in, u_char **dst,
        size_t *dst_len)
{
    EVP_CIPHER_CTX ctx;
    const EVP_CIPHER *cipher;
    int ret;
    size_t block_size, buf_size;
    int len;
    u_char *p;
    const u_char *digest;

    u_char new_digest[SHA256_DIGEST_LENGTH];

    if (key_len != ngx_http_encrypted_session_key_length
            || in.len < SHA256_DIGEST_LENGTH)
    {
        return NGX_ERROR;
    }

    digest = in.data;

    EVP_CIPHER_CTX_init(&ctx);

    cipher = EVP_aes_256_cbc();

    ret = EVP_DecryptInit(&ctx, cipher, key, iv);
    if (! ret) {
        goto evp_error;
    }

    block_size = EVP_CIPHER_block_size(cipher);

    buf_size = in.len + block_size /* for EVP_DecryptUpdate */
             + block_size /* for EVP_DecryptFinal */
             ;

    p = ngx_palloc(pool, buf_size);
    if (p == NULL) {
        return NGX_ERROR;
    }

    *dst = p;

    ret = EVP_DecryptUpdate(&ctx, p, &len, in.data + SHA256_DIGEST_LENGTH,
            in.len - SHA256_DIGEST_LENGTH);

    if (! ret) {
        goto evp_error;
    }

    p += len;

    ret = EVP_DecryptFinal(&ctx, p, &len);

    /* XXX we should still explicitly release the ctx
* or we'll leak memory here */
    EVP_CIPHER_CTX_cleanup(&ctx);

    if (! ret) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0,
                "failed to decrypt session: bad AES-256 digest.");

        return NGX_ERROR;
    }

    p += len;

    *dst_len = p - *dst;

    if (*dst_len > buf_size) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                "encrypted_session: 3des_mac_decrypt: buffer error");

        return NGX_ERROR;
    }

    SHA256(*dst, *dst_len, new_digest);

    if (ngx_strncmp(digest, new_digest, SHA256_DIGEST_LENGTH) != 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0,
                "failed to decrypt session: SHA256 checksum mismatch.");

        return NGX_ERROR;
    }

    return NGX_OK;

evp_error:

    EVP_CIPHER_CTX_cleanup(&ctx);

    return NGX_ERROR;
}

/**
Read cookie names from the conf file
*/
static char *
ngx_http_session_binding_proxy_add_variables(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_session_binding_proxy_loc_conf_t		*sbplcf = conf;

    ngx_uint_t										i;
    ngx_str_t										*value, *variable;

    value = cf->args->elts;

    sbplcf->variables = ngx_array_create(cf->pool,
        cf->args->nelts-2, sizeof(ngx_str_t));

    if(sbplcf->variables == NULL) {
        return NGX_CONF_ERROR;
    }

    for(i = 2;i<cf->args->nelts;i++) {
        if (value[i].data[0] != '$') {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid variable name \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }

        variable = ngx_array_push(sbplcf->variables);
        if(variable == NULL) {
            return NGX_CONF_ERROR;
        }

        value[i].len--;
        value[i].data++;

        variable->len = value[i].len;
		variable->data = value[i].data;
    }

    return NGX_CONF_OK;
}

/**
This function is called when the directive is specified in the conf file.
It reads all parameters in the conf for the session_binding_proxy directive.
*/
static char *
ngx_http_session_binding_proxy(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_str_t *value;
	
	ngx_http_session_binding_proxy_loc_conf_t  *sbplcf = conf;
	
    if (sbplcf->key != NGX_CONF_UNSET_PTR) {
        return "is duplicate key";
    }
	
	value = cf->args->elts;
	
	if(cf->args->nelts < 2) {
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid number of arguments for the session_binding_proxy directive");
		return NGX_CONF_ERROR;
	}
	
	if (value[1].len != ngx_http_encrypted_session_key_length) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "encrypted_session_key: the key must be of %d bytes long",
                ngx_http_encrypted_session_key_length);

        return NGX_CONF_ERROR;
    }
	
	if(ngx_http_session_binding_proxy_add_variables(cf, cmd, conf) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }
	
	sbplcf->enable = 1;
	sbplcf->key = value[1].data;

    return NGX_CONF_OK;
}

/**
Register the module on startup.
Even if the module is not used (the directive is in the conf), it is added to the list of handlers and filters.
The check functions in the handler and filter skip to the next handler or filter if the module is not enabled.
*/
static ngx_int_t ngx_http_session_binding_proxy_init(ngx_conf_t *cf)
{
	ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_session_binding_proxy_handler;
	
	ngx_http_next_header_filter = ngx_http_top_header_filter;
	ngx_http_top_header_filter = ngx_http_session_binding_proxy_header_filter;
	
	return NGX_OK;
}
