/* $Id: tei.c,v 2.0 2007/06/16 13:11:43 kkeil Exp $
 *
 * Author	Karsten Keil <kkeil@novell.com>
 *
 * Copyright 2007  by Karsten Keil <kkeil@novell.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "layer2.h"
#include <linux/random.h>
#include <linux/mISDNhw.h>
#include "core.h"

const char *tei_revision = "$Revision: 2.0 $";

#define ID_REQUEST	1
#define ID_ASSIGNED	2
#define ID_DENIED	3
#define ID_CHK_REQ	4
#define ID_CHK_RES	5
#define ID_REMOVE	6
#define ID_VERIFY	7

#define TEI_ENTITY_ID	0xf

#define MGR_PH_ACTIVE	16
#define MGR_PH_NOTREADY	17

static struct Fsm teifsm = {NULL, 0, 0, NULL, NULL};

enum {
	ST_TEI_NOP,
	ST_TEI_IDREQ,
	ST_TEI_IDVERIFY,
};

#define TEI_STATE_COUNT (ST_TEI_IDVERIFY+1)

static 	u_int	*debug;

static char *strTeiState[] =
{
	"ST_TEI_NOP",
	"ST_TEI_IDREQ",
	"ST_TEI_IDVERIFY",
};

enum {
	EV_IDREQ,
	EV_ASSIGN,
	EV_ASSIGN_REQ,
	EV_DENIED,
	EV_CHKREQ,
	EV_REMOVE,
	EV_VERIFY,
	EV_TIMER,
};

#define TEI_EVENT_COUNT (EV_TIMER+1)

static char *strTeiEvent[] =
{
	"EV_IDREQ",
	"EV_ASSIGN",
	"EV_ASSIGN_REQ",
	"EV_DENIED",
	"EV_CHKREQ",
	"EV_REMOVE",
	"EV_VERIFY",
	"EV_TIMER",
};

static u_int
new_id(struct mISDNmanager *mgr)
{
	u_int	id;

	id = mgr->nextid++;
	if (id == 0x7fff)
		mgr->nextid = 1;
	id <<= 16;
	id |= GROUP_TEI << 8;
	id |= TEI_SAPI;
	return id;
}

static void
do_send(struct mISDNmanager *mgr)
{
	if (!test_bit(MGR_PH_ACTIVE, &mgr->options))
		return;

	if (!test_and_set_bit(MGR_PH_NOTREADY, &mgr->options)) {
		struct sk_buff	*skb = skb_dequeue(&mgr->sendq);

		if (!skb) {
			test_and_clear_bit(MGR_PH_NOTREADY, &mgr->options);
			return;
		}
		mgr->lastid = mISDN_HEAD_ID(skb);
		if (mgr->ch.recv(mgr->ch.rst, skb)) {
			dev_kfree_skb(skb);
			test_and_clear_bit(MGR_PH_NOTREADY, &mgr->options);
			mgr->lastid = MISDN_ID_NONE;
		}
	}
}

static void
do_ack(struct mISDNmanager *mgr, u_int id)
{
	if (test_bit(MGR_PH_NOTREADY, &mgr->options)) {
		if (id == mgr->lastid) {
			if (test_bit(MGR_PH_ACTIVE, &mgr->options)) {
				struct sk_buff	*skb;

				skb = skb_dequeue(&mgr->sendq);
				if (skb) {
					mgr->lastid = mISDN_HEAD_ID(skb);
					if (!mgr->ch.recv(mgr->ch.rst, skb))
						return;
					dev_kfree_skb(skb);
				}
			}
			mgr->lastid = MISDN_ID_NONE;
			test_and_clear_bit(MGR_PH_NOTREADY, &mgr->options);
		}
	}
}

static void
mgr_send_down(struct mISDNmanager *mgr, struct sk_buff *skb)
{
	skb_queue_tail(&mgr->sendq, skb);
	if (!test_bit(MGR_PH_ACTIVE, &mgr->options)) {
		_queue_data(&mgr->ch, PH_ACTIVATE_REQ, MISDN_ID_ANY, 0,
		    NULL, GFP_KERNEL);
	} else {
		do_send(mgr);
	}
}

static int
dl_unit_data(struct mISDNmanager *mgr, struct sk_buff *skb)
{
	if (!test_bit(MGR_OPT_NETWORK, &mgr->options)) /* only net side send UI */
		return -EINVAL;
	if (!test_bit(MGR_PH_ACTIVE, &mgr->options))
		_queue_data(&mgr->ch, PH_ACTIVATE_REQ, MISDN_ID_ANY, 0,
		    NULL, GFP_KERNEL);
	skb_push(skb, 3);
	skb->data[0] = 0x02; /* SAPI 0 C/R = 1 */
	skb->data[1] = 0xff; /* TEI 127 */
	skb->data[2] = UI;   /* UI frame */
	mISDN_HEAD_PRIM(skb) = PH_DATA_REQ;
	mISDN_HEAD_ID(skb) = new_id(mgr);
	skb_queue_tail(&mgr->sendq, skb);
	do_send(mgr);
	return(0);
}

unsigned int
random_ri(void)
{
	u16 x;

	get_random_bytes(&x, sizeof(x));
	return x;
}

static struct layer2 *
findtei(struct mISDNmanager *mgr, int tei)
{
	struct layer2	*l2;
	u_long		flags;

	read_lock_irqsave(&mgr->lock, flags);
	list_for_each_entry(l2, &mgr->layer2, list) {
		if ((l2->sapi == 0) && (l2->tei > 0) && 
		    (l2->tei != GROUP_TEI) && (l2->tei == tei))
			goto done;
	}
	l2 = NULL;
done:
	read_unlock_irqrestore(&mgr->lock, flags);
	return l2;
}

static void
put_tei_msg(struct teimgr *tm, u_char m_id, unsigned int ri, u_char tei)
{
	struct sk_buff *skb;
	u_char bp[8];

	bp[0] = (TEI_SAPI << 2);
	if (test_bit(FLG_LAPD_NET, &tm->l2->flag))
		bp[0] |= 2; /* CR:=1 for net command */
	bp[1] = (GROUP_TEI << 1) | 0x1;
	bp[2] = UI;
	bp[3] = TEI_ENTITY_ID;
	bp[4] = ri >> 8;
	bp[5] = ri & 0xff;
	bp[6] = m_id;
	bp[7] = (tei << 1) | 1;
	skb = _alloc_mISDN_skb(PH_DATA_REQ, new_id(tm->mgr),
	    8, bp, GFP_ATOMIC);
	if (!skb) {
		printk(KERN_WARNING "%s: no skb for tei msg\n", __FUNCTION__);
		return;
	}
	mgr_send_down(tm->mgr, skb); 
}

static void
tei_id_request(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;

	if (tm->l2->tei != GROUP_TEI) {
		tm->tei_m.printdebug(&tm->tei_m,
			"assign request for allready assigned tei %d",
			tm->l2->tei);
		return;
	}
	tm->ri = random_ri();
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(&tm->tei_m,
			"assign request ri %d", tm->ri);
	put_tei_msg(tm, ID_REQUEST, tm->ri, GROUP_TEI);
	mISDN_FsmChangeState(fi, ST_TEI_IDREQ);
	mISDN_FsmAddTimer(&tm->timer, tm->tval, EV_TIMER, NULL, 1);
	tm->nval = 3;
}

static void
tei_assign_req(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;
	u_char *dp = arg;

	if (tm->l2->tei == GROUP_TEI) {
		tm->tei_m.printdebug(&tm->tei_m,
			"net tei assign request without tei");
		return;
	}
	tm->ri = ((unsigned int) *dp++ << 8);
	tm->ri += *dp++;
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(&tm->tei_m,
			"net assign request ri %d teim %d", tm->ri, *dp);
	put_tei_msg(tm, ID_ASSIGNED, tm->ri, tm->l2->tei);
	mISDN_FsmChangeState(fi, ST_TEI_NOP);
}

static void
tei_id_assign(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr	*tm = fi->userdata;
	struct layer2	*l2;
	u_char *dp = arg;
	int ri, tei;

	ri = ((unsigned int) *dp++ << 8);
	ri += *dp++;
	dp++;
	tei = *dp >> 1;
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(fi, "identity assign ri %d tei %d",
			ri, tei);
	if ((l2 = findtei(tm->mgr, tei))) {	/* same tei is in use */
		if (ri != l2->tm->ri) {
			tm->tei_m.printdebug(fi,
				"possible duplicate assignment tei %d", tei);
			tei_l2(l2, MDL_ERROR_RSP, 0);
		}
	} else if (ri == tm->ri) {
		mISDN_FsmDelTimer(&tm->timer, 1);
		mISDN_FsmChangeState(fi, ST_TEI_NOP);
		tei_l2(tm->l2, MDL_ASSIGN_REQ, tei);
//		cs->cardmsg(cs, MDL_ASSIGN | REQUEST, NULL);
	}
}

static void
tei_id_test_dup(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr	*tm = fi->userdata;
	struct layer2	*l2;
	u_char *dp = arg;
	int tei, ri;

	ri = ((unsigned int) *dp++ << 8);
	ri += *dp++;
	dp++;
	tei = *dp >> 1;
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(fi, "foreign identity assign ri %d tei %d",
			ri, tei);
	if ((l2 = findtei(tm->mgr, tei))) {	/* same tei is in use */
		if (ri != l2->tm->ri) {	/* and it wasn't our request */
			tm->tei_m.printdebug(fi,
				"possible duplicate assignment tei %d", tei);
			mISDN_FsmEvent(&l2->tm->tei_m, EV_VERIFY, NULL);
		}
	} 
}

static void
tei_id_denied(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;
	u_char *dp = arg;
	int ri, tei;

	ri = ((unsigned int) *dp++ << 8);
	ri += *dp++;
	dp++;
	tei = *dp >> 1;
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(fi, "identity denied ri %d tei %d",
			ri, tei);
}

static void
tei_id_chk_req(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;
	u_char *dp = arg;
	int tei;

	tei = *(dp+3) >> 1;
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(fi, "identity check req tei %d", tei);
	if ((tm->l2->tei != GROUP_TEI) && ((tei == GROUP_TEI) || (tei == tm->l2->tei))) {
		mISDN_FsmDelTimer(&tm->timer, 4);
		mISDN_FsmChangeState(&tm->tei_m, ST_TEI_NOP);
		put_tei_msg(tm, ID_CHK_RES, random_ri(), tm->l2->tei);
	}
}

static void
tei_id_remove(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;
	u_char *dp = arg;
	int tei;

	tei = *(dp+3) >> 1;
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(fi, "identity remove tei %d", tei);
	if ((tm->l2->tei != GROUP_TEI) &&
	    ((tei == GROUP_TEI) || (tei == tm->l2->tei))) {
		mISDN_FsmDelTimer(&tm->timer, 5);
		mISDN_FsmChangeState(&tm->tei_m, ST_TEI_NOP);
		tei_l2(tm->l2, MDL_REMOVE_REQ, 0);
//		cs->cardmsg(cs, MDL_REMOVE | REQUEST, NULL);
	}
}

static void
tei_id_verify(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;

	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(fi, "id verify request for tei %d",
			tm->l2->tei);
	put_tei_msg(tm, ID_VERIFY, 0, tm->l2->tei);
	mISDN_FsmChangeState(&tm->tei_m, ST_TEI_IDVERIFY);
	mISDN_FsmAddTimer(&tm->timer, tm->tval, EV_TIMER, NULL, 2);
	tm->nval = 2;
}

static void
tei_id_req_tout(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;

	if (--tm->nval) {
		tm->ri = random_ri();
		if (*debug & DEBUG_L2_TEI)
			tm->tei_m.printdebug(fi, "assign req(%d) ri %d",
				4 - tm->nval, tm->ri);
		put_tei_msg(tm, ID_REQUEST, tm->ri, GROUP_TEI);
		mISDN_FsmAddTimer(&tm->timer, tm->tval, EV_TIMER, NULL, 3);
	} else {
		tm->tei_m.printdebug(fi, "assign req failed");
		tei_l2(tm->l2, MDL_ERROR_RSP, 0);
//		cs->cardmsg(cs, MDL_REMOVE | REQUEST, NULL);
		mISDN_FsmChangeState(fi, ST_TEI_NOP);
	}
}

static void
tei_id_ver_tout(struct FsmInst *fi, int event, void *arg)
{
	struct teimgr *tm = fi->userdata;

	if (--tm->nval) {
		if (*debug & DEBUG_L2_TEI)
			tm->tei_m.printdebug(fi,
				"id verify req(%d) for tei %d",
				3 - tm->nval, tm->l2->tei);
		put_tei_msg(tm, ID_VERIFY, 0, tm->l2->tei);
		mISDN_FsmAddTimer(&tm->timer, tm->tval, EV_TIMER, NULL, 4);
	} else {
		tm->tei_m.printdebug(fi, "verify req for tei %d failed",
			tm->l2->tei);
		tei_l2(tm->l2, MDL_REMOVE_REQ, 0);
//		cs->cardmsg(cs, MDL_REMOVE | REQUEST, NULL);
		mISDN_FsmChangeState(fi, ST_TEI_NOP);
	}
}

static void
tei_ph_data_ind(struct teimgr *tm, u_int mt, u_char *dp, int len)
{
	if (test_bit(FLG_FIXED_TEI, &tm->l2->flag) &&
		!test_bit(FLG_LAPD_NET, &tm->l2->flag))
		return;
	if (*debug & DEBUG_L2_TEI)
		tm->tei_m.printdebug(&tm->tei_m, "tei handler mt %x", mt);
	if (mt == ID_ASSIGNED)
		mISDN_FsmEvent(&tm->tei_m, EV_ASSIGN, dp);
	else if (mt == ID_DENIED)
		mISDN_FsmEvent(&tm->tei_m, EV_DENIED, dp);
	else if (mt == ID_CHK_REQ)
		mISDN_FsmEvent(&tm->tei_m, EV_CHKREQ, dp);
	else if (mt == ID_REMOVE)
		mISDN_FsmEvent(&tm->tei_m, EV_REMOVE, dp);
	// TODO ID_VERIFY ID_CHK_RES
}

static void
new_tei_req(struct mISDNmanager *mgr, u_char *dp)
{
}

static int
ph_data_ind(struct mISDNmanager *mgr, struct sk_buff *skb)
{
	int		ret = -EINVAL;
	struct layer2	*l2;
	u_long		flags;
	u_char		mt;

	if (skb->len < 8) {
		if (*debug  & DEBUG_L2_TEI)
			printk(KERN_DEBUG "%s: short mgr frame %d/8\n",
			    __FUNCTION__, skb->len);
		goto done;
	}
	if ((skb->data[0] >> 2) != TEI_SAPI) /* not for us */
		goto done;
	if (skb->data[0] & 1) /* EA0 formal error */
		goto done;
	if (!(skb->data[1] & 1)) /* EA1 formal error */
		goto done;
	if ((skb->data[1] >> 1) != GROUP_TEI) /* not for us */
		goto done;
	if ((skb->data[2] & 0xef) != UI) /* not UI */
		goto done;
	if (skb->data[3] != TEI_ENTITY_ID) /* not tei entity */
		goto done;
	mt = skb->data[6];
	switch(mt) {
	case ID_REQUEST:
	case ID_CHK_RES:
	case ID_VERIFY:
		if (!test_bit(MGR_OPT_NETWORK, &mgr->options))
			goto done;
		break;
	case ID_ASSIGNED:
	case ID_DENIED:
	case ID_CHK_REQ:
	case ID_REMOVE:
		if (test_bit(MGR_OPT_NETWORK, &mgr->options))
			goto done;
		break;
	default:
		goto done;
	}
	ret = 0;
	if (mt == ID_REQUEST) {
		new_tei_req(mgr, &skb->data[4]);
		goto done;
	}
	read_lock_irqsave(&mgr->lock, flags);
	list_for_each_entry(l2, &mgr->layer2, list) {
		tei_ph_data_ind(l2->tm, mt, &skb->data[4], skb->len - 4);
	}
	read_unlock_irqrestore(&mgr->lock, flags);
done:
	return ret;
}

int
l2_tei(struct layer2 *l2, u_int cmd, u_long arg)
{
	struct teimgr	*tm = l2->tm;

	if (*debug & DEBUG_L2_TEI)
		printk(KERN_DEBUG "%s: cmd(%x)\n", __FUNCTION__, cmd);
	switch(cmd) {
	    case MDL_ASSIGN_IND:
		if (test_bit(FLG_FIXED_TEI, &l2->flag)) {
			if (*debug & DEBUG_L2_TEI)
				tm->tei_m.printdebug(&tm->tei_m,
					"fixed assign tei %d", l2->tei);
			tei_l2(l2, MDL_ASSIGN_REQ, l2->tei);
//			cs->cardmsg(cs, MDL_ASSIGN | REQUEST, NULL);
		} else
			mISDN_FsmEvent(&tm->tei_m, EV_IDREQ, NULL);
		break;
	    case MDL_ERROR_IND:
	    	if (!test_bit(FLG_FIXED_TEI, &l2->flag))
			mISDN_FsmEvent(&tm->tei_m, EV_VERIFY, NULL);
		break;
	}
	return 0;
}

static void
tei_debug(struct FsmInst *fi, char *fmt, ...)
{
	struct teimgr	*tm = fi->userdata;
	va_list va;

	if (!(*debug & DEBUG_L2_TEIFSM))
		return;
	va_start(va, fmt);
	printk(KERN_DEBUG "tei(%d): ", tm->l2->tei);
	vprintk(fmt, va);
	printk("\n");
	va_end(va);
}

static struct FsmNode TeiFnList[] =
{
	{ST_TEI_NOP, EV_IDREQ, tei_id_request},
	{ST_TEI_NOP, EV_ASSIGN, tei_id_test_dup},
	{ST_TEI_NOP, EV_ASSIGN_REQ, tei_assign_req},
	{ST_TEI_NOP, EV_VERIFY, tei_id_verify},
	{ST_TEI_NOP, EV_REMOVE, tei_id_remove},
	{ST_TEI_NOP, EV_CHKREQ, tei_id_chk_req},
	{ST_TEI_IDREQ, EV_TIMER, tei_id_req_tout},
	{ST_TEI_IDREQ, EV_ASSIGN, tei_id_assign},
	{ST_TEI_IDREQ, EV_DENIED, tei_id_denied},
	{ST_TEI_IDVERIFY, EV_TIMER, tei_id_ver_tout},
	{ST_TEI_IDVERIFY, EV_REMOVE, tei_id_remove},
	{ST_TEI_IDVERIFY, EV_CHKREQ, tei_id_chk_req},
};

void
release_tei(struct layer2 *l2)
{
	struct teimgr	*tm = l2->tm;
	u_long		flags;

	mISDN_FsmDelTimer(&tm->timer, 1);
	write_lock_irqsave(&tm->mgr->lock, flags);
	list_del(&l2->list);
	write_unlock_irqrestore(&tm->mgr->lock, flags);
	l2->tm = NULL;
	kfree(tm);
}

static int
create_teimgr(struct mISDNmanager *mgr, struct channel_req *crq)
{
	struct layer2	*l2;
	u_int 		opt = 0;
	u_long		flags;

	if (*debug & DEBUG_L2_TEI)
		printk(KERN_DEBUG "%s: %s proto(%x) adr(%d %d %d %d %d)\n",
			__FUNCTION__, mgr->ch.dev->name, crq->protocol,
			crq->adr.dev, crq->adr.channel, crq->adr.id,
			crq->adr.sapi, crq->adr.tei);
	if (crq->adr.sapi != 0) /* not supported yet */
		return -EINVAL;
	if (crq->adr.tei > GROUP_TEI)
		return -EINVAL;
	if (crq->adr.tei < 64)
		test_and_set_bit(OPTION_L2_FIXEDTEI, &opt);
	if (crq->adr.tei == 0)
		test_and_set_bit(OPTION_L2_PTP, &opt);
	if (test_bit(MGR_OPT_NETWORK, &mgr->options)) {
		if (crq->protocol == ISDN_P_LAPD_TE)
			return -EPROTONOSUPPORT;
	}
	if (test_bit(MGR_OPT_USER, &mgr->options)) {
		if (crq->protocol == ISDN_P_LAPD_NT)
			return -EPROTONOSUPPORT;
		if ((crq->adr.tei >= 64) && (crq->adr.tei < GROUP_TEI))
			return -EINVAL; /* dyn tei */
	}
	if (mgr->ch.dev->nrbchan > 2)
		test_and_set_bit(OPTION_L2_PMX, &opt);
	l2 = create_l2(crq->protocol, opt, (u_long)crq->adr.tei);
	if (!l2)
		return -ENOMEM;
	l2->tm = kzalloc(sizeof(struct teimgr), GFP_KERNEL);
	if (!l2->tm) {
		kfree(l2);
		printk(KERN_ERR "kmalloc teimgr failed\n");
		return -ENOMEM;
	}
	l2->tm->mgr = mgr;
	l2->tm->l2 = l2;
	l2->tm->tei_m.debug = *debug & DEBUG_L2_TEIFSM;
	l2->tm->tei_m.userdata = l2->tm;
	l2->tm->tei_m.printdebug = tei_debug;
	if (crq->protocol == ISDN_P_LAPD_TE) {
		l2->tm->tei_m.fsm = &teifsm;
		l2->tm->tei_m.state = ST_TEI_NOP;
		l2->tm->tval = 1000; /* T201  1 sec */
	} else {
		l2->tm->tei_m.fsm = &teifsm;
		l2->tm->tei_m.state = ST_TEI_NOP;
		l2->tm->tval = 2000; /* T202  2 sec */
	}
	mISDN_FsmInitTimer(&l2->tm->tei_m, &l2->tm->timer);
	write_lock_irqsave(&mgr->lock, flags);
	list_add_tail(&l2->list, &mgr->layer2);
	write_unlock_irqrestore(&mgr->lock, flags);
	crq->ch = &l2->ch;
	return 0;
}

static int
mgr_send(struct mISDNchannel *ch, struct sk_buff *skb)
{
	struct mISDNmanager	*mgr = container_of(ch,
	    struct mISDNmanager, ch);
	struct mISDNhead	*hh =  mISDN_HEAD_P(skb);
	int			ret = -EINVAL;

	switch (hh->prim) {
	case PH_DATA_IND:
		ret = ph_data_ind(mgr, skb);
		break;
	case PH_DATA_CNF:
		do_ack(mgr, hh->id);
		ret = 0;
		break;
	case PH_ACTIVATE_IND:
		test_and_set_bit(MGR_PH_ACTIVE, &mgr->options);
		do_send(mgr);
		ret = 0;
		break;
	case PH_DEACTIVATE_IND:
		test_and_clear_bit(MGR_PH_ACTIVE, &mgr->options);
		ret = 0;
		break;
	case DL_UNITDATA_REQ:
		return dl_unit_data(mgr, skb);
	}
	if (!ret)
		dev_kfree_skb(skb);
	return ret;
}

static int
mgr_ctrl(struct mISDNchannel *ch, u_int cmd, void *arg)
{
	struct mISDNmanager	*mgr = container_of(ch,
	    struct mISDNmanager, ch);
	int			ret = -EINVAL;
	switch(cmd) {
	case CREATE_CHANNEL:
		ret = create_teimgr(mgr, arg);
		break;
	}
	return ret;
}

struct mISDNmanager *
mISDN_create_manager(void)
{
	struct mISDNmanager *mgr;

	mgr = kzalloc(sizeof(struct mISDNmanager), GFP_KERNEL);
	if (!mgr)
		return NULL;
	INIT_LIST_HEAD(&mgr->layer2);
	mgr->lock = RW_LOCK_UNLOCKED;
	skb_queue_head_init(&mgr->sendq);
	mgr->nextid = 1;
	mgr->lastid = MISDN_ID_NONE;
	mgr->ch.send = mgr_send;
	mgr->ch.ctrl = mgr_ctrl;
	return mgr;
}

int TEIInit(u_int *deb)
{
	debug = deb;
	teifsm.state_count = TEI_STATE_COUNT;
	teifsm.event_count = TEI_EVENT_COUNT;
	teifsm.strEvent = strTeiEvent;
	teifsm.strState = strTeiState;
	mISDN_FsmNew(&teifsm, TeiFnList, ARRAY_SIZE(TeiFnList));
	return(0);
}

void TEIFree(void)
{
	mISDN_FsmFree(&teifsm);
}