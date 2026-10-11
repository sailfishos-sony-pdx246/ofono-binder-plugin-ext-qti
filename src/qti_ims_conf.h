/*
 * SPDX-FileCopyrightText: Johannes Meyer zum Alten Borgloh
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef QTI_IMS_CONF_H
#define QTI_IMS_CONF_H

#include <glib.h>

/*
 * Participants of an IMS conference, from the conference-info documents
 * (RFC 4575) that the network sends in SIP NOTIFYs and the modem forwards
 * in IImsRadioIndication.onRefreshConferenceInfo. Documents come as full
 * state or as partial updates; this keeps the merged state, like
 * Qualcomm's org.codeaurora.ims.parser.ConfInfo.
 */

typedef struct qti_ims_conf QtiImsConf;

typedef struct qti_ims_conf_party {
    const char* entity; /* the user's URI, e.g. sip:+49...@host;user=phone */
    gboolean seen;      /* was in the conference at some point */
    gboolean gone;      /* deleted, or none of its endpoints is connected */
} QtiImsConfParty;

typedef enum qti_ims_conf_update_result {
    QTI_IMS_CONF_UPDATE_APPLIED,
    QTI_IMS_CONF_UPDATE_STALE,   /* a partial update, not newer */
    QTI_IMS_CONF_UPDATE_INVALID  /* not a conference-info document */
} QTI_IMS_CONF_UPDATE_RESULT;

QtiImsConf*
qti_ims_conf_new(
    void);

void
qti_ims_conf_free(
    QtiImsConf* conf);

/* Forgets all parties and the version (a new conference starts) */
void
qti_ims_conf_clear(
    QtiImsConf* conf);

QTI_IMS_CONF_UPDATE_RESULT
qti_ims_conf_update(
    QtiImsConf* conf,
    const void* xml,
    gsize len,
    GError** error);

/* The party with this phone number, NULL if none or more than one */
const QtiImsConfParty*
qti_ims_conf_find(
    QtiImsConf* conf,
    const char* number);

/*
 * Someone with this number is merged into the conference (again): they
 * count as having left only after a later document lists them again
 */
void
qti_ims_conf_rejoin(
    QtiImsConf* conf,
    const char* number);

/* One line for the log, numbers masked */
char*
qti_ims_conf_describe(
    QtiImsConf* conf);

/* A URI or number for the log: digits except the last two masked, no host */
char*
qti_ims_conf_mask(
    const char* str);

#endif /* QTI_IMS_CONF_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
