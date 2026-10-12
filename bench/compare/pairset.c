/*
 * Exact set comparison for canonical tree TSV rows: t<id> TAB t<id>.
 * A pair maps injectively to (source << bits) | target. Atomic bitmap
 * insertion counts distinct pairs even when workers encounter duplicates.
 * A candidate must contain only reference pairs and the same distinct count.
 * Inputs must be immutable regular files for the duration of the check.
 *
 * Build: cc -O3 -std=c11 -pthread -o pairset pairset.c
 * Usage: pairset [-b node-bits] [-j workers] reference.tsv [candidate.tsv]
 * With no candidate, print the reference's distinct row count.
 */

#include <sys/mman.h>
#include <sys/stat.h>

#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct pair_set pair_set_t;
typedef struct scan_worker scan_worker_t;

struct pair_set {
	_Atomic uint64_t *words;
	size_t word_count;
	unsigned int bits;
};

struct scan_worker {
	const char *input;
	size_t start;
	size_t end;
	pair_set_t *seen;
	const pair_set_t *reference;
	uint64_t distinct;
	size_t error_offset;
	const char *error;
};

static unsigned int positive_option(const char *, unsigned int);
static pair_set_t allocate_set(unsigned int);
static bool parse_node(const char **, const char *, unsigned int, uint64_t *);
static bool parse_pair(const char **, const char *, unsigned int, uint64_t *);
static void *scan_pairs(void *);
static size_t row_boundary(const char *, size_t, size_t);
static bool scan_file(const char *, unsigned int, pair_set_t *,
    const pair_set_t *, uint64_t *);

static unsigned int
positive_option(const char *text, unsigned int maximum)
{
	char *end;
	unsigned long value;

	if (*text < '1' || *text > '9')
		errx(2, "invalid positive integer: %s", text);
	value = strtoul(text, &end, 10);
	if (*end != '\0' || value == 0 || value > maximum)
		errx(2, "integer out of range: %s", text);
	return ((unsigned int)value);
}

static pair_set_t
allocate_set(unsigned int bits)
{
	pair_set_t set;
	size_t i;

	set.bits = bits;
	set.word_count = ((size_t)1 << (2 * bits)) / 64;
	if (set.word_count == 0)
		set.word_count = 1;
	set.words = calloc(set.word_count, sizeof(*set.words));
	if (set.words == NULL)
		err(2, "allocate bitmap");
	for (i = 0; i < set.word_count; i++)
		atomic_init(&set.words[i], 0);
	if (!atomic_is_lock_free(set.words))
		errx(2, "64-bit bitmap atomics must be lock-free");
	return (set);
}

/* Reject alternate spellings so numeric encoding preserves TSV equality. */
static bool
parse_node(const char **cursor, const char *end, unsigned int bits,
    uint64_t *node)
{
	const char *p;
	uint64_t value, maximum;

	p = *cursor;
	if (p == end || *p++ != 't' || p == end || *p < '0' || *p > '9')
		return (false);
	if (*p == '0' && p + 1 < end && p[1] >= '0' && p[1] <= '9')
		return (false);
	maximum = ((uint64_t)1 << bits) - 1;
	value = 0;
	while (p < end && *p >= '0' && *p <= '9') {
		value = value * 10 + (unsigned int)(*p++ - '0');
		if (value > maximum)
			return (false);
	}
	*node = value;
	*cursor = p;
	return (true);
}

/* Consume one whole row before publishing its injective pair index. */
static bool
parse_pair(const char **cursor, const char *end, unsigned int bits,
    uint64_t *pair)
{
	const char *p;
	uint64_t source, target;

	p = *cursor;
	if (!parse_node(&p, end, bits, &source))
		return (false);
	if (p == end || *p != '\t')
		return (false);
	p++;
	if (!parse_node(&p, end, bits, &target))
		return (false);
	if (p < end) {
		if (*p != '\n')
			return (false);
		p++;
	}
	*pair = (source << bits) | target;
	*cursor = p;
	return (true);
}

static void *
scan_pairs(void *argument)
{
	scan_worker_t *worker;
	const char *p, *end;
	uint64_t pair, mask, previous;
	size_t row_start, word;

	worker = argument;
	p = worker->input + worker->start;
	end = worker->input + worker->end;
	while (p < end) {
		row_start = (size_t)(p - worker->input);
		if (!parse_pair(&p, end, worker->seen->bits, &pair)) {
			worker->error = "malformed or out-of-domain row";
			worker->error_offset = row_start;
			break;
		}
		word = (size_t)(pair / 64);
		mask = (uint64_t)1 << (pair % 64);
		if (worker->reference != NULL &&
		    (atomic_load_explicit(&worker->reference->words[word],
		    memory_order_relaxed) & mask) == 0) {
			worker->error = "pair absent from reference";
			worker->error_offset = row_start;
			break;
		}
		previous = atomic_fetch_or_explicit(&worker->seen->words[word],
		    mask, memory_order_relaxed);
		if ((previous & mask) == 0)
			worker->distinct++;
	}
	return (NULL);
}

/* Move a byte partition to the beginning of the next complete row. */
static size_t
row_boundary(const char *input, size_t size, size_t offset)
{
	if (offset == 0 || input[offset - 1] == '\n')
		return (offset);
	while (offset < size && input[offset - 1] != '\n')
		offset++;
	return (offset);
}

static bool
scan_file(const char *path, unsigned int worker_count, pair_set_t *seen,
    const pair_set_t *reference, uint64_t *distinct)
{
	scan_worker_t *workers;
	struct stat status;
	pthread_t *threads;
	char *input;
	size_t size, offset;
	unsigned int i, started;
	int fd, error;
	bool valid;

	*distinct = 0;
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(2, "%s", path);
	if (fstat(fd, &status) == -1)
		err(2, "stat %s", path);
	if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
	    (uintmax_t)status.st_size > SIZE_MAX)
		errx(2, "%s: expected a mappable regular file", path);
	size = (size_t)status.st_size;
	if (size == 0) {
		close(fd);
		return (true);
	}
	input = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (input == MAP_FAILED)
		err(2, "map %s", path);
	close(fd);
	workers = calloc(worker_count, sizeof(*workers));
	threads = calloc(worker_count, sizeof(*threads));
	if (workers == NULL || threads == NULL)
		err(2, "allocate workers");

	/* Adjacent workers share the same boundary; no row overlaps or vanishes. */
	offset = 0;
	started = 0;
	valid = true;
	for (i = 0; i < worker_count; i++) {
		workers[i].input = input;
		workers[i].start = offset;
		offset = size / worker_count * (i + 1) +
		    size % worker_count * (i + 1) / worker_count;
		offset = row_boundary(input, size, offset);
		workers[i].end = offset;
		workers[i].seen = seen;
		workers[i].reference = reference;
		error = pthread_create(&threads[i], NULL, scan_pairs, &workers[i]);
		if (error != 0) {
			warnx("pthread_create: error %d", error);
			valid = false;
			break;
		}
		started++;
	}
	for (i = 0; i < started; i++) {
		error = pthread_join(threads[i], NULL);
		if (error != 0)
			errx(2, "pthread_join: error %d", error);
		if (workers[i].error != NULL) {
			warnx("%s: byte %zu: %s", path, workers[i].error_offset,
			    workers[i].error);
			valid = false;
		}
		*distinct += workers[i].distinct;
	}
	free(threads);
	free(workers);
	if (munmap(input, size) == -1)
		err(2, "unmap %s", path);
	return (valid);
}

int
main(int argc, char **argv)
{
	pair_set_t reference, candidate;
	uint64_t reference_rows, candidate_rows;
	long processors;
	unsigned int bits, workers;
	int option;
	bool valid;

	bits = 17;
	processors = sysconf(_SC_NPROCESSORS_ONLN);
	workers = processors > 0 && processors <= 1024 ?
	    (unsigned int)processors : 1;
	while ((option = getopt(argc, argv, "b:j:")) != -1) {
		switch (option) {
		case 'b':
			bits = positive_option(optarg, 17);
			break;
		case 'j':
			workers = positive_option(optarg, 1024);
			break;
		default:
			errx(2, "usage: pairset [-b bits] [-j workers] reference [candidate]");
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 || argc > 2 || sizeof(size_t) < 8)
		errx(2, "usage: pairset [-b bits] [-j workers] reference [candidate]");
	reference = allocate_set(bits);
	valid = scan_file(argv[0], workers, &reference, NULL, &reference_rows);
	if (valid && argc == 2) {
		candidate = allocate_set(bits);
		valid = scan_file(argv[1], workers, &candidate, &reference,
		    &candidate_rows);
		if (valid && candidate_rows != reference_rows) {
			warnx("missing pairs: reference %" PRIu64 ", candidate %" PRIu64,
			    reference_rows, candidate_rows);
			valid = false;
		}
		free(candidate.words);
	}
	free(reference.words);
	if (!valid)
		return (1);
	printf("%" PRIu64 "\n", reference_rows);
	return (0);
}
