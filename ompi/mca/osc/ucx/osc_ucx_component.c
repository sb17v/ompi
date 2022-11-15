/*
 * Copyright (C) Mellanox Technologies Ltd. 2001-2017. ALL RIGHTS RESERVED.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2021      Triad National Security, LLC. All rights
 *                         reserved.
 *
 * Copyright (c) 2022      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "ompi_config.h"

#include "opal/util/printf.h"

#include "ompi/mca/osc/osc.h"
#include "ompi/mca/osc/base/base.h"
#include "ompi/mca/osc/base/osc_base_obj_convert.h"
#include "opal/mca/common/ucx/common_ucx.h"

#include "osc_ucx.h"
#include "osc_ucx_request.h"
#include "opal/util/sys_limits.h"

#define memcpy_off(_dst, _src, _len, _off)        \
    memcpy(((char*)(_dst)) + (_off), _src, _len); \
    (_off) += (_len);

opal_mutex_t mca_osc_service_mutex = OPAL_MUTEX_STATIC_INIT;
static int reg_id = 0;
static void _osc_ucx_init_lock(void)
{
    if(mca_osc_ucx_component.enable_mpi_threads) {
        opal_mutex_lock(&mca_osc_service_mutex);
    }
}
static void _osc_ucx_init_unlock(void)
{
    if(mca_osc_ucx_component.enable_mpi_threads) {
        opal_mutex_unlock(&mca_osc_service_mutex);
    }
}


static int component_open(void);
static int component_close(void);
static int component_register(void);
static int component_init(bool enable_progress_threads, bool enable_mpi_threads);
static int component_finalize(void);
static int component_query(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                           struct ompi_communicator_t *comm, struct opal_info_t *info, int flavor);
static int component_select(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                            struct ompi_communicator_t *comm, struct opal_info_t *info,
                            int flavor, int *model);
static int component_connect_all_dpus(struct ompi_communicator_t *comm);
static void ompi_osc_ucx_unregister_progress(void);

ompi_osc_ucx_component_t mca_osc_ucx_component = {
    { /* ompi_osc_base_component_t */
        .osc_version = {
            OMPI_OSC_BASE_VERSION_3_0_0,
            .mca_component_name = "ucx",
            MCA_BASE_MAKE_VERSION(component, OMPI_MAJOR_VERSION, OMPI_MINOR_VERSION,
                                  OMPI_RELEASE_VERSION),
            .mca_open_component = component_open,
            .mca_close_component = component_close,
            .mca_register_component_params = component_register,
        },
        .osc_data = {
            /* The component is not checkpoint ready */
            MCA_BASE_METADATA_PARAM_NONE
        },
        .osc_init = component_init,
        .osc_query = component_query,
        .osc_select = component_select,
        .osc_finalize = component_finalize,
        .osc_connect_all_dpus = component_connect_all_dpus,
    },
    .wpool                  = NULL,
    .env_initialized        = false,
    .num_incomplete_req_ops = 0,
    .num_modules            = 0,
    .acc_single_intrinsic   = false,
    .dpu_cli                = NULL,
    .dpu_offl_worker        = NULL
};

ompi_osc_ucx_module_t ompi_osc_ucx_module_template = {
    {
        .osc_win_shared_query = ompi_osc_ucx_shared_query,
        .osc_win_attach = ompi_osc_ucx_win_attach,
        .osc_win_detach = ompi_osc_ucx_win_detach,
        .osc_free = ompi_osc_ucx_free,

        .osc_put = ompi_osc_ucx_put,
        .osc_get = ompi_osc_ucx_get,
        .osc_accumulate = ompi_osc_ucx_accumulate,
        .osc_compare_and_swap = ompi_osc_ucx_compare_and_swap,
        .osc_fetch_and_op = ompi_osc_ucx_fetch_and_op,
        .osc_get_accumulate = ompi_osc_ucx_get_accumulate,

        .osc_rput = ompi_osc_ucx_rput,
        .osc_rget = ompi_osc_ucx_rget,
        .osc_raccumulate = ompi_osc_ucx_raccumulate,
        .osc_rget_accumulate = ompi_osc_ucx_rget_accumulate,

        .osc_fence = ompi_osc_ucx_fence,

        .osc_start = ompi_osc_ucx_start,
        .osc_complete = ompi_osc_ucx_complete,
        .osc_post = ompi_osc_ucx_post,
        .osc_wait = ompi_osc_ucx_wait,
        .osc_test = ompi_osc_ucx_test,

        .osc_lock = ompi_osc_ucx_lock,
        .osc_unlock = ompi_osc_ucx_unlock,
        .osc_lock_all = ompi_osc_ucx_lock_all,
        .osc_unlock_all = ompi_osc_ucx_unlock_all,

        .osc_sync = ompi_osc_ucx_sync,
        .osc_flush = ompi_osc_ucx_flush,
        .osc_flush_all = ompi_osc_ucx_flush_all,
        .osc_flush_local = ompi_osc_ucx_flush_local,
        .osc_flush_local_all = ompi_osc_ucx_flush_local_all,
    }
};

/* look up parameters for configuring this window.  The code first
   looks in the info structure passed by the user, then it checks
   for a matching MCA variable. */
static bool check_config_value_bool (char *key, opal_info_t *info)
{
    int ret, flag, param;
    bool result = false;
    const bool *flag_value = &result;

    ret = opal_info_get_bool (info, key, &result, &flag);
    if (OMPI_SUCCESS == ret && flag) {
        return result;
    }

    param = mca_base_var_find("ompi", "osc", "ucx", key);
    if (0 <= param) {
        (void) mca_base_var_get_value(param, &flag_value, NULL, NULL);
    }

    return flag_value[0];
}

static int component_open(void) {
    opal_common_ucx_mca_register();

    return OMPI_SUCCESS;
}

static int component_close(void) {
    opal_common_ucx_mca_deregister();

    return OMPI_SUCCESS;
}

static int component_register(void) {
    unsigned major          = 0;
    unsigned minor          = 0;
    unsigned release_number = 0;
    char *description_str;

    ucp_get_version(&major, &minor, &release_number);

    mca_osc_ucx_component.priority = UCX_VERSION(major, minor, release_number) >= UCX_VERSION(1, 5, 0) ? 60 : 0;

    opal_asprintf(&description_str, "Priority of the osc/ucx component (default: %d)",
             mca_osc_ucx_component.priority);
    (void) mca_base_component_var_register(&mca_osc_ucx_component.super.osc_version, "priority", description_str,
                                           MCA_BASE_VAR_TYPE_UNSIGNED_INT, NULL, 0, 0, OPAL_INFO_LVL_3,
                                           MCA_BASE_VAR_SCOPE_GROUP, &mca_osc_ucx_component.priority);
    free(description_str);

    mca_osc_ucx_component.no_locks = false;

    opal_asprintf(&description_str, "Enable optimizations available only if MPI_LOCK is "
             "not used. Info key of same name overrides this value (default: %s)",
             mca_osc_ucx_component.no_locks  ? "true" : "false");
    (void) mca_base_component_var_register(&mca_osc_ucx_component.super.osc_version, "no_locks", description_str,
                                           MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, OPAL_INFO_LVL_5,
                                           MCA_BASE_VAR_SCOPE_GROUP, &mca_osc_ucx_component.no_locks);
    free(description_str);

    mca_osc_ucx_component.acc_single_intrinsic = false;
    opal_asprintf(&description_str, "Enable optimizations for MPI_Fetch_and_op, MPI_Accumulate, etc for codes "
             "that will not use anything more than a single predefined datatype (default: %s)",
             mca_osc_ucx_component.acc_single_intrinsic  ? "true" : "false");
    (void) mca_base_component_var_register(&mca_osc_ucx_component.super.osc_version, "acc_single_intrinsic",
                                           description_str, MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, OPAL_INFO_LVL_5,
                                           MCA_BASE_VAR_SCOPE_GROUP, &mca_osc_ucx_component.acc_single_intrinsic);
    free(description_str);

    opal_common_ucx_mca_var_register(&mca_osc_ucx_component.super.osc_version);

    if (0 == access ("/dev/shm", W_OK)) {
        mca_osc_ucx_component.backing_directory = "/dev/shm";
    } else {
        mca_osc_ucx_component.backing_directory = ompi_process_info.proc_session_dir;
    }

    (void) mca_base_component_var_register (&mca_osc_ucx_component.super.osc_version, "backing_directory",
                                            "Directory to place backing files for memory windows. "
                                            "This directory should be on a local filesystem such as /tmp or "
                                            "/dev/shm (default: (linux) /dev/shm, (others) session directory)",
                                            MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, OPAL_INFO_LVL_3,
                                            MCA_BASE_VAR_SCOPE_READONLY, &mca_osc_ucx_component.backing_directory);

    return OMPI_SUCCESS;
}

static int progress_callback(void) {
    if (mca_osc_ucx_component.wpool != NULL) {
        opal_common_ucx_wpool_progress(mca_osc_ucx_component.wpool);
    }
    // mpi1sdd progress
    dpu_mpi1sdd_progress((dpu_mpi1sdd_worker_t *)mca_osc_ucx_component.dpu_offl_worker);
    // hc progress
    dpu_hc_progress(&mca_osc_ucx_component.dpu_cli->hc);
    return 0;
}

static int ucp_context_init(bool enable_mt, int proc_world_size) {
    int ret = OMPI_SUCCESS;
    ucs_status_t status;
    ucp_config_t *config = NULL;
    ucp_params_t context_params;

    status = ucp_config_read("MPI", NULL, &config);
    if (UCS_OK != status) {
        OSC_UCX_VERBOSE(1, "ucp_config_read failed: %d", status);
        return OMPI_ERROR;
    }

    /* initialize UCP context */
    memset(&context_params, 0, sizeof(context_params));
    context_params.field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_MT_WORKERS_SHARED
                                | UCP_PARAM_FIELD_ESTIMATED_NUM_EPS | UCP_PARAM_FIELD_REQUEST_INIT
                                | UCP_PARAM_FIELD_REQUEST_SIZE;
    context_params.features = UCP_FEATURE_RMA | UCP_FEATURE_AMO32 | UCP_FEATURE_AMO64;
    context_params.mt_workers_shared = (enable_mt ? 1 : 0);
    context_params.estimated_num_eps = proc_world_size;
    context_params.request_init = opal_common_ucx_req_init;
    context_params.request_size = sizeof(opal_common_ucx_request_t);

#if HAVE_DECL_UCP_PARAM_FIELD_ESTIMATED_NUM_PPN
    context_params.estimated_num_ppn = opal_process_info.num_local_peers + 1;
    context_params.field_mask |= UCP_PARAM_FIELD_ESTIMATED_NUM_PPN;
#endif

    status = ucp_init(&context_params, config, &mca_osc_ucx_component.wpool->ucp_ctx);
    if (UCS_OK != status) {
        OSC_UCX_VERBOSE(1, "ucp_init failed: %d", status);
        ret = OMPI_ERROR;
    }
    ucp_config_release(config);

    return ret;
}

static int component_init(bool enable_progress_threads, bool enable_mpi_threads) {
    opal_common_ucx_support_level_t support_level = OPAL_COMMON_UCX_SUPPORT_NONE;
    mca_base_var_source_t param_source = MCA_BASE_VAR_SOURCE_DEFAULT;
    char in_buf[DPU_HC_BUF_SIZE];
    char out_buf[DPU_HC_BUF_SIZE];
    int my_rank;
    int ret = OMPI_SUCCESS, status = -1,
        param = -1;

    mca_osc_ucx_component.enable_mpi_threads = enable_mpi_threads;
    mca_osc_ucx_component.wpool = opal_common_ucx_wpool_allocate();

    ret = ucp_context_init(enable_mpi_threads,  ompi_proc_world_size());
    if (OMPI_ERROR == ret) {
        return OMPI_ERR_NOT_AVAILABLE;
    }

    my_rank = atoi(getenv("PMIX_RANK"));

    mca_osc_ucx_component.dpu_cli = dpu_cli_connect(my_rank);
    
    dpu_mpi1sdd_host_worker_t offl_worker;
    mca_osc_ucx_component.dpu_offl_worker = calloc(1, sizeof(*mca_osc_ucx_component.dpu_offl_worker));
    dpu_mpi1sdd_init(mca_osc_ucx_component.dpu_offl_worker);
    
    {
        DPU_MPI1SDD_INIT_REQ(status, in_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
    }

    support_level = opal_common_ucx_support_level(mca_osc_ucx_component.wpool->ucp_ctx);
    if (OPAL_COMMON_UCX_SUPPORT_NONE == support_level) {
        ucp_cleanup(mca_osc_ucx_component.wpool->ucp_ctx);
        mca_osc_ucx_component.wpool->ucp_ctx = NULL;
        return OMPI_ERR_NOT_AVAILABLE;
    }

    param = mca_base_var_find("ompi","osc","ucx","priority");
    if (0 <= param) {
        (void) mca_base_var_get_value(param, NULL, &param_source, NULL);
    }

    /*
     * Retain priority if we have supported devices and transports.
     * Lower priority if we have supported transports, but not supported devices.
     */
    if(MCA_BASE_VAR_SOURCE_DEFAULT == param_source) {
        mca_osc_ucx_component.priority = (support_level == OPAL_COMMON_UCX_SUPPORT_DEVICE) ?
                    mca_osc_ucx_component.priority : 9;
    }
    OSC_UCX_VERBOSE(2, "returning priority %d", mca_osc_ucx_component.priority);

    return OMPI_SUCCESS;
}

static int component_finalize(void) {
    int status = -1;
    int i;
    size_t size;
    char in_buf[DPU_HC_BUF_SIZE];
    char out_buf[DPU_HC_BUF_SIZE];

    if (mca_osc_ucx_component.env_initialized) {
        opal_common_ucx_wpool_finalize(mca_osc_ucx_component.wpool);
    }
    opal_common_ucx_wpool_free(mca_osc_ucx_component.wpool);

    for (i = 0; i < mca_osc_ucx_component.dpu_offl_worker->ep_count; i++)
    {
        status = dpu_mpi1sdd_ep_destroy(mca_osc_ucx_component.dpu_offl_worker, i);
        if (0 != status) {
            return OMPI_ERROR;
        }
    }

    /* invoke fini call in DPU to clear up all the eps and worker we created for MPI channel*/
    {
        DPU_MPI1SDD_FINI_REQ(status, in_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
    }
    
    status = dpu_mpi1sdd_fini(mca_osc_ucx_component.dpu_offl_worker);
    if (0 != status) {
        return OMPI_ERROR;
    }
    status = dpu_cli_disconnect(mca_osc_ucx_component.dpu_cli);
    if (0 != status) {
        return OMPI_ERROR;
    }
    return OMPI_SUCCESS;
}

static int component_query(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                           struct ompi_communicator_t *comm, struct opal_info_t *info, int flavor) {
    return mca_osc_ucx_component.priority;
}

static int exchange_len_info_v1(void *my_info, size_t my_info_len, char **recv_info_ptr,
                             int **lens, int **disps_ptr, void *metadata)
{
    int ret = OMPI_SUCCESS;
    struct ompi_communicator_t *comm = (struct ompi_communicator_t *)metadata;
    int comm_size = ompi_comm_size(comm);
    (*lens) = calloc(comm_size, sizeof(int));
    int total_len, i;

    ret = comm->c_coll->coll_allgather(&my_info_len, 1, MPI_INT,
                                       (*lens), 1, MPI_INT, comm,
                                       comm->c_coll->coll_allgather_module);
    if (OMPI_SUCCESS != ret) {
        free(*lens);
        return ret;
    }

    total_len = 0;
    (*disps_ptr) = (int *)calloc(comm_size, sizeof(int));
    for (i = 0; i < comm_size; i++) {
        (*disps_ptr)[i] = total_len;
        total_len += (*lens)[i];
    }

    (*recv_info_ptr) = (char *)calloc(total_len, sizeof(char));
    ret = comm->c_coll->coll_allgatherv(my_info, my_info_len, MPI_BYTE,
                                        (void *)(*recv_info_ptr), (*lens), (*disps_ptr), MPI_BYTE,
                                        comm, comm->c_coll->coll_allgatherv_module);
    if (OMPI_SUCCESS != ret) {
        free(*lens);
        return ret;
    }

    return ret;
}

//|----0---|---1---|
//
static int exchange_len_info(void *my_info, size_t my_info_len, char **recv_info_ptr,
                             int **disps_ptr, void *metadata)
{
    struct ompi_communicator_t *comm = (struct ompi_communicator_t *)metadata;
    int comm_size = ompi_comm_size(comm);
    int *lens = calloc(comm_size, sizeof(int));
    int ret = OMPI_SUCCESS;
    
    ret = exchange_len_info_v1(my_info, my_info_len, recv_info_ptr, &lens, disps_ptr, metadata);
    
    free(lens);
    return ret;
}

static void ompi_osc_ucx_unregister_progress()
{
    int ret;

    /* May be called concurrently - protect */
    _osc_ucx_init_lock();

    mca_osc_ucx_component.num_modules--;
    OSC_UCX_ASSERT(mca_osc_ucx_component.num_modules >= 0);
    if (0 == mca_osc_ucx_component.num_modules) {
        ret = opal_progress_unregister(progress_callback);
        if (OMPI_SUCCESS != ret) {
            OSC_UCX_VERBOSE(1, "opal_progress_unregister failed: %d", ret);
        }
    }

    _osc_ucx_init_unlock();
}

static const char* ompi_osc_ucx_set_no_lock_info(opal_infosubscriber_t *obj, const char *key, const char *value)
{

    struct ompi_win_t *win = (struct ompi_win_t*) obj;
    ompi_osc_ucx_module_t *module = (ompi_osc_ucx_module_t *)win->w_osc_module;
    bool temp;

    temp = opal_str_to_bool(value);

    if (temp && !module->no_locks) {
        /* clean up the lock hash. it is up to the user to ensure no lock is
         * outstanding from this process when setting the info key */
        OBJ_DESTRUCT(&module->outstanding_locks);
        module->no_locks = true;
        win->w_flags |= OMPI_WIN_NO_LOCKS;
    } else if (!temp && module->no_locks) {
        int comm_size = ompi_comm_size (module->comm);
        int ret;

        OBJ_CONSTRUCT(&module->outstanding_locks, opal_hash_table_t);
        ret = opal_hash_table_init (&module->outstanding_locks, comm_size);
        if (OPAL_SUCCESS != ret) {
            module->no_locks = true;
        } else {
            module->no_locks = false;
        }
        win->w_flags &= ~OMPI_WIN_NO_LOCKS;
    }
    module->comm->c_coll->coll_barrier(module->comm, module->comm->c_coll->coll_barrier_module);
    return module->no_locks ? "true" : "false";
}

int ompi_osc_ucx_shared_query(struct ompi_win_t *win, int rank, size_t *size,
        int *disp_unit, void *baseptr)
{
    ompi_osc_ucx_module_t *module =
        (ompi_osc_ucx_module_t*) win->w_osc_module;

    if (module->flavor != MPI_WIN_FLAVOR_SHARED) {
        return MPI_ERR_WIN;
    }

    if (MPI_PROC_NULL != rank) {
        *size = module->sizes[rank];
        *((void**) baseptr) = (void *)module->shmem_addrs[rank];
        if (module->disp_unit == -1) {
            *disp_unit = module->disp_units[rank];
        } else {
            *disp_unit = module->disp_unit;
        }
    } else {
        int i = 0;

        *size = 0;
        *((void**) baseptr) = NULL;
        *disp_unit = 0;
        for (i = 0 ; i < ompi_comm_size(module->comm) ; ++i) {
            if (0 != module->sizes[i]) {
                *size = module->sizes[i];
                *((void**) baseptr) = (void *)module->shmem_addrs[i];
                if (module->disp_unit == -1) {
                    *disp_unit = module->disp_units[rank];
                } else {
                    *disp_unit = module->disp_unit;
                }
                break;
            }
        }
    }

    return OMPI_SUCCESS;
}

static int _create_all_endpoints(void **addrs, int *addr_lens)
{
    int i;
    int ret;

    mca_osc_ucx_component.dpu_offl_worker->ep_count = ompi_proc_world_size();
    mca_osc_ucx_component.dpu_offl_worker->eps = (dpu_ucx_ep_t *)calloc(mca_osc_ucx_component.dpu_offl_worker->ep_count,
                                            sizeof(*(mca_osc_ucx_component.dpu_offl_worker->eps)));
    for (i = 0; i < mca_osc_ucx_component.dpu_offl_worker->ep_count; i++)
    {
        void *temp_addr = malloc(addr_lens[i]);
        memcpy(temp_addr, addrs[i], addr_lens[i]);
        ret =   dpu_mpi1sdd_ep_create(mca_osc_ucx_component.dpu_offl_worker, temp_addr, i);
        if (0 != ret)
        {
            return OMPI_ERROR;
        }
    }
    return OMPI_SUCCESS;
}

static int component_connect_all_dpus(struct ompi_communicator_t *comm) {
    int ret = OMPI_SUCCESS, status = -1, comm_size, i;
    char in_buf[DPU_HC_BUF_SIZE];
    char out_buf[DPU_HC_BUF_SIZE];
    void *local_dpu_addr;
    int local_dpu_addr_sz;
    char *temp_host_addr;
    int *temp_host_addr_disp;
    char *temp_dpu_addr;
    int *temp_dpu_addr_disp;
    void **dpu_addrs;
    void **host_addrs;
    int *dpu_addr_lens;
    int *host_addr_lens;

    comm_size = ompi_comm_size(comm);
    /* Use host channel to send GET_DPU_ADDRESS command and take the response back with DPU address */
    {
        DPU_MPI1SDD_GET_ADDRS_REQ(status, in_buf, DPU_HC_BUF_SIZE, (&mca_osc_ucx_component.dpu_offl_worker->worker));
        assert(0 == status);
        status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        DPU_MPI1SDD_SET_ADDRS(out_buf, local_dpu_addr, local_dpu_addr_sz);
    }

    /* ----- This point assumes the mpi worker has host and dpu address info ----- */
    ret = exchange_len_info_v1(mca_osc_ucx_component.dpu_offl_worker->worker.local_addr,
                            mca_osc_ucx_component.dpu_offl_worker->worker.local_addr_sz,
                            &temp_host_addr, &host_addr_lens, &temp_host_addr_disp, (void *)comm);
    if(OMPI_SUCCESS != ret)
    {
        free(temp_host_addr);
        free(host_addr_lens);
        free(temp_host_addr_disp);
        return ret;
    }

    host_addrs = calloc(comm_size, sizeof(host_addrs[i]));
    for(i = 0; i < comm_size; i++) {
        host_addrs[i] = temp_host_addr + temp_host_addr_disp[i];
    }

    ret = exchange_len_info_v1(local_dpu_addr, local_dpu_addr_sz, &temp_dpu_addr,
                            &dpu_addr_lens, &temp_dpu_addr_disp, (void *)comm);
    if(OMPI_SUCCESS != ret)
    {
        free(temp_dpu_addr);
        free(dpu_addr_lens);
        free(temp_dpu_addr_disp);
        return ret;
    }

    dpu_addrs = calloc(comm_size, sizeof(dpu_addrs[i]));
    for(i = 0; i < comm_size; i++) {
        dpu_addrs[i] = temp_dpu_addr + temp_dpu_addr_disp[i];
    }

    {
        DPU_MPI1SDD_STORE_HOST_ADDR_REQ(status, in_buf, DPU_HC_BUF_SIZE, host_addr_lens, comm_size, host_addrs);
        assert(0 == status);
        status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
    }
    ret = _create_all_endpoints(dpu_addrs, dpu_addr_lens);
    if(OMPI_SUCCESS != ret) {
        return ret;
    }

    /* Create endpoint in DPU */
    {
        DPU_MPI1SDD_CREATE_EP_REQ(status, in_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
        assert(0 == status);
        assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
    }

    free(temp_host_addr);
    free(host_addr_lens);
    free(temp_host_addr_disp);
    free(host_addrs);
    free(temp_dpu_addr);
    free(dpu_addr_lens);
    free(temp_dpu_addr_disp);
    free(dpu_addrs);
    return OMPI_SUCCESS;
}

static int component_select(struct ompi_win_t *win, void **base, size_t size, int disp_unit,
                            struct ompi_communicator_t *comm, struct opal_info_t *info,
                            int flavor, int *model) {
    ompi_osc_ucx_module_t *module = NULL;
    char *name = NULL;
    long values[2];
    int ret = OMPI_SUCCESS;
    int i, comm_size = ompi_comm_size(comm);
    bool env_initialized = false;
    void *state_base = NULL;
    opal_common_ucx_mem_type_t mem_type;
    char *my_mem_addr;
    int my_mem_addr_size;
    uint64_t my_info[2] = {0};
    char *recv_buf = NULL;
    void *dynamic_base = NULL;
    unsigned long total, *rbuf;
    int flag;
    size_t pagesize;
    bool unlink_needed = false;
    int status = -1;
    char in_buf[DPU_HC_BUF_SIZE];
    char out_buf[DPU_HC_BUF_SIZE];

    /* May be called concurrently - protect */
    _osc_ucx_init_lock();

    if (mca_osc_ucx_component.env_initialized == false) {
        /* Lazy initialization of the global state.
         * As not all of the MPI applications are using One-Sided functionality
         * we don't want to initialize in the component_init()
         */

        OBJ_CONSTRUCT(&mca_osc_ucx_component.requests, opal_free_list_t);
        ret = opal_free_list_init (&mca_osc_ucx_component.requests,
                                   sizeof(ompi_osc_ucx_request_t),
                                   opal_cache_line_size,
                                   OBJ_CLASS(ompi_osc_ucx_request_t),
                                   0, 0, 8, 0, 8, NULL, 0, NULL, NULL, NULL);
        if (OMPI_SUCCESS != ret) {
            OSC_UCX_VERBOSE(1, "opal_free_list_init failed: %d", ret);
            goto select_unlock;
        }

        ret = opal_common_ucx_wpool_init(mca_osc_ucx_component.wpool);
        if (OMPI_SUCCESS != ret) {
            OSC_UCX_VERBOSE(1, "opal_common_ucx_wpool_init failed: %d", ret);
            goto select_unlock;
        }

        /* Make sure that all memory updates performed above are globally
         * observable before (mca_osc_ucx_component.env_initialized = true)
         */
        mca_osc_ucx_component.env_initialized = true;
        env_initialized = true;
    }

    /* Account for the number of active "modules" = MPI windows */
    mca_osc_ucx_component.num_modules++;

    /* If this is the first window to be registered - register the progress
     * callback
     */
    OSC_UCX_ASSERT(mca_osc_ucx_component.num_modules > 0);
    if (1 == mca_osc_ucx_component.num_modules) {
        ret = opal_progress_register(progress_callback);
        if (OMPI_SUCCESS != ret) {
            OSC_UCX_VERBOSE(1, "opal_progress_register failed: %d", ret);
            goto select_unlock;
        }
    }

select_unlock:
    _osc_ucx_init_unlock();
    if (ret) {
        goto error;
    }

    /* create module structure */
    module = (ompi_osc_ucx_module_t *)calloc(1, sizeof(ompi_osc_ucx_module_t));
    if (module == NULL) {
        ret = OMPI_ERR_TEMP_OUT_OF_RESOURCE;
        goto error_nomem;
    }

    /* fill in the function pointer part */
    memcpy(module, &ompi_osc_ucx_module_template, sizeof(ompi_osc_base_module_t));

    ret = ompi_comm_dup(comm, &module->comm);
    if (ret != OMPI_SUCCESS) {
        goto error;
    }

    *model = MPI_WIN_UNIFIED;
    opal_asprintf(&name, "ucx window %s", ompi_comm_print_cid(module->comm));
    ompi_win_set_name(win, name);
    free(name);

    module->flavor = flavor;
    module->size = size;
    module->no_locks = check_config_value_bool ("no_locks", info);
    module->acc_single_intrinsic = check_config_value_bool ("acc_single_intrinsic", info);

    /* share everyone's displacement units. Only do an allgather if
       strictly necessary, since it requires O(p) state. */
    values[0] = disp_unit;
    values[1] = -disp_unit;

    ret = module->comm->c_coll->coll_allreduce(MPI_IN_PLACE, values, 2, MPI_LONG,
                                               MPI_MIN, module->comm,
                                               module->comm->c_coll->coll_allreduce_module);
    if (OMPI_SUCCESS != ret) {
        goto error;
    }

    if (values[0] == -values[1]) { /* everyone has the same disp_unit, we do not need O(p) space */
        module->disp_unit = disp_unit;
    } else { /* different disp_unit sizes, allocate O(p) space to store them */
        module->disp_unit = -1;
        module->disp_units = calloc(comm_size, sizeof(int));
        if (module->disp_units == NULL) {
            ret = OMPI_ERR_TEMP_OUT_OF_RESOURCE;
            goto error;
        }

        ret = module->comm->c_coll->coll_allgather(&disp_unit, 1, MPI_INT,
                                                   module->disp_units, 1, MPI_INT,
                                                   module->comm,
                                                   module->comm->c_coll->coll_allgather_module);
        if (OMPI_SUCCESS != ret) {
            goto error;
        }
    }

    ret = opal_common_ucx_wpctx_create(mca_osc_ucx_component.wpool, comm_size,
                                     &exchange_len_info, (void *)module->comm,
                                     &module->ctx);
    if (OMPI_SUCCESS != ret) {
        goto error;
    }

    if (flavor == MPI_WIN_FLAVOR_SHARED) {
        /* create the segment */
        opal_output_verbose(MCA_BASE_VERBOSE_DEBUG, ompi_osc_base_framework.framework_output,
                            "allocating shared memory region of size %ld\n", (long) size);
        /* get the pagesize */
        pagesize = opal_getpagesize();

        rbuf = malloc(sizeof(unsigned long) * comm_size);
        if (NULL == rbuf) return OMPI_ERR_TEMP_OUT_OF_RESOURCE;

        /* Note that the alloc_shared_noncontig info key only has
         * meaning during window creation.  Once the window is
         * created, we can't move memory around without making
         * everything miserable.  So we intentionally do not subscribe
         * to updates on the info key, because there's no useful
         * update to occur. */
        module->noncontig_shared_win = false;
        if (OMPI_SUCCESS != opal_info_get_bool(info, "alloc_shared_noncontig",
                                               &module->noncontig_shared_win, &flag)) {
            goto error;
        }

        if (module->noncontig_shared_win) {
            opal_output_verbose(MCA_BASE_VERBOSE_DEBUG, ompi_osc_base_framework.framework_output,
                                "allocating window using non-contiguous strategy");
            total = ((size - 1) / pagesize + 1) * pagesize;
        } else {
            opal_output_verbose(MCA_BASE_VERBOSE_DEBUG, ompi_osc_base_framework.framework_output,
                                "allocating window using contiguous strategy");
            total = size;
        }
        ret = module->comm->c_coll->coll_allgather(&total, 1, MPI_UNSIGNED_LONG,
                                                  rbuf, 1, MPI_UNSIGNED_LONG,
                                                  module->comm,
                                                  module->comm->c_coll->coll_allgather_module);
        if (OMPI_SUCCESS != ret) return ret;

        total = 0;
        for (i = 0 ; i < comm_size ; ++i) {
            total += rbuf[i];
        }

        module->segment_base = NULL;
        module->shmem_addrs = NULL;
        module->sizes = NULL;

        if (total != 0) {
            /* user opal/shmem directly to create a shared memory segment */
            if (0 == ompi_comm_rank (module->comm)) {
                char *data_file;
                ret = opal_asprintf (&data_file, "%s" OPAL_PATH_SEP "osc_ucx.%s.%x.%d.%s",
                                     mca_osc_ucx_component.backing_directory, ompi_process_info.nodename,
                                     OMPI_PROC_MY_NAME->jobid, (int) OMPI_PROC_MY_NAME->vpid,
                                     ompi_comm_print_cid(module->comm));
                if (ret < 0) {
                    free(rbuf);
                    return OMPI_ERR_OUT_OF_RESOURCE;
                }

                ret = opal_shmem_segment_create (&module->seg_ds, data_file, total);
                free(data_file);
                if (OPAL_SUCCESS != ret) {
                    free(rbuf);
                    goto error;
                }

                unlink_needed = true;
            }

            ret = module->comm->c_coll->coll_bcast (&module->seg_ds, sizeof (module->seg_ds), MPI_BYTE, 0,
                                                    module->comm, module->comm->c_coll->coll_bcast_module);
            if (OMPI_SUCCESS != ret) {
                free(rbuf);
                goto error;
            }

            module->segment_base = opal_shmem_segment_attach (&module->seg_ds);
            if (NULL == module->segment_base) {
                free(rbuf);
                goto error;
            }

            /* wait for all processes to attach */
            ret = module->comm->c_coll->coll_barrier (module->comm, module->comm->c_coll->coll_barrier_module);
            if (OMPI_SUCCESS != ret) {
                free(rbuf);
                goto error;
            }

            if (0 == ompi_comm_rank (module->comm)) {
                opal_shmem_unlink (&module->seg_ds);
                unlink_needed = false;
            }
        }

        /* Although module->segment_base is pointing to a same physical address
         * for all the processes, its value which is a virtual address can be
         * different between different processes. To use direct load/store,
         * shmem_addrs can be used, however, for RDMA, virtual address of
         * remote process that will be stored in module->addrs should be used */
        module->sizes = malloc(sizeof(size_t) * comm_size);
        if (NULL == module->sizes) {
            free(rbuf);
            ret = OMPI_ERR_TEMP_OUT_OF_RESOURCE;
            goto error;
        }
        module->shmem_addrs = malloc(sizeof(uint64_t) * comm_size);
        if (NULL == module->shmem_addrs) {
            free(module->sizes);
            free(rbuf);
            ret =  OMPI_ERR_TEMP_OUT_OF_RESOURCE;
            goto error;
        }


        for (i = 0, total = 0; i < comm_size ; ++i) {
            module->sizes[i] = rbuf[i];
            if (module->sizes[i] || !module->noncontig_shared_win) {
                module->shmem_addrs[i] = ((uint64_t) module->segment_base) + total;
                total += rbuf[i];
            } else {
                module->shmem_addrs[i] = (uint64_t)NULL;
            }
        }

        free(rbuf);

        module->size = module->sizes[ompi_comm_rank(module->comm)];
        *base = (void *)module->shmem_addrs[ompi_comm_rank(module->comm)];
    }

    void **mem_base = base;
    switch (flavor) {
    case MPI_WIN_FLAVOR_DYNAMIC:
        mem_type = OPAL_COMMON_UCX_MEM_ALLOCATE_MAP;
        module->size = 0;
        mem_base = &dynamic_base;
        break;
    case MPI_WIN_FLAVOR_ALLOCATE:
        mem_type = OPAL_COMMON_UCX_MEM_ALLOCATE_MAP;
        break;
    case MPI_WIN_FLAVOR_CREATE:
        mem_type = OPAL_COMMON_UCX_MEM_MAP;
        break;
    case MPI_WIN_FLAVOR_SHARED:
        mem_type = OPAL_COMMON_UCX_MEM_MAP;
        break;
    }
    ret = opal_common_ucx_wpmem_create(module->ctx, mem_base, module->size,
                                     mem_type, &exchange_len_info,
                                     OPAL_COMMON_UCX_WPMEM_ADDR_EXCHANGE_FULL,
                                     (void *)module->comm,
                                       &my_mem_addr, &my_mem_addr_size,
                                       &module->mem);
    if (ret != OMPI_SUCCESS) {
        goto error;
    }

    state_base = (void *)&(module->state);
    ret = opal_common_ucx_wpmem_create(module->ctx, &state_base,
                                     sizeof(ompi_osc_ucx_state_t),
                                     OPAL_COMMON_UCX_MEM_MAP,
                                     &exchange_len_info,
                                     OPAL_COMMON_UCX_WPMEM_ADDR_EXCHANGE_FULL,
                                     (void *)module->comm,
                                     &my_mem_addr, &my_mem_addr_size,
                                     &module->state_mem);
    if (ret != OMPI_SUCCESS) {
        goto error;
    }

    /* exchange window addrs */
    if (flavor == MPI_WIN_FLAVOR_ALLOCATE || flavor == MPI_WIN_FLAVOR_CREATE ||
            flavor == MPI_WIN_FLAVOR_SHARED) {
        my_info[0] = (uint64_t)*base;
    } else if (flavor == MPI_WIN_FLAVOR_DYNAMIC) {
        my_info[0] = (uint64_t)dynamic_base;
    }
    my_info[1] = (uint64_t)state_base;

    recv_buf = (char *)calloc(comm_size, 2 * sizeof(uint64_t));
    ret = comm->c_coll->coll_allgather((void *)my_info, 2 * sizeof(uint64_t),
                                       MPI_BYTE, recv_buf, 2 * sizeof(uint64_t),
                                       MPI_BYTE, comm, comm->c_coll->coll_allgather_module);
    if (ret != OMPI_SUCCESS) {
        goto error;
    }

    module->addrs = calloc(comm_size, sizeof(uint64_t));
    module->state_addrs = calloc(comm_size, sizeof(uint64_t));
    for (i = 0; i < comm_size; i++) {
        memcpy(&(module->addrs[i]), recv_buf + i * 2 * sizeof(uint64_t), sizeof(uint64_t));
        memcpy(&(module->state_addrs[i]), recv_buf + i * 2 * sizeof(uint64_t) + sizeof(uint64_t), sizeof(uint64_t));
    }
    /* Send this address details to DPU*/
    // hc_mem_reg_info.size = module->size;
    // hc_mem_reg_info.rkey.addr_ptr = my_mem_addr;
    // hc_mem_reg_info.rkey.addr_len = my_mem_addr_size;
    // hc_mem_reg_info.base = module->addrs[ompi_comm_rank(module->comm)];
    
    module->hc_mem_reg_info = calloc(1, sizeof(*module->hc_mem_reg_info));
    if (0 != module->size) {
        status = dpu_hc_buffer_reg(&mca_osc_ucx_component.dpu_cli->hc, module->hc_mem_reg_info, (void *)module->addrs[ompi_comm_rank(module->comm)], module->size);
        if(0 != status) {
            printf("dpu_hc_buffer_reg failed\n");
            goto error;
        }
        {
            DPU_MRREG_REQ(status, in_buf, DPU_HC_BUF_SIZE, (module->hc_mem_reg_info), 0);
            assert(0 == status);
            status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
            assert(0 == status);
            assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
            DPU_MRREG_RSP(out_buf, module->mem_reg_id);
        }
    }
    free(recv_buf);

    /* init window state */
    module->state.lock = TARGET_LOCK_UNLOCKED;
    module->state.post_index = 0;
    memset((void *)module->state.post_state, 0, sizeof(uint64_t) * OMPI_OSC_UCX_POST_PEER_MAX);
    module->state.complete_count = 0;
    module->state.req_flag = 0;
    module->state.acc_lock = TARGET_LOCK_UNLOCKED;
    module->state.dynamic_win_count = 0;
    for (i = 0; i < OMPI_OSC_UCX_ATTACH_MAX; i++) {
        module->local_dynamic_win_info[i].refcnt = 0;
    }
    module->epoch_type.access = NONE_EPOCH;
    module->epoch_type.exposure = NONE_EPOCH;
    module->lock_count = 0;
    module->post_count = 0;
    module->start_group = NULL;
    module->post_group = NULL;
    OBJ_CONSTRUCT(&module->pending_posts, opal_list_t);
    module->start_grp_ranks = NULL;
    module->lock_all_is_nocheck = false;
    module->mpi1sdd_mem_reg_cache = NULL;
    module->mpi1sdd_mem_reg_cache_cnt = 0;
    module->mpi1sdd_ops_tracker = calloc(comm_size, sizeof(*module->mpi1sdd_ops_tracker));
    if (!module->no_locks) {
        OBJ_CONSTRUCT(&module->outstanding_locks, opal_hash_table_t);
        ret = opal_hash_table_init(&module->outstanding_locks, comm_size);
        if (ret != OPAL_SUCCESS) {
            goto error;
        }
    } else {
        win->w_flags |= OMPI_WIN_NO_LOCKS;
    }

    win->w_osc_module = &module->super;

    opal_infosubscribe_subscribe(&win->super, "no_locks", "false", ompi_osc_ucx_set_no_lock_info);

    /* sync with everyone */

    ret = module->comm->c_coll->coll_barrier(module->comm,
                                             module->comm->c_coll->coll_barrier_module);
    if (ret != OMPI_SUCCESS) {
        goto error;
    }

    ret = ompi_osc_ucx_get_comm_world_rank_map(win, &module->comm_world_rank_map);
    if(OMPI_SUCCESS != ret) {
        return ret;
    }

    return ret;

error:
    if (module->disp_units) free(module->disp_units);
    if (module->comm) ompi_comm_free(&module->comm);
    free(module);

error_nomem:
    if (env_initialized == true) {
        opal_common_ucx_wpool_finalize(mca_osc_ucx_component.wpool);
        OBJ_DESTRUCT(&mca_osc_ucx_component.requests);
        mca_osc_ucx_component.env_initialized = false;
    }

    if (0 == ompi_comm_rank (module->comm) && unlink_needed) {
        opal_shmem_unlink (&module->seg_ds);
    }
    ompi_osc_ucx_unregister_progress();
    return ret;
}

int ompi_osc_find_attached_region_position(ompi_osc_dynamic_win_info_t *dynamic_wins,
                                           int min_index, int max_index,
                                           uint64_t base, size_t len, int *insert) {
    int mid_index = (max_index + min_index) >> 1;

    if (dynamic_wins[mid_index].size == 1) {
        len = 0;
    }

    if (min_index > max_index) {
        (*insert) = min_index;
        return -1;
    }

    if (dynamic_wins[mid_index].base > base) {
        return ompi_osc_find_attached_region_position(dynamic_wins, min_index, mid_index-1,
                                                      base, len, insert);
    } else if (base + len <= dynamic_wins[mid_index].base + dynamic_wins[mid_index].size) {
        return mid_index;
    } else {
        return ompi_osc_find_attached_region_position(dynamic_wins, mid_index+1, max_index,
                                                      base, len, insert);
    }
}
inline bool ompi_osc_need_acc_lock(ompi_osc_ucx_module_t *module, int target)
{
    ompi_osc_ucx_lock_t *lock = NULL;
    opal_hash_table_get_value_uint32(&module->outstanding_locks,
                                     (uint32_t) target, (void **) &lock);

    /* if there is an exclusive lock there is no need to acqurie the accumulate lock */
    return !(NULL != lock && LOCK_EXCLUSIVE == lock->type);
}

inline int ompi_osc_state_lock(
    ompi_osc_ucx_module_t *module,
    int                    target,
    bool                  *lock_acquired,
    bool                   force_lock) {
    uint64_t result_value = -1;
    uint64_t remote_addr = (module->state_addrs)[target] + OSC_UCX_STATE_ACC_LOCK_OFFSET;
    int ret = OMPI_SUCCESS;

    if (force_lock || ompi_osc_need_acc_lock(module, target)) {
        for (;;) {
            ret = opal_common_ucx_wpmem_cmpswp(module->state_mem,
                                            TARGET_LOCK_UNLOCKED, TARGET_LOCK_EXCLUSIVE,
                                            target, &result_value, sizeof(result_value),
                                            remote_addr);
            if (ret != OMPI_SUCCESS) {
                OSC_UCX_VERBOSE(1, "opal_common_ucx_mem_cmpswp failed: %d", ret);
                return OMPI_ERROR;
            }
            if (result_value == TARGET_LOCK_UNLOCKED) {
                break;
            }

            opal_common_ucx_wpool_progress(mca_osc_ucx_component.wpool);
        }

        *lock_acquired = true;
    } else {
        *lock_acquired = false;
    }

    return OMPI_SUCCESS;
}

inline int ompi_osc_state_unlock(
    ompi_osc_ucx_module_t *module,
    int                    target,
    bool                   lock_acquired,
    void                  *free_ptr) {
    uint64_t remote_addr = (module->state_addrs)[target] + OSC_UCX_STATE_ACC_LOCK_OFFSET;
    int ret = OMPI_SUCCESS;

    if (lock_acquired) {
        uint64_t result_value = 0;
        /* fence any still active operations */
        ret = opal_common_ucx_wpmem_fence(module->mem);
        if (ret != OMPI_SUCCESS) {
            OSC_UCX_VERBOSE(1, "opal_common_ucx_mem_fence failed: %d", ret);
            return OMPI_ERROR;
        }

        ret = opal_common_ucx_wpmem_fetch(module->state_mem,
                                        UCP_ATOMIC_FETCH_OP_SWAP, TARGET_LOCK_UNLOCKED,
                                        target, &result_value, sizeof(result_value),
                                        remote_addr);
        assert(result_value == TARGET_LOCK_EXCLUSIVE);
    } else if (NULL != free_ptr){
        /* flush before freeing the buffer */
        ret = opal_common_ucx_ctx_flush(module->ctx, OPAL_COMMON_UCX_SCOPE_EP, target);
    }
    /* TODO: encapsulate in a request and make the release non-blocking */
    if (NULL != free_ptr) {
        free(free_ptr);
    }
    if (ret != OMPI_SUCCESS) {
        OSC_UCX_VERBOSE(1, "opal_common_ucx_mem_fetch failed: %d", ret);
        return OMPI_ERROR;
    }

    return ret;
}

int ompi_osc_ucx_win_attach(struct ompi_win_t *win, void *base, size_t len) {
    ompi_osc_ucx_module_t *module = (ompi_osc_ucx_module_t*) win->w_osc_module;
    int insert_index = -1, contain_index;
    char in_buf[DPU_HC_BUF_SIZE];
    char out_buf[DPU_HC_BUF_SIZE];
    int ret = OMPI_SUCCESS, status = -1;

    if (module->state.dynamic_win_count >= OMPI_OSC_UCX_ATTACH_MAX) {
        OSC_UCX_ERROR("Dynamic window attach failed: Cannot satisfy %d attached windows. "
                "Max attached windows is %d \n",
                module->state.dynamic_win_count+1,
                OMPI_OSC_UCX_ATTACH_MAX);
        return OMPI_ERR_TEMP_OUT_OF_RESOURCE;
    }

    bool lock_acquired = false;
    ret = ompi_osc_state_lock(module, ompi_comm_rank(module->comm), &lock_acquired, true);
    if (ret != OMPI_SUCCESS) {
        return ret;
    }

    if (module->state.dynamic_win_count > 0) {
        contain_index = ompi_osc_find_attached_region_position((ompi_osc_dynamic_win_info_t *)module->state.dynamic_wins,
                                                               0, (int)module->state.dynamic_win_count - 1,
                                                               (uint64_t)base, len, &insert_index);
        if (contain_index >= 0) {
            module->local_dynamic_win_info[contain_index].refcnt++;
            ompi_osc_state_unlock(module, ompi_comm_rank(module->comm), lock_acquired, NULL);
            return ret;
        }

        assert(insert_index >= 0 && (uint64_t)insert_index <= module->state.dynamic_win_count);

        memmove((void *)&module->local_dynamic_win_info[insert_index+1],
                (void *)&module->local_dynamic_win_info[insert_index],
                (OMPI_OSC_UCX_ATTACH_MAX - (insert_index + 1)) * sizeof(ompi_osc_local_dynamic_win_info_t));
        memmove((void *)&module->state.dynamic_wins[insert_index+1],
                (void *)&module->state.dynamic_wins[insert_index],
                (OMPI_OSC_UCX_ATTACH_MAX - (insert_index + 1)) * sizeof(ompi_osc_dynamic_win_info_t));
    } else {
        insert_index = 0;
    }

    ret = opal_common_ucx_wpmem_create(module->ctx, &base, len,
                                       OPAL_COMMON_UCX_MEM_MAP, &exchange_len_info,
                                       OPAL_COMMON_UCX_WPMEM_ADDR_EXCHANGE_DIRECT,
                                       (void *)module->comm,
                                       &(module->local_dynamic_win_info[insert_index].my_mem_addr),
                                       &(module->local_dynamic_win_info[insert_index].my_mem_addr_size),
                                       &(module->local_dynamic_win_info[insert_index].mem));
    if (ret != OMPI_SUCCESS) {
        ompi_osc_state_unlock(module, ompi_comm_rank(module->comm), lock_acquired, NULL);
        return ret;
    }
    /* Send to DPU */
    // Maybe this is not required -- check
    // {
    //     dpu_hc_mem_t hc_mem_reg_info;
    //     hc_mem_reg_info.base = base;
    //     hc_mem_reg_info.size = len;
    //     hc_mem_reg_info.rkey.addr_ptr = module->local_dynamic_win_info[insert_index].my_mem_addr;
    //     hc_mem_reg_info.rkey.addr_len = module->local_dynamic_win_info[insert_index].my_mem_addr_size;
    //     DPU_MRREG_REQ(status, in_buf, DPU_HC_BUF_SIZE, (&hc_mem_reg_info), 0);
    //     assert(0 == status);
    //     status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
    //     assert(0 == status);
    //     assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
    //     DPU_MRREG_RSP(out_buf, module->local_dynamic_win_info[insert_index].my_mem_reg_id);
    //     printf("Getting MY mem reg id in windows create insert_index: %d DYNAMIC: %d\n", insert_index, module->local_dynamic_win_info[insert_index].my_mem_reg_id);
    //     fflush(stdout);
    // }
    module->state.dynamic_wins[insert_index].base = (uint64_t)base;
    module->state.dynamic_wins[insert_index].size = len;

    memcpy((char *)(module->state.dynamic_wins[insert_index].mem_addr),
           (char *)module->local_dynamic_win_info[insert_index].my_mem_addr,
           module->local_dynamic_win_info[insert_index].my_mem_addr_size);

    module->local_dynamic_win_info[insert_index].refcnt++;
    module->state.dynamic_win_count++;

    return ompi_osc_state_unlock(module, ompi_comm_rank(module->comm), lock_acquired, NULL);
}

int ompi_osc_ucx_win_detach(struct ompi_win_t *win, const void *base) {
    ompi_osc_ucx_module_t *module = (ompi_osc_ucx_module_t*) win->w_osc_module;
    int insert, contain;
    int status = -1;
    char in_buf[DPU_HC_BUF_SIZE];
    char out_buf[DPU_HC_BUF_SIZE];

    bool lock_acquired = false;
    int ret = ompi_osc_state_lock(module, ompi_comm_rank(module->comm), &lock_acquired, true);
    if (ret != OMPI_SUCCESS) {
        return ret;
    }

    assert(module->state.dynamic_win_count > 0);

    contain = ompi_osc_find_attached_region_position((ompi_osc_dynamic_win_info_t *)module->state.dynamic_wins,
                                                     0, (int)module->state.dynamic_win_count,
                                                     (uint64_t)base, 1, &insert);
    assert(contain >= 0 && (uint64_t)contain < module->state.dynamic_win_count);

    /* if we can't find region - just exit */
    if (contain < 0) {
        return ompi_osc_state_unlock(module, ompi_comm_rank(module->comm), lock_acquired, NULL);
    }

    module->local_dynamic_win_info[contain].refcnt--;
    if (module->local_dynamic_win_info[contain].refcnt == 0) {
        opal_common_ucx_wpmem_free(module->local_dynamic_win_info[contain].mem);
        /* Need to handle reg_id - setting 0 as of now*/
        // {
        //     printf("Destroyed DYNAMIC win contain: %d my mem reg id: %d\n", contain, module->local_dynamic_win_info[contain].my_mem_reg_id);
        //     DPU_MRDEREG_REQ(status, in_buf, DPU_HC_BUF_SIZE, module->local_dynamic_win_info[contain].my_mem_reg_id);
        //     assert(0 == status);
        //     status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
        //     assert(0 == status);
        //     assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
        // }
        memmove((void *)&(module->local_dynamic_win_info[contain]),
                (void *)&(module->local_dynamic_win_info[contain+1]),
                (OMPI_OSC_UCX_ATTACH_MAX - (contain + 1)) * sizeof(ompi_osc_local_dynamic_win_info_t));
        memmove((void *)&module->state.dynamic_wins[contain],
                (void *)&module->state.dynamic_wins[contain+1],
                (OMPI_OSC_UCX_ATTACH_MAX - (contain + 1)) * sizeof(ompi_osc_dynamic_win_info_t));

        module->state.dynamic_win_count--;
    }

    return ompi_osc_state_unlock(module, ompi_comm_rank(module->comm), lock_acquired, NULL);

}

int ompi_osc_ucx_free(struct ompi_win_t *win) {
    ompi_osc_ucx_module_t *module = (ompi_osc_ucx_module_t*) win->w_osc_module;
    int ret, local_rank, target_rank, status = -1;
    char in_buf[DPU_HC_BUF_SIZE];
    char out_buf[DPU_HC_BUF_SIZE];
    uint64_t i;

    assert(module->lock_count == 0);
    assert(opal_list_is_empty(&module->pending_posts) == true);
    if(!module->no_locks) {
        OBJ_DESTRUCT(&module->outstanding_locks);
    }
    OBJ_DESTRUCT(&module->pending_posts);

    opal_common_ucx_ctx_flush(module->ctx, OPAL_COMMON_UCX_SCOPE_WORKER, 0);

    local_rank = module->comm_world_rank_map[ompi_comm_rank(module->comm)];
    for (i = 0; i < ompi_comm_size(module->comm); i++) {
        if (0 < module->mpi1sdd_ops_tracker[i]) {
            target_rank = module->comm_world_rank_map[i];

            DPU_MPI1SDD_HC_WORKER_FLUSH_REQ(status, in_buf, DPU_MPI1SDD_BUF_SIZE, local_rank);
            assert(0 == status);
            status = dpu_mpi1sdd_host_cmd_exec(mca_osc_ucx_component.dpu_offl_worker, target_rank, in_buf, out_buf, DPU_MPI1SDD_BUF_SIZE);
            assert(0 == status);
            assert(0 == DPU_MPI1SDD_MPIC_GET_RESP_STATUS(out_buf));

            DPU_MPI1SDD_MPIC_WORKER_FLUSH_REQ(status, in_buf, DPU_MPI1SDD_BUF_SIZE, local_rank);
            assert(0 == status);
            status = dpu_mpi1sdd_host_cmd_exec(mca_osc_ucx_component.dpu_offl_worker, target_rank, in_buf, out_buf, DPU_MPI1SDD_BUF_SIZE);
            assert(0 == status);
            assert(0 == DPU_MPI1SDD_MPIC_GET_RESP_STATUS(out_buf));
        }
    }

    ret = module->comm->c_coll->coll_barrier(module->comm,
                                             module->comm->c_coll->coll_barrier_module);
    if (ret != OMPI_SUCCESS) {
        return ret;
    }

    if (module->flavor == MPI_WIN_FLAVOR_SHARED) {
        if (module->segment_base != NULL)
            opal_shmem_segment_detach(&module->seg_ds);
        if (module->shmem_addrs != NULL)
            free(module->shmem_addrs);
        if (module->sizes != NULL)
            free(module->sizes);
    }

    if (module->flavor == MPI_WIN_FLAVOR_DYNAMIC) {
       /* MPI_Win_free should detach any memory attached to dynamic windows */
        for (i = 0; i < module->state.dynamic_win_count; i++) {
            assert(module->local_dynamic_win_info[i].refcnt >= 1);
            opal_common_ucx_wpmem_free(module->local_dynamic_win_info[i].mem);
            /* Need to handle reg_id - setting 0 as of now - put NULL check here*/
            // {
            //     printf("Destroyed DYNAMIC win my mem i: %d reg id: %d\n", i, module->local_dynamic_win_info[i].my_mem_reg_id);
            //     fflush(stdout);
            //     DPU_MRDEREG_REQ(status, in_buf, DPU_HC_BUF_SIZE, module->local_dynamic_win_info[i].my_mem_reg_id);
            //     assert(0 == status);
            //     status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
            //     assert(0 == status);
            //     assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
            // }
        }
        module->state.dynamic_win_count = 0;

        if (module->addrs[ompi_comm_rank(module->comm)] != 0) {
            free((void *)module->addrs[ompi_comm_rank(module->comm)]);
        }
    }

    free(module->addrs);
    free(module->state_addrs);

    opal_common_ucx_wpmem_free(module->state_mem);
    if (NULL != module->mem) {
        opal_common_ucx_wpmem_free(module->mem);
        if (0 != module->size)
        {
            {
                DPU_MRDEREG_REQ(status, in_buf, DPU_HC_BUF_SIZE, module->mem_reg_id);
                assert(0 == status);
                status = dpu_cli_cmd_exec(mca_osc_ucx_component.dpu_cli, in_buf, out_buf, DPU_HC_BUF_SIZE);
                assert(0 == status);
                assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
            }
            status = dpu_hc_buffer_dereg(module->hc_mem_reg_info);
            if (0 != status) {
                return OMPI_ERROR;
            }
            free(module->hc_mem_reg_info);
        }
    }

    /* TODO: Need to re-check this tomorrow morning. deregister the cache entries */
    for (i = 0; i < ompi_comm_size(module->comm); i++) {
        if (0 < module->mpi1sdd_ops_tracker[i]) {
            target_rank = module->comm_world_rank_map[i];

            DPU_MPI1SDD_MPIC_CLEAN_RKEY_CACHE_REQ(status, in_buf, DPU_MPI1SDD_BUF_SIZE, local_rank)
            assert(0 == status);
            status = dpu_mpi1sdd_host_cmd_exec(mca_osc_ucx_component.dpu_offl_worker, target_rank, in_buf, out_buf, DPU_HC_BUF_SIZE);
            assert(0 == status);
            assert(0 == DPU_MPI1SDD_GET_RESP_STATUS(out_buf));
        }
    }

    for (i = 0; i < module->mpi1sdd_mem_reg_cache_cnt; i++) {
        status = dpu_mpi1sdd_buffer_dereg(&module->mpi1sdd_mem_reg_cache[i]);
        assert(0 == status);
    }
    free(module->mpi1sdd_mem_reg_cache);

    opal_common_ucx_wpctx_release(module->ctx);

    if (module->disp_units) {
        free(module->disp_units);
    }
    ompi_comm_free(&module->comm);

    free(module);
    ompi_osc_ucx_unregister_progress();

    return ret;
}
