/*
 * evo_net.h — Non-blocking HTTP/REST client and network task queue for EVO Player.
 *
 * Implements a lightweight, thread-safe HTTP/REST client over standard BSD sockets.
 * All asynchronous requests execute on a background worker thread and pump their
 * completion callbacks on the main UI thread via evo_net_poll(), preventing network
 * latency from dropping frames on the 60fps UI loop.
 *
 * Sizing Limits and Boundaries:
 *   - EVO_NET_MAX_HOST (256): Maximum host name length including null terminator.
 *   - EVO_NET_MAX_PATH (2048): Maximum path and query string buffer size. Signed
 *     URLs with a token in the query easily exceed 256 characters; truncation
 *     is strictly treated as an error (-13).
 *   - EVO_NET_MAX_URL (2048): Maximum complete URL length.
 *   - EVO_NET_MAX_REDIRECTS (5): Maximum number of redirect hops followed.
 *   - EVO_NET_MAX_BODY (64 MiB): Hard response body cap to protect memory on
 *     the PS5. Video never routes through evo_net (FFmpeg reads the stream
 *     itself); this client is for JSON, playlists and images. Responses
 *     exceeding this cap return -17.
 *
 * Redirect Behavior:
 *   - Follows up to EVO_NET_MAX_REDIRECTS (5) HTTP redirects (301, 302, 303, 307, 308).
 *   - Location header parsed case-insensitively.
 *   - Supports absolute (http://, https://) and root-relative (/path) targets.
 *   - Status 303, and status 301/302 on a POST request, automatically switch the method
 *     to GET and drop the request payload. Status 307 and 308 preserve method and body.
 *   - Insecure downgrades from HTTPS to HTTP are strictly rejected (-15) to prevent
 *     accidental credential/token leakage.
 *   - A caller header starting "Authorization:" is never sent to a host other
 *     than the one the request was made to, whatever the flags. With
 *     EVO_NET_NO_CROSS_HOST_HEADERS (evo_net_http_request_sync) no caller
 *     header is, and a 307/308 that would carry the body to another host is
 *     not followed: the 307/308 itself is the answer.
 *
 * Request headers:
 *   - Host, User-Agent, Accept and Connection are written by evo_net. A request
 *     with a body also gets Content-Length, and "Content-Type: application/json"
 *     unless a caller header starts with "Content-Type:".
 *
 * Chunked Transfer Decoding:
 *   - Responses with 'Transfer-Encoding: chunked' are transparently decoded in-place.
 *   - Chunk extensions and trailing headers are handled according to RFC specifications.
 *   - Any malformed chunk stream is treated as an error (-16) rather than returning a
 *     partial, corrupted payload to the caller.
 *
 * Connection Lifecycle:
 *   - Requests specify 'Connection: close'. To prevent rogue or misconfigured servers
 *     from stalling the client for the full timeout window (6 seconds), response reading
 *     terminates as soon as 'Content-Length' bytes have been received.
 *
 * Asynchronous Return Codes (evo_net_request_async):
 *    0 (EVO_NET_ASYNC_OK)        : Request successfully enqueued onto worker queue.
 *   -1 (EVO_NET_ASYNC_ERR_INIT)  : Network worker thread initialization failed.
 *   -2 (EVO_NET_ASYNC_ERR_MEM)   : Memory allocation failed for request or headers.
 *   -3 (EVO_NET_ASYNC_ERR_FULL)  : Request queue is full (EVO_NET_MAX_QUEUE reached);
 *                                  rejected immediately to prevent stalling caller thread.
 *   -4 (EVO_NET_ASYNC_ERR_ARG)   : Invalid argument (NULL parameter, or URL/method too long).
 *
 * Synchronous Return Codes (evo_net_http_get_sync, evo_net_http_post_sync):
 *    0 (EVO_NET_OK)              : Success (HTTP status 2xx or 3xx resolved).
 *   -1 (EVO_NET_ERR_INVALID_URL) : Invalid or unsupported URL scheme (not http/https).
 *   -2 (EVO_NET_ERR_DNS)         : DNS host resolution failed.
 *   -3 (EVO_NET_ERR_CONNECT)     : Socket connection to host failed.
 *   -4 (EVO_NET_ERR_SEND_HDR)    : Failed to transmit HTTP request headers.
 *   -5 (EVO_NET_ERR_SEND_BODY)   : Failed to transmit HTTP request body.
 *   -6 (EVO_NET_ERR_MEM)         : Memory allocation failed for response buffer.
 *   -7 (EVO_NET_ERR_REALLOC)     : Memory reallocation failed expanding response buffer.
 *   -8 (EVO_NET_ERR_HTTP)        : HTTP error status received (status < 200 or status >= 400).
 *  -10 (EVO_NET_ERR_SSL_CTX)     : SSL context initialization failed or OpenSSL unavailable.
 *  -11 (EVO_NET_ERR_SSL_NEW)     : SSL session allocation failed.
 *  -12 (EVO_NET_ERR_SSL_CONN)    : SSL handshake failed.
 *  -13 (EVO_NET_ERR_TOO_LONG)    : URL, host, or path exceeded maximum allowed size.
 *  -14 (EVO_NET_ERR_REDIRECTS)   : Exceeded maximum allowed redirect hops (5).
 *  -15 (EVO_NET_ERR_DOWNGRADE)   : Insecure redirect downgrade (HTTPS to HTTP) rejected.
 *  -16 (EVO_NET_ERR_CHUNKED)     : Malformed HTTP chunked-transfer encoding stream.
 *  -17 (EVO_NET_ERR_BODY_LIMIT)  : Response body exceeded hard limit (EVO_NET_MAX_BODY).
 *  -18 (EVO_NET_ERR_TLS_VERIFY)  : EVO_NET_VERIFY_TLS only: the server's certificate
 *                                  did not verify against the embedded CA bundle
 *                                  (or did not match the host name).
 *  -19 (EVO_NET_ERR_TLS_STORE)   : EVO_NET_VERIFY_TLS only: the built-in certificates
 *                                  could not be loaded, so nothing can be verified.
 *  -20 (EVO_NET_ERR_TRUNCATED)   : EVO_NET_VERIFY_TLS only: the answer was cut short
 *                                  (fewer bytes than its Content-Length, a chunked
 *                                  body without its last chunk, or headers that
 *                                  never ended).
 */
#ifndef EVO_NET_H
#define EVO_NET_H

#include <stddef.h>
#include <netdb.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_NET_MAX_HOST        256
#define EVO_NET_MAX_PATH        2048
#define EVO_NET_MAX_URL         2048
#define EVO_NET_MAX_REDIRECTS   5
#define EVO_NET_MAX_BODY        (64 * 1024 * 1024)

/* evo_net_request_async return codes */
#define EVO_NET_ASYNC_OK         0
#define EVO_NET_ASYNC_ERR_INIT  -1
#define EVO_NET_ASYNC_ERR_MEM   -2
#define EVO_NET_ASYNC_ERR_FULL  -3
#define EVO_NET_ASYNC_ERR_ARG   -4

/* Synchronous HTTP return codes */
#define EVO_NET_OK               0
#define EVO_NET_ERR_INVALID_URL -1
#define EVO_NET_ERR_DNS         -2
#define EVO_NET_ERR_CONNECT     -3
#define EVO_NET_ERR_SEND_HDR    -4
#define EVO_NET_ERR_SEND_BODY   -5
#define EVO_NET_ERR_MEM         -6
#define EVO_NET_ERR_REALLOC     -7
#define EVO_NET_ERR_HTTP        -8
#define EVO_NET_ERR_SSL_CTX    -10
#define EVO_NET_ERR_SSL_NEW    -11
#define EVO_NET_ERR_SSL_CONN   -12
#define EVO_NET_ERR_TOO_LONG   -13
#define EVO_NET_ERR_REDIRECTS  -14
#define EVO_NET_ERR_DOWNGRADE  -15
#define EVO_NET_ERR_CHUNKED    -16
#define EVO_NET_ERR_BODY_LIMIT -17
#define EVO_NET_ERR_TLS_VERIFY -18
#define EVO_NET_ERR_TLS_STORE  -19
#define EVO_NET_ERR_TRUNCATED  -20

typedef void (*evo_net_cb)(int success, int status_code, const char *body, size_t body_len, void *user_data);

/*
 * Connects to the first address in `list` that answers, and returns the socket
 * (blocking, with EVO_NET_TIMEOUT_SEC read/write timeouts) or -1.
 *
 * Each address gets at most `per_addr_timeout_ms` (a non-blocking connect
 * bounded by poll). A plain blocking connect() to an address that silently drops
 * packets does not honour SO_SNDTIMEO here: it sits in the kernel's SYN retry
 * for about 75 s. That stalled evo_net's single worker thread - and every
 * request queued behind it - whenever a host had one dead address, which
 * raw.githubusercontent.com does from some networks (hardware, 2026-09-30).
 *
 * An address that timed out is remembered for five minutes and tried after the
 * ones that have not, so only the first request to a host pays for it.
 */
int evo_net_connect_list(const struct addrinfo *list, int per_addr_timeout_ms);

/* Initialize background network worker and request queue. */
int  evo_net_init(void);

/* Shutdown background network worker and clean up. */
void evo_net_shutdown(void);

/*
 * Pump completed network callbacks on the calling (main/UI) thread.
 * Call this once per frame in the main render/event loop.
 */
void evo_net_poll(void);

/*
 * Asynchronous HTTP/REST request. Returns 0 on queued, negative on error.
 * Callback is invoked on the main thread during evo_net_poll().
 */
int  evo_net_request_async(const char *method,
                           const char *url,
                           const char *post_data,
                           const char **headers,
                           int header_count,
                           evo_net_cb callback,
                           void *user_data);

/*
 * Why a request failed. An evo_net_cb only receives success and the HTTP
 * status, so a DNS failure, a refused connection, a TLS error and a reply over
 * the size cap all arrived as the same bare "success = 0".
 *
 *   evo_net_callback_error(): the EVO_NET_ERR_* of the request whose callback is
 *       running now (0 when it succeeded). Meaningful only inside the callback.
 *   evo_net_describe_failure(): that code as a sentence for the screen, led by
 *       the host name ("cf.example.ink: could not find the server ..."). Never
 *       includes the path or query, which carry the login.
 *   evo_net_last_failure(): the sentence for the most recent failed request that
 *       was not an image, if it failed within max_age_ms; 1 if filled. For a
 *       screen whose own callback only knows "it did not load".
 */
int  evo_net_callback_error(void);
void evo_net_describe_failure(int err, int status, const char *url, char *out, size_t cap);
int  evo_net_last_failure(char *out, size_t cap, int max_age_ms);

/*
 * The best available reason for "that did not work", for a screen to show in
 * place of its generic text: the pending evo_error (stream open, playlist load)
 * if there is one, else the last failed network request from the last 5 s.
 * Returns 1 and fills out, or 0 and leaves it untouched - keep the generic text.
 */
int  evo_failure_reason(char *out, size_t cap);

/*
 * Synchronous HTTP GET request. Blocks calling thread.
 * Caller must free(*out_body) if allocated.
 */
int  evo_net_http_get_sync(const char *url,
                           const char **headers,
                           int header_count,
                           char **out_body,
                           size_t *out_len,
                           int *out_status);

/*
 * Synchronous HTTP POST request with JSON or form payload. Blocks calling thread.
 * Caller must free(*out_body) if allocated.
 */
int  evo_net_http_post_sync(const char *url,
                            const char *post_data,
                            const char **headers,
                            int header_count,
                            char **out_body,
                            size_t *out_len,
                            int *out_status);

/*
 * evo_net_http_request_sync() flags.
 *
 *   EVO_NET_VERIFY_TLS: https only, through a second SSL_CTX whose trust store
 *       is the CA bundle built into the app (Mozilla's roots, evo_ca_bundle.c),
 *       with the host name checked, on every hop of a redirect. A certificate
 *       that does not verify fails the request with EVO_NET_ERR_TLS_VERIFY; a
 *       bundle that cannot be loaded fails every verified request with
 *       EVO_NET_ERR_TLS_STORE (found once, not retried). An http:// URL is
 *       refused before anything is sent (EVO_NET_ERR_INVALID_URL); a redirect
 *       from https to http is refused for every caller (EVO_NET_ERR_DOWNGRADE).
 *       The answer must arrive whole: a body shorter than its Content-Length or
 *       a chunked body that ends early is EVO_NET_ERR_TRUNCATED, never a
 *       silently half-read reply (a single-use refresh token must not be lost
 *       that way). Requests without the flag keep returning what arrived.
 *       The certificate dates are checked against the console's clock;
 *       out->error says so when that is the reason. Requests without the flag
 *       keep the unverified context, so images and GQL still load when the
 *       clock is wrong.
 *   EVO_NET_NO_CROSS_HOST_HEADERS: after a redirect to another host, send none
 *       of the caller's headers, and do not follow a 307/308 that would carry
 *       the body there.
 */
#define EVO_NET_VERIFY_TLS            0x1u
#define EVO_NET_NO_CROSS_HOST_HEADERS 0x2u

typedef struct evo_net_request {
    const char         *method;        /* "GET", "POST", ...; NULL = "GET" */
    const char         *url;
    const char         *body;          /* NULL = none; else sent with Content-Length */
    const char *const  *headers;       /* "Name: value" lines; NULL entries skipped */
    int                 header_count;
    unsigned            flags;         /* EVO_NET_VERIFY_TLS | EVO_NET_NO_CROSS_HOST_HEADERS */
} evo_net_request_t;

typedef struct evo_net_response {
    int     status;          /* HTTP status of the final answer; 0 = none */
    char   *body;            /* malloc'd and NUL-terminated, or NULL; caller frees */
    size_t  body_len;
    long    verify_result;   /* the X509_V_ERR_* when rc == EVO_NET_ERR_TLS_VERIFY, else 0 */
    char    error[256];      /* rc != EVO_NET_OK: why, led by the host name, never
                              * the path or query; "" on success */
} evo_net_response_t;

/*
 * Synchronous request, blocking the calling thread. Returns EVO_NET_OK or an
 * EVO_NET_ERR_* (EVO_NET_ERR_HTTP: a real answer with status >= 400, body
 * included). Any thread: all state is on the caller's stack.
 */
int  evo_net_http_request_sync(const evo_net_request_t *req, evo_net_response_t *out);

/*
 * The trust store EVO_NET_VERIFY_TLS uses (the embedded CA bundle), for a
 * socket of the caller's own (Twitch chat when it sends a token). Built once,
 * on the first call from any thread. The caller gets its own reference and
 * releases it with X509_STORE_free(). NULL when it cannot be built (then on
 * every later call too), or in a build without OpenSSL.
 *
 * The store is shared: evo_net's own verified requests use the very same
 * object, on any thread. Never modify it - no X509_STORE_add_cert(), no
 * flags, purpose, trust or verify parameters (set those per connection, e.g.
 * SSL_set1_host() or SSL_get0_param()). Attach it with
 * SSL_CTX_set1_cert_store(ctx, store), which takes a reference of its own,
 * and then X509_STORE_free() yours. (SSL_CTX_set_cert_store() would take over
 * your reference instead, and freeing yours afterwards would then free the
 * shared store out from under evo_net.)
 */
struct x509_store_st;
struct x509_store_st *evo_net_ca_store(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_NET_H */
