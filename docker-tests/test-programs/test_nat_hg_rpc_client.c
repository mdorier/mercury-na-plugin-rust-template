/**
 * NAT Docker test: minimal HG RPC client for relay scenarios.
 *
 * Runs only the basic RPC tests (null, with response, one-way) that work
 * within relay circuit bandwidth limits. Unlike test_rpc.c, this skips
 * overflow, cancel, multi-thread, and multi-context tests.
 *
 * Based on test/test_rpc.c.
 */

#include "mercury_unit.h"

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

/*******************/
/* Local Variables */
/*******************/

extern hg_id_t hg_test_rpc_null_id_g;
extern hg_id_t hg_test_rpc_open_id_g;
extern hg_id_t hg_test_rpc_open_id_no_resp_g;

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_test_rpc_output_cb(const struct hg_cb_info *callback_info)
{
    hg_handle_t handle = callback_info->info.forward.handle;
    struct forward_cb_args *args =
        (struct forward_cb_args *) callback_info->arg;
    rpc_open_out_t rpc_open_out_struct;
    hg_return_t ret = callback_info->ret;

    HG_TEST_CHECK_HG_ERROR(done, ret, "Error in HG callback (%s)",
        HG_Error_to_string(callback_info->ret));

    ret = HG_Get_output(handle, &rpc_open_out_struct);
    HG_TEST_CHECK_HG_ERROR(
        done, ret, "HG_Get_output() failed (%s)", HG_Error_to_string(ret));

    HG_TEST_CHECK_ERROR(
        rpc_open_out_struct.event_id != (int) args->rpc_handle->cookie,
        free, ret, HG_FAULT, "Cookie did not match RPC response");

free:
    if (ret != HG_SUCCESS)
        (void) HG_Free_output(handle, &rpc_open_out_struct);
    else {
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
static hg_return_t
hg_test_rpc_no_input(hg_handle_t handle, hg_addr_t addr, hg_id_t rpc_id,
    hg_cb_t callback, hg_request_t *request)
{
    hg_return_t ret;
    struct forward_cb_args forward_cb_args = {.request = request,
        .rpc_handle = NULL, .ret = HG_SUCCESS, .no_entry = false};
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
        .rpc_handle = &rpc_open_handle, .ret = HG_SUCCESS, .no_entry = false};
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
int
main(int argc, char *argv[])
{
    struct hg_unit_info info;
    hg_return_t hg_ret;

    hg_ret = hg_unit_init(argc, argv, false, &info);
    HG_TEST_CHECK_HG_ERROR(error, hg_ret, "hg_unit_init() failed (%s)",
        HG_Error_to_string(hg_ret));

    /* NULL RPC test */
    HG_TEST("NULL RPC (relay)");
    hg_ret = hg_test_rpc_no_input(info.handles[0], info.target_addr,
        hg_test_rpc_null_id_g, hg_test_rpc_no_output_cb, info.request);
    HG_TEST_CHECK_HG_ERROR(error, hg_ret, "hg_test_rpc_no_input() failed (%s)",
        HG_Error_to_string(hg_ret));
    HG_PASSED();

    /* Simple RPC test */
    HG_TEST("RPC with response (relay)");
    hg_ret = hg_test_rpc_input(info.handles[0], info.target_addr,
        hg_test_rpc_open_id_g, hg_test_rpc_output_cb, info.request);
    HG_TEST_CHECK_HG_ERROR(error, hg_ret, "hg_test_rpc_input() failed (%s)",
        HG_Error_to_string(hg_ret));
    HG_PASSED();

    /* RPC test with no response */
    HG_TEST("RPC without response (relay, one-way)");
    hg_ret = hg_test_rpc_input(info.handles[0], info.target_addr,
        hg_test_rpc_open_id_no_resp_g, hg_test_rpc_no_output_cb, info.request);
    HG_TEST_CHECK_HG_ERROR(error, hg_ret, "hg_test_rpc_input() failed (%s)",
        HG_Error_to_string(hg_ret));
    HG_PASSED();

    hg_unit_cleanup(&info);
    return EXIT_SUCCESS;

error:
    hg_unit_cleanup(&info);
    return EXIT_FAILURE;
}
