#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <rte_ring.h>
#include <rte_mbuf_pool_ops.h>
#include <vector>
#include <iostream>
#include <queue>
#include <boost/coroutine2/all.hpp>
#include <boost/bind.hpp>
#include <boost/context/stack_context.hpp>
#include "rocksdb/c.h"
#include "ci_lib.h"
#include <string>
#include <sys/mman.h> // mmap, munmap

#define NUM_WORKER_THREADS 16
#define NUM_WORKER_COROS 4

// shared among cores
#define RX_RING_SIZE 1024
#define RX_QUEUE_BURST_SIZE 32
#define RX_MBUF_POOL_SIZE 32767
#define RX_MBUF_CACHE_SIZE 250
#define TX_MBUF_POOL_SIZE 8191
#define TX_MBUF_CACHE_SIZE 250

// per-core states
#define TX_RING_SIZE 128
#define TX_QUEUE_BURST_SIZE 4
#define DISPATCH_RING_SIZE 256
#define DISPATCH_RING_BURST_SIZE 4
#define DISPATCH_RING_DEQUEUE_PERIOD 8
#define RETURN_RING_SIZE 512
#define RETURN_RING_BURST_SIZE 8
#define RETURN_RING_CHECKIN_PERIOD (RETURN_RING_BURST_SIZE * NUM_WORKER_THREADS * 2)
#define FREE_MBUF_MAX_BATCH_SIZE (RETURN_RING_SIZE * NUM_WORKER_THREADS)
#define MAX_RUNNING_JOBS_PER_THREAD_TO_CHECKIN 128
#define MAX_RUNNING_JOBS_TO_CHECKIN (NUM_WORKER_THREADS * MAX_RUNNING_JOBS_PER_THREAD_TO_CHECKIN)
#define MAX_NUM_RX_MBUF_PER_THREAD (DISPATCH_RING_SIZE + NUM_WORKER_COROS + RETURN_RING_BURST_SIZE)
#define MAX_NUM_TX_MBUF_PER_THREAD (NUM_WORKER_COROS + TX_QUEUE_BURST_SIZE)

#define STACK_SIZE (128 * 1024)
#define HUGE_PAGE_SIZE (1 << 30)

#define PREFETCH_OFFSET 1
#ifndef QUANTUM_CYCLE
#define QUANTUM_CYCLE 1000
#endif

#define MAKE_IP_ADDR(a, b, c, d)			\
	(((uint32_t) a << 24) | ((uint32_t) b << 16) |	\
	 ((uint32_t) c << 8) | (uint32_t) d)

#ifndef BASE_CPU
#define BASE_CPU 0
#endif

typedef struct worker_arg {
	struct rte_ring* rx_mbuf_dispatch_q;
    struct rte_ring* rx_mbuf_return_q;
    #ifdef STACKS_FROM_HUGEPAGE
    char* stack_pool;
    #endif
    int wid;
} worker_arg_t;

typedef struct worker_info
{
    struct rte_ring* rx_mbuf_dispatch_q;
    struct rte_ring* rx_mbuf_return_q;
    pthread_t* work_thread;
    int wid;
    int version_number;
    int num_running_jobs;

    worker_info(int wid) : rx_mbuf_dispatch_q(nullptr),  rx_mbuf_return_q(nullptr), work_thread(nullptr), wid(wid), version_number(0), num_running_jobs(0) {}
 
    friend bool operator< (worker_info const& lhs, worker_info const& rhs) {
    	if(lhs.version_number != rhs.version_number)
    		return lhs.version_number > rhs.version_number;
	    return lhs.num_running_jobs > rhs.num_running_jobs; // so that it's a min heap
    }
} worker_info_t;

bool worker_info_ptr_cmp(const worker_info_t* ptr1, const worker_info_t* ptr2) {
	return *ptr1 < *ptr2;
}

typedef boost::coroutines2::coroutine<void*>   coro_t;
// job type
typedef enum job_type {
    ROCKSDB_GET = 0xA,
    ROCKSDB_SCAN 
} job_type_t;
// job info passed to worker coroutine
typedef struct job_info {
    job_type_t jtype;
    uint32_t key;
    char* output_data;
} job_info_t;

typedef struct coro_info 
{
	coro_t::pull_type *coro;
	coro_t::push_type *yield;
	job_info_t *jinfo;
	struct rte_mbuf *rx_mbuf;
	struct rte_mbuf *tx_mbuf;
	uint32_t num_quanta;
	uint64_t execution_time;
	coro_info(): coro(nullptr), yield(nullptr), jinfo(nullptr), rx_mbuf(nullptr), tx_mbuf(nullptr), num_quanta(0), execution_time(0) {}
	#ifdef LAS
	friend bool operator< (coro_info const& lhs, coro_info const& rhs) {
	    return lhs.num_quanta > rhs.num_quanta; // so that it's a min heap
    }
    #endif
} coro_info_t;

struct rte_rocksdb_hdr {
        uint32_t id;
        uint32_t req_type;
        uint32_t req_size;
        uint32_t run_ns;
};

#ifdef LAS
bool coro_info_ptr_cmp(const coro_info_t* ptr1, const coro_info_t* ptr2) {
	return *ptr1 < *ptr2;
}
#endif

class SimpleStack {
private:
	char* 			vp_;
    std::size_t     size_;

public:
    SimpleStack( char* vp, std::size_t size = STACK_SIZE ) BOOST_NOEXCEPT_OR_NOTHROW :
        vp_(vp), size_( size) {
    }

    boost::context::stack_context allocate() {
        boost::context::stack_context sctx;
        sctx.size = size_;
        sctx.sp = vp_ + sctx.size; 
        return sctx;
    }

    void deallocate( boost::context::stack_context & sctx) BOOST_NOEXCEPT_OR_NOTHROW {
        BOOST_ASSERT( sctx.sp);
        // don't need to do anything, dispatcher is gonna free it
    }
};

static rocksdb_t *db;

static unsigned int dpdk_port = 1;
struct rte_mempool *rx_mbuf_pool;
struct rte_mempool *tx_mbuf_pool;
static struct rte_ether_addr my_eth;
static uint32_t my_ip;

/* parameters */
static unsigned int server_port = 8001;
static unsigned int num_rx_queues = 1;
static unsigned int num_tx_queues = NUM_WORKER_THREADS;

static bool debug_mode = false;

__thread uint64_t time_interval = 0;

__thread uint64_t get_start_time, get_end_time; 

__thread coro_t::push_type *curr_yield;

#ifdef LAS
__thread uint32_t quantum_idx = 0;
__thread uint32_t num_assigned_quanta = 1;
#endif

#ifdef STACKS_FROM_HUGEPAGE
char* stacks;
#endif

static int str_to_ip(const char *str, uint32_t *addr)
{
	uint8_t a, b, c, d;
	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
		return -EINVAL;
	}

	*addr = MAKE_IP_ADDR(a, b, c, d);
	return 0;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool, unsigned int n_rxqueues, unsigned int n_txqueues)
{
	//struct rte_eth_conf port_conf = port_conf_default;
	struct rte_eth_conf port_conf = {};
	port_conf.rxmode.offloads = DEV_RX_OFFLOAD_IPV4_CKSUM;
	port_conf.txmode.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_UDP_CKSUM;

	const uint16_t rx_rings = n_rxqueues, tx_rings = n_txqueues;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;

	printf("initializing with %u RX queues and %u TX queues\n", n_rxqueues, n_txqueues);

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL,
                                        mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Enable TX offloading */
	rte_eth_dev_info_get(0, &dev_info);
	txconf = &dev_info.default_txconf;

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                                        rte_eth_dev_socket_id(port), txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	rte_eth_macaddr_get(port, &my_eth);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			my_eth.addr_bytes[0], my_eth.addr_bytes[1],
			my_eth.addr_bytes[2], my_eth.addr_bytes[3],
			my_eth.addr_bytes[4], my_eth.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

/*
 * Validate this ethernet header. Return true if this packet is for higher
 * layers, false otherwise.
 */
static bool check_eth_hdr(const struct rte_mbuf *buf)
{
	struct rte_ether_hdr *ptr_mac_hdr;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	if (!rte_is_same_ether_addr(&ptr_mac_hdr->dst_addr, &my_eth)) {
		/* packet not to our ethernet addr */
		return false;
	}

	if (ptr_mac_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		/* packet not IPv4 */
		return false;

	return true;
}

/*
 * Return true if this IP packet is to us and contains a UDP packet,
 * false otherwise.
 */
static bool check_ip_hdr(const struct rte_mbuf *buf)
{
	struct rte_ipv4_hdr *ipv4_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *, RTE_ETHER_HDR_LEN);
	if (ipv4_hdr->dst_addr != rte_cpu_to_be_32(my_ip) || ipv4_hdr->next_proto_id != IPPROTO_UDP)
		return false;

	return true;
}

/*
 * Pin the current thread to a CPU
 */
static void pin_to_cpu(int cpu_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
	cpu_id = cpu_id + BASE_CPU;
        CPU_SET(cpu_id, &cpuset);
        pthread_t thread = pthread_self();
        int ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        if(ret == 0)
            std::cout << "Successfuly pin the current thread to cpu " << cpu_id << std::endl;
}

static uint64_t rdtsc_w_lfence(){
    unsigned int lo,hi;
    __asm__ __volatile__ ("lfence\n\t" "rdtsc": "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t rdtsc(){
    unsigned int lo,hi;
    __asm__ __volatile__ ("rdtsc": "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

void call_the_yield(long ic) {
		#ifdef TIME_STAGE
		time_interval = ic;
		#endif
		#ifdef LAS
		quantum_idx++;
		if(quantum_idx == num_assigned_quanta)
			(*curr_yield)(nullptr);
		#else
        (*curr_yield)(nullptr);
        #endif
}

void empty_handler(long ic) {
		time_interval += ic;
		// LastCycleTS = rdtsc();
		return;
}

void coro(int coro_id, job_info_t* &jinfo, coro_t::push_type &yield)
{       
    std::cout << "[coro]: coro " << coro_id << " is ready!" << std::endl;  
    /* Suspend here, wait for resume. */
    yield(&yield);
    char *err = nullptr;
    size_t vallen;
    rocksdb_readoptions_t *readoptions = rocksdb_readoptions_create();
    char key[10];
    char val[10];

    for(;;) {
    	assert(jinfo->jtype == ROCKSDB_GET);
        
        get_start_time = rdtsc_w_lfence();

        // rocksdb_readoptions_t *readoptions = rocksdb_readoptions_create();
        
        // char *returned_value = rocksdb_get(db, readoptions, jinfo->input_data, strlen(jinfo->input_data), &vallen, &err);
        snprintf(key, 10, "key%d", jinfo->key);
	rocksdb_get_in_place(db, readoptions, key, strlen(key), val, &vallen, &err);
        assert(!err);
        assert(strcmp(val, "value") == 0);
        // memcpy((char*)jinfo->output_data, returned_value, vallen);
    	// assert(strcmp(returned_value, "value") == 0);
    	// free(returned_value);

    	//rocksdb_readoptions_destroy(readoptions);

    	get_end_time = rdtsc_w_lfence();

    	// TODO: leverage this return value? 
    	yield(&yield);
    }
    rocksdb_readoptions_destroy(readoptions);

}

static bool is_rx_mbuf_valid(const struct rte_mbuf *rx_mbuf) {
	// TODO: add UDP check
	return check_eth_hdr(rx_mbuf) && check_ip_hdr(rx_mbuf);
}

void process_rx_mbuf(struct rte_mbuf *rx_mbuf, coro_info_t* idle_coro) {

	#ifdef TIME_STAGE
	uint64_t start_time = rdtsc_w_lfence();
	#endif

	//printf("Packet processed!\n");
	/*struct rte_mbuf *tx_mbuf = rte_pktmbuf_alloc(tx_mbuf_pool);
	assert(tx_mbuf!= nullptr);
	idle_coro->tx_mbuf = tx_mbuf;*/
	idle_coro->rx_mbuf = rx_mbuf;
	idle_coro->num_quanta = 0; 

	/* headers from rx_mbuf */
	struct rte_ether_hdr * rx_ptr_mac_hdr = rte_pktmbuf_mtod(rx_mbuf, struct rte_ether_hdr *);
	struct rte_ipv4_hdr * rx_ptr_ipv4_hdr = rte_pktmbuf_mtod_offset(rx_mbuf, struct rte_ipv4_hdr *, RTE_ETHER_HDR_LEN);
	struct rte_udp_hdr *rx_ptr_udp_hdr = rte_pktmbuf_mtod_offset(rx_mbuf, struct rte_udp_hdr *, RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr));
	// TODO: fix this
	/*uint32_t *seq_num = rte_pktmbuf_mtod_offset(rx_mbuf, uint32_t *, RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));
	uint16_t *jtype = rte_pktmbuf_mtod_offset(rx_mbuf, uint16_t *, RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(uint32_t) );
	idle_coro->jinfo->jtype = static_cast<job_type>(*jtype);
	idle_coro->jinfo->input_data = rte_pktmbuf_mtod_offset(rx_mbuf, char *, RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(uint32_t) + sizeof(uint16_t));*/
	struct rte_rocksdb_hdr *rx_ptr_rocksdb_hdr = rte_pktmbuf_mtod_offset(rx_mbuf, struct rte_rocksdb_hdr *, RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));
	idle_coro->jinfo->jtype = static_cast<job_type>(rte_be_to_cpu_32(rx_ptr_rocksdb_hdr->req_type));
	idle_coro->jinfo->key = rte_be_to_cpu_32(rx_ptr_rocksdb_hdr->req_size);

	/* headers of tx_mbuf */	
	//struct rte_mbuf *tx_mbuf = rte_pktmbuf_copy(rx_mbuf, tx_mbuf_pool, 0, UINT32_MAX);
	struct rte_mbuf *tx_mbuf = rte_pktmbuf_alloc(tx_mbuf_pool);
        assert(tx_mbuf!= nullptr);
	idle_coro->tx_mbuf = tx_mbuf;

	char *buf_ptr;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *rte_udp_hdr;
	struct rte_rocksdb_hdr *rte_rocksdb_hdr;

	/* ethernet header */
	buf_ptr = rte_pktmbuf_append(tx_mbuf, RTE_ETHER_HDR_LEN);
	eth_hdr = (struct rte_ether_hdr *) buf_ptr;

	rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
	rte_ether_addr_copy(&rx_ptr_mac_hdr->src_addr, &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	/* IPv4 header */
	buf_ptr = rte_pktmbuf_append(tx_mbuf, sizeof(struct rte_ipv4_hdr));
	ipv4_hdr = (struct rte_ipv4_hdr *) buf_ptr;
	ipv4_hdr->version_ihl = 0x45;
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct rte_rocksdb_hdr));
	ipv4_hdr->packet_id = rx_ptr_ipv4_hdr->packet_id;
	ipv4_hdr->fragment_offset = rx_ptr_ipv4_hdr->fragment_offset;
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->hdr_checksum = rx_ptr_ipv4_hdr->hdr_checksum;
	ipv4_hdr->src_addr = rte_cpu_to_be_32(my_ip);
	ipv4_hdr->dst_addr = rx_ptr_ipv4_hdr->src_addr;

	/* UDP header */
	buf_ptr = rte_pktmbuf_append(tx_mbuf, sizeof(struct rte_udp_hdr));
	rte_udp_hdr = (struct rte_udp_hdr *) buf_ptr;
	rte_udp_hdr->src_port = rte_cpu_to_be_16(server_port);
	rte_udp_hdr->dst_port = rx_ptr_udp_hdr->src_port;
	rte_udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + sizeof(struct rte_rocksdb_hdr));
	rte_udp_hdr->dgram_cksum = 0;
	/* RocksDB header */
	buf_ptr = rte_pktmbuf_append(tx_mbuf, sizeof(struct rte_rocksdb_hdr));
        rte_rocksdb_hdr = (struct rte_rocksdb_hdr *) buf_ptr;	
	rte_rocksdb_hdr->id = rx_ptr_rocksdb_hdr->id;
	rte_rocksdb_hdr->req_type = rx_ptr_rocksdb_hdr->req_type;
	rte_rocksdb_hdr->req_size = rx_ptr_rocksdb_hdr->req_size;
	rte_rocksdb_hdr->run_ns = 0;
	//*(uint32_t*)((char*)buf_ptr + sizeof(struct rte_udp_hdr)) = *seq_num;
	//idle_coro->jinfo->output_data = buf_ptr + sizeof(struct rte_udp_hdr) + sizeof(uint32_t);

	//tx_mbuf->l2_len = RTE_ETHER_HDR_LEN;
	//tx_mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
	//tx_mbuf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;

	#ifdef TIME_STAGE
	uint64_t end_time = rdtsc_w_lfence();
	if(end_time - start_time > 1000)
		std::cout << end_time - start_time << " cycles" << std::endl;
	#endif
}

static int rocksdb_init() 
{
    rocksdb_options_t *options = rocksdb_options_create();
    rocksdb_options_set_allow_mmap_reads(options, 1);
    rocksdb_options_set_allow_mmap_writes(options, 1);
    rocksdb_slicetransform_t * prefix_extractor = rocksdb_slicetransform_create_capped_prefix(8);
    rocksdb_options_set_prefix_extractor(options, prefix_extractor);
    rocksdb_options_set_plain_table_factory(options, 0, 10, 0.75, 3);
    // Optimize RocksDB. This is the easiest way to
    // get RocksDB to perform well
    rocksdb_options_increase_parallelism(options, 0);
    rocksdb_options_optimize_level_style_compaction(options, 0);
    // create the DB if it's not already present
    rocksdb_options_set_create_if_missing(options, 1);
    // overwrite the default 8MB block cache to support higher concurrency
    //rocksdb_block_based_table_options_t* block_options = rocksdb_block_based_options_create();
    
    //rocksdb_block_based_options_set_block_cache(block_options, rocksdb_cache_create_lru_shard(32 << 20, 8));
    
    //rocksdb_options_set_block_based_table_factory(options, block_options);
    //rocksdb_options_set_table_cache_numshardbits(options, 8);
    rocksdb_options_set_disable_auto_compactions(options, 1);
    
    // open DB
    char *err = NULL;
    char DBPath[] = "/tmpfs/experiments/my_db";
    db = rocksdb_open(options, DBPath, &err);
    if (err) {
   	 	printf("Could not open RocksDB database: %s\n", err);
      	return -1;
  	}
    // Put key-value
  /*rocksdb_writeoptions_t *writeoptions = rocksdb_writeoptions_create();
  const char *value = "value";
  for (int i = 0; i < 5000; i++) {
        char key[10];
        snprintf(key, 10, "key%d", i);
        rocksdb_put(db, writeoptions, key, strlen(key), value, strlen(value) + 1,
                    &err);
        assert(!err);
  }
  assert(!err);
  rocksdb_writeoptions_destroy(writeoptions);*/
  return 0;
}

void* worker(void* arg) {
    
	rte_thread_register();
	printf("lcore %u running in worker mode. [Ctrl+C to quit]\n", rte_lcore_id());

    worker_arg_t* worker_arg = static_cast<worker_arg_t*>(arg);
    int tid = worker_arg->wid;
    pin_to_cpu(tid + 1);

    cp_pid = gettid();
    // per thread
    #ifdef USE_EMPTY_HANDLER
    register_ci_direct(1000/*doesn't matter*/, QUANTUM_CYCLE, empty_handler);
    #else
    register_ci_direct(1000/*doesn't matter*/, QUANTUM_CYCLE, call_the_yield);
    #endif

    struct rte_ring* rx_mbuf_dispatch_q = worker_arg->rx_mbuf_dispatch_q;
    struct rte_ring* rx_mbuf_return_q = worker_arg->rx_mbuf_return_q;
    #ifdef STACKS_FROM_HUGEPAGE
    char* stack_pool = worker_arg->stack_pool;
    #endif
    struct rte_mbuf **rx_bufs = static_cast<struct rte_mbuf **>(malloc(DISPATCH_RING_BURST_SIZE * sizeof(struct rte_mbuf*)));
    struct rte_mbuf **return_rx_bufs = static_cast<struct rte_mbuf **>(malloc(RETURN_RING_BURST_SIZE * sizeof(struct rte_mbuf*)));
    struct rte_mbuf **tx_bufs = static_cast<struct rte_mbuf **>(malloc(TX_QUEUE_BURST_SIZE * sizeof(struct rte_mbuf*)));
   	
   	coro_t::pull_type *worker_coros = static_cast<coro_t::pull_type*>(malloc(NUM_WORKER_COROS * sizeof(coro_t::pull_type)));
    coro_info_t *worker_coro_infos = static_cast<coro_info_t *>(malloc(NUM_WORKER_COROS * sizeof(coro_info_t)));
    job_info_t *job_infos = static_cast<job_info*>(malloc(NUM_WORKER_COROS * sizeof(job_info_t)));

   	uint16_t num_rx_buf, nb_tx, nb_return;
   	uint16_t i; 
	uint16_t dispatch_index = 0, return_rx_buf_idx = 0, tx_buf_idx = 0;
	bool force_flush, force_dispatch;
	uint8_t port = dpdk_port;
	coro_info_t* idle_coro, next_coro;
	std::vector<coro_info_t*> idle_coros;
	idle_coros.reserve(NUM_WORKER_COROS);
	// std::deque<coro_info_t*> idle_coros;

	// flexibility in which end to use
	#ifdef LAS
	std::priority_queue<coro_info_t*, std::vector<coro_info_t*>, decltype(&coro_info_ptr_cmp)> busy_coros(coro_info_ptr_cmp);
	#else
	std::deque<coro_info_t*> busy_coros;
    #endif

    #ifdef TIME_STAGE
    uint64_t start, stage1_end, stage2_end, stage3_end, stage4_end, yield_end_time, stage1_cycles = 0, stage2_cycles = 0, stage3_cycles = 0, stage4_cycles = 0, num_samples = 0, total_work_time = 0;
    #endif

    uint64_t total_num_quanta = 0, total_execution_cycles = 0, finished_jobs = 0, prev_finished_jobs = 0;

    printf("Worker %d initialize all worker coroutines\n", tid);

    for(int coro_id = 0; coro_id < NUM_WORKER_COROS; coro_id++) {
    	#ifdef STACKS_FROM_HUGEPAGE
    	worker_coros[coro_id] = coro_t::pull_type(SimpleStack(stack_pool), boost::bind(coro, coro_id, &job_infos[coro_id], _1));
    	stack_pool += STACK_SIZE;
    	#else
    	worker_coros[coro_id] = coro_t::pull_type(boost::bind(coro, coro_id, &job_infos[coro_id], _1));
    	#endif
    	worker_coro_infos[coro_id].coro = &worker_coros[coro_id];
    	worker_coro_infos[coro_id].yield = static_cast<coro_t::push_type*>(worker_coros[coro_id].get()); 
    	worker_coro_infos[coro_id].jinfo = &job_infos[coro_id];
    }

    for(int coro_id = 0; coro_id < NUM_WORKER_COROS; coro_id++) {
    	idle_coros.push_back(&worker_coro_infos[coro_id]);
    }

    for (;;) {
    	#ifdef TIME_STAGE
    	start = rdtsc_w_lfence();
    	#endif
	    // whether to force a enqueue to return rx mbuf or send tx mbuf
		force_flush = false;
		// whether to force a dequeue to read new rx mbuf
		force_dispatch = false;

		if(!busy_coros.empty()) {
			
			#ifdef LOOP_YIELD
			for(;;) {
			#endif

			#ifdef LAS
			coro_info_t* next_coro = busy_coros.top();
			busy_coros.pop();
			num_assigned_quanta = busy_coros.top()->num_quanta - next_coro->num_quanta + 1; 
			num_assigned_quanta = (num_assigned_quanta + dispatch_index <= DISPATCH_RING_DEQUEUE_PERIOD)? num_assigned_quanta : DISPATCH_RING_DEQUEUE_PERIOD - dispatch_index;
			quantum_idx = 0;
			#else
			coro_info_t* next_coro = busy_coros.front();
			busy_coros.pop_front();
			#endif
		    // set the yield function
		    curr_yield = next_coro->yield;
		    if(next_coro->num_quanta == 0)
		    	LastCycleTS = rdtsc();
		    
		    // resume next_coro
		    (*(next_coro->coro))();
		    
		    #ifdef TIME_STAGE
		    yield_end_time = rdtsc_w_lfence();
		    next_coro->execution_time += yield_end_time - LastCycleTS;
		    total_work_time += time_interval;
		    #endif

		    // check whether next_coro finish
		    if(next_coro->coro->get() == nullptr) {
		    	// not finished
		    	next_coro->num_quanta ++;
		    	#ifdef LAS
				busy_coros.push(next_coro);
				#else
				#ifdef LOOP_YIELD
				busy_coros.push_front(next_coro);
		    	#else 
		    	busy_coros.push_back(next_coro);
		    	#endif
		    	#endif
		    }
		    else {
		    	// finished 
		    	return_rx_bufs[return_rx_buf_idx++] = next_coro->rx_mbuf;
		    	tx_bufs[tx_buf_idx++] = next_coro->tx_mbuf;
		    	total_execution_cycles += get_end_time - get_start_time; //next_coro->execution_time;
		    	total_num_quanta += next_coro->num_quanta + 1;
		    	finished_jobs++;

		    	idle_coros.push_back(next_coro);
		    	#ifdef LOOP_YIELD
		    	break;
		    	#endif
		    }
		    #ifdef LAS
		    dispatch_index += num_assigned_quanta; 
		    #else
		    dispatch_index++;
		    #endif
		    #ifdef LOOP_YIELD
			}
			#endif
		} else {
			force_dispatch = true;
			force_flush = (tx_buf_idx > 0);
		}

		#ifdef TIME_STAGE
    	stage1_end = rdtsc_w_lfence();
    	#endif

		if(force_dispatch || (dispatch_index >= DISPATCH_RING_DEQUEUE_PERIOD && !idle_coros.empty())) {
	    	// get new jobs if (1) there are idle cores and (2) dequeue_period is up
			num_rx_buf = rte_ring_dequeue_burst(rx_mbuf_dispatch_q, (void **)rx_bufs, (idle_coros.size() < DISPATCH_RING_BURST_SIZE)? idle_coros.size() : DISPATCH_RING_BURST_SIZE, nullptr); 

			for(i = 0; i < num_rx_buf; i++) {
				if(i + PREFETCH_OFFSET < num_rx_buf) {
					rte_mbuf_prefetch_part1(rx_bufs[i + PREFETCH_OFFSET]);
				}
				if(unlikely(!is_rx_mbuf_valid(rx_bufs[i]))) {
					// invalid packet, free the rx_mbuf
					// TODO: fix this
					if(return_rx_buf_idx < RETURN_RING_BURST_SIZE) {
						return_rx_bufs[return_rx_buf_idx++] = rx_bufs[i];
					}
					continue;
				} 
				idle_coro = idle_coros.back();
				idle_coros.pop_back();
				// idle_coro = idle_coros.front();
				// idle_coros.pop_front();
				process_rx_mbuf(rx_bufs[i], idle_coro);
				// prioritize new jobs
				#ifdef LAS
				busy_coros.push(idle_coro);
				#else
	  			busy_coros.push_front(idle_coro);
	  			#endif
	  		}
			dispatch_index = 0;
		} 

		#ifdef TIME_STAGE
    	stage2_end = rdtsc_w_lfence();
    	#endif

	    /* TX path */
	    if(force_flush || tx_buf_idx == TX_QUEUE_BURST_SIZE) {
	    	// std::cout << "Worker " << tid << " does TX flush" << std::endl;
	  		nb_tx = rte_eth_tx_burst(port, tid, tx_bufs, tx_buf_idx);
	  		if (unlikely(nb_tx != tx_buf_idx))
				printf("error: worker %d could not transmit all packets: %d %d\n", tid, tx_buf_idx, nb_tx);
	  		tx_buf_idx = 0;
	    }

	    #ifdef TIME_STAGE
    	stage3_end = rdtsc_w_lfence();
    	#endif

	    /* return rx_mbuf */
	    if(force_flush || return_rx_buf_idx == RETURN_RING_BURST_SIZE) {
	    	// std::cout << "Worker " << tid << " does RX return flush" << std::endl;
	    	nb_return = rte_ring_enqueue_burst(rx_mbuf_return_q, (void **)return_rx_bufs, return_rx_buf_idx, nullptr);
	    	if (unlikely(nb_return != return_rx_buf_idx)) {
	    		printf("error: worker %d could not return all rx-mbufs: %d %d\n", tid, return_rx_buf_idx, nb_return);
	    		abort();
	    	}
	    	return_rx_buf_idx = 0;
	    }

	    #ifdef TIME_STAGE
    	stage4_end = rdtsc_w_lfence();
    	stage1_cycles += stage1_end - start;
    	stage2_cycles += stage2_end - stage1_end;
    	stage3_cycles += stage3_end - stage2_end;
    	stage4_cycles += stage4_end - stage3_end;
    	uint64_t threshold = (debug_mode)? 100 : 10000000;
    	num_samples++; 
    	if(tid == 0 && num_samples >= threshold) {
    		std::cout << "Stage 1: " << stage1_cycles/num_samples;
    		std::cout << ", Stage 2: " << stage2_cycles/num_samples;
    		std::cout << ", Stage 3: " << stage3_cycles/num_samples;
    		std::cout << ", Stage 4: " << stage4_cycles/num_samples/* << std::endl*/;
    		std::cout << ", Work time: " << total_work_time/num_samples;
    		stage1_cycles = 0;
    		stage2_cycles = 0;
    		stage3_cycles = 0;
    		stage4_cycles = 0;
    		total_work_time = 0;
    		num_samples = 0;
    	}
    	#endif

    	if(unlikely(tid == 0 && (finished_jobs - prev_finished_jobs) > 100000)) {
    		std::cout << "Execution time: " << (float) total_execution_cycles * 1000 * 1000 /(finished_jobs * rte_get_timer_hz()) << " us" << std::endl;
    		prev_finished_jobs = finished_jobs;	
    		std::cout << "Average number of quanta for each job: " << (float) total_num_quanta / finished_jobs << std::endl;
    	}
	}
}

static size_t round_to_huge_page_size(size_t n) {
    return (((n - 1) / HUGE_PAGE_SIZE) + 1) * HUGE_PAGE_SIZE;
}

#ifdef STACKS_FROM_HUGEPAGE
static char* allocate_stacks_from_hugepages() {
	char *p = static_cast<char *>(mmap(nullptr, round_to_huge_page_size(NUM_WORKER_THREADS * NUM_WORKER_COROS * STACK_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB , -1, 0));
	if (p == MAP_FAILED) {
      throw std::bad_alloc();
      abort();
    }
    return p;
}

static void deallocate_stacks(char* stacks) {
	munmap(stacks, round_to_huge_page_size(NUM_WORKER_THREADS * NUM_WORKER_COROS * STACK_SIZE));
}
#endif

/*
 * Run an echo server
 */
static int run_server()
{
	pin_to_cpu(0);

	printf("lcore %u running in server mode. [Ctrl+C to quit]\n", rte_lcore_id());

	pthread_t worker_threads[NUM_WORKER_THREADS];
	// input arguments to worker pthreads
	worker_arg_t worker_args[NUM_WORKER_THREADS];
	// worker information
	std::vector<worker_info_t> worker_info_vec;

	/* initialize worker info */
	for(int wid = 0; wid < NUM_WORKER_THREADS; wid++) {
		worker_info_vec.emplace_back(wid);
	}
	/* dispatch queues */
	for(int wid = 0; wid < NUM_WORKER_THREADS; wid++) {
		char name[32];
		snprintf(name, sizeof(name), "dispatch_ring_%d", wid);
		worker_info_vec[wid].rx_mbuf_dispatch_q = rte_ring_create(name, DISPATCH_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    }
    /* return queues */
	for(int wid = 0; wid < NUM_WORKER_THREADS; wid++) {
		char name[32];
		snprintf(name, sizeof(name), "return_ring_%d", wid);
		worker_info_vec[wid].rx_mbuf_return_q = rte_ring_create(name, RETURN_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    }
	/* worker threads */
    for(int wid = 0; wid < NUM_WORKER_THREADS; wid++) {
        worker_args[wid].wid = wid;
        #ifdef STACKS_FROM_HUGEPAGE
        worker_args[wid].stack_pool = stacks; 
        stacks += NUM_WORKER_COROS * STACK_SIZE; 
        #endif
        worker_args[wid].rx_mbuf_dispatch_q = worker_info_vec[wid].rx_mbuf_dispatch_q;
        worker_args[wid].rx_mbuf_return_q = worker_info_vec[wid].rx_mbuf_return_q;
        pthread_create(&worker_threads[wid], nullptr, *worker, static_cast<void*>(&worker_args[wid]));
        worker_info_vec[wid].work_thread = &worker_threads[wid];
	}

	// a min heap of worker info ptr 
	std::priority_queue<worker_info*, std::vector<worker_info*>, decltype(&worker_info_ptr_cmp)> worker_queue(worker_info_ptr_cmp);
	for(int wid = 0; wid < NUM_WORKER_THREADS; wid++)
		worker_queue.push(&worker_info_vec[wid]);

	uint8_t port = dpdk_port;
	struct rte_mbuf *rx_bufs[RX_QUEUE_BURST_SIZE];
	struct rte_mbuf *return_rx_bufs[FREE_MBUF_MAX_BATCH_SIZE];
	uint16_t nb_rx, i, nb_return, total_return_size, return_size;
	uint16_t return_queue_checkin_idx = 0;
	worker_info_t* tmp_w;
	int cur_version_number = 0;
	//int num_received_jobs = 0;
	bool force_pull_return; 
	int total_running_jobs = 0;

	/* Run until the application is quit or killed. */
	for (;;) {
		/* if there were packets buffered, handle them first before starting to receive again */
		/* receive packets */
		nb_rx = rte_eth_rx_burst(port, 0, rx_bufs, RX_QUEUE_BURST_SIZE);
			
		if (nb_rx == 0)
			continue;

		force_pull_return = false;
		for(i = 0; i < nb_rx; i++) {
			tmp_w = worker_queue.top();
			worker_queue.pop();
			if(unlikely(rte_ring_enqueue(tmp_w->rx_mbuf_dispatch_q, rx_bufs[i]) < 0)) {
				// drop this packet
				// dispatcher may have stale information about the number of jobs each worker has, hence force a return pull
				rte_pktmbuf_free(rx_bufs[i]);
				force_pull_return = true;
				worker_queue.push(tmp_w);
				std::cout << "Packet drop: total number of running jobs " << total_running_jobs << std::endl;
				continue;
			} 
			tmp_w->num_running_jobs++;
			worker_queue.push(tmp_w); 
			return_queue_checkin_idx++;
			total_running_jobs++;
		}

		// check in
		if(return_queue_checkin_idx >= RETURN_RING_CHECKIN_PERIOD || total_running_jobs >= MAX_RUNNING_JOBS_TO_CHECKIN || force_pull_return ) {
			total_return_size = 0;
			for(i = 0; i < NUM_WORKER_THREADS; i++) {
				tmp_w = worker_queue.top();
				worker_queue.pop();
				assert(tmp_w->version_number == cur_version_number);
				return_size = 0;
				for(;;) {
					// drain the return queue
					nb_return = rte_ring_dequeue_burst(tmp_w->rx_mbuf_return_q, (void **)&return_rx_bufs[total_return_size], RETURN_RING_BURST_SIZE, nullptr);
					return_size += nb_return;
					total_return_size += nb_return;
					if(nb_return == 0/*< RETURN_RING_BURST_SIZE*/)
						break;
				}
				// std::cout << "Current version " << cur_version_number << " Worker " << tmp_w->wid << " version " << tmp_w->version_number << " returns " << return_size << " rx_mbufs" <<  std::endl;
				tmp_w->num_running_jobs -= return_size;
				tmp_w->version_number ++;
				worker_queue.push(tmp_w);
			}
			// free them in a large bulk
			rte_pktmbuf_free_bulk(return_rx_bufs, total_return_size);
			return_queue_checkin_idx = 0;
			cur_version_number++;
			total_running_jobs -= total_return_size;
		}
		/*num_received_jobs += nb_rx;
		if(num_received_jobs > 20000000) {
    		std::cout << "Terminated!" << std::endl;
    		#ifdef STACKS_FROM_HUGEPAGE
    		deallocate_stacks(stacks);
    		#endif
    		abort();
    		}*/
	}

	return 0;
}

void
rte_pktmbuf_customized_init(struct rte_mempool *mp,
		 __rte_unused void *opaque_arg,
		 void *_m,
		 __rte_unused unsigned i)
{	
	rte_pktmbuf_init(mp,opaque_arg,_m,i);
	struct rte_mbuf *tx_mbuf = static_cast<struct rte_mbuf *>(_m);
	tx_mbuf->l2_len = RTE_ETHER_HDR_LEN;
	tx_mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
	tx_mbuf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
}


struct rte_mempool *
rte_pktmbuf_pool_create_w_customized_init(const char *name, unsigned int n,
	unsigned int cache_size, uint16_t priv_size, uint16_t data_room_size,
	int socket_id)
{
	struct rte_mempool *mp;
	struct rte_pktmbuf_pool_private mbp_priv;
	const char *mp_ops_name = NULL;
	unsigned elt_size;
	int ret;

	if (RTE_ALIGN(priv_size, RTE_MBUF_PRIV_ALIGN) != priv_size) {
		RTE_LOG(ERR, MBUF, "mbuf priv_size=%u is not aligned\n",
			priv_size);
		rte_errno = EINVAL;
		return NULL;
	}
	elt_size = sizeof(struct rte_mbuf) + (unsigned)priv_size +
		(unsigned)data_room_size;
	memset(&mbp_priv, 0, sizeof(mbp_priv));
	mbp_priv.mbuf_data_room_size = data_room_size;
	mbp_priv.mbuf_priv_size = priv_size;

	mp = rte_mempool_create_empty(name, n, elt_size, cache_size,
		 sizeof(struct rte_pktmbuf_pool_private), socket_id, 0);
	if (mp == NULL)
		return NULL;

	if (mp_ops_name == NULL)
		mp_ops_name = rte_mbuf_best_mempool_ops();
	
	ret = rte_mempool_set_ops_byname(mp, mp_ops_name, NULL);
	if (ret != 0) {
		RTE_LOG(ERR, MBUF, "error setting mempool handler\n");
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}
	
	rte_pktmbuf_pool_init(mp, &mbp_priv);

	ret = rte_mempool_populate_default(mp);
	if (ret < 0) {
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}

	rte_mempool_obj_iter(mp, rte_pktmbuf_customized_init, NULL);

	return mp;
}


/* the default always assumes MPMC */
struct rte_mempool *
rte_pktmbuf_pool_create_spsc(const char *name, unsigned int n,
	unsigned int cache_size, uint16_t priv_size, uint16_t data_room_size,
	int socket_id)
{
	struct rte_mempool *mp;
	struct rte_pktmbuf_pool_private mbp_priv;
	const char *mp_ops_name = NULL;
	unsigned elt_size;
	int ret;

	if (RTE_ALIGN(priv_size, RTE_MBUF_PRIV_ALIGN) != priv_size) {
		RTE_LOG(ERR, MBUF, "mbuf priv_size=%u is not aligned\n",
			priv_size);
		rte_errno = EINVAL;
		return NULL;
	}
	elt_size = sizeof(struct rte_mbuf) + (unsigned)priv_size +
		(unsigned)data_room_size;
	memset(&mbp_priv, 0, sizeof(mbp_priv));
	mbp_priv.mbuf_data_room_size = data_room_size;
	mbp_priv.mbuf_priv_size = priv_size;

	mp = rte_mempool_create_empty(name, n, elt_size, cache_size,
		 sizeof(struct rte_pktmbuf_pool_private), socket_id, RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
	if (mp == NULL)
		return NULL;

	if (mp_ops_name == NULL)
		mp_ops_name = rte_mbuf_best_mempool_ops();
	
	ret = rte_mempool_set_ops_byname(mp, mp_ops_name, NULL);
	if (ret != 0) {
		RTE_LOG(ERR, MBUF, "error setting mempool handler\n");
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}
	
	rte_pktmbuf_pool_init(mp, &mbp_priv);

	ret = rte_mempool_populate_default(mp);
	if (ret < 0) {
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}

	rte_mempool_obj_iter(mp, rte_pktmbuf_init, NULL);

	return mp;
}


/*
 * Initialize dpdk.
 */
static int dpdk_init(int argc, char *argv[])
{
	int args_parsed;

	/* Initialize the Environment Abstraction Layer (EAL). */
	args_parsed = rte_eal_init(argc, argv);
	if (args_parsed < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	/* Check that there is a port to send/receive on. */
	if (!rte_eth_dev_is_valid_port(dpdk_port))
		rte_exit(EXIT_FAILURE, "Error: port is not available\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	rx_mbuf_pool = rte_pktmbuf_pool_create_spsc("MBUF_RX_POOL", RX_MBUF_POOL_SIZE,
		RX_MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (rx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create rx mbuf pool\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	tx_mbuf_pool = rte_pktmbuf_pool_create_w_customized_init("MBUF_TX_POOL", TX_MBUF_POOL_SIZE,
		TX_MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (tx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create tx mbuf pool\n");

	return args_parsed;
}

static int parse_args(int argc, char *argv[])
{
	long tmp;
	int next_arg;

	/* argv[0] is still the program name */
	if (argc != 2 ) {
		printf("invalid number of arguments: %d\n", argc);
		return -EINVAL;
	}
	str_to_ip(argv[1], &my_ip);
	return 0;
}

/* perform basic sanity check of the settings */
void sanity_check()
{	
	assert(RX_MBUF_POOL_SIZE > NUM_WORKER_THREADS * MAX_NUM_RX_MBUF_PER_THREAD);
	assert(TX_MBUF_POOL_SIZE > NUM_WORKER_THREADS * MAX_NUM_TX_MBUF_PER_THREAD);
}
/*
 * The main function, which does initialization and starts the client or server.
 */
int main(int argc, char *argv[])
{
	sanity_check();

	int args_parsed, res;

	#ifdef STACKS_FROM_HUGEPAGE
 	stacks = allocate_stacks_from_hugepages();
 	#endif

	rocksdb_init();
	
	/* Initialize dpdk. */
	args_parsed = dpdk_init(argc, argv);

	/* initialize our arguments */
	argc -= args_parsed;
	argv += args_parsed;
	res = parse_args(argc, argv);
	if (res < 0)
		return 0;

	/* set thread id */
	cp_pid = gettid();

	/* initialize port */
	if (port_init(dpdk_port, rx_mbuf_pool, num_rx_queues, num_tx_queues) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %d\n", dpdk_port);

	//rocksdb_init();
	run_server();

	return 0;
}
