/*
 *  oFono - Open Source Telephony - binder based adaptation QTI plugin
 *
 *  Copyright (C) 2022 Jolla Ltd.
 *  Copyright (C) 2024 TheKit <thekit@disroot.org>
 *  Copyright (C) 2024 Marius Gripsgard <marius@ubports.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <glib-object.h>

#include "qti_ims_call.h"
#include "qti_radio_ext.h"
#include "qti_radio_ext_types.h"

#include <binder_ext_call_impl.h>
#include <gbinder.h>

#include <ofono/log.h>

#include <gutil_idlepool.h>
#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_log.h>

#undef DBG
#define DBG(fmt, ...) \
    gutil_log(GLOG_MODULE_CURRENT, GLOG_LEVEL_ALWAYS, "ims:"fmt, ##__VA_ARGS__)

typedef GObjectClass QtiImsCallClass;
typedef struct qti_ims_call {
    GObject parent;
    GUtilIdlePool* pool;
    QtiRadioExt* radio_ext;
    GPtrArray* calls;
    GHashTable* id_map;
    gboolean ringback_tone; /* last value passed to oFono */
    /*
     * IMS conference. The modem merges its ACTIVE and HOLDING call into
     * a new isMpty call and ends the merged calls. oFono models a
     * conference as several calls in its multiparty list, so the merged
     * calls stay in self->calls as members that follow the state of the
     * isMpty call. A new isMpty call is flagged BINDER_EXT_CALL_FLAG_MPTY,
     * which ofono-binder-plugin does not report to the oFono core.
     */
    guint conf_call_id;       /* modem id of the isMpty call, 0 if none */
    GHashTable* conf_members; /* call id -> CONF_MEMBER_LIVE/GONE */
    GHashTable* conf_pending; /* the same, while a merge is in progress */
    guint conf_check_id;      /* timeout source */
} QtiImsCall;

static
void
qti_ims_call_iface_init(
    BinderExtCallInterface* iface);

GType qti_ims_call_get_type() G_GNUC_INTERNAL;
G_DEFINE_TYPE_WITH_CODE(QtiImsCall, qti_ims_call, G_TYPE_OBJECT,
G_IMPLEMENT_INTERFACE(BINDER_EXT_TYPE_CALL, qti_ims_call_iface_init))

#define THIS_TYPE qti_ims_call_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, QtiImsCall)
#define PARENT_CLASS qti_ims_call_parent_class

#define ID_KEY(id) GUINT_TO_POINTER(id)
#define ID_VALUE(id) GUINT_TO_POINTER(id)

#define CONF_MEMBER_LIVE GUINT_TO_POINTER(1) /* the modem still has the call */
#define CONF_MEMBER_GONE GUINT_TO_POINTER(2) /* merged, kept for oFono */
#define CONF_CHECK_TIMEOUT_SEC (5)

typedef struct qti_ims_call_result_request {
    int ref_count;
    guint id;
    guint id_mapped;
    guint param;
    BinderExtCall* ext;
    BinderExtCallResultFunc complete;
    GDestroyNotify destroy;
    void* user_data;
} QtiImsCallResultRequest;

typedef struct qti_ims_call_swap_second_request {
    gboolean step1_success;
    BINDER_EXT_CALL_ANSWER_FLAGS answer_flags;
    guint call_hold;
    guint call_incoming;
    BinderExtCall *ext;
    BinderExtCallResultFunc complete;
    GDestroyNotify destroy;
    void *user_data;
} QtiImsCallSwapSecondRequest;

enum qti_ims_call_signal {
    SIGNAL_CALL_STATE_CHANGED,
    SIGNAL_CALL_END,
    SIGNAL_CALL_RING,
    SIGNAL_CALL_SUPP_SVC_NOTIFY,
    SIGNAL_CALL_RINGBACK_TONE,
    SIGNAL_COUNT
};

#define SIGNAL_CALL_STATE_CHANGED_NAME    "qti-ims-call-state-changed"
#define SIGNAL_CALL_END_NAME              "qti-ims-call-end"
#define SIGNAL_CALL_RING_NAME             "qti-ims-call-ring"
#define SIGNAL_CALL_SUPP_SVC_NOTIFY_NAME  "qti-ims-call-supp-svc-notify"
#define SIGNAL_CALL_RINGBACK_TONE_NAME    "qti-ims-call-ringback-tone"

static guint qti_ims_call_signals[SIGNAL_COUNT] = { 0 };

static
QtiImsCallResultRequest*
qti_ims_call_result_request_new(
    BinderExtCall* ext,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCallResultRequest* req =
        g_slice_new0(QtiImsCallResultRequest);

    req->ref_count = 1;
    req->ext = binder_ext_call_ref(ext);
    req->complete = complete;
    req->destroy = destroy;
    req->user_data = user_data;
    return req;
}

static
void
qti_ims_call_result_request_free(
    QtiImsCallResultRequest* req)
{
    BinderExtCall* ext = req->ext;

    if (req->destroy) {
        req->destroy(req->user_data);
    }
    if (req->id_mapped) {
        g_hash_table_remove(THIS(ext)->id_map, ID_KEY(req->id_mapped));
    }
    binder_ext_call_unref(ext);
    gutil_slice_free(req);
}

static
gboolean
qti_ims_call_result_request_unref(
    QtiImsCallResultRequest* req)
{
    if (!--(req->ref_count)) {
        qti_ims_call_result_request_free(req);
        return TRUE;
    } else {
        return FALSE;
    }
}

static
void
qti_ims_call_result_request_destroy(
    gpointer req)
{
    qti_ims_call_result_request_unref(req);
}

/*==========================================================================*
 * BinderExtCallInterface
 *==========================================================================*/

static
BinderExtCallInfo*
qti_ims_call_info_new(const QtiRadioCallInfo* info)
{
    const gsize number_len = (info->number && *info->number) ?
        strlen(info->number) : 0;
    const gsize name_len = (info->name && *info->name) ?
        strlen(info->name) : 0;

    const gsize total = G_ALIGN8(sizeof(BinderExtCallInfo)) +
        (number_len ? G_ALIGN8(number_len + 1) : 0) +
        (name_len ? G_ALIGN8(name_len + 1) : 0);

    BinderExtCallInfo* dest = g_malloc0(total);
    char* ptr = ((char*)dest) + G_ALIGN8(sizeof(BinderExtCallInfo));

    dest->call_id = info->index;
    dest->state = qti_radio_ims_call_radio_state_to_state(info->state);
    dest->type = BINDER_EXT_CALL_TYPE_VOICE;
    dest->flags = BINDER_EXT_CALL_FLAG_IMS | BINDER_EXT_CALL_FLAG_INCOMING;
    dest->toa = info->toa;

    // Copy number if present
    if (number_len) {
        dest->number = ptr;
        memcpy(ptr, info->number, number_len);
        ptr += G_ALIGN8(number_len + 1);
    } else {
        dest->number = NULL;
    }

    // Copy name if present
    if (name_len) {
        dest->name = ptr;
        memcpy(ptr, info->name, name_len);
        ptr += G_ALIGN8(name_len + 1);
    } else {
        dest->name = NULL;
    }

    return dest;
}

// find call by id
static
BinderExtCallInfo*
qti_ims_call_info_find(
    QtiImsCall* self,
    guint call_id)
{
    for (int i = 0; i < self->calls->len; i++) {
        BinderExtCallInfo* info =
            (BinderExtCallInfo*) g_ptr_array_index(self->calls, i);
        if (info->call_id == call_id) {
            return info;
        }
    }
    return NULL;
}

static
BinderExtCallInfo*
qti_ims_call_info_find_by_state(
    QtiImsCall* self,
    BINDER_EXT_CALL_STATE state)
{
    for (int i = 0; i < self->calls->len; i++) {
        BinderExtCallInfo* info = (BinderExtCallInfo*) g_ptr_array_index(self->calls, i);
        if (info->state == state)
            return info;
    }
    return NULL; // no call was found
}

static
guint
qti_ims_call_id_find_by_state(
    QtiImsCall* self,
    BINDER_EXT_CALL_STATE state)
{
    BinderExtCallInfo* info = qti_ims_call_info_find_by_state(self, state);
    if (info)
        return info->call_id;
    return 0;
}

/*==========================================================================*
 * Conference
 *==========================================================================*/

static
void
qti_ims_call_emit_end(
    QtiImsCall* self,
    guint call_id)
{
    g_signal_emit(self, qti_ims_call_signals[SIGNAL_CALL_END], 0, call_id,
        BINDER_EXT_CALL_DISCONNECT_UNKNOWN);
}

static
gboolean
qti_ims_call_conf_merging(
    QtiImsCall* self)
{
    return g_hash_table_size(self->conf_pending) ||
        g_hash_table_size(self->conf_members);
}

static
gboolean
qti_ims_call_conf_is_member(
    QtiImsCall* self,
    guint call_id)
{
    return g_hash_table_contains(self->conf_members, ID_KEY(call_id)) ||
        g_hash_table_contains(self->conf_pending, ID_KEY(call_id));
}

static
gboolean
qti_ims_call_conf_is_gone(
    QtiImsCall* self,
    guint call_id)
{
    return g_hash_table_lookup(self->conf_members, ID_KEY(call_id)) ==
        CONF_MEMBER_GONE ||
        g_hash_table_lookup(self->conf_pending, ID_KEY(call_id)) ==
        CONF_MEMBER_GONE;
}

static
void
qti_ims_call_conf_mark_gone(
    QtiImsCall* self,
    guint call_id)
{
    g_hash_table_insert(g_hash_table_contains(self->conf_members,
        ID_KEY(call_id)) ? self->conf_members : self->conf_pending,
        ID_KEY(call_id), CONF_MEMBER_GONE);
}

/* The modem knows a conference only by its isMpty call */
static
guint
qti_ims_call_modem_id(
    QtiImsCall* self,
    guint call_id)
{
    if (self->conf_call_id && call_id != self->conf_call_id &&
        g_hash_table_contains(self->conf_members, ID_KEY(call_id))) {
        DBG("call %u is in conference call %u", call_id, self->conf_call_id);
        return self->conf_call_id;
    }
    return call_id;
}

/* Ends a call that only oFono still has */
static
void
qti_ims_call_conf_drop(
    QtiImsCall* self,
    guint call_id)
{
    BinderExtCallInfo* call = qti_ims_call_info_find(self, call_id);

    g_hash_table_remove(self->conf_members, ID_KEY(call_id));
    g_hash_table_remove(self->conf_pending, ID_KEY(call_id));
    if (call) {
        qti_ims_call_emit_end(self, call_id);
        g_ptr_array_remove(self->calls, call);
    }
}

static
void
qti_ims_call_conf_drop_gone(
    QtiImsCall* self,
    GHashTable* table)
{
    GHashTableIter it;
    gpointer key, value;
    GSList* gone = NULL;
    GSList* l;

    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        if (value == CONF_MEMBER_GONE) {
            gone = g_slist_prepend(gone, key);
        }
    }
    for (l = gone; l; l = l->next) {
        DBG("ending merged call %u", GPOINTER_TO_UINT(l->data));
        qti_ims_call_conf_drop(self, GPOINTER_TO_UINT(l->data));
    }
    g_slist_free(gone);
}

/* Members follow the state of the conference call */
static
void
qti_ims_call_conf_sync(
    QtiImsCall* self)
{
    BinderExtCallInfo* conf = self->conf_call_id ?
        qti_ims_call_info_find(self, self->conf_call_id) : NULL;

    // The conference call is an MO call to the conference server and may
    // pass DIALING/ALERTING first; members only take ACTIVE and HOLDING
    if (conf && (conf->state == BINDER_EXT_CALL_STATE_ACTIVE ||
        conf->state == BINDER_EXT_CALL_STATE_HOLDING)) {
        GHashTableIter it;
        gpointer key;

        g_hash_table_iter_init(&it, self->conf_members);
        while (g_hash_table_iter_next(&it, &key, NULL)) {
            BinderExtCallInfo* call =
                qti_ims_call_info_find(self, GPOINTER_TO_UINT(key));

            if (call) {
                call->state = conf->state;
            }
        }
    }
}

static
void
qti_ims_call_conf_end(
    QtiImsCall* self)
{
    /* Calls the modem still has get their own END */
    qti_ims_call_conf_drop_gone(self, self->conf_members);
    qti_ims_call_conf_drop_gone(self, self->conf_pending);
    g_hash_table_remove_all(self->conf_members);
    g_hash_table_remove_all(self->conf_pending);
    self->conf_call_id = 0;
}

static
gboolean
qti_ims_call_conf_check(
    gpointer user_data)
{
    QtiImsCall* self = THIS(user_data);

    self->conf_check_id = 0;
    if (!self->conf_call_id && g_hash_table_size(self->conf_members)) {
        DBG("no conference call after the merge, ending the merged calls");
        qti_ims_call_conf_end(self);
        g_signal_emit(self, qti_ims_call_signals[SIGNAL_CALL_STATE_CHANGED], 0);
    }
    return G_SOURCE_REMOVE;
}

static
void
qti_ims_call_conf_commit(
    QtiImsCall* self)
{
    GHashTableIter it;
    gpointer key, value;

    g_hash_table_iter_init(&it, self->conf_pending);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        g_hash_table_insert(self->conf_members, key, value);
    }
    g_hash_table_remove_all(self->conf_pending);

    if (self->conf_call_id) {
        qti_ims_call_conf_sync(self);
    } else if (!self->conf_check_id) {
        /* Normally the isMpty call is already there; don't wait forever */
        self->conf_check_id = g_timeout_add_seconds(CONF_CHECK_TIMEOUT_SEC,
            qti_ims_call_conf_check, self);
    }
}

static
void
qti_ims_call_conf_rollback(
    QtiImsCall* self)
{
    /* Calls that ended while the merge was pending really ended */
    qti_ims_call_conf_drop_gone(self, self->conf_pending);
    g_hash_table_remove_all(self->conf_pending);

    if (self->conf_call_id && !g_hash_table_size(self->conf_members)) {
        BinderExtCallInfo* conf =
            qti_ims_call_info_find(self, self->conf_call_id);

        /* An isMpty call without members: let oFono see it after all */
        DBG("conference failed, showing call %u", self->conf_call_id);
        if (conf) {
            conf->flags &= ~BINDER_EXT_CALL_FLAG_MPTY;
        }
        self->conf_call_id = 0;
    }
}

static
void
qti_ims_call_conf_reset(
    QtiImsCall* self)
{
    self->conf_call_id = 0;
    g_hash_table_remove_all(self->conf_members);
    g_hash_table_remove_all(self->conf_pending);
    if (self->conf_check_id) {
        g_source_remove(self->conf_check_id);
        self->conf_check_id = 0;
    }
}

static
void
qti_ims_call_set_ringback_tone(
    QtiImsCall* self,
    gboolean start)
{
    self->ringback_tone = start;
    g_signal_emit(self, qti_ims_call_signals[SIGNAL_CALL_RINGBACK_TONE], 0,
        start);
}

static
gboolean
qti_ims_call_state_is_setup(
    BINDER_EXT_CALL_STATE state)
{
    return state == BINDER_EXT_CALL_STATE_DIALING ||
        state == BINDER_EXT_CALL_STATE_ALERTING;
}

static
void
qti_ims_call_handle_ringback_tone(
    QtiRadioExt* radio,
    gboolean start,
    void* user_data)
{
    qti_ims_call_set_ringback_tone(THIS(user_data), start);
}

static
void
qti_ims_call_handle_call_info(
    QtiRadioExt* radio,
    GPtrArray* updated_calls,
    void* user_data)
{
    QtiImsCall* self = THIS(user_data);

    // The modem sends onRingbackTone(STOP) before ACTIVE or END. If it ever
    // does not, stop the local tone when the outgoing call leaves
    // DIALING/ALERTING, otherwise telepathy-ring keeps "alert-tone-needed"
    // set for the next call. This runs before the END handling below, so
    // the stop reaches telepathy-ring while it still knows the call.
    if (self->ringback_tone) {
        for (int i = 0; i < updated_calls->len; i++) {
            QtiRadioCallInfo* info = g_ptr_array_index(updated_calls, i);
            BinderExtCallInfo* call = qti_ims_call_info_find(self, info->index);

            if (call && qti_ims_call_state_is_setup(call->state) &&
                !qti_ims_call_state_is_setup(
                    qti_radio_ims_call_radio_state_to_state(info->state))) {
                DBG("call %d left call setup without ringback STOP, "
                    "stopping the ringback tone", info->index);
                qti_ims_call_set_ringback_tone(self, FALSE);
                break;
            }
        }
    }

    // loop over the updated calls
    for (int i = 0; i < updated_calls->len; i++) {
        QtiRadioCallInfo*  info = g_ptr_array_index(updated_calls, i);
        gint32 call_id = info->index;
        BINDER_EXT_CALL_STATE state =
            qti_radio_ims_call_radio_state_to_state(info->state);

        BinderExtCallInfo* call = qti_ims_call_info_find(self, call_id);
        gboolean is_new = FALSE;

        if (call && state != BINDER_EXT_CALL_STATE_END &&
            qti_ims_call_conf_is_gone(self, call_id)) {
            // The modem reuses the id of a merged call. oFono can't have
            // two calls with the same id, so the member has to go.
            DBG("call id %d reused, dropping the merged call", call_id);
            qti_ims_call_conf_drop(self, call_id);
            call = NULL;
        }

        if (state == BINDER_EXT_CALL_STATE_END) {
            if (self->conf_call_id && call_id == self->conf_call_id) {
                DBG("conference call %d ended", call_id);
                qti_ims_call_conf_end(self);
                if (call && (call->flags & BINDER_EXT_CALL_FLAG_MPTY)) {
                    // oFono never saw this one
                    g_ptr_array_remove(self->calls, call);
                    continue;
                }
            } else if (qti_ims_call_conf_is_member(self, call_id)) {
                DBG("call %d merged into the conference", call_id);
                qti_ims_call_conf_mark_gone(self, call_id);
                continue;
            }

          // The signal takes (int call_id, int reason); it used to get "" as
          // the reason. ofono-binder-plugin reports anything but ERROR for a
          // call it did not hang up itself as a remote hangup.
          g_signal_emit(THIS(user_data), qti_ims_call_signals[SIGNAL_CALL_END],
                        0, call_id, BINDER_EXT_CALL_DISCONNECT_UNKNOWN);

          if (call)
            g_ptr_array_remove(self->calls, call);
        } else if (call) {
            call->state = state;
        } else {
            // add a new call
            call = qti_ims_call_info_new(info);
            g_ptr_array_add(self->calls, call);
            is_new = TRUE;
            if (!info->isMT && !info->isMpty &&
                g_hash_table_size(self->conf_pending)) {
                // Qualcomm's own client expects isMpty in the first report
                // of the conference call; log it if the modem differs
                DBG("new call %d during a merge without isMpty", call_id);
            }
        }

        if (info->isMpty && state != BINDER_EXT_CALL_STATE_END &&
            !self->conf_call_id && qti_ims_call_conf_merging(self)) {
            // Qualcomm's ImsServiceClassTracker.handleCalls: a new MO
            // isMpty call during a merge is the conference
            DBG("conference call %d%s", call_id, is_new ? " (new)" : "");
            self->conf_call_id = call_id;
            if (is_new) {
                call->flags |= BINDER_EXT_CALL_FLAG_MPTY;
            }
        }
    }

    qti_ims_call_conf_sync(self);
    g_signal_emit(THIS(user_data),
                    qti_ims_call_signals[SIGNAL_CALL_STATE_CHANGED], 0);
}

static
void
qti_ims_call_handle_voice_disabled(
    QtiRadioExt* radio,
    void* user_data)
{
    DBG("Remove the list of calls as voice is disabled");

    QtiImsCall* self = THIS(user_data);
    if (self->ringback_tone) {
        /* The calls are dropped without END, so the safety net in
         * qti_ims_call_handle_call_info would never see them leave setup */
        qti_ims_call_set_ringback_tone(self, FALSE);
    }
    qti_ims_call_conf_reset(self);
    if (self->calls) {
        g_ptr_array_remove_range(self->calls, 0, self->calls->len);
    }
}

static
void
qti_ims_call_handle_ring(
    QtiRadioExt* radio,
    void* user_data)
{
    g_signal_emit(THIS(user_data),
        qti_ims_call_signals[SIGNAL_CALL_RING], 0);
}

static
const BinderExtCallInfo* const*
qti_ims_call_get_calls(
    BinderExtCall* ext)
{
    static const BinderExtCallInfo* none = NULL;
    QtiImsCall* self = THIS(ext);

    return self->calls->len ? (const BinderExtCallInfo**)self->calls->pdata : &none;
}

static
const char*
qti_ims_call_result_response_name(QTI_RADIO_ERROR_CODE code)
{
    switch (code) {
    case QTI_RADIO_ERROR_INVALID: return "INVALID";
    case QTI_RADIO_ERROR_SUCCESS: return "SUCCESS";
    case QTI_RADIO_ERROR_RADIO_NOT_AVAILABLE: return "RADIO_NOT_AVAILABLE";
    case QTI_RADIO_ERROR_GENERIC_FAILURE: return "GENERIC_FAILURE";
    case QTI_RADIO_ERROR_PASSWORD_INCORRECT: return "PASSWORD_INCORRECT";
    case QTI_RADIO_ERROR_REQUEST_NOT_SUPPORTED: return "REQUEST_NOT_SUPPORTED";
    case QTI_RADIO_ERROR_CANCELLED: return "CANCELLED";
    case QTI_RADIO_ERROR_NO_MEMORY: return "NO_MEMORY";
    case QTI_RADIO_ERROR_UNUSED: return "UNUSED";
    case QTI_RADIO_ERROR_INVALID_PARAMETER: return "INVALID_PARAMETER";
    case QTI_RADIO_ERROR_REJECTED_BY_REMOTE: return "REJECTED_BY_REMOTE";
    case QTI_RADIO_ERROR_IMS_DEREGISTERED: return "IMS_DEREGISTERED";
    case QTI_RADIO_ERROR_NETWORK_NOT_SUPPORTED: return "NETWORK_NOT_SUPPORTED";
    case QTI_RADIO_ERROR_HOLD_RESUME_FAILED: return "HOLD_RESUME_FAILED";
    case QTI_RADIO_ERROR_HOLD_RESUME_CANCELED: return "HOLD_RESUME_CANCELED";
    case QTI_RADIO_ERROR_REINVITE_COLLISION: return "REINVITE_COLLISION";
    case QTI_RADIO_ERROR_FDN_CHECK_FAILURE: return "FDN_CHECK_FAILURE";
    case QTI_RADIO_ERROR_SS_MODIFIED_TO_DIAL: return "SS_MODIFIED_TO_DIAL";
    case QTI_RADIO_ERROR_SS_MODIFIED_TO_USSD: return "SS_MODIFIED_TO_USSD";
    case QTI_RADIO_ERROR_SS_MODIFIED_TO_SS: return "SS_MODIFIED_TO_SS";
    case QTI_RADIO_ERROR_SS_MODIFIED_TO_DIAL_VIDEO: return "SS_MODIFIED_TO_DIAL_VIDEO";
    case QTI_RADIO_ERROR_DIAL_MODIFIED_TO_USSD: return "DIAL_MODIFIED_TO_USSD";
    case QTI_RADIO_ERROR_DIAL_MODIFIED_TO_SS: return "DIAL_MODIFIED_TO_SS";
    case QTI_RADIO_ERROR_DIAL_MODIFIED_TO_DIAL: return "DIAL_MODIFIED_TO_DIAL";
    case QTI_RADIO_ERROR_DIAL_MODIFIED_TO_DIAL_VIDEO: return "DIAL_MODIFIED_TO_DIAL_VIDEO";
    case QTI_RADIO_ERROR_DIAL_VIDEO_MODIFIED_TO_USSD: return "DIAL_VIDEO_MODIFIED_TO_USSD";
    case QTI_RADIO_ERROR_DIAL_VIDEO_MODIFIED_TO_SS: return "DIAL_VIDEO_MODIFIED_TO_SS";
    case QTI_RADIO_ERROR_DIAL_VIDEO_MODIFIED_TO_DIAL: return "DIAL_VIDEO_MODIFIED_TO_DIAL";
    case QTI_RADIO_ERROR_DIAL_VIDEO_MODIFIED_TO_DIAL_VIDEO: return "DIAL_VIDEO_MODIFIED_TO_DIAL_VIDEO";
    case QTI_RADIO_ERROR_USSD_CS_FALLBACK: return "USSD_CS_FALLBACK";
    case QTI_RADIO_ERROR_CF_SERVICE_NOT_REGISTERED: return "CF_SERVICE_NOT_REGISTERED";
    default: return "?";
    }
}

static
void
qti_ims_call_result_response(
    QtiRadioExt* radio,
    GBinderReader* reader,
    void* user_data)
{
    QtiImsCallResultRequest* req = user_data;
    BinderExtCallResultFunc complete = req->complete;
    gint32 result;

    if (!gbinder_reader_read_int32(reader, &result)) {
        ofono_warn("qti_ims_call_result_response: Failed to parse response");
        result = -1;
    }

    DBG("qti_ims_call_result_response: %s(%d)",
        qti_ims_call_result_response_name(result), result);

    if (complete) {
        complete(req->ext,
            (result == QTI_RADIO_ERROR_SUCCESS) ?
                BINDER_EXT_CALL_RESULT_OK :
                BINDER_EXT_CALL_RESULT_ERROR,
            req->user_data);
    }
}

static
guint
qti_ims_call_dial(
    BinderExtCall* ext,
    const char* number,
    BINDER_EXT_TOA toa,
    BINDER_EXT_CALL_CLIR clir,
    BINDER_EXT_CALL_DIAL_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCall* self = THIS(ext);

    QtiImsCallResultRequest* req = qti_ims_call_result_request_new(ext,
        complete, NULL, user_data);
    guint id = qti_radio_ext_dial(self->radio_ext, number, toa, clir, flags,
        qti_ims_call_result_response, qti_ims_call_result_request_destroy, req);

    // On failure qti_radio_ext has already freed req through
    // qti_ims_call_result_request_destroy. The caller's destroy belongs to
    // us only for a non-zero id (ofono-binder-plugin refs its data only then).
    if (id) {
        req->id = id;
        req->destroy = destroy;
        g_hash_table_insert(self->id_map, ID_KEY(id), ID_VALUE(id));
    }

    DBG("Dialing return %d", id);

    return id;
}

static
guint
qti_ims_call_answer(
    BinderExtCall* ext,
    BINDER_EXT_CALL_ANSWER_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCall* self = THIS(ext);
    QTI_RADIO_RTT_MODE mode = flags & BINDER_EXT_CALL_ANSWER_FLAG_RTT ?
        QTI_RADIO_RTT_MODE_FULL : QTI_RADIO_RTT_MODE_DISABLED;
    QTI_RADIO_IP_PRESENTATION presentation = QTI_RADIO_IP_PRESENTATION_NUM_DEFAULT;
    QTI_RADIO_CALL_TYPE call_type = QTI_RADIO_CALL_TYPE_VOICE;

    QtiImsCallResultRequest* req = qti_ims_call_result_request_new(ext,
        complete, NULL, user_data);

    guint id = qti_radio_ext_answer(self->radio_ext, call_type, presentation, mode,
        qti_ims_call_result_response, qti_ims_call_result_request_destroy, req);

    // On failure qti_radio_ext has already freed req through
    // qti_ims_call_result_request_destroy. The caller's destroy belongs to
    // us only for a non-zero id (ofono-binder-plugin refs its data only then).
    if (id) {
        req->id = id;
        req->destroy = destroy;
        g_hash_table_insert(self->id_map, ID_KEY(id), ID_VALUE(id));
    }

    return id;
}

static
guint
qti_ims_call_hold(
    BinderExtCall* ext,
    guint call_id,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCall* self = THIS(ext);

    call_id = qti_ims_call_modem_id(self, call_id);
    DBG("Put call on hold: %u", call_id);

    QtiImsCallResultRequest* req = qti_ims_call_result_request_new(ext,
        complete, NULL, user_data);

    guint id = qti_radio_ext_hold(self->radio_ext, call_id,
        qti_ims_call_result_response, qti_ims_call_result_request_destroy, req);

    // On failure qti_radio_ext has already freed req through
    // qti_ims_call_result_request_destroy. The caller's destroy belongs to
    // us only for a non-zero id (ofono-binder-plugin refs its data only then).
    if (id) {
        req->id = id;
        req->destroy = destroy;
        g_hash_table_insert(self->id_map, ID_KEY(id), ID_VALUE(id));
    }

    return id;
}

static
guint
qti_ims_call_resume(
    BinderExtCall* ext,
    guint call_id,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCall* self = THIS(ext);

    call_id = qti_ims_call_modem_id(self, call_id);
    DBG("Resume call: %u", call_id);

    QtiImsCallResultRequest* req = qti_ims_call_result_request_new(ext,
        complete, NULL, user_data);

    guint id = qti_radio_ext_resume(self->radio_ext, call_id,
        qti_ims_call_result_response, qti_ims_call_result_request_destroy, req);

    // On failure qti_radio_ext has already freed req through
    // qti_ims_call_result_request_destroy. The caller's destroy belongs to
    // us only for a non-zero id (ofono-binder-plugin refs its data only then).
    if (id) {
        req->id = id;
        req->destroy = destroy;
        g_hash_table_insert(self->id_map, ID_KEY(id), ID_VALUE(id));
    }

    return id;
}

static
guint
qti_ims_call_hangup(
    BinderExtCall* ext,
    guint call_id,
    BINDER_EXT_CALL_HANGUP_REASON reason,
    BINDER_EXT_CALL_HANGUP_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCall* self = THIS(ext);

    // ofono-binder-plugin up to 1.1.25 passes reason and flags swapped
    // (fixed upstream by 3e79f62a, "binder_voicecall: fix
    // binder_ext_call_hangup argument order"). HANGUP_FLAGS has only
    // NO_FLAGS (0), so a non-zero flags value is the reason; this does
    // nothing once a fixed ofono-binder-plugin is installed.
    if (reason == BINDER_EXT_CALL_HANGUP_TERMINATE &&
        ((int)flags == BINDER_EXT_CALL_HANGUP_IGNORE ||
         (int)flags == BINDER_EXT_CALL_HANGUP_REJECT)) {
        reason = (BINDER_EXT_CALL_HANGUP_REASON)flags;
        flags = BINDER_EXT_CALL_HANGUP_NO_FLAGS;
    }

    // Hanging up a member ends the whole conference (no per-participant
    // hangup without the participant URIs from onRefreshConferenceInfo)
    call_id = qti_ims_call_modem_id(self, call_id);

    QtiImsCallResultRequest* req = qti_ims_call_result_request_new(ext,
        complete, NULL, user_data);

    guint id = qti_radio_ext_hangup(self->radio_ext, call_id, reason, flags,
        qti_ims_call_result_response, qti_ims_call_result_request_destroy, req);

    // On failure qti_radio_ext has already freed req through
    // qti_ims_call_result_request_destroy. The caller's destroy belongs to
    // us only for a non-zero id (ofono-binder-plugin refs its data only then).
    if (id) {
        req->id = id;
        req->destroy = destroy;
        g_hash_table_insert(self->id_map, ID_KEY(id), ID_VALUE(id));
    }

    return id;
}

static
guint
qti_ims_call_swap_step_activate(
    BinderExtCall* ext,
    BINDER_EXT_CALL_ANSWER_FLAGS answer_flags,
    guint call_hold,
    guint call_incoming,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (call_incoming) {
        DBG("Proceeding by answering incoming call. Hopefully, call=%u is answered", call_incoming);
        return qti_ims_call_answer(ext, answer_flags, complete, destroy,
                                   user_data);
    }

    if (call_hold) {
        DBG("Proceeding with resuming a call=%u", call_hold);
        return qti_ims_call_resume(ext, call_hold, complete, destroy,
                                   user_data);
    }

    DBG("Nothing to do - unexpected call of qti_ims_call_swap_step_activate");
    return 0;
}

static
void
qti_ims_call_swap_step1_complete(
    BinderExtCall* ext,
    BINDER_EXT_CALL_RESULT result,
    void* user_data)
{
    QtiImsCallSwapSecondRequest *req = (QtiImsCallSwapSecondRequest *)user_data;

    if (result != BINDER_EXT_CALL_RESULT_OK) {
        DBG("Call swap step 1 operation failed, cancelling further processing");
        if (req->complete)
            req->complete(req->ext, result, req->user_data);
        return;
    }

    req->step1_success = TRUE;

    if (!qti_ims_call_swap_step_activate(req->ext, req->answer_flags,
                                         req->call_hold, req->call_incoming,
                                         req->complete, req->destroy,
                                         req->user_data)) {
        // Step 2 did not go out, so it does not own the caller's data:
        // complete here and let qti_ims_call_swap_step1_destroy release it
        DBG("Call swap step 2 could not be submitted");
        req->step1_success = FALSE;
        if (req->complete)
            req->complete(req->ext, BINDER_EXT_CALL_RESULT_ERROR,
                          req->user_data);
    }
}

static
void
qti_ims_call_swap_step1_destroy(
    gpointer user_data)
{
    QtiImsCallSwapSecondRequest *req = (QtiImsCallSwapSecondRequest *)user_data;
    // Step 2 was not submitted, so the caller's destroy is ours. req->ext
    // holds no reference (the step 1 request has its own), so no unref here.
    if (!req->step1_success && req->destroy)
        req->destroy(req->user_data);
    g_free(user_data);
}

static
guint
qti_ims_call_swap(
    BinderExtCall* ext,
    BINDER_EXT_CALL_SWAP_FLAGS swap_flags,
    BINDER_EXT_CALL_ANSWER_FLAGS answer_flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCall* self = THIS(ext);
    QtiImsCallSwapSecondRequest *rdata = NULL;
    guint id;

    DBG("Call swap: swap_flags=%u answer_flags=%u", swap_flags, answer_flags);

    guint call_active =
        qti_ims_call_id_find_by_state(self, BINDER_EXT_CALL_STATE_ACTIVE);
    guint call_hold =
        qti_ims_call_id_find_by_state(self, BINDER_EXT_CALL_STATE_HOLDING);
    guint call_incoming =
        qti_ims_call_id_find_by_state(self, BINDER_EXT_CALL_STATE_INCOMING);
    guint call_waiting =
        qti_ims_call_id_find_by_state(self, BINDER_EXT_CALL_STATE_WAITING);
    gboolean full_swap = (call_active && (call_hold || call_incoming || call_waiting));

    if (!call_incoming)
        call_incoming = call_waiting;

    DBG("Current calls: active=%u onhold=%u incoming=%u -> full_swap=%u",
        call_active, call_hold, call_incoming, full_swap);

    if (full_swap) {
        // prepare for two step operation
        rdata = g_new0(QtiImsCallSwapSecondRequest, 1);

        rdata->step1_success = FALSE;
        rdata->answer_flags = answer_flags;
        rdata->call_hold = call_hold;
        rdata->call_incoming = call_incoming;
        rdata->ext = ext;
        rdata->complete = complete;
        rdata->destroy = destroy;
        rdata->user_data = user_data;

        // replace callbacks and data
        complete = qti_ims_call_swap_step1_complete;
        destroy = qti_ims_call_swap_step1_destroy;
        user_data = rdata;
    }

    // deal with active call first
    if (swap_flags == BINDER_EXT_CALL_SWAP_FLAG_HANGUP && call_active) {
        id = qti_ims_call_hangup(
            ext, call_active, BINDER_EXT_CALL_HANGUP_TERMINATE,
            BINDER_EXT_CALL_HANGUP_NO_FLAGS, complete, destroy, user_data);
    } else if (call_active) {
        id = qti_ims_call_hold(ext, call_active, complete, destroy, user_data);
    } else {
        // this is called only if there are no active calls
        id = qti_ims_call_swap_step_activate(ext, answer_flags, call_hold,
                                             call_incoming, complete, destroy,
                                             user_data);
    }

    if (!id && rdata) {
        // step 1 did not go out, nobody will call step1_destroy
        g_free(rdata);
    }
    return id;
}

static
void
qti_ims_call_conference_response(
    QtiRadioExt* radio,
    GBinderReader* reader,
    void* user_data)
{
    QtiImsCallResultRequest* req = user_data;
    QtiImsCall* self = THIS(req->ext);
    BinderExtCallResultFunc complete = req->complete;
    gint32 result;

    /* conferenceResponse(token, ErrorCode, SipErrorInfo), like hold */
    if (!gbinder_reader_read_int32(reader, &result)) {
        ofono_warn("qti_ims_call_conference_response: Failed to parse response");
        result = -1;
    }

    DBG("conference response: %s(%d)",
        qti_ims_call_result_response_name(result), result);

    /* Update the calls before oFono builds its multiparty list */
    if (result == QTI_RADIO_ERROR_SUCCESS) {
        qti_ims_call_conf_commit(self);
    } else {
        qti_ims_call_conf_rollback(self);
    }
    g_signal_emit(self, qti_ims_call_signals[SIGNAL_CALL_STATE_CHANGED], 0);

    if (complete) {
        complete(req->ext,
            (result == QTI_RADIO_ERROR_SUCCESS) ?
                BINDER_EXT_CALL_RESULT_OK :
                BINDER_EXT_CALL_RESULT_ERROR,
            req->user_data);
    }
}

static
void
qti_ims_call_conference_request_destroy(
    gpointer user_data)
{
    QtiImsCallResultRequest* req = user_data;
    QtiImsCall* self = THIS(req->ext);

    if (g_hash_table_size(self->conf_pending)) {
        /* Cancelled, or no response */
        DBG("conference request dropped without a response");
        qti_ims_call_conf_rollback(self);
        g_signal_emit(self, qti_ims_call_signals[SIGNAL_CALL_STATE_CHANGED], 0);
    }
    qti_ims_call_result_request_unref(req);
}

static
guint
qti_ims_call_conference(
    BinderExtCall* ext,
    BINDER_EXT_CALL_CONFERENCE_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiImsCall* self = THIS(ext);
    gboolean have_active = FALSE;
    gboolean have_held = FALSE;
    QtiImsCallResultRequest* req;
    guint id;

    // A 0 return makes ofono-binder-plugin send the IRadio conference
    // request instead. oFono core already refuses a second CreateMultiparty
    // while one is pending, so this is not expected.
    if (g_hash_table_size(self->conf_pending)) {
        DBG("conference: a merge is already in progress");
        return 0;
    }

    // Like Qualcomm's ImsConferenceController.sendConferenceRequest: the
    // modem merges its ACTIVE and its HOLDING call. Members of an existing
    // conference stand for the (hidden) conference call.
    for (guint i = 0; i < self->calls->len; i++) {
        BinderExtCallInfo* call = g_ptr_array_index(self->calls, i);

        if (call->flags & BINDER_EXT_CALL_FLAG_MPTY) {
            continue;
        } else if (call->state == BINDER_EXT_CALL_STATE_ACTIVE) {
            have_active = TRUE;
        } else if (call->state == BINDER_EXT_CALL_STATE_HOLDING) {
            have_held = TRUE;
        } else {
            continue;
        }
        if (!g_hash_table_contains(self->conf_members,
            ID_KEY(call->call_id))) {
            g_hash_table_insert(self->conf_pending, ID_KEY(call->call_id),
                CONF_MEMBER_LIVE);
        }
    }

    // oFono core checks for a held and an active call too, so this only
    // happens if one of them is a CS call; then IRadio is the right path
    if (!have_active || !have_held) {
        DBG("conference: needs an active and a held call");
        g_hash_table_remove_all(self->conf_pending);
        return 0;
    }

    DBG("conference: merging %u call(s) into %s", g_hash_table_size(
        self->conf_pending), self->conf_call_id ? "the conference" :
        "a new conference");

    // The caller's destroy belongs to us only once the request is out. On
    // failure qti_radio_ext_result_request_submit has already called
    // qti_ims_call_conference_request_destroy (rolled back, req freed),
    // and a caller that gets 0 must not see its destroy called.
    req = qti_ims_call_result_request_new(ext, complete, NULL, user_data);
    id = qti_radio_ext_conference(self->radio_ext,
        qti_ims_call_conference_response,
        qti_ims_call_conference_request_destroy, req);

    if (id) {
        req->id = id;
        req->destroy = destroy;
        g_hash_table_insert(self->id_map, ID_KEY(id), ID_VALUE(id));
    }

    return id;
}

// static
// guint
// qti_ims_call_send_dtmf(
//     BinderExtCall* ext,
//     const char* tones,
//     BinderExtCallResultFunc complete,
//     GDestroyNotify destroy,
//     void* user_data)
// {
//     // when implemented, enable in qti_ims_call_iface_init
//     DBG("send_dtmf is not implemented yet");
//     return 0;
// }

static
void
qti_ims_call_cancel(
    BinderExtCall* ext,
    guint id)
{
    QtiImsCall* self = THIS(ext);
    const guint mapped = GPOINTER_TO_UINT(g_hash_table_lookup(self->id_map,
        ID_KEY(id)));

    qti_radio_ext_cancel(self->radio_ext, mapped ? mapped : id);
}

static
void
qti_ims_call_handle_handover(
    QtiRadioExt* radio,
    QTI_RADIO_HANDOVER_TYPE type,
    QTI_RADIO_TECH_TYPE srcTech,
    QTI_RADIO_TECH_TYPE targetTech,
    void* user_data)
{
    QtiImsCall* self = THIS(user_data);

    DBG("Handle handover event: type=%d srcTech=%d targetTech=%d", type, srcTech, targetTech);

    if (type == QTI_RADIO_HANDOVER_COMPLETE_SUCCESS) {
        // Based on testing and is a concern for calls on hold
        //
        // If there is no interaction with the calls, such handover will close
        // calls that are in holding state. Sometimes if helps to trigger hold request on these calls. This
        // request will fail with GENERIC_FAILURE(3). However, as a result, with some probability, call 
        // will stay on hold even after handover.
        //
        // Alternative is to hangup all calls on hold in this case. This seem to work every time it
        // was tested and at least leads to a clean state.
        //
        // Without doing anything, calls can either be dropped after some time or, as happened rarely, can
        // linger around without any ability by ofono to close them. To avoid such ambiquity, this implementation
        // triggers hangup on these calls.
        for (int i = 0; i < self->calls->len; i++) {
            BinderExtCallInfo* info = (BinderExtCallInfo*) g_ptr_array_index(self->calls, i);
            if (info->state == BINDER_EXT_CALL_STATE_HOLDING) {
                // DBG("Sending on hold request for call %u to keep it alive after handover", info->call_id);
                // qti_ims_call_hold(BINDER_EXT_CALL(self), info->call_id, NULL,
                //                   NULL, NULL);

                DBG("Hangup request for call %u to cleanup after handover", info->call_id);
                qti_ims_call_hangup(BINDER_EXT_CALL(self), info->call_id,
                                    BINDER_EXT_CALL_HANGUP_TERMINATE,
                                    BINDER_EXT_CALL_HANGUP_NO_FLAGS, NULL,
                                    NULL, NULL);
            }
        }
    }
}

static
gulong
qti_ims_call_add_calls_changed_handler(
    BinderExtCall* ext,
    BinderExtCallFunc cb,
    void* user_data)
{
    return G_LIKELY(cb) ? g_signal_connect(THIS(ext),
        SIGNAL_CALL_STATE_CHANGED_NAME, G_CALLBACK(cb), user_data) : 0;
}

static
gulong
qti_ims_call_add_disconnect_handler(
    BinderExtCall* ext,
    BinderExtCallDisconnectFunc cb,
    void* user_data)
{
    return G_LIKELY(cb) ? g_signal_connect(THIS(ext),
        SIGNAL_CALL_END_NAME, G_CALLBACK(cb), user_data) : 0;
}

static
gulong
qti_ims_call_add_ring_handler(
    BinderExtCall* ext,
    BinderExtCallFunc cb,
    void* user_data)
{
    return G_LIKELY(cb) ? g_signal_connect(THIS(ext),
        SIGNAL_CALL_RING_NAME, G_CALLBACK(cb), user_data) : 0;
}

static
gulong
qti_ims_call_add_ssn_handler(
    BinderExtCall* ext,
    BinderExtCallSuppSvcNotifyFunc cb,
    void* user_data)
{
    return G_LIKELY(cb) ? g_signal_connect(THIS(ext),
        SIGNAL_CALL_SUPP_SVC_NOTIFY_NAME, G_CALLBACK(cb), user_data) : 0;
}

static
gulong
qti_ims_call_add_ringback_tone_handler(
    BinderExtCall* ext,
    BinderExtCallRingbackToneFunc cb,
    void* user_data)
{
    return G_LIKELY(cb) ? g_signal_connect(THIS(ext),
        SIGNAL_CALL_RINGBACK_TONE_NAME, G_CALLBACK(cb), user_data) : 0;
}

void
qti_ims_call_iface_init(
    BinderExtCallInterface* iface)
{
    iface->flags |= BINDER_EXT_CALL_INTERFACE_FLAG_IMS_SUPPORT |
        BINDER_EXT_CALL_INTERFACE_FLAG_IMS_REQUIRED;
    iface->version = BINDER_EXT_CALL_INTERFACE_VERSION;
    iface->get_calls = qti_ims_call_get_calls;
    iface->dial = qti_ims_call_dial;
    iface->answer = qti_ims_call_answer;
    iface->swap = qti_ims_call_swap;
    iface->conference = qti_ims_call_conference;
    iface->send_dtmf = NULL; // not implemented - qti_ims_call_send_dtmf;
    iface->hangup = qti_ims_call_hangup;
    iface->cancel = qti_ims_call_cancel;
    iface->add_calls_changed_handler =
        qti_ims_call_add_calls_changed_handler;
    iface->add_disconnect_handler = qti_ims_call_add_disconnect_handler;
    iface->add_ring_handler = qti_ims_call_add_ring_handler;
    iface->add_ssn_handler = qti_ims_call_add_ssn_handler;
    iface->add_ringback_tone_handler = qti_ims_call_add_ringback_tone_handler;
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtCall*
qti_ims_call_new(
    QtiRadioExt* radio_ext)
{
    if (G_LIKELY(radio_ext)) {
        QtiImsCall* self = g_object_new(THIS_TYPE, NULL);

        self->radio_ext = qti_radio_ext_ref(radio_ext);
        self->calls = g_ptr_array_new_null_terminated(0, g_free, TRUE);

        qti_radio_ext_add_call_state_handler(radio_ext,
            qti_ims_call_handle_call_info, self);
        qti_radio_ext_add_ring_handler(radio_ext,
            qti_ims_call_handle_ring, self);
        qti_radio_ext_add_ringback_tone_handler(radio_ext,
            qti_ims_call_handle_ringback_tone, self);
        qti_radio_ext_add_voice_disabled_handler(radio_ext,
            qti_ims_call_handle_voice_disabled, self);
        qti_radio_ext_add_handover_handler(radio_ext,
            qti_ims_call_handle_handover, self);

        return BINDER_EXT_CALL(self);
    }
    return NULL;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
qti_ims_call_finalize(
    GObject* object)
{
    QtiImsCall* self = THIS(object);

    if (self->conf_check_id) {
        g_source_remove(self->conf_check_id);
    }
    qti_radio_ext_unref(self->radio_ext);
    gutil_idle_pool_destroy(self->pool);
    g_ptr_array_unref(self->calls);
    g_hash_table_unref(self->id_map);
    g_hash_table_unref(self->conf_members);
    g_hash_table_unref(self->conf_pending);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
qti_ims_call_init(
    QtiImsCall* self)
{
    self->pool = gutil_idle_pool_new();
    self->id_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->conf_members = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->conf_pending = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static
void
qti_ims_call_class_init(
    QtiImsCallClass* klass)
{
    GType type = G_OBJECT_CLASS_TYPE(klass);

    G_OBJECT_CLASS(klass)->finalize = qti_ims_call_finalize;
    qti_ims_call_signals[SIGNAL_CALL_STATE_CHANGED] =
        g_signal_new(SIGNAL_CALL_STATE_CHANGED_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    qti_ims_call_signals[SIGNAL_CALL_END] =
        g_signal_new(SIGNAL_CALL_END_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            2, G_TYPE_INT, G_TYPE_INT);
    qti_ims_call_signals[SIGNAL_CALL_RING] =
        g_signal_new(SIGNAL_CALL_RING_NAME, type, G_SIGNAL_RUN_FIRST, 0,
            NULL, NULL, NULL, G_TYPE_NONE, 0);
    qti_ims_call_signals[SIGNAL_CALL_SUPP_SVC_NOTIFY] =
        g_signal_new(SIGNAL_CALL_SUPP_SVC_NOTIFY_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            1, G_TYPE_POINTER);
    qti_ims_call_signals[SIGNAL_CALL_RINGBACK_TONE] =
        g_signal_new(SIGNAL_CALL_RINGBACK_TONE_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            1, G_TYPE_BOOLEAN);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
