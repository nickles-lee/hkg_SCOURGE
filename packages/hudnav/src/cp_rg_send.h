/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * CarPlay route guidance: "send StartRouteGuidanceUpdates (0x5200)" step for the iAP2Service-2.0 patch library.
 *
 * Logic only; the vendor functions are injected (cp_rg_vendor) so the whole step is host-testable with fakes.
 * The real table is built from the executable base in cp_rg.c (offsets for iAP2Service-2.0
 * sha256 e663d5129177a8bd..., verified in docs/hudnav/CP_PROTOCOL.md):
 *   encode   = base+0x1d0a0  int  (int svc, int msgid, vec *ids, vec *ptrs, vec *lens, uchar **out)  -> length
 *   pkt_ctor = base+0x1abd6  void (pkt *p, const uchar *data, int len)   copies data into a packet-owned buffer
 *   send     = base+0x152de  long (void *self, pkt *p, int session, int flag)   (flag 0 as the stock senders)
 *   pkt_dtor = base+0x1ad46  void (pkt *p)
 *   free_arr = base+0x4ef0   operator delete[] (PLT)           frees the encoder's `out` buffer, as stock does
 * Exactly the sequence of the stock sender sendIdentificationInformation (0x204e6): encode -> pkt_ctor ->
 * send(self, pkt, session, 0) -> free out -> pkt_dtor. `self` and `session` are the first and last arguments
 * of the receive dispatcher handleReceivedData (0x13afa), so the send runs in the stock reply context.
 */
#ifndef CP_RG_SEND_H
#define CP_RG_SEND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  (*encode)(int svc, int msgid, void *ids, void *ptrs, void *lens, unsigned char **out);
    void (*pkt_ctor)(void *pkt, const unsigned char *data, int len);
    long (*send)(void *self, void *pkt, int session, int flag);
    void (*pkt_dtor)(void *pkt);
    void (*free_arr)(void *p);
} cp_rg_vendor;

typedef struct {
    const cp_rg_vendor *v;
    int ident_patched;            /* set by the Identification wrapper once ids 6/7 + param 30 were added */
    int with_component_id;        /* experiment: add param 0 = 42 (component Identifier) to 0x5200 */
    unsigned char sent[256];      /* per link session id: 0x5200 already sent */
    int sends;                    /* number of 0x5200 sends attempted (diagnostics) */
    int last_rc;                  /* last send() return value */
} cp_rg_send_ctx;

#define CP_MSG_START_IDENT   0x1d00   /* phone: StartIdentification (new connection) */
#define CP_MSG_AUTH_OK       0xaa05   /* phone: AuthenticationSucceeded */
#define CP_MSG_START_RGU     0x5200   /* accessory: StartRouteGuidanceUpdates */

/* Message id of an iAP2 control message `40 40 len(BE16) id(BE16) ...`, or -1 if not a well formed header
 * (same validation idea as the stock 0x1dcae: header + length + at least the 6 header bytes). */
int cp_rg_msg_id(const unsigned char *d, int n);

/* Zero the context and set the vendor table. */
void cp_rg_send_init(cp_rg_send_ctx *c, const cp_rg_vendor *v);

/* Call AFTER the stock dispatcher has handled the received message d[0..n) of link session `session`.
 * Sends 0x5200 once per session when the message is AuthenticationSucceeded and the Identification was patched.
 * 0x1d00 (a new Identification round) clears the "sent" flag of that session.
 * Returns 1 if a 0x5200 was sent in this call, else 0. Never sends anything else. */
int cp_rg_after_dispatch(cp_rg_send_ctx *c, void *self, const unsigned char *d, int n, int session);

/* Send 0x5200 now (building the three empty/1-element vectors by hand, no allocation of our own).
 * Returns the vendor send() return value, or -1 if the encoder produced nothing. */
long cp_rg_send_start(cp_rg_send_ctx *c, void *self, int session);

#ifdef __cplusplus
}
#endif
#endif
