/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/* Stand-in for liblinux-head-unit.so: exports the 3 real mangled virtuals and a vtable
 * that references them by symbol (R_X86_64_64 relocs), like the vendor lib. */
#include <stdio.h>
#define S "_ZN24NavigationStatusCallback"
void _ZN24NavigationStatusCallback24navigationStatusCallbackEi(void *s, int st){ printf("ORIG status %d\n", st); }
void _ZN24NavigationStatusCallback34navigationNextTurnDistanceCallbackEiii45NavigationNextTurnDistanceEvent_DistanceUnits(void *s,int m,int t,int d,int u){ printf("ORIG dist %d\n", m); }
void _ZN24NavigationStatusCallback26navigationNextTurnCallbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEiiS7_ii(void *s,const void *r,int a,int b,const void *i,int c,int d){ printf("ORIG turn side=%d ev=%d ang=%d\n", a,b,c); }
void *aa_vtable[3] = {
  (void*)_ZN24NavigationStatusCallback24navigationStatusCallbackEi,
  (void*)_ZN24NavigationStatusCallback34navigationNextTurnDistanceCallbackEiii45NavigationNextTurnDistanceEvent_DistanceUnits,
  (void*)_ZN24NavigationStatusCallback26navigationNextTurnCallbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEiiS7_ii };
