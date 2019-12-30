#define _GNU_SOURCE
#include <stdio.h>
#include <json-c/json.h>
#include <assert.h>

#include "l1.h" /* For metric and flag... FIXME */
#include "l3.h"
#include "wire.h"

#include "wa.h"
#include "bnode.h"
#include "session.h"
#include "chat.h"
#include "monitor.h"

#define DEBUG LOG_LEVEL_DEBUG
#include "log.h"
#include "utils.h"

int
l3_recv_bnode(wa_t *wa, bnode_t *bn);

int
l3_recv_message(wa_t *wa, bnode_t *bn, int last);

int
l3_recv_chat(wa_t *wa, bnode_t *bn)
{
	char *jid;
	int count;

	if(!bn->desc)
		return 1;

	if(strcmp(bn->desc, "chat") != 0)
		return 1;

	jid = strdup(bnode_attr_get(bn, "jid"));
	count = atoi(bnode_attr_get(bn, "count"));

	chat_update(wa, jid, count);

	return 0;
}
int
l3_recv_contact(wa_t *wa, bnode_t *bn)
{
	json_object *j;
	user_t *u;
	char *jid, *notify, *name;

	if(!bn->desc)
		return 1;

	if(strcmp(bn->desc, "user") != 0)
		return 1;

	if(!bn->attr)
		return 1;

	j = json_object_object_get(bn->attr, "jid");
	if(!j)
		return 1;

	jid = strdup(json_object_get_string(j));

	j = json_object_object_get(bn->attr, "short");
	if(!j)
		return 1;

	notify = strdup(json_object_get_string(j));

	j = json_object_object_get(bn->attr, "name");
	if(!j)
		return 1;

	name = strdup(json_object_get_string(j));

	u = malloc(sizeof(user_t));
	assert(u);
	u->name = name;
	u->notify = notify;
	u->jid = jid;

	/* FIXME: The user u is freed inside session_update_user */
	session_update_user(wa, u);

	return 0;
}


int
l3_recv_response_chat(wa_t *wa, bnode_t *bn)
{
	int i, ret=0;

	if(bn->type != BNODE_LIST)
		return -1;

	if(!bn->data.list)
		return -1;

	for(i=0; i<bn->len; i++)
	{
		ret |= l3_recv_chat(wa, bn->data.list[i]);
	}

	return ret;
}

int
l3_recv_contacts(wa_t *wa, bnode_t *bn)
{
	int i, ret=0;
	json_object *jval;
	const char *val;

	if(bn->type != BNODE_LIST)
		return -1;

	if(!bn->data.list)
		return -1;

	if(bn->attr)
	{
		jval = json_object_object_get(bn->attr, "type");
		if(jval)
		{
			val = json_object_get_string(jval);
			assert(val);

			if(strcmp(val, "frequent") == 0)
			{
				LOG_INFO("Ignoring frequent contact list\n");
				return 0;
			}
		}
	}


	for(i=0; i<bn->len; i++)
	{
		ret |= l3_recv_contact(wa, bn->data.list[i]);
	}

	LOG_WARN(" ----- CONTACTS RECEIVED! ----- \n");
	wa->state = WA_STATE_CONTACTS_RECEIVED;

	/* As we update the state, some msg can be dequeued */
	chat_flush(wa);

	/* TODO: By now we mark the ready state after we flush the chat list,
	 * but we may still be waiting on the last message part */
	wa->state = WA_STATE_READY;

	return ret;
}

int
l3_recv_response(wa_t *wa, bnode_t *bn)
{
	json_object *jtype;
	const char *type;

	if(!bn->attr)
		return 1;

	jtype = json_object_object_get(bn->attr, "type");

	if(!jtype)
		return 1;

	type = json_object_get_string(jtype);
	assert(type);

	if(strcmp(type, "contacts") == 0)
		return l3_recv_contacts(wa, bn);
	else if(strcmp(type, "chat") == 0)
		return l3_recv_response_chat(wa, bn);

	/* Unknown response msg */

	return 0;
}

int
l3_recv_action_child(wa_t *wa, bnode_t *bn, int last)
{
	if(strcmp("contacts", bn->desc) == 0)
		return l3_recv_contacts(wa, bn);
	else if(strcmp("message", bn->desc) == 0)
		return l3_recv_message(wa, bn, last);

	return -1;
}


int
l3_recv_action(wa_t *wa, bnode_t *bn)
{
	int i, ret = 0, last = 0;
	bnode_t *child;

	if(bn->type == BNODE_LIST)
	{
		for(i=0; i<bn->len; i++)
		{
			if(i == bn->len -1)
				last = 1;

			child = (bnode_t *) bn->data.list[i];
			ret |= l3_recv_action_child(wa, child, last);
		}
	}

	return ret;
}

int
l3_recv_message(wa_t *wa, bnode_t *bn, int last)
{
	dg_t *dg;

	//LOG_INFO("Received msg bnode:\n");
	//bnode_print(bn, 0);

	dg = dg_cmd(L3, L4, "recv_message");
	dg_meta_set_int(dg, "last", last);

	dg->data = safe_malloc(sizeof(buf_t));
	dg->data->ptr = bn->data.bytes; 
	dg->data->len = bn->len; 

	return wire_handle(wa, dg);
}

int
l3_recv_frequent_contact(wa_t *wa, bnode_t *bn)
{
	return 0; //TODO session_update_user(wa, u);
}

int
l3_recv_frequent_contacts(wa_t *wa, bnode_t *bn)
{
	user_t *u;
	json_object *j;
	const char *type;
	int i,ret = 0;

	u = malloc(sizeof(user_t));
	assert(u);

	j = json_object_object_get(bn->attr, "type");
	assert(j);
	type = json_object_get_string(j);

	if(strcmp(type, "frequent") != 0)
	{
		LOG_WARN("Unknown contact type: %s\n", type);
		return -1;
	}

	if(bn->type != BNODE_LIST)
		return -1;

	for(i=0; i<bn->len; i++)
	{
		ret |= l3_recv_frequent_contact(wa, bn);
	}

	return ret;
}


/* We have an order problem with the three packets that arrive at the beginning
 * of the connection, sometimes out of order:
 *
 * a) <action:message add:last> is received with the last message in
 * each recent conversation.
 *
 * b) <action:contacts> contains all the contact list.
 *
 * c) <action:message add:before last:true> contains the last ~20 messages of
 * each conversation, *excluding the last one*. Can consist of multiple packets.
 *
 * This behavior leads to the need of queueing the packets, as we want to read
 * (b), then (c) then (a), so we have all the contacts before reading the
 * messages.
 *
 * Also, we don't know which packet is which until we decrypt and parse each of
 * them. Thus, we need to queue them *after* the parsing process.
 */

int
l3_recv_bnode(wa_t *wa, bnode_t *bn)
{

	if(!bn->desc)
	{
		LOG_WARN("desc is NULL\n");
		return -1;
	}
	bnode_summary(bn, 0);
	if(strcmp("action", bn->desc) == 0)
		return l3_recv_action(wa, bn);
	else if(strcmp("response", bn->desc) == 0)
		return l3_recv_response(wa, bn);
	else
	{
		LOG_WARN("Unknown bnode with desc: %s\n", bn->desc);
	}

	return 0;
}

static int
l3_recv_dg(wa_t *wa, dg_t *dg)
{
	bnode_t *bn;
	int ret;

	bn = bnode_from_buf(dg->data);
	ret = l3_recv_bnode(wa, bn);

	bnode_free(bn);

	return ret;
}

int
l3_send_relay(wa_t *wa, bnode_t *child, char *tag)
{
	bnode_t *b;
	int ret;
	buf_t *out;
	char *msg_counter;

	b = malloc(sizeof(bnode_t));
	assert(b);

	b->desc = strdup("action");
	b->attr = json_object_new_object();

	json_object_object_add(b->attr, "type",
			json_object_new_string("relay"));

	asprintf(&msg_counter, "%d", wa->msg_counter++);
	json_object_object_add(b->attr, "epoch",
			json_object_new_string(msg_counter));

	b->type = BNODE_LIST;
	b->data.list = (bnode_t **) malloc(sizeof(bnode_t *) * 1);
	b->len = 1;

	b->data.list[0] = child;

	out = bnode_to_buf(b);

	dg_t *dg = dg_cmd(L3, L2, "send");
	dg_meta_set_int(dg, "metric", METRIC_MESSAGE);
	dg_meta_set_int(dg, "flag", FLAG_IGNORE);
	dg_meta_set(dg, "tag", tag);
	dg->data = out;

	ret = wire_handle(wa, dg);

	free(b);
	/*free(buf); Not here */
	free(out);
	free(msg_counter);

	return ret;
}

int
l3_send_relay_message(wa_t *wa, dg_t *dg)
{
	bnode_t *b;
	int ret;
	char *tag;

	/* The id of the message is used as tag */
	tag = dg_meta_get(dg, "tag");

	if(!tag)
	{
		LOG_ERR("Expecting the 'tag' key in metadata\n");
		return 1;
	}

	/* FIXME: Use safe memory */
	b = malloc(sizeof(bnode_t));
	assert(b);

	b->desc = strdup("message");
	b->attr = NULL;

	b->type = BNODE_BINARY;
	b->data.bytes = dg->data->ptr;
	b->len = dg->data->len;

	ret = l3_send_relay(wa, b, tag);

	/* FIXME: Proper free of bnode */
	free(b);

	return ret;
}

static int
l3_send_seen(wa_t *wa, dg_t *dg_in)
{
	/* Send the following:
	 *
	 * metric = 11
	 * flag = 0xc0
	 *
	 * BNODE
	 * {
	 *   desc: action
	 *   type: list
	 *   attr:
	 *   {
	 *     type : set
	 *     epoch : 6 (Incremental msg counter)
	 *   }
	 *   content
	 *   {
	 *     BNODE
	 *     {
	 *       desc: read
	 *       type: empty
	 *       attr:
	 *       {
	 *         jid : 34666666666@s.whatsapp.net (The sender)
	 *         index : XXXXXXXXXXXXXXXXXXXXXXXXXXXXX (The last msg id)
	 *         owner : false (If the msg comes from us)
	 *         count : 2 (Number of messages seen)
	 *       }
	 *       content: empty
	 *     }
	 * }
	 * */

	dg_t *dg;
	bnode_t *root, *child;
	int ret;
	char *epoch, *jid, *id;

	jid = dg_meta_get(dg_in, "jid");
	id = dg_meta_get(dg_in, "id");

	if(!jid || !id)
	{
		LOG_ERR("Missing 'jid' or 'id' in metadata\n");
		return 1;
	}

	asprintf(&epoch, "%d", wa->msg_counter++);

	root = calloc(sizeof(bnode_t), 1);
	assert(root);

	root->desc = strdup("action");

	bnode_attr_add(root, "type", "set");
	bnode_attr_add(root, "epoch", epoch);

	root->type = BNODE_LIST;
	root->data.list = malloc(sizeof(bnode_t *) * 1);
	root->len = 1;

	child = calloc(sizeof(bnode_t), 1);

	child->desc = strdup("read");
	child->type = BNODE_EMPTY;

	bnode_attr_add(child, "jid", jid);
	bnode_attr_add(child, "index", id);
	bnode_attr_add(child, "owner", "false");

	/* TODO: Implement count > 1 */

	bnode_attr_add(child, "count", "1");

	root->data.list[0] = child;

	/* Original flags:
	 *
	 * flags = FLAG_EXPIRES | FLAG_SKIP_OFFLINE;
	 *
	 * But we want an ack to proceed.
	 */

	dg = dg_cmd(L3, L2, "send");
	dg_meta_set_int(dg, "metric", METRIC_READ);
	dg_meta_set_int(dg, "flag", FLAG_EXPIRES | FLAG_ACK_REQUEST);
	dg->data = bnode_to_buf(root);

	ret = wire_handle(wa, dg);

	/* The child is automatically free'd */
	bnode_free(root);

	free(epoch);

	dg_free(dg);

	return ret;
}

int
l3_send(wa_t *wa, dg_t *dg)
{
	char *cmd;

	cmd = dg_meta_get(dg, "cmd");

	if(!cmd)
	{
		LOG_ERR("Malformed datagram without cmd\n");
		return -1;
	}

	if(strcmp(cmd, "send_relay_message") == 0)
		return l3_send_relay_message(wa, dg);
	else if(strcmp(cmd, "send_seen") == 0)
		return l3_send_seen(wa, dg);

	LOG_ERR("Unknown cmd in datagram: %s\n", cmd);
	return -1;
}

int
l3_recv(wa_t *wa, dg_t *dg)
{
	char *cmd;

	cmd = dg_meta_get(dg, "cmd");

	if(!cmd)
	{
		LOG_ERR("Malformed datagram without cmd\n");
		return -1;
	}

	if(strcmp(cmd, "recv") == 0)
		return l3_recv_dg(wa, dg);

	LOG_ERR("Unknown cmd in datagram: %s\n", cmd);
	return -1;
}
