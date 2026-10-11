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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <glib-object.h>

#include "qti_radio_ext.h"
#include "qti_radio_ext_types.h"
#include "qti_utils.h"

#include <radio_types.h>
#include <binder_ext_ims_impl.h>
#include <binder_ext_call_impl.h>
#include <binder_ext_sms_impl.h>

#include <ofono/log.h>
#include <ofono/misc.h>

#include <gbinder.h>

#include <gutil_idlepool.h>
#include <gutil_log.h>
#include <gutil_macros.h>

#undef DBG
#define DBG(fmt, ...) \
    gutil_log(GLOG_MODULE_CURRENT, GLOG_LEVEL_ALWAYS, "ims:"fmt, ##__VA_ARGS__)


#define QTI_RADIO_CALL_TIMEOUT (3*1000) /* ms */

typedef GObjectClass QtiRadioExtClass;
typedef struct qti_radio_ext {
    GObject parent;
    char* slot;
    GBinderClient* client;
    GBinderRemoteObject* remote;
    GBinderLocalObject* response;
    GBinderLocalObject* indication;
    GUtilIdlePool* pool;
    GHashTable* requests;
    gboolean voice_enabled;
} QtiRadioExt;

GType qti_radio_ext_get_type() G_GNUC_INTERNAL;
G_DEFINE_TYPE(QtiRadioExt, qti_radio_ext, G_TYPE_OBJECT)

#define THIS_TYPE qti_radio_ext_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, QtiRadioExt)
#define PARENT_CLASS qti_radio_ext_parent_class
#define KEY(serial) GUINT_TO_POINTER(serial)

typedef struct qti_radio_ext_request QtiRadioExtRequest;

typedef void (*QtiRadioExtArgWriteFunc)(
    GBinderWriter* args,
    va_list va);

typedef void (*QtiRadioExtRequestHandlerFunc)(
    QtiRadioExtRequest* req,
    const GBinderReader* args);

struct qti_radio_ext_request {
    guint id;  /* request id */
    gulong tx; /* binder transaction id */
    QtiRadioExt* radio;
    gint32 response_code;
    QtiRadioExtRequestHandlerFunc handle_response;
    void (*free)(QtiRadioExtRequest* req);
    GDestroyNotify destroy;
    void* user_data;
};

typedef struct qti_radio_ext_result_request {
    QtiRadioExtRequest base;
    QtiRadioExtResultFunc complete;
} QtiRadioExtResultRequest;

enum qti_radio_ext_signal {
    SIGNAL_IMS_REG_STATUS_CHANGED,
    SIGNAL_EXT_CALL_STATE_CHANGED,
    SIGNAL_EXT_ON_RING,
    SIGNAL_EXT_ON_INCOMING_SMS,
    SIGNAL_EXT_ON_INCOMING_SMS_REPORT,
    SIGNAL_EXT_ON_VOICE_DISABLED,
    SIGNAL_EXT_HANDOVER,
    SIGNAL_EXT_ON_RINGBACK_TONE,
    SIGNAL_EXT_ON_CONF_INFO,
    SIGNAL_COUNT
};

#define SIGNAL_IMS_REG_STATUS_CHANGED_NAME          "qti-radio-ext-ims-reg-status-changed"
#define SIGNAL_EXT_CALL_STATE_CHANGED_NAME          "qti-radio-ext-call-state-changed"
#define SIGNAL_EXT_ON_RING_NAME                     "qti-radio-ext-on-ring"
#define SIGNAL_EXT_ON_INCOMING_SMS_NAME             "qti-radio-ext-on-incoming-sms"
#define SIGNAL_EXT_ON_INCOMING_SMS_REPORT_NAME      "qti-radio-ext-on-incoming-sms-report"
#define SIGNAL_EXT_ON_VOICE_DISABLED_NAME           "qti-radio-ext-on-voice-disabled"
#define SIGNAL_EXT_HANDOVER_NAME                    "qti-radio-ext-handover"
#define SIGNAL_EXT_ON_RINGBACK_TONE_NAME            "qti-radio-ext-on-ringback-tone"
#define SIGNAL_EXT_ON_CONF_INFO_NAME                "qti-radio-ext-on-conf-info"

static guint qti_radio_ext_signals[SIGNAL_COUNT] = { 0 };

static GLogModule qti_radio_ext_binder_log_module = {
    .max_level = GLOG_LEVEL_VERBOSE,
    .level = GLOG_LEVEL_VERBOSE,
    .flags = GLOG_FLAG_HIDE_NAME
};

static GLogModule qti_radio_ext_binder_dump_module = {
    .parent = &qti_radio_ext_binder_log_module,
    .max_level = GLOG_LEVEL_VERBOSE,
    .level = GLOG_LEVEL_INHERIT,
    .flags = GLOG_FLAG_HIDE_NAME
};

static
void
qti_radio_ext_log_notify(
    struct ofono_debug_desc* desc)
{
    qti_radio_ext_binder_log_module.level = (desc->flags &
        OFONO_DEBUG_FLAG_PRINT) ? GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static
void
qti_radio_ext_dump_notify(
    struct ofono_debug_desc* desc)
{
    qti_radio_ext_binder_dump_module.level = (desc->flags &
        OFONO_DEBUG_FLAG_PRINT) ? GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static struct ofono_debug_desc logger_trace OFONO_DEBUG_ATTR = {
    .name = "qti_binder_trace",
    .flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
    .notify = qti_radio_ext_log_notify
};

static struct ofono_debug_desc logger_dump OFONO_DEBUG_ATTR = {
    .name = "qti_binder_dump",
    .flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
    .notify = qti_radio_ext_dump_notify
};

static
guint
qti_radio_ext_new_req_id()
{
    // Start from 1
    static guint last_id = 1;
    return last_id++;
}

static const char*
qti_radio_ext_req_name(
    guint32 req)
{
    switch (req) {
#define QTI_RADIO_REQ_(req, resp, name, NAME) \
        case QTI_RADIO_REQ_##NAME: return #name;
    QTI_RADIO_EXT_IMS_CALL_AIDL(QTI_RADIO_REQ_)
#undef QTI_RADIO_REQ_
    }
    return NULL;
}

static const char*
qti_radio_ext_resp_name(
    guint32 respcode)
{
// handles duplicate definitions for response
#define QTI_RADIO_RESP_(req, resp, name, NAME) \
        if (respcode == QTI_RADIO_RESP_##NAME) return #name;
    QTI_RADIO_EXT_IMS_CALL_AIDL(QTI_RADIO_RESP_)
#undef QTI_RADIO_RESP_

   return NULL;
}

static const char*
qti_radio_ext_ind_name(
    guint32 ind)
{
    switch (ind) {
#define QTI_RADIO_IND_(code, name, NAME) \
        case QTI_RADIO_IND_##NAME: return #name;
    QTI_RADIO_IND_AIDL(QTI_RADIO_IND_)
#undef QTI_RADIO_IND_
    }
    return "???";
}

static
void
qti_radio_ext_log_req(
    QtiRadioExt* self,
    guint32 code,
    guint32 serial)
{
    static const GLogModule* log = &qti_radio_ext_binder_log_module;
    const int level = GLOG_LEVEL_VERBOSE;
    const char* name;

    if (!gutil_log_enabled(log, level))
        return;

    name = qti_radio_ext_req_name(code);

    if (serial) {
        gutil_log(log, level, "REQ: %s < [%08x] %u %s",
            self->slot, serial, code, name ? name : "???");
    } else {
        gutil_log(log, level, "REQ: %s < %u %s",
            self->slot, code, name ? name : "???");
    }
}

void
qti_radio_ext_log_resp(
    QtiRadioExt* self,
    guint32 code,
    guint32 serial)
{
    static const GLogModule* log = &qti_radio_ext_binder_log_module;
    const int level = GLOG_LEVEL_VERBOSE;
    const char* name;

    if (!gutil_log_enabled(log, level))
        return;

    name = qti_radio_ext_resp_name(code);

    gutil_log(log, level, "RESP: %s > [%08x] %u %s",
        self->slot, serial, code, name ? name : "???");
}

static
void
qti_radio_ext_log_ind(
    QtiRadioExt* self,
    guint32 code)
{
    static const GLogModule* log = &qti_radio_ext_binder_log_module;
    const int level = GLOG_LEVEL_VERBOSE;
    const char* name;

    if (!gutil_log_enabled(log, level))
        return;

    name = qti_radio_ext_ind_name(code);

    gutil_log(log, level, "IND: %s > %u %s", self->slot, code,
        name ? name : "???");
}

static
void
qti_radio_ext_dump_data(
    const GBinderReader* reader)
{
    // static const GLogModule* log = &qti_radio_ext_binder_dump_module;
    // const int level = GLOG_LEVEL_VERBOSE;
    // gsize size;
    // const guint8* data;

    // if (!gutil_log_enabled(log, level))
    //     return;

    // data = gbinder_reader_get_data(reader, &size);
    // gutil_log_dump(log, level, "  ",  data, size);
}

static
void
qti_radio_ext_dump_request(
    GBinderLocalRequest* args)
{
    // static const GLogModule* log = &qti_radio_ext_binder_dump_module;
    // const int level = GLOG_LEVEL_VERBOSE;
    // GBinderWriter writer;
    // const guint8* data;
    // gsize size;

    // if (!gutil_log_enabled(log, level))
    //     return;

    // /* Use writer API to fetch the raw data */
    // gbinder_local_request_init_writer(args, &writer);
    // data = gbinder_writer_get_data(&writer, &size);
    // gutil_log_dump(log, level, "  ", data, size);
}

QTI_RADIO_REG_STATE
qti_radio_ext_read_ims_reg_status_info(
    QtiRadioExt* self,
    GBinderReader* reader)
{
    gint32 hasdata;
    gint32 datasz;
    gint32 state = QTI_RADIO_REG_STATE_FAILED_TO_READ;
    gint32 error_code;
    char *error_message = NULL;
    gint32 radio_tech;
    char *uri = NULL;

    gboolean success = (
        gbinder_reader_read_int32(reader, &hasdata) &&
        hasdata &&
        gbinder_reader_read_int32(reader, &datasz) &&
        gbinder_reader_read_int32(reader, &state) &&
        gbinder_reader_read_int32(reader, &error_code)
    );
    if (success) {
        error_message = gbinder_reader_read_string16(reader);
        success = success && gbinder_reader_read_int32(reader, &radio_tech);
        if (success)
            uri = gbinder_reader_read_string16(reader);
    }

    if (success) {
        DBG("%s: QtiRadioRegInfo state:%d radiotech:%d"
            " error_code:%d "
            " uri:%s error_msg:%s",
            self->slot, state, radio_tech, error_code, uri ? uri : "",
            error_message ? error_message : "");
    }

    g_free(error_message);
    g_free(uri);

    if (success) {
      return state;
    } else {
        DBG("%s: failed to parse QtiRadioRegInfo", self->slot);
        return QTI_RADIO_REG_STATE_FAILED_TO_READ;
    }
}

static
void
qti_radio_ext_handle_ims_reg_status_report(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;

    gbinder_reader_copy(&reader, args);
    QTI_RADIO_REG_STATE state = qti_radio_ext_read_ims_reg_status_info(self, &reader);

    if (state != QTI_RADIO_REG_STATE_FAILED_TO_READ)
        g_signal_emit(self, qti_radio_ext_signals[SIGNAL_IMS_REG_STATUS_CHANGED],
                    0, state);
}

BINDER_EXT_CALL_STATE
qti_radio_ims_call_radio_state_to_state(
    QTI_RADIO_CALL_STATE state)
{
    switch (state) {
    case QTI_RADIO_CALL_STATE_INCOMING:
        return BINDER_EXT_CALL_STATE_INCOMING;
    case QTI_RADIO_CALL_STATE_ALERTING:
        return BINDER_EXT_CALL_STATE_ALERTING;
    case QTI_RADIO_CALL_STATE_HOLDING:
        return BINDER_EXT_CALL_STATE_HOLDING;
    case QTI_RADIO_CALL_STATE_WAITING:
        return BINDER_EXT_CALL_STATE_WAITING;
    case QTI_RADIO_CALL_STATE_ACTIVE:
        return BINDER_EXT_CALL_STATE_ACTIVE;
    case QTI_RADIO_CALL_STATE_END:
        return BINDER_EXT_CALL_STATE_END;
    case QTI_RADIO_CALL_STATE_DIALING:
        return BINDER_EXT_CALL_STATE_DIALING;
    default:
        return BINDER_EXT_CALL_STATE_INVALID;
    }
}

static
const char *
qti_ims_call_radio_state_name(QTI_RADIO_CALL_STATE state)
{
    switch (state) {
    case QTI_RADIO_CALL_STATE_INCOMING:
        return "INCOMING";
    case QTI_RADIO_CALL_STATE_ALERTING:
        return "ALERTING";
    case QTI_RADIO_CALL_STATE_HOLDING:
        return "HOLDING";
    case QTI_RADIO_CALL_STATE_WAITING:
        return "WAITING";
    case QTI_RADIO_CALL_STATE_ACTIVE:
        return "ACTIVE";
    case QTI_RADIO_CALL_STATE_END:
        return "END";
    case QTI_RADIO_CALL_STATE_DIALING:
        return "DIALING";
    default:
        return "?";
    }
}

static
const char*
qti_radio_call_type_name(QTI_RADIO_CALL_TYPE type)
{
    switch (type) {
    case QTI_RADIO_CALL_TYPE_UNKNOWN:
        return "UNKNOWN";
    case QTI_RADIO_CALL_TYPE_VOICE:
        return "VOICE";
    case QTI_RADIO_CALL_TYPE_VT_TX:
        return "VT_TX";
    case QTI_RADIO_CALL_TYPE_VT_RX:
        return "VT_RX";
    case QTI_RADIO_CALL_TYPE_VT:
        return "VT";
    case QTI_RADIO_CALL_TYPE_VT_NODIR:
        return "VT_NODIR";
    case QTI_RADIO_CALL_TYPE_CS_VS_TX:
        return "CS_VS_TX";
    case QTI_RADIO_CALL_TYPE_CS_VS_RX:
        return "CS_VS_RX";
    case QTI_RADIO_CALL_TYPE_PS_VS_TX:
        return "PS_VS_TX";
    case QTI_RADIO_CALL_TYPE_PS_VS_RX:
        return "PS_VS_RX";
    case QTI_RADIO_CALL_TYPE_SMS:
        return "SMS";
    case QTI_RADIO_CALL_TYPE_UT:
        return "UT";
    case QTI_RADIO_CALL_TYPE_USSD:
        return "USSD";
    case QTI_RADIO_CALL_TYPE_CALLCOMPOSER:
        return "CALLCOMPOSER";
    default:
        return "?";
    }
}


static
gboolean
qti_radio_ext_read_call_info(GBinderReader* reader, QtiRadioCallInfo* info)
{
    gsize parcel_size;
    gsize initial_size;
    char* temp_number = NULL;
    char* temp_name = NULL;
    char* temp_history_info = NULL;
    char* temp_diversion_info = NULL;
    gboolean success = TRUE;

    // parcelable header
    parcel_size = qti_binder_read_parcelable_size(reader);
    if (parcel_size == 0)
      return FALSE;

    // initial location
    initial_size = gbinder_reader_bytes_read(reader);

    // actual fields
    success = gbinder_reader_read_int32(reader, &info->state) &&
              gbinder_reader_read_int32(reader, &info->index) &&
              gbinder_reader_read_int32(reader, &info->toa) &&
              gbinder_reader_read_bool(reader, &info->isMpty) &&
              gbinder_reader_read_bool(reader, &info->isMT);

    if (success) gbinder_reader_read_parcelable(reader, NULL); // mtMultiLineInfo

    success = success && gbinder_reader_read_int32(reader, &info->als) &&
              gbinder_reader_read_bool(reader, &info->isVoice) &&
              gbinder_reader_read_bool(reader, &info->isVoicePrivacy);

    if (success)
      temp_number = gbinder_reader_read_string16(reader);

    success =
        success && gbinder_reader_read_int32(reader, &info->numberPresentation);

    if (success)
      temp_name = gbinder_reader_read_string16(reader);

    success =
        success && gbinder_reader_read_int32(reader, &info->namePresentation);

    if (success) {
      gbinder_reader_read_parcelable(reader, NULL); // callDetails
      gbinder_reader_read_parcelable(reader, NULL); // failCause
    }

    success = success && gbinder_reader_read_bool(reader, &info->isEncrypted) &&
              gbinder_reader_read_bool(reader, &info->isCalledPartyRinging);

    if (success)
      temp_history_info = gbinder_reader_read_string16(reader);

    success = success &&
              gbinder_reader_read_bool(reader, &info->isVideoConfSupported);

    if (success)
      gbinder_reader_read_parcelable(reader, NULL); // verstatInfo

    success = success && gbinder_reader_read_int32(reader, &info->tirMode) &&
              gbinder_reader_read_bool(reader, &info->isPreparatory);

    if (success) {
        gbinder_reader_read_parcelable(reader, NULL); // crsData
        gbinder_reader_read_parcelable(reader, NULL); // callProgInfo

        temp_diversion_info = gbinder_reader_read_string16(reader);

        qti_binder_skip_parcelable_end(reader, parcel_size, initial_size);

        // gbinder_reader_read_parcelable(reader, NULL); // additionalCallInfo
        // gbinder_reader_read_parcelable(reader, NULL); // audioQuality

        // this one seems to be absent in reality
        // if (!gbinder_reader_read_int32(reader, &info->modemCallId)) goto fail;
    }

    // Copy strings to fixed-length arrays with null termination
    if (temp_number)
      g_strlcpy(info->number, temp_number, sizeof(info->number));

    if (temp_name)
      g_strlcpy(info->name, temp_name, sizeof(info->name));

    if (temp_history_info)
      g_strlcpy(info->historyInfo, temp_history_info,
                sizeof(info->historyInfo));

    if (temp_diversion_info)
      g_strlcpy(info->diversionInfo, temp_diversion_info,
                sizeof(info->diversionInfo));

    // Cleanup
    g_free(temp_number);
    g_free(temp_name);
    g_free(temp_history_info);
    g_free(temp_diversion_info);

    return success;
}


static
void
qti_radio_ext_handle_call_state_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    guint32 count;
    gboolean success;

    // array of BinderExtCallInfo
    GPtrArray* call_info_ptr = g_ptr_array_new_with_free_func(g_free);

    gbinder_reader_copy(&reader, args);

    // get size of the vector
    success = gbinder_reader_read_uint32(&reader, &count);
    DBG("CallState indicator: number of calls %u", count);

    for (guint32 i = 0; success && i < count; i++) {
        // read one call info
        QtiRadioCallInfo* info = g_new0(QtiRadioCallInfo, 1);
        success = qti_radio_ext_read_call_info(&reader, info);

        if (!success) {
          DBG("Failed to parse CallInfo %d", success);
          g_free(info);
        } else {
            DBG("state=%s(%d) index=%d toa=%d isMpty=%d\n"
                "isMT=%d als=%d isVoice=%d isVoicePrivacy=%d\n"
                "number=%s numberPresentation=%d name=%s namePresentation=%d\n"
                "isEncrypted=%d isCalledPartyRinging=%d historyInfo=%s isVideoConfSupported=%d\n"
                "tirMode=%d isPreparatory=%d diversionInfo=%s",
                qti_ims_call_radio_state_name(info->state), info->state, info->index, info->toa, info->isMpty,
                info->isMT, info->als, info->isVoice, info->isVoicePrivacy,
                info->number, info->numberPresentation,
                info->name, info->namePresentation,
                info->isEncrypted, info->isCalledPartyRinging,
                info->historyInfo, info->isVideoConfSupported,
                info->tirMode, info->isPreparatory,
                info->diversionInfo
            );

            g_ptr_array_add(call_info_ptr, info);
        }
    }

    if (success)
        g_signal_emit(self, qti_radio_ext_signals[SIGNAL_EXT_CALL_STATE_CHANGED],
                      0, call_info_ptr);
    g_ptr_array_unref(call_info_ptr);
}

// ServiceStatusInfo

typedef struct {
    gboolean isValid;
    gint32 callType;
    gint32 status;
    gint32 restrictCause;
    gint32 countAccTech;
    // StatusForAccessTech[] accTechStatus;  // skip for now
    gint32 rttMode;
} AIDLServiceStatusInfo;

static
const char*
qti_radio_status_type_name(QTI_RADIO_STATUS_TYPE status)
{
    switch (status) {
    case QTI_RADIO_STATUS_INVALID:
        return "INVALID";
    case QTI_RADIO_STATUS_DISABLED:
        return "DISABLED";
    case QTI_RADIO_STATUS_PARTIALLY_ENABLED:
        return "PARTIALLY_ENABLED";
    case QTI_RADIO_STATUS_ENABLED:
        return "ENABLED";
    case QTI_RADIO_STATUS_NOT_SUPPORTED:
        return "NOT_SUPPORTED";
    default:
        return "?";
    }
}

static
gboolean
qti_radio_ext_read_service_status_info(GBinderReader* reader,
                                       AIDLServiceStatusInfo* info)
{
    gsize parcel_size;
    gsize initial_size;

    memset(info, 0, sizeof(*info));

    parcel_size = qti_binder_read_parcelable_size(reader);
    if (parcel_size == 0)
        return FALSE;

    initial_size = gbinder_reader_bytes_read(reader);

    if (!gbinder_reader_read_bool(reader, &info->isValid)) goto fail;
    if (!gbinder_reader_read_int32(reader, &info->callType)) goto fail;
    if (!gbinder_reader_read_int32(reader, &info->status)) goto fail;
    if (!gbinder_reader_read_int32(reader, &info->restrictCause)) goto fail;

    // accTechStatus[] is another typed array, skip for now:
    if (!gbinder_reader_read_int32(reader, &info->countAccTech)) goto fail;
    for (gint32 i=0; i < info->countAccTech; ++i)
        gbinder_reader_read_parcelable(reader, NULL); // accTechStatus

    if (!gbinder_reader_read_int32(reader, &info->rttMode)) goto fail;

    qti_binder_skip_parcelable_end(reader, parcel_size, initial_size);
    return TRUE;

fail:
    memset(info, 0, sizeof(*info));
    return FALSE;
}

static
void
qti_radio_ext_handle_service_status_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    guint32 count;
    gboolean success;

    gbinder_reader_copy(&reader, args);

    success = gbinder_reader_read_uint32(&reader, &count);
    DBG("ServiceStatus indicator: %u entries", count);

    for (guint32 i = 0; success && i < count; i++) {
        AIDLServiceStatusInfo info;
        success = qti_radio_ext_read_service_status_info(&reader, &info);

        if (!success) {
            DBG("Failed to parse ServiceStatusInfo %u", i);
        } else {
            DBG("isValid=%d callType=%s(%d) status=%s(%d) restrictCause=%d rttMode=%d",
                info.isValid,
                qti_radio_call_type_name(info.callType), info.callType,
                qti_radio_status_type_name(info.status), info.status,
                info.restrictCause, info.rttMode);

            if (info.callType == QTI_RADIO_CALL_TYPE_VOICE) {
                gboolean enabled = (info.status == QTI_RADIO_STATUS_ENABLED);
                if (enabled != self->voice_enabled) {
                    self->voice_enabled = enabled;
                    if (!enabled) {
                        g_signal_emit(
                            self,
                            qti_radio_ext_signals[SIGNAL_EXT_ON_VOICE_DISABLED],
                            0);
                    }
                }
            }
        }
    }
}

static
const char*
qti_radio_tech_type_name(QTI_RADIO_TECH_TYPE tech)
{
    switch (tech) {
    case QTI_RADIO_TECH_INVALID:  return "INVALID";
    case QTI_RADIO_TECH_ANY:      return "ANY";
    case QTI_RADIO_TECH_UNKNOWN:  return "UNKNOWN";
    case QTI_RADIO_TECH_GPRS:     return "GPRS";
    case QTI_RADIO_TECH_EDGE:     return "EDGE";
    case QTI_RADIO_TECH_UMTS:     return "UMTS";
    case QTI_RADIO_TECH_IS95A:    return "IS95A";
    case QTI_RADIO_TECH_IS95B:    return "IS95B";
    case QTI_RADIO_TECH_RTT_1X:    return "RTT_1X";
    case QTI_RADIO_TECH_EVDO_0:   return "EVDO_0";
    case QTI_RADIO_TECH_EVDO_A:   return "EVDO_A";
    case QTI_RADIO_TECH_HSDPA:    return "HSDPA";
    case QTI_RADIO_TECH_HSUPA:    return "HSUPA";
    case QTI_RADIO_TECH_HSPA:     return "HSPA";
    case QTI_RADIO_TECH_EVDO_B:   return "EVDO_B";
    case QTI_RADIO_TECH_EHRPD:    return "EHRPD";
    case QTI_RADIO_TECH_LTE:      return "LTE";
    case QTI_RADIO_TECH_HSPAP:    return "HSPAP";
    case QTI_RADIO_TECH_GSM:      return "GSM";
    case QTI_RADIO_TECH_TD_SCDMA: return "TD_SCDMA";
    case QTI_RADIO_TECH_WIFI:     return "WIFI";
    case QTI_RADIO_TECH_IWLAN:    return "IWLAN";
    case QTI_RADIO_TECH_NR5G:     return "NR5G";
    case QTI_RADIO_TECH_C_IWLAN:  return "C_IWLAN";
    default:                      return "?";
    }
}

static
const char*
qti_radio_handover_type_name(QTI_RADIO_HANDOVER_TYPE type)
{
    switch (type) {
    case QTI_RADIO_HANDOVER_INVALID:
        return "INVALID";
    case QTI_RADIO_HANDOVER_START:
        return "START";
    case QTI_RADIO_HANDOVER_COMPLETE_SUCCESS:
        return "COMPLETE_SUCCESS";
    case QTI_RADIO_HANDOVER_COMPLETE_FAIL:
        return "COMPLETE_FAIL";
    case QTI_RADIO_HANDOVER_CANCEL:
        return "CANCEL";
    case QTI_RADIO_HANDOVER_NOT_TRIGGERED:
        return "NOT_TRIGGERED";
    case QTI_RADIO_HANDOVER_NOT_TRIGGERED_MOBILE_DATA_OFF:
        return "NOT_TRIGGERED_MOBILE_DATA_OFF";
    default:
        return "?";
    }
}

static
void
qti_radio_ext_handle_handover_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    gboolean success;

    gbinder_reader_copy(&reader, args);

    gsize parcel_size = qti_binder_read_parcelable_size(&reader);
    if (parcel_size == 0)
        return;

    gint32 type;
    gint32 srcTech;
    gint32 targetTech;
    char *errorCode = NULL;
    char *errorMessage = NULL;

    success = (gbinder_reader_read_int32(&reader, &type) &&
               gbinder_reader_read_int32(&reader, &srcTech) &&
               gbinder_reader_read_int32(&reader, &targetTech));

    if (success) {
        gbinder_reader_read_parcelable(&reader, NULL);
        errorCode = gbinder_reader_read_string16(&reader);
        errorMessage = gbinder_reader_read_string16(&reader);
    } else {
        DBG("Handover: error while parsing data");
    }

    DBG("Handover: type=%s(%d) src=%s(%d) target=%s(%d) errorCode=%s "
        "errorMsg=%s parcelSize=%lu",
        qti_radio_handover_type_name(type), type,
        qti_radio_tech_type_name(srcTech), srcTech,
        qti_radio_tech_type_name(targetTech), targetTech,
        errorCode ? errorCode : "", errorMessage ? errorMessage : "",
        parcel_size);

    g_signal_emit(self, qti_radio_ext_signals[SIGNAL_EXT_HANDOVER],
                  0, type, srcTech, targetTech);

    g_free(errorCode);
    g_free(errorMessage);
}

static
const char*
qti_radio_tone_operation_name(
    gint32 op)
{
    switch (op) {
    case QTI_RADIO_TONE_OPERATION_INVALID: return "INVALID";
    case QTI_RADIO_TONE_OPERATION_STOP:    return "STOP";
    case QTI_RADIO_TONE_OPERATION_START:   return "START";
    default:                               return "?";
    }
}

static
void
qti_radio_ext_handle_ringback_tone_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    /* oneway onRingbackTone(in ToneOperation tone), ToneOperation is int */
    GBinderReader reader;
    gint32 op;

    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, &op)) {
        /* Like ims.apk: START plays the local tone, anything else stops it */
        const gboolean start = (op == QTI_RADIO_TONE_OPERATION_START);

        DBG("%s: RingbackTone: %s (%d) -> %s", self->slot,
            qti_radio_tone_operation_name(op), op, start ? "start" : "stop");
        g_signal_emit(self, qti_radio_ext_signals[SIGNAL_EXT_ON_RINGBACK_TONE],
            0, start);
    } else {
        DBG("%s: failed to parse ringback tone data", self->slot);
    }
}

static
void
qti_radio_ext_handle_conf_info_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    /*
     * oneway onRefreshConferenceInfo(in ConferenceInfo conferenceInfo),
     * parcelable ConferenceInfo { byte[] confInfoUri;
     * ConferenceCallState conferenceCallState; } (ims.apk). confInfoUri
     * is the conference-info document of the network's NOTIFY.
     */
    GBinderReader reader;
    const void* xml = NULL;
    gsize len = 0;
    gint32 state = 0;

    gbinder_reader_copy(&reader, args);
    if (!qti_binder_read_parcelable_size(&reader)) {
        DBG("%s: failed to parse conference info", self->slot);
        return;
    }
    xml = gbinder_reader_read_byte_array(&reader, &len);
    if (!xml) {
        len = 0;
    }
    if (!gbinder_reader_read_int32(&reader, &state)) {
        DBG("%s: failed to parse conference info", self->slot);
        return;
    }

    DBG("%s: conference info: %u bytes, conference call state %d",
        self->slot, (guint)len, state);
    g_signal_emit(self, qti_radio_ext_signals[SIGNAL_EXT_ON_CONF_INFO], 0,
        xml, (guint)len, state);
}

static
void
qti_radio_ext_handle_vops_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;
    gbinder_reader_copy(&reader, args);

    guint32 vopsEnabled;
    gboolean success = gbinder_reader_read_uint32(&reader, &vopsEnabled);
    if (success) {
        DBG("VOPS vopsEnabled=%d [TODO - LINK WITH oFono]", vopsEnabled);
    } else {
        DBG("Failed to parse VOPS indication");
    }
}

static const char*
qti_radio_ext_radio_state_name(
    gint32 resp)
{
    switch (resp) {
    case 0:
      return "INVALID";
    case 1:
      return "OFF";
    case 2:
      return "UNAVAILABLE";
    case 3:
        return "ON";
    default:
        return "UNKNOWN";
    }
}

static const char*
qti_radio_ext_service_domain_name(
    gint32 resp)
{
    switch (resp) {
    case 0:
      return "INVALID";
    case 1:
      return "NO_SRV";
    case 2:
      return "CS_ONLY";
    case 3:
        return "PS_ONLY";
    case 4:
        return "CS_PS";
    case 5:
        return "CAMPED";
    default:
        return "UNKNOWN";
    }
}

static
void
qti_radio_ext_handle_int32_event(
    QtiRadioExt* self,
    const GBinderReader* args,
    const char* param_name,
    const char* (*name_func)(gint32))
{
    GBinderReader reader;
    gbinder_reader_copy(&reader, args);

    gint32 value;
    gboolean success = gbinder_reader_read_int32(&reader, &value);
    if (success) {
        const char* name = name_func ?
            name_func(value) : "UNKNOWN";
        DBG("%s: %s (%d)", param_name, name, value);
    } else {
        DBG("Failed to parse %s", param_name);
    }
}

static
void
qti_radio_ext_handle_incoming_sms_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;

    gbinder_reader_copy(&reader, args);

    gsize parcel_size = qti_binder_read_parcelable_size(&reader);
    if (parcel_size == 0) {
        DBG("%s: failed to parse incoming SMS data", self->slot);
        return;
    }

    char* format = gbinder_reader_read_string16(&reader);
    gsize pdu_len = 0;
    const void* pdu = gbinder_reader_read_byte_array(&reader, &pdu_len);
    gint32 verstat = -1;
    if (!format || !pdu || !gbinder_reader_read_int32(&reader,&verstat)) {
        DBG("%s: failed to parse incoming SMS data", self->slot);
        g_free(format);
        return;
    }

    DBG("%s: Incoming SMS indication format=%s verstat=%d pdu_len=%zu",
          self->slot, format, verstat, pdu_len);

    g_signal_emit(self, qti_radio_ext_signals[SIGNAL_EXT_ON_INCOMING_SMS], 0,
                  pdu, pdu_len);
}

static
void
qti_radio_ext_handle_incoming_sms_report_indication(
    QtiRadioExt* self,
    const GBinderReader* args)
{
    GBinderReader reader;

    gbinder_reader_copy(&reader, args);

    gsize parcel_size = qti_binder_read_parcelable_size(&reader);
    if (parcel_size == 0) {
        DBG("%s: failed to parse incoming SMS report data (prclsz)", self->slot);
        return;
    }

    gint32 msg_ref;
    if (!gbinder_reader_read_int32(&reader,&msg_ref)) {
        DBG("%s: failed to parse incoming SMS report data (msgref)", self->slot);
        return;
    }

    char* format = gbinder_reader_read_string16(&reader);
    gsize pdu_len = 0;
    const void* pdu = gbinder_reader_read_byte_array(&reader, &pdu_len);
    if (!format || !pdu) {
        DBG("%s: failed to parse incoming SMS data", self->slot);
        g_free(format);
        return;
    }

    DBG("%s: Incoming SMS indication msgref=%d, format=%s pdu_len=%zu",
          self->slot, msg_ref, format, pdu_len);

    g_signal_emit(self, qti_radio_ext_signals[SIGNAL_EXT_ON_INCOMING_SMS_REPORT], 0,
                  pdu, pdu_len, msg_ref);
}

static
GBinderLocalReply*
qti_radio_ext_indication(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    QtiRadioExt* self = THIS(user_data);
    const char* iface = gbinder_remote_request_interface(req);
    GBinderReader args;

    gbinder_remote_request_init_reader(req, &args);
    qti_radio_ext_log_ind(self, code);
    qti_radio_ext_dump_data(&args);

    if (g_str_equal(iface, QTI_RADIO_INDICATION_AIDL)) {
        switch(code) {
        case QTI_RADIO_IND_CALL_STATE_INDICATION:
            qti_radio_ext_handle_call_state_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_RING_INDICATION:
            g_signal_emit(self, qti_radio_ext_signals[SIGNAL_EXT_ON_RING], 0);
            return NULL;
        case QTI_RADIO_IND_RINGBACK_TONE_INDICATION:
            qti_radio_ext_handle_ringback_tone_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_REFRESH_CONF_INFO_INDICATION:
            qti_radio_ext_handle_conf_info_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_REG_STATE_INDICATION:
            qti_radio_ext_handle_ims_reg_status_report(self, &args);
            return NULL;
        case QTI_RADIO_IND_SVC_STATUS_INDICATION:
            qti_radio_ext_handle_service_status_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_HANDOVER_INDICATION:
            qti_radio_ext_handle_handover_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_VOPS_INDICATION:
            qti_radio_ext_handle_vops_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_INCOMING_SMS:
            qti_radio_ext_handle_incoming_sms_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_SMS_SEND_STATUS:
            qti_radio_ext_handle_incoming_sms_report_indication(self, &args);
            return NULL;
        case QTI_RADIO_IND_SERVICE_DOMAIN_CHANGED:
            qti_radio_ext_handle_int32_event(self, &args, "ServiceDomain",
                                            qti_radio_ext_service_domain_name);
            return NULL;
        case QTI_RADIO_IND_RADIO_STATE_CHANGED:
            qti_radio_ext_handle_int32_event(self, &args, "RadioState",
                                            qti_radio_ext_radio_state_name);
            return NULL;
        default:
          DBG("Code ignored: %#x -> %s", code, qti_radio_ext_ind_name(code));
        }
    } else {
      DBG("Unknown iface: %s", iface);
    }

    return NULL;
}

gulong
qti_radio_ext_add_ims_reg_status_handler(
    QtiRadioExt* self,
    QtiRadioExtImsRegStatusFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_IMS_REG_STATUS_CHANGED_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_call_state_handler(
    QtiRadioExt* self,
    QtiRadioExtCallStateFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_CALL_STATE_CHANGED_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_ring_handler(
    QtiRadioExt* self,
    QtiRadioExtRingFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_ON_RING_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_ringback_tone_handler(
    QtiRadioExt* self,
    QtiRadioExtRingbackToneFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_ON_RINGBACK_TONE_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_conf_info_handler(
    QtiRadioExt* self,
    QtiRadioExtConfInfoFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_ON_CONF_INFO_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_incoming_sms_handler(
    QtiRadioExt* self,
    QtiRadioExtIncomingSmsFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_ON_INCOMING_SMS_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_incoming_sms_report_handler(
    QtiRadioExt* self,
    QtiRadioExtIncomingSmsReportFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_ON_INCOMING_SMS_REPORT_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_voice_disabled_handler(
    QtiRadioExt* self,
    QtiRadioExtVoiceDisabledFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_ON_VOICE_DISABLED_NAME, G_CALLBACK(handler), user_data) : 0;
}

gulong
qti_radio_ext_add_handover_handler(
    QtiRadioExt* self,
    QtiRadioExtHandoverFunc handler,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(handler)) ? g_signal_connect(self,
        SIGNAL_EXT_HANDOVER_NAME, G_CALLBACK(handler), user_data) : 0;
}

static
GBinderLocalReply*
qti_radio_ext_response(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    QtiRadioExt* self = THIS(user_data);
    const char* iface = gbinder_remote_request_interface(req);
    GBinderReader reader;
    guint32 serial = 0;

    gbinder_remote_request_init_reader(req, &reader);

    gbinder_reader_read_uint32(&reader, &serial);
    qti_radio_ext_log_resp(self, code, serial);
    qti_radio_ext_dump_data(&reader);

    if (serial) {
        QtiRadioExtRequest* req = g_hash_table_lookup(self->requests,
            KEY(serial));

        if (req && req->response_code == code) {
            g_object_ref(self);
            if (req->handle_response) {
                req->handle_response(req, &reader);
            }
            g_hash_table_remove(self->requests, KEY(serial));
            g_object_unref(self);
        } else {
            DBG("Unexpected response %s %u", iface, code);
            *status = GBINDER_STATUS_FAILED;
        }
    }

    return NULL;
}

static
void
qti_radio_ext_result_response(
    QtiRadioExtRequest* req,
    const GBinderReader* args)
{
    GBinderReader reader;
    QtiRadioExt* self = req->radio;
    QtiRadioExtResultRequest* result_req = G_CAST(req,
        QtiRadioExtResultRequest, base);

    gbinder_reader_copy(&reader, args);
    if (result_req->complete) {
        result_req->complete(self, &reader, req->user_data);
    }
}

static
void
qti_radio_ext_request_default_free(
    QtiRadioExtRequest* req)
{
    if (req->destroy) {
        req->destroy(req->user_data);
    }
    g_free(req);
}

static
void
qti_radio_ext_request_destroy(
    gpointer user_data)
{
    QtiRadioExtRequest* req = user_data;

    gbinder_client_cancel(req->radio->client, req->tx);
    req->free(req);
}

static
gpointer
qti_radio_ext_request_alloc(
    QtiRadioExt* self,
    gint32 resp,
    QtiRadioExtRequestHandlerFunc handler,
    GDestroyNotify destroy,
    void* user_data,
    gsize size)
{
    QtiRadioExtRequest* req = g_malloc0(size);

    req->radio = self;
    req->response_code = resp;
    req->handle_response = handler;
    req->id = qti_radio_ext_new_req_id(self);
    req->free = qti_radio_ext_request_default_free;
    req->destroy = destroy;
    req->user_data = user_data;
    if (resp > 0)
      g_hash_table_insert(self->requests, KEY(req->id), req);
    return req;
}

static
QtiRadioExtResultRequest*
qti_radio_ext_result_request_new(
    QtiRadioExt* self,
    gint32 resp,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QtiRadioExtResultRequest* req =
        (QtiRadioExtResultRequest*)qti_radio_ext_request_alloc(self, resp,
            qti_radio_ext_result_response, destroy, user_data,
            sizeof(QtiRadioExtResultRequest));

    req->complete = complete;
    return req;
}

static
void
qti_radio_ext_request_sent(
    GBinderClient* client,
    GBinderRemoteReply* reply,
    int status,
    void* user_data)
{
    ((QtiRadioExtRequest*)user_data)->tx = 0;
}

static
gulong
qti_radio_ext_call(
    QtiRadioExt* self,
    gint32 code,
    gint32 serial,
    GBinderLocalRequest* req,
    GBinderClientReplyFunc reply,
    GDestroyNotify destroy,
    void* user_data)
{
    qti_radio_ext_log_req(self, code, serial);
    qti_radio_ext_dump_request(req);

    return gbinder_client_transact(self->client, code,
        GBINDER_TX_FLAG_ONEWAY, req, reply, destroy, user_data);
}

void
qti_radio_ext_cancel(
    QtiRadioExt* self,
    guint id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_hash_table_remove(self->requests, KEY(id));
    }
}


static
gulong
qti_radio_ext_submit_request(
    QtiRadioExtRequest* request,
    gint32 code,
    gint32 serial,
    GBinderLocalRequest* args)
{
    return (request->tx = qti_radio_ext_call(request->radio,
        code, serial, args, qti_radio_ext_request_sent, NULL, request));
}

static
guint
qti_radio_ext_result_request_submit(
    QtiRadioExt* self,
    gint32 req_code,
    gint32 resp_code,
    QtiRadioExtArgWriteFunc write_args,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data,
    ...)
{
    if (G_LIKELY(self)) {
        GBinderLocalRequest* args;
        GBinderWriter writer;
        gboolean is_async = (resp_code > 0);
        QtiRadioExtResultRequest* req =
            qti_radio_ext_result_request_new(self, resp_code,
                complete, destroy, user_data);
        const guint req_id = req->base.id;

        args = gbinder_client_new_request2(self->client, req_code);
        gbinder_local_request_init_writer(args, &writer);
        gbinder_writer_append_int32(&writer, req_id);
        if (write_args) {
            va_list va;

            va_start(va, user_data);
            write_args(&writer, va);
            va_end(va);
        }

        if (is_async) {
            /* Submit the request */
            qti_radio_ext_submit_request(&req->base, req_code, req_id, args);
            gbinder_local_request_unref(args);
            if (req->base.tx) {
                /* Success */
                return req_id;
            }
            g_hash_table_remove(self->requests, KEY(req_id));
        } else {
            // sync requests
            qti_radio_ext_log_req(self, req_code, req_id);
            qti_radio_ext_dump_request(args);

            int status;
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
                self->client, req_code, args, &status);
            if (status == GBINDER_STATUS_OK) {
                DBG("Reply status: OK");

                GBinderReader reader;
                gbinder_remote_reply_init_reader(reply, &reader);
                qti_radio_ext_dump_data(&reader);

                g_object_ref(self);
                if (req->base.handle_response) {
                    req->base.handle_response((QtiRadioExtRequest *)req, &reader);
                }
                g_object_unref(self);
            } else {
                DBG("Reply status: failed - %d", status);
            }

            gbinder_remote_reply_unref(reply);
            gbinder_local_request_unref(args);
        }
    }
    return 0;
}


static const GBinderClientIfaceInfo radio_iface_info[] = {
    {QTI_RADIO_AIDL, UINT_MAX }
};

typedef struct qti_radio_interface_desc {
    QTI_RADIO_INTERFACE version;
    const char* radio;
    const char* response;
    const char* indication;
} QtiRadioInterfaceDesc;

#define QTI_RADIO_INTERFACE_DESC(v) \
        QTI_RADIO_INTERFACE_##v, \
        QTI_RADIO_##v, \
        QTI_RADIO_RESPONSE_##v, \
        QTI_RADIO_INDICATION_##v

static const QtiRadioInterfaceDesc qti_radio_interfaces[] = {
    { QTI_RADIO_INTERFACE_DESC(AIDL) }
};

#define DEFAULT_INTERFACE QTI_RADIO_INTERFACE_AIDL


static
QtiRadioExt*
qti_radio_ext_create(
    GBinderServiceManager* sm,
    GBinderRemoteObject* remote,
    const char* slot,
    const QtiRadioInterfaceDesc* desc)
{
    QtiRadioExt* self = g_object_new(THIS_TYPE, NULL);
    const gint code = QTI_RADIO_REQ_SET_CALLBACK;
    GBinderLocalRequest* req;
    GBinderWriter writer;
    int status;

    self->slot = g_strdup(slot);
    self->voice_enabled = FALSE;

    self->client = gbinder_client_new2(remote,
        radio_iface_info, G_N_ELEMENTS(radio_iface_info));
    self->response = gbinder_servicemanager_new_local_object(sm,
        desc->response, qti_radio_ext_response, self);
    self->indication = gbinder_servicemanager_new_local_object(sm,
        desc->indication, qti_radio_ext_indication, self);
    gbinder_local_object_set_stability(self->response, GBINDER_STABILITY_VINTF);
    gbinder_local_object_set_stability(self->indication, GBINDER_STABILITY_VINTF);
    req = gbinder_client_new_request2(self->client, code);
    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_local_object(&writer, self->response);
    gbinder_writer_append_local_object(&writer, self->indication);
    qti_radio_ext_log_req(self, code, 0 /*serial*/);
    qti_radio_ext_dump_request(req);
    gbinder_remote_reply_unref(gbinder_client_transact_sync_reply(self->client,
        code, req, &status));
    gbinder_local_request_unref(req);

    return self;
}

/*==========================================================================*
 * API
 *==========================================================================*/


QtiRadioExt*
qti_radio_ext_new_with_version(
    const char* dev,
    const char* slot,
    QTI_RADIO_INTERFACE max_version)
{
    QtiRadioExt* self = NULL;

    GBinderServiceManager* sm = gbinder_servicemanager_new(dev);
    if (sm) {
        guint i;
        for (i = 0; i < G_N_ELEMENTS(qti_radio_interfaces) && !self; i++) {
            const QtiRadioInterfaceDesc* desc = qti_radio_interfaces + i;

            if (desc->version <= max_version) {
                char* fqname = g_strconcat(desc->radio, "/", slot, NULL);
                // try to connect to the service 5 times
                // service might not be ready yet
                GBinderRemoteObject* obj = NULL;
                for (int i = 0; i < 5; i++) {
                    obj = gbinder_servicemanager_get_service_sync(sm, fqname, NULL);
                    if (obj) {
                        break;
                    }
                    // wait 500ms before trying again
                    g_usleep(500000);
                }

                if (obj) {
                    DBG("Connected to %s", fqname);
                    self = qti_radio_ext_create(sm, obj, slot, desc);
                    DBG("Created radio_ext");
                } else {
                    DBG("can't connect to %s", fqname);
                }
                g_free(fqname);
            }
        }
        gbinder_servicemanager_unref(sm);
    }

    return self;
}

QtiRadioExt*
qti_radio_ext_new(
    const char* dev,
    const char* slot)
{
    return qti_radio_ext_new_with_version(dev, slot, DEFAULT_INTERFACE);
}


QtiRadioExt*
qti_radio_ext_ref(
    QtiRadioExt* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return self;
}

void
qti_radio_ext_unref(
    QtiRadioExt* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(self);
    }
}

static
void
qti_radio_ext_set_reg_state_args(
    GBinderWriter* args,
    va_list va)
{
    gbinder_writer_append_int32(args, va_arg(va, gint32));
}

static
QTI_RADIO_REG_STATE
qti_radio_ext_reg_state(
    BINDER_EXT_IMS_REGISTRATION state)
{
    switch (state) {
    case BINDER_EXT_IMS_REGISTRATION_ON:
        return QTI_RADIO_REG_STATE_REGISTERED;
    case BINDER_EXT_IMS_REGISTRATION_OFF:
        return QTI_RADIO_REG_STATE_NOT_REGISTERED;
    default:
        return QTI_RADIO_REG_STATE_INVALID;
    }
}


guint
qti_radio_ext_set_reg_state(
    QtiRadioExt* self,
    BINDER_EXT_IMS_REGISTRATION state,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QTI_RADIO_REG_STATE reg_state = qti_radio_ext_reg_state(state);

    DBG("Setting registration state %d", reg_state);

    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_REQ_REG_CHANGE,
        QTI_RADIO_RESP_REQ_REG_CHANGE,
        qti_radio_ext_set_reg_state_args,
        complete, destroy, user_data,
        reg_state);
}

static
void
qti_radio_ext_dial_args(
    GBinderWriter* writer,
    va_list va)
{
    const char* number = va_arg(va, const char*);
    BINDER_EXT_TOA toa = va_arg(va, BINDER_EXT_TOA);
    BINDER_EXT_CALL_CLIR clir = va_arg(va, BINDER_EXT_CALL_CLIR);
    BINDER_EXT_CALL_DIAL_FLAGS flags = va_arg(va, BINDER_EXT_CALL_DIAL_FLAGS);

    gint32 initial_size;
    gint32 initial_size_call_details;
    gint32 initial_size_multilineinfo;
    gint32 initial_size_redialinfo;

    gint32 clir_qti = QTI_RADIO_IP_PRESENTATION_NUM_DEFAULT;

    if (clir == BINDER_EXT_CALL_CLIR_INVOCATION)
      clir_qti = QTI_RADIO_IP_PRESENTATION_NUM_INVOCATION;
    else if (clir == BINDER_EXT_CALL_CLIR_SUPPRESSION)
      clir_qti = QTI_RADIO_IP_PRESENTATION_NUM_SUPRESSION;

    // Non-null parcelable
    gbinder_writer_append_int32(writer, 1);
    initial_size = gbinder_writer_bytes_written(writer);
    // Dummy parcelable size, replaced at the end
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_string16(writer, number);
    gbinder_writer_append_int32(writer, clir_qti);

    // CallDetails
    gbinder_writer_append_int32(writer, 1);
    initial_size_call_details = gbinder_writer_bytes_written(writer);
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_int32(writer, QTI_RADIO_CALL_TYPE_VOICE); // call type
    gbinder_writer_append_int32(writer, QTI_RADIO_CALL_DOMAIN_AUTOMATIC); // call domain

    gbinder_writer_append_int32(writer, 1); // number of extras strings
    // string from dumped aidl transaction in LOS
    gbinder_writer_append_string16(
        writer, "android.telecom.extra.START_CALL_WITH_VIDEO_STATE=0");

    gbinder_writer_append_int32(writer, 0); // localAbility
    gbinder_writer_append_int32(writer, 0); // peerAbility

    gbinder_writer_append_int32(writer, -1); // callSubstate
    gbinder_writer_append_int32(writer, -1); // mediaId
    gbinder_writer_append_int32(writer, -1); // causeCode

    // rttMode
    gbinder_writer_append_int32(writer,
                                flags & BINDER_EXT_CALL_FLAG_RTT
                                    ? QTI_RADIO_RTT_MODE_FULL
                                    : QTI_RADIO_RTT_MODE_DISABLED);

    gbinder_writer_append_string16(writer, ""); // sipAlternateUri
    gbinder_writer_append_bool(writer, FALSE); // isVosSupported

    // write CallDetails size
    gbinder_writer_overwrite_int32(writer, initial_size_call_details,
        gbinder_writer_bytes_written(writer) - initial_size_call_details);

    // CallDetails: done

    gbinder_writer_append_bool(writer, FALSE); // isConferenceUri
    gbinder_writer_append_bool(writer, FALSE); // isCallPull
    gbinder_writer_append_bool(writer, FALSE); // isEncrypted

    // Multiline info
    gbinder_writer_append_int32(writer, 1);
    initial_size_multilineinfo = gbinder_writer_bytes_written(writer);
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_string16(writer, ""); // msisdn
    gbinder_writer_append_int32(writer, 0); // registrationStatus
    gbinder_writer_append_int32(writer, 1); // linetype: maybe corresponds to primary

    // write Multiline info size
    gbinder_writer_overwrite_int32(writer, initial_size_multilineinfo,
        gbinder_writer_bytes_written(writer) - initial_size_multilineinfo);

    // RedialInfo
    gbinder_writer_append_int32(writer, 1);
    initial_size_redialinfo = gbinder_writer_bytes_written(writer);
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_int32(writer, QTI_RADIO_CALL_FAIL_CAUSE_ERROR_UNSPECIFIED); // callFailReason: default "misc"
    gbinder_writer_append_int32(writer, 0); // callFailRadioTech: unknown?

    // write RedialInfo size
    gbinder_writer_overwrite_int32(writer, initial_size_redialinfo,
        gbinder_writer_bytes_written(writer) - initial_size_redialinfo);

    // write parcelable size
    gbinder_writer_overwrite_int32(writer, initial_size,
        gbinder_writer_bytes_written(writer) - initial_size);
}

guint
qti_radio_ext_dial(
    QtiRadioExt* self,
    const char* number,
    BINDER_EXT_TOA toa,
    BINDER_EXT_CALL_CLIR clir,
    BINDER_EXT_CALL_DIAL_FLAGS flags,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_DIAL,
        QTI_RADIO_RESP_DIAL,
        qti_radio_ext_dial_args,
        complete, destroy, user_data,
        number, toa, clir, flags);
}

typedef struct qti_radio_ext_answer_request {
  gint32 call_type RADIO_ALIGNED(4);
  gint32 presentation RADIO_ALIGNED(4);
  gint32 mode RADIO_ALIGNED(4);
} RADIO_ALIGNED(8) QtiRadioAnswerReq;

static
void
qti_radio_ext_answer_args(
    GBinderWriter* args,
    va_list va)
{
    QtiRadioAnswerReq req;
    req.call_type = va_arg(va, gint32);
    req.presentation = va_arg(va, gint32);
    req.mode = va_arg(va, gint32);
    gbinder_writer_append_parcelable(args, &req, sizeof(req));
}

guint
qti_radio_ext_answer(
    QtiRadioExt* self,
    QTI_RADIO_CALL_TYPE call_type,
    QTI_RADIO_IP_PRESENTATION presentation,
    QTI_RADIO_RTT_MODE mode,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_ANSWER,
        QTI_RADIO_RESP_ANSWER,
        qti_radio_ext_answer_args,
        complete, destroy, user_data,
        call_type, presentation, mode);
}

static
void
qti_radio_ext_write_hangup_request(
    GBinderWriter* writer,
    gint32 conn_index,
    gboolean multiparty,
    const char* conn_uri,
    gint32 fail_cause)
{
    gint32 initial_size;
    gint32 initial_size_failCause;
    gint32 initial_size_sipErrorInfo;

    /* Non-null parcelable */
    gbinder_writer_append_int32(writer, 1);
    initial_size = gbinder_writer_bytes_written(writer);
    /* Dummy parcelable size, replaced at the end */
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_int32(writer, conn_index);
    gbinder_writer_append_bool(writer, multiparty);
    gbinder_writer_append_string16(writer, conn_uri);
    gbinder_writer_append_int32(writer, G_MAXINT); // conf_id

    // parcelable for failCauseResponse
    gbinder_writer_append_int32(writer, 1);
    initial_size_failCause = gbinder_writer_bytes_written(writer);
    /* Dummy parcelable size, replaced at the end */
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_int32(writer, fail_cause);
    gbinder_writer_append_int32(writer, 0); // errorInfo byte array set to empty one (0 bytes)
    gbinder_writer_append_string16(writer, ""); // networkErrorString
    gbinder_writer_append_bool(writer, FALSE); // hasErrorDetails

    // parcelable for SipErrorInfo
    gbinder_writer_append_int32(writer, 1); // SipErrorInfo parcelable is empty
    initial_size_sipErrorInfo = gbinder_writer_bytes_written(writer);
    /* Dummy parcelable size, replaced at the end */
    gbinder_writer_append_int32(writer, 0);
    gbinder_writer_append_int32(writer, 0); // sip error set zero
    gbinder_writer_append_string16(writer, ""); // networkErrorString

    /* Overwrite parcelable size for SipErrorInfo */
    gbinder_writer_overwrite_int32(writer, initial_size_sipErrorInfo,
        gbinder_writer_bytes_written(writer) - initial_size_sipErrorInfo);

    /* Overwrite parcelable size for failCauseResponse */
    gbinder_writer_overwrite_int32(writer, initial_size_failCause,
        gbinder_writer_bytes_written(writer) - initial_size_failCause);

    /* Overwrite parcelable size */
    gbinder_writer_overwrite_int32(writer, initial_size,
        gbinder_writer_bytes_written(writer) - initial_size);
}

static
void
qti_radio_ext_hangup_args(
    GBinderWriter* writer,
    va_list va)
{
    guint call_id = va_arg(va, guint);
    BINDER_EXT_CALL_HANGUP_REASON reason = va_arg(va, BINDER_EXT_CALL_HANGUP_REASON);
    BINDER_EXT_CALL_HANGUP_FLAGS flags = va_arg(va, BINDER_EXT_CALL_HANGUP_FLAGS);

    gint32 failCauseReason; // Normal call end BINDER_EXT_CALL_HANGUP_TERMINATE
    if (reason == BINDER_EXT_CALL_HANGUP_IGNORE)
        failCauseReason = QTI_RADIO_CALL_FAIL_CAUSE_NORMAL_UNSPECIFIED;
    else if (reason == BINDER_EXT_CALL_HANGUP_REJECT)
        failCauseReason = QTI_RADIO_CALL_FAIL_CAUSE_USER_REJECT;
    else // BINDER_EXT_CALL_HANGUP_TERMINATE
      failCauseReason = QTI_RADIO_CALL_FAIL_CAUSE_NORMAL;

    (void)flags;
    qti_radio_ext_write_hangup_request(writer, call_id, FALSE, "",
        failCauseReason);
}

static
void
qti_radio_ext_remove_participant_args(
    GBinderWriter* writer,
    va_list va)
{
    const char* uri = va_arg(va, const char*);

    /*
     * Like ims.apk's ImsCallSessionImpl.removeParticipants ->
     * hangupWithReason(0, uri, null, true, CODE_USER_TERMINATED) ->
     * StableAidl.fromHangup: no connection index (INT_MAX), multiParty
     * set, the participant's URI, fail cause 501.
     */
    qti_radio_ext_write_hangup_request(writer, G_MAXINT, TRUE, uri,
        QTI_RADIO_CALL_FAIL_CAUSE_USER_BUSY);
}

guint
qti_radio_ext_remove_participant(
    QtiRadioExt* self,
    const char* uri,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_HANGUP,
        QTI_RADIO_RESP_HANGUP,
        qti_radio_ext_remove_participant_args,
        complete, destroy, user_data,
        uri);
}

guint
qti_radio_ext_hangup(
    QtiRadioExt* self,
    guint call_id,
    BINDER_EXT_CALL_HANGUP_REASON reason,
    BINDER_EXT_CALL_HANGUP_FLAGS flags,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_HANGUP,
        QTI_RADIO_RESP_HANGUP,
        qti_radio_ext_hangup_args,
        complete, destroy, user_data,
        call_id, reason, flags);
}

static
void
qti_radio_ext_hold_args(
    GBinderWriter* writer,
    va_list va)
{
    guint call_id = va_arg(va, guint);

    gbinder_writer_append_int32(writer, call_id);
}

guint
qti_radio_ext_hold(
    QtiRadioExt* self,
    guint call_id,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_HOLD,
        QTI_RADIO_RESP_HOLD,
        qti_radio_ext_hold_args,
        complete, destroy, user_data,
        call_id);
}

static
void
qti_radio_ext_resume_args(
    GBinderWriter* writer,
    va_list va)
{
    guint call_id = va_arg(va, guint);

    gbinder_writer_append_int32(writer, call_id);
}

guint
qti_radio_ext_resume(
    QtiRadioExt* self,
    guint call_id,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_RESUME,
        QTI_RADIO_RESP_RESUME,
        qti_radio_ext_resume_args,
        complete, destroy, user_data,
        call_id);
}

/*
 * IImsRadio V12 conference(int token) = 14, answered by
 * conferenceResponse(int token, ErrorCode, SipErrorInfo) = 13
 * (vendor.qti.hardware.radio.ims-V12-ndk_platform.so BpImsRadio::conference,
 * BpImsRadioResponse::conferenceResponse; ims.apk TRANSACTION_conference).
 * The modem merges its ACTIVE and HOLDING calls; there are no arguments
 * besides the token, which qti_radio_ext_result_request_submit writes.
 */
guint
qti_radio_ext_conference(
    QtiRadioExt* self,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_CONFERENCE,
        QTI_RADIO_RESP_CONFERENCE,
        NULL,
        complete, destroy, user_data);
}

static
void
qti_radio_ext_send_ims_sms_args(
    GBinderWriter* writer,
    va_list va)
{
    const char* smsc = va_arg(va, const char*);
    const void* pdu = va_arg(va, const void*);
    gsize pdu_len = va_arg(va, gsize);
    guint msg_ref = va_arg(va, guint);
    BINDER_EXT_SMS_SEND_FLAGS flags = va_arg(va, BINDER_EXT_SMS_SEND_FLAGS);

    gint32 initial_size;

    // Non-null parcelable
    gbinder_writer_append_int32(writer, 1);

    initial_size = gbinder_writer_bytes_written(writer);
    // Dummy parcelable size, replaced at the end
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_int32(writer, msg_ref);
    gbinder_writer_append_string16(writer, "3gpp");
    gbinder_writer_append_string16(writer, smsc ? smsc : "");
    gbinder_writer_append_bool(writer, FALSE); // retry via ofono if needed
    gbinder_writer_append_byte_array(writer, pdu, pdu_len);

    // write parcelable size
    gbinder_writer_overwrite_int32(writer, initial_size,
        gbinder_writer_bytes_written(writer) - initial_size);
}

guint
qti_radio_ext_send_ims_sms(
    QtiRadioExt* self,
    const char* smsc,
    const void* pdu,
    gsize pdu_len,
    guint msg_ref,
    BINDER_EXT_SMS_SEND_FLAGS flags,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_SEND_IMS_SMS,
        QTI_RADIO_RESP_SEND_IMS_SMS,
        qti_radio_ext_send_ims_sms_args,
        complete, destroy, user_data,
        smsc, pdu, pdu_len, msg_ref, flags);
}

static
void
qti_radio_ext_acknowledge_sms_args(
    GBinderWriter* writer,
    va_list va)
{
    guint32 message_ref = va_arg(va, guint32);
    guint32 sms_result = va_arg(va, guint32);

    gint32 initial_size;

    // Non-null parcelable
    gbinder_writer_append_int32(writer, 1);

    initial_size = gbinder_writer_bytes_written(writer);
    // Dummy parcelable size, replaced at the end
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_int32(writer, message_ref);
    gbinder_writer_append_int32(writer, sms_result);

    // write parcelable size
    gbinder_writer_overwrite_int32(writer, initial_size,
        gbinder_writer_bytes_written(writer) - initial_size);
}

guint
qti_radio_ext_acknowledge_sms(
    QtiRadioExt* self,
    guint32 message_ref,
    gboolean sms_result,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    QTI_RADIO_IMS_SMS_DELIVER_STATUS_RESULT sms_result_code = sms_result ? QTI_RADIO_DELIVER_STATUS_OK : QTI_RADIO_DELIVER_STATUS_ERROR;

    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_ACK_SMS,
        QTI_RADIO_RESP_ACK_SMS,
        qti_radio_ext_acknowledge_sms_args,
        complete, destroy, user_data,
        message_ref, sms_result_code);
}

static
void
qti_radio_ext_acknowledge_sms_report_args(
    GBinderWriter* writer,
    va_list va)
{
    guint32 message_ref = va_arg(va, guint32);
    guint32 sms_report_code = va_arg(va, guint32);

    gint32 initial_size;

    // Non-null parcelable
    gbinder_writer_append_int32(writer, 1);

    initial_size = gbinder_writer_bytes_written(writer);
    // Dummy parcelable size, replaced at the end
    gbinder_writer_append_int32(writer, 0);

    gbinder_writer_append_int32(writer, message_ref);
    gbinder_writer_append_int32(writer, sms_report_code);

    // write parcelable size
    gbinder_writer_overwrite_int32(writer, initial_size,
        gbinder_writer_bytes_written(writer) - initial_size);
}

guint
qti_radio_ext_acknowledge_sms_report(
    QtiRadioExt* self,
    guint32 message_ref,
    gboolean sms_report,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    guint32 sms_report_code = sms_report ? QTI_RADIO_STATUS_REPORT_OK : QTI_RADIO_STATUS_REPORT_ERROR;

    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_ACK_SMS_REPORT,
        QTI_RADIO_RESP_ACK_SMS_REPORT,
        qti_radio_ext_acknowledge_sms_report_args,
        complete, destroy, user_data,
        message_ref, sms_report_code);
}


static
void
qti_radio_ext_get_ims_reg_state_args(
    GBinderWriter* args,
    va_list va)
{
    // empty
}

guint
qti_radio_ext_get_ims_reg_state(
    QtiRadioExt* self,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return qti_radio_ext_result_request_submit(self,
        QTI_RADIO_REQ_GET_IMS_REG_STATE,
        QTI_RADIO_RESP_GET_IMS_REG_STATE,
        qti_radio_ext_get_ims_reg_state_args,
        complete, destroy, user_data);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
qti_radio_ext_finalize(
    GObject* object)
{
    QtiRadioExt* self = THIS(object);

    g_free(self->slot);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
qti_radio_ext_init(
    QtiRadioExt* self)
{
    self->pool = gutil_idle_pool_new();
    self->requests = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
        qti_radio_ext_request_destroy);
}

static
void
qti_radio_ext_class_init(
    QtiRadioExtClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = qti_radio_ext_finalize;
    qti_radio_ext_signals[SIGNAL_IMS_REG_STATUS_CHANGED] =
        g_signal_new(SIGNAL_IMS_REG_STATUS_CHANGED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            1, G_TYPE_UINT);
    qti_radio_ext_signals[SIGNAL_EXT_CALL_STATE_CHANGED] =
        g_signal_new(SIGNAL_EXT_CALL_STATE_CHANGED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            1, G_TYPE_PTR_ARRAY);
    qti_radio_ext_signals[SIGNAL_EXT_ON_RING] =
        g_signal_new(SIGNAL_EXT_ON_RING_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            0);
    qti_radio_ext_signals[SIGNAL_EXT_ON_INCOMING_SMS] =
        g_signal_new(SIGNAL_EXT_ON_INCOMING_SMS_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            2, G_TYPE_POINTER, G_TYPE_UINT);
    qti_radio_ext_signals[SIGNAL_EXT_ON_INCOMING_SMS_REPORT] =
        g_signal_new(SIGNAL_EXT_ON_INCOMING_SMS_REPORT_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            3, G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_UINT);
    qti_radio_ext_signals[SIGNAL_EXT_ON_VOICE_DISABLED] =
        g_signal_new(SIGNAL_EXT_ON_VOICE_DISABLED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            0);
    qti_radio_ext_signals[SIGNAL_EXT_HANDOVER] =
        g_signal_new(SIGNAL_EXT_HANDOVER_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            3, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);
    qti_radio_ext_signals[SIGNAL_EXT_ON_RINGBACK_TONE] =
        g_signal_new(SIGNAL_EXT_ON_RINGBACK_TONE_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            1, G_TYPE_BOOLEAN);
    qti_radio_ext_signals[SIGNAL_EXT_ON_CONF_INFO] =
        g_signal_new(SIGNAL_EXT_ON_CONF_INFO_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            3, G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_INT);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
