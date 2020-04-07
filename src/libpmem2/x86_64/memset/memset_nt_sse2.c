// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2017-2020, Intel Corporation */

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

#include "pmem2_arch.h"
#include "flush.h"
#include "memcpy_memset.h"
#include "memset_sse2.h"
#include "out.h"
#include "valgrind_internal.h"

static force_inline void
memset_movnt4x64b(char *dest, __m128i xmm)
{
	_mm_stream_si128((__m128i *)dest + 0, xmm);
	_mm_stream_si128((__m128i *)dest + 1, xmm);
	_mm_stream_si128((__m128i *)dest + 2, xmm);
	_mm_stream_si128((__m128i *)dest + 3, xmm);
	_mm_stream_si128((__m128i *)dest + 4, xmm);
	_mm_stream_si128((__m128i *)dest + 5, xmm);
	_mm_stream_si128((__m128i *)dest + 6, xmm);
	_mm_stream_si128((__m128i *)dest + 7, xmm);
	_mm_stream_si128((__m128i *)dest + 8, xmm);
	_mm_stream_si128((__m128i *)dest + 9, xmm);
	_mm_stream_si128((__m128i *)dest + 10, xmm);
	_mm_stream_si128((__m128i *)dest + 11, xmm);
	_mm_stream_si128((__m128i *)dest + 12, xmm);
	_mm_stream_si128((__m128i *)dest + 13, xmm);
	_mm_stream_si128((__m128i *)dest + 14, xmm);
	_mm_stream_si128((__m128i *)dest + 15, xmm);
}

static force_inline void
memset_movnt2x64b(char *dest, __m128i xmm)
{
	_mm_stream_si128((__m128i *)dest + 0, xmm);
	_mm_stream_si128((__m128i *)dest + 1, xmm);
	_mm_stream_si128((__m128i *)dest + 2, xmm);
	_mm_stream_si128((__m128i *)dest + 3, xmm);
	_mm_stream_si128((__m128i *)dest + 4, xmm);
	_mm_stream_si128((__m128i *)dest + 5, xmm);
	_mm_stream_si128((__m128i *)dest + 6, xmm);
	_mm_stream_si128((__m128i *)dest + 7, xmm);
}

static force_inline void
memset_movnt1x64b(char *dest, __m128i xmm)
{
	_mm_stream_si128((__m128i *)dest + 0, xmm);
	_mm_stream_si128((__m128i *)dest + 1, xmm);
	_mm_stream_si128((__m128i *)dest + 2, xmm);
	_mm_stream_si128((__m128i *)dest + 3, xmm);
}

static force_inline void
memset_movnt1x32b(char *dest, __m128i xmm)
{
	_mm_stream_si128((__m128i *)dest + 0, xmm);
	_mm_stream_si128((__m128i *)dest + 1, xmm);
}

static force_inline void
memset_movnt1x16b(char *dest, __m128i xmm)
{
	_mm_stream_si128((__m128i *)dest, xmm);
}

static force_inline void
memset_movnt1x8b(char *dest, __m128i xmm)
{
	uint64_t x = (uint64_t)_mm_cvtsi128_si64(xmm);

	_mm_stream_si64((long long *)dest, (long long)x);
}

static force_inline void
memset_movnt1x4b(char *dest, __m128i xmm)
{
	uint32_t x = (uint32_t)_mm_cvtsi128_si32(xmm);

	_mm_stream_si32((int *)dest, (int)x);
}

static force_inline void
memset_movnt_sse2(char *dest, int c, size_t len, flush_fn flush,
		barrier_fn barrier)
{
	char *orig_dest = dest;
	size_t orig_len = len;

	__m128i xmm = _mm_set1_epi8((char)c);

	size_t cnt = (uint64_t)dest & 63;
	if (cnt > 0) {
		cnt = 64 - cnt;

		if (cnt > len)
			cnt = len;

		memset_small_sse2(dest, xmm, cnt, flush);

		dest += cnt;
		len -= cnt;
	}

	while (len >= 4 * 64) {
		memset_movnt4x64b(dest, xmm);
		dest += 4 * 64;
		len -= 4 * 64;
	}

	if (len >= 2 * 64) {
		memset_movnt2x64b(dest, xmm);
		dest += 2 * 64;
		len -= 2 * 64;
	}

	if (len >= 1 * 64) {
		memset_movnt1x64b(dest, xmm);

		dest += 1 * 64;
		len -= 1 * 64;
	}

	if (len == 0)
		goto end;

	/* There's no point in using more than 1 nt store for 1 cache line. */
	if (util_is_pow2(len)) {
		if (len == 32)
			memset_movnt1x32b(dest, xmm);
		else if (len == 16)
			memset_movnt1x16b(dest, xmm);
		else if (len == 8)
			memset_movnt1x8b(dest, xmm);
		else if (len == 4)
			memset_movnt1x4b(dest, xmm);
		else
			goto nonnt;

		goto end;
	}

nonnt:
	memset_small_sse2(dest, xmm, len, flush);
end:
	barrier();

	VALGRIND_DO_FLUSH(orig_dest, orig_len);
}

void
memset_movnt_sse2_noflush(char *dest, int c, size_t len)
{
	LOG(15, "dest %p c %d len %zu", dest, c, len);

	memset_movnt_sse2(dest, c, len, noflush, barrier_after_ntstores);
}

void
memset_movnt_sse2_empty(char *dest, int c, size_t len)
{
	LOG(15, "dest %p c %d len %zu", dest, c, len);

	memset_movnt_sse2(dest, c, len, flush_empty_nolog,
			barrier_after_ntstores);
}

void
memset_movnt_sse2_clflush(char *dest, int c, size_t len)
{
	LOG(15, "dest %p c %d len %zu", dest, c, len);

	memset_movnt_sse2(dest, c, len, flush_clflush_nolog,
			barrier_after_ntstores);
}

void
memset_movnt_sse2_clflushopt(char *dest, int c, size_t len)
{
	LOG(15, "dest %p c %d len %zu", dest, c, len);

	memset_movnt_sse2(dest, c, len, flush_clflushopt_nolog,
			no_barrier_after_ntstores);
}

void
memset_movnt_sse2_clwb(char *dest, int c, size_t len)
{
	LOG(15, "dest %p c %d len %zu", dest, c, len);

	memset_movnt_sse2(dest, c, len, flush_clwb_nolog,
			no_barrier_after_ntstores);
}
