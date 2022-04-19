#ifndef IR_PRIVATE_H
#define IR_PRIVATE_H

#define IR_ALIGNED_SIZE(size, alignment) \
	(((size) + ((alignment) - 1)) & ~((alignment) - 1))

#define IR_MAX(a, b)          (((a) > (b)) ? (a) : (b))
#define IR_MIN(a, b)          (((a) < (b)) ? (a) : (b))

#define IR_IS_POWER_OF_TWO(x) (!((x) & ((x) - 1)))

#define IR_LOG2(x) ir_ntzl(x)

IR_ALWAYS_INLINE uint8_t ir_rol8(uint8_t op1, uint8_t op2)
{
	return (op1 << op2) | (op1 >> (8 - op2));
}

IR_ALWAYS_INLINE uint16_t ir_rol16(uint16_t op1, uint16_t op2)
{
	return (op1 << op2) | (op1 >> (16 - op2));
}

IR_ALWAYS_INLINE uint32_t ir_rol32(uint32_t op1, uint32_t op2)
{
	return (op1 << op2) | (op1 >> (32 - op2));
}

IR_ALWAYS_INLINE uint64_t ir_rol64(uint64_t op1, uint64_t op2)
{
	return (op1 << op2) | (op1 >> (64 - op2));
}

IR_ALWAYS_INLINE uint8_t ir_ror8(uint8_t op1, uint8_t op2)
{
	return (op1 >> op2) | (op1 << (8 - op2));
}

IR_ALWAYS_INLINE uint16_t ir_ror16(uint16_t op1, uint16_t op2)
{
	return (op1 >> op2) | (op1 << (16 - op2));
}

IR_ALWAYS_INLINE uint32_t ir_ror32(uint32_t op1, uint32_t op2)
{
	return (op1 >> op2) | (op1 << (32 - op2));
}

IR_ALWAYS_INLINE uint64_t ir_ror64(uint64_t op1, uint64_t op2)
{
	return (op1 >> op2) | (op1 << (64 - op2));
}

IR_ALWAYS_INLINE int64_t ir_ipow(int64_t op1, int64_t op2)
{
	int64_t ret = 1;

	if (op2 == 0) {
		return 1;
	} else if (op1 == 0) {
		return 0;
	}

	while (op2 >= 1) {
		if (op2 % 2) {
			op2--;
			ret = ret * op1;
		} else {
			op2 /= 2;
			op1 = op1 * op1;
		}
	}
	return ret;
}

IR_ALWAYS_INLINE uint64_t ir_upow(uint64_t op1, uint64_t op2)
{
	uint64_t ret = 1;

	if (op2 == 0) {
		return 1;
	} else if (op1 == 0) {
		return 0;
	}

	while (op2 >= 1) {
		if (op2 % 2) {
			op2--;
			ret = ret * op1;
		} else {
			op2 /= 2;
			op1 = op1 * op1;
		}
	}
	return ret;
}

/* Number of trailing zero bits (0x01 -> 0; 0x40 -> 6; 0x00 -> LEN) */
IR_ALWAYS_INLINE uint32_t ir_ntz(uint32_t num)
{
#if (defined(__GNUC__) || __has_builtin(__builtin_ctz))
	return __builtin_ctz(num);
#elif defined(_WIN32)
	uint32_t index;

	if (!BitScanForward(&index, num)) {
		/* undefined behavior */
		return 32;
	}

	return index;
#else
	int n;

	if (num == 0) return 32;

	n = 1;
	if ((num & 0x0000ffff) == 0) {n += 16; num = num >> 16;}
	if ((num & 0x000000ff) == 0) {n +=  8; num = num >>  8;}
	if ((num & 0x0000000f) == 0) {n +=  4; num = num >>  4;}
	if ((num & 0x00000003) == 0) {n +=  2; num = num >>  2;}
	return n - (num & 1);
#endif
}

/* Number of trailing zero bits (0x01 -> 0; 0x40 -> 6; 0x00 -> LEN) */
IR_ALWAYS_INLINE uint32_t ir_ntzl(uint64_t num)
{
#if (defined(__GNUC__) || __has_builtin(__builtin_ctzl))
	return __builtin_ctzl(num);
#elif defined(_WIN32)
	unsigned long index;

#if defined(_WIN64)
	if (!BitScanForward64(&index, num)) {
#else
	if (!BitScanForward(&index, num)) {
#endif
		/* undefined behavior */
		return 64;
	}

	return (uint32_t) index;
#else
	uint32_t n;

	if (num == Z_UL(0)) return 64;

	n = 1;
	if ((num & 0xffffffff) == 0) {n += 32; num = num >> Z_UL(32);}
	if ((num & 0x0000ffff) == 0) {n += 16; num = num >> 16;}
	if ((num & 0x000000ff) == 0) {n +=  8; num = num >>  8;}
	if ((num & 0x0000000f) == 0) {n +=  4; num = num >>  4;}
	if ((num & 0x00000003) == 0) {n +=  2; num = num >>  2;}
	return n - (num & 1);
#endif
}

/* Number of leading zero bits (Undefined for zero) */
IR_ALWAYS_INLINE int ir_nlz(uint32_t num)
{
#if (defined(__GNUC__) || __has_builtin(__builtin_clz))
	return __builtin_clz(num);
#elif defined(_WIN32)
	uint32_t index;

	if (!BitScanReverse(&index, num)) {
		/* undefined behavior */
		return 32;
	}

	return (int) (32 - 1)- index;
#else
	uint32_t x;
	uint32_t n;

	n = 32;
	x = num >> 16; if (x != 0) {n -= 16; num = x;}
	x = num >> 8;  if (x != 0) {n -=  8; num = x;}
	x = num >> 4;  if (x != 0) {n -=  4; num = x;}
	x = num >> 2;  if (x != 0) {n -=  2; num = x;}
	x = num >> 1;  if (x != 0) return n - 2;
	return n - num;
#endif
}

/* Bitsets */
typedef uint32_t *ir_bitset;

IR_ALWAYS_INLINE uint32_t ir_bitset_len(uint32_t n)
{
	return (n + 31) / 32;
}

IR_ALWAYS_INLINE ir_bitset ir_bitset_malloc(uint32_t n)
{
	return ir_mem_calloc(ir_bitset_len(n), sizeof(uint32_t));
}

IR_ALWAYS_INLINE void ir_bitset_incl(ir_bitset set, uint32_t n)
{
	set[n / 32] |= 1 << (n % 32);
}

IR_ALWAYS_INLINE void ir_bitset_excl(ir_bitset set, uint32_t n)
{
	set[n / 32] &= ~(1 << (n % 32));
}

IR_ALWAYS_INLINE bool ir_bitset_in(ir_bitset set, uint32_t n)
{
	return (set[(n / 32)] & (1 << (n % 32))) != 0;
}

IR_ALWAYS_INLINE void ir_bitset_clear(ir_bitset set, uint32_t len)
{
	memset(set, 0, len * sizeof(uint32_t));
}

IR_ALWAYS_INLINE void ir_bitset_fill(ir_bitset set, uint32_t len)
{
	memset(set, 0xff, len * sizeof(uint32_t));
}

IR_ALWAYS_INLINE bool ir_bitset_empty(ir_bitset set, uint32_t len)
{
	uint32_t i;
	for (i = 0; i < len; i++) {
		if (set[i]) {
			return 0;
		}
	}
	return 1;
}

IR_ALWAYS_INLINE bool ir_bitset_equal(ir_bitset set1, ir_bitset set2, uint32_t len)
{
    return memcmp(set1, set2, len * sizeof(uint32_t)) == 0;
}

IR_ALWAYS_INLINE void ir_bitset_copy(ir_bitset set1, ir_bitset set2, uint32_t len)
{
    memcpy(set1, set2, len * sizeof(uint32_t));
}

IR_ALWAYS_INLINE void ir_bitset_intersection(ir_bitset set1, ir_bitset set2, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
		set1[i] &= set2[i];
	}
}

IR_ALWAYS_INLINE void ir_bitset_union(ir_bitset set1, ir_bitset set2, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		set1[i] |= set2[i];
	}
}

IR_ALWAYS_INLINE void ir_bitset_difference(ir_bitset set1, ir_bitset set2, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		set1[i] = set1[i] & ~set2[i];
	}
}

IR_ALWAYS_INLINE bool ir_bitset_subset(ir_bitset set1, ir_bitset set2, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (set1[i] & ~set2[i]) {
			return 0;
		}
	}
	return 1;
}

IR_ALWAYS_INLINE int ir_bitset_first(ir_bitset set, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (set[i]) {
			return 32 * i + ir_ntz(set[i]);
		}
	}
	return -1; /* empty set */
}

IR_ALWAYS_INLINE int ir_bitset_last(ir_bitset set, uint32_t len)
{
	uint32_t i = len;

	while (i > 0) {
		i--;
		if (set[i]) {
			uint32_t j = 32 * i - 1;
			uint32_t x = set[i];
			do {
				x = x >> 1;
				j++;
			} while (x != 0);
			return j;
		}
	}
	return -1; /* empty set */
}

IR_ALWAYS_INLINE int ir_bitset_pop_first(ir_bitset set, uint32_t len) {
	int i = ir_bitset_first(set, len);
	if (i >= 0) {
		ir_bitset_excl(set, i);
	}
	return i;
}

#define IR_BITSET_FOREACH(set, len, bit) do { \
	ir_bitset _set = (set); \
	uint32_t _i, _len = (len); \
	for (_i = 0; _i < _len; _i++) { \
		uint32_t _x = _set[_i]; \
		if (_x) { \
			(bit) = 32 * _i; \
			for (; _x != 0; _x >>= 1, (bit)++) { \
				if (!(_x & 1)) continue;

#define IR_BITSET_FOREACH_END() \
			} \
		} \
	} \
} while (0)


/* Dynamic array of numeric references */
typedef struct _ir_array {
	ir_ref   *refs;
	uint32_t  size;
} ir_array;

void ir_array_grow(ir_array *a, uint32_t size);
void ir_array_insert(ir_array *a, uint32_t i, ir_ref val);
void ir_array_remove(ir_array *a, uint32_t i);

IR_ALWAYS_INLINE void ir_array_init(ir_array *a, uint32_t size)
{
	a->refs = ir_mem_calloc(size, sizeof(ir_ref));
	a->size = size;
}

IR_ALWAYS_INLINE void ir_array_free(ir_array *a)
{
	ir_mem_free(a->refs);
	a->refs = NULL;
	a->size = 0;
}

IR_ALWAYS_INLINE uint32_t ir_array_size(ir_array *a)
{
	return a->size;
}

IR_ALWAYS_INLINE ir_ref ir_array_get(ir_array *a, uint32_t i)
{
	return (i < a->size) ? a->refs[i] : IR_UNUSED;
}

IR_ALWAYS_INLINE ir_ref ir_array_at(ir_array *a, uint32_t i)
{
	IR_ASSERT(i < a->size);
	return a->refs[i];
}

IR_ALWAYS_INLINE void ir_array_set(ir_array *a, uint32_t i, ir_ref val)
{
	if (i >= a->size) {
		ir_array_grow(a, i);
	}
	a->refs[i] = val;
}

/* List/Stack of numeric references */
typedef struct _ir_list {
	ir_array a;
	uint32_t len;
} ir_list;

bool ir_list_contains(ir_list *l, ir_ref val);
void ir_list_insert(ir_list *l, uint32_t i, ir_ref val);
void ir_list_remove(ir_list *l, uint32_t i);

IR_ALWAYS_INLINE void ir_list_init(ir_list *l, uint32_t size)
{
	ir_array_init(&l->a, size);
	l->len = 0;
}

IR_ALWAYS_INLINE void ir_list_free(ir_list *l)
{
	ir_array_free(&l->a);
	l->len = 0;
}

IR_ALWAYS_INLINE void ir_list_clear(ir_list *l)
{
	l->len = 0;
}

IR_ALWAYS_INLINE uint32_t ir_list_len(ir_list *l)
{
	return l->len;
}

IR_ALWAYS_INLINE uint32_t ir_list_capasity(ir_list *l)
{
	return ir_array_size(&l->a);
}

IR_ALWAYS_INLINE void ir_list_push(ir_list *l, ir_ref val)
{
	ir_array_set(&l->a, l->len++, val);
}

IR_ALWAYS_INLINE ir_ref ir_list_pop(ir_list *l)
{
	IR_ASSERT(l->len > 0);
	return ir_array_at(&l->a, --l->len);
}

IR_ALWAYS_INLINE ir_ref ir_list_peek(ir_list *l)
{
	IR_ASSERT(l->len > 0);
	return ir_array_at(&l->a, l->len - 1);
}

IR_ALWAYS_INLINE ir_ref ir_list_at(ir_list *l, uint32_t i)
{
	IR_ASSERT(i < l->len);
	return ir_array_at(&l->a, i);
}

/* Worklist (unique list) */
typedef struct _ir_worklist {
	ir_list l;
	ir_bitset visited;
} ir_worklist;

IR_ALWAYS_INLINE void ir_worklist_init(ir_worklist *w, uint32_t size)
{
	ir_list_init(&w->l, size);
	w->visited = ir_bitset_malloc(size);
}

IR_ALWAYS_INLINE void ir_worklist_free(ir_worklist *w)
{
	ir_list_free(&w->l);
	ir_mem_free(w->visited);
}

IR_ALWAYS_INLINE uint32_t ir_worklist_len(ir_worklist *w)
{
	return ir_list_len(&w->l);
}

IR_ALWAYS_INLINE uint32_t ir_worklist_capasity(ir_worklist *w)
{
	return ir_list_capasity(&w->l);
}

IR_ALWAYS_INLINE void ir_worklist_clear(ir_worklist *w)
{
	ir_list_free(&w->l);
	ir_bitset_clear(w->visited, ir_bitset_len(ir_worklist_capasity(w)));
}

IR_ALWAYS_INLINE bool ir_worklist_push(ir_worklist *w, ir_ref val)
{
	IR_ASSERT(val >= 0 && val < ir_worklist_capasity(w));
	if (ir_bitset_in(w->visited, val)) {
		return 0;
	}
	ir_bitset_incl(w->visited, val);
	ir_list_push(&w->l, val);
	return 1;
}

IR_ALWAYS_INLINE ir_ref ir_worklist_pop(ir_worklist *w)
{
	return ir_list_pop(&w->l);
}

IR_ALWAYS_INLINE ir_ref ir_worklist_peek(ir_worklist *w)
{
	return ir_list_peek(&w->l);
}

#endif /* IR_PRIVATE_H */
