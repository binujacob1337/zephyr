/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>

#include <bluetooth/hci.h>

#include "util/mem.h"
#include "util/mfifo.h"
#include "util/memq.h"
#include "util.h"

#include "hal/cpu_vendor_hal.h"
#include "hal/ccm.h"

#include "pdu.h"
#include "ll.h"
#include "ll_feat.h"
#include "ll_settings.h"
#include "lll.h"
#include "lll_vendor.h"
#include "lll_adv.h"
#include "lll_scan.h"
#include "lll_sync.h"
#include "lll_conn.h"

/* Backing storage for elements in mfifo_done */
static struct {
	void *free;
	uint8_t pool[sizeof(struct node_rx_event_done) * EVENT_DONE_MAX];
} mem_done;

static struct {
	void *free;
	uint8_t pool[sizeof(memq_link_t) * EVENT_DONE_MAX];
} mem_link_done;

/*
 * just a big number
 */

#define PDU_RX_POOL_SIZE 16384
#if defined(CONFIG_BT_RX_USER_PDU_LEN)
#define PDU_RX_USER_PDU_OCTETS_MAX (CONFIG_BT_RX_USER_PDU_LEN)
#else
#define PDU_RX_USER_PDU_OCTETS_MAX 0
#endif
#define NODE_RX_HEADER_SIZE      (offsetof(struct node_rx_pdu, pdu))
#define NODE_RX_STRUCT_OVERHEAD  (NODE_RX_HEADER_SIZE)

#define PDU_ADVERTIZE_SIZE (PDU_AC_LL_SIZE_MAX + PDU_AC_LL_SIZE_EXTRA)
#define PDU_DATA_SIZE      (PDU_DC_LL_HEADER_SIZE + LL_LENGTH_OCTETS_RX_MAX)


#define PDU_RX_NODE_POOL_ELEMENT_SIZE			\
	MROUND(						\
		NODE_RX_STRUCT_OVERHEAD			\
		+ MAX(MAX(PDU_ADVERTIZE_SIZE,		\
			  PDU_DATA_SIZE),		\
		      PDU_RX_USER_PDU_OCTETS_MAX)	\
		)

#if defined(CONFIG_BT_CTLR_PHY) && defined(CONFIG_BT_CTLR_DATA_LENGTH)
#define LL_PDU_RX_CNT (3+128)
#else
#define LL_PDU_RX_CNT (2+128)
#endif

#define PDU_RX_CNT    (CONFIG_BT_CTLR_RX_BUFFERS + 3)
#define RX_CNT        (PDU_RX_CNT + LL_PDU_RX_CNT)

static MEMQ_DECLARE(ull_rx);
static MEMQ_DECLARE(ll_rx);

#if defined(CONFIG_BT_CONN)
static MFIFO_DEFINE(ll_pdu_rx_free, sizeof(void *), LL_PDU_RX_CNT);
#endif /* CONFIG_BT_CONN */

static MFIFO_DEFINE(pdu_rx_free, sizeof(void *), PDU_RX_CNT);

static struct {
	void *free;
	uint8_t pool[PDU_RX_POOL_SIZE];
} mem_pdu_rx;

/*
 * just a big number
 */
#define LINK_RX_POOL_SIZE 16384

static struct {
	uint8_t quota_pdu; /* Number of un-utilized buffers */

	void *free;
	uint8_t pool[LINK_RX_POOL_SIZE];
} mem_link_rx;

sys_slist_t ut_rx_q;

static inline int init_reset(void);
static inline void rx_alloc(uint8_t max);
static inline void ll_rx_link_inc_quota(int8_t delta);


void ull_ticker_status_give(uint32_t status, void *param)
{

}

uint32_t ull_ticker_status_take(uint32_t ret, uint32_t volatile *ret_cb)
{
	return *ret_cb;
}

void *ull_disable_mark(void *param)
{
	return NULL;
}

void *ull_disable_unmark(void *param)
{
	return NULL;
}

int ull_disable(void *lll)
{
	return 0;
}

void *ll_rx_link_alloc(void)
{
	return mem_acquire(&mem_link_rx.free);
}

void ll_rx_link_release(void *link)
{
	mem_release(link, &mem_link_rx.free);
}

void *ll_rx_alloc(void)
{
	return mem_acquire(&mem_pdu_rx.free);
}

void ll_rx_release(void *node_rx)
{
	mem_release(node_rx, &mem_pdu_rx.free);
}

void *ll_pdu_rx_alloc(void)
{
	void *temp;

	temp = MFIFO_DEQUEUE(ll_pdu_rx_free);

	return temp;
}




void ll_rx_put(memq_link_t *link, void *rx)
{
	sys_slist_append(&ut_rx_q, (sys_snode_t *) rx);
}

void ll_rx_sched(void)
{

}

void ll_reset(void)
{
	MFIFO_INIT(ll_pdu_rx_free);
	init_reset();
}

static inline int init_reset(void)
{
	memq_link_t *link;

	/* Initialize done pool. */
	mem_init(mem_done.pool, sizeof(struct node_rx_event_done),
		 EVENT_DONE_MAX, &mem_done.free);

	/* Initialize done link pool. */
	mem_init(mem_link_done.pool, sizeof(memq_link_t), EVENT_DONE_MAX,
		 &mem_link_done.free);

	/* Initialize rx pool. */
	mem_init(mem_pdu_rx.pool, (PDU_RX_NODE_POOL_ELEMENT_SIZE),
		 sizeof(mem_pdu_rx.pool) / (PDU_RX_NODE_POOL_ELEMENT_SIZE),
		 &mem_pdu_rx.free);

	/* Initialize rx link pool. */
	mem_init(mem_link_rx.pool, sizeof(memq_link_t),
		 sizeof(mem_link_rx.pool) / sizeof(memq_link_t),
		 &mem_link_rx.free);

	/* Acquire a link to initialize ull rx memq */
	link = mem_acquire(&mem_link_rx.free);

	/* Initialize ull rx memq */
	MEMQ_INIT(ull_rx, link);

	/* Acquire a link to initialize ll rx memq */
	link = mem_acquire(&mem_link_rx.free);

	/* Initialize ll rx memq */
	MEMQ_INIT(ll_rx, link);

	/* Allocate rx free buffers */
	mem_link_rx.quota_pdu = RX_CNT;
	rx_alloc(UINT8_MAX);

	return 0;
}

static inline void rx_alloc(uint8_t max)
{
	uint8_t idx;

#if defined(CONFIG_BT_CONN)
	while (mem_link_rx.quota_pdu &&
	       MFIFO_ENQUEUE_IDX_GET(ll_pdu_rx_free, &idx)) {
		memq_link_t *link;
		struct node_rx_hdr *rx;

		link = mem_acquire(&mem_link_rx.free);
		if (!link) {
			break;
		}

		rx = mem_acquire(&mem_pdu_rx.free);
		if (!rx) {
			mem_release(link, &mem_link_rx.free);
			break;
		}

		link->mem = NULL;
		rx->link = link;

		MFIFO_BY_IDX_ENQUEUE(ll_pdu_rx_free, idx, rx);

		ll_rx_link_inc_quota(-1);
	}
#endif /* CONFIG_BT_CONN */

	if (max > mem_link_rx.quota_pdu) {
		max = mem_link_rx.quota_pdu;
	}

	while ((max--) && MFIFO_ENQUEUE_IDX_GET(pdu_rx_free, &idx)) {
		memq_link_t *link;
		struct node_rx_hdr *rx;

		link = mem_acquire(&mem_link_rx.free);
		if (!link) {
			break;
		}

		rx = mem_acquire(&mem_pdu_rx.free);
		if (!rx) {
			mem_release(link, &mem_link_rx.free);
			break;
		}

		rx->link = link;

		MFIFO_BY_IDX_ENQUEUE(pdu_rx_free, idx, rx);

		ll_rx_link_inc_quota(-1);
	}
}

static inline void ll_rx_link_inc_quota(int8_t delta)
{
	mem_link_rx.quota_pdu += delta;
}