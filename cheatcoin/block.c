/* работа с блоками, T13.654-T13.720 $DVS:time$ */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include "../ldus/source/include/ldus/rbtree.h"
#include "block.h"
#include "crypt.h"
#include "wallet.h"
#include "storage.h"
#include "transport.h"
#include "log.h"
#include "main.h"

#define MAIN_CHAIN_PERIOD	(64 << 10)
#define MAX_WAITING_MAIN	2
#define DEF_TIME_LIMIT		0 /*(MAIN_CHAIN_PERIOD / 2)*/
#define CHEATCOIN_TEST_ERA	0x16800000000l
#define CHEATCOIN_MAIN_ERA	0x16900000000l
#define CHEATCOIN_ERA		cheatcoin_era
#define MAIN_START_AMOUNT	(1l << 42)
#define MAIN_BIG_PERIOD_LOG	21
#define MAIN_TIME(t)		((t) >> 16)
#define MAX_LINKS			15
#define MAKE_BLOCK_PERIOD	13
#define QUERY_RETRIES		2

enum bi_flags {
	BI_MAIN			= 0x01,
	BI_MAIN_CHAIN	= 0x02,
	BI_APPLIED		= 0x04,
	BI_MAIN_REF		= 0x08,
	BI_REF			= 0x10,
	BI_OURS			= 0x20,
};

struct block_internal {
	struct ldus_rbtree node;
	cheatcoin_hash_t hash;
	cheatcoin_diff_t difficulty;
	cheatcoin_amount_t amount;
	cheatcoin_time_t time;
	uint64_t storage_pos;
	struct block_internal *ref, *link[MAX_LINKS];
	uint8_t flags, nlinks, max_diff_link, reserved;
	uint16_t in_mask;
	uint16_t n_our_key;
};

#define ourprev link[MAX_LINKS - 2]
#define ournext link[MAX_LINKS - 1]

static cheatcoin_amount_t g_balance = 0;
static cheatcoin_time_t time_limit = DEF_TIME_LIMIT, cheatcoin_era = CHEATCOIN_MAIN_ERA;
static struct ldus_rbtree *root = 0;
static struct block_internal *top_main_chain = 0, *pretop_main_chain = 0,
		*ourfirst = 0, *ourlast = 0, *noref_first = 0, *noref_last = 0;
static pthread_mutex_t block_mutex;

static uint64_t get_timestamp(void) {
	struct timeval tp;
	gettimeofday(&tp, 0);
	return tp.tv_sec << 10 | ((tp.tv_usec << 10) / 1000000);
}

static inline int lessthen(struct ldus_rbtree *l, struct ldus_rbtree *r) {
	return memcmp(l + 1, r + 1, 24) < 0;
}

ldus_rbtree_define_prefix(lessthen, static inline, )

static struct block_internal *block_by_hash(cheatcoin_hashlow_t hash) {
	return (struct block_internal *)ldus_rbtree_find(root, (struct ldus_rbtree *)hash - 1);
}

static void log_block(const char *mess, cheatcoin_hash_t h, cheatcoin_time_t t) {
	cheatcoin_info("%s: %016lx%016lx%016lx%016lx t=%lx", mess,
		((uint64_t*)h)[3], ((uint64_t*)h)[2], ((uint64_t*)h)[1], ((uint64_t*)h)[0], t);
}

static inline void accept_amount(struct block_internal *bi, cheatcoin_amount_t sum) {
	if (!sum) return;
	bi->amount += sum;
	if (bi->flags & BI_OURS) {
		struct block_internal *ti;
		g_balance += sum;
		while ((ti = bi->ourprev) && bi->amount > ti->amount) {
			bi->ourprev = ti->ourprev, ti->ournext = bi->ournext, bi->ournext = ti, ti->ourprev = bi;
			*(bi->ourprev ? &bi->ourprev->ournext : &ourfirst) = bi;
			*(ti->ournext ? &ti->ournext->ourprev : &ourlast ) = ti;
		}
		while ((ti = bi->ournext) && bi->amount < ti->amount) {
			bi->ournext = ti->ournext, ti->ourprev = bi->ourprev, bi->ourprev = ti, ti->ournext = bi;
			*(bi->ournext ? &bi->ournext->ourprev : &ourlast ) = bi;
			*(ti->ourprev ? &ti->ourprev->ournext : &ourfirst) = ti;
		}
	}
}

static uint64_t apply_block(struct block_internal *bi) {
	struct cheatcoin_block buf, *b;
	cheatcoin_amount_t sum_in, sum_out, link_amount[MAX_LINKS];
	int i, n;
	if (bi->flags & BI_MAIN_REF) return -1l;
	bi->flags |= BI_MAIN_REF;
	for (i = 0; i < bi->nlinks; ++i) {
		cheatcoin_amount_t ref_amount = apply_block(bi->link[i]);
		if (ref_amount == -1l) continue;
		bi->link[i]->ref = bi;
		if (bi->amount + ref_amount >= bi->amount)
			accept_amount(bi, ref_amount);
	}
	b = cheatcoin_storage_load(bi->time, bi->storage_pos, &buf);
	if (!b) return 0;
	for (i = n = 0; i < CHEATCOIN_BLOCK_FIELDS; ++i) {
		if (cheatcoin_type(b, i) == CHEATCOIN_FIELD_IN || cheatcoin_type(b, i) == CHEATCOIN_FIELD_OUT)
			link_amount[n++] = b->field[i].amount;
	}
	sum_in = 0, sum_out = b->field[0].amount;
	for (i = 0; i < bi->nlinks; ++i) {
		if (1 << i & bi->in_mask) {
			if (bi->link[i]->amount < link_amount[i]) return 0;
			if (sum_in + link_amount[i] < sum_in) return 0;
			sum_in += link_amount[i];
		} else {
			if (sum_out + link_amount[i] < sum_out) return 0;
			sum_out += link_amount[i];
		}
	}
	if (sum_in + bi->amount < sum_in || sum_in + bi->amount < sum_out) return 0;
	for (i = 0; i < bi->nlinks; ++i) {
		if (1 << i & bi->in_mask) accept_amount(bi->link[i], -link_amount[i]);
		else accept_amount(bi->link[i], link_amount[i]);
	}
	accept_amount(bi, sum_in - sum_out);
	bi->flags |= BI_APPLIED;
	return b->field[0].amount;
}

static uint64_t unapply_block(struct block_internal *bi) {
	struct cheatcoin_block buf, *b = 0;
	int i, n;
	if (bi->flags & BI_APPLIED && (b = cheatcoin_storage_load(bi->time, bi->storage_pos, &buf))) {
		cheatcoin_amount_t sum = b->field[0].amount;
		for (i = n = 0; i < CHEATCOIN_BLOCK_FIELDS; ++i) {
			if (cheatcoin_type(b, i) == CHEATCOIN_FIELD_IN)
				accept_amount(bi->link[n++],  b->field[i].amount), sum -= b->field[i].amount;
			else if (cheatcoin_type(b, i) == CHEATCOIN_FIELD_OUT)
				accept_amount(bi->link[n++], -b->field[i].amount), sum += b->field[i].amount;
		}
		accept_amount(bi, sum);
		bi->flags &= ~BI_APPLIED;
	}
	bi->flags &= ~BI_MAIN_REF;
	for (i = 0; i < bi->nlinks; ++i)
		if (bi->link[i]->ref == bi && bi->link[i]->flags & BI_MAIN_REF) accept_amount(bi, unapply_block(bi->link[i]));
	return b ? -b->field[0].amount : 0;
}

/* по данному кол-ву главных блоков возвращает объем циркулирующих читкоинов */
cheatcoin_amount_t cheatcoin_get_supply(uint64_t nmain) {
	cheatcoin_amount_t res = 0, amount = MAIN_START_AMOUNT;
	while (nmain >> MAIN_BIG_PERIOD_LOG) {
		res += (1l << MAIN_BIG_PERIOD_LOG) * amount;
		nmain -= 1l << MAIN_BIG_PERIOD_LOG;
		amount >>= 1;
	}
	res += nmain * amount;
	return res;
}

static void set_main(struct block_internal *m) {
	cheatcoin_amount_t amount = MAIN_START_AMOUNT >> (g_cheatcoin_stats.nmain >> MAIN_BIG_PERIOD_LOG);
	m->flags |= BI_MAIN;
	accept_amount(m, amount);
	g_cheatcoin_stats.nmain++;
	if (g_cheatcoin_stats.nmain > g_cheatcoin_stats.total_nmain)
		g_cheatcoin_stats.total_nmain = g_cheatcoin_stats.nmain;
	accept_amount(m, apply_block(m));
	log_block((m->flags & BI_OURS ? "MAIN +" : "MAIN  "), m->hash, m->time);
}

static void unset_main(struct block_internal *m) {
	cheatcoin_amount_t amount;
	g_cheatcoin_stats.nmain--;
	g_cheatcoin_stats.total_nmain--;
	amount = MAIN_START_AMOUNT >> (g_cheatcoin_stats.nmain >> MAIN_BIG_PERIOD_LOG);
	m->flags &= ~BI_MAIN;
	accept_amount(m, -amount);
	accept_amount(m, unapply_block(m));
	log_block("UNMAIN", m->hash, m->time);
}

static void check_new_main(void) {
	struct block_internal *b, *p = 0;
	int i;
	for (b = top_main_chain, i = 0; b && !(b->flags & BI_MAIN); b = b->link[b->max_diff_link])
		if (b->flags & BI_MAIN_CHAIN) p = b, ++i;
	if (p && i > MAX_WAITING_MAIN) set_main(p);
}

static void unwind_main(struct block_internal *b) {
	struct block_internal *t;
	for (t = top_main_chain; t != b; t = t->link[t->max_diff_link]) 
		{ t->flags &= ~BI_MAIN_CHAIN; if (t->flags & BI_MAIN) unset_main(t); }
}

static inline void hash_for_signature(struct cheatcoin_block b[2], const struct cheatcoin_public_key *key, cheatcoin_hash_t hash) {
	memcpy((uint8_t *)(b + 1) + 1, (void *)((long)key->pub & ~1l), sizeof(cheatcoin_hash_t));
	*(uint8_t *)(b + 1) = ((long)key->pub & 1) | 0x02;
	cheatcoin_hash(b, sizeof(struct cheatcoin_block) + sizeof(cheatcoin_hash_t) + 1, hash);
	cheatcoin_debug("Hash  : hash=[%s] data=[%s]", cheatcoin_log_hash(hash),
			cheatcoin_log_array(b, sizeof(struct cheatcoin_block) + sizeof(cheatcoin_hash_t) + 1));
}

static inline cheatcoin_diff_t hash_difficulty(cheatcoin_hash_t hash) {
	cheatcoin_diff_t res = ((cheatcoin_diff_t *)hash)[1];
	return -(cheatcoin_diff_t)1 / (res >> 32);
}

/* возвращает номер открытого ключа из массива keys длины nkeys, который подходит к подписи, начинающейся с поля signo_r блока b,
 * или -1, если ни один не подходит
 */
static int valid_signature(const struct cheatcoin_block *b, int signo_r, int nkeys, struct cheatcoin_public_key *keys) {
	struct cheatcoin_block buf[2];
	cheatcoin_hash_t hash;
	int i, signo_s = -1;
	memcpy(buf, b, sizeof(struct cheatcoin_block));
	for (i = signo_r; i < CHEATCOIN_BLOCK_FIELDS; ++i)
		if (cheatcoin_type(b,i) == CHEATCOIN_FIELD_SIGN_IN || cheatcoin_type(b,i) == CHEATCOIN_FIELD_SIGN_OUT) {
			memset(&buf[0].field[i], 0, sizeof(struct cheatcoin_field));
			if (i > signo_r && signo_s < 0 && cheatcoin_type(b,i) == cheatcoin_type(b,signo_r)) signo_s = i;
		}
	if (signo_s >= 0) for (i = 0; i < nkeys; ++i) {
		hash_for_signature(buf, keys + i, hash);
		if (!cheatcoin_verify_signature(keys[i].key, hash, b->field[signo_r].data, b->field[signo_s].data)) return i;
	}
	return -1;
}

/* основная функция; проверить и добавить в базу новый блок; возвращает: > 0 - добавлен, = 0  - уже есть, < 0 - ошибка */
static int add_block_nolock(struct cheatcoin_block *b, cheatcoin_time_t limit) {
	uint64_t timestamp = get_timestamp(), sum_in = 0, sum_out = 0, *psum, theader = b->field[0].transport_header;
	struct cheatcoin_public_key public_keys[16], *our_keys = 0;
	int i, j, k, nkeys = 0, nourkeys = 0, nsignin = 0, nsignout = 0, signinmask = 0, signoutmask = 0, inmask = 0, outmask = 0,
		verified_keys_mask = 0, err, type, nkey;
	struct block_internal bi, *ref, *bsaved, *ref0;
	cheatcoin_diff_t diff0, diff;
	memset(&bi, 0, sizeof(struct block_internal));
	b->field[0].transport_header = 0;
	cheatcoin_hash(b, sizeof(struct cheatcoin_block), bi.hash);
	if (block_by_hash(bi.hash)) return 0;
	if (cheatcoin_type(b,0) != CHEATCOIN_FIELD_HEAD) { i = cheatcoin_type(b,0); err = 1; goto end; }
	bi.time = b->field[0].time;
	if (bi.time > timestamp + MAIN_CHAIN_PERIOD / 4 || bi.time < CHEATCOIN_ERA
			|| (limit && timestamp - bi.time > limit)) { i = 0; err = 2; goto end; }
	check_new_main();
	for (i = 1; i < CHEATCOIN_BLOCK_FIELDS; ++i) switch((type = cheatcoin_type(b,i))) {
		case CHEATCOIN_FIELD_NONCE:			break;
		case CHEATCOIN_FIELD_IN:			inmask  |= 1 << i; break;
		case CHEATCOIN_FIELD_OUT:			outmask |= 1 << i; break;
		case CHEATCOIN_FIELD_SIGN_IN:		if (++nsignin  & 1) signinmask  |= 1 << i; break;
		case CHEATCOIN_FIELD_SIGN_OUT:		if (++nsignout & 1) signoutmask |= 1 << i; break;
		case CHEATCOIN_FIELD_PUBLIC_KEY_0:
		case CHEATCOIN_FIELD_PUBLIC_KEY_1:
			if ((public_keys[nkeys].key = cheatcoin_public_to_key(b->field[i].data, type - CHEATCOIN_FIELD_PUBLIC_KEY_0)))
				public_keys[nkeys++].pub = (uint64_t *)((long)&b->field[i].data | (type - CHEATCOIN_FIELD_PUBLIC_KEY_0));
			break;
		default: err = 3; goto end;
	}
	if (nsignout & 1) { i = nsignout; err = 4; goto end; }
	if (nsignout) our_keys = cheatcoin_wallet_our_keys(&nourkeys);
	for (i = 1; i < CHEATCOIN_BLOCK_FIELDS; ++i) if (1 << i & (signinmask | signoutmask)) {
		nkey = valid_signature(b, i, nkeys, public_keys);
		if (nkey >= 0) verified_keys_mask |= 1 << nkey;
		if (1 << i & signoutmask && !(bi.flags & BI_OURS) && (nkey = valid_signature(b, i, nourkeys, our_keys)) >= 0)
			bi.flags |= BI_OURS, bi.n_our_key = nkey;
	}
	for (i = j = 0; i < nkeys; ++i) if (1 << i & verified_keys_mask)
		memcpy(public_keys + j++, public_keys + i, sizeof(struct cheatcoin_public_key));
	nkeys = j;
	bi.difficulty = diff0 = hash_difficulty(bi.hash);
	sum_out += b->field[0].amount;
	for (i = 1; i < CHEATCOIN_BLOCK_FIELDS; ++i) if (1 << i & (inmask | outmask)) {
		if (!(ref = block_by_hash(b->field[i].hash))) { err = 5; goto end; }
		if (ref->time >= bi.time) { err = 6; goto end; }
		if (bi.nlinks >= MAX_LINKS) { err = 7; goto end; }
		if (1 << i & inmask) {
			if (b->field[i].amount) {
				struct cheatcoin_block buf, *bref = cheatcoin_storage_load(ref->time, ref->storage_pos, &buf);
				if (!bref) { err = 8; goto end; }
				for (j = k = 0; j < CHEATCOIN_BLOCK_FIELDS; ++j)
					if (cheatcoin_type(bref, j) == CHEATCOIN_FIELD_SIGN_OUT && (++k & 1)
							&& valid_signature(bref, j, nkeys, public_keys) >= 0) break;
				if (j == CHEATCOIN_BLOCK_FIELDS) { err = 9; goto end; }
			}
			psum = &sum_in;
			bi.in_mask |= 1 << bi.nlinks;
		} else psum = &sum_out;
		if (*psum + b->field[i].amount < *psum) { err = 0xA; goto end; }
		*psum += b->field[i].amount;
		bi.link[bi.nlinks] = ref;
		if (MAIN_TIME(ref->time) < MAIN_TIME(bi.time)) diff = diff0 + ref->difficulty;
		else {
			diff = ref->difficulty;
			while (ref && MAIN_TIME(ref->time) == MAIN_TIME(bi.time)) ref = ref->link[ref->max_diff_link];
			if (ref && diff < diff0 + ref->difficulty) diff = diff0 + ref->difficulty;
		}
		if (diff > bi.difficulty) bi.difficulty = diff, bi.max_diff_link = bi.nlinks;
		bi.nlinks++;
	}
	if (bi.in_mask ? sum_in < sum_out : sum_out != b->field[0].amount) { err = 0xB; goto end; }
	bsaved = malloc(sizeof(struct block_internal));
	if (!bsaved) { err = 0xC; goto end; }
	if (!(theader & (sizeof(struct block_internal) - 1))) bi.storage_pos = theader;
	else bi.storage_pos = cheatcoin_storage_save(b);
	memcpy(bsaved, &bi, sizeof(struct block_internal));
	ldus_rbtree_insert(&root, &bsaved->node);
	g_cheatcoin_stats.nblocks++;
	if (g_cheatcoin_stats.nblocks > g_cheatcoin_stats.total_nblocks)
		g_cheatcoin_stats.total_nblocks = g_cheatcoin_stats.nblocks;
	if (bi.difficulty > g_cheatcoin_stats.difficulty) {
		cheatcoin_info("Diff  : %lx%016lx (+%lx%016lx)", (unsigned long)(bi.difficulty >> 64), (unsigned long)bi.difficulty,
				(unsigned long)(diff0 >> 64), (unsigned long)diff0);
		for (ref = bsaved, ref0 = 0; ref && !(ref->flags & BI_MAIN_CHAIN); ref = ref->link[ref->max_diff_link]) {
			if ((!ref->link[ref->max_diff_link] || ref->link[ref->max_diff_link]->difficulty < ref->difficulty)
					&& (!ref0 || MAIN_TIME(ref0->time) > MAIN_TIME(ref->time)))
				{ ref->flags |= BI_MAIN_CHAIN; if (ref0 == bsaved) pretop_main_chain = ref; ref0 = ref; }
		}
		if (ref && MAIN_TIME(ref->time) == MAIN_TIME(bsaved->time)) ref = ref->link[ref->max_diff_link];
		unwind_main(ref);
		top_main_chain = bsaved;
		g_cheatcoin_stats.difficulty = bi.difficulty;
		if (g_cheatcoin_stats.difficulty > g_cheatcoin_stats.max_difficulty)
			g_cheatcoin_stats.max_difficulty = g_cheatcoin_stats.difficulty;
	}
	if (bi.flags & BI_OURS)
		bsaved->ourprev = ourlast, *(ourlast ? &ourlast->ournext : &ourfirst) = bsaved, ourlast = bsaved;
	for (i = 0; i < bi.nlinks; ++i) if (!(bi.link[i]->flags & BI_REF)) {
		for (ref0 = 0, ref = noref_first; ref != bi.link[i]; ref0 = ref, ref = ref->ref);
		*(ref0 ? &ref0->ref : &noref_first) = ref->ref;
		if (ref == noref_last) noref_last = ref0;
		bi.link[i]->flags |= BI_REF;
		g_cheatcoin_extstats.nnoref--;
	}
	*(noref_last ? &noref_last->ref : &noref_first) = bsaved;
	noref_last = bsaved;
	g_cheatcoin_extstats.nnoref++;
	log_block((bi.flags & BI_OURS ? "Good +" : "Good  "), bi.hash, bi.time);
	return 1;
end:
	{
		char buf[32];
		sprintf(buf, "Err %2x", ((i << 4) | err) & 0xff);
		log_block(buf, bi.hash, bi.time);
		err = -err;
	}
	return err;
}

static void *add_block_callback(void *block, void *data) {
	struct cheatcoin_block *b = (struct cheatcoin_block *)block;
	cheatcoin_time_t *t = (cheatcoin_time_t *)data;
	pthread_mutex_lock(&block_mutex);
	if (*t < CHEATCOIN_ERA) add_block_nolock(b, *t);
	else if (add_block_nolock(b, 0) >= 0 && b->field[0].time > *t) *t = b->field[0].time;
	pthread_mutex_unlock(&block_mutex);
	return 0;
}

/* проверить блок и включить его в базу данных, возвращает не 0 в случае ошибки */
int cheatcoin_add_block(struct cheatcoin_block *b) {
	int res;
	pthread_mutex_lock(&block_mutex);
	res = add_block_nolock(b, time_limit);
	pthread_mutex_unlock(&block_mutex);
	return res;
}

#define setfld(fldtype, src, hashtype) (\
	b[0].field[0].type |= (uint64_t)(fldtype) << (i << 2), \
	memcpy(&b[0].field[i++], (void *)(src), sizeof(hashtype)) \
)

#define pretop_block() (top_main_chain && MAIN_TIME(top_main_chain->time) == MAIN_TIME(send_time) ? pretop_main_chain : top_main_chain)

/* создать и опубликовать блок; в первых ninput полях fields содержатся адреса входов и соотв. кол-во читкоинов,
 * в следующих noutput полях - аналогично - выходы; fee - комиссия; send_time - время отправки блока; если оно больше текущего, то
 * проводится майнинг для генерации наиболее оптимального хеша */
int cheatcoin_create_block(struct cheatcoin_field *fields, int ninput, int noutput, cheatcoin_amount_t fee, cheatcoin_time_t send_time) {
	struct cheatcoin_block b[2];
	int i, j, res, mining, defkeynum, keysnum[CHEATCOIN_BLOCK_FIELDS], nkeys, nkeysnum = 0, outsigkeyind = -1;
	struct cheatcoin_public_key *defkey = cheatcoin_wallet_default_key(&defkeynum), *keys = cheatcoin_wallet_our_keys(&nkeys), *key;
	cheatcoin_hash_t hash, min_hash;
	struct block_internal *ref, *pretop = pretop_block();
	for (i = 0; i < ninput; ++i) {
		ref = block_by_hash(fields[i].hash);
		if (!ref || !(ref->flags & BI_OURS)) return -1;
		for (j = 0; j < nkeysnum && ref->n_our_key != keysnum[j]; ++j);
		if (j == nkeysnum) {
			if (outsigkeyind < 0 && ref->n_our_key == defkeynum) outsigkeyind = nkeysnum;
			keysnum[nkeysnum++] = ref->n_our_key;
		}
	}
	res = 1 + ninput + noutput + 3 * nkeysnum + (outsigkeyind < 0 ? 2 : 0);
	if (res > CHEATCOIN_BLOCK_FIELDS) return -1;
	if (!send_time) send_time = get_timestamp(), mining = 0;
	else mining = (send_time > get_timestamp() && res + 1 <= CHEATCOIN_BLOCK_FIELDS);
	res += mining;
begin:
	memset(b, 0, sizeof(struct cheatcoin_block)); i = 1;
	b[0].field[0].type = CHEATCOIN_FIELD_HEAD | (mining ? (uint64_t)CHEATCOIN_FIELD_SIGN_IN << ((CHEATCOIN_BLOCK_FIELDS - 1) * 4) : 0);
	b[0].field[0].time = send_time;
	b[0].field[0].amount = fee;
	if (res < CHEATCOIN_BLOCK_FIELDS && mining && top_main_chain) { setfld(CHEATCOIN_FIELD_OUT, top_main_chain->hash, cheatcoin_hashlow_t); res++; }
	for (ref = noref_first; ref && res < CHEATCOIN_BLOCK_FIELDS; ref = ref->ref, ++res) setfld(CHEATCOIN_FIELD_OUT, ref->hash, cheatcoin_hashlow_t);
	for (j = 0; j < ninput; ++j) setfld(CHEATCOIN_FIELD_IN, fields + j, cheatcoin_hash_t);
	for (j = 0; j < noutput; ++j) setfld(CHEATCOIN_FIELD_OUT, fields + ninput + j, cheatcoin_hash_t);
	for (j = 0; j < nkeysnum; ++j) {
		key = keys + keysnum[j];
		b[0].field[0].type |= (uint64_t)((j == outsigkeyind ? CHEATCOIN_FIELD_SIGN_OUT : CHEATCOIN_FIELD_SIGN_IN) * 0x11) << ((i + j + nkeysnum) * 4);
		setfld(CHEATCOIN_FIELD_PUBLIC_KEY_0 + ((long)key->pub & 1), (long)key->pub & ~1l, cheatcoin_hash_t);
	}
	if (outsigkeyind < 0) b[0].field[0].type |= (uint64_t)(CHEATCOIN_FIELD_SIGN_OUT * 0x11) << ((i + j + nkeysnum) * 4);
	for (j = 0; j < nkeysnum; ++j, i += 2) {
		key = keys + keysnum[j];
		hash_for_signature(b, key, hash);
		cheatcoin_sign(key->key, hash, b[0].field[i].data, b[0].field[i + 1].data);
	}
	if (outsigkeyind < 0) {
		hash_for_signature(b, defkey, hash);
		cheatcoin_sign(defkey->key, hash, b[0].field[i].data, b[0].field[i + 1].data);
	}
	if (mining) {
		cheatcoin_amount_t min_nonce;
		cheatcoin_generate_random_array(b[0].field[CHEATCOIN_BLOCK_FIELDS - 1].data, sizeof(cheatcoin_hash_t));
		j = 0; do {
			cheatcoin_hash(b, sizeof(struct cheatcoin_block), hash);
			if (!j || cheatcoin_cmphash(hash, min_hash) < 0) {
				memcpy(min_hash, hash, sizeof(cheatcoin_hash_t));
				min_nonce = b[0].field[CHEATCOIN_BLOCK_FIELDS - 1].amount; j = 1;
			}
			b[0].field[CHEATCOIN_BLOCK_FIELDS - 1].amount++;
			if (pretop != pretop_block() && get_timestamp() < send_time) {
				pretop = pretop_block();
				cheatcoin_info("Mining: start from beginning because of pre-top block changed");
				goto begin;
			}
		} while (get_timestamp() <= send_time);
		b[0].field[CHEATCOIN_BLOCK_FIELDS - 1].amount = min_nonce;
	} else cheatcoin_hash(b, sizeof(struct cheatcoin_block), min_hash);
	b[0].field[0].transport_header = 1;
	log_block("Create", min_hash, b[0].field[0].time);
	res = cheatcoin_add_block(b);
	if (res > 0) { cheatcoin_send_new_block(b); res = 0; }
	return res;
}

static void *maining_thread(void *arg) {
	for(;;) cheatcoin_create_block(0, 0, 0, 0, get_timestamp() | (MAIN_CHAIN_PERIOD - 1));
	return 0;
}

static int request_blocks(cheatcoin_time_t t, cheatcoin_time_t dt) {
	int i, res;
	if (dt <= MAIN_CHAIN_PERIOD) {
		cheatcoin_time_t t0 = time_limit;
		for (i = 0; cheatcoin_info("QueryB: t=%lx dt=%lx", t, dt),
				i < QUERY_RETRIES && (res = cheatcoin_request_blocks(t, t + dt, &t0, add_block_callback)) < 0; ++i);
		if (res <= 0) return -1;
	} else {
		struct cheatcoin_storage_sum lsums[16], rsums[16];
		if (cheatcoin_load_sums(t, t + dt, lsums) <= 0) return -1;
		cheatcoin_debug("Local : [%s]", cheatcoin_log_array(lsums, 16 * sizeof(struct cheatcoin_storage_sum)));
		for (i = 0; cheatcoin_info("QueryS: t=%lx dt=%lx", t, dt),
				i < QUERY_RETRIES && (res = cheatcoin_request_sums(t, t + dt, rsums)) < 0; ++i);
		if (res <= 0) return -1;
		dt >>= 4;
		cheatcoin_debug("Remote: [%s]", cheatcoin_log_array(rsums, 16 * sizeof(struct cheatcoin_storage_sum)));
		for (i = 0; i < 16; ++i) if (lsums[i].size != rsums[i].size || lsums[i].sum != rsums[i].sum)
			if (request_blocks(t + i * dt, dt)) return -1;
	}
	return 0;
}

/* основной поток, работающий с блоками */
static void *work_thread(void *arg) {
	cheatcoin_time_t t = CHEATCOIN_ERA, st;
	int nmaining = (long)arg;

	/* загрузка блоков из локального хранилища */
	cheatcoin_mess("Loading blocks from local storage...");
	cheatcoin_load_blocks(t, get_timestamp(), &t, add_block_callback);

	/* запуск потоков майнинга */
	cheatcoin_mess("Starting %d maining threads...", nmaining);
	while (nmaining--) {
		pthread_t th;
		pthread_create(&th, 0, maining_thread, 0);
	}

	/* периодическая контрольная загрузка блоков из сети и определение главного блока */
	cheatcoin_mess("Entering main cycle...");
	for (;;) {
		st = get_timestamp();
		if (st - t >= MAIN_CHAIN_PERIOD) t = st, request_blocks(0, 1l << 48);
		if (g_cheatcoin_extstats.nnoref > CHEATCOIN_BLOCK_FIELDS - 5 && !(rand() % MAKE_BLOCK_PERIOD))
			cheatcoin_create_block(0, 0, 0, 0, 0);
		pthread_mutex_lock(&block_mutex);
		check_new_main();
		pthread_mutex_unlock(&block_mutex);
		sleep(1);
	}
	return 0;
}

/* начало регулярной обработки блоков; n_maining_threads - число потоков для майнинга на CPU */
int cheatcoin_blocks_start(int n_maining_threads) {
	pthread_mutexattr_t attr;
	pthread_t t;
	if (g_cheatcoin_testnet) cheatcoin_era = CHEATCOIN_TEST_ERA;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&block_mutex, &attr);
	return pthread_create(&t, 0, work_thread, (void *)(long)n_maining_threads);
}

/* для каждого своего блока вызывается callback */
int cheatcoin_traverse_our_blocks(void *data, int (*callback)(void *data, cheatcoin_hash_t hash,
		cheatcoin_amount_t amount, int n_our_key)) {
	struct block_internal *bi;
	int res = 0;
	pthread_mutex_lock(&block_mutex);
	for (bi = ourfirst; !res && bi; bi = bi->ournext)
		res = (*callback)(data, bi->hash, bi->amount, bi->n_our_key);
	pthread_mutex_unlock(&block_mutex);
	return res;
}

/* возвращает баланс адреса, или всех наших адресов, если hash == 0 */
cheatcoin_amount_t cheatcoin_get_balance(cheatcoin_hash_t hash) {
	struct block_internal *bi;
	if (!hash) return g_balance;
	bi = block_by_hash(hash);
	if (!bi) return 0;
	return bi->amount;
}