/**
 * NAT Docker test: HG client with DCUtR verification.
 *
 * For Scenario 2 (DCUtR success). Based on test_rpc.c but with
 * signal-file coordination:
 *
 * 1. hg_unit_init() reads hostfile, does HG_Addr_lookup2() which triggers
 *    NA_Addr_lookup with relay: prefix -> blocks for DCUtR
 * 2. Write signal file /shared/dcutr_lookup_done
 * 3. Wait for signal file /shared/relay_killed (up to 15s)
 * 4. Run RPC tests (proving direct connection works without relay)
 * 5. hg_unit_cleanup() sends finalize to server
 */

#include "mercury_unit.h"

#include <unistd.h>

/****************/
/* Local Macros */
/****************/

/* Wait timeout in ms */
#define HG_TEST_WAIT_TIMEOUT (HG_TEST_TIMEOUT * 1000)

/************************************/
/* Local Type and Struct Definition */
/************************************/

struct forward_cb_args {
    hg_request_t *request;
    rpc_handle_t *rpc_handle;
    hg_return_t ret;
    bool no_entry;
};

/********************/
/* Local Prototypes */
/********************/

static hg_return_t
hg_test_rpc_no_input(hg_handle_t handle, hg_addr_t addr, hg_id_t rpc_id,
    hg_cb_t callback, hg_request_t *request);

static hg_return_t
hg_test_rpc_input(hg_handle_t handle, hg_addr_t addr, hg_id_t rpc_id,
    hg_cb_t callback, hg_request_t *request);

static hg_return_t
hg_test_rpc_output_cb(const struct hg_cb_info *callback_info);

static hg_return_t
hg_test_rpc_no_output_cb(const struct hg_cb_info *callback_info);

/*******************/
/* Local Variables */
/*******************/

extern hg_id_t hg_test_rpc_null_id_g;
extern hg_id_t hg_test_rpc_open_id_g;

/*---------------------------------------------------------------------------*/
/**
 * Write a signal file so the driver knows we have finished a phase.
 */
static void
write_signal(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "done\n");
        fclose(f);
    }
}

/**
 * Wait for a signal file to appear (up to timeout_ms milliseconds).
 * Returns 0 on success, -1 on timeout.
 */
static int
wait_for_signal(const char *path, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (access(path, F_OK) == 0)
            return 0;
        usleep(10000); /* 10 ms */
        elapsed += 10;
    }
    return -1;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_test_rpc_no_input(hg_handle_t handle, hg_addr_t addr, hg_id_t rpc_id,
    hg_cb_t callback, hg_request_t *request)
{
    hg_return_t ret;
    struct forward_cb_args forward_cb_args = {.request = request,
        .rpc_handle = NULL,
        .ret = HG_SUCCESS,
        .no_entry = false};
    unsigned int flag;
    int rc;

    hg_request_reset(request);

    ret = HG_Reset(handle, addr, rpc_id);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Reset() failed (%s)", HG_Error_to_string(ret));

    ret = HG_Forward(handle, callback, &forward_cb_args, NULL);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Forward() failed (%s)", HG_Error_to_string(ret));

    rc = hg_request_wait(request, HG_TEST_WAIT_TIMEOUT, &flag);
    HG_TEST_CHECK_ERROR(rc != HG_UTIL_SUCCESS, error, ret, HG_PROTOCOL_ERROR,
        "hg_request_wait() failed");

    HG_TEST_CHECK_ERROR(
        !flag, error, ret, HG_TIMEOUT, "hg_request_wait() timed out");
    ret = forward_cb_args.ret;
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "Error in HG callback (%s)", HG_Error_to_string(ret));

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_test_rpc_input(hg_handle_t handle, hg_addr_t addr, hg_id_t rpc_id,
    hg_cb_t callback, hg_request_t *request)
{
    hg_return_t ret;
    rpc_handle_t rpc_open_handle = {.cookie = 100};
    struct forward_cb_args forward_cb_args = {.request = request,
        .rpc_handle = &rpc_open_handle,
        .ret = HG_SUCCESS,
        .no_entry = false};
    rpc_open_in_t in_struct = {
        .handle = rpc_open_handle, .path = HG_TEST_RPC_PATH};
    unsigned int flag;
    int rc;

    hg_request_reset(request);

    ret = HG_Reset(handle, addr, rpc_id);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Reset() failed (%s)", HG_Error_to_string(ret));

    ret = HG_Forward(handle, callback, &forward_cb_args, &in_struct);
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "HG_Forward() failed (%s)", HG_Error_to_string(ret));

    rc = hg_request_wait(request, HG_TEST_WAIT_TIMEOUT, &flag);
    HG_TEST_CHECK_ERROR(rc != HG_UTIL_SUCCESS, error, ret, HG_PROTOCOL_ERROR,
        "hg_request_wait() failed");

    HG_TEST_CHECK_ERROR(
        !flag, error, ret, HG_TIMEOUT, "hg_request_wait() timed out");
    ret = forward_cb_args.ret;
    HG_TEST_CHECK_HG_ERROR(
        error, ret, "Error in HG callback (%s)", HG_Error_to_string(ret));

    return HG_SUCCESS;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_test_rpc_output_cb(const struct hg_cb_info *callback_info)
{
    hg_handle_t handle = callback_info->info.forward.handle;
    struct forward_cb_args *args =
        (struct forward_cb_args *) callback_info->arg;
    int rpc_open_ret;
    int rpc_open_event_id;
    rpc_open_out_t rpc_open_out_struct;
    hg_return_t ret = callback_info->ret;

    if (args->no_entry && ret == HG_NOENTRY)
        goto done;

    HG_TEST_CHECK_HG_ERROR(done, ret, "Error in HG callback (%s)",
        HG_Error_to_string(callback_info->ret));

    /* Get output */
    ret = HG_Get_output(handle, &rpc_open_out_struct);
    HG_TEST_CHECK_HG_ERROR(
        done, ret, "HG_Get_output() failed (%s)", HG_Error_to_string(ret));

    /* Get output parameters */
    rpc_open_ret = rpc_open_out_struct.ret;
    rpc_open_event_id = rpc_open_out_struct.event_id;
    HG_TEST_LOG_DEBUG("rpc_open returned: %d with event_id: %d", rpc_open_ret,
        rpc_open_event_id);
    (void) rpc_open_ret;
    HG_TEST_CHECK_ERROR(rpc_open_event_id != (int) args->rpc_handle->cookie,
        free, ret, HG_FAULT, "Cookie did not match RPC response");

free:
    if (ret != HG_SUCCESS)
        (void) HG_Free_output(handle, &rpc_open_out_struct);
    else {
        /* Free output */
        ret = HG_Free_output(handle, &rpc_open_out_struct);
        HG_TEST_CHECK_HG_ERROR(
            done, ret, "HG_Free_output() failed (%s)", HG_Error_to_string(ret));
    }

done:
    args->ret = ret;
    hg_request_complete(args->request);

    return HG_SUCCESS;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_test_rpc_no_output_cb(const struct hg_cb_info *callback_info)
{
    struct forward_cb_args *args =
        (struct forward_cb_args *) callback_info->arg;

    args->ret = callback_info->ret;
    hg_request_complete(args->request);

    return HG_SUCCESS;
}

/*---------------------------------------------------------------------------*/
int
main(int argc, char *argv[])
{
    struct hg_unit_info info;
    hg_return_t hg_ret;

    /* Initialize the interface (client mode).
     * hg_unit_init reads the hostfile and calls HG_Addr_lookup2(),
     * which triggers NA_Addr_lookup with the relay: prefix.
     * This blocks until DCUtR completes or times out. */
    hg_ret = hg_unit_init(argc, argv, false, &info);
    HG_TEST_CHECK_HG_ERROR(error, hg_ret, "hg_unit_init() failed (%s)",
        HG_Error_to_string(hg_ret));

    printf("# DCUtR lookup complete\n");

    /* Signal the driver that lookup is done.
     * The driver will now kill the relay server. */
    write_signal("/shared/dcutr_lookup_done");

    /* Wait for the driver to confirm relay has been killed. */
    printf("# Waiting for relay to be killed...\n");
    if (wait_for_signal("/shared/relay_killed", 15000) != 0) {
        fprintf(stderr, "Timed out waiting for relay_killed signal\n");
        goto error;
    }

    /* Small grace period for the connection close to propagate */
    usleep(500000); /* 500ms */
    printf("# Relay is dead — testing over direct connection only\n");

    /* NULL RPC test */
    HG_TEST("NULL RPC (post-DCUtR, no relay)");
    hg_ret = hg_test_rpc_no_input(info.handles[0], info.target_addr,
        hg_test_rpc_null_id_g, hg_test_rpc_no_output_cb, info.request);
    HG_TEST_CHECK_HG_ERROR(error, hg_ret, "hg_test_rpc_no_input() failed (%s)",
        HG_Error_to_string(hg_ret));
    HG_PASSED();

    /* Simple RPC test */
    HG_TEST("RPC with response (post-DCUtR, no relay)");
    hg_ret = hg_test_rpc_input(info.handles[0], info.target_addr,
        hg_test_rpc_open_id_g, hg_test_rpc_output_cb, info.request);
    HG_TEST_CHECK_HG_ERROR(error, hg_ret, "hg_test_rpc_input() failed (%s)",
        HG_Error_to_string(hg_ret));
    HG_PASSED();

    hg_unit_cleanup(&info);

    return EXIT_SUCCESS;

error:
    hg_unit_cleanup(&info);

    return EXIT_FAILURE;
}
