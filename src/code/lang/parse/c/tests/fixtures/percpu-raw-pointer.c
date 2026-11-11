/*
 * Fixture: compile Linux-style raw per-CPU pointer conversion macros.
 */
#define TYPEOF_UNQUAL(value) __typeof_unqual__(value)
#define raw_cpu_ptr(pointer)                          \
  ({                                                  \
    unsigned long address = (unsigned long)(pointer); \
    (TYPEOF_UNQUAL(*(pointer)) *)address;             \
  })

static unsigned long first_version[3];
static unsigned long second_version[3];
static unsigned long *version_counts[2] = {first_version, second_version};

struct nyc_percpu_stats {
  unsigned long **vs_count;
};

struct nyc_percpu_server {
  struct nyc_percpu_stats *stats;
};

struct nyc_percpu_request {
  struct nyc_percpu_server *server;
};

int main(void) {
  int version = 1;
  int procedure = 2;
  struct nyc_percpu_stats stats = {.vs_count = version_counts};
  struct nyc_percpu_server server = {.stats = &stats};
  struct nyc_percpu_request request = {.server = &server};
  struct nyc_percpu_request *rqstp = &request;
  *raw_cpu_ptr(&rqstp->server->stats->vs_count[version][procedure]) += 1;
  return (int)second_version[2];
}
