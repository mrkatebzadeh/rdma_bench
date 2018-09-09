#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "latency.h"
#include "libhrd_cpp/hrd.h"

DEFINE_uint64(is_client, 0, "Is this process a client?");
static constexpr size_t kAppBufSize = KB(4);
static constexpr size_t kAppDataSize = 16;
static_assert(kAppDataSize <= kHrdMaxInline, "");

void run_server() {
  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = 3185;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  hrd_publish_conn_qp(cb, 0, "server");
  printf("main: Server published. Waiting for client.\n");

  hrd_qp_attr_t* clt_qp = nullptr;
  while (clt_qp == nullptr) {
    clt_qp = hrd_get_published_qp("client");
    if (clt_qp == nullptr) usleep(200000);
  }

  printf("main: Server %s found client! Connecting..\n", "server");
  hrd_connect_qp(cb, 0, clt_qp);
  hrd_publish_ready("server");
  printf("main: Server ready. Going to sleep.\n");

  while (true) sleep(1);
}

void run_client() {
  Latency latency;
  hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = 3185;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);

  hrd_publish_conn_qp(cb, 0, "client");
  printf("main: Client published. Waiting for server.\n");

  hrd_qp_attr_t* srv_qp = nullptr;
  while (srv_qp == nullptr) {
    srv_qp = hrd_get_published_qp("server");
    if (srv_qp == nullptr) usleep(2000);
  }

  printf("main: Client found server. Connecting..\n");
  hrd_connect_qp(cb, 0, srv_qp);
  printf("main: Client connected!\n");

  hrd_wait_till_ready("server");

  struct ibv_send_wr write_wr, read_wr, *bad_send_wr;
  struct ibv_sge write_sge, read_sge;
  struct ibv_wc wc;

  // Any garbage write
  auto* ptr = reinterpret_cast<volatile uint8_t*>(&cb->conn_buf[0]);
  for (size_t i = 0; i < kAppBufSize; i++) ptr[i] = 31;

  size_t num_iters = 0;

  while (true) {
    if (num_iters == 100000) {
      printf("avg %.1f us, 50 %.1f us, 99 %.1f us\n", latency.avg() / 10.0,
             latency.perc(.50) / 10.0, latency.perc(.99) / 10.0);
      latency.reset();
      num_iters = 0;
    }

    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);

    // WRITE
    write_sge.addr = reinterpret_cast<uint64_t>(cb->conn_buf);
    write_sge.length = kAppDataSize;
    write_sge.lkey = cb->conn_buf_mr->lkey;

    write_wr.opcode = IBV_WR_RDMA_WRITE;
    write_wr.num_sge = 1;
    write_wr.next = nullptr;
    write_wr.sg_list = &write_sge;
    write_wr.send_flags = 0 | IBV_SEND_INLINE;  // Unsignaled
    write_wr.wr.rdma.remote_addr = srv_qp->buf_addr;
    write_wr.wr.rdma.rkey = srv_qp->rkey;

    // READ
    read_sge.addr = reinterpret_cast<uint64_t>(cb->conn_buf);
    read_sge.length = kAppDataSize;
    read_sge.lkey = cb->conn_buf_mr->lkey;

    read_wr.opcode = IBV_WR_RDMA_READ;
    read_wr.num_sge = 1;
    read_wr.next = nullptr;
    read_wr.sg_list = &read_sge;
    read_wr.send_flags = IBV_SEND_SIGNALED;
    read_wr.wr.rdma.remote_addr = srv_qp->buf_addr;
    read_wr.wr.rdma.rkey = srv_qp->rkey;

    // Make a chain
    write_wr.next = &read_wr;
    read_wr.next = nullptr;

    int ret = ibv_post_send(cb->conn_qp[0], &write_wr, &bad_send_wr);
    rt_assert(ret == 0);
    hrd_poll_cq(cb->conn_cq[0], 1, &wc);  // Block till the RDMA read completes
    num_iters++;

    double us = ns_since(start) / 1000.0;
    latency.update(us * 10);
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_is_client == 1 ? run_client() : run_server();
  return 0;
}
