// Brand new utf8 validation algorithm

#ifdef __x86_64__

#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>

/*
 * http://www.unicode.org/versions/Unicode6.0.0/ch03.pdf - page 94
 *
 * Table 3-7. Well-Formed UTF-8 Byte Sequences
 *
 * +--------------------+------------+-------------+------------+-------------+
 * | Code Points        | First Byte | Second Byte | Third Byte | Fourth Byte |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+0000..U+007F     | 00..7F     |             |            |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+0080..U+07FF     | C2..DF     | 80..BF      |            |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+0800..U+0FFF     | E0         | A0..BF      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+1000..U+CFFF     | E1..EC     | 80..BF      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+D000..U+D7FF     | ED         | 80..9F      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+E000..U+FFFF     | EE..EF     | 80..BF      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+10000..U+3FFFF   | F0         | 90..BF      | 80..BF     | 80..BF      |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+40000..U+FFFFF   | F1..F3     | 80..BF      | 80..BF     | 80..BF      |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+100000..U+10FFFF | F4         | 80..8F      | 80..BF     | 80..BF      |
 * +--------------------+------------+-------------+------------+-------------+
 */

int utf8_naive(const unsigned char *data, int len);

#if 0
static void print128(const char *s, const __m128i v128)
{
  const unsigned char *v8 = (const unsigned char *)&v128;
  if (s)
    printf("%s:\t", s);
  for (int i = 0; i < 16; i++)
    printf("%02x ", v8[i]);
  printf("\n");
}
#endif

struct previous_input {
    __m128i input;
    __m128i follow_bytes;
};

static const int8_t _follow_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00 ~ BF */
    1, 1,                               /* C0 ~ DF */
    2,                                  /* E0 ~ EF */
    3,                                  /* F0 ~ FF */
};

static const int8_t _range_min_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7,    8 */
    0x00, 0x80, 0x80, 0x80, 0xA0, 0x80, 0x90, 0x80, 0xC2,
    /* Must be invalid */
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
};

static const int8_t _range_max_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7,    8 */
    0x7F, 0xBF, 0xBF, 0xBF, 0xBF, 0x9F, 0xBF, 0x8F, 0xF4,
    /* Must be invalid */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

/* Get number of followup bytes to take care per high nibble */
static inline __m128i get_followup_bytes(const __m128i input,
        const __m128i follow_table)
{
    /* Why no _mm_srli_epi8 ? */
    const __m128i high_nibbles =
        _mm_and_si128(_mm_srli_epi16(input, 4), _mm_set1_epi8(0x0F));

    return _mm_shuffle_epi8(follow_table, high_nibbles);
}

static inline __m128i validate(const unsigned char *data, __m128i error,
       struct previous_input *prev, const __m128i tables[])
{
    const __m128i input = _mm_lddqu_si128((const __m128i *)data);

    __m128i follow_bytes = get_followup_bytes(input, tables[0]);
    __m128i follow_mask = _mm_cmpgt_epi8(follow_bytes, _mm_set1_epi8(0));
    __m128i range, tmp;

    /* 2nd byte */
    /* range = (follow_bytes, prev.follow_bytes) << 1 byte */
    range = _mm_alignr_epi8(follow_bytes, prev->follow_bytes, 15);

    /* 3rd bytes */
    __m128i prev_follow_bytes;
    /* saturate sub 1 */
    tmp = _mm_subs_epu8(follow_bytes, _mm_set1_epi8(1));
    prev_follow_bytes = _mm_subs_epu8(prev->follow_bytes, _mm_set1_epi8(1));
    /* range |= (tmp, prev_follow_bytes) << 2 bytes */
    tmp = _mm_alignr_epi8(tmp, prev_follow_bytes, 14);
    range = _mm_or_si128(range, tmp);

    /* 4th bytes */
    /* saturate sub 2 */
    tmp = _mm_subs_epu8(follow_bytes, _mm_set1_epi8(2));
    prev_follow_bytes = _mm_subs_epu8(prev->follow_bytes, _mm_set1_epi8(2));
    /* range |= (tmp, prev_follow_bytes) << 3 bytes */
    tmp = _mm_alignr_epi8(tmp, prev_follow_bytes, 13);
    range = _mm_or_si128(range, tmp);

    /* Overlap will lead to 9, 10, 11 */
    range = _mm_add_epi8(range, _mm_and_si128(follow_mask, _mm_set1_epi8(8)));

    /*
     * Check special cases (not 80..BF)
     * +------------+---------------------+-------------------+
     * | First Byte | Special Second Byte | range table index |
     * +------------+---------------------+-------------------+
     * | E0         | A0..BF              | 4                 |
     * | ED         | 80..9F              | 5                 |
     * | F0         | 90..BF              | 6                 |
     * | F4         | 80..8F              | 7                 |
     * +------------+---------------------+-------------------+
     */
    __m128i pos;
    /* tmp = (input, prev.input) << 1 byte */
    tmp = _mm_alignr_epi8(input, prev->input, 15);
    pos = _mm_cmpeq_epi8(tmp, _mm_set1_epi8(0xE0));
    range = _mm_add_epi8(range, _mm_and_si128(pos, _mm_set1_epi8(2)));  /*2+2*/
    pos = _mm_cmpeq_epi8(tmp, _mm_set1_epi8(0xED));
    range = _mm_add_epi8(range, _mm_and_si128(pos, _mm_set1_epi8(3)));  /*2+3*/
    pos = _mm_cmpeq_epi8(tmp, _mm_set1_epi8(0xF0));
    range = _mm_add_epi8(range, _mm_and_si128(pos, _mm_set1_epi8(3)));  /*3+3*/
    pos = _mm_cmpeq_epi8(tmp, _mm_set1_epi8(0xF4));
    range = _mm_add_epi8(range, _mm_and_si128(pos, _mm_set1_epi8(4)));  /*3+4*/

    /* Check value range */
    __m128i minv = _mm_shuffle_epi8(tables[1], range);
    __m128i maxv = _mm_shuffle_epi8(tables[2], range);

    /* error |= ((input < min) | (input > max)) */
    error = _mm_or_si128(error, _mm_cmplt_epi8(input, minv));
    error = _mm_or_si128(error, _mm_cmpgt_epi8(input, maxv));

    prev->input = input;
    prev->follow_bytes = follow_bytes;

    return error;
}

int utf8_range(const unsigned char *data, int len)
{
    if (len >= 16) {
        struct previous_input previous_input;

        previous_input.input = _mm_set1_epi8(0);
        previous_input.follow_bytes = _mm_set1_epi8(0);

        /* Cached constant tables */
        __m128i tables[3];

        tables[0] = _mm_lddqu_si128((const __m128i *)_follow_tbl);
        tables[1] = _mm_lddqu_si128((const __m128i *)_range_min_tbl);
        tables[2] = _mm_lddqu_si128((const __m128i *)_range_max_tbl);

        __m128i error = _mm_set1_epi8(0);

        while (len >= 16) {
            error = validate(data, error, &previous_input, tables);

            data += 16;
            len -= 16;
        }

        /* Delay error check till loop ends */
        /* Reduce error vector, error_reduced = 0xFFFF if error == 0 */
        int error_reduced =
            _mm_movemask_epi8(_mm_cmpeq_epi8(error, _mm_set1_epi8(0)));
        if (error_reduced != 0xFFFF)
            return 0;

        /* Find previous token (not 80~BF) */
        int32_t token4 = _mm_extract_epi32(previous_input.input, 3);

        const int8_t *token = (const int8_t *)&token4;
        int lookahead = 0;
        if (token[3] > (int8_t)0xBF)
            lookahead = 1;
        else if (token[2] > (int8_t)0xBF)
            lookahead = 2;
        else if (token[1] > (int8_t)0xBF)
            lookahead = 3;
        data -= lookahead;
        len += lookahead;
    }

    /* Check remaining bytes with naive method */
    return utf8_naive(data, len);
}

#ifdef DEBUG
int main(void)
{
    const unsigned char src[] =
        "\x00\x00\x00\x00\xc2\x80\x00\x00\x00\xe0\xa0\x80\x00\x00\xf4\x80" \
        "\x80\x80\x00\x00\x00\xc2\x80\x00\x00\x00\xe1\x80\x80\x00\x00\xf1" \
        "\x80\x80\x80\x00\x00";

    int ret = utf8_range(src, sizeof(src)-1);
    printf("%s\n", ret ? "ok": "bad");

    return 0;
}
#endif

#endif
