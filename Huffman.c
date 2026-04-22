#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA_SIZE   256          
#define MAX_CODE_LEN 256          
#define MAGIC        0x48U        
#define VERSION      0x02U        
#define HUF_OK        0
#define HUF_ERR_IO   -1
#define HUF_ERR_MEM  -2
#define HUF_ERR_FMT  -3
#define HUF_ERR_DATA -4
#define LOG(fmt, ...)  fprintf(stderr, "[huf] " fmt "\n", ##__VA_ARGS__)
#define DIE(fmt, ...)  do { LOG(fmt, ##__VA_ARGS__); exit(1); } while(0)
#define IS_LEAF(n)  ((n)->left == NULL && (n)->right == NULL)


typedef struct Node {
    unsigned char  ch;
    unsigned long  freq;
    struct Node   *left;
    struct Node   *right;
} Node;

typedef struct {
    unsigned char bits[MAX_CODE_LEN];  
    int           len;                 
} Code;

typedef struct {
    Node **data;
    int    size;
    int    cap;
} Heap;

typedef struct {
    FILE         *fp;
    unsigned char buf;       
    int           pos;       
} BW;

typedef struct {
    FILE         *fp;
    unsigned char buf;       
    int           left;      
} BR;

typedef struct {
    unsigned char magic;
    unsigned char version;
    int           unique;                  
    unsigned long freq[ALPHA_SIZE];        
    unsigned long orig_size;               
    unsigned char padding;                 
} Header;


static Heap *heap_new(int cap)
{
    Heap *h  = malloc(sizeof(Heap));
    h->data  = malloc((size_t)cap * sizeof(Node *));
    h->size  = 0;
    h->cap   = cap;
    if (!h || !h->data) DIE("heap_new: out of memory");
    return h;
}

static void heap_free(Heap *h)
{
    if (h) { free(h->data); free(h); }
}

static void hswap(Heap *h, int a, int b)
{
    Node *t = h->data[a];
    h->data[a] = h->data[b];
    h->data[b] = t;
}

static void sift_up(Heap *h, int i)
{
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->data[p]->freq > h->data[i]->freq) {
            hswap(h, p, i);
            i = p;
        } else break;
    }
}

static void sift_down(Heap *h, int i)
{
    for (;;) {
        int l = 2*i+1, r = 2*i+2, s = i;
        if (l < h->size && h->data[l]->freq < h->data[s]->freq) s = l;
        if (r < h->size && h->data[r]->freq < h->data[s]->freq) s = r;
        if (s == i) break;
        hswap(h, i, s);
        i = s;
    }
}

static void heap_push(Heap *h, Node *n)
{
    if (h->size >= h->cap) DIE("heap_push: overflow");
    h->data[h->size++] = n;
    sift_up(h, h->size - 1);
}

static Node *heap_pop(Heap *h)
{
    if (h->size == 0) return NULL;
    Node *m = h->data[0];
    h->data[0] = h->data[--h->size];
    if (h->size > 0) sift_down(h, 0);
    return m;
}


static Node *node_new(unsigned char ch, unsigned long freq)
{
    Node *n = calloc(1, sizeof(Node));
    if (!n) DIE("node_new: out of memory");
    n->ch   = ch;
    n->freq = freq;
    return n;
}

static Node *tree_build(unsigned long freq[ALPHA_SIZE])
{
    Heap *h = heap_new(ALPHA_SIZE * 2);

    for (int i = 0; i < ALPHA_SIZE; i++) {
        if (freq[i]) heap_push(h, node_new((unsigned char)i, freq[i]));
    }

    if (h->size == 0) { heap_free(h); return NULL; }

    if (h->size == 1) {
        Node *only = heap_pop(h);
        Node *root = node_new(0, only->freq);
        root->left = only;
        heap_free(h);
        return root;
    }

    while (h->size > 1) {
        Node *a = heap_pop(h);
        Node *b = heap_pop(h);
        Node *m = node_new(0, a->freq + b->freq);
        m->left  = a;
        m->right = b;
        heap_push(h, m);
    }

    Node *root = heap_pop(h);
    heap_free(h);
    return root;
}

static void gen_codes_dfs(Node *n, unsigned char *buf, int depth,
                           Code table[ALPHA_SIZE])
{
    if (!n) return;

    if (IS_LEAF(n)) {
        if (depth == 0) { buf[0] = '0'; depth = 1; }
        memcpy(table[n->ch].bits, buf, (size_t)depth);
        table[n->ch].bits[depth] = '\0';
        table[n->ch].len         = depth;
        return;
    }

    if (n->left)  { buf[depth] = '0'; gen_codes_dfs(n->left,  buf, depth+1, table); }
    if (n->right) { buf[depth] = '1'; gen_codes_dfs(n->right, buf, depth+1, table); }
}

static void gen_codes(Node *root, Code table[ALPHA_SIZE])
{
    memset(table, 0, sizeof(Code) * ALPHA_SIZE);
    if (!root) return;
    unsigned char buf[MAX_CODE_LEN];
    gen_codes_dfs(root, buf, 0, table);
}

static void tree_free(Node *n)
{
    if (!n) return;
    tree_free(n->left);
    tree_free(n->right);
    free(n);
}

static unsigned long tree_wpl(const Node *n, int d)
{
    if (!n) return 0;
    if (IS_LEAF(n)) return (unsigned long)d * n->freq;
    return tree_wpl(n->left, d+1) + tree_wpl(n->right, d+1);
}

static int tree_depth(const Node *n)
{
    if (!n || IS_LEAF(n)) return 0;
    int l = tree_depth(n->left);
    int r = tree_depth(n->right);
    return 1 + (l > r ? l : r);
}

static BW *bw_open(FILE *fp)
{
    BW *bw = calloc(1, sizeof(BW));
    if (!bw) DIE("bw_open: out of memory");
    bw->fp  = fp;
    bw->buf = 0;
    bw->pos = 0;
    return bw;
}

static void bw_write(BW *bw, int bit)
{
    bw->buf = (unsigned char)((bw->buf << 1) | (bit & 1));
    if (++bw->pos == 8) {
        fputc(bw->buf, bw->fp);
        bw->buf = 0;
        bw->pos = 0;
    }
}

static int bw_flush(BW *bw)
{
    int pad = 0;
    if (bw->pos > 0) {
        pad     = 8 - bw->pos;
        bw->buf = (unsigned char)(bw->buf << pad);
        fputc(bw->buf, bw->fp);
        bw->buf = 0;
        bw->pos = 0;
    }
    return pad;
}

static void bw_free(BW *bw) { if (bw) free(bw); }

static BR *br_open(FILE *fp)
{
    BR *br = calloc(1, sizeof(BR));
    if (!br) DIE("br_open: out of memory");
    br->fp   = fp;
    br->buf  = 0;
    br->left = 0;
    return br;
}

static int br_read(BR *br)
{
    if (br->left == 0) {
        int c = fgetc(br->fp);
        if (c == EOF) return -1;
        br->buf  = (unsigned char)c;
        br->left = 8;
    }
    return (br->buf >> --br->left) & 1;
}

static void br_free(BR *br) { if (br) free(br); }

static int header_write(FILE *fp, const Header *h)
{
    if (fputc(h->magic,   fp) == EOF) return HUF_ERR_IO;
    if (fputc(h->version, fp) == EOF) return HUF_ERR_IO;

    if (fwrite(&h->unique, sizeof(int), 1, fp) != 1) return HUF_ERR_IO;

    for (int i = 0; i < ALPHA_SIZE; i++) {
        if (h->freq[i] > 0) {
            unsigned char c = (unsigned char)i;
            if (fwrite(&c,        1,                    1, fp) != 1) return HUF_ERR_IO;
            if (fwrite(&h->freq[i], sizeof(unsigned long), 1, fp) != 1) return HUF_ERR_IO;
        }
    }

    if (fwrite(&h->orig_size, sizeof(unsigned long), 1, fp) != 1) return HUF_ERR_IO;

    unsigned char pad = 0;                      
    if (fwrite(&pad, 1, 1, fp) != 1) return HUF_ERR_IO;

    return HUF_OK;
}

static int header_read(FILE *fp, Header *h)
{
    memset(h, 0, sizeof(Header));

    int m = fgetc(fp);
    if (m == EOF || (unsigned char)m != MAGIC) {
        LOG("header_read: bad magic 0x%02X", (unsigned)m);
        return HUF_ERR_FMT;
    }
    h->magic = (unsigned char)m;

    int v = fgetc(fp);
    if (v == EOF || (unsigned char)v != VERSION) {
        LOG("header_read: unsupported version 0x%02X", (unsigned)v);
        return HUF_ERR_FMT;
    }
    h->version = (unsigned char)v;

    if (fread(&h->unique, sizeof(int), 1, fp) != 1) return HUF_ERR_IO;
    if (h->unique < 1 || h->unique > ALPHA_SIZE) {
        LOG("header_read: invalid unique count %d", h->unique);
        return HUF_ERR_FMT;
    }

    for (int i = 0; i < h->unique; i++) {
        unsigned char c; unsigned long f;
        if (fread(&c, 1,                    1, fp) != 1) return HUF_ERR_IO;
        if (fread(&f, sizeof(unsigned long), 1, fp) != 1) return HUF_ERR_IO;
        h->freq[c] = f;
    }

    if (fread(&h->orig_size, sizeof(unsigned long), 1, fp) != 1) return HUF_ERR_IO;

    int p = fgetc(fp);
    if (p == EOF) return HUF_ERR_IO;
    h->padding = (unsigned char)p;

    return HUF_OK;
}

static void header_inspect(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { LOG("inspect: cannot open %s", path); return; }

    Header h;
    if (header_read(fp, &h) != HUF_OK) {
        LOG("inspect: bad header in %s", path);
        fclose(fp);
        return;
    }
    fseek(fp, 0, SEEK_END);
    unsigned long fsize = (unsigned long)ftell(fp);
    fclose(fp);

    printf("─────────────────────────────────────────────\n");
    printf("  File      : %s\n",    path);
    printf("  Magic     : 0x%02X\n", h.magic);
    printf("  Version   : 0x%02X\n", h.version);
    printf("  Unique    : %d chars\n", h.unique);
    printf("  Orig size : %lu bytes\n", h.orig_size);
    printf("  Padding   : %d bits\n", h.padding);
    printf("  File size : %lu bytes\n", fsize);
    printf("  Ratio     : %.1f%%\n", fsize*100.0/h.orig_size);
    printf("\n  Char  Freq\n");
    for (int i = 0; i < ALPHA_SIZE; i++) {
        if (h.freq[i]) {
            if (i >= 32 && i < 127)
                printf("   '%c'   %lu\n", (char)i, h.freq[i]);
            else
                printf("  0x%02X   %lu\n", i, h.freq[i]);
        }
    }
    printf("─────────────────────────────────────────────\n");
}

static void print_stats(const char *src, const char *dst,
                         unsigned long orig, unsigned long comp)
{
    double ratio   = orig ? 100.0 * comp / orig : 100.0;
    double savings = 100.0 - ratio;
    printf("─────────────────────────────────────────────\n");
    printf("  Source    : %s\n",   src);
    printf("  Output    : %s\n",   dst);
    printf("  Original  : %lu bytes\n", orig);
    printf("  Compressed: %lu bytes\n", comp);
    printf("  Ratio     : %.1f%%  (%.1f%% savings)\n", ratio, savings);
    printf("─────────────────────────────────────────────\n");
}

static int compress(const char *src, const char *dst)
{
    FILE *fin = fopen(src, "rb");
    if (!fin) { LOG("compress: cannot open '%s'", src); return HUF_ERR_IO; }

    fseek(fin, 0, SEEK_END);
    unsigned long fsize = (unsigned long)ftell(fin);
    rewind(fin);

    if (fsize == 0) {
        fclose(fin);
        LOG("compress: '%s' is empty", src);
        return HUF_ERR_IO;
    }

    unsigned char *data = malloc(fsize);
    if (!data) { fclose(fin); LOG("compress: OOM"); return HUF_ERR_MEM; }
    if (fread(data, 1, fsize, fin) != fsize) {
        fclose(fin); free(data);
        LOG("compress: read error");
        return HUF_ERR_IO;
    }
    fclose(fin);

    unsigned long freq[ALPHA_SIZE];
    memset(freq, 0, sizeof(freq));
    for (unsigned long i = 0; i < fsize; i++) freq[data[i]]++;

    int unique = 0;
    for (int i = 0; i < ALPHA_SIZE; i++) if (freq[i]) unique++;

    Node *root = tree_build(freq);
    if (!root) { free(data); LOG("compress: tree_build failed"); return HUF_ERR_DATA; }

    Code table[ALPHA_SIZE];
    gen_codes(root, table);

    LOG("tree: depth=%d  wpl=%lu bits", tree_depth(root), tree_wpl(root, 0));
    tree_free(root);

    FILE *fout = fopen(dst, "wb");
    if (!fout) { free(data); LOG("compress: cannot create '%s'", dst); return HUF_ERR_IO; }

    Header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = MAGIC;
    hdr.version = VERSION;
    hdr.unique  = unique;
    hdr.orig_size = fsize;
    memcpy(hdr.freq, freq, sizeof(freq));

    if (header_write(fout, &hdr) != HUF_OK) {
        fclose(fout); free(data);
        LOG("compress: header write failed");
        return HUF_ERR_IO;
    }

    long pad_offset = ftell(fout) - 1L;

    BW *bw = bw_open(fout);

    for (unsigned long i = 0; i < fsize; i++) {
        const Code *c = &table[data[i]];
        for (int b = 0; b < c->len; b++)
            bw_write(bw, c->bits[b] - '0');    
    }

    int padding = bw_flush(bw);
    bw_free(bw);
    free(data);

    fseek(fout, pad_offset, SEEK_SET);
    unsigned char padbyte = (unsigned char)padding;
    fwrite(&padbyte, 1, 1, fout);

    fseek(fout, 0, SEEK_END);
    unsigned long comp_size = (unsigned long)ftell(fout);
    fclose(fout);

    print_stats(src, dst, fsize, comp_size);
    return HUF_OK;
}

static int decompress(const char *src, const char *dst)
{
    FILE *fin = fopen(src, "rb");
    if (!fin) { LOG("decompress: cannot open '%s'", src); return HUF_ERR_IO; }

    Header hdr;
    int rc = header_read(fin, &hdr);
    if (rc != HUF_OK) {
        fclose(fin);
        LOG("decompress: invalid header in '%s'", src);
        return rc;
    }

    Node *root = tree_build(hdr.freq);
    if (!root) {
        fclose(fin);
        LOG("decompress: tree_build returned NULL");
        return HUF_ERR_DATA;
    }

    FILE *fout = fopen(dst, "wb");
    if (!fout) {
        fclose(fin); tree_free(root);
        LOG("decompress: cannot create '%s'", dst);
        return HUF_ERR_IO;
    }

    BR    *br      = br_open(fin);
    Node  *cur     = root;
    unsigned long decoded = 0;
    int    corrupt = 0;

    while (decoded < hdr.orig_size) {
        int bit = br_read(br);
        if (bit == -1) {
            LOG("decompress: unexpected EOF at %lu/%lu", decoded, hdr.orig_size);
            corrupt = 1;
            break;
        }

        cur = (bit == 0) ? cur->left : cur->right;

        if (!cur) {
            LOG("decompress: NULL node at decoded=%lu", decoded);
            corrupt = 1;
            break;
        }

        if (IS_LEAF(cur)) {
            if (fputc(cur->ch, fout) == EOF) {
                LOG("decompress: write error at decoded=%lu", decoded);
                corrupt = 1;
                break;
            }
            decoded++;
            cur = root;    
        }
    }

    br_free(br);
    fclose(fin);
    fclose(fout);
    tree_free(root);

    if (!corrupt && decoded == hdr.orig_size) {
        printf("─────────────────────────────────────────────\n");
        printf("  Source    : %s\n", src);
        printf("  Output    : %s\n", dst);
        printf("  Decoded   : %lu bytes — OK\n", decoded);
        printf("─────────────────────────────────────────────\n");
        return HUF_OK;
    }

    LOG("decompress: only %lu/%lu bytes recovered", decoded, hdr.orig_size);
    return HUF_ERR_DATA;
}

static int verify(const char *orig_path, const char *rest_path)
{
    FILE *f1 = fopen(orig_path, "rb");
    FILE *f2 = fopen(rest_path, "rb");

    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        LOG("verify: cannot open files");
        return HUF_ERR_IO;
    }

    unsigned long pos = 0;
    int mismatch = 0;

    for (;;) {
        int c1 = fgetc(f1);
        int c2 = fgetc(f2);
        if (c1 == EOF && c2 == EOF) break;
        if (c1 != c2) {
            LOG("verify: mismatch at byte %lu (0x%02X vs 0x%02X)",
                pos,
                c1 == EOF ? 0xFF : (unsigned char)c1,
                c2 == EOF ? 0xFF : (unsigned char)c2);
            mismatch = 1;
            break;
        }
        pos++;
    }

    fclose(f1);
    fclose(f2);

    if (!mismatch) {
        printf("verify: PASS — %lu bytes identical\n", pos);
        return HUF_OK;
    }
    LOG("verify: FAIL");
    return HUF_ERR_DATA;
}

static int print_codes(const char *path)
{
    FILE *fin = fopen(path, "rb");
    if (!fin) { LOG("print_codes: cannot open '%s'", path); return HUF_ERR_IO; }

    fseek(fin, 0, SEEK_END);
    unsigned long fsize = (unsigned long)ftell(fin);
    rewind(fin);

    if (fsize == 0) { fclose(fin); LOG("print_codes: empty file"); return HUF_ERR_IO; }

    unsigned char *data = malloc(fsize);
    if (!data) { fclose(fin); return HUF_ERR_MEM; }
    fread(data, 1, fsize, fin);
    fclose(fin);

    unsigned long freq[ALPHA_SIZE];
    memset(freq, 0, sizeof(freq));
    for (unsigned long i = 0; i < fsize; i++) freq[data[i]]++;
    free(data);

    Node *root = tree_build(freq);
    Code  table[ALPHA_SIZE];
    gen_codes(root, table);

    printf("─────────────────────────────────────────────\n");
    printf("  %-6s  %-8s  %-6s  %-20s  %s\n",
           "char", "freq", "freq%", "code", "savings/byte");
    printf("─────────────────────────────────────────────\n");

    for (int i = 0; i < ALPHA_SIZE; i++) {
        if (!freq[i]) continue;
        double pct = 100.0 * freq[i] / fsize;
        int    saving = 8 - table[i].len;
        char   label[8];
        if (i >= 32 && i < 127) snprintf(label, sizeof(label), "'%c'", (char)i);
        else                     snprintf(label, sizeof(label), "x%02X", i);

        printf("  %-6s  %-8lu  %5.1f%%  %-20s  %+d bits\n",
               label, freq[i], pct,
               (char *)table[i].bits, saving);
    }

    unsigned long wpl = tree_wpl(root, 0);
    printf("─────────────────────────────────────────────\n");
    printf("  Total encoded bits : %lu\n", wpl);
    printf("  Encoded bytes      : %lu\n", (wpl + 7) / 8);
    printf("  Original bytes     : %lu\n", fsize);
    printf("  Estimated ratio    : %.1f%%\n",
           100.0 * ((wpl + 7) / 8) / fsize);
    printf("─────────────────────────────────────────────\n");

    tree_free(root);
    return HUF_OK;
}


static void usage(const char *prog)
{
    printf("\n  Huffman Compression Tool\n");
    printf("  ──────────────────────────────────────────────\n");
    printf("  %s c  <input>   <output.huf>   compress\n", prog);
    printf("  %s d  <input.huf>  <output>   decompress\n", prog);
    printf("  %s v  <original>  <restored>   verify\n", prog);
    printf("  %s i  <file.huf>               inspect header\n", prog);
    printf("  %s p  <input>                  print code table\n", prog);
    printf("\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 0; }

    char mode = argv[1][0];

    if (mode == 'c') {
        if (argc < 4) { usage(argv[0]); return 1; }
        return compress(argv[2], argv[3])  == HUF_OK ? 0 : 1;
    }
    if (mode == 'd') {
        if (argc < 4) { usage(argv[0]); return 1; }
        return decompress(argv[2], argv[3]) == HUF_OK ? 0 : 1;
    }
    if (mode == 'v') {
        if (argc < 4) { usage(argv[0]); return 1; }
        return verify(argv[2], argv[3])     == HUF_OK ? 0 : 1;
    }
    if (mode == 'i') {
        if (argc < 3) { usage(argv[0]); return 1; }
        header_inspect(argv[2]);
        return 0;
    }
    if (mode == 'p') {
        if (argc < 3) { usage(argv[0]); return 1; }
        return print_codes(argv[2])         == HUF_OK ? 0 : 1;
    }

    fprintf(stderr, "unknown mode '%c'\n", mode);
    usage(argv[0]);
    return 1;
}
