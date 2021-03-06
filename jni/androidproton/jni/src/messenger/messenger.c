/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include <proton/messenger.h>
#include <proton/driver.h>
#include <proton/util.h>
#include <proton/ssl.h>
#include <proton/object.h>
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../util.h"
#include "../platform.h"
#include "../platform_fmt.h"
#include "store.h"
#include "transform.h"
#include "subscription.h"

#ifdef __ANDROID__
/*android headers
 *
 */
#include <android/log.h>
#define LOG_TAG		"my-log-tag"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

typedef struct pn_link_ctx_t pn_link_ctx_t;

typedef struct {
  pn_string_t *text;
  bool passive;
  char *scheme;
  char *user;
  char *pass;
  char *host;
  char *port;
  char *name;
} pn_address_t;

// algorithm for granting credit to receivers
typedef  enum {
  // pn_messenger_recv( X ), where:
  LINK_CREDIT_EXPLICIT,  // X > 0
  LINK_CREDIT_AUTO   // X == -1
} pn_link_credit_mode_t;

struct pn_messenger_t {
  char *name;
  char *certificate;
  char *private_key;
  char *password;
  char *trusted_certificates;
  int timeout;
  bool blocking;
  pn_driver_t *driver;
  int send_threshold;
  pn_link_credit_mode_t credit_mode;
  int credit_batch;  // when LINK_CREDIT_AUTO
  int credit;        // available
  int distributed;   // credit
  int receivers;     // # receiver links
  int draining;      // # links in drain state
  pn_list_t *credited;
  pn_list_t *blocked;
  pn_timestamp_t next_drain;
  uint64_t next_tag;
  pni_store_t *outgoing;
  pni_store_t *incoming;
  pn_list_t *subscriptions;
  pn_subscription_t *incoming_subscription;
  pn_error_t *error;
  pn_transform_t *routes;
  pn_transform_t *rewrites;
  pn_address_t address;
  pn_tracker_t outgoing_tracker;
  pn_tracker_t incoming_tracker;
  pn_string_t *original;
  pn_string_t *rewritten;
  bool worked;
  int connection_error;
};

typedef struct {
  char *host;
  char *port;
  pn_subscription_t *subscription;
  pn_ssl_domain_t *domain;
} pn_listener_ctx_t;

static pn_listener_ctx_t *pn_listener_ctx(pn_listener_t *lnr,
                                          pn_messenger_t *messenger,
                                          const char *scheme,
                                          const char *host,
                                          const char *port)
{
  pn_listener_ctx_t *ctx = (pn_listener_ctx_t *) pn_listener_context(lnr);
  assert(!ctx);
  ctx = (pn_listener_ctx_t *) malloc(sizeof(pn_listener_ctx_t));
  ctx->domain = pn_ssl_domain(PN_SSL_MODE_SERVER);
  if (messenger->certificate) {
    int err = pn_ssl_domain_set_credentials(ctx->domain, messenger->certificate,
                                            messenger->private_key,
                                            messenger->password);

    if (err) {
      pn_error_format(messenger->error, PN_ERR, "invalid credentials");
      pn_ssl_domain_free(ctx->domain);
      free(ctx);
      return NULL;
    }
  }

  if (!(scheme && !strcmp(scheme, "amqps"))) {
    pn_ssl_domain_allow_unsecured_client(ctx->domain);
  }

  pn_subscription_t *sub = pn_subscription(messenger, scheme, host, port);
  ctx->subscription = sub;
  ctx->host = pn_strdup(host);
  ctx->port = pn_strdup(port);
  pn_listener_set_context(lnr, ctx);
  return ctx;
}

static void pn_listener_ctx_free(pn_listener_t *lnr)
{
  pn_listener_ctx_t *ctx = (pn_listener_ctx_t *) pn_listener_context(lnr);
  // XXX: subscriptions are freed when the messenger is freed pn_subscription_free(ctx->subscription);
  free(ctx->host);
  free(ctx->port);
  pn_ssl_domain_free(ctx->domain);
  free(ctx);
  pn_listener_set_context(lnr, NULL);
}

typedef struct {
  char *address;
  char *scheme;
  char *user;
  char *pass;
  char *host;
  char *port;
  pn_connector_t *connector;
} pn_connection_ctx_t;

static pn_connection_ctx_t *pn_connection_ctx(pn_connection_t *conn,
                                              pn_connector_t *connector,
                                              const char *scheme,
                                              const char *user,
                                              const char *pass,
                                              const char *host,
                                              const char *port)
{
  pn_connection_ctx_t *ctx = (pn_connection_ctx_t *) pn_connection_get_context(conn);
  assert(!ctx);
  ctx = (pn_connection_ctx_t *) malloc(sizeof(pn_connection_ctx_t));
  ctx->scheme = pn_strdup(scheme);
  ctx->user = pn_strdup(user);
  ctx->pass = pn_strdup(pass);
  ctx->host = pn_strdup(host);
  ctx->port = pn_strdup(port);
  ctx->connector = connector;
  pn_connection_set_context(conn, ctx);
  return ctx;
}

static void pn_connection_ctx_free(pn_connection_t *conn)
{
  pn_connection_ctx_t *ctx = (pn_connection_ctx_t *) pn_connection_get_context(conn);
  free(ctx->scheme);
  free(ctx->user);
  free(ctx->pass);
  free(ctx->host);
  free(ctx->port);
  free(ctx);
  pn_connection_set_context(conn, NULL);
}

#define OUTGOING (0x0000000000000000)
#define INCOMING (0x1000000000000000)

#define pn_tracker(direction, sequence) ((direction) | (sequence))
#define pn_tracker_direction(tracker) ((tracker) & (0x1000000000000000))
#define pn_tracker_sequence(tracker) ((pn_sequence_t) ((tracker) & (0x00000000FFFFFFFF)))

static char *build_name(const char *name)
{
  if (name) {
    return pn_strdup(name);
  } else {
    return pn_i_genuuid();
  }
}

struct pn_link_ctx_t {
  pn_subscription_t *subscription;
};

// compute the maximum amount of credit each receiving link is
// entitled to.  The actual credit given to the link depends on what
// amount of credit is actually available.
static int per_link_credit( pn_messenger_t *messenger )
{
  if (messenger->receivers == 0) return 0;
  int total = messenger->credit + messenger->distributed;
  return pn_max(total/messenger->receivers, 1);
}

static void link_ctx_setup( pn_messenger_t *messenger,
                            pn_connection_t *connection,
                            pn_link_t *link )
{
  if (pn_link_is_receiver(link)) {
    messenger->receivers++;
    pn_link_ctx_t *ctx = (pn_link_ctx_t *) calloc(1, sizeof(pn_link_ctx_t));
    assert( ctx );
    assert( !pn_link_get_context(link) );
    pn_link_set_context( link, ctx );
    pn_list_add(messenger->blocked, link);
  }
}

static void link_ctx_release( pn_messenger_t *messenger, pn_link_t *link )
{
  if (pn_link_is_receiver(link)) {
    assert( messenger->receivers > 0 );
    messenger->receivers--;
    pn_link_ctx_t *ctx = (pn_link_ctx_t *) pn_link_get_context( link );
    assert( ctx );
    if (pn_link_get_drain(link)) {
      pn_link_set_drain(link, false);
      assert( messenger->draining > 0 );
      messenger->draining--;
    }
    pn_list_remove(messenger->credited, link);
    pn_list_remove(messenger->blocked, link);
    pn_link_set_context( link, NULL );
    free( ctx );
  }
}

pn_messenger_t *pn_messenger(const char *name)
{
  pn_messenger_t *m = (pn_messenger_t *) malloc(sizeof(pn_messenger_t));

  if (m) {
    m->name = build_name(name);
    m->certificate = NULL;
    m->private_key = NULL;
    m->password = NULL;
    m->trusted_certificates = NULL;
    m->timeout = -1;
    m->blocking = true;
    m->driver = pn_driver();
    m->credit_mode = LINK_CREDIT_EXPLICIT;
    m->credit_batch = 1024;
    m->credit = 0;
    m->distributed = 0;
    m->receivers = 0;
    m->draining = 0;
    m->credited = pn_list(0, 0);
    m->blocked = pn_list(0, 0);
    m->next_drain = 0;
    m->next_tag = 0;
    m->outgoing = pni_store();
    m->incoming = pni_store();
    m->subscriptions = pn_list(0, PN_REFCOUNT);
    m->incoming_subscription = NULL;
    m->error = pn_error();
    m->routes = pn_transform();
    m->rewrites = pn_transform();
    m->outgoing_tracker = 0;
    m->incoming_tracker = 0;
    m->address.text = pn_string(NULL);
    m->original = pn_string(NULL);
    m->rewritten = pn_string(NULL);
    m->connection_error = 0;
  }

  return m;
}

int pni_messenger_add_subscription(pn_messenger_t *messenger, pn_subscription_t *subscription)
{
  return pn_list_add(messenger->subscriptions, subscription);
}


const char *pn_messenger_name(pn_messenger_t *messenger)
{
  return messenger->name;
}

int pn_messenger_set_certificate(pn_messenger_t *messenger, const char *certificate)
{
  if (messenger->certificate) free(messenger->certificate);
  messenger->certificate = pn_strdup(certificate);
  return 0;
}

const char *pn_messenger_get_certificate(pn_messenger_t *messenger)
{
  return messenger->certificate;
}

int pn_messenger_set_private_key(pn_messenger_t *messenger, const char *private_key)
{
  if (messenger->private_key) free(messenger->private_key);
  messenger->private_key = pn_strdup(private_key);
  return 0;
}

const char *pn_messenger_get_private_key(pn_messenger_t *messenger)
{
  return messenger->private_key;
}

int pn_messenger_set_password(pn_messenger_t *messenger, const char *password)
{
  if (messenger->password) free(messenger->password);
  messenger->password = pn_strdup(password);
  return 0;
}

const char *pn_messenger_get_password(pn_messenger_t *messenger)
{
  return messenger->password;
}

int pn_messenger_set_trusted_certificates(pn_messenger_t *messenger, const char *trusted_certificates)
{
  if (messenger->trusted_certificates) free(messenger->trusted_certificates);
  messenger->trusted_certificates = pn_strdup(trusted_certificates);
  return 0;
}

const char *pn_messenger_get_trusted_certificates(pn_messenger_t *messenger)
{
  return messenger->trusted_certificates;
}

int pn_messenger_set_timeout(pn_messenger_t *messenger, int timeout)
{
  if (!messenger) return PN_ARG_ERR;
  messenger->timeout = timeout;
  return 0;
}

int pn_messenger_get_timeout(pn_messenger_t *messenger)
{
  return messenger ? messenger->timeout : 0;
}

bool pn_messenger_is_blocking(pn_messenger_t *messenger)
{
  assert(messenger);
  return messenger->blocking;
}

int pn_messenger_set_blocking(pn_messenger_t *messenger, bool blocking)
{
  messenger->blocking = blocking;
  return 0;
}

static void pni_messenger_reclaim(pn_messenger_t *messenger, pn_connection_t *conn);

static void pni_driver_reclaim(pn_messenger_t *messenger, pn_driver_t *driver)
{
  pn_listener_t *l = pn_listener_head(driver);
  while (l) {
    pn_listener_ctx_free(l);
    l = pn_listener_next(l);
  }

  pn_connector_t *c = pn_connector_head(driver);
  while (c) {
    pn_connection_t *conn = pn_connector_connection(c);
    pni_messenger_reclaim(messenger, conn);
    c = pn_connector_next(c);
  }
}

void pn_messenger_free(pn_messenger_t *messenger)
{
  if (messenger) {
    pn_free(messenger->rewritten);
    pn_free(messenger->original);
    pn_free(messenger->address.text);
    free(messenger->name);
    free(messenger->certificate);
    free(messenger->private_key);
    free(messenger->password);
    free(messenger->trusted_certificates);
    pni_driver_reclaim(messenger, messenger->driver);
    pn_driver_free(messenger->driver);
    pn_error_free(messenger->error);
    pni_store_free(messenger->incoming);
    pni_store_free(messenger->outgoing);
    pn_free(messenger->subscriptions);
    pn_free(messenger->rewrites);
    pn_free(messenger->routes);
    pn_free(messenger->credited);
    pn_free(messenger->blocked);
    free(messenger);
  }
}

int pn_messenger_errno(pn_messenger_t *messenger)
{
  if (messenger) {
    return pn_error_code(messenger->error);
  } else {
    return PN_ARG_ERR;
  }
}

pn_error_t *pn_messenger_error(pn_messenger_t *messenger)
{
  assert(messenger);
  return messenger->error;
}

// Run the credit scheduler, grant flow as needed.  Return True if
// credit allocation for any link has changed.
bool pn_messenger_flow(pn_messenger_t *messenger)
{
  bool updated = false;
  if (messenger->receivers == 0) return updated;

  if (messenger->credit_mode == LINK_CREDIT_AUTO) {
    // replenish, but limit the max total messages buffered
    const int max = messenger->receivers * messenger->credit_batch;
    const int used = messenger->distributed + pn_messenger_incoming(messenger);
    if (max > used)
      messenger->credit = max - used;
  }

  // account for any credit left over after draining links has completed
  if (messenger->draining > 0) {
    for (size_t i = 0; i < pn_list_size(messenger->credited); i++) {
      pn_link_t *link = (pn_link_t *) pn_list_get(messenger->credited, i);
      if (pn_link_get_drain(link)) {
        if (!pn_link_draining(link)) {
          // drain completed!
          int drained = pn_link_drained(link);
          //          printf("%s: drained %i from %p\n", messenger->name, drained, (void *) ctx->link);
          messenger->distributed -= drained;
          messenger->credit += drained;
          pn_link_set_drain(link, false);
          messenger->draining--;
          pn_list_remove(messenger->credited, link);
          pn_list_add(messenger->blocked, link);
        }
      }
    }
  }

  const int batch = per_link_credit(messenger);
  while (messenger->credit > 0 && pn_list_size(messenger->blocked)) {
    pn_link_t *link = (pn_link_t *) pn_list_get(messenger->blocked, 0);
    pn_list_del(messenger->blocked, 0, 1);

    const int more = pn_min( messenger->credit, batch );
    messenger->distributed += more;
    messenger->credit -= more;
    //    printf("%s: flowing %i to %p\n", messenger->name, more, (void *) ctx->link);
    pn_link_flow(link, more);
    pn_list_add(messenger->credited, link);
    pn_connection_t *conn = pn_session_connection(pn_link_session(link));
    pn_connection_ctx_t *cctx;
    cctx = (pn_connection_ctx_t *)pn_connection_get_context(conn);
    // flow changed, must process it
    pn_connector_process( cctx->connector );
    updated = true;
  }

  if (!pn_list_size(messenger->blocked)) {
    messenger->next_drain = 0;
  } else {
    // not enough credit for all links
    if (!messenger->draining) {
      //      printf("%s: let's drain\n", messenger->name);
      if (messenger->next_drain == 0) {
        messenger->next_drain = pn_i_now() + 250;
        //        printf("%s: initializing next_drain\n", messenger->name);
      } else if (messenger->next_drain <= pn_i_now()) {
        // initiate drain, free up at most enough to satisfy blocked
        messenger->next_drain = 0;
        int needed = pn_list_size(messenger->blocked) * batch;
        for (size_t i = 0; i < pn_list_size(messenger->credited); i++) {
          pn_link_t *link = (pn_link_t *) pn_list_get(messenger->credited, i);
          if (!pn_link_get_drain(link)) {
            //            printf("%s: initiating drain from %p\n", messenger->name, (void *) ctx->link);
            pn_link_set_drain(link, true);
            needed -= pn_link_remote_credit(link);
            messenger->draining++;
            pn_connection_t *conn =
              pn_session_connection(pn_link_session(link));
            pn_connection_ctx_t *cctx;
            cctx = (pn_connection_ctx_t *)pn_connection_get_context(conn);
            // drain requested on link, must process it
            pn_connector_process( cctx->connector );
            updated = true;
          }

          if (needed <= 0) {
            break;
          }
        }
      } else {
        //        printf("%s: delaying\n", messenger->name);
      }
    }
  }
  return updated;
}

static void pn_error_report(const char *pfx, const char *error)
{
  fprintf(stderr, "%s ERROR %s\n", pfx, error);
}

static int pn_transport_config(pn_messenger_t *messenger,
                               pn_connector_t *connector,
                               pn_connection_t *connection)
{
  pn_connection_ctx_t *ctx = (pn_connection_ctx_t *) pn_connection_get_context(connection);
  pn_transport_t *transport = pn_connector_transport(connector);
  if (ctx->scheme && !strcmp(ctx->scheme, "amqps")) {
    pn_ssl_domain_t *d = pn_ssl_domain(PN_SSL_MODE_CLIENT);
    if (messenger->certificate && messenger->private_key) {
      int err = pn_ssl_domain_set_credentials( d, messenger->certificate,
                                               messenger->private_key,
                                               messenger->password);
      if (err) {
        pn_error_report("CONNECTION", "invalid credentials");
        return err;
      }
    }
    if (messenger->trusted_certificates) {
      int err = pn_ssl_domain_set_trusted_ca_db(d, messenger->trusted_certificates);
      if (err) {
        pn_error_report("CONNECTION", "invalid certificate db");
        return err;
      }
      err = pn_ssl_domain_set_peer_authentication(d, PN_SSL_VERIFY_PEER_NAME, NULL);
      if (err) {
        pn_error_report("CONNECTION", "error configuring ssl to verify peer");
      }
    } else {
      int err = pn_ssl_domain_set_peer_authentication(d, PN_SSL_ANONYMOUS_PEER, NULL);
      if (err) {
        pn_error_report("CONNECTION", "error configuring ssl for anonymous peer");
        return err;
      }
    }
    pn_ssl_t *ssl = pn_ssl(transport);
    pn_ssl_init(ssl, d, NULL);
    pn_ssl_set_peer_hostname(ssl, pn_connection_get_hostname(connection));
    pn_ssl_domain_free( d );
  }

  pn_sasl_t *sasl = pn_sasl(transport);
  if (ctx->user) {
    pn_sasl_plain(sasl, ctx->user, ctx->pass);
  } else {
    pn_sasl_mechanisms(sasl, "ANONYMOUS");
    pn_sasl_client(sasl);
  }

  return 0;
}

static void pn_condition_report(const char *pfx, pn_condition_t *condition)
{
  if (pn_condition_is_redirect(condition)) {
    fprintf(stderr, "%s NOTICE (%s) redirecting to %s:%i\n",
            pfx,
            pn_condition_get_name(condition),
            pn_condition_redirect_host(condition),
            pn_condition_redirect_port(condition));
  } else if (pn_condition_is_set(condition)) {
    char error[1024];
    snprintf(error, 1024, "(%s) %s",
             pn_condition_get_name(condition),
             pn_condition_get_description(condition));
    pn_error_report(pfx, error);
  }
}

int pni_pump_in(pn_messenger_t *messenger, const char *address, pn_link_t *receiver)
{
  pn_delivery_t *d = pn_link_current(receiver);
  if (!pn_delivery_readable(d) && !pn_delivery_partial(d)) {
    return 0;
  }

  pni_entry_t *entry = pni_store_put(messenger->incoming, address);
  pn_buffer_t *buf = pni_entry_bytes(entry);
  pni_entry_set_delivery(entry, d);

  pn_link_ctx_t *ctx = (pn_link_ctx_t *) pn_link_get_context( receiver );
  pni_entry_set_context(entry, ctx ? ctx->subscription : NULL);

  size_t pending = pn_delivery_pending(d);
  int err = pn_buffer_ensure(buf, pending + 1);
  if (err) return pn_error_format(messenger->error, err, "get: error growing buffer");
  char *encoded = pn_buffer_bytes(buf).start;
  ssize_t n = pn_link_recv(receiver, encoded, pending);
  if (n != (ssize_t) pending) {
    return pn_error_format(messenger->error, n,
                           "didn't receive pending bytes: %" PN_ZI " %" PN_ZI,
                           n, pending);
  }
  n = pn_link_recv(receiver, encoded + pending, 1);
  pn_link_advance(receiver);

  // account for the used credit
  assert( ctx );
  assert( messenger->distributed );
  messenger->distributed--;

  pn_link_t *link = receiver;
  // replenish if low (< 20% maximum batch) and credit available
  if (!pn_link_get_drain(link) && pn_list_size(messenger->blocked) == 0 && messenger->credit > 0) {
    const int max = per_link_credit(messenger);
    const int lo_thresh = (int)(max * 0.2 + 0.5);
    if (pn_link_remote_credit(link) < lo_thresh) {
      const int more = pn_min(messenger->credit, max - pn_link_remote_credit(link));
      messenger->credit -= more;
      messenger->distributed += more;
      pn_link_flow(link, more);
    }
  }
  // check if blocked
  if (pn_list_index(messenger->blocked, link) < 0 && pn_link_remote_credit(link) == 0) {
    pn_list_remove(messenger->credited, link);
    if (pn_link_get_drain(link)) {
      pn_link_set_drain(link, false);
      assert( messenger->draining > 0 );
      messenger->draining--;
    }
    pn_list_add(messenger->blocked, link);
  }

  if (n != PN_EOS) {
    return pn_error_format(messenger->error, n, "PN_EOS expected");
  }
  pn_buffer_append(buf, encoded, pending); // XXX

  return 0;
}

void pni_messenger_reclaim_link(pn_messenger_t *messenger, pn_link_t *link)
{
  if (pn_link_is_receiver(link) && pn_link_credit(link) > 0) {
    int credit = pn_link_credit(link);
    messenger->credit += credit;
    messenger->distributed -= credit;
  }

  pn_delivery_t *d = pn_unsettled_head(link);
  while (d) {
    pni_entry_t *e = (pni_entry_t *) pn_delivery_get_context(d);
    if (e) {
      pni_entry_set_delivery(e, NULL);
      if (pn_delivery_buffered(d)) {
        pni_entry_set_status(e, PN_STATUS_ABORTED);
      }
    }
    d = pn_unsettled_next(d);
  }

  link_ctx_release(messenger, link);
}

int pni_pump_out(pn_messenger_t *messenger, const char *address, pn_link_t *sender);

void pn_messenger_endpoints(pn_messenger_t *messenger, pn_connection_t *conn, pn_connector_t *ctor)
{
  if (pn_connection_state(conn) & PN_LOCAL_UNINIT) {
    pn_connection_open(conn);
  }

  pn_delivery_t *d = pn_work_head(conn);
  while (d) {
    pn_link_t *link = pn_delivery_link(d);
    if (pn_delivery_updated(d)) {
      if (pn_link_is_sender(link)) {
        pn_delivery_update(d, pn_delivery_remote_state(d));
      }
      pni_entry_t *e = (pni_entry_t *) pn_delivery_get_context(d);
      if (e) pni_entry_updated(e);
    }
    pn_delivery_clear(d);
    if (pn_delivery_readable(d)) {
      int err = pni_pump_in(messenger, pn_terminus_get_address(pn_link_source(link)), link);
      if (err) {
        fprintf(stderr, "%s\n", pn_error_text(messenger->error));
      }
    }
    d = pn_work_next(d);
  }

  if (pn_work_head(conn)) {
    return;
  }

  pn_session_t *ssn = pn_session_head(conn, PN_LOCAL_UNINIT);
  while (ssn) {
    pn_session_open(ssn);
    ssn = pn_session_next(ssn, PN_LOCAL_UNINIT);
  }

  pn_link_t *link = pn_link_head(conn, PN_LOCAL_UNINIT);
  while (link) {
    pn_terminus_copy(pn_link_source(link), pn_link_remote_source(link));
    pn_terminus_copy(pn_link_target(link), pn_link_remote_target(link));
    link_ctx_setup( messenger, conn, link );
    pn_link_open(link);
    if (pn_link_is_receiver(link)) {
      pn_listener_t *listener = pn_connector_listener(ctor);
      pn_listener_ctx_t *ctx = (pn_listener_ctx_t *) pn_listener_context(listener);
      ((pn_link_ctx_t *)pn_link_get_context(link))->subscription = ctx ? ctx->subscription : NULL;
    }
    link = pn_link_next(link, PN_LOCAL_UNINIT);
  }

  link = pn_link_head(conn, PN_LOCAL_ACTIVE | PN_REMOTE_ACTIVE);
  while (link) {
    if (pn_link_is_sender(link)) {
      pni_pump_out(messenger, pn_terminus_get_address(pn_link_target(link)), link);
    } else {
      pn_link_ctx_t *ctx = (pn_link_ctx_t *) pn_link_get_context(link);
      if (ctx) {
        const char *addr = pn_terminus_get_address(pn_link_remote_source(link));
        if (ctx->subscription) {
          pni_subscription_set_address(ctx->subscription, addr);
        }
      }
    }
    link = pn_link_next(link, PN_LOCAL_ACTIVE | PN_REMOTE_ACTIVE);
  }

  ssn = pn_session_head(conn, PN_LOCAL_ACTIVE | PN_REMOTE_CLOSED);
  while (ssn) {
    pn_condition_report("SESSION", pn_session_remote_condition(ssn));
    pn_session_close(ssn);
    ssn = pn_session_next(ssn, PN_LOCAL_ACTIVE | PN_REMOTE_CLOSED);
  }

  link = pn_link_head(conn, PN_REMOTE_CLOSED);
  while (link) {
    if (PN_LOCAL_ACTIVE & pn_link_state(link)) {
      pn_condition_report("LINK", pn_link_remote_condition(link));
      pn_link_close(link);
      pni_messenger_reclaim_link(messenger, link);
      pn_link_free(link);
    }
    link = pn_link_next(link, PN_REMOTE_CLOSED);
  }

  if (pn_connection_state(conn) == (PN_LOCAL_ACTIVE | PN_REMOTE_CLOSED)) {
    pn_condition_t *condition = pn_connection_remote_condition(conn);
    pn_condition_report("CONNECTION", condition);
    pn_connection_close(conn);
    if (pn_condition_is_redirect(condition)) {
      const char *host = pn_condition_redirect_host(condition);
      char buf[1024];
      sprintf(buf, "%i", pn_condition_redirect_port(condition));

      pn_connector_process(ctor);
      pn_connector_set_connection(ctor, NULL);
      pn_driver_t *driver = messenger->driver;
      pn_connector_t *connector = pn_connector(driver, host, buf, NULL);
      pn_transport_unbind(pn_connector_transport(ctor));
      pn_connection_reset(conn);
      pn_transport_config(messenger, connector, conn);
      pn_connector_set_connection(connector, conn);
    }
  } else if (pn_connector_closed(ctor) && !(pn_connection_state(conn) & PN_REMOTE_CLOSED)) {
    pn_error_report("CONNECTION", "connection aborted");
  }

  pn_messenger_flow(messenger);
}

void pni_messenger_reclaim(pn_messenger_t *messenger, pn_connection_t *conn)
{
  if (!conn) return;

  pn_link_t *link = pn_link_head(conn, 0);
  while (link) {
    pni_messenger_reclaim_link(messenger, link);
    link = pn_link_next(link, 0);
  }

  pn_connection_ctx_free(conn);
  pn_connection_free(conn);
}

pn_connection_t *pn_messenger_connection(pn_messenger_t *messenger,
                                         pn_connector_t *connector,
                                         const char *scheme,
                                         char *user,
                                         char *pass,
                                         char *host,
                                         char *port)
{
  pn_connection_t *connection = pn_connection();
  if (!connection) return NULL;
  pn_connection_ctx(connection, connector, scheme, user, pass, host, port);

  pn_connection_set_container(connection, messenger->name);
  pn_connection_set_hostname(connection, host);
  return connection;
}

int pn_messenger_tsync(pn_messenger_t *messenger, bool (*predicate)(pn_messenger_t *), int timeout)
{
  pn_connector_t *ctor = pn_connector_head(messenger->driver);
  while (ctor) {
    pn_connection_t *conn = pn_connector_connection(ctor);
    pn_messenger_endpoints(messenger, conn, ctor);
    pn_connector_process(ctor);
    ctor = pn_connector_next(ctor);
  }

  pn_timestamp_t now = pn_i_now();
  long int deadline = now + timeout;
  bool pred;

  while (true) {
    pred = predicate(messenger);
    int remaining = deadline - now;
    if (pred || (timeout >= 0 && remaining < 0)) break;

    // Update the credit scheduler. If the scheduler detects credit
    // imbalance on the links, wake up in time to service credit drain
    pn_messenger_flow(messenger);
    if (messenger->next_drain) {
      if (now >= messenger->next_drain)
        remaining = 0;
      else {
        const int delay = messenger->next_drain - now;
        remaining = (remaining < 0) ? delay : pn_min( remaining, delay );
      }
    }
    int error = pn_driver_wait(messenger->driver, remaining);
    if (error && error != PN_INTR) return error;

    pn_listener_t *l;
    while ((l = pn_driver_listener(messenger->driver))) {
      messenger->worked = true;
      pn_listener_ctx_t *ctx = (pn_listener_ctx_t *) pn_listener_context(l);
      pn_subscription_t *sub = ctx->subscription;
      const char *scheme = pn_subscription_scheme(sub);
      pn_connector_t *c = pn_listener_accept(l);
      pn_transport_t *t = pn_connector_transport(c);

      pn_ssl_t *ssl = pn_ssl(t);
      pn_ssl_init(ssl, ctx->domain, NULL);

      pn_sasl_t *sasl = pn_sasl(t);
      pn_sasl_mechanisms(sasl, "ANONYMOUS");
      pn_sasl_server(sasl);
      pn_sasl_done(sasl, PN_SASL_OK);
      pn_connection_t *conn =
        pn_messenger_connection(messenger, c, scheme, NULL, NULL, NULL, NULL);
      pn_connector_set_connection(c, conn);
    }

    pn_connector_t *c;
    while ((c = pn_driver_connector(messenger->driver))) {
      messenger->worked = true;
      pn_connector_process(c);
      pn_connection_t *conn = pn_connector_connection(c);
      pn_messenger_endpoints(messenger, conn, c);
      if (pn_connector_closed(c)) {
        pn_connector_free(c);
        if (conn) {
          pni_messenger_reclaim(messenger, conn);
        }
      } else {
        pn_connector_process(c);
      }
    }

    if (timeout >= 0) {
      now = pn_i_now();
    }

    if (error == PN_INTR) {
      return pred ? 0 : PN_INTR;
    }
  }

  return pred ? 0 : PN_TIMEOUT;
}

int pn_messenger_sync(pn_messenger_t *messenger, bool (*predicate)(pn_messenger_t *))
{
  if (messenger->blocking) {
    return pn_messenger_tsync(messenger, predicate, messenger->timeout);
  } else {
    int err = pn_messenger_tsync(messenger, predicate, 0);
    if (err == PN_TIMEOUT) {
      return PN_INPROGRESS;
    } else {
      return err;
    }
  }
}

int pn_messenger_start(pn_messenger_t *messenger)
{
  if (!messenger) return PN_ARG_ERR;
  // right now this is a noop
  return 0;
}

bool pn_messenger_stopped(pn_messenger_t *messenger)
{
  return pn_connector_head(messenger->driver) == NULL;
}

int pn_messenger_stop(pn_messenger_t *messenger)
{
  if (!messenger) return PN_ARG_ERR;

  pn_connector_t *ctor = pn_connector_head(messenger->driver);
  while (ctor) {
    pn_connection_t *conn = pn_connector_connection(ctor);
    pn_link_t *link = pn_link_head(conn, PN_LOCAL_ACTIVE);
    while (link) {
      pn_link_close(link);
      link = pn_link_next(link, PN_LOCAL_ACTIVE);
    }
    pn_connection_close(conn);
    ctor = pn_connector_next(ctor);
  }

  pn_listener_t *l = pn_listener_head(messenger->driver);
  while (l) {
    pn_listener_close(l);
    pn_listener_t *prev = l;
    l = pn_listener_next(l);
    pn_listener_ctx_free(prev);
    pn_listener_close(prev);
    pn_listener_free(prev);
  }

  return pn_messenger_sync(messenger, pn_messenger_stopped);
}

static bool pn_streq(const char *a, const char *b)
{
  return a == b || (a && b && !strcmp(a, b));
}

static const char *default_port(const char *scheme)
{
  if (scheme && pn_streq(scheme, "amqps"))
    return "5671";
  else
    return "5672";
}

static void pni_parse(pn_address_t *address)
{
  address->passive = false;
  address->scheme = NULL;
  address->user = NULL;
  address->pass = NULL;
  address->host = NULL;
  address->port = NULL;
  address->name = NULL;
  parse_url(pn_string_buffer(address->text), &address->scheme, &address->user,
            &address->pass, &address->host, &address->port, &address->name);
  if (address->host[0] == '~') {
    address->passive = true;
    address->host++;
  }
}

static int pni_route(pn_messenger_t *messenger, const char *address)
{
  pn_address_t *addr = &messenger->address;
  int err = pn_transform_apply(messenger->routes, address, addr->text);
  if (err) return pn_error_format(messenger->error, PN_ERR,
                                  "transformation error");
  pni_parse(addr);
  return 0;
}

pn_connection_t *pn_messenger_resolve(pn_messenger_t *messenger, const char *address, char **name)
{
  assert(messenger);
  messenger->connection_error = 0;
  char domain[1024];
  if (address && sizeof(domain) < strlen(address) + 1) {
    pn_error_format(messenger->error, PN_ERR,
                    "address exceeded maximum length: %s", address);
    return NULL;
  }
  int err = pni_route(messenger, address);
  if (err) return NULL;

  bool passive = messenger->address.passive;
  char *scheme = messenger->address.scheme;
  char *user = messenger->address.user;
  char *pass = messenger->address.pass;
  char *host = messenger->address.host;
  char *port = messenger->address.port;
  *name = messenger->address.name;

  if (passive) {
    pn_listener_t *lnr = pn_listener_head(messenger->driver);
    while (lnr) {
      pn_listener_ctx_t *ctx = (pn_listener_ctx_t *) pn_listener_context(lnr);
      if (pn_streq(host, ctx->host) && pn_streq(port, ctx->port)) {
        return NULL;
      }
      lnr = pn_listener_next(lnr);
    }

    lnr = pn_listener(messenger->driver, host, port ? port : default_port(scheme), NULL);
    if (lnr) {
      pn_listener_ctx_t *ctx = pn_listener_ctx(lnr, messenger, scheme, host, port);
      if (!ctx) {
        pn_listener_close(lnr);
        pn_listener_free(lnr);
      }
    } else {
      pn_error_format(messenger->error, PN_ERR,
                      "unable to bind to address %s: %s:%s", address, host, port,
                      pn_driver_error(messenger->driver));
    }

    return NULL;
  }

  domain[0] = '\0';

  if (user) {
    strcat(domain, user);
    strcat(domain, "@");
  }
  strcat(domain, host);
  if (port) {
    strcat(domain, ":");
    strcat(domain, port);
  }

  pn_connector_t *ctor = pn_connector_head(messenger->driver);

  while (ctor) {
    pn_connection_t *connection = pn_connector_connection(ctor);

    pn_connection_ctx_t *ctx = (pn_connection_ctx_t *) pn_connection_get_context(connection);
    if (pn_streq(scheme, ctx->scheme) && pn_streq(user, ctx->user) &&
        pn_streq(pass, ctx->pass) && pn_streq(host, ctx->host) &&
        pn_streq(port, ctx->port)) {
      return connection;
    }

    const char *container = pn_connection_remote_container(connection);
    if (pn_streq(container, domain)) {
      return connection;
    }

    ctor = pn_connector_next(ctor);
  }

  pn_connector_t *connector = pn_connector(messenger->driver, host,
                                           port ? port : default_port(scheme),
                                           NULL);

  if (!connector) {
    pn_error_format(messenger->error, PN_ERR,
                    "unable to connect to %s: %s", address,
                    pn_driver_error(messenger->driver));

    return NULL;
  }

  pn_connection_t *connection =
    pn_messenger_connection(messenger, connector, scheme, user, pass, host, port);
  err = pn_transport_config(messenger, connector, connection);
  if (err) {
    pni_messenger_reclaim(messenger, connection);
    pn_connector_close(connector);
    pn_connector_free(connector);
    messenger->connection_error = err;
    return NULL;
  }

  pn_connection_open(connection);

  pn_connector_set_connection(connector, connection);

  return connection;
}

pn_link_t *pn_messenger_link(pn_messenger_t *messenger, const char *address, bool sender)
{
  char *name = NULL;

  pn_connection_t *connection = pn_messenger_resolve(messenger, address, &name);
  if (!connection) return NULL;
  pn_connection_ctx_t *cctx = (pn_connection_ctx_t *) pn_connection_get_context(connection);

  pn_link_t *link = pn_link_head(connection, PN_LOCAL_ACTIVE);
  while (link) {
    if (pn_link_is_sender(link) == sender) {
      const char *terminus = pn_link_is_sender(link) ?
        pn_terminus_get_address(pn_link_target(link)) :
        pn_terminus_get_address(pn_link_source(link));
      if (pn_streq(name, terminus))
        return link;
    }
    link = pn_link_next(link, PN_LOCAL_ACTIVE);
  }

  pn_session_t *ssn = pn_session(connection);

  pn_session_open(ssn);
  link = sender ? pn_sender(ssn, "sender-xxx") : pn_receiver(ssn, "receiver-xxx");
  if ((sender && pn_messenger_get_outgoing_window(messenger)) ||
      (!sender && pn_messenger_get_incoming_window(messenger))) {
    // use explicit settlement via dispositions (not pre-settled)
    pn_link_set_snd_settle_mode( link, PN_SND_UNSETTLED );
    pn_link_set_rcv_settle_mode( link, PN_RCV_SECOND );
  }

  // XXX
  if (pn_streq(name, "#")) {
    if (pn_link_is_sender(link)) {
      pn_terminus_set_dynamic(pn_link_target(link), true);
    } else {
      pn_terminus_set_dynamic(pn_link_source(link), true);
    }
  } else {
    pn_terminus_set_address(pn_link_target(link), name);
    pn_terminus_set_address(pn_link_source(link), name);
  }
  link_ctx_setup( messenger, connection, link );
  if (!sender) {
    pn_link_ctx_t *ctx = (pn_link_ctx_t *)pn_link_get_context(link);
    assert( ctx );
    ctx->subscription = pn_subscription(messenger, cctx->scheme, cctx->host,
                                        cctx->port);
  }

  pn_link_open(link);
  return link;
}

pn_link_t *pn_messenger_source(pn_messenger_t *messenger, const char *source)
{
  return pn_messenger_link(messenger, source, false);
}

pn_link_t *pn_messenger_target(pn_messenger_t *messenger, const char *target)
{
  return pn_messenger_link(messenger, target, true);
}

pn_subscription_t *pn_messenger_subscribe(pn_messenger_t *messenger, const char *source)
{
  pni_route(messenger, source);
  if (pn_error_code(messenger->error)) return NULL;

  bool passive = messenger->address.passive;
  char *scheme = messenger->address.scheme;
  char *host = messenger->address.host;
  char *port = messenger->address.port;

  if (passive) {
    pn_listener_t *lnr = pn_listener(messenger->driver, host,
                                     port ? port : default_port(scheme), NULL);
    if (lnr) {
      pn_listener_ctx_t *ctx = pn_listener_ctx(lnr, messenger, scheme, host, port);
      if (ctx) {
        return ctx->subscription;
      } else {
        pn_listener_close(lnr);
        pn_listener_free(lnr);
        return NULL;
      }
    } else {
      pn_error_format(messenger->error, PN_ERR,
                      "unable to subscribe to address %s: %s", source,
                      pn_driver_error(messenger->driver));
      return NULL;
    }
  } else {
    pn_link_t *src = pn_messenger_source(messenger, source);
    if (!src) return NULL;
    pn_link_ctx_t *ctx = (pn_link_ctx_t *) pn_link_get_context( src );
    return ctx ? ctx->subscription : NULL;
  }
}

int pn_messenger_get_outgoing_window(pn_messenger_t *messenger)
{
  return pni_store_get_window(messenger->outgoing);
}

int pn_messenger_set_outgoing_window(pn_messenger_t *messenger, int window)
{
  pni_store_set_window(messenger->outgoing, window);
  return 0;
}

int pn_messenger_get_incoming_window(pn_messenger_t *messenger)
{
  return pni_store_get_window(messenger->incoming);
}

int pn_messenger_set_incoming_window(pn_messenger_t *messenger, int window)
{
  pni_store_set_window(messenger->incoming, window);
  return 0;
}

static void outward_munge(pn_messenger_t *mng, pn_message_t *msg)
{
  char stackbuf[256];
  char *heapbuf = NULL;
  char *buf = stackbuf;
  const char *address = pn_message_get_reply_to(msg);
  int len = address ? strlen(address) : 0;
  if (len > 1 && address[0] == '~' && address[1] == '/') {
    unsigned needed = len + strlen(mng->name) + 9;
    if (needed > sizeof(stackbuf)) {
      heapbuf = (char *) malloc(needed);
      buf = heapbuf;
    }
    sprintf(buf, "amqp://%s/%s", mng->name, address + 2);
    pn_message_set_reply_to(msg, buf);
  } else if (len == 1 && address[0] == '~') {
    unsigned needed = strlen(mng->name) + 8;
    if (needed > sizeof(stackbuf)) {
      heapbuf = (char *) malloc(needed);
      buf = heapbuf;
    }
    sprintf(buf, "amqp://%s", mng->name);
    pn_message_set_reply_to(msg, buf);
  }
  if (heapbuf) free (heapbuf);
}

int pni_bump_out(pn_messenger_t *messenger, const char *address)
{
  pni_entry_t *entry = pni_store_get(messenger->outgoing, address);
  if (!entry) return 0;

  pni_entry_set_status(entry, PN_STATUS_ABORTED);
  pni_entry_free(entry);
  return 0;
}

int pni_pump_out(pn_messenger_t *messenger, const char *address, pn_link_t *sender)
{
  pni_entry_t *entry = pni_store_get(messenger->outgoing, address);
  if (!entry) {
    pn_link_drained(sender);
    return 0;
  }

  pn_buffer_t *buf = pni_entry_bytes(entry);
  pn_bytes_t bytes = pn_buffer_bytes(buf);
  char *encoded = bytes.start;
  size_t size = bytes.size;

  // XXX: proper tag
  char tag[8];
  void *ptr = &tag;
  uint64_t next = messenger->next_tag++;
  *((uint64_t *) ptr) = next;
  pn_delivery_t *d = pn_delivery(sender, pn_dtag(tag, 8));
  pni_entry_set_delivery(entry, d);
  ssize_t n = pn_link_send(sender, encoded, size);
  if (n < 0) {
    pni_entry_free(entry);
    return pn_error_format(messenger->error, n, "send error: %s",
                           pn_error_text(pn_link_error(sender)));
  } else {
    pn_link_advance(sender);
    pni_entry_free(entry);
    return 0;
  }
}

static void pni_default_rewrite(pn_messenger_t *messenger, const char *address,
                                pn_string_t *dst)
{
  pn_address_t *addr = &messenger->address;
  if (address && strstr(address, "@")) {
    int err = pn_string_set(addr->text, address);
    if (err) assert(false);
    pni_parse(addr);
    if (addr->user || addr->pass)
    {
      pn_string_format(messenger->rewritten, "%s%s%s%s%s%s%s",
                       addr->scheme ? addr->scheme : "",
                       addr->scheme ? "://" : "",
                       addr->host,
                       addr->port ? ":" : "",
                       addr->port ? addr->port : "",
                       addr->name ? "/" : "",
                       addr->name ? addr->name : "");
    }
  }
}

static void pni_rewrite(pn_messenger_t *messenger, pn_message_t *msg)
{
  const char *address = pn_message_get_address(msg);
  pn_string_set(messenger->original, address);

  int err = pn_transform_apply(messenger->rewrites, address,
                               messenger->rewritten);
  if (err) assert(false);
  if (!pn_transform_matched(messenger->rewrites)) {
    pni_default_rewrite(messenger, pn_string_get(messenger->rewritten),
                        messenger->rewritten);
  }
  pn_message_set_address(msg, pn_string_get(messenger->rewritten));
}

static void pni_restore(pn_messenger_t *messenger, pn_message_t *msg)
{
  pn_message_set_address(msg, pn_string_get(messenger->original));
}

int pn_messenger_put(pn_messenger_t *messenger, pn_message_t *msg)
{
  if (!messenger) return PN_ARG_ERR;
  if (!msg) return pn_error_set(messenger->error, PN_ARG_ERR, "null message");
  outward_munge(messenger, msg);
  const char *address = pn_message_get_address(msg);

  pni_entry_t *entry = pni_store_put(messenger->outgoing, address);
  if (!entry)
    return pn_error_format(messenger->error, PN_ERR, "store error");

  messenger->outgoing_tracker = pn_tracker(OUTGOING, pni_entry_track(entry));
  pn_buffer_t *buf = pni_entry_bytes(entry);

  pni_rewrite(messenger, msg);
  while (true) {
    char *encoded = pn_buffer_bytes(buf).start;
    size_t size = pn_buffer_capacity(buf);

    int err = pn_message_encode(msg, encoded, &size);
    if (err == PN_OVERFLOW) {

      err = pn_buffer_ensure(buf, 2*pn_buffer_capacity(buf));
      if (err) {

        pni_entry_free(entry);
        pni_restore(messenger, msg);

        return pn_error_format(messenger->error, err, "put: error growing buffer");
      }
    } else if (err) {

      pni_restore(messenger, msg);
      return pn_error_format(messenger->error, err, "encode error: %s",
                             pn_message_error(msg));
    } else {

      pni_restore(messenger, msg);
      pn_buffer_append(buf, encoded, size); // XXX

      pn_link_t *sender = pn_messenger_target(messenger, address);

      if (!sender) {
        int err = pn_error_code(messenger->error);
        if (err) {
          return err;
        } else if (messenger->connection_error) {
          return pni_bump_out(messenger, address);
        } else {
          return 0;
        }
      } else {

        return pni_pump_out(messenger, address, sender);
      }
    }
  }

  return PN_ERR;
}

pn_tracker_t pn_messenger_outgoing_tracker(pn_messenger_t *messenger)
{
  assert(messenger);
  return messenger->outgoing_tracker;
}

pni_store_t *pn_tracker_store(pn_messenger_t *messenger, pn_tracker_t tracker)
{
  if (pn_tracker_direction(tracker) == OUTGOING) {
    return messenger->outgoing;
  } else {
    return messenger->incoming;
  }
}

pn_status_t pn_messenger_status(pn_messenger_t *messenger, pn_tracker_t tracker)
{
  pni_store_t *store = pn_tracker_store(messenger, tracker);
  pni_entry_t *e = pni_store_entry(store, pn_tracker_sequence(tracker));
  if (e) {
    return pni_entry_get_status(e);
  } else {
    return PN_STATUS_UNKNOWN;
  }
}

bool pn_messenger_buffered(pn_messenger_t *messenger, pn_tracker_t tracker)
{
  pni_store_t *store = pn_tracker_store(messenger, tracker);
  pni_entry_t *e = pni_store_entry(store, pn_tracker_sequence(tracker));
  if (e) {
    pn_delivery_t *d = pni_entry_get_delivery(e);
    if (d) {
      bool b = pn_delivery_buffered(d);
      return b;
    } else {
      return true;
    }
  } else {
    return false;
  }
}

int pn_messenger_settle(pn_messenger_t *messenger, pn_tracker_t tracker, int flags)
{
  pni_store_t *store = pn_tracker_store(messenger, tracker);
  return pni_store_update(store, pn_tracker_sequence(tracker), PN_STATUS_UNKNOWN, flags, true, true);
}

// true if all pending output has been sent to peer
bool pn_messenger_sent(pn_messenger_t *messenger)
{
  int total = pni_store_size(messenger->outgoing);

  pn_connector_t *ctor = pn_connector_head(messenger->driver);
  while (ctor) {

    // check if transport is done generating output
    pn_transport_t *transport = pn_connector_transport(ctor);
    if (transport) {
      if (!pn_transport_quiesced(transport)) {
        pn_connector_process(ctor);
        return false;
      }
    }

    pn_connection_t *conn = pn_connector_connection(ctor);

    pn_link_t *link = pn_link_head(conn, PN_LOCAL_ACTIVE);
    while (link) {
      if (pn_link_is_sender(link)) {
        total += pn_link_queued(link);

        pn_delivery_t *d = pn_unsettled_head(link);
        while (d) {
          if (!pn_delivery_remote_state(d) && !pn_delivery_settled(d)) {
            total++;
          }
          d = pn_unsettled_next(d);
        }
      }
      link = pn_link_next(link, PN_LOCAL_ACTIVE);
    }

    ctor = pn_connector_next(ctor);
  }

  return total <= messenger->send_threshold;
}

bool pn_messenger_rcvd(pn_messenger_t *messenger)
{
  if (pni_store_size(messenger->incoming) > 0) return true;

  pn_connector_t *ctor = pn_connector_head(messenger->driver);
  while (ctor) {
    pn_connection_t *conn = pn_connector_connection(ctor);

    pn_delivery_t *d = pn_work_head(conn);
    while (d) {
      if (pn_delivery_readable(d) && !pn_delivery_partial(d)) {
        return true;
      }
      d = pn_work_next(d);
    }
    ctor = pn_connector_next(ctor);
  }

  if (!pn_connector_head(messenger->driver) && !pn_listener_head(messenger->driver)) {
    return true;
  } else {
    return false;
  }
}

static bool work_pred(pn_messenger_t *messenger) {
  return messenger->worked;
}

int pn_messenger_work(pn_messenger_t *messenger, int timeout)
{
  messenger->worked = false;
  int err = pn_messenger_tsync(messenger, work_pred, timeout);
  if (err) return err;
  return (int) (messenger->worked ? 1 : 0);
}

int pni_messenger_work(pn_messenger_t *messenger)
{
  if (messenger->blocking) {
    return pn_messenger_work(messenger, messenger->timeout);
  } else {
    int err = pn_messenger_work(messenger, 0);
    if (err == PN_TIMEOUT) {
      return PN_INPROGRESS;
    } else {
      return err;
    }
  }
}

int pn_messenger_interrupt(pn_messenger_t *messenger)
{
  assert(messenger);
  if (messenger->driver) {
    return pn_driver_wakeup(messenger->driver);
  } else {
    return 0;
  }
}

int pn_messenger_send(pn_messenger_t *messenger, int n)
{
  if (n == -1) {
    messenger->send_threshold = 0;
  } else {
    messenger->send_threshold = pn_messenger_outgoing(messenger) - n;
    if (messenger->send_threshold < 0)
      messenger->send_threshold = 0;
  }
  return pn_messenger_sync(messenger, pn_messenger_sent);
}

int pn_messenger_recv(pn_messenger_t *messenger, int n)
{

  if (!messenger) return PN_ARG_ERR;
  if (messenger->blocking && !pn_listener_head(messenger->driver)
      && !pn_connector_head(messenger->driver))
    return pn_error_format(messenger->error, PN_STATE_ERR, "no valid sources");

  // re-compute credit, and update credit scheduler
  if (n == -1) {
    messenger->credit_mode = LINK_CREDIT_AUTO;
  } else {
    messenger->credit_mode = LINK_CREDIT_EXPLICIT;
    if (n > messenger->distributed)
      messenger->credit = n - messenger->distributed;
    else  // cancel unallocated
      messenger->credit = 0;
  }

  pn_messenger_flow(messenger);

  int err = pn_messenger_sync(messenger, pn_messenger_rcvd);
  if (err) return err;

  if (!pn_messenger_incoming(messenger) &&
      messenger->blocking &&
      !pn_listener_head(messenger->driver) &&
      !pn_connector_head(messenger->driver)) {

    return pn_error_format(messenger->error, PN_STATE_ERR, "no valid sources");
  } else {

    return 0;
  }
}

int pn_messenger_receiving(pn_messenger_t *messenger)
{
  assert(messenger);
  return messenger->credit + messenger->distributed;
}

int pn_messenger_get(pn_messenger_t *messenger, pn_message_t *msg)
{
  if (!messenger) return PN_ARG_ERR;

  pni_entry_t *entry = pni_store_get(messenger->incoming, NULL);
  // XXX: need to drain credit before returning EOS
  if (!entry) return PN_EOS;

  messenger->incoming_tracker = pn_tracker(INCOMING, pni_entry_track(entry));
  pn_buffer_t *buf = pni_entry_bytes(entry);
  pn_bytes_t bytes = pn_buffer_bytes(buf);
  const char *encoded = bytes.start;
  size_t size = bytes.size;

  messenger->incoming_subscription = (pn_subscription_t *) pni_entry_get_context(entry);

  if (msg) {

    int err = pn_message_decode(msg, encoded, size);

    pni_entry_free(entry);

    if (err) {
      return pn_error_format(messenger->error, err, "error decoding message: %s",
                             pn_message_error(msg));
    } else {

      return 0;
    }
  } else {
    pni_entry_free(entry);
    return 0;
  }
}

pn_tracker_t pn_messenger_incoming_tracker(pn_messenger_t *messenger)
{
  assert(messenger);
  return messenger->incoming_tracker;
}

pn_subscription_t *pn_messenger_incoming_subscription(pn_messenger_t *messenger)
{
  assert(messenger);
  return messenger->incoming_subscription;
}

int pn_messenger_accept(pn_messenger_t *messenger, pn_tracker_t tracker, int flags)
{
  if (pn_tracker_direction(tracker) != INCOMING) {
    return pn_error_format(messenger->error, PN_ARG_ERR,
                           "invalid tracker, incoming tracker required");
  }

  return pni_store_update(messenger->incoming, pn_tracker_sequence(tracker),
                          PN_STATUS_ACCEPTED, flags, false, false);
}

int pn_messenger_reject(pn_messenger_t *messenger, pn_tracker_t tracker, int flags)
{
  if (pn_tracker_direction(tracker) != INCOMING) {
    return pn_error_format(messenger->error, PN_ARG_ERR,
                           "invalid tracker, incoming tracker required");
  }

  return pni_store_update(messenger->incoming, pn_tracker_sequence(tracker),
                          PN_STATUS_REJECTED, flags, false, false);
}

int pn_messenger_queued(pn_messenger_t *messenger, bool sender)
{
  if (!messenger) return 0;

  int result = 0;

  pn_connector_t *ctor = pn_connector_head(messenger->driver);
  while (ctor) {
    pn_connection_t *conn = pn_connector_connection(ctor);

    pn_link_t *link = pn_link_head(conn, PN_LOCAL_ACTIVE);
    while (link) {
      if (pn_link_is_sender(link)) {
        if (sender) {
          result += pn_link_queued(link);
        }
      } else if (!sender) {
        result += pn_link_queued(link);
      }
      link = pn_link_next(link, PN_LOCAL_ACTIVE);
    }
    ctor = pn_connector_next(ctor);
  }

  return result;
}

int pn_messenger_outgoing(pn_messenger_t *messenger)
{
  return pni_store_size(messenger->outgoing) + pn_messenger_queued(messenger, true);
}

int pn_messenger_incoming(pn_messenger_t *messenger)
{
  return pni_store_size(messenger->incoming) + pn_messenger_queued(messenger, false);
}

int pn_messenger_route(pn_messenger_t *messenger, const char *pattern, const char *address)
{
  pn_transform_rule(messenger->routes, pattern, address);
  return 0;
}

int pn_messenger_rewrite(pn_messenger_t *messenger, const char *pattern, const char *address)
{
  pn_transform_rule(messenger->rewrites, pattern, address);
  return 0;
}
