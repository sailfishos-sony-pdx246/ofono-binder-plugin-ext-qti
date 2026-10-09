/*
 *  oFono - Open Source Telephony - binder based adaptation QTI plugin
 *
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

#ifndef QTI_RADIO_EXT_H
#define QTI_RADIO_EXT_H

#include <radio_types.h>
#include <binder_ext_ims_impl.h>
#include <binder_ext_call_impl.h>
#include <binder_ext_sms_impl.h>

#include "qti_radio_ext_types.h"

#define BINDER_EXT_CALL_STATE_END (BINDER_EXT_CALL_STATE_INVALID - 1)

typedef struct qti_radio_ext QtiRadioExt;

typedef void (*QtiRadioExtResultFunc)(
    QtiRadioExt* radio,
    GBinderReader* reader,
    void* user_data);

typedef void (*QtiRadioExtImsRegStatusFunc)(
    QtiRadioExt* radio,
    guint32 status,
    void* user_data);

typedef void (*QtiRadioExtCallStateFunc)(
    QtiRadioExt* radio,
    GPtrArray* updated_calls,
    void* user_data);

typedef void (*QtiRadioExtRingFunc)(
    QtiRadioExt* radio,
    void* user_data);

typedef void (*QtiRadioExtRingbackToneFunc)(
    QtiRadioExt* radio,
    gboolean start,
    void* user_data);

typedef void (*QtiRadioExtIncomingSmsFunc)(
    QtiRadioExt* radio,
    const void* pdu,
    guint pdu_len,
    void* user_data);

typedef void (*QtiRadioExtIncomingSmsReportFunc)(
    QtiRadioExt* radio,
    const void* pdu,
    guint pdu_len,
    guint msg_ref,
    void* user_data);

typedef void (*QtiRadioExtVoiceDisabledFunc)(
    QtiRadioExt* radio,
    void* user_data);

typedef void (*QtiRadioExtHandoverFunc)(
    QtiRadioExt* radio,
    QTI_RADIO_HANDOVER_TYPE type,
    QTI_RADIO_TECH_TYPE srcTech,
    QTI_RADIO_TECH_TYPE targetTech,
    void* user_data);

QtiRadioExt*
qti_radio_ext_new(
    const char* dev,
    const char* slot);

QtiRadioExt*
qti_radio_ext_ref(
    QtiRadioExt* self);

void
qti_radio_ext_unref(
    QtiRadioExt* self);

void
qti_radio_ext_cancel(
    QtiRadioExt* self,
    guint id);

QTI_RADIO_REG_STATE
qti_radio_ext_read_ims_reg_status_info(
    QtiRadioExt* self,
    GBinderReader* reader);

BINDER_EXT_CALL_STATE
qti_radio_ims_call_radio_state_to_state(QTI_RADIO_CALL_STATE state);

guint
qti_radio_ext_set_reg_state(
    QtiRadioExt* self,
    BINDER_EXT_IMS_REGISTRATION state,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
qti_radio_ext_dial(
    QtiRadioExt* self,
    const char* number,
    BINDER_EXT_TOA toa,
    BINDER_EXT_CALL_CLIR clir,
    BINDER_EXT_CALL_DIAL_FLAGS flags,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
qti_radio_ext_answer(
    QtiRadioExt* self,
    QTI_RADIO_CALL_TYPE call_type,
    QTI_RADIO_IP_PRESENTATION presentation,
    QTI_RADIO_RTT_MODE mode,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
qti_radio_ext_hangup(
    QtiRadioExt* self,
    guint call_id,
    BINDER_EXT_CALL_HANGUP_REASON reason,
    BINDER_EXT_CALL_HANGUP_FLAGS flags,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
qti_radio_ext_hold(
    QtiRadioExt* self,
    guint call_id,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
qti_radio_ext_resume(
    QtiRadioExt* self,
    guint call_id,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

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
    void* user_data);

guint
qti_radio_ext_get_ims_reg_state(
    QtiRadioExt* self,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

gulong
qti_radio_ext_add_ims_reg_status_handler(
    QtiRadioExt* self,
    QtiRadioExtImsRegStatusFunc handler,
    void* user_data);

gulong
qti_radio_ext_add_call_state_handler(
    QtiRadioExt* self,
    QtiRadioExtCallStateFunc handler,
    void* user_data);

gulong
qti_radio_ext_add_ring_handler(
    QtiRadioExt* self,
    QtiRadioExtRingFunc handler,
    void* user_data);

gulong
qti_radio_ext_add_ringback_tone_handler(
    QtiRadioExt* self,
    QtiRadioExtRingbackToneFunc handler,
    void* user_data);

gulong
qti_radio_ext_add_incoming_sms_handler(
    QtiRadioExt* self,
    QtiRadioExtIncomingSmsFunc handler,
    void* user_data);

gulong
qti_radio_ext_add_incoming_sms_report_handler(
    QtiRadioExt* self,
    QtiRadioExtIncomingSmsReportFunc handler,
    void* user_data);

guint
qti_radio_ext_acknowledge_sms(
    QtiRadioExt* self,
    guint32 message_ref,
    gboolean sms_result,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
qti_radio_ext_acknowledge_sms_report(
    QtiRadioExt* self,
    guint32 message_ref,
    gboolean sms_report,
    QtiRadioExtResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

gulong
qti_radio_ext_add_voice_disabled_handler(
    QtiRadioExt* self,
    QtiRadioExtVoiceDisabledFunc handler,
    void* user_data);

gulong
qti_radio_ext_add_handover_handler(
    QtiRadioExt* self,
    QtiRadioExtHandoverFunc handler,
    void* user_data);

#endif /* QTI_RADIO_EXT_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
