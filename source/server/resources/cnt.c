#include <stdlib.h>
#include "../onem2m.h"
#include "../logger.h"
#include "../util.h"
#include "../dbmanager.h"
#include "../config.h"

extern ResourceTree *rt;
extern cJSON *ATTRIBUTES;
extern pthread_mutex_t main_lock;

int create_cnt(oneM2MPrimitive *o2pt, RTNode *parent_rtnode)
{
    cJSON *root = cJSON_Duplicate(o2pt->request_pc, 1);
    cJSON *pjson = NULL;

    cJSON *cnt = cJSON_GetObjectItem(root, "m2m:cnt");

    add_general_attribute(cnt, parent_rtnode, RT_CNT);

    int rsc = validate_cnt(o2pt, cnt, OP_CREATE);
    if (rsc != RSC_OK)
    {
        cJSON_Delete(root);
        return rsc;
    }

    // Add cr attribute
    if ((pjson = cJSON_GetObjectItem(cnt, "cr")))
    {
        if (pjson->type == cJSON_NULL)
        {
            cJSON_AddStringToObject(cnt, "cr", o2pt->fr);
        }
        else
        {
            handle_error(o2pt, RSC_BAD_REQUEST, "creator attribute with arbitary value is not allowed");
            cJSON_Delete(root);
            return o2pt->rsc;
        }
    }

    // Add st, cni, cbs, mni, mbs attribute
    cJSON_AddNumberToObject(cnt, "st", 0);
    cJSON_AddNumberToObject(cnt, "cni", 0);
    cJSON_AddNumberToObject(cnt, "cbs", 0);
#if CSE_RVI >= RVI_3
    bool parent_was_announced = false; 
    cJSON *final_at = cJSON_CreateArray();

    if (parent_rtnode->ty == RT_AE) //check parent resource(AE) was announced 
    {
        cJSON *parent_at = cJSON_GetObjectItem(parent_rtnode->obj, "at");
        if(parent_at && cJSON_GetArraySize(parent_at) > 0)
        {
            parent_was_announced = true;
        }
    }

    if (parent_was_announced)//when aeA is announced, cnt is also announced under aeA as cntA 
    {
        if (handle_annc_create(parent_rtnode, cnt, cJSON_GetObjectItem(cnt, "at"), final_at) == -1)
        {
            cJSON_Delete(root);
            cJSON_Delete(final_at);
            return handle_error(o2pt, RSC_BAD_REQUEST, "invalid attribute in `aa`");
        }

        if (cJSON_GetArraySize(final_at) > 0)
        {
            cJSON_DeleteItemFromObject(cnt, "at");
            cJSON_AddItemToObject(cnt, "at", final_at);
        }
        else
        {
            cJSON_Delete(final_at);
        }
    }
    else//when ae is not announced, cnt is announced under cbA as cntA 
    {
        if (handle_annc_create(parent_rtnode->parent, cnt, cJSON_GetObjectItem(cnt, "at"), final_at) == -1)
        {
            cJSON_Delete(root);
            cJSON_Delete(final_at);
            return handle_error(o2pt, RSC_BAD_REQUEST, "invalid attribute in `aa`");
        }

        if (cJSON_GetArraySize(final_at) > 0)
        {
            cJSON_DeleteItemFromObject(cnt, "at");
            cJSON_AddItemToObject(cnt, "at", final_at);
        }
        else
        {
            cJSON_Delete(final_at);
        }
    }
#endif
    if (rsc != RSC_OK)
    {
        cJSON_Delete(root);
        return rsc;
    }
    // add default mni if not set
    if (!cJSON_GetObjectItem(cnt, "mni"))
    {
        cJSON_AddNumberToObject(cnt, "mni", DEFAULT_MAX_NR_INSTANCES);
    }

    o2pt->rsc = RSC_CREATED;

    // Add uri attribute
    char ptr[MAX_URI_SIZE + 1] = {0};
    cJSON *rn = cJSON_GetObjectItem(cnt, "rn");
    if (!rn || snprintf(ptr, sizeof(ptr), "%s/%s", get_uri_rtnode(parent_rtnode), rn->valuestring) >= (int)sizeof(ptr))
    {
        cJSON_Delete(root);
        return handle_error(o2pt, RSC_BAD_REQUEST, "CNT URI is too long");
    }

    // Store to DB
    int result = db_store_resource(cnt, ptr);
    if (result != 1)
    {
        handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "DB store fail");
        cJSON_Delete(root);
        return o2pt->rsc;
    }

    RTNode *child_rtnode = create_rtnode(cnt, RT_CNT);
    add_child_resource_tree(parent_rtnode, child_rtnode);
    make_response_body(o2pt, child_rtnode);

    cJSON_DetachItemFromObject(root, "m2m:cnt");
    cJSON_Delete(root);

    return RSC_CREATED;
}

int update_cnt(oneM2MPrimitive *o2pt, RTNode *target_rtnode)
{
    char invalid_key[][9] = {"ty", "pi", "ri", "rn", "ct", "cr"};
    cJSON *m2m_cnt = cJSON_GetObjectItem(o2pt->request_pc, "m2m:cnt");
    int invalid_key_size = sizeof(invalid_key) / (9 * sizeof(char));
    char cnt_ri[64] = {0};
    bool mutation_locked = false;
    bool main_locked = false;
    bool tx_active = false;
    bool live_mutated = false;
    int result = RSC_OK;
    cJSON *cnt_snapshot_obj = NULL;
    RTNode cnt_snapshot = {0};

    int updateAttrCnt = cJSON_GetArraySize(m2m_cnt);

    for (int i = 0; i < invalid_key_size; i++)
    {
        if (cJSON_GetObjectItem(m2m_cnt, invalid_key[i]))
        {
            handle_error(o2pt, RSC_BAD_REQUEST, "unsupported attribute on update");
            return RSC_BAD_REQUEST;
        }
    }

#if MONO_THREAD == 0
    pthread_mutex_lock(&main_lock);
    main_locked = true;
    target_rtnode = get_rtnode(o2pt);
#endif
    if (!target_rtnode || target_rtnode->ty != RT_CNT || !get_ri_rtnode(target_rtnode))
    {
#if MONO_THREAD == 0
        pthread_mutex_unlock(&main_lock);
        main_locked = false;
#endif
        return handle_error(o2pt, RSC_NOT_FOUND, "CNT no longer exists");
    }
    snprintf(cnt_ri, sizeof(cnt_ri), "%s", get_ri_rtnode(target_rtnode));
#if MONO_THREAD == 0
    pthread_mutex_unlock(&main_lock);
    main_locked = false;
#endif

    cnt_mutation_lock(cnt_ri);
    mutation_locked = true;
#if MONO_THREAD == 0
    pthread_mutex_lock(&main_lock);
    main_locked = true;
    target_rtnode = get_rtnode(o2pt);
#endif
    if (!target_rtnode || target_rtnode->ty != RT_CNT || !get_ri_rtnode(target_rtnode) ||
        strcmp(cnt_ri, get_ri_rtnode(target_rtnode)) != 0)
    {
        result = handle_error(o2pt, RSC_NOT_FOUND, "CNT no longer exists");
        goto cleanup;
    }
    cnt_snapshot_obj = cJSON_Duplicate(target_rtnode->obj, 1);
    cnt_snapshot.uri = target_rtnode->uri ? strdup(target_rtnode->uri) : NULL;
    if (!cnt_snapshot_obj || !cnt_snapshot.uri)
    {
        result = handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "Unable to snapshot CNT");
        goto cleanup;
    }
    cnt_snapshot.obj = cnt_snapshot_obj;
    cnt_snapshot.ty = RT_CNT;
#if MONO_THREAD == 0
    if (!cJSON_GetObjectItem(m2m_cnt, "at"))
    {
        pthread_mutex_unlock(&main_lock);
        main_locked = false;
    }
#endif

    cJSON *cnt = cnt_snapshot_obj;
    cJSON *pjson = NULL;
    cJSON *acpi_obj = NULL;
    bool acpi_flag = false;

    result = validate_cnt(o2pt, m2m_cnt, OP_UPDATE);

    // update acpi
    if (cJSON_GetObjectItem(m2m_cnt, "acpi"))
    {

        // delete removed acpi
        cJSON_ArrayForEach(acpi_obj, cJSON_GetObjectItem(cnt, "acpi"))
        {
            acpi_flag = false;
            cJSON_ArrayForEach(pjson, cJSON_GetObjectItem(m2m_cnt, "acpi"))
            {
                if (strcmp(acpi_obj->valuestring, pjson->valuestring) != 0)
                {
                    acpi_flag = true;
                    break;
                }
            }
            if (!acpi_flag)
            {
                logger("UTIL", LOG_LEVEL_INFO, "acpi %s", acpi_obj->valuestring);
                if (!has_acpi_update_privilege(o2pt, acpi_obj->valuestring))
                {
                    result = handle_error(o2pt, RSC_ORIGINATOR_HAS_NO_PRIVILEGE, "no privilege to update acpi");
                    goto cleanup;
                }
            }
        }

        // validate new acpi
        if (cJSON_GetArraySize(cJSON_GetObjectItem(m2m_cnt, "acpi")) > 0)
        {
            if (validate_acpi(o2pt, cJSON_GetObjectItem(m2m_cnt, "acpi"), ACOP_UPDATE) != RSC_OK)
            {
                result = handle_error(o2pt, RSC_BAD_REQUEST, "no privilege to update acpi");
                goto cleanup;
            }
        }
    }

    if (result != RSC_OK)
        goto cleanup;

    cJSON_AddNumberToObject(m2m_cnt, "st", cJSON_GetObjectItem(cnt, "st")->valueint + 1);

    cJSON *at = NULL;
    if ((at = cJSON_GetObjectItem(m2m_cnt, "at")))
    {
        cJSON *final_at = cJSON_CreateArray();
        handle_annc_update(target_rtnode, at, final_at);
        cJSON_DeleteItemFromObject(m2m_cnt, "at");
        cJSON_AddItemToObject(m2m_cnt, "at", final_at);
    }

    char *lt = get_local_time(0);
    cJSON_AddItemToObject(m2m_cnt, "lt", cJSON_CreateString(lt));
    free(lt);

#if MONO_THREAD == 0
    if (!main_locked)
    {
        pthread_mutex_lock(&main_lock);
        main_locked = true;
        target_rtnode = get_rtnode(o2pt);
    }
#endif
    if (!target_rtnode || target_rtnode->ty != RT_CNT || !get_ri_rtnode(target_rtnode) ||
        strcmp(cnt_ri, get_ri_rtnode(target_rtnode)) != 0)
    {
        result = handle_error(o2pt, RSC_NOT_FOUND, "CNT no longer exists");
        goto cleanup;
    }

    if (!db_begin_tx())
    {
        result = handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "DB transaction start failed");
        goto cleanup;
    }
    tx_active = true;

    update_resource(target_rtnode->obj, m2m_cnt);
    live_mutated = true;

    if (delete_cin_under_cnt_mni_mbs(target_rtnode) != 0)
    {
        result = handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT retention fail");
        goto cleanup;
    }

    // Persist the complete post-retention snapshot so cni/cbs and the policy
    // update commit atomically with any CIN rows removed above.
    result = db_update_resource(target_rtnode->obj,
                                cJSON_GetObjectItem(target_rtnode->obj, "ri")->valuestring,
                                RT_CNT);
    if (result != 1)
    {
        result = handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "CNT persistence fail");
        goto cleanup;
    }
    if (!db_commit_tx())
    {
        tx_active = false;
        result = handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "DB commit fail");
        goto cleanup;
    }
    tx_active = false;

    for (int i = 0; i < updateAttrCnt; i++)
    {
        cJSON_DeleteItemFromArray(m2m_cnt, 0);
    }

    make_response_body(o2pt, target_rtnode);
    o2pt->rsc = RSC_UPDATED;
    result = RSC_UPDATED;

cleanup:
    if (tx_active)
    {
        db_rollback_tx();
        tx_active = false;
    }
    if (live_mutated && result != RSC_UPDATED && target_rtnode && cnt_snapshot_obj)
    {
        cJSON *restored = cJSON_Duplicate(cnt_snapshot_obj, 1);
        if (restored)
        {
            cJSON_Delete(target_rtnode->obj);
            target_rtnode->obj = restored;
        }
        else
        {
            logger("CNT", LOG_LEVEL_ERROR, "Unable to restore CNT after persistence failure");
        }
    }
#if MONO_THREAD == 0
    if (main_locked)
        pthread_mutex_unlock(&main_lock);
#endif
    if (mutation_locked)
        cnt_mutation_unlock(cnt_ri);
    if (cnt_snapshot_obj)
        cJSON_Delete(cnt_snapshot_obj);
    free(cnt_snapshot.uri);
    return result;
}

int validate_cnt(oneM2MPrimitive *o2pt, cJSON *cnt, Operation op)
{
    cJSON *pjson = NULL;
    char *ptr = NULL;
    if (!cnt)
    {
        if (o2pt->rvi >= 3)
            return handle_error(o2pt, RSC_CONTENTS_UNACCEPTABLE, "insufficient mandatory attribute(s)");
        else
            return handle_error(o2pt, RSC_BAD_REQUEST, "insufficient mandatory attribute(s)");
    }

    pjson = cJSON_GetObjectItem(cnt, "rn");
    if (pjson)
    {
        if (!strcmp(pjson->valuestring, "la") || !strcmp(pjson->valuestring, "ol"))
        {
            handle_error(o2pt, RSC_OPERATION_NOT_ALLOWED, "attribute `rn` is invalid");
            return RSC_BAD_REQUEST;
        }

        if (!strcmp(pjson->valuestring, "latest") || !strcmp(pjson->valuestring, "oldest"))
        {
            handle_error(o2pt, RSC_OPERATION_NOT_ALLOWED, "attribute `rn` is invalid");
            return RSC_BAD_REQUEST;
        }
    }

    if (op == OP_CREATE)
    {
        pjson = cJSON_GetObjectItem(cnt, "acpi");
        if (pjson && cJSON_GetArraySize(pjson) > 0)
        {
            int result = validate_acpi(o2pt, pjson, ACOP_CREATE);
            if (result != RSC_OK)
                return result;
        }
    }

    if (op == OP_UPDATE)
    {
        pjson = cJSON_GetObjectItem(cnt, "acpi");
        if (pjson && cJSON_GetArraySize(cnt) > 1)
        {
            handle_error(o2pt, RSC_BAD_REQUEST, "only attribute `acpi` is allowed when updating `acpi`");
            return RSC_BAD_REQUEST;
        }
    }

    pjson = cJSON_GetObjectItem(cnt, "mia");
    if (pjson && pjson->valueint < 0)
    {
        handle_error(o2pt, RSC_BAD_REQUEST, "attribute `mia` is invalid");
        return RSC_BAD_REQUEST;
    }

    pjson = cJSON_GetObjectItem(cnt, "mni");
    if (pjson && pjson->valueint < 0)
    {
        handle_error(o2pt, RSC_BAD_REQUEST, "attribute `mni` is invalid");
        return RSC_BAD_REQUEST;
    }

    pjson = cJSON_GetObjectItem(cnt, "mbs");
    if (pjson && pjson->valueint < 0)
    {
        handle_error(o2pt, RSC_BAD_REQUEST, "attribute `mbs` is invalid");
        return RSC_BAD_REQUEST;
    }

    cJSON *aa = cJSON_GetObjectItem(cnt, "aa");
    cJSON *attr = cJSON_GetObjectItem(ATTRIBUTES, get_resource_key(RT_CNT));
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
