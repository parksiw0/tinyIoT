#include <stdlib.h>
#include <regex.h>
#include <time.h>
#include "../onem2m.h"
#include "../logger.h"
#include "../util.h"
#include "../dbmanager.h"
#include "../config.h"

extern ResourceTree *rt;
extern cJSON *ATTRIBUTES;
extern pthread_mutex_t main_lock;

static int apply_max_instance_age(cJSON *cnt, cJSON *cin)
{
    cJSON *mia = cJSON_GetObjectItem(cnt, "mia");
    cJSON *ct = cJSON_GetObjectItem(cin, "ct");
    cJSON *et = cJSON_GetObjectItem(cin, "et");
    if (!cJSON_IsNumber(mia) || mia->valueint <= 0)
        return 1;
    if (!cJSON_IsString(ct) || !cJSON_IsString(et))
        return 0;

    int year, month, day, hour, minute, second, millisecond = 0;
    int parsed = sscanf(ct->valuestring, "%4d%2d%2dT%2d%2d%2d,%3d",
                        &year, &month, &day, &hour, &minute, &second, &millisecond);
    if (parsed < 6)
        return 0;

    struct tm created_tm = {0};
    created_tm.tm_year = year - 1900;
    created_tm.tm_mon = month - 1;
    created_tm.tm_mday = day;
    created_tm.tm_hour = hour;
    created_tm.tm_min = minute;
    created_tm.tm_sec = second;
    created_tm.tm_isdst = -1;
    time_t expires = mktime(&created_tm);
    if (expires == (time_t)-1)
        return 0;
    expires += mia->valueint;

    struct tm expires_tm;
    if (!localtime_r(&expires, &expires_tm))
        return 0;
    char max_et[25] = {0};
    size_t len = strftime(max_et, sizeof(max_et), "%Y%m%dT%H%M%S", &expires_tm);
    if (len == 0 || snprintf(max_et + len, sizeof(max_et) - len, ",%03d", millisecond) < 0)
        return 0;
    if (strcmp(et->valuestring, max_et) > 0)
    {
        cJSON *limited_et = cJSON_CreateString(max_et);
        if (!limited_et)
            return 0;
        if (!cJSON_ReplaceItemInObject(cin, "et", limited_et))
        {
            cJSON_Delete(limited_et);
            return 0;
        }
    }
    return 1;
}

int create_cin(oneM2MPrimitive *o2pt, RTNode *parent_rtnode)
{
    (void)parent_rtnode;
    char parent_ri[64] = {0};
    char parent_uri[MAX_URI_SIZE + 1] = {0};
    bool mutation_locked = false;
    bool main_locked = false;
    bool tx_active = false;
    bool cnt_persisted = false;
    cJSON *root = NULL;
    cJSON *cin = NULL;
    cJSON *parent_obj = NULL;
    cJSON *latest_obj = NULL;
    char *latest_alias = NULL;
    RTNode parent_snapshot = {0};
    RTNode *cin_rtnode = NULL;
    int final_cni = -1;

#if MONO_THREAD == 0
    pthread_mutex_lock(&main_lock);
    main_locked = true;
#endif
    RTNode *live_parent = get_rtnode(o2pt);
    if (!live_parent || live_parent->ty != RT_CNT || !get_ri_rtnode(live_parent))
    {
        handle_error(o2pt, RSC_NOT_FOUND, "CNT parent no longer exists");
        goto cleanup;
    }
    snprintf(parent_ri, sizeof(parent_ri), "%s", get_ri_rtnode(live_parent));
#if MONO_THREAD == 0
    pthread_mutex_unlock(&main_lock);
    main_locked = false;
#endif

    cnt_mutation_lock(parent_ri);
    mutation_locked = true;
#if MONO_THREAD == 0
    pthread_mutex_lock(&main_lock);
    main_locked = true;
#endif
    live_parent = get_rtnode(o2pt);
    if (!live_parent || live_parent->ty != RT_CNT || !get_ri_rtnode(live_parent) ||
        strcmp(parent_ri, get_ri_rtnode(live_parent)) != 0)
    {
        handle_error(o2pt, RSC_NOT_FOUND, "CNT parent no longer exists");
        goto cleanup;
    }
    if (snprintf(parent_uri, sizeof(parent_uri), "%s", get_uri_rtnode(live_parent)) >= (int)sizeof(parent_uri))
    {
        handle_error(o2pt, RSC_BAD_REQUEST, "CNT URI is too long");
        goto cleanup;
    }
    parent_obj = cJSON_Duplicate(live_parent->obj, 1);
    parent_snapshot.uri = strdup(parent_uri);
    if (!parent_obj || !parent_snapshot.uri)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "Unable to snapshot CNT parent");
        goto cleanup;
    }
    parent_snapshot.obj = parent_obj;
    parent_snapshot.ty = RT_CNT;
#if MONO_THREAD == 0
    pthread_mutex_unlock(&main_lock);
    main_locked = false;
#endif

    root = cJSON_Duplicate(o2pt->request_pc, 1);
    cin = root ? cJSON_GetObjectItem(root, "m2m:cin") : NULL;
    if (!cin)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "Unable to allocate CIN");
        goto cleanup;
    }

    cJSON *rn = NULL;
#if !ALLOW_CIN_RN
    rn = cJSON_GetObjectItem(cin, "rn");
    if (rn != NULL)
    {
        handle_error(o2pt, RSC_BAD_REQUEST, "rn attribute for cin is assigned by CSE");
        goto cleanup;
    }
#endif

    add_general_attribute(cin, &parent_snapshot, RT_CIN);
    if (!apply_max_instance_age(parent_obj, cin))
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "Unable to apply CNT maxInstanceAge");
        goto cleanup;
    }

    // Add the contentSize attribute.
    cJSON *con = cJSON_GetObjectItem(cin, "con");
    if (cJSON_IsString(con))
        cJSON_AddNumberToObject(cin, "cs", strlen(cJSON_GetStringValue(con)));

    // The final stateTag is assigned atomically with the parent update below.
    cJSON *st = cJSON_GetObjectItem(parent_obj, "st");
    if (!st)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT stateTag is missing");
        goto cleanup;
    }
    cJSON_AddNumberToObject(cin, "st", st->valueint);

    int rsc = validate_cin(o2pt, parent_obj, cin, OP_CREATE);
    if (rsc != RSC_OK)
        goto cleanup;

    cJSON *pjson = NULL;
    if ((pjson = cJSON_GetObjectItem(cin, "cr")))
    {
        if (pjson->type == cJSON_NULL)
        {
            cJSON_DeleteItemFromObject(cin, "cr");
            cJSON_AddStringToObject(cin, "cr", o2pt->fr);
        }
        else
        {
            handle_error(o2pt, RSC_BAD_REQUEST, "creator attribute with arbitary value is not allowed");
            goto cleanup;
        }
    }
#if CSE_RVI >= RVI_3
    bool parent_was_announced = false;
    cJSON *final_at = cJSON_CreateArray();
    if (!final_at)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "Unable to allocate announced attribute list");
        goto cleanup;
    }
    cJSON *parent_at = cJSON_GetObjectItem(parent_obj, "at");
    if (parent_at && cJSON_GetArraySize(parent_at) > 0)
        parent_was_announced = true;

    if (parent_was_announced)
    {
        if (handle_annc_create(&parent_snapshot, cin, cJSON_GetObjectItem(cin, "at"), final_at) == -1)
        {
            cJSON_Delete(final_at);
            handle_error(o2pt, RSC_BAD_REQUEST, "invalid attribute in `aa`");
            goto cleanup;
        }
        cJSON_DeleteItemFromObject(cin, "at");
        cJSON_AddItemToObject(cin, "at", final_at);
    }
    else
    {
        cJSON *at = cJSON_GetObjectItem(cin, "at");
        cJSON_Delete(final_at);
        if (at && cJSON_GetArraySize(at) > 0)
        {
            handle_error(o2pt, RSC_BAD_REQUEST, "cinA can't be announced alone");
            goto cleanup;
        }
    }
#endif

    cin_rtnode = create_rtnode(cin, RT_CIN);
    if (!cin_rtnode)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "Unable to allocate CIN node");
        goto cleanup;
    }
    cJSON_DetachItemFromObject(root, "m2m:cin");

    char ptr[MAX_URI_SIZE + 1] = {0};
    rn = cJSON_GetObjectItem(cin, "rn");
    if (!rn || snprintf(ptr, sizeof(ptr), "%s/%s", parent_uri, rn->valuestring) >= (int)sizeof(ptr))
    {
        handle_error(o2pt, RSC_BAD_REQUEST, "CIN URI is too long");
        goto cleanup;
    }

    // Reserve everything needed to update the in-memory latest cache before
    // committing the DB transaction. After commit, only ownership is moved.
    latest_obj = cJSON_Duplicate(cin, 1);
    latest_alias = strdup("la");
    if (!latest_obj || !latest_alias)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "Unable to allocate latest CIN cache");
        goto cleanup;
    }

    // The per-CNT mutation lock keeps this snapshot stable while the DB
    // transaction runs, so PostgreSQL work does not hold the global tree lock.
    if (update_cnt_cin_memory(&parent_snapshot, cin_rtnode, 1) != 0)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT update fail");
        goto cleanup;
    }
    if (!db_begin_tx())
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "DB transaction start failed");
        goto cleanup;
    }
    tx_active = true;

    if (db_store_resource(cin, ptr) != 1)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "DB store fail");
        goto cleanup;
    }
    // oneM2M retention is evaluated after reflecting the new CIN. This also
    // makes mni=0 remove the just-created (and therefore oldest) CIN.
    if (delete_cin_under_cnt_mni_mbs(&parent_snapshot) != 0)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT retention fail");
        goto cleanup;
    }
    cJSON *snapshot_cni = cJSON_GetObjectItem(parent_obj, "cni");
    if (!cJSON_IsNumber(snapshot_cni))
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT counters are missing");
        goto cleanup;
    }
    final_cni = snapshot_cni->valueint;

    // cni/cbs can normally be recounted after a crash. If retention leaves no
    // CIN, no child stateTag remains from which to recover the parent's st.
    if ((CNT_FLUSH_MS <= 0 || final_cni == 0) &&
        db_update_resource(parent_obj, parent_ri, RT_CNT) != 1)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT persistence fail");
        goto cleanup;
    }
    cnt_persisted = CNT_FLUSH_MS <= 0 || final_cni == 0;

    int committed = db_commit_tx();
    tx_active = false;
    if (!committed)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "DB commit fail");
        goto cleanup;
    }

#if MONO_THREAD == 0
    pthread_mutex_lock(&main_lock);
    main_locked = true;
#endif
    live_parent = get_rtnode(o2pt);
    if (!live_parent || live_parent->ty != RT_CNT || !get_ri_rtnode(live_parent) ||
        strcmp(parent_ri, get_ri_rtnode(live_parent)) != 0)
    {
#if MONO_THREAD == 0
        pthread_mutex_unlock(&main_lock);
        main_locked = false;
#endif
        // The CIN committed after its parent was concurrently removed; remove the orphan row.
        db_delete_onem2m_resource(cin_rtnode);
        handle_error(o2pt, RSC_NOT_FOUND, "CNT parent no longer exists");
        goto cleanup;
    }

    cJSON *live_cni = cJSON_GetObjectItem(live_parent->obj, "cni");
    cJSON *live_cbs = cJSON_GetObjectItem(live_parent->obj, "cbs");
    cJSON *live_st = cJSON_GetObjectItem(live_parent->obj, "st");
    cJSON *snapshot_cbs = cJSON_GetObjectItem(parent_obj, "cbs");
    cJSON *snapshot_st = cJSON_GetObjectItem(parent_obj, "st");
    if (!cJSON_IsNumber(live_cni) || !cJSON_IsNumber(live_cbs) || !cJSON_IsNumber(live_st) ||
        !cJSON_IsNumber(snapshot_cbs) || !cJSON_IsNumber(snapshot_st))
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT counters are missing");
        goto cleanup;
    }
    cJSON_SetIntValue(live_cni, final_cni);
    cJSON_SetIntValue(live_cbs, snapshot_cbs->valueint);
    cJSON_SetIntValue(live_st, snapshot_st->valueint);

    if (CNT_FLUSH_MS > 0 && !cnt_persisted)
        cnt_flush_mark(live_parent);

    make_response_body(o2pt, cin_rtnode);
    o2pt->rsc = RSC_CREATED;

    if (final_cni == 0)
    {
        free_rtnode(cin_rtnode);
        cin_rtnode = NULL;
        goto cleanup;
    }

    RTNode *rtnode = live_parent->child;
    if (!rtnode)
    {
        if (cin_rtnode->rn)
            free(cin_rtnode->rn);
        cin_rtnode->rn = latest_alias;
        latest_alias = NULL;
        add_child_resource_tree(live_parent, cin_rtnode);
        cin_rtnode = NULL;
    }
    else
    {
        while (rtnode && strcmp(rtnode->rn, "la") != 0)
        {
            rtnode = rtnode->sibling_right;
        }
        if (!rtnode)
        {
            if (cin_rtnode->rn)
                free(cin_rtnode->rn);
            cin_rtnode->rn = latest_alias;
            latest_alias = NULL;
            add_child_resource_tree(live_parent, cin_rtnode);
            cin_rtnode = NULL;
        }
        else
        {
            cJSON_Delete(rtnode->obj);
            rtnode->obj = latest_obj;
            latest_obj = NULL;
            free_rtnode(cin_rtnode);
            cin_rtnode = NULL;
        }
    }

cleanup:
    if (tx_active)
    {
        db_rollback_tx();
        tx_active = false;
    }
    if (cin_rtnode)
        free_rtnode(cin_rtnode);
#if MONO_THREAD == 0
    if (main_locked)
        pthread_mutex_unlock(&main_lock);
#endif
    if (root)
        cJSON_Delete(root);
    if (parent_obj)
        cJSON_Delete(parent_obj);
    cJSON_Delete(latest_obj);
    free(latest_alias);
    free(parent_snapshot.uri);
    if (mutation_locked)
        cnt_mutation_unlock(parent_ri);

    return o2pt->rsc;
}

bool validate_cnf(const char *cnf) 
{
    regex_t regex;
    int reti;
    const char *pattern = "^[^:/]+/[^:/]+:[0-2](:[0-5])?$";
    reti = regcomp(&regex, pattern, REG_EXTENDED);
    if (reti) {
        return false;
    }
    reti = regexec(&regex, cnf, 0, NULL, 0);
    regfree(&regex);
    return (reti == 0);
}

int validate_cin(oneM2MPrimitive *o2pt, cJSON *parent_cnt, cJSON *cin, Operation op)
{
    cJSON *pjson = NULL, *pjson2 = NULL;
    char *ptr = NULL;

    cJSON *mbs = NULL;
    cJSON *cs = NULL;

    if ((pjson = cJSON_GetObjectItem(cin, "rn")))
    {
        if (!strcmp(pjson->valuestring, "la") || !strcmp(pjson->valuestring, "latest"))
        {
            handle_error(o2pt, RSC_NOT_ACCEPTABLE, "attribute `rn` is invalid");
            return RSC_BAD_REQUEST;
        }
        if (!strcmp(pjson->valuestring, "ol") || !strcmp(pjson->valuestring, "oldest"))
        {
            handle_error(o2pt, RSC_NOT_ACCEPTABLE, "attribute `rn` is invalid");
            return RSC_BAD_REQUEST;
        }
    }

    if ((pjson = cJSON_GetObjectItem(cin, "acpi")))
    {
        return handle_error(o2pt, RSC_BAD_REQUEST, "attribute `acpi` for `cin` is not supported");
    }

    if ((mbs = cJSON_GetObjectItem(parent_cnt, "mbs")))
    {
        logger("CIN", LOG_LEVEL_DEBUG, "mbs %d", mbs->valueint);
        if ((cs = cJSON_GetObjectItem(cin, "cs")))
        {
            logger("CIN", LOG_LEVEL_DEBUG, "cs %d", cs->valueint);
            if (mbs->valueint >= 0 && cs->valueint > mbs->valueint)
            {
                return handle_error(o2pt, RSC_NOT_ACCEPTABLE, "contentInstance size exceed `mbs`");
            }
        }
    }

    if ((pjson = cJSON_GetObjectItem(cin, "cnf")))
    {
        // cnf check
        if (!validate_cnf(pjson->valuestring)) {
            return handle_error(o2pt, RSC_BAD_REQUEST, "attribute `cnf` is invalid");
        }
    }
    cJSON *aa = cJSON_GetObjectItem(cin, "aa");
    if (aa && CSE_RVI < RVI_3)
    {
        return handle_error(o2pt, RSC_BAD_REQUEST, "attribute `aa` is not supported");
    }
    cJSON *attr = cJSON_GetObjectItem(ATTRIBUTES, get_resource_key(RT_CIN));
    cJSON_ArrayForEach(pjson, aa)
    {
        if (strcmp(pjson->valuestring, "lbl") == 0)
            continue;
        if (strcmp(pjson->valuestring, "ast") == 0)
            continue;
        if (strcmp(pjson->valuestring, "lnk") == 0)
            continue;
        if (!cJSON_GetObjectItem(attr, pjson->valuestring))
        {
            return handle_error(o2pt, RSC_BAD_REQUEST, "invalid attribute in `aa`");
        }
    }

    return RSC_OK;
}
