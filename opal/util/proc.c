/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2013-2022 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2013      Inria.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2020      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "opal_config.h"

#include "opal/mca/pmix/pmix-internal.h"
#include "opal/util/arch.h"
#include "opal/util/proc.h"
#include "opal/util/string_copy.h"
#include "proc.h"

opal_process_name_t opal_name_wildcard = {OPAL_JOBID_WILDCARD, OPAL_VPID_WILDCARD};
opal_process_name_t opal_name_invalid = {OPAL_JOBID_INVALID, OPAL_VPID_INVALID};

opal_process_info_t opal_process_info = {
    .my_name = {OPAL_JOBID_INVALID, OPAL_VPID_INVALID},
    .myprocid = {{0}, PMIX_RANK_INVALID},
    .nativelaunch = false,
    .nodename = NULL,
    .top_session_dir = NULL,
    .job_session_dir = NULL,
    .proc_session_dir = NULL,
    .num_local_peers = 0, /* there is nobody else but me */
    .my_local_rank = 0,   /* I'm the only process around here */
    .my_node_rank = 0,
    .cpuset = NULL,
    .locality = NULL,
    .pid = 0,
    .num_procs = 0,
    .app_num = 0,
    .univ_size = 0,
    .app_sizes = NULL,
    .app_ldrs = NULL,
    .command = NULL,
    .num_apps = 0,
    .initial_wdir = NULL,
    .reincarnation = 0,
    .proc_is_bound = false,
    .initial_errhandler = NULL,
};

static opal_proc_t opal_local_proc = {{.opal_list_next = NULL, .opal_list_prev = NULL},
                                      .proc_name = {OPAL_JOBID_INVALID, OPAL_VPID_INVALID},
                                      .proc_arch = 0,
                                      .proc_flags = 0,
                                      .proc_convertor = NULL};
static opal_proc_t *opal_proc_my_name = &opal_local_proc;

static void opal_proc_construct(opal_proc_t *proc)
{
    proc->proc_arch = opal_local_arch;
    proc->proc_convertor = NULL;
    proc->proc_flags = 0;
    proc->proc_name = *OPAL_NAME_INVALID;
}

static void opal_proc_destruct(opal_proc_t *proc)
{
    proc->proc_flags = 0;
    proc->proc_name = *OPAL_NAME_INVALID;
    proc->proc_convertor = NULL;
}

OBJ_CLASS_INSTANCE(opal_proc_t, opal_list_item_t, opal_proc_construct, opal_proc_destruct);

OBJ_CLASS_INSTANCE(opal_namelist_t, opal_list_item_t, NULL, NULL);

static int opal_compare_opal_procs(const opal_process_name_t p1, const opal_process_name_t p2)
{
    if (p1.jobid < p2.jobid) {
        return -1;
    }
    if (p1.jobid > p2.jobid) {
        return 1;
    }
    if (p1.vpid < p2.vpid) {
        return -1;
    }
    if (p1.vpid > p2.vpid) {
        return 1;
    }
    return 0;
}

opal_compare_proc_fct_t opal_compare_proc = opal_compare_opal_procs;

opal_proc_t *opal_proc_local_get(void)
{
    return opal_proc_my_name;
}

int opal_proc_local_set(opal_proc_t *proc)
{
    if (proc != opal_proc_my_name) {
        if (NULL != proc)
            OBJ_RETAIN(proc);
        if (&opal_local_proc != opal_proc_my_name) {
            OBJ_RELEASE(opal_proc_my_name);
        }
        if (NULL != proc) {
            opal_proc_my_name = proc;
        } else {
            opal_proc_my_name = &opal_local_proc;
        }
    }
    return OPAL_SUCCESS;
}

/* this function is used to temporarily set the local
 * name while OPAL and upper layers are initializing,
 * thus allowing debug messages to be more easily
 * understood */
void opal_proc_set_name(opal_process_name_t *name)
{
    /* to protect alignment, copy the name across */
    memcpy(&opal_local_proc.proc_name, name, sizeof(opal_process_name_t));
}

/**
 * The following functions are surrogates for the RTE functionality, and are not supposed
 * to be called. Instead, the corresponding function pointer should be set by the upper layer
 * before the call to opal_init, to make them point to the correct accessors based on the
 * underlying RTE.
 */
static char *opal_process_name_print_should_never_be_called(const opal_process_name_t procname)
{
    return "My Name is Nobody";
}

static char *opal_vpid_print_should_never_be_called(const opal_vpid_t unused)
{
    return "My VPID";
}

static char *opal_jobid_print_should_never_be_called(const opal_jobid_t unused)
{
    return "My JOBID";
}

static int opal_convert_string_to_process_name_should_never_be_called(opal_process_name_t *name,
                                                                      const char *name_string)
{
    return OPAL_ERR_NOT_SUPPORTED;
}

static int
opal_convert_process_name_to_string_should_never_be_called(char **name_string,
                                                           const opal_process_name_t *name)
{
    return OPAL_ERR_NOT_SUPPORTED;
}

static int opal_snprintf_jobid_should_never_be_called(char *name_string, size_t size,
                                                      opal_jobid_t jobid)
{
    (void) opal_string_copy(name_string, "My JOBID", size);
    return OPAL_SUCCESS;
}

static int opal_convert_string_to_jobid_should_never_be_called(opal_jobid_t *jobid,
                                                               const char *jobid_string)
{
    return OPAL_ERR_NOT_SUPPORTED;
}

static struct opal_proc_t *opal_proc_for_name_should_never_be_called(opal_process_name_t name)
{
    return NULL;
}

/**
 * Return the hostname of the target proc. The default implementation
 * only support the current proc. Once a RTE is initialized it must replace
 * the default function with one that can handle multiple, potentially
 * distributed, processes.
 */
static char*
opal_get_proc_hostname_local_only(const opal_proc_t *proc)
{
    /* if the proc is NULL, then we can't know */
    if (NULL == proc) {
        return strdup("unknown");
    }

    /* if it is my own hostname we are after, then just hand back
     * the value in opal_process_info */
    if (proc == opal_proc_my_name) {
        return strdup(opal_process_info.nodename);
    }
    return strdup("unknown");
}

char *(*opal_process_name_print)(const opal_process_name_t)
    = opal_process_name_print_should_never_be_called;
char *(*opal_vpid_print)(const opal_vpid_t) = opal_vpid_print_should_never_be_called;
char *(*opal_jobid_print)(const opal_jobid_t) = opal_jobid_print_should_never_be_called;
int (*opal_convert_string_to_process_name)(opal_process_name_t *name, const char *name_string)
    = opal_convert_string_to_process_name_should_never_be_called;
int (*opal_convert_process_name_to_string)(char **name_string, const opal_process_name_t *name)
    = opal_convert_process_name_to_string_should_never_be_called;
int (*opal_snprintf_jobid)(char *name_string, size_t size, opal_jobid_t jobid)
    = opal_snprintf_jobid_should_never_be_called;
int (*opal_convert_string_to_jobid)(opal_jobid_t *jobid, const char *jobid_string)
    = opal_convert_string_to_jobid_should_never_be_called;
struct opal_proc_t *(*opal_proc_for_name)(const opal_process_name_t name)
    = opal_proc_for_name_should_never_be_called;
char *(*opal_get_proc_hostname)(const opal_proc_t *proc)
    = opal_get_proc_hostname_local_only;

