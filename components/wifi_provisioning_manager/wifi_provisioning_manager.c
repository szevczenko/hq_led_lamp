/**
 * @file wifi_provisioning_manager.c
 * @brief Product-owned Wi-Fi provisioning adapter (TASK-126)
 *
 * Implementation of the documented wifi_provisioning_manager.h product
 * contract on top of the platform provisioning application
 * (wifi_http_provisioning.h), the shared Mongoose process
 * (mongoose_process.h) and the platform Wi-Fi manager saved-credentials
 * query (wifi_managment.h).
 *
 * Design rules enforced here:
 *
 *   1. Narrow surface: the ONLY platform headers included are
 *      wifi_http_provisioning.h, mongoose_process.h and wifi_managment.h,
 *      and only the operations the product contract needs are used
 *      (start/stop/get_state, the runtime URL overrides under the
 *      test-only guard, MongooseProcess_IsRunning, wifi_mgmt_is_read_data).
 *      None of these platform types or include paths appear in the public
 *      header (include/wifi_provisioning_manager.h).
 *
 *   2. Secrecy: nothing that could contain an SSID, a password, a token or
 *      a provisioning URL with embedded credentials is ever accepted,
 *      stored or logged here.  The adapter logs state transitions, listener
 *      bind outcomes and error codes only.
 *
 *   3. Mongoose ownership: the portal rides the shared Mongoose process
 *      that also hosts MQTT/TLS.  start() checks MongooseProcess_IsRunning()
 *      up front and fails cleanly when the process is missing, and this
 *      module never calls MongooseProcess_Init()/Deinit() — stop() and
 *      deinit() close only the portal's own listeners and never tear the
 *      shared process down.
 *
 *   4. Serialization: init/deinit/start/stop are serialized by one OSAL
 *      mutex, so concurrent lifecycle calls can never interleave on the
 *      portal state or the test-only URL override buffers.  The adapter
 *      never executes its own logic on the Mongoose poll thread; the
 *      platform start/stop dispatch their listener work to the poll thread
 *      via MongooseProcess_Invoke() and block this CALLER until the poll
 *      thread completes it (the poll thread itself never blocks on the
 *      adapter).
 */

#include "wifi_provisioning_manager.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "mongoose_process.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

/** @brief Maximum length (incl. terminator) of a test-only URL override. */
#define WIFI_PROVISIONING_MANAGER_URL_MAX_LEN 128u

/* --------------------------------------------------------------------- */
/* Internal state                                                          */
/* --------------------------------------------------------------------- */

typedef struct wifi_provisioning_manager_ctx
{
  _Atomic(osal_mutex_id_t) lock;   /**< Published atomically (CAS-adopted). */
  atomic_bool initialized;         /**< init() completed, deinit() cleared. */
#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
  /* Test/host-build-only listen-URL overrides, applied at the next start().
   * Protected by @c lock; never logged. */
  char http_url[WIFI_PROVISIONING_MANAGER_URL_MAX_LEN];
  bool http_url_set;
  char dns_url[WIFI_PROVISIONING_MANAGER_URL_MAX_LEN];
  bool dns_url_set;
#endif
} wifi_provisioning_manager_ctx_t;

static wifi_provisioning_manager_ctx_t s_ctx = {
    .lock        = NULL,
    .initialized = false,
};

/* --------------------------------------------------------------------- */
/* Lock helpers (lazy, race-free publication)                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Create the adapter mutex before concurrent use is possible.
 *
 * The adapter owns no constructor, so the mutex is created lazily — but the
 * creation itself is race-free: an atomic compare-and-swap adopts exactly one
 * winner's mutex, and every loser destroys its duplicate before anyone can
 * observe it.  Once published the handle stays valid for the process lifetime
 * (it is never deleted, matching the network adapter precedent).
 *
 * @return true when the adapter lock is available.
 */
static bool wifi_provisioning_manager_ensure_lock(void)
{
    osal_mutex_id_t current =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    if (current != NULL)
    {
        return true;
    }

    osal_mutex_id_t created = NULL;
    if (osal_mutex_create(&created, "wifi_prov_mgr") != OSAL_SUCCESS)
    {
        return false;
    }

    osal_mutex_id_t expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&s_ctx.lock, &expected,
                                                created,
                                                memory_order_release,
                                                memory_order_acquire))
    {
        /* This thread adopted its mutex; a mutex was published only once. */
        return true;
    }

    /* Another thread won the creation race; drop our duplicate. */
    (void)osal_mutex_delete(created);
    return true;
}

/**
 * @brief Take the adapter lock.
 *
 * @return true when the lock is held and must be released with
 *         wifi_provisioning_manager_unlock(); false means nothing was taken.
 */
static bool wifi_provisioning_manager_lock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    return (lock != NULL) && (osal_mutex_take(lock) == OSAL_SUCCESS);
}

/** @brief Release the adapter lock after a successful
 *         wifi_provisioning_manager_lock(). */
static void wifi_provisioning_manager_unlock(void)
{
    osal_mutex_id_t lock = atomic_load_explicit(&s_ctx.lock,
                                                memory_order_acquire);
    if (lock != NULL)
    {
        (void)osal_mutex_give(lock);
    }
}

/* --------------------------------------------------------------------- */
/* Platform state mapping                                                 */
/* --------------------------------------------------------------------- */

/**
 * @brief Map the platform provisioning state onto the product state type.
 *
 * Explicit switch so the product enum never aliases a platform type through
 * a cast.  Unknown platform values map to ERROR (defensive default).
 */
static wifi_provisioning_manager_state_t
wifi_provisioning_manager_map_state(wifi_http_provisioning_state_t state)
{
    switch (state)
    {
        case WIFI_PROVISIONING_STOPPED:
            return WIFI_PROVISIONING_MANAGER_STOPPED;
        case WIFI_PROVISIONING_STARTING:
            return WIFI_PROVISIONING_MANAGER_STARTING;
        case WIFI_PROVISIONING_RUNNING:
            return WIFI_PROVISIONING_MANAGER_RUNNING;
        case WIFI_PROVISIONING_STOPPING:
            return WIFI_PROVISIONING_MANAGER_STOPPING;
        case WIFI_PROVISIONING_ERROR:
        default:
            return WIFI_PROVISIONING_MANAGER_ERROR;
    }
}

/* --------------------------------------------------------------------- */
/* Adapter lifecycle                                                      */
/* --------------------------------------------------------------------- */

wifi_provisioning_manager_status_t wifi_provisioning_manager_init(void)
{
    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        atomic_store_explicit(&s_ctx.initialized, true, memory_order_release);
        osal_log_info("[prov_mgr] wifi provisioning adapter initialized");
    }

    wifi_provisioning_manager_unlock();
    return WIFI_PROVISIONING_MANAGER_OK;
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_deinit(void)
{
    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    /* Best-effort teardown: if the provisioning portal is (or was) active,
     * close only its own listeners.  The shared Mongoose process is never
     * deinitialized here.  A failed stop is logged but does not block the
     * adapter deinitialization. */
    if (wifi_http_provisioning_get_state() != WIFI_PROVISIONING_STOPPED)
    {
        if (wifi_http_provisioning_stop())
        {
            osal_log_info("[prov_mgr] provisioning portal stopped "
                          "(adapter deinit)");
        }
        else
        {
            osal_log_error("[prov_mgr] provisioning portal stop failed "
                           "during adapter deinit (state=%d)",
                           (int)wifi_http_provisioning_get_state());
        }
    }

    atomic_store_explicit(&s_ctx.initialized, false, memory_order_release);

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
    s_ctx.http_url_set = false;
    s_ctx.dns_url_set  = false;
#endif

    wifi_provisioning_manager_unlock();
    osal_log_info("[prov_mgr] wifi provisioning adapter deinitialized");
    return WIFI_PROVISIONING_MANAGER_OK;
}

/* --------------------------------------------------------------------- */
/* Portal lifecycle                                                       */
/* --------------------------------------------------------------------- */

wifi_provisioning_manager_status_t wifi_provisioning_manager_start(void)
{
    bool platform_ok;

    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] start before init()");
        return WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED;
    }

    /* The portal rides the shared Mongoose process (which also hosts
     * MQTT/TLS).  Require it up front; never initialize or deinitialize it
     * from the adapter. */
    if (!MongooseProcess_IsRunning())
    {
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] provisioning start blocked: shared "
                       "Mongoose process not running");
        return WIFI_PROVISIONING_MANAGER_ERR_MONGOOSE_NOT_RUNNING;
    }

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY
    /* Test/host-build-only: hand the overrides to the platform so tests can
     * bind non-privileged high ports.  On the target build this block is
     * compiled out and the platform uses its compiled-in default listen
     * URLs.  The setter results are intentionally ignored: the platform
     * refuses a URL change while the portal is already running, which only
     * happens on an (idempotent) repeat start. */
    if (s_ctx.http_url_set)
    {
        (void)wifi_http_provisioning_set_http_url(s_ctx.http_url);
    }
    if (s_ctx.dns_url_set)
    {
        (void)wifi_http_provisioning_set_dns_url(s_ctx.dns_url);
    }
#endif

    platform_ok = wifi_http_provisioning_start();
    if (!platform_ok)
    {
        const wifi_http_provisioning_state_t platform_state =
            wifi_http_provisioning_get_state();
        wifi_provisioning_manager_unlock();
        /* Error code only — never the listen URLs. */
        osal_log_error("[prov_mgr] provisioning portal start failed "
                       "(platform_state=%d)",
                       (int)platform_state);
        return WIFI_PROVISIONING_MANAGER_ERR_START_FAILED;
    }

    wifi_provisioning_manager_unlock();
    /* Reached RUNNING: both listeners (HTTP portal + captive DNS) bound. */
    osal_log_info("[prov_mgr] provisioning portal running "
                  "(listeners bound)");
    return WIFI_PROVISIONING_MANAGER_OK;
}

wifi_provisioning_manager_status_t wifi_provisioning_manager_stop(void)
{
    bool platform_ok;

    if (!wifi_provisioning_manager_ensure_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    if (!wifi_provisioning_manager_lock())
    {
        osal_log_error("[prov_mgr] adapter lock unavailable");
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] stop before init()");
        return WIFI_PROVISIONING_MANAGER_ERR_NOT_INITIALIZED;
    }

    /* Closes only the provisioning listeners; the shared Mongoose process
     * (and with it MQTT/TLS) is left untouched. */
    platform_ok = wifi_http_provisioning_stop();
    if (!platform_ok)
    {
        const wifi_http_provisioning_state_t platform_state =
            wifi_http_provisioning_get_state();
        wifi_provisioning_manager_unlock();
        osal_log_error("[prov_mgr] provisioning portal stop failed "
                       "(platform_state=%d)",
                       (int)platform_state);
        return WIFI_PROVISIONING_MANAGER_ERR_STOP_FAILED;
    }

    wifi_provisioning_manager_unlock();
    osal_log_info("[prov_mgr] provisioning portal stopped");
    return WIFI_PROVISIONING_MANAGER_OK;
}

/* --------------------------------------------------------------------- */
/* Queries                                                                */
/* --------------------------------------------------------------------- */

wifi_provisioning_manager_state_t wifi_provisioning_manager_get_state(void)
{
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        return WIFI_PROVISIONING_MANAGER_STOPPED;
    }
    return wifi_provisioning_manager_map_state(
        wifi_http_provisioning_get_state());
}

bool wifi_provisioning_manager_is_active(void)
{
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        return false;
    }
    return wifi_http_provisioning_get_state() == WIFI_PROVISIONING_RUNNING;
}

bool wifi_provisioning_manager_has_saved_credentials(void)
{
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        return false;
    }
    return wifi_mgmt_is_read_data();
}

/* --------------------------------------------------------------------- */
/* Test/host-build-only listen-URL overrides                              */
/* --------------------------------------------------------------------- */

#ifdef WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY

/**
 * @brief Common validation/shared storage for the URL overrides.
 *
 * @param[out] slot  Destination buffer (protected by the adapter lock).
 * @param[out] set   Destination "configured" flag (protected by the lock).
 * @param[in]  url   Candidate URL.
 * @return           true when stored.
 */
static bool wifi_provisioning_manager_store_url(char *slot, bool *set,
                                                const char *url)
{
    size_t len;

    if (url == NULL)
    {
        return false;
    }
    len = strlen(url);
    if ((len == 0U) || (len >= WIFI_PROVISIONING_MANAGER_URL_MAX_LEN))
    {
        return false;
    }

    if (!wifi_provisioning_manager_ensure_lock() ||
        !wifi_provisioning_manager_lock())
    {
        return false;
    }
    if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
    {
        wifi_provisioning_manager_unlock();
        return false;
    }
    memcpy(slot, url, len);
    slot[len] = '\0';
    *set = true;
    wifi_provisioning_manager_unlock();
    return true;
}

bool wifi_provisioning_manager_set_http_url(const char *url)
{
    bool ok = wifi_provisioning_manager_store_url(
        s_ctx.http_url, &s_ctx.http_url_set, url);
    if (!ok)
    {
        osal_log_warning("[prov_mgr] HTTP listen-URL override rejected "
                         "(invalid or adapter not initialized)");
    }
    /* Never log the URL value itself. */
    return ok;
}

bool wifi_provisioning_manager_set_dns_url(const char *url)
{
    bool ok = wifi_provisioning_manager_store_url(
        s_ctx.dns_url, &s_ctx.dns_url_set, url);
    if (!ok)
    {
        osal_log_warning("[prov_mgr] DNS listen-URL override rejected "
                         "(invalid or adapter not initialized)");
    }
    /* Never log the URL value itself. */
    return ok;
}

#endif /* WIFI_PROVISIONING_MANAGER_TEST_OBSERVABILITY */