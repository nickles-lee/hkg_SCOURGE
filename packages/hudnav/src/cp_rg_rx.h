/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef HUDNAV_CP_RG_RX_H
#define HUDNAV_CP_RG_RX_H

/* Feed the bytes returned by the one phone-link reader, in read order. */
void cp_rx_feed(const unsigned char *buf, long n);

/* A whole control message from the stock dispatcher (no link layer). */
void cp_rx_feed_message(int session, const unsigned char *msg, long n);

#endif
