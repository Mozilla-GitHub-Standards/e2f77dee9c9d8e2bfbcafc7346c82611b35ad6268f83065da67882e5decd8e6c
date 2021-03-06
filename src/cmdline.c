/*
 * Copyright (C) 2014  Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "ptr.h"
#include "base64.h"
#include "llcp.h"
#include "ndef.h"
#include "nfc-re.h"
#include "nfc.h"
#include "nfc-nci.h"
#include "nfc-tag.h"
#include "snep.h"
#include "cb.h"

struct nfc_ndef_record_param {
    unsigned long flags;
    enum ndef_tnf tnf;
    const char* type;
    const char* id;
    const char* payload;
};

#define NFC_NDEF_PARAM_RECORD_INIT(_rec) \
    _rec = { \
        .flags = 0, \
        .tnf = 0, \
        .type = NULL, \
        .id = NULL, \
        .payload = NULL \
    }

ssize_t
build_ndef_msg(const struct nfc_ndef_record_param* record, size_t nrecords,
               uint8_t* buf, size_t len)
{
    size_t off;
    size_t i;

    assert(record || !nrecords);
    assert(buf || !len);

    off = 0;

    for (i = 0; i < nrecords; ++i, ++record) {
        size_t idlen;
        uint8_t flags;
        struct ndef_rec* ndef;
        ssize_t res;

        idlen = strlen(record->id);

        flags = record->flags |
                ( NDEF_FLAG_MB * (!i) ) |
                ( NDEF_FLAG_ME * (i+1 == nrecords) ) |
                ( NDEF_FLAG_IL * (!!idlen) );

        ndef = (struct ndef_rec*)(buf + off);
        off += ndef_create_rec(ndef, flags, record->tnf, 0, 0, 0);

        /* decode type */
        res = decode_base64(record->type, strlen(record->type),
                            buf+off, len-off);
        if (res < 0) {
            return -1;
        }
        ndef_rec_set_type_len(ndef, res);
        off += res;

        if (flags & NDEF_FLAG_IL) {
            /* decode id */
            res = decode_base64(record->id, strlen(record->id),
                                buf+off, len-off);
            if (res < 0) {
                return -1;
            }
            ndef_rec_set_id_len(ndef, res);
            off += res;
        }

        /* decode payload */
        res = decode_base64(record->payload, strlen(record->payload),
                            buf+off, len-off);
        if (res < 0) {
            return -1;
        } else if ((res > 255) && (flags & NDEF_FLAG_SR)) {
            cb.log_err("KO: NDEF flag SR set for long payload of %zu bytes",
                       res);
            return -1;
        }
        ndef_rec_set_payload_len(ndef, res);
        off += res;
    }
    return off;
}

struct nfc_snep_param {
    long dsap;
    long ssap;
    size_t nrecords;
    struct nfc_ndef_record_param record[4];
};

#define NFC_SNEP_PARAM_INIT() \
    { \
        .dsap = LLCP_SAP_LM, \
        .ssap = LLCP_SAP_LM, \
        .nrecords = 0, \
        .record = { \
            NFC_NDEF_PARAM_RECORD_INIT([0]), \
            NFC_NDEF_PARAM_RECORD_INIT([1]), \
            NFC_NDEF_PARAM_RECORD_INIT([2]), \
            NFC_NDEF_PARAM_RECORD_INIT([3]) \
        } \
    }

static ssize_t
create_snep_cp(void *data, size_t len, struct snep* snep)
{
    const struct nfc_snep_param* param;
    ssize_t res;

    param = data;
    assert(param);

    res = build_ndef_msg(param->record, param->nrecords,
                         snep->info, len-sizeof(*snep));
    if (res < 0) {
        return -1;
    }
    return snep_create_req_put(snep, res);
}

static ssize_t
nfc_send_snep_put_cb(void* data,
                     struct nfc_device* nfc,
                     size_t maxlen, union nci_packet* ntf)
{
    struct nfc_snep_param* param;
    ssize_t res;

    param = data;
    assert(param);

    if (!nfc->active_re) {
        cb.log_err("KO: no active remote endpoint\n");
        return -1;
    }
    if ((param->dsap < 0) && (param->ssap < 0)) {
        param->dsap = nfc->active_re->last_dsap;
        param->ssap = nfc->active_re->last_ssap;
    }
    res = nfc_re_send_snep_put(nfc->active_re, param->dsap, param->ssap,
                               create_snep_cp, data);
    if (res < 0) {
        cb.log_err("KO: 'snep put' failed\r\n");
        return -1;
    }
    return res;
}

static ssize_t
nfc_recv_process_ndef_cb(void* data, size_t len, const struct ndef_rec* ndef)
{
    const struct nfc_snep_param* param;
    ssize_t remain;
    char base64[3][512];

    param = data;
    assert(param);

    remain = len;

    cb.log_msg("[");

    while (remain) {
        size_t tlen, plen, ilen, reclen;

        if (remain < (ssize_t)sizeof(*ndef)) {
            return -1; /* too short */
        }
        tlen = encode_base64(ndef_rec_const_type(ndef),
                             ndef_rec_type_len(ndef),
                             base64[0], sizeof(base64[0]));
        ilen = encode_base64(ndef_rec_const_id(ndef), ndef_rec_id_len(ndef),
                             base64[1], sizeof(base64[1]));
        plen = encode_base64(ndef_rec_const_payload(ndef),
                             ndef_rec_payload_len(ndef),
                             base64[2], sizeof(base64[2]));

        /* print NDEF message in JSON format */
        cb.log_msg("{\"tnf\": %d,"
                   " \"type\": \"%.*s\","
                   " \"id\": \"%.*s\","
                   " \"payload\": \"%.*s\"}",
                   ndef->flags & NDEF_TNF_BITS,
                   tlen, base64[0], ilen, base64[1], plen, base64[2]);

        /* advance record */
        reclen = ndef_rec_len(ndef);
        remain -= reclen;
        ndef = (const struct ndef_rec*)(((const unsigned char*)ndef) + reclen);
        if (remain) {
          cb.log_msg(","); /* more to come */
        }
    }
    cb.log_msg("]\r\n");
    return 0;
}

static ssize_t
nfc_recv_snep_put_cb(void* data,  struct nfc_device* nfc)
{
    struct nfc_snep_param* param;
    ssize_t res;

    param = data;
    assert(param);

    if (!nfc->active_re) {
        cb.log_err("KO: no active remote endpoint\r\n");
        return -1;
    }
    if ((param->dsap < 0) && (param->ssap < 0)) {
        param->dsap = nfc->active_re->last_dsap;
        param->ssap = nfc->active_re->last_ssap;
    }
    res = nfc_re_recv_snep_put(nfc->active_re, param->dsap, param->ssap,
                               nfc_recv_process_ndef_cb, data);
    if (res < 0) {
        cb.log_err("KO: 'snep put' failed\r\n");
        return -1;
    }
    return 0;
}

static const char*
lex_token(const char* field, const char* delim, char** args)
{
    const char *tok;

    assert(args);

    tok = strsep(args, delim);
    if (!tok) {
        cb.log_err("KO: no token %s given\r\n", field);
        return NULL;
    }
    return tok;
}

static int
parse_token_l(const char* field, const char* delim, char** args, long* val)
{
    const char* tok;

    assert(val);

    tok = lex_token(field, delim, args);
    if (!tok) {
        return -1;
    }
    errno = 0;
    *val = strtol(tok, NULL, 0);
    if (errno) {
        cb.log_err("KO: invalid value '%s' for token %s, error %d(%s)\r\n",
                   tok, field, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int
parse_token_ul(const char* field, const char* delim,
               char** args, unsigned long* val)
{
    const char* tok;

    assert(val);

    tok = lex_token(field, delim, args);
    if (!tok) {
        return -1;
    }
    errno = 0;
    *val = strtoul(tok, NULL, 0);
    if (errno) {
        cb.log_err("KO: invalid value '%s' for token %s, error %d(%s)\r\n",
                   tok, field, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int
parse_token_s(const char* field, const char* delim,
              char** args, const char** val, int allow_empty)
{
    // TODO: we could add support for escaped characters, if necessary

    assert(val);

    *val = lex_token(field, delim, args);
    if (!*val) {
        return -1;
    }
    if (!allow_empty && !(*val)[0]) {
        cb.log_err("KO: empty token %s\r\n", field);
        return -1;
    }
    return 0;
}

static int
parse_sap(const char* field, char** args, long* sap, int can_autodetect)
{
    assert(args);
    assert(sap);

    if (parse_token_l(field, " ", args, sap) < 0) {
        return -1;
    }
    if (((*sap == -1) && !can_autodetect) ||
         (*sap < -1) || !(*sap < LLCP_NUMBER_OF_SAPS)) {
        cb.log_err("KO: invalid %s '%ld'\r\n",
                      field, *sap);
        return -1;
    }
    return 0;
}

/* Each record is given by its flag bits, TNF value, type,
 * payload, and id. Id is optional. Type, payload, and id
 * are given in base64url encoding.
 */
static int
parse_ndef_rec(char** args, struct nfc_ndef_record_param* record)
{
    const char* p;
    unsigned long tnf;

    assert(args);
    assert(record);

    /* read opening bracket */
    p = strsep(args, "[");
    if (!p) {
        cb.log_err("KO: no NDEF record given\r\n");
        return -1;
    }
    /* read flags */
    if (parse_token_ul("NDEF flags", " ,", args, &record->flags) < 0) {
        return -1;
    }
    if (record->flags & ~NDEF_FLAG_BITS) {
        cb.log_err("KO: invalid NDEF flags '%u'\r\n",
                      record->flags);
        return -1;
    }
    /* read TNF */
    if (parse_token_ul("NDEF TNF", " ,", args, &tnf) < 0) {
        return -1;
    }
    if (!(tnf < NDEF_NUMBER_OF_TNFS)) {
        cb.log_err("KO: invalid NDEF TNF '%u'\r\n",
                   record->tnf);
        return -1;
    }
    record->tnf = tnf;
    /* read type */
    if (parse_token_s("NDEF type", " ,", args, &record->type, 0) < 0) {
        return -1;
    }
    /* read id; might by empty */
    if (parse_token_s("NDEF id", " ,", args, &record->id, 1) < 0) {
        return -1;
    }
    /* read payload */
    if (parse_token_s("NDEF payload", "]", args, &record->payload, 0) < 0) {
        return -1;
    }
    return 0;
}

static ssize_t
parse_ndef_msg(char** args, size_t nrecs, struct nfc_ndef_record_param* rec)
{
    size_t i;

    assert(args);

    for (i = 0; i < nrecs && *args && strlen(*args); ++i) {
        if (parse_ndef_rec(args, rec+i) < 0) {
          return -1;
        }
    }
    if (*args && strlen(*args)) {
        cb.log_err("KO: invalid characters near EOL: %s\r\n",
                   *args);
        return -1;
    }
    return i;
}

static int
parse_re_index(char** args, unsigned long nres, unsigned long* i)
{
    assert(i);

    if (parse_token_ul("remote endpoint", " ", args, i) < 0) {
        return -1;
    }
    if (!(*i < nres)) {
        cb.log_err("KO: unknown remote endpoint %lu\r\n", *i);
        return -1;
    }
    return 0;
}

static int
parse_nci_ntf_type(char** args, unsigned long* ntype)
{
    assert(ntype);

    if (parse_token_ul("discover notification type", " ", args, ntype) < 0) {
        return -1;
    }
    if (!(*ntype < NUMBER_OF_NCI_NOTIFICATION_TYPES)) {
        cb.log_err("KO: unknown discover notification type %lu\r\n", *ntype);
        return -1;
    }
    return 0;
}

static int
parse_rf_index(char** args, long* rf)
{
    assert(rf);

    if (parse_token_l("rf index", " ", args, rf) < 0) {
        return -1;
    }
    if (*rf < -1 || *rf >= NUMBER_OF_SUPPORTED_NCI_RF_INTERFACES) {
        cb.log_err("KO: unknown rf index %lu\r\n", *rf);
        return -1;
    }
    return 0;
}

static int
parse_nci_deactivate_ntf_type(char** args, unsigned long* dtype)
{
    assert(dtype);

    if (parse_token_ul("deactivate notification type", " ", args, dtype) < 0) {
        return -1;
    }
    if (!(*dtype < NUMBER_OF_NCI_RF_DEACT_TYPE)) {
        cb.log_err("KO: unknown deactivate notification type %lu\r\n", *dtype);
        return -1;
    }
    return 0;
}

static int
parse_nci_deactivate_ntf_reason(char** args, unsigned long* dreason)
{
    assert(dreason);

    if (parse_token_ul("deactivate notification reason", " ", args, dreason) < 0) {
        return -1;
    }
    if (!(*dreason < NUMBER_OF_NCI_RF_DEACT_REASON)) {
        cb.log_err("KO: unknown deactivate notification reason %lu\r\n", *dreason);
        return -1;
    }
    return 0;
}

int
nfc_cmd_snep(char* args)
{
    char *p;

    if (!args) {
        cb.log_err("KO: no arguments given\r\n");
        return -1;
    }

    p = strsep(&args, " ");
    if (!p) {
        cb.log_err("KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "put")) {
        ssize_t nrecords;
        struct nfc_snep_param param = NFC_SNEP_PARAM_INIT();

        /* read DSAP */
        if (parse_sap("DSAP", &args, &param.dsap, 1) < 0) {
            return -1;
        }
        /* read SSAP */
        if (parse_sap("SSAP", &args, &param.ssap, 1) < 0) {
            return -1;
        }
        /* The emulator supports up to 4 records per NDEF
         * message. If no records are given, the emulator
         * will print the current content of the LLCP data-
         * link buffer.
         */
        nrecords = parse_ndef_msg(&args, ARRAY_SIZE(param.record),
                                  param.record);
        if (nrecords < 0) {
            return -1;
        }
        param.nrecords = nrecords;
        if (param.nrecords) {
            /* put SNEP request onto SNEP server */
            if (cb.send_dta(nfc_send_snep_put_cb, &param) < 0) {
                /* error message generated in create function */
                return -1;
            }
        } else {
            /* put SNEP request onto SNEP server */
            if (cb.recv_dta(nfc_recv_snep_put_cb, &param) < 0) {
                /* error message generated in create function */
                return -1;
            }
        }
    } else {
        cb.log_err("KO: invalid operation '%s'\r\n", p);
        return -1;
    }

    return 0;
}

struct nfc_ntf_param {
    struct nfc_re* re;
    unsigned long ntype;
    long rf;
    unsigned long dreason;
    unsigned long dtype;
};

#define NFC_NTF_PARAM_INIT() \
    { \
      .re = NULL, \
      .ntype = 0, \
      .rf = -1, \
      .dreason = 0, \
      .dtype = 0 \
    }

static ssize_t
nfc_rf_discovery_ntf_cb(void* data,
                        struct nfc_device* nfc, size_t maxlen,
                        union nci_packet* ntf)
{
    ssize_t res;
    const struct nfc_ntf_param* param = data;
    res = nfc_create_rf_discovery_ntf(param->re, param->ntype, nfc, ntf);
    if (res < 0) {
        cb.log_err("KO: rf_discover_ntf failed\r\n");
        return -1;
    }
    return res;
}

static ssize_t
nfc_rf_intf_activated_ntf_cb(void* data,
                             struct nfc_device* nfc, size_t maxlen,
                             union nci_packet* ntf)
{
    ssize_t res;
    struct nfc_ntf_param* param = data;
    if (!param->re) {
        if (!nfc->active_re) {
            cb.log_err("KO: no active remote-endpoint\n");
            return -1;
        }
        param->re = nfc->active_re;
    }
    nfc_clear_re(param->re);
    if (nfc->active_rf) {
        // Already select an active rf interface,so do nothing.
    } else if (param->rf == -1) {
        // Auto select active rf interface based on remote-endpoint protocol and mode.
        nfc->active_rf = nfc_find_rf_by_protocol_and_mode(nfc,
                                                          param->re->rfproto,
                                                          param->re->mode);
        if (!nfc->active_rf) {
            cb.log_err("KO: no active rf interface\r\n");
            return -1;
        }
    } else {
        nfc->active_rf = nfc->rf + param->rf;
    }

    res = nfc_create_rf_intf_activated_ntf(param->re, nfc, ntf);
    if (res < 0) {
        cb.log_err("KO: rf_intf_activated_ntf failed\r\n");
        return -1;
    }
    return res;
}

static ssize_t
nfc_rf_intf_deactivate_ntf_cb(void* data,
                              struct nfc_device* nfc, size_t maxlen,
                              union nci_packet* ntf)
{
    ssize_t res;
    struct nfc_ntf_param* param = data;

    assert(data);
    assert(nfc);

    res = nfc_create_deactivate_ntf(param->dtype, param->dreason, ntf);
    if (res < 0) {
        cb.log_err("KO: rf_intf_deactivate_ntf failed\r\n");
        return -1;
    }
    return res;
}

int
nfc_cmd_nci(char*  args)
{
    char *p;

    if (!args) {
        cb.log_err("KO: no arguments given\r\n");
        return -1;
    }

    /* read notification type */
    p = strsep(&args, " ");
    if (!p) {
        cb.log_err("KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "rf_discover_ntf")) {
        unsigned long i;
        struct nfc_ntf_param param = NFC_NTF_PARAM_INIT();
        /* read remote-endpoint index */
        if (parse_re_index(&args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        param.re = nfc_res + i;

        /* read discover notification type */
        if (parse_nci_ntf_type(&args, &param.ntype) < 0) {
            return -1;
        }

        /* generate RF_DISCOVER_NTF */
        if (cb.send_ntf(nfc_rf_discovery_ntf_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else if (!strcmp(p, "rf_intf_activated_ntf")) {
        struct nfc_ntf_param param = NFC_NTF_PARAM_INIT();
        if (args && *args) {
            unsigned long i;
            /* read remote-endpoint index */
            if (parse_re_index(&args, ARRAY_SIZE(nfc_res), &i) < 0) {
                return -1;
            }
            param.re = nfc_res + i;

            if (args && *args) {
                /* read rf interface index */
                if (parse_rf_index(&args, &param.rf) < 0) {
                    return -1;
                }
            } else {
                param.rf = -1;
            }
        } else {
            param.re = NULL;
            param.rf = -1;
        }
        /* generate RF_INTF_ACTIVATED_NTF; if param.re == NULL,
         * active RE will be used */
        if (cb.send_ntf(nfc_rf_intf_activated_ntf_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else if (!strcmp(p, "rf_intf_deactivate_ntf")) {
        struct nfc_ntf_param param = NFC_NTF_PARAM_INIT();
        if (args && *args) {
            /* read deactivate ntf type */
            if (parse_nci_deactivate_ntf_type(&args, &param.dtype) < 0) {
                return -1;
            }
            /* read deactivate ntf reason */
            if (parse_nci_deactivate_ntf_reason(&args, &param.dreason) < 0) {
                return -1;
            }
        } else {
            param.dtype = NCI_RF_DEACT_DISCOVERY;
            param.dreason = NCI_RF_DEACT_RF_LINK_LOSS;
        }
        if (cb.send_ntf(nfc_rf_intf_deactivate_ntf_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else {
        cb.log_err("KO: invalid operation '%s'\r\n", p);
        return -1;
    }

    return 0;
}

struct nfc_llcp_param {
    long dsap;
    long ssap;
};

#define NFC_LLCP_PARAM_INIT() \
    { \
        .dsap = 0, \
        .ssap = 0 \
    }

static ssize_t
nfc_llcp_connect_cb(void* data, struct nfc_device* nfc, size_t maxlen,
                    union nci_packet* packet)
{
    struct nfc_llcp_param* param = data;
    ssize_t res;

    if (!nfc->active_re) {
        cb.log_err("KO: no active remote endpoint\n");
        return -1;
    }
    if ((param->dsap < 0) && (param->ssap < 0)) {
        param->dsap = nfc->active_re->last_dsap;
        param->ssap = nfc->active_re->last_ssap;
    }
    if (!param->dsap) {
        cb.log_err("KO: DSAP is 0\r\n");
        return -1;
    }
    if (!param->ssap) {
        cb.log_err("KO: SSAP is 0\r\n");
        return -1;
    }
    res = nfc_re_send_llcp_connect(nfc->active_re, param->dsap, param->ssap);
    if (res < 0) {
        cb.log_err("KO: LLCP connect failed\r\n");
        return -1;
    }
    return 0;
}

int
nfc_cmd_llcp(char* args)
{
    char *p;

    if (!args) {
        cb.log_err("KO: no arguments given\r\n");
        return -1;
    }

    p = strsep(&args, " ");
    if (!p) {
        cb.log_err("KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "connect")) {
        struct nfc_llcp_param param = NFC_LLCP_PARAM_INIT();

        /* read DSAP */
        if (parse_sap("DSAP", &args, &param.dsap, 1) < 0) {
            return -1;
        }
        /* read SSAP */
        if (parse_sap("SSAP", &args, &param.ssap, 1) < 0) {
            return -1;
        }
        if (cb.send_dta(nfc_llcp_connect_cb, &param) < 0) {
            /* error message generated in create function */
            return -1;
        }
    } else {
        cb.log_err("KO: invalid operation '%s'\r\n", p);
        return -1;
    }

    return 0;
}

int
nfc_cmd_tag(char* args)
{
    char *p;

    if (!args) {
        cb.log_err("KO: no arguments given\r\n");
        return -1;
    }

    p = strsep(&args, " ");
    if (!p) {
        cb.log_err("KO: no operation given\r\n");
        return -1;
    }
    if (!strcmp(p, "set")) {
        unsigned long i;
        ssize_t res;
        ssize_t nrecords;
        struct nfc_ndef_record_param record[4];
        struct nfc_re* re;
        uint8_t buf[MAXIMUM_SUPPORTED_TAG_SIZE];

        /* read remote-endpoint index */
        if (parse_re_index(&args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        re = nfc_res + i;

        if (!re->tag) {
            cb.log_err("KO: remote endpoint is not a tag\r\n");
            return -1;
        }

        nrecords = parse_ndef_msg(&args, ARRAY_SIZE(record), record);
        if (nrecords < 0) {
            return -1;
        }

        res = build_ndef_msg(record, nrecords, buf, ARRAY_SIZE(buf));
        if (res < 0) {
            return -1;
        }

        if (nfc_tag_set_data(re->tag, buf, res) < 0) {
            return -1;
        }
    } else if (!strcmp(p, "clear")) {
        unsigned long i;
        struct nfc_re* re;

        /* read remote-endpoint index */
        if (parse_re_index(&args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        re = nfc_res + i;

        if (nfc_tag_set_data(re->tag, NULL, 0) < 0) {
            return -1;
        }
    } else if (!strcmp(p, "format")) {
        unsigned long i;
        struct nfc_re* re;

        /* read remote-endpoint index */
        if (parse_re_index(&args, ARRAY_SIZE(nfc_res), &i) < 0) {
            return -1;
        }
        re = nfc_res + i;

        if (nfc_tag_format(re->tag) < 0) {
            return -1;
        }
    }

    return 0;
}
