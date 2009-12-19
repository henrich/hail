#ifndef __CLD_COMMON_H__
#define __CLD_COMMON_H__

/*
 * Copyright 2009 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdint.h>

unsigned long long cld_sid2llu(const uint8_t *sid);
void __cld_rand64(void *p);
const char *cld_errstr(enum cle_err_codes ecode);
int cld_readport(const char *fname);

#endif /* __CLD_COMMON_H__ */
