/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#include <stdio.h>
extern void *aa_vtable[3];
int main(void){
  ((void(*)(void*,int))aa_vtable[0])(0,1);
  ((void(*)(void*,int,int,int,int))aa_vtable[1])(0,120,10,120000,0);
  ((void(*)(void*,const void*,int,int,const void*,int,int))aa_vtable[2])(0,0,2,3,0,90,1);
  fflush(stdout); return 0; }
