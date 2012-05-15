#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <zlib.h>
#include "endian.h"
#include "bgzf.h"
#include "sam.h"

#include "khash.h"
KHASH_DECLARE(s2i, kh_cstr_t, int64_t)

typedef khash_t(s2i) sdict_t;

/**********************
 *** BAM header I/O ***
 **********************/

bam_hdr_t *bam_hdr_init()
{
	return (bam_hdr_t*)calloc(1, sizeof(bam_hdr_t));
}

void bam_hdr_destroy(bam_hdr_t *h)
{
	int32_t i;
	if (h == 0) return;
	if (h->target_name) {
		for (i = 0; i < h->n_targets; ++i)
			free(h->target_name[i]);
		free(h->target_name);
		free(h->target_len);
	}
	free(h->text); free(h->cigar_tab);
	if (h->sdict) kh_destroy(s2i, (sdict_t*)h->sdict);
	free(h);
}

static bam_hdr_t *hdr_from_dict(sdict_t *d)
{
	bam_hdr_t *h;
	khint_t k;
	h = bam_hdr_init();
	h->sdict = d;
	h->n_targets = kh_size(d);
	h->target_len = (uint32_t*)malloc(4 * h->n_targets);
	h->target_name = (char**)malloc(sizeof(void*) * h->n_targets);
	for (k = kh_begin(d); k != kh_end(d); ++k) {
		if (!kh_exist(d, k)) continue;
		h->target_name[kh_val(d, k)>>32] = (char*)kh_key(d, k);
		h->target_len[kh_val(d, k)>>32]  = kh_val(d, k)<<32>>32;
		kh_val(d, k) >>= 32;
	}
	return h;
}

bam_hdr_t *bam_hdr_read(void *_fp)
{
	bam_hdr_t *h;
	BGZF *fp = (BGZF*)_fp;
	char buf[4];
	int magic_len, has_EOF;
	int32_t i = 1, name_len;
	// check EOF
	has_EOF = bgzf_check_EOF(fp);
	if (has_EOF < 0) {
		if (errno != ESPIPE && hts_verbose >= 2) perror("[W::sam_hdr_read] bgzf_check_EOF");
	} else if (has_EOF == 0 && hts_verbose >= 2)
		fprintf(stderr, "[W::%s] EOF marker is absent. The input is probably truncated.\n", __func__);
	// read "BAM1"
	magic_len = bgzf_read(fp, buf, 4);
	if (magic_len != 4 || strncmp(buf, "BAM\1", 4)) {
		if (hts_verbose >= 1) fprintf(stderr, "[E::%s] invalid BAM binary header\n", __func__);
		return 0;
	}
	h = bam_hdr_init();
	// read plain text and the number of reference sequences
	bgzf_read(fp, &h->l_text, 4);
	if (fp->is_be) ed_swap_4p(&h->l_text);
	h->text = (char*)malloc(h->l_text + 1);
	h->text[h->l_text] = 0; // make sure it is NULL terminated
	bgzf_read(fp, h->text, h->l_text);
	bgzf_read(fp, &h->n_targets, 4);
	if (fp->is_be) ed_swap_4p(&h->n_targets);
	// read reference sequence names and lengths
	h->target_name = (char**)calloc(h->n_targets, sizeof(char*));
	h->target_len = (uint32_t*)calloc(h->n_targets, 4);
	for (i = 0; i != h->n_targets; ++i) {
		bgzf_read(fp, &name_len, 4);
		if (fp->is_be) ed_swap_4p(&name_len);
		h->target_name[i] = (char*)calloc(name_len, 1);
		bgzf_read(fp, h->target_name[i], name_len);
		bgzf_read(fp, &h->target_len[i], 4);
		if (fp->is_be) ed_swap_4p(&h->target_len[i]);
	}
	return h;
}

int bam_hdr_write(void *_fp, const bam_hdr_t *h)
{
	char buf[4];
	int32_t i, name_len, x;
	BGZF *fp = (BGZF*)_fp;
	// write "BAM1"
	strncpy(buf, "BAM\1", 4);
	bgzf_write(fp, buf, 4);
	// write plain text and the number of reference sequences
	if (fp->is_be) {
		x = ed_swap_4(h->l_text);
		bgzf_write(fp, &x, 4);
		if (h->l_text) bgzf_write(fp, h->text, h->l_text);
		x = ed_swap_4(h->n_targets);
		bgzf_write(fp, &x, 4);
	} else {
		bgzf_write(fp, &h->l_text, 4);
		if (h->l_text) bgzf_write(fp, h->text, h->l_text);
		bgzf_write(fp, &h->n_targets, 4);
	}
	// write sequence names and lengths
	for (i = 0; i != h->n_targets; ++i) {
		char *p = h->target_name[i];
		name_len = strlen(p) + 1;
		if (fp->is_be) {
			x = ed_swap_4(name_len);
			bgzf_write(fp, &x, 4);
		} else bgzf_write(fp, &name_len, 4);
		bgzf_write(fp, p, name_len);
		if (fp->is_be) {
			x = ed_swap_4(h->target_len[i]);
			bgzf_write(fp, &x, 4);
		} else bgzf_write(fp, &h->target_len[i], 4);
	}
	bgzf_flush(fp);
	return 0;
}

int bam_get_tid(bam_hdr_t *h, const char *ref)
{
	sdict_t *d = (sdict_t*)h->sdict;
	khint_t k;
	if (h->sdict == 0) {
		int i, absent;
		d = kh_init(s2i);
		for (i = 0; i < h->n_targets; ++i) {
			k = kh_put(s2i, d, h->target_name[i], &absent);
			kh_val(d, k) = i;
		}
		h->sdict = d;
	}
	k = kh_get(s2i, d, ref);
	return k == kh_end(d)? -1 : kh_val(d, k);
}

/*************************
 *** BAM alignment I/O ***
 *************************/

bam1_t *bam_init1()
{
	return (bam1_t*)calloc(1, sizeof(bam1_t));
}

void bam_destroy1(bam1_t *b)
{
	if (b == 0) return;
	free(b->data); free(b);
}

int bam_cigar2qlen(int n_cigar, const uint32_t *cigar)
{
	int k, l;
	for (k = l = 0; k < n_cigar; ++k)
		if (bam_cigar_type(bam_cigar_op(cigar[k]))&1)
			l += bam_cigar_oplen(cigar[k]);
	return l;
}

int bam_cigar2rlen(int n_cigar, const uint32_t *cigar)
{
	int k, l;
	for (k = l = 0; k < n_cigar; ++k)
		if (bam_cigar_type(bam_cigar_op(cigar[k]))&2)
			l += bam_cigar_oplen(cigar[k]);
	return l;
}

static inline int aux_type2size(int x)
{
	if (x == 'C' || x == 'c' || x == 'A') return 1;
	else if (x == 'S' || x == 's') return 2;
	else if (x == 'I' || x == 'i' || x == 'f') return 4;
	else return 0;
}

static void swap_data(const bam1_core_t *c, int l_data, uint8_t *data)
{
	uint8_t *s;
	uint32_t *cigar = (uint32_t*)(data + c->l_qname);
	int i;
	s = data + c->n_cigar*4 + c->l_qname + c->l_qseq + (c->l_qseq + 1)/2;
	for (i = 0; i < c->n_cigar; ++i) ed_swap_4p(&cigar[i]);
	while (s < data + l_data) {
		uint8_t type;
		s += 2; // skip key
		type = toupper(*s); ++s; // skip type
		if (type == 'C' || type == 'A') ++s;
		else if (type == 'S') { ed_swap_2p(s); s += 2; }
		else if (type == 'I' || type == 'F') { ed_swap_4p(s); s += 4; }
		else if (type == 'D') { ed_swap_8p(s); s += 8; }
		else if (type == 'Z' || type == 'H') { while (*s) ++s; ++s; }
		else if (type == 'B') {
			int32_t n, Bsize = aux_type2size(*s);
			memcpy(&n, s + 1, 4);
			if (Bsize == 2) {
				for (i = 0; i < n; i += 2)
					ed_swap_2p(s + 5 + i);
			} else if (Bsize == 4) {
				for (i = 0; i < n; i += 4)
					ed_swap_4p(s + 5 + i);
			}
			ed_swap_4p(s+1); 
		}
	}
}

int bam_read1(void *_fp, bam1_t *b)
{
	BGZF *fp = (BGZF*)_fp;
	bam1_core_t *c = &b->core;
	int32_t block_len, ret, i;
	uint32_t x[8];

	if ((ret = bgzf_read(fp, &block_len, 4)) != 4) {
		if (ret == 0) return -1; // normal end-of-file
		else return -2; // truncated
	}
	if (bgzf_read(fp, x, 32) != 32) return -3;
	if (fp->is_be) {
		ed_swap_4p(&block_len);
		for (i = 0; i < 8; ++i) ed_swap_4p(x + i);
	}
	c->tid = x[0]; c->pos = x[1];
	c->bin = x[2]>>16; c->qual = x[2]>>8&0xff; c->l_qname = x[2]&0xff;
	c->flag = x[3]>>16; c->n_cigar = x[3]&0xffff;
	c->l_qseq = x[4];
	c->mtid = x[5]; c->mpos = x[6]; c->isize = x[7];
	b->l_data = block_len - 32;
	if (b->m_data < b->l_data) {
		b->m_data = b->l_data;
		kroundup32(b->m_data);
		b->data = (uint8_t*)realloc(b->data, b->m_data);
	}
	if (bgzf_read(fp, b->data, b->l_data) != b->l_data) return -4;
	//b->l_aux = b->l_data - c->n_cigar * 4 - c->l_qname - c->l_qseq - (c->l_qseq+1)/2;
	if (fp->is_be) swap_data(c, b->l_data, b->data);
	return 4 + block_len;
}

int bam_write1(void *_fp, const bam1_t *b)
{
	BGZF *fp = (BGZF*)_fp;
	const bam1_core_t *c = &b->core;
	uint32_t x[8], block_len = b->l_data + 32, y;
	int i;
	x[0] = c->tid;
	x[1] = c->pos;
	x[2] = (uint32_t)c->bin<<16 | c->qual<<8 | c->l_qname;
	x[3] = (uint32_t)c->flag<<16 | c->n_cigar;
	x[4] = c->l_qseq;
	x[5] = c->mtid;
	x[6] = c->mpos;
	x[7] = c->isize;
	bgzf_flush_try(fp, 4 + block_len);
	if (fp->is_be) {
		for (i = 0; i < 8; ++i) ed_swap_4p(x + i);
		y = block_len;
		bgzf_write(fp, ed_swap_4p(&y), 4);
		swap_data(c, b->l_data, b->data);
	} else bgzf_write(fp, &block_len, 4);
	bgzf_write(fp, x, 32);
	bgzf_write(fp, b->data, b->l_data);
	if (fp->is_be) swap_data(c, b->l_data, b->data);
	return 4 + block_len;
}

/********************
 *** BAM indexing ***
 ********************/

bam_idx_t *bam_index(void *_fp)
{
	BGZF *fp = (BGZF*)_fp;
	bam1_t *b;
	hts_idx_t *idx;
	bam_hdr_t *h;
	h = bam_hdr_read(fp);
	idx = hts_idx_init(h->n_targets, bgzf_tell(fp));
	bam_hdr_destroy(h);
	b = bam_init1();
	while (bam_read1(fp, b) >= 0) {
		int l;
		l = bam_cigar2rlen(b->core.n_cigar, bam_get_cigar(b));
		hts_idx_push(idx, b->core.tid, b->core.pos, b->core.pos + l, bgzf_tell(fp), b->core.bin, !(b->core.flag&BAM_FUNMAP));
	}
	hts_idx_finish(idx, bgzf_tell(fp));
	bam_destroy1(b);
	return idx;
}

int bam_index_build(const char *fn, const char *_fnidx)
{
	char *fnidx;
	FILE *fpidx;
	BGZF *fp;
	bam_idx_t *idx;

	if ((fp = bgzf_open(fn, "r")) == 0) return -1;
	idx = bam_index(fp);
	bgzf_close(fp);
	if (_fnidx == 0) {
		fnidx = (char*)malloc(strlen(fn) + 5);
		strcat(strcpy(fnidx, fn), ".bai");
	} else fnidx = strdup(_fnidx);
	if ((fpidx = fopen(fnidx, "wb")) == 0) {
		if (hts_verbose >= 1) fprintf(stderr, "[E::%s] fail to create the index file\n", __func__);
		return -1;
	}
	free(fnidx);
	fwrite("BAI\1", 1, 4, fpidx);
	hts_idx_save(idx, fpidx, 0);
	fclose(fpidx);
	hts_idx_destroy(idx);
	return 0;
}

bam_idx_t *bam_index_load_local(const char *fnidx)
{
	bam_idx_t *idx;
	FILE *fpidx;
	char magic[4];
	if ((fpidx = fopen(fnidx, "rb")) == 0) {
		if (hts_verbose >= 1) fprintf(stderr, "[E::%s] fail to open the index file\n", __func__);
		return 0;
	}
	fread(magic, 1, 4, fpidx);
	idx = hts_idx_load(fpidx, 0);
	fclose(fpidx);
	return idx;
}

static inline int is_overlap(uint32_t beg, uint32_t end, const bam1_t *b)
{
	uint32_t rbeg = b->core.pos;
	uint32_t rend = b->core.n_cigar? rbeg + bam_cigar2rlen(b->core.n_cigar, bam_get_cigar(b)) : rbeg + 1;
	return (rend > beg && rbeg < end);
}

int bam_iter_read(void *_fp, bam_iter_t *iter, bam1_t *b)
{
	BGZF *fp = (BGZF*)_fp;
	int ret;
	if (iter && iter->finished) return -1;
	if (iter == 0 || iter->from_first) {
		ret = bam_read1(fp, b);
		if (ret < 0 && iter) iter->finished = 1;
		return ret;
	}
	if (iter->off == 0) return -1;
	for (;;) {
		if (iter->curr_off == 0 || iter->curr_off >= iter->off[iter->i].v) { // then jump to the next chunk
			if (iter->i == iter->n_off - 1) { ret = -1; break; } // no more chunks
			if (iter->i < 0 || iter->off[iter->i].v != iter->off[iter->i+1].u) { // not adjacent chunks; then seek
				bgzf_seek(fp, iter->off[iter->i+1].u, SEEK_SET);
				iter->curr_off = bgzf_tell(fp);
			}
			++iter->i;
		}
		if ((ret = bam_read1(fp, b)) >= 0) {
			iter->curr_off = bgzf_tell(fp);
			if (b->core.tid != iter->tid || b->core.pos >= iter->end) { // no need to proceed
				ret = -1; break;
			} else if (is_overlap(iter->beg, iter->end, b)) return ret;
		} else break; // end of file or error
	}
	iter->finished = 1;
	return ret;
}

bam_iter_t *bam_iter_querys(bam_idx_t *idx, bam_hdr_t *h, const char *reg)
{
	int tid, beg, end;
	char *q, *tmp;
	if (h == 0 || reg == 0) return hts_iter_query(idx, HTS_IDX_START, 0, 0);
	q = (char*)hts_parse_reg(reg, &beg, &end);
	tmp = (char*)alloca(q - reg + 1);
	strncpy(tmp, reg, q - reg);
	tmp[q - reg] = 0;
	if ((tid = bam_get_tid(h, tmp)) < 0)
		tid = bam_get_tid(h, reg);
	if (tid < 0) return 0;
	return hts_iter_query(idx, tid, beg, end);
}

/**********************
 *** SAM header I/O ***
 **********************/

#include "kseq.h"
#include "kstring.h"

bam_hdr_t *sam_hdr_parse(int l_text, const char *text)
{
	const char *q, *r, *p;
	khash_t(s2i) *d;
	d = kh_init(s2i);
	for (p = text; *p; ++p) {
		if (strncmp(p, "@SQ", 3) == 0) {
			char *sn = 0;
			int ln = -1;
			for (q = p + 4;; ++q) {
				if (strncmp(q, "SN:", 3) == 0) {
					q += 3;
					for (r = q; *r != '\t' && *r != '\n'; ++r);
					sn = (char*)calloc(r - q + 1, 1);
					strncpy(sn, q, r - q);
					q = r;
				} else if (strncmp(q, "LN:", 3) == 0)
					ln = strtol(q + 3, (char**)&q, 10);
				while (*q != '\t' && *q != '\n') ++q;
				if (*q == '\n') break;
			}
			p = q;
			if (sn && ln >= 0) {
				khint_t k;
				int absent;
				k = kh_put(s2i, d, sn, &absent);
				if (!absent) {
					if (hts_verbose >= 2)
						fprintf(stderr, "[W::%s] duplicated sequence '%s'\n", __func__, sn);
					free(sn);
				} else kh_val(d, k) = (int64_t)(kh_size(d) - 1)<<32 | ln;
			}
		}
		while (*p != '\n') ++p;
	}
	return hdr_from_dict(d);
}

bam_hdr_t *sam_hdr_read(htsFile *fp)
{
	if (!fp->is_bin) {
		kstring_t str;
		bam_hdr_t *h;
		str.l = str.m = 0; str.s = 0;
		while (hts_getline(fp, KS_SEP_LINE, &fp->line) >= 0) {
			if (fp->line.s[0] != '@') break;
			kputsn(fp->line.s, fp->line.l, &str);
			kputc('\n', &str);
		}
		h = sam_hdr_parse(str.l, str.s);
		h->l_text = str.l; h->text = str.s;
		return h;
	} else return bam_hdr_read(fp->fp);
}

int sam_hdr_write(htsFile *fp, const bam_hdr_t *h)
{
	if (!fp->is_bin) {
		char *p;
		fputs(h->text, (FILE*)fp->fp);
		p = strstr(h->text, "@SQ\t");
		if (p == h->text || *(p-1) == '\n') {
			int i;
			for (i = 0; i < h->n_targets; ++i) {
				fp->line.l = 0;
				kputsn("@SQ\tSN:", 7, &fp->line); kputs(h->target_name[i], &fp->line);
				kputsn("\tLN:", 4, &fp->line); kputw(h->target_len[i], &fp->line); kputc('\n', &fp->line);
				fwrite(fp->line.s, 1, fp->line.l, (FILE*)fp->fp);
			}
		}
		fflush((FILE*)fp->fp);
	} else bam_hdr_write(fp->fp, h);
	return 0;
}

/**********************
 *** SAM record I/O ***
 **********************/

int sam_parse1(kstring_t *s, bam_hdr_t *h, bam1_t *b)
{
#define _read_token(_p) (_p); for (; *(_p) && *(_p) != '\t'; ++(_p)); if (*(_p) != '\t') goto err_ret; *(_p)++ = 0
#define _read_token_aux(_p) (_p); for (; *(_p) && *(_p) != '\t'; ++(_p)); *(_p)++ = 0 // this is different in that it does not test *(_p)=='\t'
#define _get_mem(type_t, _x, _s, _l) ks_resize((_s), (_s)->l + (_l)); *(_x) = (type_t*)((_s)->s + (_s)->l); (_s)->l += (_l)
#define _parse_err(cond, msg) do { if ((cond) && hts_verbose >= 1) { fprintf(stderr, "[E::%s] " msg "\n", __func__); goto err_ret; } } while (0)
#define _parse_warn(cond, msg) if ((cond) && hts_verbose >= 2) fprintf(stderr, "[W::%s] " msg "\n", __func__)

	uint8_t *t;
	char *p = s->s, *q;
	int i;
	kstring_t str;
	bam1_core_t *c = &b->core;

	str.l = b->l_data = 0;
	str.s = (char*)b->data; str.m = b->m_data;
	memset(c, 0, 32);
	if (h->cigar_tab == 0) {
		h->cigar_tab = (uint8_t*)calloc(128, 1);
		for (i = 0; BAM_CIGAR_STR[i]; ++i)
			h->cigar_tab[(int)BAM_CIGAR_STR[i]] = i;
	}
	// qname
	q = _read_token(p);
	kputsn_(q, p - q, &str);
	c->l_qname = p - q;
	// flag
	c->flag = strtol(p, &p, 0);
	if (*p++ != '\t') goto err_ret; // malformated flag
	// chr
	q = _read_token(p);
	if (strcmp(q, "*")) {
		_parse_err(h->n_targets == 0, "missing SAM header");
		c->tid = bam_get_tid(h, q);
		_parse_warn(c->tid < 0, "urecognized reference name; treated as unmapped");
	} else c->tid = -1;
	// pos
	c->pos = strtol(p, &p, 10) - 1;
	if (*p++ != '\t') goto err_ret;
	if (c->pos < 0 && c->tid >= 0) {
		_parse_warn(1, "mapped query cannot have zero coordinate; treated as unmapped");
		c->tid = -1;
	}
	if (c->tid < 0) c->flag |= BAM_FUNMAP;
	// mapq
	c->qual = strtol(p, &p, 10);
	if (*p++ != '\t') goto err_ret;
	// cigar
	if (*p != '*') {
		uint32_t *cigar;
		for (q = p, c->n_cigar = 0; *q && *q != '\t'; ++q)
			if (!isdigit(*q)) ++c->n_cigar;
		_get_mem(uint32_t, &cigar, &str, c->n_cigar<<2);
		for (i = 0, q = p; i < c->n_cigar; ++i, ++q) {
			int op;
			cigar[i] = strtol(q, &q, 10)<<BAM_CIGAR_SHIFT;
			op = (uint8_t)*q >= 128? -1 : h->cigar_tab[(int)*q];
			_parse_err(op < 0, "urecognized CIGAR operator");
			cigar[i] |= op;
		}
		p = q + 1;
		i = bam_cigar2rlen(c->n_cigar, cigar);
	} else {
		_parse_warn(!(c->flag&BAM_FUNMAP), "mapped query must have a CIGAR; treated as unmapped");
		c->flag |= BAM_FUNMAP;
		q = _read_token(p);
		i = 1;
	}
	c->bin = hts_reg2bin(c->pos, c->pos + i);
	// mate chr
	q = _read_token(p);
	if (strcmp(q, "=") == 0) c->mtid = c->tid;
	else if (strcmp(q, "*") == 0) c->mtid = -1;
	else c->mtid = bam_get_tid(h, q);
	// mpos
	c->mpos = strtol(p, &p, 10) - 1;
	if (*p++ != '\t') goto err_ret;
	if (c->mpos < 0 && c->mtid >= 0) {
		_parse_warn(1, "mapped mate cannot have zero coordinate; treated as unmapped");
		c->mtid = -1;
	}
	// tlen
	c->isize = strtol(p, &p, 10);
	if (*p++ != '\t') goto err_ret;
	// seq
	q = _read_token(p);
	if (strcmp(q, "*")) {
		c->l_qseq = p - q - 1;
		i = bam_cigar2qlen(c->n_cigar, (uint32_t*)(str.s + c->l_qname));
		_parse_err(c->n_cigar && i != c->l_qseq, "CIGAR and query sequence are of different length");
		i = (c->l_qseq + 1) >> 1;
		_get_mem(uint8_t, &t, &str, i);
		memset(t, 0, i);
		for (i = 0; i < c->l_qseq; ++i)
			t[i>>1] |= seq_nt16_table[(int)q[i]] << ((~i&1)<<2);
	} else c->l_qseq = 0;
	// qual
	q = _read_token_aux(p);
	_get_mem(uint8_t, &t, &str, c->l_qseq);
	if (strcmp(q, "*")) {
		_parse_err(p - q - 1 != c->l_qseq, "SEQ and QUAL are of different length");
		for (i = 0; i < c->l_qseq; ++i) t[i] = q[i] - 33;
	} else memset(t, 0xff, c->l_qseq);
	// aux
	while (p < s->s + s->l) {
		uint8_t type;
		q = _read_token_aux(p); // FIXME: can be accelerated for long 'B' arrays
		_parse_err(p - q - 1 < 6, "incomplete aux field");
		kputsn_(q, 2, &str);
		q += 3; type = *q++; ++q; // q points to value
		if (type == 'A' || type == 'a' || type == 'c' || type == 'C') {
			kputc_('A', &str);
			kputc_(*q, &str);
		} else if (type == 'i' || type == 'I') {
			long x;
			x = strtol(q, &q, 10);
			if (x < 0) {
				if (x >= INT8_MIN) {
					kputc_('c', &str); kputc_(x, &str);
				} else if (x >= INT16_MIN) {
					int16_t y = x;
					kputc_('s', &str); kputsn_((char*)&y, 2, &str);
				} else {
					int32_t y = x;
					kputc_('i', &str); kputsn_(&y, 4, &str);
				}
			} else {
				if (x <= UINT8_MAX) {
					kputc_('C', &str); kputc_(x, &str);
				} else if (x <= UINT16_MAX) {
					uint16_t y = x;
					kputc_('S', &str); kputsn_(&y, 2, &str);
				} else {
					uint32_t y = x;
					kputc_('I', &str); kputsn_(&y, 4, &str);
				}
			}
		} else if (type == 'f') {
			float x;
			x = strtod(q, &q);
			kputc_('f', &str); kputsn_(&x, 4, &str);
		} else if (type == 'Z' || type == 'H') {
			kputc_('Z', &str);kputsn_(q, p - q, &str); // note that this include the trailing NULL
		} else if (type == 'B') {
			int32_t n;
			char *r;
			_parse_err(p - q - 1 < 3, "incomplete B-typed aux field");
			type = *q++; // q points to the first ',' following the typing byte
			for (r = q, n = 0; *r; ++r)
				if (*r == ',') ++n;
			kputc_('B', &str); kputc_(type, &str); kputsn_(&n, 4, &str);
			// FIXME: to evaluate which is faster: a) aligned array and then memmove(); b) unaligned array; c) kputsn_()
			if (type == 'c')      while (q + 1 < p) { int8_t   x = strtol(q + 1, &q, 0); kputc_(x, &str); }
			else if (type == 'C') while (q + 1 < p) { uint8_t  x = strtol(q + 1, &q, 0); kputc_(x, &str); }
			else if (type == 's') while (q + 1 < p) { int16_t  x = strtol(q + 1, &q, 0); kputsn_(&x, 2, &str); }
			else if (type == 'S') while (q + 1 < p) { uint16_t x = strtol(q + 1, &q, 0); kputsn_(&x, 2, &str); }
			else if (type == 'i') while (q + 1 < p) { int32_t  x = strtol(q + 1, &q, 0); kputsn_(&x, 4, &str); }
			else if (type == 'I') while (q + 1 < p) { uint32_t x = strtol(q + 1, &q, 0); kputsn_(&x, 4, &str); }
			else if (type == 'f') while (q + 1 < p) { float    x = strtod(q + 1, &q);    kputsn_(&x, 4, &str); }
			else _parse_err(1, "unrecognized type");
		} else _parse_err(1, "unrecognized type");
	}
	b->data = (uint8_t*)str.s; b->l_data = str.l; b->m_data = str.m;
	return 0;

#undef _parse_warn
#undef _parse_err
#undef _get_mem
#undef _read_token_aux
#undef _read_token
err_ret:
	b->data = (uint8_t*)str.s; b->l_data = str.l; b->m_data = str.m;
	return -2;
}

int sam_read1(htsFile *fp, bam_hdr_t *h, bam1_t *b)
{
	if (!fp->is_bin) {
		int ret;
		if (fp->line.l == 0) {
			ret = hts_getline(fp, KS_SEP_LINE, &fp->line);
			if (ret < 0) return -1;
		}
		ret = sam_parse1(&fp->line, h, b);
		if (ret < 0 && hts_verbose >= 1)
			fprintf(stderr, "[W::%s] parse error at line %lld\n", __func__, (long long)fp->lineno);
		fp->line.l = 0;
		return ret;
	} else return bam_read1(fp->fp, b);
}

int sam_format1(const bam_hdr_t *h, const bam1_t *b, kstring_t *str)
{
	int i;
	uint8_t *s;
	const bam1_core_t *c = &b->core;

	str->l = 0;
	kputsn(bam_get_qname(b), c->l_qname-1, str); kputc('\t', str); // query name
	kputw(c->flag, str); kputc('\t', str); // flag
	if (c->tid >= 0) { // chr
		kputs(h->target_name[c->tid] , str);
		kputc('\t', str);
	} else kputsn("*\t", 2, str);
	kputw(c->pos + 1, str); kputc('\t', str); // pos
	kputw(c->qual, str); kputc('\t', str); // qual
	if (c->n_cigar) { // cigar
		uint32_t *cigar = bam_get_cigar(b);
		for (i = 0; i < c->n_cigar; ++i) {
			kputw(bam_cigar_oplen(cigar[i]), str);
			kputc(bam_cigar_opchr(cigar[i]), str);
		}
	} else kputc('*', str);
	kputc('\t', str);
	if (c->mtid < 0) kputsn("*\t", 2, str); // mate chr
	else if (c->mtid == c->tid) kputsn("=\t", 2, str);
	else {
		kputs(h->target_name[c->mtid], str);
		kputc('\t', str);
	}
	kputw(c->mpos + 1, str); kputc('\t', str); // mate pos
	kputw(c->isize, str); kputc('\t', str); // template len
	if (c->l_qseq) { // seq and qual
		uint8_t *s = bam_get_seq(b);
		for (i = 0; i < c->l_qseq; ++i) kputc("=ACMGRSVTWYHKDBN"[bam_seqi(s, i)], str);
		kputc('\t', str);
		s = bam_get_qual(b);
		if (s[0] == 0xff) kputc('*', str);
		else for (i = 0; i < c->l_qseq; ++i) kputc(s[i] + 33, str);
	} else kputsn("*\t*", 3, str);
	s = bam_get_aux(b); // aux
	while (s < b->data + b->l_data) {
		uint8_t type, key[2];
		key[0] = s[0]; key[1] = s[1];
		s += 2; type = *s++;
		kputc('\t', str); kputsn((char*)key, 2, str); kputc(':', str);
		if (type == 'A') { kputsn("A:", 2, str); kputc(*s, str); ++s; }
		else if (type == 'C') { kputsn("i:", 2, str); kputw(*s, str); ++s; }
		else if (type == 'c') { kputsn("i:", 2, str); kputw(*(int8_t*)s, str); ++s; }
		else if (type == 'S') { kputsn("i:", 2, str); kputw(*(uint16_t*)s, str); s += 2; }
		else if (type == 's') { kputsn("i:", 2, str); kputw(*(int16_t*)s, str); s += 2; }
		else if (type == 'I') { kputsn("i:", 2, str); kputuw(*(uint32_t*)s, str); s += 4; }
		else if (type == 'i') { kputsn("i:", 2, str); kputw(*(int32_t*)s, str); s += 4; }
		else if (type == 'f') { ksprintf(str, "f:%g", *(float*)s); s += 4; }
		else if (type == 'd') { ksprintf(str, "d:%g", *(double*)s); s += 8; }
		else if (type == 'Z' || type == 'H') { kputc(type, str); kputc(':', str); while (*s) kputc(*s++, str); ++s; }
		else if (type == 'B') {
			uint8_t sub_type = *(s++);
			int32_t n;
			memcpy(&n, s, 4);
			s += 4; // no point to the start of the array
			kputsn("B:", 2, str); kputc(sub_type, str); // write the typing
			for (i = 0; i < n; ++i) { // FIXME: for better performance, put the loop after "if"
				kputc(',', str);
				if ('c' == sub_type)      { kputw(*(int8_t*)s, str); ++s; }
				else if ('C' == sub_type) { kputw(*(uint8_t*)s, str); ++s; }
				else if ('s' == sub_type) { kputw(*(int16_t*)s, str); s += 2; }
				else if ('S' == sub_type) { kputw(*(uint16_t*)s, str); s += 2; }
				else if ('i' == sub_type) { kputw(*(int32_t*)s, str); s += 4; }
				else if ('I' == sub_type) { kputuw(*(uint32_t*)s, str); s += 4; }
				else if ('f' == sub_type) { ksprintf(str, "%g", *(float*)s); s += 4; }
			}
		}
	}
	return str->l;
}

int sam_write1(htsFile *fp, const bam_hdr_t *h, const bam1_t *b)
{
	if (!fp->is_bin) {
		sam_format1(h, b, &fp->line);
		fwrite(fp->line.s, 1, fp->line.l, (FILE*)fp->fp);
		fputc('\n', (FILE*)fp->fp);
		return fp->line.l + 1;
	} else return bam_write1(fp->fp, b);
}
