/*
 * Test the performance of scheduler with two threads
 * polling a ring.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include <getopt.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_ring.h>
#include <rte_errno.h>

#define BURST		64
#define RING_SIZE	128

#define NS_PER_BIN	100
#define MAX_US		10000	/* 10 ms */
#define NS_PER_US	1000
#define NS_PER_SEC	1000000000.
#define MAX_BINS	((MAX_US * NS_PER_US)/NS_PER_BIN)
#define PERCENTILE_BINS 8


static unsigned int delay_us    = 1;
static unsigned int test_length = 120;

static uint64_t tsc_hz;
static uint64_t hits[MAX_BINS];

static void
update_hist(uint64_t cycles)
{
	double ns = (cycles * NS_PER_SEC) / tsc_hz;
	unsigned int bin;

	bin = ns / NS_PER_BIN;
	if (bin > MAX_BINS)
		bin = MAX_BINS - 1;
	hits[bin]++;
}

static void
dump_hist(void)
{
	unsigned int i, ns;
	uint64_t count, sum;

	unsigned int percentile_i;
	static const double percentile_bin[PERCENTILE_BINS] = { 25, 50, 75, 90, 99, 99.9, 99.99, 99.999 };
	unsigned int percentile_val[PERCENTILE_BINS] = {0};

	sum = 0;
	count = 0;
	for (i = 0, ns = 0; i < MAX_BINS; i++, ns += NS_PER_BIN) {
		count += hits[i];
		sum += hits[i] * ns;
	}

	printf("Samples:\t%"PRIu64"\n", count);
	printf("Average:\t%.1fns\n", (double) sum / count);

	sum = 0;
	percentile_i = 0;
	printf("Ns\tCount\tRatio\tPercent\n");
	for (i = 0, ns = 0; i < MAX_BINS; i++, ns += NS_PER_BIN) {
		uint64_t x = hits[i];
		double r, p;

		if (x == 0)
			continue;

		r = (100. * x) / count;
		sum += hits[i];
		p = (100. * sum) / count;

		if (percentile_i < PERCENTILE_BINS && p >= percentile_bin[percentile_i]) {
			percentile_val[percentile_i] = ns;
			percentile_i++;
		}

		printf("%u\t%"PRIu64"\t%.1f%%\t%.3f%%\n",
		       ns, x, r, p);
	}
	printf("\n");

	for (i = 0; i < PERCENTILE_BINS; ++i) {
		printf( "percentile %6.3lf = %u \n", percentile_bin[i], percentile_val[i]);
	}

	fflush(stdout);
}

static bool running = true;
static struct rte_ring *echo_ring;


static int
pinger(void *dummy __rte_unused)
{
	unsigned int lcore_id = rte_lcore_id();
	char ring_name[RTE_RING_NAMESIZE];
	struct rte_ring *my_ring;
	int ret;

	snprintf(ring_name, sizeof(ring_name), "pinger%u", lcore_id);
	my_ring = rte_ring_create(ring_name, 2, rte_socket_id(),
				  RING_F_SP_ENQ|RING_F_SC_DEQ);
	if (!my_ring)
		rte_exit(EXIT_FAILURE, "ring create failed: %d\n", rte_errno);

	while (running) {
		uint64_t t0, t1;
		void *req = my_ring;
		void *resp;

		t0 = rte_get_tsc_cycles();

		if (rte_ring_mp_enqueue(echo_ring, req) < 0) {
			fprintf(stderr, "ring enqueue_failed\n");
			return -1;
		}

		resp = NULL;
		while ((ret = rte_ring_sc_dequeue(my_ring, &resp)) == -ENOENT
		       && running)
			rte_pause();

		if (ret != 0)
			return -1;
		
		t1 = rte_get_tsc_cycles();
		if (req != resp) {
			fprintf(stderr, "did not get my request back\n");
			return -1;
		}

		update_hist(t1 - t0);

		usleep(delay_us);
	}

	return 0;
}

static void
echoer(void)
{
	void *reqs[BURST];
	unsigned int i, n;
	int ret;

	while (running) {
		n = rte_ring_sc_dequeue_burst(echo_ring, reqs, BURST, NULL);

		for (i = 0; i < n; i++) {
			struct rte_ring *r = reqs[i];

			ret = rte_ring_sp_enqueue(r, r);
			if (ret != 0)
				rte_exit(EXIT_FAILURE, "echo enqueue failed\n");
		}
		rte_pause();
	}
}

static void
end_test(int signum __rte_unused)
{
	running = false;
}

static void
usage(const char *prgname)
{
	printf("Usage: %s [EAL options] -- -t DELAY_US\n",
	       prgname);
	exit(0);
}

static int
parse_args(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "t:d:")) != EOF) {
		switch (opt) {
		case 't':
			test_length = atoi(optarg);
			break;
		case 'd':
			delay_us = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return -1;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	unsigned int lcore_id;
	int ret;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");

	argc -= ret;
	argv += ret;
	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid arguments\n");

	if (rte_lcore_count() < 2)
		rte_exit(EXIT_FAILURE, "Need two (or more) cores\n");

	echo_ring = rte_ring_create("echo", RING_SIZE,
				    rte_socket_id(), RING_F_SC_DEQ);
	if (!echo_ring)
		rte_exit(EXIT_FAILURE, "Can not create ring:%d\n", rte_errno);

	signal(SIGINT, end_test);
	signal(SIGALRM, end_test);
	tsc_hz = rte_get_tsc_hz();

	printf("Test will run for %u seconds\n\n", test_length);
	fflush(stdout);
	alarm(test_length);

	ret = rte_eal_mp_remote_launch(pinger, NULL, SKIP_MASTER);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot launch cores\n");

	echoer();

	RTE_LCORE_FOREACH_SLAVE(lcore_id)
		rte_eal_wait_lcore(lcore_id);

	dump_hist();
	return ret;
}
