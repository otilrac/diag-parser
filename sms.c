#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <osmocom/gsm/rsl.h>
#include <osmocom/gsm/tlv.h>
#include <osmocom/gsm/gsm48.h>
#include <osmocom/gsm/gsm48_ie.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/protocol/gsm_04_11.h>
#include <assert.h>

#include "sms.h"
#include "address.h"
#include "session.h"
#include "bit_func.h"

#define DCS_COMPRESSED 0x80

#define APPEND_INFO(sm, ...) snprintf((sm)->info+strlen((sm)->info), sizeof((sm)->info)-strlen((sm)->info), ##__VA_ARGS__);

struct sec_header {
	uint16_t cpl;
	uint8_t chl;
	uint8_t spi1;
	uint8_t spi2;
	uint8_t kic;
	uint8_t kid;
	uint8_t tar[3];
	uint8_t cntr[5];
	uint8_t pcntr;
};

struct sec_header_rp {
	uint16_t rpl;
	uint8_t rhl;
	uint8_t tar[3];
	uint8_t cntr[5];
	uint8_t pcntr;
	uint8_t status;
	uint8_t sign[8];
	uint8_t sw[0];
};

enum sms_class {
	CLASS_DISPLAY = 0,
	CLASS_ME = 1,
	CLASS_SIM = 2,
	CLASS_TE = 3,
	CLASS_NONE = 4
};

enum sms_class get_sms_class(uint8_t dcs)
{
	uint8_t coding_group = dcs >> 4;
	enum sms_class class = CLASS_NONE;

	if ((coding_group & 0xc) == 0) {
		if (dcs & 0x10) {
			class = (dcs & 0x03);
		}
	} else if (coding_group == 0xf) {
		class = (dcs & 0x03);
	}

	return class;
}

enum sms_alphabet get_sms_alphabet(uint8_t dcs)
{
	uint8_t coding_group = dcs >> 4;
	enum sms_alphabet alpha = DCS_NONE;

	if (dcs == 0x00) {
		/* default alphabet, 7bit */
		alpha = DCS_7BIT_DEFAULT;
		return alpha;
	}

	if ((coding_group & 0xc) == 0) {
		/* DCS 00xx xxxx */

		switch ((dcs >> 2) & 0x03) {
		case 0:
			alpha = DCS_7BIT_DEFAULT;
			break;
		case 1:
			alpha = DCS_8BIT_DATA;
			break;
		case 2:
			alpha = DCS_UCS2;
			break;
		}

		if (dcs & 0x20) {
			alpha |= DCS_COMPRESSED;
		}
	} else if (coding_group == 0xc || coding_group == 0xd)
		alpha = DCS_7BIT_DEFAULT;
	else if (coding_group == 0xe)
		alpha = DCS_UCS2;
	else if (coding_group == 0xf) {
		/* DCS 1111 xxxx */
		if (dcs & 0x04) {
			alpha = DCS_8BIT_DATA;
		} else {
			alpha = DCS_7BIT_DEFAULT;
		}
	}

	return alpha;
}

void handle_text(struct sms_meta *sm, uint8_t *msg, unsigned len)
{
	uint8_t text[256];

	if (len == 0) {
		APPEND_INFO(sm, "<NO DATA>");
		return;
	}

	if (sm->alphabet & DCS_COMPRESSED) {
		APPEND_INFO(sm, "<COMPRESSED DATA>");
		return;
	}

	switch (sm->alphabet) {
	case DCS_7BIT_DEFAULT:
		gsm_7bit_decode_n(text, sizeof(text), msg, len);
		if (strlen(text)) {
			//FIXME: sanitize string!
			//APPEND_INFO(sm, "%s", text);
			APPEND_INFO(sm, "TEXT_7BIT");
		} else {
			APPEND_INFO(sm, "<FAILED TO DECODE TEXT>");
		}
		break;
	case DCS_UCS2:
		APPEND_INFO(sm, "TEXT_16BIT");
		break;
	case DCS_NONE:
	case DCS_8BIT_DATA:
		if (sm->pid == 124 ||
		    sm->pid == 127 ||
		    sm->dcs == 246 ||
		    sm->dcs == 22) {
			APPEND_INFO(sm, "OTA ");
			sm->ota = 1;
		}
		APPEND_INFO(sm, "DATA_8BIT");
		break;
	default:
		APPEND_INFO(sm, "<UNKNOWN ALPHABET>");
	}
}

void handle_sec_cp(struct sms_meta *sm, uint8_t *msg, unsigned len)
{
	struct sec_header *sh = (struct sec_header *) msg;

	assert(sm != NULL);
	assert(msg != NULL);

	switch ((sh->spi1 >> 3) & 0x03) {
	case 0:
		APPEND_INFO(sm, "NO_CNTR ");
		break;
	case 1:
		APPEND_INFO(sm, "CNTR_AV ");
		break;
	case 2:
		APPEND_INFO(sm, "CNTR_HI ");
		break;
	case 3:
		APPEND_INFO(sm, "CNTR_+1 ");
		break;

	}

	if (sh->spi1 & 0x04) {
		APPEND_INFO(sm, "ENC ");
		switch (sh->kic & 0x03) {
		case 0:
			APPEND_INFO(sm, "IMPLICIT ");
			break;
		case 1:
			switch((sh->kic>>2) & 0x03) {
			case 0:
				APPEND_INFO(sm, "1DES-CBC ");
				break;
			case 1:
				APPEND_INFO(sm, "3DES-2K ");
				break;
			case 2:
				APPEND_INFO(sm, "3DES-3K ");
				break;
			case 3:
				APPEND_INFO(sm, "1DES-EBC ");
				break;
			}
			break;
		case 2:
			APPEND_INFO(sm, "RESERVED ");
			break;
		case 3:
			APPEND_INFO(sm, "PROPRIET ");
			break;
		}
	} else {
		APPEND_INFO(sm, "NOENC ");
	}

	switch (sh->spi1 & 0x03) {
	case 0:
		APPEND_INFO(sm, "NOCC ");
		break;
	case 1:
		APPEND_INFO(sm, "RC ");
		break;
	case 2:
		APPEND_INFO(sm, "CC ");
		break;
	case 3:
		APPEND_INFO(sm, "DS ");
		break;
	}

	if (sh->spi1 & 0x03) {
		switch (sh->kid & 0x03) {
		case 0:
			APPEND_INFO(sm, "IMPLICIT ");
			break;
		case 1:
			switch((sh->kid>>2) & 0x03) {
			case 0:
				APPEND_INFO(sm, "1DES-CBC ");
				break;
			case 1:
				APPEND_INFO(sm, "3DES-2K ");
				break;
			case 2:
				APPEND_INFO(sm, "3DES-3K ");
				break;
			case 3:
				APPEND_INFO(sm, "RESERVED ");
				break;
			}
			break;
		case 2:
			APPEND_INFO(sm, "RESERVED ");
			break;
		case 3:
			APPEND_INFO(sm, "PROPRIET ");
			break;
		}
	}

	APPEND_INFO(sm, "TAR %02X%02X%02X ", sh->tar[0], sh->tar[1], sh->tar[2]);

	if ((sh->spi1 & 0x04 == 0)) {
		APPEND_INFO(sm, "CNTR %02X%02X%02X%02X%02X ",
				sh->cntr[0], sh->cntr[1],
				sh->cntr[2], sh->cntr[3],
				sh->cntr[4]);
	}
}

void handle_sec_rp(struct sms_meta *sm, uint8_t *msg, unsigned len)
{
	struct sec_header_rp *rp = (struct sec_header_rp *) msg;
	uint8_t sign_len;

	APPEND_INFO(sm, "TAR %02X%02X%02X ",
		rp->tar[0], rp->tar[1], rp->tar[2]);

	APPEND_INFO(sm, "POR %02X ", rp->status);

	if (len > 13) {
		sign_len = (len-13 > 16 ? 16 : len-13);
		APPEND_INFO(sm, "CC %s ", osmo_hexdump_nospc(rp->sign, sign_len));
	} else {
		APPEND_INFO(sm, "CC -- ");
	}
}

void handle_udh(struct sms_meta *sm, uint8_t *msg, unsigned len)
{
	uint8_t header_len;
	uint8_t *user_data;
	unsigned user_data_len;
	uint8_t offset = 1;
	uint8_t ota_cmd = 0;
	uint16_t ref;
	uint8_t total_frags;
	uint8_t this_frag;
	char alt_dest[32];

	assert(sm != NULL);
	assert(msg != NULL);

	if (len == 0) {
		APPEND_INFO(sm, "NO DATA");	
		return;
	}
	header_len = msg[0];

	/* Sanity check */
	if (header_len > (len-1)) {
		APPEND_INFO(sm, "SANITY CHECK FAILED (SMS_UDH_LEN)");
		return;
	}

	/* Data offset */
	user_data = msg + header_len + 1;
	user_data_len = len - header_len - 1;

	/* Parse header elements (TLV) */
	while (offset <= header_len) {
		uint8_t type = msg[offset++];
		uint8_t vlen = msg[offset++];

		if (offset+vlen > len) {
			APPEND_INFO(sm, "SANITY CHECK FAILED (UDH_IEI_LEN)");
			return;
		}

		switch (type) {
		case 0x00:
			/* Concatenated header, 8bit reference */
			assert(vlen == 3);
			ref = msg[offset+0];
			total_frags = msg[offset+1];
			this_frag = msg[offset+2];
			if (this_frag > total_frags) {
				APPEND_INFO(sm, "SANITY CHECK FAILED (SMS_FRAG_8)");
				return;
			}
			APPEND_INFO(sm, "[%d/%d] ", this_frag, total_frags);	
			sm->concat = 1;
			break;
		case 0x01:
			/* Special SMS indication */
			assert(vlen == 2);
			break;
		case 0x04:
			/* Application address port, 8bit */
			assert(vlen == 2);
			APPEND_INFO(sm, "PORT8 %d->%d ",
				 msg[offset+1],
				 msg[offset+0]);	
			break;
		case 0x05:
			/* Application address port, 16bit */
			assert(vlen == 4);
			APPEND_INFO(sm, "PORT16 %d->%d ",
				 msg[offset+0]<<8|msg[offset+1],
				 msg[offset+2]<<8|msg[offset+3]);	
			break;
		case 0x06:
			/* Service center control parameters */
			assert(vlen == 1);
			break;
		case 0x07:
			/* UDH source indicator */
			assert(vlen == 1);
			break;
		case 0x08:
			/* Concatenated header, 16bit reference */
			assert(vlen == 4);
			ref = msg[offset+0]<<8|msg[offset+1];
			total_frags = msg[offset+2];
			this_frag = msg[offset+3];
			if (this_frag > total_frags) {
				APPEND_INFO(sm, "SANITY CHECK FAILED (SMS_FRAG_16)");
				return;
			}
			sm->concat = 1;
			break;
		case 0x0a:
			/* Text formatting (EMS) */
			break;
		case 0x22:
			/* Alternate reply address */
			assert(vlen >= (msg[offset]/2 + 1));
			handle_address(&msg[offset+1], msg[offset], alt_dest, 1);
			APPEND_INFO(sm, "REPLY_ADDR=%s ", alt_dest);
			break;
		case 0x24:
			/* National language shift */
			assert(vlen == 1);
			APPEND_INFO(sm, "LANG_SHIFT=%d ", msg[offset]);
			break;
		case 0x70:
			/* OTA Command */
			sm->ota = 1;
			ota_cmd = 1;
			break;
		case 0x71:
			/* OTA Response */
			sm->ota = 1;
			ota_cmd = 0;
			break;
		case 0xda:
			/* SMSC-specific */
			assert(vlen <= header_len);
			printf("SMSC-specific %s\n", osmo_hexdump_nospc(&msg[offset], vlen));
			break;
		default:
			printf("Unhandled UDH-IEI 0x%02x, vlen=%d\n", type, vlen);
		}

		offset += len;
	}

	/* Parse message content */
	if (sm->ota) {
		APPEND_INFO(sm, "OTA ");
		if (ota_cmd) {
			handle_sec_cp(sm, user_data, user_data_len);
		} else {
			handle_sec_rp(sm, user_data, user_data_len);
		}
	} else {
		handle_text(sm, user_data, user_data_len);
	}
}

void handle_tpdu(struct session_info *s, uint8_t *msg, unsigned len, uint8_t from_network, char *smsc)
{
	uint8_t off;
	uint8_t f_len;
	uint8_t vp;
	struct sms_meta *sm;

	assert(s != NULL);
	assert(msg != NULL);
	assert(len > 2);
	assert(smsc != NULL);

	sm = (struct sms_meta *) malloc(sizeof(struct sms_meta));

	assert(sm != NULL);

	memset(sm, 0, sizeof(*sm));

	/* Store SMSC */
	if (smsc[0]) {
		strncpy(sm->smsc, smsc, sizeof(sm->smsc));
	} else {
		strncpy(sm->smsc, "<NO ADDRESS>", sizeof(sm->smsc));
	}

	/* UDH presence */
	sm->udhi = !!(msg[0] & 0x40);

	/* Validity period */
	vp = (msg[0] >> 3) & 0x03;

	/* Skip flags [+mr] */
	if (from_network) {
		off = 1;
	} else {
		off = 2;
	}

	/* Decode from/to address */
	f_len = msg[off++] + 1;
	handle_address(&msg[off], f_len, sm->msisdn, 1);
	if (from_network) {
		sm->from_network = 1;
		APPEND_MSG_INFO(s, ", FROM %s", sm->msisdn);
	} else {
		sm->from_network = 0;
		APPEND_MSG_INFO(s, ", TO %s", sm->msisdn);
	}
	off += f_len/2 + 1;

	/* TP-PID and TP-DCS */
	sm->pid = msg[off++];
	sm->dcs = msg[off++];

	/* Validity period */
	if (!from_network) {
		switch (vp) {
		case 0:
			break;
		case 2:
			off += 1;
			break;
		case 1:
		case 3:
			off += 7;
			break;
		}
	}

	/* Timestamp */
	if (from_network) {
		off += 7;
	}

	/* User data length */
	sm->length = msg[off++];

	/* Data length sanity check */
	if ((sm->dcs & 0xe0) != 0x20) {
		if (sm->length*7/8 > len - off) {
			printf("len %d off %d sm->len %d\n", len, off, sm->length);
			free(sm);
			return;
		}
	} else {
		//FIXME: estimate compressed length
	}

	/* Store unparsed bytes */
	memcpy(sm->data, &msg[off], len - off);

	sm->alphabet = get_sms_alphabet(sm->dcs);
	sm->class = get_sms_class(sm->dcs);

	/* Handle UDH if present */
	if (sm->udhi) {
		handle_udh(sm, &msg[off], sm->length);
	} else {
		handle_text(sm, &msg[off], sm->length);
	}

	//FIXME: discard normal sms, store only dcs = 192, 22, 246

	/* Append SMS to list */
	if (s->sms_list) {
		sm->sequence = s->sms_list->sequence+1;
	} else {
		sm->sequence = 0;
	}
	sm->next = s->sms_list;
	s->sms_list = sm;
}

void handle_rpdata(struct session_info *s, uint8_t *data, unsigned len, uint8_t from_network)
{
	uint8_t off = 0;
	uint8_t f_len;
	uint8_t type;
	uint8_t smsc[GSM48_MI_SIZE];

	assert(s != NULL);
	assert(data != NULL);
	assert(len > 0);

	/* originating (SMSC) address length */
	f_len = data[off++];
	if (f_len) {
		/* Sanity check */
		if (!from_network) {
			SET_MSG_INFO(s, " FAILED SANITY CHECK (SMS_SMSC_MO)");
			return;
		}
		handle_address(&data[off], f_len, smsc, 0);
	}
	off += f_len;

	/* destination (SMSC) address length */
	f_len = data[off++];
	if (f_len) {
		/* Sanity check */
		if (from_network) {
			SET_MSG_INFO(s, " FAILED SANITY CHECK (SMS_SMSC_MT)");
			return;
		}
		handle_address(&data[off], f_len, smsc, 0);
	}
	off += f_len;

	/* user data length */
	f_len = data[off++];

	/* MTI type */
	type = data[off] & 0x03;
	
	if (from_network) {
		switch (type) {
		case 0:
			/* SMS-DELIVER */
			APPEND_MSG_INFO(s, "-DELIVER");
			handle_tpdu(s, &data[off], f_len, from_network, smsc);
			break;
		case 1:
			/* SMS-SUBMIT REPORT */
			APPEND_MSG_INFO(s, "-SUBMIT-REPORT");
			break;
		case 2:
			/* SMS-STATUS REPORT */
			APPEND_MSG_INFO(s, "-STATUS-REPORT");
			break;
		case 3:
			/* RESERVED */
			APPEND_MSG_INFO(s, "-RESERVED");
			break;
		}
	} else {
		switch (type) {
		case 0:
			/* SMS-DELIVER REPORT */
			APPEND_MSG_INFO(s, "-DELIVER-REPORT");
			break;
		case 1:
			/* SMS-SUBMIT */
			APPEND_MSG_INFO(s, "-SUBMIT");
			handle_tpdu(s, &data[off], f_len, from_network, smsc);
			break;
		case 2:
			/* SMS-COMMAND */
			APPEND_MSG_INFO(s, "-COMMAND");
			break;
		case 3:
			/* RESERVED */
			APPEND_MSG_INFO(s, "-RESERVED");
			break;
		}
	}
}

void handle_cpdata(struct session_info *s, uint8_t *data, unsigned len)
{
	assert(len >= sizeof(struct gsm411_rp_hdr));
	struct gsm411_rp_hdr *rp = (struct gsm411_rp_hdr *) data;

	switch (rp->msg_type & 0x0f) {
	case GSM411_MT_RP_DATA_MO:
		SET_MSG_INFO(s, "SMS RP-DATA");
		handle_rpdata(s, rp->data, len-sizeof(struct gsm411_rp_hdr), 0);
		s->mo = 1;
		break;
	case GSM411_MT_RP_DATA_MT:
		SET_MSG_INFO(s, "SMS RP-DATA");
		handle_rpdata(s, rp->data, len-sizeof(struct gsm411_rp_hdr), 1);
		s->mt = 1;
		break;
	case GSM411_MT_RP_ACK_MO:
		SET_MSG_INFO(s, "SMS RP-ACK");
		s->mt = 1;
		break;
	case GSM411_MT_RP_ACK_MT:
		SET_MSG_INFO(s, "SMS RP-ACK");
		s->mo = 1;
		break;
	case GSM411_MT_RP_ERROR_MO:
		SET_MSG_INFO(s, "SMS RP-ERROR");
		s->mt = 1;
		break;
	case GSM411_MT_RP_ERROR_MT:
		SET_MSG_INFO(s, "SMS RP-ACK");
		s->mo = 1;
		break;
	case GSM411_MT_RP_SMMA_MO:
		SET_MSG_INFO(s, "SMS RP-SMMA");
		s->mo = 1;
		break;
	default:
		s->unknown = 1;
	}
}

void handle_sms(struct session_info *s, struct gsm48_hdr *dtap, unsigned len)
{
	s->sms = 1;
	s->sms_presence = 1;

	switch (dtap->msg_type & 0x1f) {
	case GSM411_MT_CP_DATA:
		handle_cpdata(s, dtap->data, len-sizeof(struct gsm48_hdr));
		break;
	case GSM411_MT_CP_ACK:
		SET_MSG_INFO(s, "SMS CP-ACK");
		break;
	case GSM411_MT_CP_ERROR:
		SET_MSG_INFO(s, "SMS CP-ERROR");
		break;
	default:
		s->unknown = 1;
	}
}

void sms_make_sql(int sid, struct sms_meta *sm, char *query, unsigned len)
{
	char *smsc;
	char *msisdn;
	char *info;
	char *data;
	char *data_hex;

	assert(sm != NULL);
	assert(query != NULL);

	smsc = strescape_or_null(sm->smsc);
	msisdn = strescape_or_null(sm->msisdn);
	info = strescape_or_null(sm->info);
	if (sm->length) { 
		data_hex = strescape_or_null(osmo_hexdump_nospc(sm->data,sm->length));
		data = malloc(strlen(data_hex)+2);
		snprintf(data, strlen(data_hex)+2, "X%s", data_hex);
		free(data_hex);
	} else {
		data = strdup("'<NO DATA>'"); 
	}

	snprintf(query, len, "INSERT INTO sms_meta (id,sequence,from_network,"
		"pid,dcs,alphabet,class,udhi,ota,concat,smsc,msisdn,info,length,data)"
		" VALUES (%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%d,%s);\n",
		 sid, sm->sequence, sm->from_network, sm->pid, sm->dcs, sm->alphabet,
		 sm->class, sm->udhi, sm->ota, sm->concat, smsc, msisdn, info, sm->length, data);

	free(smsc);
	free(msisdn);
	free(info);
	free(data);
}

