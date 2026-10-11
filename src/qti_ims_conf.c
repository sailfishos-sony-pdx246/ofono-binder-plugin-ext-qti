/*
 * SPDX-FileCopyrightText: Johannes Meyer zum Alten Borgloh
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qti_ims_conf.h"

#include <string.h>

/* RFC 4575 'state' attribute */
typedef enum qti_ims_conf_state {
    CONF_STATE_NONE,    /* attribute not present */
    CONF_STATE_FULL,
    CONF_STATE_PARTIAL,
    CONF_STATE_DELETED
} CONF_STATE;

#define ENDPOINT_IN  GINT_TO_POINTER(1)
#define ENDPOINT_OUT GINT_TO_POINTER(2)

/*
 * Numbers match if their last MATCH_DIGITS digits do (fewer if one is
 * shorter), so that the national and the international format of the
 * same number match. Both need at least MATCH_MIN_DIGITS digits.
 */
#define MATCH_DIGITS (9)
#define MATCH_MIN_DIGITS (6)

typedef struct qti_ims_conf_party_priv {
    QtiImsConfParty pub;
    char* entity;
    char* digits;          /* of the entity's user part, or of an endpoint */
    GHashTable* endpoints; /* endpoint entity -> ENDPOINT_IN/OUT */
    gboolean had_endpoints;
    gboolean listed;       /* in the document being applied */
} QtiImsConfPartyPriv;

struct qti_ims_conf {
    GHashTable* parties;   /* entity -> QtiImsConfPartyPriv* */
    char* entity;          /* the conference's URI, if a document named it */
    gboolean have_version;
    guint version;
};

/* A parsed document */

typedef struct qti_ims_conf_doc_endpoint {
    char* entity;
    CONF_STATE state;
    char* status;          /* NULL if the document has none */
} ConfDocEndpoint;

typedef struct qti_ims_conf_doc_user {
    char* entity;
    CONF_STATE state;
    GPtrArray* endpoints;  /* ConfDocEndpoint* */
} ConfDocUser;

typedef struct qti_ims_conf_doc {
    gboolean is_conf_info;
    char* entity;
    CONF_STATE state;
    gboolean have_version;
    guint version;
    gboolean have_users;
    CONF_STATE users_state;
    GPtrArray* users;      /* ConfDocUser* */
    ConfDocEndpoint* status_endpoint; /* inside its <status> element */
    GString* text;
} ConfDoc;

/*==========================================================================*
 * Helpers
 *==========================================================================*/

/* The digits of a number, or of the user part of a sip:, sips: or tel: URI */
static
char*
qti_ims_conf_digits(
    const char* str)
{
    GString* digits = g_string_new(NULL);

    if (str) {
        const char* p = str;
        const char* end;

        if (!g_ascii_strncasecmp(p, "sip:", 4) ||
            !g_ascii_strncasecmp(p, "tel:", 4)) {
            p += 4;
        } else if (!g_ascii_strncasecmp(p, "sips:", 5)) {
            p += 5;
        }
        for (end = p + strcspn(p, "@;?>"); p < end; p++) {
            if (g_ascii_isdigit(*p)) {
                g_string_append_c(digits, *p);
            }
        }
    }
    return g_string_free(digits, FALSE);
}

static
gboolean
qti_ims_conf_digits_match(
    const char* a,
    const char* b)
{
    const gsize la = strlen(a);
    const gsize lb = strlen(b);
    gsize n;

    if (la < MATCH_MIN_DIGITS || lb < MATCH_MIN_DIGITS) {
        return FALSE;
    }
    n = MIN(MIN(la, lb), MATCH_DIGITS);
    return !memcmp(a + la - n, b + lb - n, n);
}

/* RFC 4575 endpoint-status-type; "connect-fail" is Android's addition */
static
gboolean
qti_ims_conf_status_is_in(
    const char* status)
{
    return g_ascii_strcasecmp(status, "disconnected") &&
        g_ascii_strcasecmp(status, "connect-fail");
}

char*
qti_ims_conf_mask(
    const char* str)
{
    GString* out;
    const char* p;
    gsize len;

    if (!str) {
        return g_strdup("(none)");
    }

    /* The scheme stays, the user part shows its last two characters */
    out = g_string_new(NULL);
    p = str;
    if (!g_ascii_strncasecmp(p, "sip:", 4) ||
        !g_ascii_strncasecmp(p, "tel:", 4)) {
        g_string_append_len(out, p, 4);
        p += 4;
    } else if (!g_ascii_strncasecmp(p, "sips:", 5)) {
        g_string_append_len(out, p, 5);
        p += 5;
    }
    len = strcspn(p, "@;?>");
    for (gsize i = 0; i < len; i++) {
        g_string_append_c(out, (i + 2 >= len) ? p[i] : '*');
    }
    if (p[len] == '@') {
        g_string_append(out, "@...");
    }
    return g_string_free(out, FALSE);
}

/*==========================================================================*
 * Parser
 *==========================================================================*/

static
void
qti_ims_conf_doc_endpoint_free(
    gpointer data)
{
    ConfDocEndpoint* ep = data;

    g_free(ep->entity);
    g_free(ep->status);
    g_free(ep);
}

static
void
qti_ims_conf_doc_user_free(
    gpointer data)
{
    ConfDocUser* user = data;

    g_free(user->entity);
    g_ptr_array_free(user->endpoints, TRUE);
    g_free(user);
}

static
void
qti_ims_conf_doc_free(
    ConfDoc* doc)
{
    g_ptr_array_free(doc->users, TRUE);
    g_string_free(doc->text, TRUE);
    g_free(doc->entity);
    g_free(doc);
}

static
const char*
qti_ims_conf_local_name(
    const char* name)
{
    const char* colon = strrchr(name, ':');

    return colon ? colon + 1 : name;
}

static
const char*
qti_ims_conf_attr(
    const char** names,
    const char** values,
    const char* attr)
{
    for (guint i = 0; names[i]; i++) {
        if (strncmp(names[i], "xmlns", 5) &&
            !strcmp(qti_ims_conf_local_name(names[i]), attr)) {
            return values[i];
        }
    }
    return NULL;
}

static
CONF_STATE
qti_ims_conf_state(
    const char* value)
{
    if (!value) {
        return CONF_STATE_NONE;
    } else if (!g_ascii_strcasecmp(value, "partial")) {
        return CONF_STATE_PARTIAL;
    } else if (!g_ascii_strcasecmp(value, "deleted")) {
        return CONF_STATE_DELETED;
    } else {
        return CONF_STATE_FULL;
    }
}

/*
 * Only the conference's own users count: <conference-info> <users> <user>
 * <endpoint> <status>, at exactly these depths (sidebars-by-val has
 * nested <users> of other conferences).
 */
static
void
qti_ims_conf_start_element(
    GMarkupParseContext* ctx,
    const char* element,
    const char** names,
    const char** values,
    gpointer user_data,
    GError** error)
{
    ConfDoc* doc = user_data;
    const char* name = qti_ims_conf_local_name(element);
    const guint depth =
        g_slist_length((GSList*)g_markup_parse_context_get_element_stack(ctx));

    if (depth == 1) {
        if (!strcmp(name, "conference-info")) {
            const char* version = qti_ims_conf_attr(names, values, "version");

            doc->is_conf_info = TRUE;
            doc->entity = g_strdup(qti_ims_conf_attr(names, values,
                "entity"));
            doc->state = qti_ims_conf_state(qti_ims_conf_attr(names,
                values, "state"));
            if (version) {
                char* end = NULL;
                const guint64 v = g_ascii_strtoull(version, &end, 10);

                if (end != version && !*end && v <= G_MAXUINT) {
                    doc->have_version = TRUE;
                    doc->version = (guint)v;
                }
            }
        }
    } else if (!doc->is_conf_info) {
        return;
    } else if (depth == 2) {
        if (!strcmp(name, "users")) {
            doc->have_users = TRUE;
            doc->users_state = qti_ims_conf_state(qti_ims_conf_attr(names,
                values, "state"));
        }
    } else if (depth == 3) {
        if (!strcmp(name, "user") && doc->have_users) {
            ConfDocUser* user = g_new0(ConfDocUser, 1);

            user->entity = g_strdup(qti_ims_conf_attr(names, values,
                "entity"));
            user->state = qti_ims_conf_state(qti_ims_conf_attr(names,
                values, "state"));
            user->endpoints = g_ptr_array_new_with_free_func(
                qti_ims_conf_doc_endpoint_free);
            g_ptr_array_add(doc->users, user);
        }
    } else if (depth == 4) {
        const GSList* stack = g_markup_parse_context_get_element_stack(ctx);

        if (!strcmp(name, "endpoint") && doc->users->len &&
            !strcmp(qti_ims_conf_local_name(stack->next->data), "user")) {
            ConfDocUser* user = g_ptr_array_index(doc->users,
                doc->users->len - 1);
            ConfDocEndpoint* ep = g_new0(ConfDocEndpoint, 1);

            ep->entity = g_strdup(qti_ims_conf_attr(names, values,
                "entity"));
            ep->state = qti_ims_conf_state(qti_ims_conf_attr(names,
                values, "state"));
            g_ptr_array_add(user->endpoints, ep);
        }
    } else if (depth == 5) {
        const GSList* stack = g_markup_parse_context_get_element_stack(ctx);

        if (!strcmp(name, "status") && doc->users->len &&
            !strcmp(qti_ims_conf_local_name(stack->next->data),
                "endpoint")) {
            ConfDocUser* user = g_ptr_array_index(doc->users,
                doc->users->len - 1);

            if (user->endpoints->len) {
                doc->status_endpoint = g_ptr_array_index(user->endpoints,
                    user->endpoints->len - 1);
                g_string_truncate(doc->text, 0);
            }
        }
    }
}

static
void
qti_ims_conf_end_element(
    GMarkupParseContext* ctx,
    const char* element,
    gpointer user_data,
    GError** error)
{
    ConfDoc* doc = user_data;

    if (doc->status_endpoint &&
        !strcmp(qti_ims_conf_local_name(element), "status")) {
        g_free(doc->status_endpoint->status);
        doc->status_endpoint->status =
            g_strstrip(g_strdup(doc->text->str));
        doc->status_endpoint = NULL;
    }
}

static
void
qti_ims_conf_text(
    GMarkupParseContext* ctx,
    const char* text,
    gsize len,
    gpointer user_data,
    GError** error)
{
    ConfDoc* doc = user_data;

    if (doc->status_endpoint) {
        g_string_append_len(doc->text, text, len);
    }
}

static
ConfDoc*
qti_ims_conf_doc_parse(
    const void* xml,
    gsize len,
    GError** error)
{
    static const GMarkupParser parser = {
        qti_ims_conf_start_element,
        qti_ims_conf_end_element,
        qti_ims_conf_text,
        NULL,
        NULL
    };
    const char* text = xml;
    ConfDoc* doc;
    GMarkupParseContext* ctx;
    gboolean ok;

    if (!text) {
        text = "";
        len = 0;
    }
    /* A UTF-8 BOM and trailing NULs would upset GMarkup */
    if (len >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) {
        text += 3;
        len -= 3;
    }
    while (len && !text[len - 1]) {
        len--;
    }

    doc = g_new0(ConfDoc, 1);
    doc->users = g_ptr_array_new_with_free_func(qti_ims_conf_doc_user_free);
    doc->text = g_string_new(NULL);
    ctx = g_markup_parse_context_new(&parser, 0, doc, NULL);
    ok = g_markup_parse_context_parse(ctx, text, len, error) &&
        g_markup_parse_context_end_parse(ctx, error);
    g_markup_parse_context_free(ctx);

    if (!ok) {
        qti_ims_conf_doc_free(doc);
        return NULL;
    }
    return doc;
}

/*==========================================================================*
 * Parties
 *==========================================================================*/

static
void
qti_ims_conf_party_free(
    gpointer data)
{
    QtiImsConfPartyPriv* party = data;

    g_hash_table_destroy(party->endpoints);
    g_free(party->entity);
    g_free(party->digits);
    g_free(party);
}

static
QtiImsConfPartyPriv*
qti_ims_conf_party_new(
    const char* entity)
{
    QtiImsConfPartyPriv* party = g_new0(QtiImsConfPartyPriv, 1);

    party->pub.entity = party->entity = g_strdup(entity);
    party->digits = qti_ims_conf_digits(entity);
    party->endpoints = g_hash_table_new_full(g_str_hash, g_str_equal,
        g_free, NULL);
    return party;
}

static
void
qti_ims_conf_party_eval(
    QtiImsConfPartyPriv* party)
{
    if (g_hash_table_size(party->endpoints)) {
        GHashTableIter it;
        gpointer key, value;

        party->had_endpoints = TRUE;
        party->pub.gone = TRUE;
        g_hash_table_iter_init(&it, party->endpoints);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            if (value == ENDPOINT_IN) {
                party->pub.gone = FALSE;
                break;
            }
        }
    } else {
        /* Listed without endpoints: gone only if it had some before */
        party->pub.gone = party->had_endpoints;
    }
    if (!party->pub.gone) {
        party->pub.seen = TRUE;
    }

    /* The user's URI may carry no number; an endpoint's may */
    if (strlen(party->digits) < MATCH_MIN_DIGITS) {
        GHashTableIter it;
        gpointer key;

        g_hash_table_iter_init(&it, party->endpoints);
        while (g_hash_table_iter_next(&it, &key, NULL)) {
            char* digits = qti_ims_conf_digits(key);

            if (strlen(digits) >= MATCH_MIN_DIGITS) {
                g_free(party->digits);
                party->digits = digits;
                break;
            }
            g_free(digits);
        }
    }
}

static
void
qti_ims_conf_apply_user(
    QtiImsConf* conf,
    const ConfDocUser* user,
    gboolean replace)
{
    QtiImsConfPartyPriv* party;

    if (!user->entity || !user->entity[0]) {
        return;
    }

    party = g_hash_table_lookup(conf->parties, user->entity);
    if (!party) {
        if (user->state == CONF_STATE_DELETED) {
            return;
        }
        party = qti_ims_conf_party_new(user->entity);
        g_hash_table_insert(conf->parties, party->entity, party);
    }
    party->listed = TRUE;

    if (user->state == CONF_STATE_DELETED) {
        g_hash_table_remove_all(party->endpoints);
        party->had_endpoints = TRUE;
        party->pub.gone = TRUE;
        return;
    }

    if (replace) {
        g_hash_table_remove_all(party->endpoints);
    }
    for (guint i = 0; i < user->endpoints->len; i++) {
        const ConfDocEndpoint* ep = g_ptr_array_index(user->endpoints, i);
        const char* key = ep->entity ? ep->entity : "";

        if (ep->state == CONF_STATE_DELETED) {
            g_hash_table_remove(party->endpoints, key);
        } else if (ep->status) {
            g_hash_table_insert(party->endpoints, g_strdup(key),
                qti_ims_conf_status_is_in(ep->status) ?
                ENDPOINT_IN : ENDPOINT_OUT);
        } else if (!g_hash_table_contains(party->endpoints, key)) {
            /* No status yet: count it as in */
            g_hash_table_insert(party->endpoints, g_strdup(key),
                ENDPOINT_IN);
        }
    }
    qti_ims_conf_party_eval(party);
}

/*==========================================================================*
 * API
 *==========================================================================*/

QtiImsConf*
qti_ims_conf_new(
    void)
{
    QtiImsConf* conf = g_new0(QtiImsConf, 1);

    conf->parties = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
        qti_ims_conf_party_free);
    return conf;
}

void
qti_ims_conf_free(
    QtiImsConf* conf)
{
    if (conf) {
        g_hash_table_destroy(conf->parties);
        g_free(conf->entity);
        g_free(conf);
    }
}

void
qti_ims_conf_clear(
    QtiImsConf* conf)
{
    g_hash_table_remove_all(conf->parties);
    g_free(conf->entity);
    conf->entity = NULL;
    conf->have_version = FALSE;
    conf->version = 0;
}

QTI_IMS_CONF_UPDATE_RESULT
qti_ims_conf_update(
    QtiImsConf* conf,
    const void* xml,
    gsize len,
    GError** error)
{
    ConfDoc* doc = qti_ims_conf_doc_parse(xml, len, error);
    GHashTableIter it;
    gpointer value;
    gboolean doc_full, users_full;

    if (!doc) {
        return QTI_IMS_CONF_UPDATE_INVALID;
    } else if (!doc->is_conf_info) {
        g_set_error_literal(error, G_MARKUP_ERROR,
            G_MARKUP_ERROR_UNKNOWN_ELEMENT, "no <conference-info>");
        qti_ims_conf_doc_free(doc);
        return QTI_IMS_CONF_UPDATE_INVALID;
    }

    /* Another conference's document: start over */
    if (doc->entity && doc->entity[0] && conf->entity &&
        strcmp(doc->entity, conf->entity)) {
        qti_ims_conf_clear(conf);
    }

    /*
     * Versions count per subscription and start again at 1 when the modem
     * subscribes again, so only a partial update has to be newer; a full
     * document replaces the state whatever its version.
     */
    if (doc->state == CONF_STATE_PARTIAL && doc->have_version &&
        conf->have_version && doc->version <= conf->version) {
        qti_ims_conf_doc_free(doc);
        return QTI_IMS_CONF_UPDATE_STALE;
    }

    if (doc->entity && doc->entity[0] && !conf->entity) {
        conf->entity = g_strdup(doc->entity);
    }
    if (doc->have_version) {
        conf->have_version = TRUE;
        conf->version = doc->version;
    }

    if (doc->state == CONF_STATE_DELETED) {
        /* The conference is gone */
        g_hash_table_iter_init(&it, conf->parties);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            ((QtiImsConfPartyPriv*)value)->pub.gone = TRUE;
        }
        qti_ims_conf_doc_free(doc);
        return QTI_IMS_CONF_UPDATE_APPLIED;
    }

    /*
     * RFC 4575 defaults a missing 'state' to "full". Only trust that in a
     * full document: in a partial one, reading a missing state as full
     * would make every unlisted party leave.
     */
    doc_full = (doc->state != CONF_STATE_PARTIAL);
    users_full = doc->have_users && (doc->users_state == CONF_STATE_FULL ||
        (doc->users_state == CONF_STATE_NONE && doc_full));

    g_hash_table_iter_init(&it, conf->parties);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        ((QtiImsConfPartyPriv*)value)->listed = FALSE;
    }

    for (guint i = 0; i < doc->users->len; i++) {
        const ConfDocUser* user = g_ptr_array_index(doc->users, i);
        const gboolean replace = (user->state == CONF_STATE_FULL ||
            (user->state == CONF_STATE_NONE && users_full));

        qti_ims_conf_apply_user(conf, user, replace);
    }

    if (users_full) {
        /* A full list: whoever is not on it has left */
        g_hash_table_iter_init(&it, conf->parties);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            QtiImsConfPartyPriv* party = value;

            if (!party->listed) {
                party->pub.gone = TRUE;
            }
        }
    }

    qti_ims_conf_doc_free(doc);
    return QTI_IMS_CONF_UPDATE_APPLIED;
}

const QtiImsConfParty*
qti_ims_conf_find(
    QtiImsConf* conf,
    const char* number)
{
    char* digits = qti_ims_conf_digits(number);
    QtiImsConfPartyPriv* found = NULL;
    QtiImsConfPartyPriv* found_in = NULL;
    guint matches = 0, matches_in = 0;
    GHashTableIter it;
    gpointer value;

    g_hash_table_iter_init(&it, conf->parties);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        QtiImsConfPartyPriv* party = value;

        if (qti_ims_conf_digits_match(digits, party->digits)) {
            found = party;
            matches++;
            if (!party->pub.gone) {
                found_in = party;
                matches_in++;
            }
        }
    }
    g_free(digits);

    /* More than one: prefer the one that is still in the conference */
    if (matches == 1) {
        return &found->pub;
    } else if (matches_in == 1) {
        return &found_in->pub;
    } else {
        return NULL;
    }
}

void
qti_ims_conf_rejoin(
    QtiImsConf* conf,
    const char* number)
{
    char* digits = qti_ims_conf_digits(number);
    GHashTableIter it;
    gpointer value;

    g_hash_table_iter_init(&it, conf->parties);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        QtiImsConfPartyPriv* party = value;

        if (party->pub.gone &&
            qti_ims_conf_digits_match(digits, party->digits)) {
            party->pub.seen = FALSE;
        }
    }
    g_free(digits);
}

char*
qti_ims_conf_describe(
    QtiImsConf* conf)
{
    GString* out = g_string_new(NULL);
    GHashTableIter it;
    gpointer value;

    if (conf->have_version) {
        g_string_append_printf(out, "version %u, ", conf->version);
    }
    g_string_append_printf(out, "%u part%s", g_hash_table_size(conf->parties),
        g_hash_table_size(conf->parties) == 1 ? "y" : "ies");

    g_hash_table_iter_init(&it, conf->parties);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        QtiImsConfPartyPriv* party = value;
        char* masked = qti_ims_conf_mask(party->entity);

        g_string_append_printf(out, "; %s %s%s (%u endpoint%s)", masked,
            party->pub.gone ? "gone" : "in",
            party->pub.seen ? "" : ", never in",
            g_hash_table_size(party->endpoints),
            g_hash_table_size(party->endpoints) == 1 ? "" : "s");
        g_free(masked);
    }
    return g_string_free(out, FALSE);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
