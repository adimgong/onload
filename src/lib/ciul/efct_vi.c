/* SPDX-License-Identifier: BSD-2-Clause */
/* X-SPDX-Copyright-Text: (c) Copyright 2021 Xilinx, Inc. */

#include "ef_vi_internal.h"
#include "efct_vi.h"
#include <etherfabric/vi.h>
#include <etherfabric/efct_vi.h>
#include <etherfabric/internal/efct_uk_api.h>
#include <ci/efhw/common.h>
#include <ci/tools/byteorder.h>
#include <ci/tools/sysdep.h>


#define M_(FIELD) (CI_MASK64(FIELD ## _WIDTH) << FIELD ## _LBN)
#define M(FIELD) M_(EFCT_RX_HEADER_ ## FIELD)
#define CHECK_FIELDS (M(L2_STATUS) | M(L3_STATUS) | M(L4_STATUS) | M(ROLLOVER))

struct efct_rx_descriptor
{
  uint16_t refcnt;
  uint16_t superbuf_pkts;
  /* Points to the next descriptor in the descriptor buf -1 if NONE*/
  int16_t  sbid_next;
  uint8_t  padding_[1];
  uint8_t  final_ts_status;
  uint64_t final_timestamp;
};

/* pkt_ids are:
 *  bits 0..15 packet index in superbuf
 *  bits 16..26 superbuf index
 *  bits 27..29 rxq (as an index in to vi->efct_rxqs.q, not as a hardware ID)
 *  bits 30..31 unused/zero
 *  [NB: bit 31 is stolen by some users to cache the superbuf's sentinel]
 * This layout is not part of the stable ABI. rxq index is slammed up against
 * superbuf index to allow for dirty tricks where we mmap all superbufs in
 * contiguous virtual address space and thus avoid some arithmetic.
 */

#define PKT_ID_PKT_BITS  16
#define PKT_ID_SBUF_BITS 11
#define PKT_ID_RXQ_BITS   3
#define PKT_ID_TOTAL_BITS (PKT_ID_PKT_BITS + PKT_ID_SBUF_BITS + PKT_ID_RXQ_BITS)

/* Enforce compile-time restrictions on the pkt_id fields */
static inline void assert_pkt_id_fields(void)
{
  /* Packet index must be large enough for the number of packets in a superbuf.
   * We check against the expected value here, and (at runtime) against the
   * actual value provided by the driver in rx_rollover.
   *
   * The value of 16 is fairly arbitrary and could be reduced to 9 if more
   * bits are needed elsewhere.
   */
  EF_VI_BUILD_ASSERT((1u << PKT_ID_PKT_BITS) >=
                     (EFCT_RX_SUPERBUF_BYTES / EFCT_PKT_STRIDE));

  /* Superbuf index must be exactly the right size for the number of superbufs
   * per rxq, since the two fields are combined to give the global index.
   *
   * In principle, CI_EFCT_MAX_SUPERBUFS can be changed, but the bitfield size
   * must be changed to match.
   */
  EF_VI_BUILD_ASSERT((1u << PKT_ID_SBUF_BITS) == CI_EFCT_MAX_SUPERBUFS);

  /* Queue index must be large enough for the number of queues. */
  EF_VI_BUILD_ASSERT((1u << PKT_ID_RXQ_BITS) >= EF_VI_MAX_EFCT_RXQS);

  /* Bit 31 must be available for abuse. */
  EF_VI_BUILD_ASSERT(PKT_ID_TOTAL_BITS <= 31);
}

static int pkt_id_to_index_in_superbuf(uint32_t pkt_id)
{
  return pkt_id & ((1u << PKT_ID_PKT_BITS) - 1);
}

static int pkt_id_to_global_superbuf_ix(uint32_t pkt_id)
{
  EF_VI_ASSERT(pkt_id >> PKT_ID_TOTAL_BITS == 0);
  return pkt_id >> PKT_ID_PKT_BITS;
}

static int pkt_id_to_local_superbuf_ix(uint32_t pkt_id)
{
  return pkt_id_to_global_superbuf_ix(pkt_id) & (CI_EFCT_MAX_SUPERBUFS - 1);
}

static int pkt_id_to_rxq_ix(uint32_t pkt_id)
{
  return pkt_id_to_global_superbuf_ix(pkt_id) / CI_EFCT_MAX_SUPERBUFS;
}

static struct efct_rx_descriptor* efct_rx_desc_for_sb(ef_vi* vi, uint32_t qid, uint32_t sbid) {
  ef_vi_rxq* q = &vi->vi_rxq;
  struct efct_rx_descriptor* desc = q->descriptors;
  return desc + ((qid * CI_EFCT_MAX_SUPERBUFS) | sbid);
}

void efct_rx_sb_free_push(ef_vi* vi, uint32_t qid, uint32_t sbid)
{
  int16_t* head = &vi->ep_state->rxq.sb_desc_free_head[qid];
  efct_rx_desc_for_sb(vi, qid, sbid)->sbid_next = *head;
  *head = sbid;
}

int16_t efct_rx_sb_free_next(ef_vi* vi, uint32_t qid, uint32_t sbid)
{
  return efct_rx_desc_for_sb(vi, qid, sbid)->sbid_next;
}

static bool efct_rxq_is_active(const ef_vi_efct_rxq* rxq)
{
  return *rxq->live.superbuf_pkts != 0;
}

/* The superbuf descriptor for this packet */
static struct efct_rx_descriptor* efct_rx_desc(ef_vi* vi, uint32_t pkt_id)
{
  ef_vi_rxq* q = &vi->vi_rxq;
  struct efct_rx_descriptor* desc = q->descriptors;
  return desc + pkt_id_to_global_superbuf_ix(pkt_id);
}

static const char* efct_superbuf_base(const ef_vi* vi, size_t pkt_id)
{
#ifdef __KERNEL__
  /* FIXME: is this right? I think the table is indexed by huge page not sbuf */
  return vi->efct_rxqs.q[0].superbufs[pkt_id_to_global_superbuf_ix(pkt_id)];
#else
  /* Sneakily rely on vi->efct_rxqs.q[i].superbuf being contiguous, thus avoiding
   * an array lookup (or, more specifically, relying on the TLB to do the
   * lookup for us) */
  return vi->efct_rxqs.q[0].superbuf +
         (size_t)pkt_id_to_global_superbuf_ix(pkt_id) * EFCT_RX_SUPERBUF_BYTES;
#endif
}

/* The header preceding this packet. Note: this contains metadata for the
 * previous packet, not this one. */
static const ci_oword_t* efct_rx_header(const ef_vi* vi, size_t pkt_id)
{
  return (const ci_oword_t*)(efct_superbuf_base(vi, pkt_id) +
                        pkt_id_to_index_in_superbuf(pkt_id) * EFCT_PKT_STRIDE);
}

static uint32_t rxq_ptr_to_pkt_id(uint32_t ptr)
{
  /* Masking off the sentinel */
  return ptr & 0x7fffffff;
}

static int rxq_ptr_to_sentinel(uint32_t ptr)
{
  return ptr >> 31;
}

static bool efct_rxq_need_rollover(const ef_vi_efct_rxq_ptr* rxq_ptr)
{
  return rxq_ptr_to_pkt_id(rxq_ptr->next) >= rxq_ptr->end;
}

static bool efct_rxq_need_config(const ef_vi_efct_rxq* rxq)
{
  return *rxq->live.config_generation != rxq->config_generation;
}

/* The header following the next packet, or null if not available.
 * `next` is a rxq "pointer", containing packet id and sentinel. */
static const ci_oword_t* efct_rx_next_header(const ef_vi* vi, uint32_t next)
{
  const ci_oword_t* header = efct_rx_header(vi, rxq_ptr_to_pkt_id(next));
  int sentinel = CI_QWORD_FIELD(*header, EFCT_RX_HEADER_SENTINEL);

  return sentinel == rxq_ptr_to_sentinel(next) ? header : NULL;
}

/* Check for actions needed on an rxq. This must match the checks made in
 * efct_poll_rx to ensure none are missed. */
static bool efct_rxq_check_event(const ef_vi* vi, int qid)
{
  const ef_vi_efct_rxq* rxq = &vi->efct_rxqs.q[qid];
  const ef_vi_efct_rxq_ptr* rxq_ptr = &vi->ep_state->rxq.rxq_ptr[qid];
  if( ! efct_rxq_is_active(rxq) )
    return false;
  if( efct_rxq_need_rollover(rxq_ptr) )
#ifndef __KERNEL__
    /* only signal new event if rollover can be done */
    return vi->efct_rxqs.ops->available(vi, qid);
#else
    /* Returning no event interferes with oo_handle_wakeup_int_driven
     * Let the interrupt handler deal with the event */
    return true;
#endif

  return efct_rxq_need_config(rxq) ||
     efct_rx_next_header(vi, rxq_ptr->next) != NULL;
}

/* Check whether a received packet is available */
static bool efct_rx_check_event(const ef_vi* vi)
{
  int i;

  if( ! vi->vi_rxq.mask )
    return false;
  for( i = 0; i < EF_VI_MAX_EFCT_RXQS; ++i )
    if( efct_rxq_check_event(vi, i) )
      return true;
  return false;
}

/* tx packet descriptor, stored in the ring until completion */
/* TODO fix the size of this, and update tx_desc_bytes in vi_init.c */
struct efct_tx_descriptor
{
  /* total length including header and padding, in bytes */
  uint16_t len;
};

/* state of a partially-completed tx operation */
struct efct_tx_state
{
  /* base address of the aperture */
  volatile uint64_t* aperture;
  /* up to 7 bytes left over after writing a block in 64-bit chunks */
  uint64_t tail;
  /* number of left over bytes in 'tail' */
  unsigned tail_len;
  /* number of 64-bit words from start of aperture */
  uint64_t offset;
  /* mask to keep offset within the aperture range */
  uint64_t mask;
};

/* generic tx header */
uint64_t efct_tx_header(unsigned packet_length, unsigned ct_thresh,
                               unsigned timestamp_flag, unsigned warm_flag,
                               unsigned action)
{
  ci_qword_t qword;

  RANGECHCK(packet_length, EFCT_TX_HEADER_PACKET_LENGTH_WIDTH);
  RANGECHCK(ct_thresh, EFCT_TX_HEADER_CT_THRESH_WIDTH);
  RANGECHCK(timestamp_flag, EFCT_TX_HEADER_TIMESTAMP_FLAG_WIDTH);
  RANGECHCK(warm_flag, EFCT_TX_HEADER_WARM_FLAG_WIDTH);
  RANGECHCK(action, EFCT_TX_HEADER_ACTION_WIDTH);

  CI_POPULATE_QWORD_5(qword,
      EFCT_TX_HEADER_PACKET_LENGTH, packet_length,
      EFCT_TX_HEADER_CT_THRESH, ct_thresh,
      EFCT_TX_HEADER_TIMESTAMP_FLAG, timestamp_flag,
      EFCT_TX_HEADER_WARM_FLAG, warm_flag,
      EFCT_TX_HEADER_ACTION, action);

  return qword.u64[0];
}

/* tx header for standard (non-templated) send */
ci_inline uint64_t
efct_tx_pkt_header(ef_vi* vi, unsigned length, unsigned ct_thresh)
{
  return efct_tx_header(length, ct_thresh, 0, 0, 0) |
         vi->vi_txq.efct_fixed_header;
}

/* check that we have space to send a packet of this length */
ci_inline bool efct_tx_check(ef_vi* vi, int len)
{
  /* We require the txq to be large enough for the maximum number of packets
   * which can be written to the FIFO. Each packet consumes at least 64 bytes.
   */
  BUG_ON((vi->vi_txq.mask + 1) <
         (vi->vi_txq.ct_fifo_bytes + EFCT_TX_HEADER_BYTES) / EFCT_TX_ALIGNMENT);

  return ef_vi_transmit_space_bytes(vi) >= len;
}

/* initialise state for a transmit operation */
ci_inline void efct_tx_init(ef_vi* vi, struct efct_tx_state* tx)
{
  unsigned offset = vi->ep_state->txq.ct_added;
  BUG_ON(offset % EFCT_TX_ALIGNMENT != 0);
  tx->aperture = (void*) vi->vi_ctpio_mmap_ptr;
  tx->tail = 0;
  tx->tail_len = 0;
  tx->offset = offset >> 3;
  tx->mask = vi->vi_txq.efct_aperture_mask;
}

/* store a left-over byte from the start or end of a block */
ci_inline void efct_tx_tail_byte(struct efct_tx_state* tx, uint8_t byte)
{
  BUG_ON(tx->tail_len >= 8);
  tx->tail = (tx->tail << 8) | byte;
  tx->tail_len++;
}

/* write a 64-bit word to the CTPIO aperture, dealing with wrapping */
ci_inline void efct_tx_word(struct efct_tx_state* tx, uint64_t value)
{
  tx->aperture[tx->offset++ & tx->mask] = value;
}

/* write a block of bytes to the CTPIO aperture, dealing with wrapping and leftovers */
ci_inline void efct_tx_block(struct efct_tx_state* __restrict__ tx, char* base, int len)
{
  if( tx->tail_len != 0 ) {
    while( len > 0 && tx->tail_len < 8 ) {
      efct_tx_tail_byte(tx, *base);
      base++;
      len--;
    }

    if( tx->tail_len == 8 ) {
      efct_tx_word(tx, CI_BSWAP_BE64(tx->tail));
      tx->tail = 0;
      tx->tail_len = 0;
    }
  }

  while( len >= 8 ) {
    efct_tx_word(tx, *(uint64_t*)base);
    base += 8;
    len -= 8;
  }

  while( len > 0 ) {
    efct_tx_tail_byte(tx, *base);
    base++;
    len--;
  }
}

/* complete a tx operation, writing leftover bytes and padding as needed */
ci_inline void efct_tx_complete(ef_vi* vi, struct efct_tx_state* tx, uint32_t dma_id, int len)
{
  ef_vi_txq* q = &vi->vi_txq;
  ef_vi_txq_state* qs = &vi->ep_state->txq;
  struct efct_tx_descriptor* desc = q->descriptors;
  int i = qs->added & q->mask;

  if( tx->tail_len != 0 ) {
    tx->tail <<= (8 - tx->tail_len) * 8;
    efct_tx_word(tx, CI_BSWAP_BE64(tx->tail));
  }
  while( tx->offset % (EFCT_TX_ALIGNMENT >> 3) != 0 )
    efct_tx_word(tx, 0);

  /* Force the write-combined traffic to be flushed to PCIe, to limit the
   * maximum possible reordering the NIC will see to one packet. Benchmarks
   * demonstrate that this sfence is well-parallelised by the CPU, so smarter
   * algorithms trying to avoid it for small packets are unlikely to be
   * cost-effective */
#if defined __x86_64__ || defined __i386__
  /* Our compat tools define ci_wmb() as just a compiler fence on x86, since
   * that's usually right due to TSO. Not in this case. */
  ci_x86_sfence();
#else
  ci_wmb();
#endif

  len = CI_ROUND_UP(len + EFCT_TX_HEADER_BYTES, EFCT_TX_ALIGNMENT);
  desc[i].len = len;
  q->ids[i] = dma_id;
  qs->ct_added += len;
  qs->added += 1;
}

/* get a tx completion event, or null if no valid event available */
ci_qword_t* efct_tx_get_event(const ef_vi* vi, uint32_t evq_ptr)
{
  ci_qword_t* event = (ci_qword_t*)(vi->evq_base + (evq_ptr & vi->evq_mask));

  int expect_phase = (evq_ptr & (vi->evq_mask + 1)) != 0;
  int actual_phase = CI_QWORD_FIELD(*event, EFCT_EVENT_PHASE);

  return actual_phase == expect_phase ? event : NULL;
}

/* check whether a tx completion event is available */
ci_inline bool efct_tx_check_event(const ef_vi* vi)
{
  return vi->evq_mask && efct_tx_get_event(vi, vi->ep_state->evq.evq_ptr);
}

/* Writes an unsolicited credit sequence value (max 7-bit wide) to the appropiate 
 * register. This function should be called on timesync events, and upon an 
 * unsolicited_credit_overflow. The sequence should correspond to how many 
 * unsolicited credit events have been seen + a small buffer extra. When this
 * extra buffer is consumed, an unsolicited credit overflow is expected, and
 * the register should be reset with a sensible default. */
static void efct_grant_unsol_credit(ef_vi* vi, bool clear_overflow, uint32_t credit_seq)
{
  uint32_t* unsol_reg = (void*) (vi->io + EFCT_EVQ_UNSOL_CREDIT_REGISTER_OFFSET);
  ci_qword_t qword;

  credit_seq &= vi->unsol_credit_seq_mask;
  CI_POPULATE_QWORD_2(qword,
                      EFCT_EVQ_UNSOL_GRANT_SEQ, credit_seq,
                      EFCT_EVQ_UNSOL_CLEAR_OVERFLOW, clear_overflow);

  writel(qword.u64[0], unsol_reg);
}

/* handle a tx completion event */
void efct_tx_handle_event(ef_vi* vi, ci_qword_t event, ef_event* ev_out)
{
  ef_vi_txq* q = &vi->vi_txq;
  ef_vi_txq_state* qs = &vi->ep_state->txq;
  struct efct_tx_descriptor* desc = vi->vi_txq.descriptors;

  unsigned seq = CI_QWORD_FIELD(event, EFCT_TX_EVENT_SEQUENCE);
  unsigned seq_mask = (1 << EFCT_TX_EVENT_SEQUENCE_WIDTH) - 1;

  /* Fully inclusive range as both previous and seq are both inclusive */
  while( (qs->previous & seq_mask) != ((seq + 1) & seq_mask) ) {
    BUG_ON(qs->previous == qs->added);
    qs->ct_removed += desc[qs->previous & q->mask].len;
    qs->previous += 1;
  }

  if( CI_QWORD_FIELD(event, EFCT_TX_EVENT_TIMESTAMP_STATUS) ) {
    uint64_t ptstamp;
    uint32_t ptstamp_seconds;
    uint32_t timesync_seconds;

    EF_VI_ASSERT(vi->vi_flags & EF_VI_TX_TIMESTAMPS);
    EF_VI_ASSERT(CI_QWORD_FIELD(event, EFCT_TX_EVENT_TIMESTAMP_STATUS) == 1);
    ptstamp = CI_QWORD_FIELD64(event, EFCT_TX_EVENT_PARTIAL_TSTAMP);
    ptstamp_seconds = ptstamp >> 32;
    timesync_seconds = (vi->ep_state->evq.sync_timestamp_major & 0xFF);
    ev_out->tx_timestamp.ts_sec = vi->ep_state->evq.sync_timestamp_major;
    if ( ptstamp_seconds == ((timesync_seconds + 1) % 256) ) {
      ev_out->tx_timestamp.ts_sec++;
    } 
    ev_out->tx_timestamp.ts_nsec = (ptstamp & 0xFFFFFFFF) >> vi->ts_subnano_bits;
    ev_out->tx_timestamp.ts_nsec &= ~EF_EVENT_TX_WITH_TIMESTAMP_SYNC_MASK;
    ev_out->tx_timestamp.ts_nsec |= vi->ep_state->evq.sync_flags;
    ev_out->tx_timestamp.type = EF_EVENT_TYPE_TX_WITH_TIMESTAMP;
    ev_out->tx_timestamp.rq_id = q->ids[(qs->previous - 1) & q->mask];
    ev_out->tx_timestamp.flags = EF_EVENT_FLAG_CTPIO;
    ev_out->tx_timestamp.q_id = CI_QWORD_FIELD(event, EFCT_TX_EVENT_LABEL);
    /* Delivering the tx event with timestamp counts as removing it, as we
     * must only be delivering a single event, so _unbundle isn't used. */
    qs->removed++;

  } else {
    ev_out->tx.type = EF_EVENT_TYPE_TX;
    ev_out->tx.desc_id = qs->previous;
    ev_out->tx.flags = EF_EVENT_FLAG_CTPIO;
    ev_out->tx.q_id = CI_QWORD_FIELD(event, EFCT_TX_EVENT_LABEL);
  }
}

int efct_ef_vi_transmit(ef_vi* vi, ef_addr base, int len,
                               ef_request_id dma_id)
{
  /* TODO need to avoid calling this with CTPIO fallback buffers */
  struct efct_tx_state tx;

  if( ! efct_tx_check(vi, len) )
    return -EAGAIN;

  efct_tx_init(vi, &tx);
  efct_tx_word(&tx, efct_tx_pkt_header(vi, len, EFCT_TX_CT_DISABLE));
  efct_tx_block(&tx, (void*)(uintptr_t)base, len);
  efct_tx_complete(vi, &tx, dma_id, len);

  return 0;
}

int efct_ef_vi_transmitv(ef_vi* vi, const ef_iovec* iov, int iov_len,
                                ef_request_id dma_id)
{
  struct efct_tx_state tx;
  int len = 0, i;

  efct_tx_init(vi, &tx);

  for( i = 0; i < iov_len; ++i )
    len += iov[i].iov_len;

  if( ! efct_tx_check(vi, len) )
    return -EAGAIN;

  efct_tx_word(&tx, efct_tx_pkt_header(vi, len, EFCT_TX_CT_DISABLE));

  for( i = 0; i < iov_len; ++i )
    efct_tx_block(&tx, (void*)(uintptr_t)iov[i].iov_base, iov[i].iov_len);

  efct_tx_complete(vi, &tx, dma_id, len);

  return 0;
}

void efct_ef_vi_transmit_push(ef_vi* vi)
{
}

static int efct_ef_vi_transmit_pio(ef_vi* vi, int offset, int len,
                                   ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_copy_pio(ef_vi* vi, int offset,
                                        const void* src_buf, int len,
                                        ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

static void efct_ef_vi_transmit_pio_warm(ef_vi* vi)
{
}

static void efct_ef_vi_transmit_copy_pio_warm(ef_vi* vi, int pio_offset,
                                              const void* src_buf, int len)
{
}

static bool tx_warm_active(ef_vi* vi)
{
  ci_qword_t qword;
  qword.u64[0] = vi->vi_txq.efct_fixed_header;
  return CI_QWORD_FIELD(qword, EFCT_TX_HEADER_WARM_FLAG);
}

#define EFCT_TX_POSTED_ID 0xefc7efc7
static void efct_ef_vi_transmitv_ctpio(ef_vi* vi, size_t len,
                                       const struct iovec* iov, int iovcnt,
                                       unsigned threshold)
{
  struct efct_tx_state tx;
  unsigned threshold_extra;
  int i;
  uint32_t dma_id;

  /* If we didn't have space then we must report this in _fallback and have
   * another go */
  vi->last_ctpio_failed = !efct_tx_check(vi, len);
  if(unlikely( vi->last_ctpio_failed ))
    return;
  efct_tx_init(vi, &tx);

  /* ef_vi interface takes threshold in bytes, but the efct hardware interface
   * takes multiples of 64 (rounded up), and includes the 8-byte header in the
   * count. Anything too big to fit in the field is equivalent to disabling
   * cut-through; test that first to avoid arithmetic overflow.
   */
  threshold_extra = EFCT_TX_HEADER_BYTES + EFCT_TX_ALIGNMENT - 1;
  if( threshold > EFCT_TX_CT_DISABLE * EFCT_TX_ALIGNMENT - threshold_extra )
    threshold = EFCT_TX_CT_DISABLE;
  else
    threshold = (threshold + threshold_extra) / EFCT_TX_ALIGNMENT;

  efct_tx_word(&tx, efct_tx_pkt_header(vi, len, threshold));

  for( i = 0; i < iovcnt; ++i )
    efct_tx_block(&tx, iov[i].iov_base, iov[i].iov_len);

  /* Use a valid but bogus dma_id rather than invalid EF_REQUEST_ID_MASK to
   * support tcpdirect, which relies on the correct return value from
   * ef_vi_transmit_unbundle to free its otherwise unused transmit buffers.
   *
   * For compat with existing ef_vi apps which will post a fallback and may
   * want to use the dma_id we'll replace this value with the real one then.
   *
   * For transmit warmup, use an invalid dma_id so that it is ignored.
   */
  dma_id = tx_warm_active(vi) ? EF_REQUEST_ID_MASK : EFCT_TX_POSTED_ID;
  efct_tx_complete(vi, &tx, dma_id, len);
}

static void efct_ef_vi_transmitv_ctpio_copy(ef_vi* vi, size_t frame_len,
                                            const struct iovec* iov, int iovcnt,
                                            unsigned threshold, void* fallback)
{
  int i;

  efct_ef_vi_transmitv_ctpio(vi, frame_len, iov, iovcnt, threshold);

  /* This could be made more efficient, if anyone cares enough */
  for( i = 0; i < iovcnt; ++i ) {
    memcpy(fallback, iov[i].iov_base, iov[i].iov_len);
    fallback = (char*)fallback + iov[i].iov_len;
  }
}

static inline int efct_ef_vi_ctpio_fallback(ef_vi* vi, ef_request_id dma_id)
{
  ef_vi_txq *q = &vi->vi_txq;
  ef_vi_txq_state* qs = &vi->ep_state->txq;
  unsigned di = (qs->added - 1) & q->mask;

  EF_VI_BUG_ON(qs->added == qs->removed);
  EF_VI_BUG_ON(q->ids[di] != EFCT_TX_POSTED_ID);
  q->ids[di] = dma_id;

  return 0;
}

static int efct_ef_vi_transmit_ctpio_fallback(ef_vi* vi, ef_addr dma_addr,
                                              size_t len, ef_request_id dma_id)
{
  if(unlikely( vi->last_ctpio_failed )) {
    int rc = efct_ef_vi_transmit(vi, dma_addr, len, dma_id);
    vi->last_ctpio_failed = rc == -EAGAIN;
    return rc;
  }
  return efct_ef_vi_ctpio_fallback(vi, dma_id);
}


static int efct_ef_vi_transmitv_ctpio_fallback(ef_vi* vi,
                                               const ef_iovec* dma_iov,
                                               int dma_iov_len,
                                               ef_request_id dma_id)
{
  if(unlikely( vi->last_ctpio_failed )) {
    int rc = efct_ef_vi_transmitv(vi, dma_iov, dma_iov_len, dma_id);
    vi->last_ctpio_failed = rc == -EAGAIN;
    return rc;
  }
  return efct_ef_vi_ctpio_fallback(vi, dma_id);
}

static int efct_ef_vi_transmit_alt_select(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_alt_select_default(ef_vi* vi)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_alt_stop(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_alt_go(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_receive_set_discards(ef_vi* vi, unsigned discard_err_flags)
{
  discard_err_flags &= EF_VI_DISCARD_RX_L4_CSUM_ERR |
                       EF_VI_DISCARD_RX_L3_CSUM_ERR |
                       EF_VI_DISCARD_RX_ETH_FCS_ERR |
                       EF_VI_DISCARD_RX_ETH_LEN_ERR |
                       EF_VI_DISCARD_RX_L2_CLASS_OTHER |
                       EF_VI_DISCARD_RX_L3_CLASS_OTHER |
                       EF_VI_DISCARD_RX_L4_CLASS_OTHER;

  vi->rx_discard_mask = discard_err_flags;
  return 0;
}

static uint64_t efct_ef_vi_receive_get_discards(ef_vi* vi)
{
  return vi->rx_discard_mask;
}

static int efct_ef_vi_transmit_alt_discard(ef_vi* vi, unsigned alt_id)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_receive_init(ef_vi* vi, ef_addr addr,
                                   ef_request_id dma_id)
{
  /* TODO X3 */
  return -ENOSYS;
}

static void efct_ef_vi_receive_push(ef_vi* vi)
{
  /* TODO X3 */
}

static int rx_rollover(ef_vi* vi, int qid)
{
  uint32_t pkt_id;
  uint32_t next;
  uint32_t superbuf_pkts = *vi->efct_rxqs.q[qid].live.superbuf_pkts;
  ef_vi_efct_rxq_ptr* rxq_ptr = &vi->ep_state->rxq.rxq_ptr[qid];
  unsigned sbseq;
  bool sentinel;
  struct efct_rx_descriptor* desc;

  int rc = vi->efct_rxqs.ops->next(vi, qid, &sentinel, &sbseq);
  if( rc < 0 )
    return rc;

  pkt_id = (qid * CI_EFCT_MAX_SUPERBUFS + rc) << PKT_ID_PKT_BITS;
  next = pkt_id | ((uint32_t)sentinel << 31);

  if( rxq_ptr->end == 0 ) {
    /* special case for when we want to ignore the first metadata, e.g. at
     * queue startup */
    rxq_ptr->prev = pkt_id;
    ++next;
  }
  else if( sbseq != (rxq_ptr->next >> 32) + 1 ) {
    /* nodescdrop on the swrxq. This is the same as the startup case, but it
     * also means that we're going to discard the last packet of the previous
     * superbuf */
    efct_vi_rxpkt_release(vi, rxq_ptr->prev);
    rxq_ptr->prev = pkt_id;
    ++next;
  }
  rxq_ptr->next = ((uint64_t)sbseq << 32) | next;
  rxq_ptr->end = pkt_id + superbuf_pkts;

  /* Preload the superbuf's refcount with all the (potential) packets in
   * it - more efficient than incrementing for each rx individually */
  EF_VI_ASSERT(superbuf_pkts > 0);
  EF_VI_ASSERT(superbuf_pkts < (1 << PKT_ID_PKT_BITS));
  desc = efct_rx_desc(vi, pkt_id);
  desc->refcnt = superbuf_pkts;
  desc->superbuf_pkts = superbuf_pkts;

  return 0;
}

static void efct_rx_discard(int qid, uint32_t pkt_id, uint16_t discard_flags,
                            const ci_oword_t* header, ef_event* ev)
{
  ev->rx_ref_discard.type = EF_EVENT_TYPE_RX_REF_DISCARD;
  ev->rx_ref_discard.len = CI_OWORD_FIELD(*header,
                                             EFCT_RX_HEADER_PACKET_LENGTH);
  ev->rx_ref_discard.pkt_id = pkt_id;
  ev->rx_ref_discard.q_id = qid;
  ev->rx_ref_discard.filter_id = CI_OWORD_FIELD(*header, EFCT_RX_HEADER_FILTER);
  ev->rx_ref_discard.user = CI_OWORD_FIELD(*header,
                                              EFCT_RX_HEADER_USER);
  ev->rx_ref_discard.flags = discard_flags;
}

static inline uint16_t header_status_flags(const ci_oword_t *header)
{
  uint16_t flags = 0;

  if ( CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L2_STATUS) ==
                      EFCT_RX_HEADER_L2_STATUS_FCS_ERR )
    flags |= EF_VI_DISCARD_RX_ETH_FCS_ERR;
  if ( CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L2_STATUS) ==
                      EFCT_RX_HEADER_L2_STATUS_LEN_ERR )
    flags |= EF_VI_DISCARD_RX_ETH_LEN_ERR;
  if ( (CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L3_CLASS) ==
                       EFCT_RX_HEADER_L3_CLASS_IP4) &&
       (header->u64[0] & M(L3_STATUS)) )
    flags |= EF_VI_DISCARD_RX_L3_CSUM_ERR;
  if ( (CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L3_CLASS) ==
                       EFCT_RX_HEADER_L3_CLASS_IP6) &&
       (header->u64[0] & M(L3_STATUS)) )
    flags |= EF_VI_DISCARD_RX_L3_CSUM_ERR;
  if ( (CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L4_CLASS) ==
                       EFCT_RX_HEADER_L4_CLASS_TCP) &&
       (header->u64[0] & M(L4_STATUS)) )
    flags |= EF_VI_DISCARD_RX_L4_CSUM_ERR;
  if ( (CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L4_CLASS) ==
                       EFCT_RX_HEADER_L4_CLASS_UDP) &&
       (header->u64[0] & M(L4_STATUS)) )
    flags |= EF_VI_DISCARD_RX_L4_CSUM_ERR;
  if ( (CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L4_CLASS) ==
                        EFCT_RX_HEADER_L4_CLASS_OTHER) )
      flags |= EF_VI_DISCARD_RX_L4_CLASS_OTHER;
  if ( (CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L3_CLASS) ==
                        EFCT_RX_HEADER_L3_CLASS_OTHER) )
      flags |= EF_VI_DISCARD_RX_L3_CLASS_OTHER;
  if ( (CI_OWORD_FIELD(*header, EFCT_RX_HEADER_L2_CLASS) ==
                        EFCT_RX_HEADER_L2_CLASS_OTHER) )
      flags |= EF_VI_DISCARD_RX_L2_CLASS_OTHER;
  return flags;
}

static inline int efct_poll_rx(ef_vi* vi, int qid, ef_event* evs, int evs_len)
{
  ef_vi_rxq_state* qs = &vi->ep_state->rxq;
  ef_vi_efct_rxq_ptr* rxq_ptr = &qs->rxq_ptr[qid];
  ef_vi_efct_rxq* rxq = &vi->efct_rxqs.q[qid];
  int i;

  if( efct_rxq_need_rollover(rxq_ptr) )
    if( rx_rollover(vi, qid) < 0 )
      /* ef_eventq_poll() has historically never been able to fail, so we
       * maintain that policy */
      return 0;

  if( efct_rxq_need_config(rxq) ) {
    unsigned new_generation = OO_ACCESS_ONCE(*rxq->live.config_generation);
    /* We have to use the live config_generation from before we started
     * thinking, to deal with multiple successive refreshes correctly, but we
     * must write it after we're done, to deal with concurrent calls to
     * efct_rxq_check_event() */
    if( vi->efct_rxqs.ops->refresh(vi, qid) < 0 ) {
#ifndef __KERNEL__
      /* Update rxq's value even if the refresh_func fails, since retrying it
       * every poll is unlikely to be productive either. Except in
       * kernelspace, since one of the possible outcomes is a crash and we
       * don't want that */
      rxq->config_generation = new_generation;
#endif
      return 0;
    }
    rxq->config_generation = new_generation;
  }

  /* Avoid crossing a superbuf in a single poll. Otherwise we'd need to check
   * for rollover after each packet. */
  evs_len = CI_MIN(evs_len, (int)(rxq_ptr->end -
                                  rxq_ptr_to_pkt_id(rxq_ptr->next)));

  for( i = 0; i < evs_len; ++i ) {
    const ci_oword_t* header;
    struct efct_rx_descriptor* desc;
    uint32_t pkt_id;
    uint16_t discard_flags = 0;

    header = efct_rx_next_header(vi, rxq_ptr->next);
    if( header == NULL )
      break;

    pkt_id = rxq_ptr->prev;
    desc = efct_rx_desc(vi, pkt_id);

    /* Do a course grained check first, then get rid of the false positives.*/
    if(unlikely( header->u64[0] & CHECK_FIELDS ) &&
       (header->u64[0] & M(ROLLOVER) ||
        (discard_flags = header_status_flags(header) & vi->rx_discard_mask)) ) {
      if( CI_OWORD_FIELD(*header, EFCT_RX_HEADER_ROLLOVER) ) {
        int prev_sb = pkt_id_to_local_superbuf_ix(pkt_id);
        int next_sb = pkt_id_to_local_superbuf_ix(rxq_ptr_to_pkt_id(rxq_ptr->next));
        int nskipped;
        if( next_sb == prev_sb ) {
          /* We created the desc->refcnt assuming that this superbuf would be
           * full of packets. It wasn't, so consume all the unused refs */
          nskipped = rxq_ptr->end - pkt_id;
        }
        else {
          /* i.e. the current packet is the one straddling a superbuf
           * boundary. We consume the last packet of the first superbuf
           * (it's the bogus 'manual rollover' packet) and the
           * entirety of the current superbuf, which is the one the NIC wants
           * to get rid of */
          nskipped = 1;
          vi->efct_rxqs.ops->free(vi, qid, next_sb);
        }

        EF_VI_ASSERT(nskipped > 0);
        EF_VI_ASSERT(nskipped <= desc->refcnt);
        desc->refcnt -= nskipped;
        if( desc->refcnt == 0 )
          vi->efct_rxqs.ops->free(vi, qid, prev_sb);

        /* Force a rollover on the next poll */
        rxq_ptr->end = 0;
        break;
      }

      efct_rx_discard(rxq->qid, pkt_id, discard_flags, header, &evs[i]);
    }
    else {
      /* For simplicity, require configuration for a fixed data offset.
       * Otherwise, we'd also have to check NEXT_FRAME_LOC in the previous
       * buffer. In theory the hardware could use variable offsets, but for now
       * we rely on knowing that the current implementation uses fixed offsets.
       */
      BUG_ON(CI_OWORD_FIELD(*header, EFCT_RX_HEADER_NEXT_FRAME_LOC) != 1);

      evs[i].rx_ref.type = EF_EVENT_TYPE_RX_REF;
      evs[i].rx_ref.len = CI_OWORD_FIELD(*header, EFCT_RX_HEADER_PACKET_LENGTH);
      evs[i].rx_ref.pkt_id = pkt_id;
      /* q_id should technically be set to the queue label, however currently
       * we don't allow the label to be changed so it's always the hardware
       * qid */
      evs[i].rx_ref.q_id = rxq->qid;
      evs[i].rx_ref.filter_id = CI_OWORD_FIELD(*header, EFCT_RX_HEADER_FILTER);
      evs[i].rx_ref.user = CI_OWORD_FIELD(*header, EFCT_RX_HEADER_USER);
    }

    /* This is only necessary for the final packet of each superbuf, storing
     * metadata from the next superbuf, but it may be faster to do it
     * unconditionally. */
    desc->final_timestamp = CI_OWORD_FIELD(*header, EFCT_RX_HEADER_TIMESTAMP);
    desc->final_ts_status = CI_OWORD_FIELD(*header,
                                           EFCT_RX_HEADER_TIMESTAMP_STATUS);

    rxq_ptr->prev = rxq_ptr_to_pkt_id(rxq_ptr->next++);
  }

  return i;
}

static void efct_tx_handle_error_event(ef_vi* vi, ci_qword_t event,
                                       ef_event* ev_out)
{
  ef_vi_txq_state* qs = &vi->ep_state->txq;

  /* If we get an error event then all that we'll get subsequently for this
   * TXQ is a flush, as the queue will be torn down. That means there's no
   * need to update any of our queue state tracking.
   */
  ev_out->tx_error.type = EF_EVENT_TYPE_TX_ERROR;
  ev_out->tx_error.q_id = CI_QWORD_FIELD(event, EFCT_ERROR_LABEL);
  ev_out->tx_error.flags = 0;
  ev_out->tx_error.desc_id = ++qs->previous;
  ev_out->tx_error.subtype = CI_QWORD_FIELD(event, EFCT_ERROR_REASON);
}

static int efct_tx_handle_control_event(ef_vi* vi, ci_qword_t event,
                                        ef_event* ev_out)
{
  uint8_t time_sync;
  uint8_t time_set;
  int n_evs = 0;

  switch( CI_QWORD_FIELD(event, EFCT_CTRL_SUBTYPE) ) {
    case EFCT_CTRL_EV_ERROR:
      efct_tx_handle_error_event(vi, event, ev_out);
      n_evs++;
      ef_log("%s: ERROR: MCDI TX error event %u (raw: "CI_QWORD_FMT") - "
             "check parameters to transmit_init()", __FUNCTION__,
             QWORD_GET_U(EFCT_ERROR_REASON, event), CI_QWORD_VAL(event));
      break;
    case EFCT_CTRL_EV_FLUSH:
      LOG(ef_log("%s: Saw flush in poll", __FUNCTION__));
      break;
    case EFCT_CTRL_EV_TIME_SYNC:
      vi->ep_state->evq.sync_timestamp_major = CI_QWORD_FIELD64(event, EFCT_TIME_SYNC_EVENT_TIME_HIGH) >> 16;
      vi->ep_state->evq.sync_timestamp_minor = CI_QWORD_FIELD64(event, EFCT_TIME_SYNC_EVENT_TIME_HIGH) & 0xFFFF;
      time_sync = (CI_QWORD_FIELD(event, EFCT_TIME_SYNC_EVENT_CLOCK_IN_SYNC) ? EF_VI_SYNC_FLAG_CLOCK_IN_SYNC : 0);
      time_set = (CI_QWORD_FIELD(event, EFCT_TIME_SYNC_EVENT_CLOCK_IS_SET) ? EF_VI_SYNC_FLAG_CLOCK_SET : 0);
      vi->ep_state->evq.sync_flags = time_sync | time_set;
      vi->ep_state->evq.unsol_credit_seq++;
      efct_grant_unsol_credit(vi, false, vi->ep_state->evq.unsol_credit_seq);
      break;
    case EFCT_CTRL_EV_UNSOL_OVERFLOW:
      LOG(ef_log("%s: Saw unsol overflow", __FUNCTION__));
      /* Set unsol_seq to default, but leave 1 credit-space in reserve for overflow event. */
      vi->ep_state->evq.unsol_credit_seq = CI_CFG_TIME_SYNC_EVENT_EVQ_CAPACITY - 1;
      efct_grant_unsol_credit(vi, true, vi->ep_state->evq.unsol_credit_seq);
      break;
  }

  return n_evs;
}

int efct_poll_tx(ef_vi* vi, ef_event* evs, int evs_len)
{
  ef_eventq_state* evq = &vi->ep_state->evq;
  ci_qword_t* event;
  int n_evs = 0;

  /* Check for overflow. If the previous entry has been overwritten already,
   * then it will have the wrong phase value and will appear invalid */
  BUG_ON(efct_tx_get_event(vi, evq->evq_ptr - sizeof(*event)) == NULL);

  while( n_evs < evs_len ) {
    event = efct_tx_get_event(vi, evq->evq_ptr);
    if( event == NULL )
      break;
    evq->evq_ptr += sizeof(*event);

    switch( CI_QWORD_FIELD(*event, EFCT_EVENT_TYPE) ) {
      case EFCT_EVENT_TYPE_TX:
        efct_tx_handle_event(vi, *event, &evs[n_evs]);
        n_evs++;
        /* Don't report more than one tx event per poll. This is to avoid a
         * horrendous sequencing problem if a simple TX event is followed by a
         * TX_WITH_TIMESTAMP; we'd need to update the queue state for the
         * second event *after* the later call to ef_vi_transmit_unbundle()
         * for the first event. */
        return n_evs;
      case EFCT_EVENT_TYPE_CONTROL:
        n_evs += efct_tx_handle_control_event(vi, *event, &evs[n_evs]);
        break;
      default:
        ef_log("%s:%d: ERROR: event="CI_QWORD_FMT,
               __FUNCTION__, __LINE__, CI_QWORD_VAL(*event));
        break;
    }
  }

  return n_evs;
}

static int efct_ef_eventq_poll(ef_vi* vi, ef_event* evs, int evs_len)
{
  int n = 0;
  uint64_t qs = *vi->efct_rxqs.active_qs;
  for ( ; ; ) {
    int i = __builtin_ffsll(qs);
    if (i == 0)
      break;
    --i;
    qs &= ~(1ull << i);
    n += efct_poll_rx(vi, i, evs + n, evs_len - n);
  }
  if( vi->vi_txq.mask )
    n += efct_poll_tx(vi, evs + n, evs_len - n);
  return n;
}

static void efct_ef_eventq_prime(ef_vi* vi)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_prime(ef_vi* vi, unsigned v)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_run(ef_vi* vi, unsigned v)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_clear(ef_vi* vi)
{
  /* TODO X3 */
}

static void efct_ef_eventq_timer_zero(ef_vi* vi)
{
  /* TODO X3 */
}

static ssize_t efct_ef_vi_transmit_memcpy(struct ef_vi* vi,
                                          const ef_remote_iovec* dst_iov,
                                          int dst_iov_len,
                                          const ef_remote_iovec* src_iov,
                                          int src_iov_len)
{
  return -EOPNOTSUPP;
}

static int efct_ef_vi_transmit_memcpy_sync(struct ef_vi* vi,
                                           ef_request_id dma_id)
{
  return -EOPNOTSUPP;
}

int efct_vi_find_free_rxq(ef_vi* vi, int qid)
{
  int ix;

  for( ix = 0; ix < vi->efct_rxqs.max_qs; ++ix ) {
    if( vi->efct_rxqs.q[ix].qid == qid )
      return -EALREADY;
    if( ! efct_rxq_is_active(&vi->efct_rxqs.q[ix]) )
      return ix;
  }
  return -ENOSPC;
}

void efct_vi_start_rxq(ef_vi* vi, int ix, int qid)
{
  vi->efct_rxqs.q[ix].qid = qid;
  vi->efct_rxqs.q[ix].config_generation = 0;
  vi->ep_state->rxq.rxq_ptr[ix].end = 0;
}

static int
efct_design_parameters(struct ef_vi* vi, struct efab_nic_design_parameters* dp)
{
#define GET(PARAM) EFAB_NIC_DP_GET(*dp, PARAM)

  /* Some values which are used on the critical path which we don't expect to
   * change are hard-coded. We need to check these values, and will need to
   * accommodate run-time values if the parameter ever does change.
   */

  /* If the superbuf size changes, we will need to use it as a runtime value,
   * replacing EFCT_RX_SUPERBUF_BYTES and its dependent values */
  if( GET(rx_superbuf_bytes) != EFCT_RX_SUPERBUF_BYTES ) {
    LOG(ef_log("%s: unsupported rx_superbuf_bytes, %ld != %d", __FUNCTION__,
               (long)GET(rx_superbuf_bytes), EFCT_RX_SUPERBUF_BYTES));
    return -EOPNOTSUPP;
  }

  /* If the frame offset changes or is no longer fixed, we will need to
   * update efct_vi_rxpkt_get (and duplicated code in efct_vi_rx_future_peek).
   * It could use the parameter if it is still fixed, or read from the header.
   */
  if( GET(rx_frame_offset) != EFCT_RX_HEADER_NEXT_FRAME_LOC_1 - 2) {
    LOG(ef_log("%s: unsupported rx_frame_offset, %ld != %d", __FUNCTION__,
               (long)GET(rx_frame_offset), EFCT_RX_HEADER_NEXT_FRAME_LOC_1 - 2));
    return -EOPNOTSUPP;
  }

  /* When writing to the aperture we use a bitmask to keep within range. This
   * requires the size a power of two, and we shift by 3 because we write
   * a uint64_t (8 bytes) at a time.
   */
  if( ! EF_VI_IS_POW2(GET(tx_aperture_bytes)) ) {
    LOG(ef_log("%s: unsupported tx_aperture_bytes, %ld not a power of 2",
               __FUNCTION__, (long)GET(tx_aperture_bytes)));
    return -EOPNOTSUPP;
  }
  vi->vi_txq.efct_aperture_mask = (GET(tx_aperture_bytes) - 1) >> 3;

  /* FIFO size, reduced by 8 bytes for the TX header. Hardware reduces this
   * by one cache line to make their overflow tracking easier */
  vi->vi_txq.ct_fifo_bytes = GET(tx_fifo_bytes) -
                             EFCT_TX_ALIGNMENT - EFCT_TX_HEADER_BYTES;
  vi->ts_subnano_bits = GET(timestamp_subnano_bits);
  vi->unsol_credit_seq_mask = GET(unsol_credit_seq_mask);

  return 0;
}

static int efct_post_filter_add(struct ef_vi* vi,
                                const ef_filter_spec* fs,
                                const ef_filter_cookie* cookie, int rxq)
{
#ifdef __KERNEL__
  return 0; /* EFCT TODO */
#else
  int rc;
  unsigned n_superbufs;

   /* Block filters don't attach to an RXQ */
  if( ef_vi_filter_is_block_only(cookie) )
    return 0;

  EF_VI_ASSERT(rxq >= 0);
  n_superbufs = CI_ROUND_UP((vi->vi_rxq.mask + 1) * EFCT_PKT_STRIDE,
                            EFCT_RX_SUPERBUF_BYTES) / EFCT_RX_SUPERBUF_BYTES;
  rc = vi->efct_rxqs.ops->attach(vi, rxq, n_superbufs);
  if( rc == -EALREADY )
    rc = 0;
  return rc;
#endif
}

const void* efct_vi_rxpkt_get(ef_vi* vi, uint32_t pkt_id)
{
  EF_VI_ASSERT(vi->nic_type.arch == EF_VI_ARCH_EFCT);

  /* assume DP_FRAME_OFFSET_FIXED (correct for initial hardware) */
  return (char*)efct_rx_header(vi, pkt_id) + EFCT_RX_HEADER_NEXT_FRAME_LOC_1;
}

void efct_vi_rxpkt_release(ef_vi* vi, uint32_t pkt_id)
{
  EF_VI_ASSERT(efct_rx_desc(vi, pkt_id)->refcnt > 0);

  if( --efct_rx_desc(vi, pkt_id)->refcnt == 0 )
    vi->efct_rxqs.ops->free(vi, pkt_id_to_rxq_ix(pkt_id),
                            pkt_id_to_local_superbuf_ix(pkt_id));
}

const void* efct_vi_rx_future_peek(ef_vi* vi)
{
  uint64_t qs = *vi->efct_rxqs.active_qs;
  for( ; CI_LIKELY( qs ); qs &= (qs - 1) ) {
    unsigned qid = __builtin_ctzll(qs);
    ef_vi_efct_rxq_ptr* rxq_ptr = &vi->ep_state->rxq.rxq_ptr[qid];
    unsigned pkt_id;

    /* Skip queues that have pending non-packet related work
     * The work will be picked up by poll or noticed by efct_rxq_check_event */
    if( efct_rxq_need_rollover(rxq_ptr)  ||
        efct_rxq_need_config(&vi->efct_rxqs.q[qid]) )
      continue;
    pkt_id = OO_ACCESS_ONCE(rxq_ptr->prev);
    EF_VI_ASSERT(pkt_id < rxq_ptr->end);
    {
      const char* start = (char*)efct_rx_header(vi, pkt_id) +
                          EFCT_RX_HEADER_NEXT_FRAME_LOC_1;
      uint64_t v = *(volatile uint64_t*)(start - 2);
      if(CI_LIKELY( v != CI_EFCT_DEFAULT_POISON )) {
        vi->future_qid = qid;
        return start;
      }
    }
  }
  return NULL;
}

int efct_vi_rx_future_poll(ef_vi* vi, ef_event* evs, int evs_len)
{
  int count;

  EF_VI_ASSERT(((ci_int8) vi->future_qid) >= 0);
  EF_VI_ASSERT(efct_rxq_is_active(&vi->efct_rxqs.q[vi->future_qid]));
  count = efct_poll_rx(vi, vi->future_qid, evs, evs_len);
#ifndef NDEBUG
  if( count )
    vi->future_qid = -1;
#endif
  return count;
}

int efct_ef_eventq_check_event(const ef_vi* vi)
{
  return efct_tx_check_event(vi) || efct_rx_check_event(vi);
}

unsigned efct_vi_next_rx_rq_id(ef_vi* vi, int qid)
{
  if( efct_rxq_need_config(&vi->efct_rxqs.q[qid]) )
    return ~0u;
  return vi->ep_state->rxq.rxq_ptr[qid].prev;
}

int efct_vi_rxpkt_get_timestamp(ef_vi* vi, uint32_t pkt_id,
                                ef_timespec* ts_out, unsigned* flags_out)
{
  const struct efct_rx_descriptor* desc = efct_rx_desc(vi, pkt_id);
  uint64_t ts;
  unsigned status;
  ci_qword_t time_sync;

  time_sync.u64[0] =
    OO_ACCESS_ONCE(*vi->efct_rxqs.q[pkt_id_to_rxq_ix(pkt_id)].live.time_sync);

  if( pkt_id_to_index_in_superbuf(pkt_id) == desc->superbuf_pkts - 1 ) {
    ts = desc->final_timestamp;
    status = desc->final_ts_status;
  }
  else {
    const ci_oword_t* header = efct_rx_header(vi, pkt_id + 1);
    ts = CI_OWORD_FIELD(*header, EFCT_RX_HEADER_TIMESTAMP);
    status = CI_OWORD_FIELD(*header, EFCT_RX_HEADER_TIMESTAMP_STATUS);
  }

  if( status != 1 )
    return -ENODATA;

  ts_out->tv_sec = ts >> 32;
  ts_out->tv_nsec = (uint32_t)ts >> 2;
  *flags_out =
    (CI_QWORD_FIELD(time_sync, EFCT_TIME_SYNC_CLOCK_IS_SET) ?
      EF_VI_SYNC_FLAG_CLOCK_SET : 0) |
    (CI_QWORD_FIELD(time_sync, EFCT_TIME_SYNC_CLOCK_IN_SYNC) ?
      EF_VI_SYNC_FLAG_CLOCK_IN_SYNC : 0);
  return 0;
}

int efct_vi_get_wakeup_params(ef_vi* vi, int qid, unsigned* sbseq,
                              unsigned* pktix)
{
  ef_vi_rxq_state* qs = &vi->ep_state->rxq;
  ef_vi_efct_rxq_ptr* rxq_ptr = &qs->rxq_ptr[qid];
  ef_vi_efct_rxq* rxq = &vi->efct_rxqs.q[qid];
  uint64_t sbseq_next;
  unsigned ix;

  if( ! efct_rxq_is_active(rxq) )
    return -ENOENT;

  sbseq_next = OO_ACCESS_ONCE(rxq_ptr->next);
  ix = pkt_id_to_index_in_superbuf(sbseq_next);

  if( ix >= *rxq->live.superbuf_pkts ) {
    *sbseq = (sbseq_next >> 32) + 1;
    *pktix = 0;
  }
  else {
    *sbseq = sbseq_next >> 32;
    *pktix = ix;
  }
  return 0;
}

void efct_vi_start_transmit_warm(ef_vi* vi)
{
  ci_qword_t qword;
  qword.u64[0] = vi->vi_txq.efct_fixed_header;

  EF_VI_ASSERT(vi->nic_type.arch == EF_VI_ARCH_EFCT);
  EF_VI_ASSERT(CI_QWORD_FIELD(qword, EFCT_TX_HEADER_WARM_FLAG) == 0);

  CI_SET_QWORD_FIELD(qword, EFCT_TX_HEADER_WARM_FLAG, 1);
  vi->vi_txq.efct_fixed_header = qword.u64[0];
}

void efct_vi_stop_transmit_warm(ef_vi* vi)
{
  ci_qword_t qword;
  qword.u64[0] = vi->vi_txq.efct_fixed_header;

  EF_VI_ASSERT(vi->nic_type.arch == EF_VI_ARCH_EFCT);
  EF_VI_ASSERT(CI_QWORD_FIELD(qword, EFCT_TX_HEADER_WARM_FLAG) == 1);

  CI_SET_QWORD_FIELD(qword, EFCT_TX_HEADER_WARM_FLAG, 0);
  vi->vi_txq.efct_fixed_header = qword.u64[0];
}

static void efct_vi_initialise_ops(ef_vi* vi)
{
  vi->ops.transmit               = efct_ef_vi_transmit;
  vi->ops.transmitv              = efct_ef_vi_transmitv;
  vi->ops.transmitv_init         = efct_ef_vi_transmitv;
  vi->ops.transmit_push          = efct_ef_vi_transmit_push;
  vi->ops.transmit_pio           = efct_ef_vi_transmit_pio;
  vi->ops.transmit_copy_pio      = efct_ef_vi_transmit_copy_pio;
  vi->ops.transmit_pio_warm      = efct_ef_vi_transmit_pio_warm;
  vi->ops.transmit_copy_pio_warm = efct_ef_vi_transmit_copy_pio_warm;
  vi->ops.transmitv_ctpio        = efct_ef_vi_transmitv_ctpio;
  vi->ops.transmitv_ctpio_copy   = efct_ef_vi_transmitv_ctpio_copy;
  vi->ops.transmit_alt_select    = efct_ef_vi_transmit_alt_select;
  vi->ops.transmit_alt_select_default = efct_ef_vi_transmit_alt_select_default;
  vi->ops.transmit_alt_stop      = efct_ef_vi_transmit_alt_stop;
  vi->ops.transmit_alt_go        = efct_ef_vi_transmit_alt_go;
  vi->ops.receive_set_discards   = efct_ef_vi_receive_set_discards;
  vi->ops.receive_get_discards   = efct_ef_vi_receive_get_discards;
  vi->ops.transmit_alt_discard   = efct_ef_vi_transmit_alt_discard;
  vi->ops.receive_init           = efct_ef_vi_receive_init;
  vi->ops.receive_push           = efct_ef_vi_receive_push;
  vi->ops.eventq_prime           = efct_ef_eventq_prime;
  vi->ops.eventq_timer_prime     = efct_ef_eventq_timer_prime;
  vi->ops.eventq_timer_run       = efct_ef_eventq_timer_run;
  vi->ops.eventq_timer_clear     = efct_ef_eventq_timer_clear;
  vi->ops.eventq_timer_zero      = efct_ef_eventq_timer_zero;
  vi->ops.transmit_memcpy        = efct_ef_vi_transmit_memcpy;
  vi->ops.transmit_memcpy_sync   = efct_ef_vi_transmit_memcpy_sync;
  vi->ops.transmit_ctpio_fallback = efct_ef_vi_transmit_ctpio_fallback;
  vi->ops.transmitv_ctpio_fallback = efct_ef_vi_transmitv_ctpio_fallback;
  vi->internal_ops.design_parameters = efct_design_parameters;
  vi->internal_ops.post_filter_add = efct_post_filter_add;
  vi->ops.eventq_poll = efct_ef_eventq_poll;
}

void efct_vi_init(ef_vi* vi)
{
  int i;
  EF_VI_BUILD_ASSERT(sizeof(struct efct_tx_descriptor) ==
                     EFCT_TX_DESCRIPTOR_BYTES);
  EF_VI_BUILD_ASSERT(sizeof(struct efct_rx_descriptor) ==
                     EFCT_RX_DESCRIPTOR_BYTES);
  EF_VI_ASSERT( vi->nic_type.nic_flags & EFHW_VI_NIC_CTPIO_ONLY );

  efct_vi_initialise_ops(vi);
  vi->evq_phase_bits = 1;
  /* Set default rx_discard_mask for EFCT */
  vi->rx_discard_mask = (
     EF_VI_DISCARD_RX_L4_CSUM_ERR |
     EF_VI_DISCARD_RX_L3_CSUM_ERR |
     EF_VI_DISCARD_RX_ETH_FCS_ERR |
     EF_VI_DISCARD_RX_ETH_LEN_ERR
  );

  vi->vi_txq.efct_fixed_header =
      efct_tx_header(0, 0, (vi->vi_flags & EF_VI_TX_TIMESTAMPS) ? 1 : 0, 0, 0);
  for( i = 0; i < CI_ARRAY_SIZE(vi->ep_state->rxq.sb_desc_free_head); i++ )
    vi->ep_state->rxq.sb_desc_free_head[i] = -1;

  vi->efct_rxqs.active_qs = &vi->efct_rxqs.max_qs;
  for( i = 0; i < EF_VI_MAX_EFCT_RXQS; ++i )
    vi->efct_rxqs.q[i].live.superbuf_pkts = &vi->efct_rxqs.q[i].config_generation;
}
