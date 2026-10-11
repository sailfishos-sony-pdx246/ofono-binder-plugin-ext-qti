/*
 * Copyright (C) 2022 Jolla Ltd.
 * Copyright (C) 2022 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2024 Marius Gripsgard <marius@ubports.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#include "qti_slot.h"
#include "qti_ims.h"
#include "qti_radio_ext.h"
#include "qti_ims_call.h"
#include "qti_ims_sms.h"

#include <binder_ext_slot_impl.h>

#include <radio_instance.h>
#include <ofono/log.h>
#include <gutil_macros.h>
#include <gutil_log.h>

#undef DBG
#define DBG(fmt, ...) \
    gutil_log(GLOG_MODULE_CURRENT, GLOG_LEVEL_ALWAYS, "ims:"fmt, ##__VA_ARGS__)

typedef BinderExtSlotClass QtiSlotClass;
typedef struct qti_slot {
    BinderExtSlot parent;
    BinderExtIms* ims;
    QtiRadioExt* radio_ext;
    BinderExtCall* call;
    BinderExtSms* sms;
} QtiSlot;

GType qti_slot_get_type() G_GNUC_INTERNAL;
G_DEFINE_TYPE(QtiSlot, qti_slot, BINDER_EXT_TYPE_SLOT)

#define THIS_TYPE qti_slot_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, QtiSlot)
#define PARENT_CLASS qti_slot_parent_class

static
void
qti_slot_terminate(
    QtiSlot* self)
{
    if (self->ims) {
        binder_ext_ims_unref(self->ims);
        self->ims = NULL;
    }
}

/*==========================================================================*
 * BinderExtSlot
 *==========================================================================*/

static
gpointer
qti_slot_get_interface(
    BinderExtSlot* slot,
    GType iface)
{
    QtiSlot* self = THIS(slot);

    if (iface == BINDER_EXT_TYPE_IMS) {
        return self->ims;
    } else if (iface == BINDER_EXT_TYPE_CALL) {
        return self->call;
    } else if (iface == BINDER_EXT_TYPE_SMS) {
        return self->sms;
    } else {
        return BINDER_EXT_SLOT_CLASS(PARENT_CLASS)->get_interface(slot, iface);
    }
}

static
void
qti_slot_shutdown(
    BinderExtSlot* slot)
{
    qti_slot_terminate(THIS(slot));
    BINDER_EXT_SLOT_CLASS(PARENT_CLASS)->shutdown(slot);
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtSlot*
qti_slot_new(
    RadioInstance* radio,
    GHashTable* params)
{
    QtiSlot* self = g_object_new(THIS_TYPE, NULL);
    BinderExtSlot* slot = &self->parent;
    char* radio_slot =  g_strdup_printf("imsradio%d", radio->slot_index);
    const char *dev = "/dev/binder"; //radio->dev;

    DBG("In QTI slot_new %s", dev);

    self->radio_ext = qti_radio_ext_new(dev, radio_slot);
    if (self->radio_ext) {
        /* extPlugin = qti:memberHangup=participant (see qti.conf) */
        const char* member_hangup = params ?
            g_hash_table_lookup(params, "memberHangup") : NULL;

        self->ims = qti_ims_new(radio_slot, self->radio_ext);
        self->call = qti_ims_call_new(self->radio_ext);
        self->sms = qti_ims_sms_new(self->radio_ext);
        if (member_hangup && g_strcmp0(member_hangup, "participant") &&
            g_strcmp0(member_hangup, "conference")) {
            ofono_warn("qti: unknown memberHangup value '%s', using "
                "'conference'", member_hangup);
        }
        qti_ims_call_set_remove_participants(self->call,
            !g_strcmp0(member_hangup, "participant"));
    } else {
        DBG("slot->radio_ext is null ");
    }

    return slot;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
qti_slot_finalize(
    GObject* object)
{
    qti_slot_terminate(THIS(object));
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
qti_slot_init(
    QtiSlot* self)
{
}

static
void
qti_slot_class_init(
    QtiSlotClass* klass)
{
    klass->get_interface = qti_slot_get_interface;
    klass->shutdown = qti_slot_shutdown;
    G_OBJECT_CLASS(klass)->finalize = qti_slot_finalize;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
