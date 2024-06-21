/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <pulse/rtclock.h>
#include <pulse/timeval.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/mutex.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/ioline.h>
#include <pulsecore/arpa-inet.h>
#include <pulsecore/random.h>
#include <pulsecore/core-rtclock.h>

#include "rtsp-client.h"
#include "rtsp-util.h"

#define RECONNECT_INTERVAL (5 * PA_USEC_PER_SEC)

enum wait_state {
    WAIT_NONE,
    WAIT_RESPONSE,
    WAIT_HEADERS
};

enum auth_method {
    AUTH_NONE,
    AUTH_BASIC,
    AUTH_DIGEST
};

struct pa_rtsp_client {
    pa_mainloop_api *mainloop;
    char *hostname;
    uint16_t port;

    pa_socket_client *sc;
    pa_ioline *ioline;

    pa_rtsp_cb_t callback;

    void *userdata;
    const char *useragent;
    const char *username;
    const char *password;
    enum auth_method mth;
    char *realm, *nonce;

    pa_rtsp_state_t state;
    pa_rtsp_status_t status;
    enum wait_state waiting;
    pa_mutex *mutex;
    int length;

    pa_headerlist* headers;
    char *last_header;
    pa_strbuf *header_buffer;
    pa_headerlist* response_headers;

    char *localip;
    char *url;
    uint16_t rtp_port;
    uint32_t cseq;
    char *session;
    char *transport;
    pa_time_event *reconnect_event;
    bool autoreconnect;
};

pa_rtsp_client* pa_rtsp_client_new(pa_mainloop_api *mainloop, const char *hostname, uint16_t port, const char *useragent, bool autoreconnect) {
    pa_rtsp_client *c;

    pa_assert(mainloop);
    pa_assert(hostname);
    pa_assert(port > 0);

    c = pa_xnew0(pa_rtsp_client, 1);
    c->mainloop = mainloop;
    c->hostname = pa_xstrdup(hostname);
    c->port = port;
    c->headers = pa_headerlist_new();

    if (useragent)
        c->useragent = useragent;
    else
        c->useragent = "PulseAudio RTSP Client";

    c->mth = AUTH_NONE;
    c->autoreconnect = autoreconnect;

    c->waiting = WAIT_NONE;
    c->mutex = pa_mutex_new(false, false);
    c->length = 0;

    return c;
}

static void free_events(pa_rtsp_client *c) {
    pa_assert(c);

    if (c->reconnect_event) {
        c->mainloop->time_free(c->reconnect_event);
        c->reconnect_event = NULL;
    }
}

void pa_rtsp_client_free(pa_rtsp_client *c) {
    pa_assert(c);

    free_events(c);
    if (c->sc)
        pa_socket_client_unref(c->sc);

    pa_rtsp_disconnect(c);

    pa_xfree(c->hostname);
    pa_xfree(c->url);
    pa_xfree(c->localip);
    pa_xfree(c->session);
    pa_xfree(c->transport);
    pa_xfree(c->last_header);
    pa_xfree(c->realm);
    pa_xfree(c->nonce);
    pa_mutex_free(c->mutex);
    c->mutex = NULL;
    if (c->header_buffer)
        pa_strbuf_free(c->header_buffer);
    if (c->response_headers)
        pa_headerlist_free(c->response_headers);
    pa_headerlist_free(c->headers);

    pa_xfree(c);
}

static void authenticate(pa_rtsp_client *c) {
    const char *current = NULL;
    const char *wath;
    char space[] = " ";
    char *token = NULL;
    char *val = NULL, *mth = NULL;
    char comma[] = ",";

    pa_xfree(c->realm);
    pa_xfree(c->nonce);

    c->mth = AUTH_NONE;
    c->realm = c->nonce = NULL;

    if (!c->username || !c->password)
        return;

    wath = pa_headerlist_gets(c->response_headers, "WWW-Authenticate");

    if (!wath)
        return;

    mth = pa_split(wath, space, &current);

    if (pa_safe_streq(mth, "Basic"))
        c->mth = AUTH_BASIC;
    else if (pa_safe_streq(mth, "Digest"))
        c->mth = AUTH_DIGEST;
    else
        goto done;

    while ((token = pa_split(wath, comma, &current))) {
        if ((val = strstr(token, "="))) {
            if (NULL == c->realm && val > strstr(token, "realm")) {
                if (!(c->realm = pa_xstrdup(val + 2)))
                    goto done;
                pa_rtsp_rtrim_char(c->realm, '\"');
            }
            else if (NULL == c->nonce && val > strstr(token, "nonce")) {
                if (!(c->nonce = pa_xstrdup(val + 2)))
                    goto done;
                pa_rtsp_rtrim_char(c->nonce, '\"');
            }
        }

        pa_xfree(token);
        token = NULL;
    }

done:
    pa_xfree(token);
    pa_xfree(mth);
}

static void headers_read(pa_rtsp_client *c) {
    char delimiters[] = ";";
    char* token = NULL;
    const char *clength;

    pa_assert(c);
    pa_assert(c->response_headers);
    pa_assert(c->callback);

    c->length = 0;

    clength = pa_headerlist_gets(c->response_headers, "Content-Length");
    if (clength && pa_atoi(clength, &c->length) < 0)
        pa_log_warn("Unexpected value in content-length: %s", clength);

    if (c->status == STATUS_UNAUTHORIZED)
        authenticate(c);

    /* Deal with a SETUP response */
    if (STATE_SETUP == c->state) {
        const char* token_state = NULL;
        const char* pc = NULL;
        c->session = pa_xstrdup(pa_headerlist_gets(c->response_headers, "Session"));
        c->transport = pa_xstrdup(pa_headerlist_gets(c->response_headers, "Transport"));

        if (!c->session || !c->transport) {
            pa_log("Invalid SETUP response.");
            goto done;
        }

        /* Now parse out the server port component of the response. */
        while ((token = pa_split(c->transport, delimiters, &token_state))) {
            if ((pc = strchr(token, '='))) {
                if (0 == strncmp(token, "server_port", 11)) {
                    uint32_t p;

                    if (pa_atou(pc + 1, &p) < 0 || p <= 0 || p > 0xffff) {
                        pa_log("Invalid SETUP response (invalid server_port).");
                        goto done;
                    }

                    c->rtp_port = p;
                    break;
                }
            }
            pa_xfree(token);
        }
        if (0 == c->rtp_port) {
            /* Error no server_port in response */
            pa_log("Invalid SETUP response (no port number).");
            goto done;
        }
    }

done:
    pa_xfree(token);

    c->waiting = WAIT_NONE;

    /* Call our callback */
    c->callback(c, c->state, c->status, c->response_headers, c->userdata);
}

static void line_callback(pa_ioline *line, const char *s, size_t l, void *userdata) {
    pa_rtsp_client *c = userdata;
    char *delimpos;
    char *s2, *s2p;

    pa_assert(line);
    pa_assert(c);
    pa_assert(c->callback);

    if (!s) {
        /* Keep the ioline/iochannel open as they will be freed automatically */
        c->ioline = NULL;
        c->callback(c, STATE_DISCONNECTED, STATUS_NO_RESPONSE, NULL, c->userdata);
        return;
    }

    /* Skip any body from the last response */
    if (c->length) {
        if (l > c->length) {
            l -= c->length;
            s += c->length;
            c->length = 0;
        } else {
            c->length -= l;
            return;
        }
    }

    pa_assert(l);

    if (c->waiting == WAIT_NONE) {
        pa_log_warn("Received more data than content length");
        return;
    }

    s2 = pa_xstrdup(s);
    /* Trim trailing carriage returns */
    s2p = s2 + strlen(s2) - 1;
    while (s2p >= s2 && '\r' == *s2p) {
        *s2p = '\0';
        s2p -= 1;
    }

    if (c->waiting == WAIT_RESPONSE && pa_streq(s2, "RTSP/1.0 200 OK")) {
        if (c->response_headers)
            pa_headerlist_free(c->response_headers);
        c->response_headers = pa_headerlist_new();

        c->status = STATUS_OK;
        c->waiting = WAIT_HEADERS;
        goto exit;
    } else if (c->waiting == WAIT_RESPONSE && pa_streq(s2, "RTSP/1.0 401 Unauthorized")) {
        if (c->response_headers)
            pa_headerlist_free(c->response_headers);
        c->response_headers = pa_headerlist_new();

        c->status = STATUS_UNAUTHORIZED;
        c->waiting = WAIT_HEADERS;
        goto exit;
    } else if (c->waiting == WAIT_RESPONSE) {
        if (c->response_headers)
            pa_headerlist_free(c->response_headers);
        c->response_headers = pa_headerlist_new();

        pa_log_warn("Unexpected/Unhandled response: %s", s2);

        if (pa_streq(s2, "RTSP/1.0 400 Bad Request"))
            c->status = STATUS_BAD_REQUEST;
        else if (pa_streq(s2, "RTSP/1.0 500 Internal Server Error"))
            c->status = STATUS_INTERNAL_ERROR;
        else
            c->status = STATUS_NO_RESPONSE;
        c->waiting = WAIT_HEADERS;
        goto exit;
    }

    if (!strlen(s2)) {
        /* End of headers */
        /* We will have a header left from our looping iteration, so add it in :) */
        if (c->last_header) {
            char *tmp = pa_strbuf_to_string_free(c->header_buffer);
            /* This is not a continuation header so let's dump it into our proplist */
            pa_headerlist_puts(c->response_headers, c->last_header, tmp);
            pa_xfree(tmp);
            pa_xfree(c->last_header);
            c->last_header = NULL;
            c->header_buffer = NULL;
        }

        pa_log_debug("Response received. Dispatching");
        headers_read(c);
        goto exit;
    }

    /* Read and parse a header (we know it's not empty) */
    /* TODO: Move header reading into the headerlist. */

    /* If the first character is a space, it's a continuation header */
    if (c->last_header && ' ' == s2[0]) {
        pa_assert(c->header_buffer);

        /* Add this line to the buffer (sans the space) */
        pa_strbuf_puts(c->header_buffer, &(s2[1]));
        goto exit;
    }

    if (c->last_header) {
        char *tmp = pa_strbuf_to_string_free(c->header_buffer);
        /* This is not a continuation header so let's dump the full
          header/value into our proplist */
        pa_headerlist_puts(c->response_headers, c->last_header, tmp);
        pa_xfree(tmp);
        pa_xfree(c->last_header);
        c->last_header = NULL;
        c->header_buffer = NULL;
    }

    delimpos = strstr(s2, ":");
    if (!delimpos) {
        pa_log_warn("Unexpected response when expecting header: %s", s);
        goto exit;
    }

    pa_assert(!c->header_buffer);
    pa_assert(!c->last_header);

    c->header_buffer = pa_strbuf_new();
    if (strlen(delimpos) > 1) {
        /* Cut our line off so we can copy the header name out */
        *delimpos++ = '\0';

        /* Trim the front of any spaces */
        while (' ' == *delimpos)
            ++delimpos;

        pa_strbuf_puts(c->header_buffer, delimpos);
    } else {
        /* Cut our line off so we can copy the header name out */
        *delimpos = '\0';
    }

    /* Save the header name */
    c->last_header = pa_xstrdup(s2);

  exit:
    pa_xfree(s2);
}

static void reconnect_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *t, void *userdata) {
    pa_rtsp_client *c = userdata;
    pa_assert(c);
    if (pa_rtsp_connect(c))
        c->callback(c, STATE_DISCONNECTED, STATUS_NO_RESPONSE, NULL, c->userdata);
}

static void on_connection(pa_socket_client *sc, pa_iochannel *io, void *userdata) {
    pa_rtsp_client *c = userdata;
    union {
        struct sockaddr sa;
        struct sockaddr_in in;
        struct sockaddr_in6 in6;
    } sa;
    socklen_t sa_len = sizeof(sa);

    pa_assert(sc);
    pa_assert(c);
    pa_assert(STATE_CONNECT == c->state);
    pa_assert(c->sc == sc);
    pa_socket_client_unref(c->sc);
    c->sc = NULL;
    c->waiting = WAIT_NONE;

    if (!io) {
        if (c->autoreconnect) {
            struct timeval tv;

            pa_log_warn("Connection to server %s:%d failed: %s - will try later", c->hostname, c->port, pa_cstrerror(errno));

            if (!c->reconnect_event)
                c->reconnect_event = c->mainloop->time_new(c->mainloop, pa_timeval_rtstore(&tv, pa_rtclock_now() + RECONNECT_INTERVAL, true), reconnect_cb, c);
            else
                c->mainloop->time_restart(c->reconnect_event, pa_timeval_rtstore(&tv, pa_rtclock_now() + RECONNECT_INTERVAL, true));
        } else {
            pa_log("Connection to server %s:%d failed: %s", c->hostname, c->port, pa_cstrerror(errno));
            c->callback(c, STATE_DISCONNECTED, STATUS_NO_RESPONSE, NULL, c->userdata);
        }
        return;
    }
    pa_assert(!c->ioline);

    c->ioline = pa_ioline_new(io);
    pa_ioline_set_callback(c->ioline, line_callback, c);

    /* Get the local IP address for use externally */
    if (0 == getsockname(pa_iochannel_get_recv_fd(io), &sa.sa, &sa_len)) {
        char buf[INET6_ADDRSTRLEN];
        const char *res = NULL;

        if (AF_INET == sa.sa.sa_family) {
            if ((res = inet_ntop(sa.sa.sa_family, &sa.in.sin_addr, buf, sizeof(buf)))) {
                c->localip = pa_xstrdup(res);
            }
        } else if (AF_INET6 == sa.sa.sa_family) {
            if ((res = inet_ntop(AF_INET6, &sa.in6.sin6_addr, buf, sizeof(buf)))) {
                c->localip = pa_xstrdup(res);
            }
        }
    }
    pa_log_debug("Established RTSP connection from local ip %s", c->localip);

    if (c->callback)
        c->callback(c, c->state, STATUS_OK, NULL, c->userdata);
}

int pa_rtsp_connect(pa_rtsp_client *c) {
    pa_assert(c);
    pa_assert(!c->sc);

    pa_xfree(c->session);
    c->session = NULL;

    pa_log_debug("Attempting to connect to server '%s:%d'", c->hostname, c->port);
    if (!(c->sc = pa_socket_client_new_string(c->mainloop, true, c->hostname, c->port))) {
        pa_log("failed to connect to server '%s:%d'", c->hostname, c->port);
        return -1;
    }

    pa_socket_client_set_callback(c->sc, on_connection, c);
    c->waiting = WAIT_RESPONSE;
    c->state = STATE_CONNECT;
    c->status = STATUS_NO_RESPONSE;
    return 0;
}

void pa_rtsp_set_callback(pa_rtsp_client *c, pa_rtsp_cb_t callback, void *userdata) {
    pa_assert(c);

    c->callback = callback;
    c->userdata = userdata;
}

void pa_rtsp_disconnect(pa_rtsp_client *c) {
    pa_assert(c);

    if (c->ioline) {
        pa_ioline_close(c->ioline);
        pa_ioline_unref(c->ioline);
    }
    c->ioline = NULL;
}

const char* pa_rtsp_localip(pa_rtsp_client *c) {
    pa_assert(c);

    return c->localip;
}

uint32_t pa_rtsp_serverport(pa_rtsp_client *c) {
    pa_assert(c);

    return c->rtp_port;
}

void pa_rtsp_set_url(pa_rtsp_client *c, const char *url) {
    pa_assert(c);

    pa_xfree(c->url);
    c->url = pa_xstrdup(url);
}

void pa_rtsp_set_credentials(pa_rtsp_client *c, const char *username, const char*password) {
    c->username = username;
    c->password = password;
}

bool pa_rtsp_has_header(pa_rtsp_client *c, const char *key) {
    pa_assert(c);
    pa_assert(key);

    return pa_headerlist_contains(c->headers, key);
}

void pa_rtsp_add_header(pa_rtsp_client *c, const char *key, const char *value) {
    pa_assert(c);
    pa_assert(key);
    pa_assert(value);

    pa_headerlist_puts(c->headers, key, value);
}

const char* pa_rtsp_get_header(pa_rtsp_client *c, const char *key) {
    pa_assert(c);
    pa_assert(key);

    return pa_headerlist_gets(c->headers, key);
}

void pa_rtsp_remove_header(pa_rtsp_client *c, const char *key) {
    pa_assert(c);
    pa_assert(key);

    pa_headerlist_remove(c->headers, key);
}

static char *get_auth(pa_rtsp_client *c, const char *method, const char *url) {
    char *ath = NULL, *response = NULL;

    pa_assert(method);
    pa_assert(url);

    if (!c->username || !c->password)
        return NULL;

    switch (c->mth) {
        case AUTH_NONE:
            break;
        case AUTH_BASIC:
            pa_rtsp_basic_response(c->username, c->password, &response);
            ath = pa_sprintf_malloc("Basic %s", response);
            break;
        case AUTH_DIGEST:
            pa_rtsp_digest_response(c->username, c->realm, c->password, c->nonce, method, url, &response);
            ath = pa_sprintf_malloc("Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"",
                c->username, c->realm, c->nonce, url, response);
            break;
    }

    pa_xfree(response);

    return ath;
}

static int rtsp_exec(pa_rtsp_client *c, const char *cmd, const char *url,
                     pa_headerlist *headers, const char *content_type, const char *content) {
    pa_strbuf *buf;
    char *hdrs;

    pa_assert(cmd);
    pa_assert(url);
    pa_assert(c);
    pa_assert(c->ioline);

    if (!pa_mutex_try_lock(c->mutex)) {
        pa_log_warn("Can't send command (locked): %s", cmd);
        return -1;
    }

    if (c->waiting != WAIT_NONE) {
        pa_log_warn("Can't send command (busy): %s", cmd);
        pa_mutex_unlock(c->mutex);
        return -1;
    }

    pa_log_debug("Sending command: %s", cmd);

    c->waiting = WAIT_RESPONSE;

    pa_mutex_unlock(c->mutex);

    buf = pa_strbuf_new();
    pa_strbuf_printf(buf, "%s %s RTSP/1.0\r\nCSeq: %d\r\n", cmd, url, ++c->cseq);
    if (c->session)
        pa_strbuf_printf(buf, "Session: %s\r\n", c->session);

    /* Add the headers */
    if (headers) {
        hdrs = pa_headerlist_to_string(headers);
        pa_strbuf_puts(buf, hdrs);
        pa_xfree(hdrs);
    }

    if (content_type && content) {
        pa_strbuf_printf(buf, "Content-Type: %s\r\nContent-Length: %d\r\n",
          content_type, (int)strlen(content));
    }

    char *auth = get_auth(c, cmd, url);

    if (auth) {
        pa_strbuf_printf(buf, "Authorization: %s\r\n", auth);
        pa_xfree(auth);
    }

    pa_strbuf_printf(buf, "User-Agent: %s\r\n", c->useragent);

    if (c->headers) {
        hdrs = pa_headerlist_to_string(c->headers);
        pa_strbuf_puts(buf, hdrs);
        pa_xfree(hdrs);
    }

    pa_strbuf_puts(buf, "\r\n");

    if (content_type && content) {
        pa_strbuf_puts(buf, content);
    }

    /* Our packet is created... now we can send it :) */
    hdrs = pa_strbuf_to_string_free(buf);
    /*pa_log_debug("Submitting request:");
    pa_log_debug(hdrs);*/
    pa_ioline_puts(c->ioline, hdrs);
    pa_xfree(hdrs);

    return 0;
}

int pa_rtsp_options(pa_rtsp_client *c) {
    int rv;

    pa_assert(c);

    if (!(rv = rtsp_exec(c, "OPTIONS", "*", NULL, NULL, NULL)))
        c->state = STATE_OPTIONS;

    return rv;
}

int pa_rtsp_announce(pa_rtsp_client *c, const char *sdp) {
    int rv;

    pa_assert(c);

    if (!sdp)
        return -1;

    if (!(rv = rtsp_exec(c, "ANNOUNCE", c->url, NULL, "application/sdp", sdp)))
        c->state = STATE_ANNOUNCE;

    return rv;
}

int pa_rtsp_setup(pa_rtsp_client *c, const char *transport) {
    pa_headerlist *headers;
    int rv;

    pa_assert(c);

    headers = pa_headerlist_new();
    if (!transport)
        pa_headerlist_puts(headers, "Transport", "RTP/AVP/TCP;unicast;interleaved=0-1;mode=record");
    else
        pa_headerlist_puts(headers, "Transport", transport);

    if (!(rv = rtsp_exec(c, "SETUP", c->url, headers, NULL, NULL)))
        c->state = STATE_SETUP;

    pa_headerlist_free(headers);
    return rv;
}

int pa_rtsp_record(pa_rtsp_client *c, uint16_t *seq, uint32_t *rtptime) {
    pa_headerlist *headers;
    char *info;
    int rv;

    pa_assert(c);

    if (!c->session) {
        /* No session in progress */
        return -1;
    }

    pa_random(seq, sizeof(*seq));
    pa_random(rtptime, sizeof(*rtptime));

    headers = pa_headerlist_new();
    pa_headerlist_puts(headers, "Range", "npt=0-");
    info = pa_sprintf_malloc("seq=%u;rtptime=%u", *seq, *rtptime);
    pa_headerlist_puts(headers, "RTP-Info", info);
    pa_xfree(info);

    if (!(rv = rtsp_exec(c, "RECORD", c->url, headers, NULL, NULL)))
        c->state = STATE_RECORD;

    pa_headerlist_free(headers);
    return rv;
}

int pa_rtsp_setparameter(pa_rtsp_client *c, const char *param) {
    int rv;

    pa_assert(c);

    if (!param)
        return -1;

    if (!(rv = rtsp_exec(c, "SET_PARAMETER", c->url, NULL, "text/parameters", param)))
        c->state = STATE_SET_PARAMETER;

    return rv;
}

int pa_rtsp_post(pa_rtsp_client *c, const char *url) {
    int rv;

    pa_assert(c);

    if (!(rv = rtsp_exec(c, "POST", url, NULL, NULL, NULL)))
        c->state = STATE_POST;

    return rv;
}

int pa_rtsp_flush(pa_rtsp_client *c, uint16_t seq, uint32_t rtptime) {
    pa_headerlist* headers;
    char *info;
    int rv;

    pa_assert(c);

    headers = pa_headerlist_new();
    info = pa_sprintf_malloc("seq=%u;rtptime=%u", seq, rtptime);
    pa_headerlist_puts(headers, "RTP-Info", info);
    pa_xfree(info);

    if (!(rv = rtsp_exec(c, "FLUSH", c->url, headers, NULL, NULL)))
        c->state = STATE_FLUSH;

    pa_headerlist_free(headers);
    return rv;
}

int pa_rtsp_teardown(pa_rtsp_client *c) {
    int rv;

    pa_assert(c);

    if (!(rv = rtsp_exec(c, "TEARDOWN", c->url, NULL, NULL, NULL)))
        c->state = STATE_TEARDOWN;

    return rv;
}
