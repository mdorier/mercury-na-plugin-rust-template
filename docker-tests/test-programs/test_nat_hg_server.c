/**
 * NAT Docker test: HG server that publishes relay circuit address.
 *
 * Unlike test_hg_server which publishes a direct TCP address,
 * this server overwrites the hostfile with the relay circuit address
 * so that clients on other private networks can reach it via relay.
 *
 * Based on test/test_hg_server.c.
 */

#include "mercury_unit.h"

#include <string.h>

/****************/
/* Local Macros */
/****************/

#define HG_TEST_PROGRESS_TIMEOUT 100
#define HG_TEST_TRIGGER_TIMEOUT  HG_MAX_IDLE_TIME

/************************************/
/* Local Type and Struct Definition */
/************************************/

struct hg_test_worker {
    struct hg_thread_work thread_work;
    hg_class_t *hg_class;
    hg_context_t *context;
};

/********************/
/* Local Prototypes */
/********************/

static HG_THREAD_RETURN_TYPE
hg_test_progress_thread(void *arg);
static HG_THREAD_RETURN_TYPE
hg_test_progress_work(void *arg);

/*******************/
/* Local Variables */
/*******************/

/*---------------------------------------------------------------------------*/
static HG_THREAD_RETURN_TYPE
hg_test_progress_thread(void *arg)
{
    hg_context_t *context = (hg_context_t *) arg;
    struct hg_test_context_info *hg_test_context_info =
        (struct hg_test_context_info *) HG_Context_get_data(context);
    hg_thread_ret_t tret = (hg_thread_ret_t) 0;
    hg_return_t ret = HG_SUCCESS;

    do {
        if (hg_atomic_get32(&hg_test_context_info->finalizing))
            break;

        ret = HG_Progress(context, HG_TEST_PROGRESS_TIMEOUT);
    } while (ret == HG_SUCCESS || ret == HG_TIMEOUT);
    HG_TEST_CHECK_ERROR(ret != HG_SUCCESS && ret != HG_TIMEOUT, done, tret,
        (hg_thread_ret_t) 0, "HG_Progress() failed (%s)",
        HG_Error_to_string(ret));

done:
    printf("Exiting\n");
    hg_thread_exit(tret);
    return tret;
}

/*---------------------------------------------------------------------------*/
static HG_THREAD_RETURN_TYPE
hg_test_progress_work(void *arg)
{
    struct hg_test_worker *worker = (struct hg_test_worker *) arg;
    hg_context_t *context = worker->context;
    struct hg_test_context_info *hg_test_context_info =
        (struct hg_test_context_info *) HG_Context_get_data(context);
    hg_thread_ret_t tret = (hg_thread_ret_t) 0;
    hg_return_t ret = HG_SUCCESS;

    do {
        unsigned int actual_count = 0;

        do {
            ret = HG_Trigger(context, 0, 1, &actual_count);
        } while ((ret == HG_SUCCESS) && actual_count);
        HG_TEST_CHECK_ERROR(ret != HG_SUCCESS && ret != HG_TIMEOUT, done, tret,
            (hg_thread_ret_t) 0, "HG_Trigger() failed (%s)",
            HG_Error_to_string(ret));

        if (hg_atomic_get32(&hg_test_context_info->finalizing)) {
            /* Make sure everything was progressed/triggered */
            do {
                ret = HG_Progress(context, 0);
                HG_Trigger(context, 0, 1, &actual_count);
            } while (ret == HG_SUCCESS);
            break;
        }

        /* Use same value as HG_TEST_TRIGGER_TIMEOUT for convenience */
        ret = HG_Progress(context, HG_TEST_TRIGGER_TIMEOUT);
    } while (ret == HG_SUCCESS || ret == HG_TIMEOUT);
    HG_TEST_CHECK_ERROR(ret != HG_SUCCESS && ret != HG_TIMEOUT, done, tret,
        (hg_thread_ret_t) 0, "HG_Progress() failed (%s)",
        HG_Error_to_string(ret));

done:
    return tret;
}

/*---------------------------------------------------------------------------*/
int
main(int argc, char *argv[])
{
    struct hg_unit_info info;
    struct hg_test_worker *progress_workers = NULL;
    struct hg_test_context_info *hg_test_context_info;
    hg_return_t ret;

    /* Force to listen */
    ret = hg_unit_init(argc, argv, true, &info);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "hg_unit_init() failed (%s)", HG_Error_to_string(ret));

    /*
     * Overwrite the hostfile with the relay circuit address.
     *
     * hg_unit_init() already called hg_test_self_addr_publish() which wrote
     * the direct TCP address. For NAT scenarios, clients on other private
     * networks cannot reach the direct address.
     *
     * We construct the relay circuit address from:
     *   - MERCURY_RELAY_ADDR env var (e.g., /ip4/172.20.0.10/tcp/4001/p2p/<relay_id>)
     *   - Self peer ID extracted from HG_Addr_to_string output
     *
     * Result format: relay:libp2p+tcp:<relay_addr>/p2p-circuit/p2p/<self_id>
     */
    {
        hg_addr_t self_addr = HG_ADDR_NULL;
        char addr_str[2048];
        hg_size_t addr_len = sizeof(addr_str);
        char circuit_addr[4096];
        const char *relay_multiaddr;
        const char *self_peer_id;
        na_return_t na_ret;

        /* Get the relay multiaddr from environment */
        relay_multiaddr = getenv("MERCURY_RELAY_ADDR");
        HG_TEST_CHECK_ERROR(relay_multiaddr == NULL, error, ret, HG_FAULT,
            "MERCURY_RELAY_ADDR not set");

        /* Get self address string to extract our peer ID */
        ret = HG_Addr_self(info.hg_class, &self_addr);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Addr_self() failed (%s)", HG_Error_to_string(ret));

        ret = HG_Addr_to_string(
            info.hg_class, addr_str, &addr_len, self_addr);
        HG_TEST_CHECK_HG_ERROR(error, ret, "HG_Addr_to_string() failed (%s)",
            HG_Error_to_string(ret));

        ret = HG_Addr_free(info.hg_class, self_addr);
        HG_TEST_CHECK_HG_ERROR(
            error, ret, "HG_Addr_free() failed (%s)", HG_Error_to_string(ret));

        /* Extract peer ID from self address string.
         * Format: libp2p+tcp:/ip4/.../tcp/.../p2p/<peer_id>
         * Find last "/p2p/" and take everything after it. */
        self_peer_id = strstr(addr_str, "/p2p/");
        HG_TEST_CHECK_ERROR(self_peer_id == NULL, error, ret, HG_FAULT,
            "Could not find /p2p/ in self address: %s", addr_str);
        self_peer_id += 5; /* skip "/p2p/" */

        /* Construct relay circuit address:
         * relay:libp2p+tcp:<relay_multiaddr>/p2p-circuit/p2p/<self_peer_id> */
        snprintf(circuit_addr, sizeof(circuit_addr),
            "relay:libp2p+tcp:%s/p2p-circuit/p2p/%s",
            relay_multiaddr, self_peer_id);
        printf("# NAT server publishing circuit address: %s\n", circuit_addr);

        na_ret = na_test_set_config(
            info.hg_test_info.na_test_info.hostfile, circuit_addr, false);
        HG_TEST_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret,
            (hg_return_t) na_ret, "na_test_set_config() failed (%s)",
            NA_Error_to_string(na_ret));
    }

    HG_TEST_READY_MSG();

    hg_test_context_info =
        (struct hg_test_context_info *) HG_Context_get_data(info.context);

    if (info.hg_test_info.na_test_info.max_contexts > 1) {
        hg_uint8_t context_count =
            (hg_uint8_t) (info.hg_test_info.na_test_info.max_contexts);
        hg_uint8_t i;

        progress_workers =
            malloc(sizeof(struct hg_test_worker) * context_count);
        HG_TEST_CHECK_ERROR_NORET(progress_workers == NULL, error,
            "Could not allocate progress_workers");

        progress_workers[0].thread_work.func = hg_test_progress_work;
        progress_workers[0].thread_work.args = &progress_workers[0];
        progress_workers[0].hg_class = info.hg_class;
        progress_workers[0].context = info.context;

        for (i = 0; i < context_count - 1; i++) {
            progress_workers[i + 1].thread_work.func = hg_test_progress_work;
            progress_workers[i + 1].thread_work.args = &progress_workers[i + 1];
            progress_workers[i + 1].hg_class = info.hg_class;
            progress_workers[i + 1].context = info.secondary_contexts[i];

            hg_thread_pool_post(
                info.thread_pool, &progress_workers[i + 1].thread_work);
        }
        /* Use main thread for progress on main context */
        hg_test_progress_work(&progress_workers[0]);
    } else {
        hg_thread_t progress_thread;

        hg_thread_create(
            &progress_thread, hg_test_progress_thread, info.context);

        do {
            unsigned int trigger_count = 0;
            if (hg_atomic_get32(&hg_test_context_info->finalizing))
                break;

            ret = HG_Trigger(info.context, HG_TEST_TRIGGER_TIMEOUT, 1,
                &trigger_count);
        } while (ret == HG_SUCCESS || ret == HG_TIMEOUT);
        HG_TEST_CHECK_ERROR_NORET(ret != HG_SUCCESS && ret != HG_TIMEOUT, error,
            "HG_Trigger() failed (%s)", HG_Error_to_string(ret));

        hg_thread_join(progress_thread);
    }

    hg_unit_cleanup(&info);
    free(progress_workers);

    return EXIT_SUCCESS;

error:
    hg_unit_cleanup(&info);
    free(progress_workers);

    return EXIT_FAILURE;
}
